/*
 * IoT Pharma Monitoring Device
 *
 * Cold-chain monitor built on the nRF9160: temperature, pressure,
 * humidity, GNSS location and excursion/MKT statistics, buffered to
 * flash and forwarded to AWS IoT Core over LTE-M / NB-IoT.
 *
 * CONTROL FLOW
 *   A single scheduling thread drives everything from monotonic uptime
 *   deadlines. This is deliberate: with one thread there are no races
 *   between sampling, publishing and GNSS, the ordering of events is
 *   reproducible, and the timing is trivial to reason about during a
 *   design review. Asynchronous work (modem, MQTT) arrives through
 *   callbacks that only ever set flags or give semaphores.
 *
 *   boot
 *     -> modem init -> storage -> sensors -> GNSS -> cloud lib
 *     -> LTE attach -> time sync -> backfill timestamps -> MQTT connect
 *     -> loop { sample | GNSS fix | publish | sleep }
 *
 * DATA INTEGRITY RULE
 *   Every sample is committed to flash before any transmission is
 *   attempted, and is only released after the broker returns a PUBACK.
 *   Loss of coverage therefore cannot create a hole in the record.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_config.h"
#include "record.h"
#include "log_store.h"
#include "sensors.h"
#include "gnss_ctrl.h"
#include "excursion.h"
#include "payload.h"
#include "cloud_client.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>
#include <date_time.h>

#include <string.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(main, CONFIG_PHARMA_LOG_LEVEL);

/* ------------------------------------------------------------------ *
 * Static working storage.
 *
 * Everything the hot path needs is allocated statically so that memory
 * exhaustion is a build-time property, not a field failure.
 * ------------------------------------------------------------------ */
static struct sample_record batch[APP_PUBLISH_BATCH_MAX];
static char                 payload_buf[APP_PAYLOAD_BUF_SIZE];

static K_SEM_DEFINE(lte_ready_sem, 0, 1);
static K_SEM_DEFINE(time_ready_sem, 0, 1);

static volatile bool lte_registered;
static volatile bool time_valid;

/* Offset that converts device uptime into UTC, learned at first sync. */
static int64_t uptime_to_utc_ms;
static bool    time_backfilled;

/* Scheduling deadlines, in uptime milliseconds. */
static int64_t next_sample_ms;
static int64_t next_publish_ms;
static int64_t next_gnss_ms;
static int64_t last_alarm_ms;

static uint32_t reconnect_backoff_s = APP_RECONNECT_BACKOFF_MIN_S;

/* ------------------------------------------------------------------ *
 * Modem and network callbacks
 * ------------------------------------------------------------------ */

static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		switch (evt->nw_reg_status) {
		case LTE_LC_NW_REG_REGISTERED_HOME:
		case LTE_LC_NW_REG_REGISTERED_ROAMING:
			if (!lte_registered) {
				lte_registered = true;
				LOG_INF("LTE registered (%s)",
					evt->nw_reg_status ==
						LTE_LC_NW_REG_REGISTERED_HOME ?
						"home" : "roaming");
				k_sem_give(&lte_ready_sem);
			}
			break;
		case LTE_LC_NW_REG_NOT_REGISTERED:
		case LTE_LC_NW_REG_REGISTRATION_DENIED:
		case LTE_LC_NW_REG_UNKNOWN:
		case LTE_LC_NW_REG_UICC_FAIL:
			if (lte_registered) {
				LOG_WRN("LTE registration lost (%d)",
					evt->nw_reg_status);
			}
			lte_registered = false;
			break;
		default:
			break;
		}
		break;

	case LTE_LC_EVT_PSM_UPDATE:
		/*
		 * These are the values the NETWORK granted, which are often
		 * not what we requested. Log them: if the active timer is
		 * long the device will not reach the 2.7 uA floor and the
		 * battery estimate in docs/POWER_BUDGET.md will not hold.
		 */
		LOG_INF("PSM granted: TAU %d s, active %d s",
			evt->psm_cfg.tau, evt->psm_cfg.active_time);
		if (evt->psm_cfg.active_time < 0) {
			LOG_WRN("network refused PSM - expect much higher current");
		}
		break;

	case LTE_LC_EVT_EDRX_UPDATE:
		LOG_INF("eDRX granted: %d s cycle", (int)evt->edrx_cfg.edrx);
		break;

	case LTE_LC_EVT_RRC_UPDATE:
		LOG_DBG("RRC %s", evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ?
			"connected" : "idle");
		break;

	default:
		break;
	}
}

static void date_time_handler(const struct date_time_evt *evt)
{
	switch (evt->type) {
	case DATE_TIME_OBTAINED_MODEM:
	case DATE_TIME_OBTAINED_NTP:
	case DATE_TIME_OBTAINED_EXT:
		if (!time_valid) {
			time_valid = true;
			k_sem_give(&time_ready_sem);
		}
		break;
	case DATE_TIME_NOT_OBTAINED:
		LOG_WRN("date-time not obtained");
		break;
	default:
		break;
	}
}

static void cloud_state_changed(bool connected)
{
	if (connected) {
		reconnect_backoff_s = APP_RECONNECT_BACKOFF_MIN_S;
	}
}

static void cloud_cmd_received(const char *topic, size_t topic_len,
			       const char *payload, size_t payload_len)
{
	ARG_UNUSED(topic);
	ARG_UNUSED(topic_len);

	/*
	 * Downlink commands are intentionally minimal for now. Anything
	 * that changes the alarm window or the sampling cadence of a
	 * regulated device must be authenticated and audited, so it is
	 * left as an explicit integration step rather than a silent
	 * remote-write path.
	 */
	LOG_INF("command received (%u bytes): %.*s",
		(unsigned int)payload_len, (int)payload_len, payload);
}

/* ------------------------------------------------------------------ *
 * Time handling
 * ------------------------------------------------------------------ */

/*
 * Return a timestamp for a record.
 *
 * Before the modem has network time we still have to log - a shipment
 * does not wait for a base station. Records taken in that window carry
 * uptime and are flagged as not-absolute; once time arrives they are
 * corrected in place by log_store_backfill_time().
 */
static int64_t record_timestamp(bool *absolute)
{
	int64_t utc_ms;

	if (date_time_now(&utc_ms) == 0) {
		*absolute = true;
		return utc_ms;
	}

	*absolute = false;
	return k_uptime_get();
}

static void try_backfill_time(void)
{
	int64_t utc_ms;

	if (time_backfilled || !time_valid) {
		return;
	}
	if (date_time_now(&utc_ms) != 0) {
		return;
	}

	uptime_to_utc_ms = utc_ms - k_uptime_get();
	(void)log_store_backfill_time(uptime_to_utc_ms);
	time_backfilled = true;
}

/* ------------------------------------------------------------------ *
 * Sampling
 * ------------------------------------------------------------------ */

static bool sample_once(void)
{
	struct sample_record rec;
	struct sensor_reading sr;
	struct gnss_fix fix;
	bool absolute = false;
	bool alarm_edge = false;
	uint16_t batt = 0U;
	int err;

	memset(&rec, 0, sizeof(rec));

	rec.ts_ms    = record_timestamp(&absolute);
	rec.amb_cc   = REC_TEMP_INVALID;
	rec.probe_cc = REC_TEMP_INVALID;
	rec.hum_cpct = REC_HUM_INVALID;
	rec.press_pa = REC_PRESS_INVALID;

	if (absolute) {
		rec.flags |= REC_FLAG_TIME_VALID;
	}

	err = sensors_read(&sr);
	if (err) {
		LOG_ERR("sensor read failed (%d) - logging an empty record", err);
	} else {
		rec.amb_cc   = sr.amb_cc;
		rec.probe_cc = sr.probe_cc;
		rec.hum_cpct = sr.hum_cpct;
		rec.press_pa = sr.press_pa;

		if (sr.amb_valid) {
			rec.flags |= REC_FLAG_AMB_VALID;
		}
		if (sr.probe_valid) {
			rec.flags |= REC_FLAG_PROBE_VALID;
		}
	}

	if (sensors_battery_mv(&batt) == 0) {
		rec.batt_mv = batt;
	}

	/* Attach the most recent GNSS fix. Location changes far more
	 * slowly than temperature, so re-using the last fix is correct
	 * and avoids a 50 mA receiver run on every sample.
	 */
	gnss_ctrl_last_fix(&fix);
	if (fix.valid) {
		rec.lat_e7 = fix.lat_e7;
		rec.lon_e7 = fix.lon_e7;
		rec.flags |= REC_FLAG_FIX_VALID;
	}

	rec.flags |= excursion_update(&rec, &alarm_edge);

	err = log_store_push(&rec);
	if (err) {
		LOG_ERR("failed to persist sample (%d)", err);
	} else {
		int16_t t_cc;

		if (record_compliance_temp(&rec, &t_cc)) {
			LOG_INF("seq %u: %d.%02d C, %u.%02u %%RH, %u Pa, %u mV%s",
				rec.seq, t_cc / 100, abs(t_cc % 100),
				rec.hum_cpct == REC_HUM_INVALID ? 0U :
					rec.hum_cpct / 100U,
				rec.hum_cpct == REC_HUM_INVALID ? 0U :
					rec.hum_cpct % 100U,
				rec.press_pa == REC_PRESS_INVALID ? 0U :
					rec.press_pa,
				rec.batt_mv,
				(rec.flags & REC_FLAG_ALARM) ? "  [ALARM]" : "");
		}
	}

	return alarm_edge;
}

/* ------------------------------------------------------------------ *
 * Publishing
 * ------------------------------------------------------------------ */

static int ensure_cloud(void)
{
	int err;

	if (cloud_client_is_connected()) {
		return 0;
	}
	if (!lte_registered) {
		return -ENETDOWN;
	}

	LOG_INF("reconnecting to cloud");

	err = cloud_client_connect(APP_CLOUD_CONNECT_TIMEOUT_S);
	if (err) {
		LOG_WRN("cloud connect failed (%d), backing off %u s",
			err, reconnect_backoff_s);
		k_sleep(K_SECONDS(reconnect_backoff_s));

		reconnect_backoff_s *= 2U;
		if (reconnect_backoff_s > APP_RECONNECT_BACKOFF_MAX_S) {
			reconnect_backoff_s = APP_RECONNECT_BACKOFF_MAX_S;
		}
		return err;
	}

	reconnect_backoff_s = APP_RECONNECT_BACKOFF_MIN_S;
	return 0;
}

/*
 * Drain the flash buffer to the broker, one batch at a time.
 *
 * Returns the number of records confirmed delivered. Stops at the first
 * failure and leaves everything still unacknowledged in flash so the
 * next attempt resumes exactly where this one stopped.
 */
static int publish_pending(void)
{
	struct excursion_stats stats;
	int delivered = 0;

	if (ensure_cloud() != 0) {
		return 0;
	}

	excursion_get_stats(&stats);

	while (log_store_pending() > 0U) {
		size_t count = 0U;
		size_t len = 0U;
		uint32_t last_seq;
		int err;

		err = log_store_peek(batch, ARRAY_SIZE(batch), &count);
		if (err || count == 0U) {
			break;
		}

		err = payload_build_telemetry(payload_buf, sizeof(payload_buf),
					      cloud_client_id(), batch, count,
					      &stats, log_store_dropped(), &len);
		if (err == -ENOMEM && count > 1U) {
			/*
			 * Defensive: should not happen with the configured
			 * batch size, but if a record ever encodes larger
			 * than expected, halve the batch rather than stall
			 * the pipeline forever.
			 */
			LOG_WRN("batch of %u too large, retrying with %u",
				(unsigned int)count, (unsigned int)(count / 2U));
			count /= 2U;
			err = payload_build_telemetry(payload_buf,
						      sizeof(payload_buf),
						      cloud_client_id(),
						      batch, count, &stats,
						      log_store_dropped(), &len);
		}
		if (err) {
			LOG_ERR("payload build failed (%d)", err);
			break;
		}

		last_seq = batch[count - 1U].seq;

		err = cloud_client_publish_telemetry(payload_buf, len, 30U);
		if (err) {
			LOG_WRN("publish failed (%d), %u records stay buffered",
				err, log_store_pending());
			break;
		}

		/* Broker has acknowledged: safe to release from flash. */
		(void)log_store_ack(last_seq);
		delivered += (int)count;
	}

	if (delivered > 0) {
		LOG_INF("delivered %d records, %u still pending",
			delivered, log_store_pending());
	}

	return delivered;
}

static void publish_alarm(void)
{
	struct excursion_stats stats;
	size_t count = 0U, len = 0U;
	int64_t now = k_uptime_get();

	if ((last_alarm_ms != 0) &&
	    (now - last_alarm_ms < (int64_t)APP_ALARM_MIN_INTERVAL_S * 1000)) {
		LOG_INF("alarm suppressed (rate limit)");
		return;
	}

	if (ensure_cloud() != 0) {
		LOG_WRN("alarm raised but cloud unreachable - buffered in flash");
		return;
	}

	excursion_get_stats(&stats);

	/* Send the most recent records so the alarm carries its own
	 * evidence rather than just a flag.
	 */
	if (log_store_peek(batch, ARRAY_SIZE(batch), &count) != 0 || count == 0U) {
		return;
	}

	if (payload_build_telemetry(payload_buf, sizeof(payload_buf),
				    cloud_client_id(), batch, count, &stats,
				    log_store_dropped(), &len) != 0) {
		return;
	}

	if (cloud_client_publish_alarm(payload_buf, len, 30U) == 0) {
		last_alarm_ms = now;
		LOG_WRN("ALARM published");
	}
}

static void publish_shadow(void)
{
	struct excursion_stats stats;
	size_t len = 0U;
	uint16_t batt = 0U;

	if (!cloud_client_is_connected()) {
		return;
	}

	excursion_get_stats(&stats);
	(void)sensors_battery_mv(&batt);

	if (payload_build_shadow(payload_buf, sizeof(payload_buf), &stats,
				 batt, log_store_pending(),
				 log_store_dropped(), &len) == 0) {
		(void)cloud_client_publish_shadow(payload_buf, len);
	}
}

/* ------------------------------------------------------------------ *
 * GNSS
 * ------------------------------------------------------------------ */

static void do_gnss_fix(void)
{
	struct gnss_fix fix;
	int err;

	/*
	 * Take the fix while the modem is idle. GNSS and LTE share one RF
	 * front end, so running a fix during a publish makes both slower
	 * and more expensive.
	 */
	LOG_INF("starting GNSS fix (timeout %d s)", APP_GNSS_TIMEOUT_S);

	err = gnss_ctrl_fix(&fix, APP_GNSS_TIMEOUT_S);
	if (err) {
		LOG_WRN("no GNSS fix this cycle (%d)%s", err,
			gnss_ctrl_was_blocked() ? " - LTE contention" : "");
	}
}

/* ------------------------------------------------------------------ *
 * Startup
 * ------------------------------------------------------------------ */

static int bring_up_network(void)
{
	int err;

	LOG_INF("attaching to LTE (this can take minutes on first boot)");

	err = lte_lc_connect_async(lte_handler);
	if (err) {
		LOG_ERR("lte_lc_connect_async failed (%d)", err);
		return err;
	}

	if (k_sem_take(&lte_ready_sem,
		       K_SECONDS(APP_LTE_CONNECT_TIMEOUT_S)) != 0) {
		LOG_ERR("LTE attach timed out after %d s",
			APP_LTE_CONNECT_TIMEOUT_S);
		return -ETIMEDOUT;
	}

	return 0;
}

static void bring_up_time(void)
{
	date_time_register_handler(date_time_handler);
	(void)date_time_update_async(NULL);

	if (k_sem_take(&time_ready_sem,
		       K_SECONDS(APP_TIME_SYNC_TIMEOUT_S)) == 0) {
		LOG_INF("network time obtained");
	} else {
		LOG_WRN("no network time yet - logging with uptime, will backfill");
	}

	try_backfill_time();
}

int main(void)
{
	int err;
	int64_t now;

	LOG_INF("=====================================================");
	LOG_INF(" IoT Pharma Monitoring Device  v%s", APP_FW_VERSION);
	LOG_INF(" profile %s  sample %ds  publish %ds  gnss %ds",
		APP_PROFILE_NAME, APP_SAMPLE_PERIOD_S,
		APP_PUBLISH_PERIOD_S, APP_GNSS_PERIOD_S);
	LOG_INF("=====================================================");

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("nrf_modem_lib_init failed (%d) - halting", err);
		return err;
	}

	err = log_store_init();
	if (err) {
		LOG_ERR("storage init failed (%d) - halting", err);
		return err;
	}

	err = sensors_init();
	if (err) {
		/*
		 * Refuse to run rather than silently produce an unusable
		 * compliance record. This is the correct behaviour for a
		 * regulated device: no data is better than wrong data.
		 */
		LOG_ERR("sensor configuration invalid (%d) - halting", err);
		return err;
	}

	excursion_init();

	err = gnss_ctrl_init();
	if (err) {
		LOG_WRN("GNSS init failed (%d) - continuing without location",
			err);
	}

	err = cloud_client_init(cloud_cmd_received, cloud_state_changed);
	if (err) {
		LOG_ERR("cloud init failed (%d) - halting", err);
		return err;
	}

	/*
	 * Start sampling immediately, before the network is up. A shipment
	 * that begins in a warehouse with no coverage must still be logged
	 * from the first minute.
	 */
	now = k_uptime_get();
	next_sample_ms  = now;
	next_gnss_ms    = now + 30000;   /* let the modem settle first */
	next_publish_ms = now + (int64_t)APP_PUBLISH_PERIOD_S * 1000;

	if (bring_up_network() == 0) {
		bring_up_time();
		if (ensure_cloud() == 0) {
			publish_shadow();
		}
	} else {
		LOG_WRN("starting offline - samples will buffer to flash");
	}

	LOG_INF("entering main loop");

	while (1) {
		int64_t sleep_ms;
		bool alarm_edge = false;

		now = k_uptime_get();

		/* ---- sample ---------------------------------------- */
		if (now >= next_sample_ms) {
			alarm_edge = sample_once();
			next_sample_ms = now +
					 (int64_t)APP_SAMPLE_PERIOD_S * 1000;

			try_backfill_time();
		}

		/* ---- alarm: publish out of turn --------------------- */
		if (alarm_edge && APP_EXCURSION_IMMEDIATE) {
			publish_alarm();
			/* The alarm carried the buffered records, so let the
			 * normal drain follow immediately to clear them.
			 */
			(void)publish_pending();
			next_publish_ms = k_uptime_get() +
					  (int64_t)APP_PUBLISH_PERIOD_S * 1000;
		}

		/* ---- GNSS ------------------------------------------ */
		now = k_uptime_get();
		if (now >= next_gnss_ms) {
			do_gnss_fix();
			next_gnss_ms = k_uptime_get() +
				       (int64_t)APP_GNSS_PERIOD_S * 1000;
		}

		/* ---- scheduled publish ----------------------------- */
		now = k_uptime_get();
		if (now >= next_publish_ms) {
			(void)publish_pending();
			publish_shadow();
			next_publish_ms = k_uptime_get() +
					  (int64_t)APP_PUBLISH_PERIOD_S * 1000;
		}

		/* ---- sleep until the next deadline ------------------ */
		now = k_uptime_get();
		sleep_ms = MIN(next_sample_ms, MIN(next_gnss_ms, next_publish_ms))
			   - now;

		if (sleep_ms < 10) {
			sleep_ms = 10;      /* never spin */
		}

		/* Cast to int: sleep_ms is bounded by the longest period
		 * (6 h = 21.6e6 ms) and fits comfortably in 32 bits. Avoids
		 * depending on 64-bit conversions in Zephyr's reduced printf.
		 */
		LOG_DBG("idle for %d ms", (int)sleep_ms);
		k_sleep(K_MSEC(sleep_ms));
	}

	return 0;
}
