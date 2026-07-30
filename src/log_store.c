/*
 * IoT Pharma Monitoring Device - persistent sample ring buffer (NVS)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "log_store.h"
#include "app_config.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/drivers/flash.h>
#include <string.h>

LOG_MODULE_REGISTER(log_store, CONFIG_PHARMA_LOG_LEVEL);

/* ------------------------------------------------------------------ *
 * NVS id map
 *   1            : metadata (acked_seq, dropped)
 *   16 .. 16+N-1 : record slots, slot = seq % capacity
 * ------------------------------------------------------------------ */
#define NVS_ID_META      1U
#define NVS_ID_REC_BASE  16U
#define NVS_ID_MAX       65534U

#define STORE_MAGIC      0x50484D31U   /* "PHM1" */

struct store_meta {
	uint32_t magic;
	uint32_t acked_seq;
	uint32_t dropped;
	uint32_t rsv;
};

BUILD_ASSERT(sizeof(struct store_meta) == 16, "meta layout changed");

static struct nvs_fs fs;
static struct k_mutex lock;

static uint32_t capacity;    /* number of record slots            */
static uint32_t next_seq;    /* next sequence number to assign    */
static uint32_t acked_seq;   /* highest seq confirmed delivered   */
static uint32_t dropped;     /* evicted before acknowledgement    */
static bool     ready;

/* ------------------------------------------------------------------ */

static inline uint16_t slot_id(uint32_t seq)
{
	return (uint16_t)(NVS_ID_REC_BASE + (seq % capacity));
}

/* Oldest sequence number that is both still stored and still unacked. */
static uint32_t oldest_unacked(void)
{
	uint32_t oldest_stored;

	if (next_seq <= 1U) {
		return 1U;               /* nothing written yet */
	}

	/* Slots hold seq in [next_seq - capacity, next_seq - 1]. */
	oldest_stored = (next_seq > capacity) ? (next_seq - capacity) : 1U;

	return MAX(oldest_stored, acked_seq + 1U);
}

static int meta_save(void)
{
	struct store_meta m = {
		.magic     = STORE_MAGIC,
		.acked_seq = acked_seq,
		.dropped   = dropped,
		.rsv       = 0U,
	};
	ssize_t rc = nvs_write(&fs, NVS_ID_META, &m, sizeof(m));

	if (rc < 0) {
		LOG_ERR("meta write failed (%d)", (int)rc);
		return (int)rc;
	}
	return 0;
}

static void meta_load(void)
{
	struct store_meta m;
	ssize_t rc = nvs_read(&fs, NVS_ID_META, &m, sizeof(m));

	if (rc == (ssize_t)sizeof(m) && m.magic == STORE_MAGIC) {
		acked_seq = m.acked_seq;
		dropped   = m.dropped;
		LOG_INF("meta restored: acked=%u dropped=%u", acked_seq, dropped);
	} else {
		acked_seq = 0U;
		dropped   = 0U;
		LOG_WRN("no valid metadata, starting fresh");
	}
}

/*
 * Recover next_seq by scanning every slot for the highest sequence.
 *
 * Cost: one NVS read per slot, once, at boot. NVS reads are flash reads
 * (no erase), so even 2048 slots complete in well under a second. Doing
 * it this way means push() never has to write metadata, which is what
 * keeps flash wear at ~20 erases/sector/year.
 */
static void scan_recover_head(void)
{
	struct sample_record rec;
	uint32_t max_seq = 0U;
	uint32_t found = 0U;

	for (uint32_t i = 0U; i < capacity; i++) {
		ssize_t rc = nvs_read(&fs, (uint16_t)(NVS_ID_REC_BASE + i),
				      &rec, sizeof(rec));

		if (rc != (ssize_t)sizeof(rec)) {
			continue;            /* empty slot */
		}
		found++;
		if (rec.seq > max_seq) {
			max_seq = rec.seq;
		}
	}

	next_seq = max_seq + 1U;
	LOG_INF("recovered %u slots, last seq=%u, next=%u",
		found, max_seq, next_seq);
}

/* ------------------------------------------------------------------ */

int log_store_init(void)
{
	struct flash_pages_info info;
	uint32_t part_size, sector_count, max_by_flash;
	int rc;

	k_mutex_init(&lock);

	fs.flash_device = FIXED_PARTITION_DEVICE(storage_partition);
	if (!device_is_ready(fs.flash_device)) {
		LOG_ERR("flash device not ready");
		return -ENODEV;
	}

	fs.offset = FIXED_PARTITION_OFFSET(storage_partition);
	part_size = FIXED_PARTITION_SIZE(storage_partition);

	rc = flash_get_page_info_by_offs(fs.flash_device, fs.offset, &info);
	if (rc) {
		LOG_ERR("flash_get_page_info_by_offs failed (%d)", rc);
		return rc;
	}

	fs.sector_size  = info.size;
	sector_count    = part_size / info.size;
	fs.sector_count = (uint16_t)sector_count;

	if (sector_count < 2U) {
		LOG_ERR("storage_partition needs >= 2 sectors (have %u)",
			sector_count);
		return -ENOSPC;
	}

	rc = nvs_mount(&fs);
	if (rc) {
		LOG_ERR("nvs_mount failed (%d)", rc);
		return rc;
	}

	/*
	 * Usable capacity. One sector is always reserved for NVS garbage
	 * collection, and each entry costs sizeof(record) + an 8-byte
	 * allocation-table entry.
	 */
	max_by_flash = ((sector_count - 1U) * info.size) /
		       (sizeof(struct sample_record) + 8U);

	capacity = MIN(max_by_flash, (uint32_t)APP_STORE_MAX_RECORDS);
	capacity = MIN(capacity, NVS_ID_MAX - NVS_ID_REC_BASE);

	if (capacity == 0U) {
		LOG_ERR("computed zero capacity");
		return -ENOSPC;
	}

	LOG_INF("NVS: %u sectors x %u B, capacity %u records (%u h @ %u s)",
		sector_count, info.size, capacity,
		(capacity * APP_SAMPLE_PERIOD_S) / 3600U, APP_SAMPLE_PERIOD_S);

	meta_load();
	scan_recover_head();

	/* Guard against metadata that is ahead of the data (corrupt state). */
	if (acked_seq >= next_seq) {
		LOG_WRN("acked_seq %u >= next_seq %u, clamping",
			acked_seq, next_seq);
		acked_seq = (next_seq > 0U) ? next_seq - 1U : 0U;
		(void)meta_save();
	}

	ready = true;
	LOG_INF("store ready: pending=%u dropped=%u",
		log_store_pending(), dropped);
	return 0;
}

uint32_t log_store_capacity(void)
{
	return capacity;
}

int log_store_push(struct sample_record *rec)
{
	ssize_t rc;
	int ret = 0;

	if (!ready) {
		return -ENODEV;
	}
	if (rec == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	/*
	 * Writing sequence S overwrites whatever occupied slot S % capacity,
	 * which was sequence S - capacity. If that record was never
	 * acknowledged we are losing audit data - count it explicitly.
	 */
	if (next_seq > capacity) {
		uint32_t evicted = next_seq - capacity;

		if (evicted > acked_seq) {
			dropped++;
			LOG_WRN("ring full: evicting unacked seq %u (total dropped %u)",
				evicted, dropped);
			/* Persist the drop count; this is an audit event. */
			(void)meta_save();
		}
	}

	rec->seq = next_seq;

	rc = nvs_write(&fs, slot_id(rec->seq), rec, sizeof(*rec));
	if (rc < 0) {
		LOG_ERR("nvs_write seq %u failed (%d)", rec->seq, (int)rc);
		ret = (int)rc;
		goto out;
	}
	if (rc != (ssize_t)sizeof(*rec)) {
		/* nvs_write returns 0 when the value is unchanged; for a
		 * record containing a unique seq this cannot legitimately
		 * happen, so treat it as an error.
		 */
		LOG_ERR("short write for seq %u (%d)", rec->seq, (int)rc);
		ret = -EIO;
		goto out;
	}

	next_seq++;

out:
	k_mutex_unlock(&lock);
	return ret;
}

int log_store_peek(struct sample_record *out, size_t max, size_t *count)
{
	uint32_t seq, first;
	size_t n = 0U;

	if (!ready) {
		return -ENODEV;
	}
	if (out == NULL || count == NULL || max == 0U) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	first = oldest_unacked();

	for (seq = first; seq < next_seq && n < max; seq++) {
		ssize_t rc = nvs_read(&fs, slot_id(seq), &out[n], sizeof(out[n]));

		if (rc != (ssize_t)sizeof(out[n])) {
			LOG_WRN("slot for seq %u unreadable (%d), skipping",
				seq, (int)rc);
			continue;
		}
		/*
		 * Verify the slot still holds the sequence we asked for.
		 * A mismatch means the ring wrapped past this entry while
		 * we were reading - the data is gone, do not report it.
		 */
		if (out[n].seq != seq) {
			LOG_WRN("slot aliasing at seq %u (found %u), skipping",
				seq, out[n].seq);
			continue;
		}
		n++;
	}

	*count = n;
	k_mutex_unlock(&lock);
	return 0;
}

int log_store_ack(uint32_t up_to_seq)
{
	int rc = 0;

	if (!ready) {
		return -ENODEV;
	}

	k_mutex_lock(&lock, K_FOREVER);

	if (up_to_seq > acked_seq && up_to_seq < next_seq) {
		acked_seq = up_to_seq;
		rc = meta_save();
		LOG_DBG("acked up to seq %u, %u pending",
			acked_seq, log_store_pending());
	}

	k_mutex_unlock(&lock);
	return rc;
}

uint32_t log_store_pending(void)
{
	uint32_t first;

	if (!ready || next_seq <= 1U) {
		return 0U;
	}

	first = oldest_unacked();
	return (next_seq > first) ? (next_seq - first) : 0U;
}

uint32_t log_store_dropped(void)
{
	return dropped;
}

uint32_t log_store_last_seq(void)
{
	return (next_seq > 0U) ? next_seq - 1U : 0U;
}

int log_store_backfill_time(int64_t uptime_to_utc_offset_ms)
{
	uint32_t seq, first;
	int fixed = 0;

	if (!ready) {
		return -ENODEV;
	}

	k_mutex_lock(&lock, K_FOREVER);

	first = oldest_unacked();

	for (seq = first; seq < next_seq; seq++) {
		struct sample_record rec;
		ssize_t rc = nvs_read(&fs, slot_id(seq), &rec, sizeof(rec));

		if (rc != (ssize_t)sizeof(rec) || rec.seq != seq) {
			continue;
		}
		if (rec.flags & REC_FLAG_TIME_VALID) {
			continue;                    /* already absolute */
		}

		rec.ts_ms += uptime_to_utc_offset_ms;
		rec.flags |= REC_FLAG_TIME_VALID;

		rc = nvs_write(&fs, slot_id(seq), &rec, sizeof(rec));
		if (rc == (ssize_t)sizeof(rec)) {
			fixed++;
		}
	}

	k_mutex_unlock(&lock);

	if (fixed) {
		LOG_INF("backfilled UTC onto %d buffered records", fixed);
	}
	return fixed;
}
