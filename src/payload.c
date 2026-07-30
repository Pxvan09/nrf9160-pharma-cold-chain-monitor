/*
 * IoT Pharma Monitoring Device - JSON payload encoder
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "payload.h"
#include "app_config.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(payload, CONFIG_PHARMA_LOG_LEVEL);

/* ------------------------------------------------------------------ *
 * Bounded append helper.
 *
 * Once the buffer overflows the context is poisoned and every further
 * append is a no-op, so a truncated message can never be transmitted:
 * the caller checks ovf and discards the whole batch.
 * ------------------------------------------------------------------ */
struct bctx {
	char  *buf;
	size_t size;
	size_t used;
	bool   ovf;
};

__printf_like(2, 3)
static void bappend(struct bctx *c, const char *fmt, ...)
{
	va_list ap;
	int n;
	size_t space;

	if (c->ovf) {
		return;
	}

	space = c->size - c->used;      /* always >= 1, see bctx_init */

	va_start(ap, fmt);
	n = vsnprintf(c->buf + c->used, space, fmt, ap);
	va_end(ap);

	if (n < 0 || (size_t)n >= space) {
		c->ovf = true;
		return;
	}
	c->used += (size_t)n;
}

static void bctx_init(struct bctx *c, char *buf, size_t size)
{
	c->buf  = buf;
	c->size = size;
	c->used = 0U;
	c->ovf  = (buf == NULL) || (size == 0U);

	if (!c->ovf) {
		buf[0] = '\0';
	}
}

/* ------------------------------------------------------------------ *
 * Timestamp: transmitted as UNIX seconds in a uint32.
 *
 * Deliberately not %lld. Zephyr's reduced printf does not guarantee
 * 64-bit conversions, and seconds-since-epoch fits a uint32 until 2106,
 * which is comfortably past the service life of a shipping logger.
 * ------------------------------------------------------------------ */
static uint32_t ts_seconds(const struct sample_record *r)
{
	int64_t s;

	if (!(r->flags & REC_FLAG_TIME_VALID)) {
		return 0U;     /* cloud treats 0 as "device clock not synced" */
	}

	s = r->ts_ms / 1000;
	if (s < 0) {
		return 0U;
	}
	if (s > (int64_t)UINT32_MAX) {
		return UINT32_MAX;
	}
	return (uint32_t)s;
}

static void encode_record(struct bctx *c, const struct sample_record *r)
{
	bappend(c, "{\"s\":%u,\"ts\":%u", r->seq, ts_seconds(r));

	/* Only emit a field when the reading is actually valid, so the
	 * cloud can distinguish "sensor failed" from a plausible value.
	 */
	if (r->flags & REC_FLAG_AMB_VALID) {
		bappend(c, ",\"t\":%d", (int)r->amb_cc);
	}
	if (r->flags & REC_FLAG_PROBE_VALID) {
		bappend(c, ",\"pt\":%d", (int)r->probe_cc);
	}
	if (r->hum_cpct != REC_HUM_INVALID) {
		bappend(c, ",\"h\":%u", (unsigned int)r->hum_cpct);
	}
	if (r->press_pa != REC_PRESS_INVALID) {
		bappend(c, ",\"p\":%u", r->press_pa);
	}
	if (r->flags & REC_FLAG_FIX_VALID) {
		bappend(c, ",\"la\":%d,\"lo\":%d",
			(int)r->lat_e7, (int)r->lon_e7);
	}
	if (r->batt_mv != 0U) {
		bappend(c, ",\"b\":%u", (unsigned int)r->batt_mv);
	}

	bappend(c, ",\"f\":%u}", (unsigned int)r->flags);
}

static void encode_stats(struct bctx *c, const struct excursion_stats *s)
{
	bappend(c, ",\"st\":{\"al\":%u,\"ev\":%u,\"lo_s\":%u,\"hi_s\":%u,\"ns\":%u",
		s->alarm_active ? 1U : 0U, s->events,
		s->time_low_s, s->time_high_s, s->samples);

	if (s->samples > 0U) {
		bappend(c, ",\"min\":%d,\"max\":%d",
			(int)s->min_cc, (int)s->max_cc);
	}
	if (s->mkt_valid) {
		bappend(c, ",\"mkt\":%d", (int)s->mkt_cc);
	}
	bappend(c, "}");
}

/* ------------------------------------------------------------------ */

int payload_build_telemetry(char *buf, size_t size,
			    const char *device_id,
			    const struct sample_record *recs, size_t n,
			    const struct excursion_stats *stats,
			    uint32_t dropped,
			    size_t *out_len)
{
	struct bctx c;

	if (buf == NULL || device_id == NULL || recs == NULL ||
	    n == 0U || out_len == NULL) {
		return -EINVAL;
	}

	bctx_init(&c, buf, size);

	bappend(&c, "{\"v\":%d,\"id\":\"%s\",\"fw\":\"%s\",\"prof\":\"%s\"",
		APP_SCHEMA_VERSION, device_id, APP_FW_VERSION, APP_PROFILE_NAME);
	bappend(&c, ",\"lo\":%d,\"hi\":%d",
		(int)APP_TEMP_LO_CC, (int)APP_TEMP_HI_CC);
	bappend(&c, ",\"n\":%u,\"drop\":%u", (unsigned int)n, dropped);

	if (stats != NULL) {
		encode_stats(&c, stats);
	}

	bappend(&c, ",\"d\":[");
	for (size_t i = 0U; i < n; i++) {
		if (i > 0U) {
			bappend(&c, ",");
		}
		encode_record(&c, &recs[i]);
	}
	bappend(&c, "]}");

	if (c.ovf) {
		LOG_ERR("payload overflow: %u records will not fit %u bytes",
			(unsigned int)n, (unsigned int)size);
		return -ENOMEM;
	}

	*out_len = c.used;
	return 0;
}

int payload_build_shadow(char *buf, size_t size,
			 const struct excursion_stats *stats,
			 uint16_t batt_mv, uint32_t pending, uint32_t dropped,
			 size_t *out_len)
{
	struct bctx c;

	if (buf == NULL || out_len == NULL) {
		return -EINVAL;
	}

	bctx_init(&c, buf, size);

	bappend(&c, "{\"state\":{\"reported\":{");
	bappend(&c, "\"fw\":\"%s\",\"schema\":%d,\"prof\":\"%s\"",
		APP_FW_VERSION, APP_SCHEMA_VERSION, APP_PROFILE_NAME);
	bappend(&c, ",\"lo_cc\":%d,\"hi_cc\":%d",
		(int)APP_TEMP_LO_CC, (int)APP_TEMP_HI_CC);
	bappend(&c, ",\"sample_s\":%d,\"pub_s\":%d,\"gnss_s\":%d",
		APP_SAMPLE_PERIOD_S, APP_PUBLISH_PERIOD_S, APP_GNSS_PERIOD_S);
	bappend(&c, ",\"batt_mv\":%u,\"pending\":%u,\"dropped\":%u",
		(unsigned int)batt_mv, pending, dropped);

	if (stats != NULL) {
		bappend(&c, ",\"alarm\":%u,\"events\":%u",
			stats->alarm_active ? 1U : 0U, stats->events);
		if (stats->mkt_valid) {
			bappend(&c, ",\"mkt_cc\":%d", (int)stats->mkt_cc);
		}
	}

	bappend(&c, "}}}");

	if (c.ovf) {
		LOG_ERR("shadow payload overflow");
		return -ENOMEM;
	}

	*out_len = c.used;
	return 0;
}
