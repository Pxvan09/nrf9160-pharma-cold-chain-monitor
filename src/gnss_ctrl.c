/*
 * IoT Pharma Monitoring Device - GNSS control
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gnss_ctrl.h"
#include "app_config.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <nrf_modem_gnss.h>
#include <string.h>
#include <stdlib.h>   /* abs() - used in the log formatting below */

LOG_MODULE_REGISTER(gnss, CONFIG_PHARMA_LOG_LEVEL);

static K_SEM_DEFINE(fix_sem, 0, 1);

static struct nrf_modem_gnss_pvt_data_frame pvt;   /* written from ISR */
static struct gnss_fix last_fix;
static volatile bool   fix_ready;
static volatile bool   sleep_timeout;
static volatile bool   blocked;
static int64_t         start_uptime_ms;

/* ------------------------------------------------------------------ *
 * Event handler.
 *
 * Runs in interrupt context. Keep it to: read the frame, set a flag,
 * signal a semaphore. All interpretation happens in gnss_ctrl_fix().
 * ------------------------------------------------------------------ */
static void gnss_event_handler(int event_id)
{
	int err;

	switch (event_id) {
	case NRF_MODEM_GNSS_EVT_PVT:
		err = nrf_modem_gnss_read(&pvt, sizeof(pvt),
					  NRF_MODEM_GNSS_DATA_PVT);
		if (err) {
			break;
		}
		if (pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
			fix_ready = true;
			k_sem_give(&fix_sem);
		}
		/*
		 * If LTE keeps stealing the receive windows the modem sets
		 * NOT_ENOUGH_WINDOW_TIME. Record it so the caller can decide
		 * whether to schedule fixes at a quieter moment.
		 */
		if (pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_NOT_ENOUGH_WINDOW_TIME) {
			blocked = true;
		}
		break;

	case NRF_MODEM_GNSS_EVT_BLOCKED:
		blocked = true;
		break;

	case NRF_MODEM_GNSS_EVT_SLEEP_AFTER_TIMEOUT:
		/* Retry budget exhausted: stop waiting immediately instead
		 * of burning the full caller timeout with the receiver on.
		 */
		sleep_timeout = true;
		k_sem_give(&fix_sem);
		break;

	default:
		break;
	}
}

/* ------------------------------------------------------------------ */

int gnss_ctrl_init(void)
{
	int err;

	memset(&last_fix, 0, sizeof(last_fix));

	err = nrf_modem_gnss_event_handler_set(gnss_event_handler);
	if (err) {
		LOG_ERR("event_handler_set failed (%d)", err);
		return err;
	}

	/*
	 * Single-fix mode: interval 0 means "one fix then stop".
	 * (1 would be continuous navigation, >=10 periodic.)
	 */
	err = nrf_modem_gnss_fix_interval_set(0);
	if (err) {
		LOG_ERR("fix_interval_set failed (%d)", err);
		return err;
	}

	err = nrf_modem_gnss_fix_retry_set(APP_GNSS_TIMEOUT_S);
	if (err) {
		LOG_ERR("fix_retry_set failed (%d)", err);
		return err;
	}

	/*
	 * Multiple hot starts: we fix repeatedly over a shipment, hours
	 * apart, so the receiver should keep its data between runs.
	 */
	err = nrf_modem_gnss_use_case_set(NRF_MODEM_GNSS_USE_CASE_MULTIPLE_HOT_START);
	if (err) {
		LOG_WRN("use_case_set failed (%d), continuing with default", err);
	}

	LOG_INF("GNSS ready (single fix, %d s retry)", APP_GNSS_TIMEOUT_S);
	return 0;
}

int gnss_ctrl_fix(struct gnss_fix *out, uint32_t timeout_s)
{
	int err;
	int ret = 0;

	if (out == NULL) {
		return -EINVAL;
	}

	memset(out, 0, sizeof(*out));

	fix_ready     = false;
	sleep_timeout = false;
	blocked       = false;
	k_sem_reset(&fix_sem);

	err = nrf_modem_gnss_fix_retry_set((uint16_t)timeout_s);
	if (err) {
		LOG_WRN("fix_retry_set failed (%d)", err);
	}

	start_uptime_ms = k_uptime_get();

	err = nrf_modem_gnss_start();
	if (err) {
		LOG_ERR("gnss_start failed (%d)", err);
		return err;
	}

	/* Wait a little beyond the modem's own retry budget so that the
	 * SLEEP_AFTER_TIMEOUT event is the normal exit path.
	 */
	if (k_sem_take(&fix_sem, K_SECONDS(timeout_s + 5U)) != 0) {
		LOG_WRN("GNSS wait expired with no modem event");
		ret = -ETIMEDOUT;
	} else if (!fix_ready) {
		LOG_WRN("GNSS timed out%s", blocked ? " (LTE was blocking)" : "");
		ret = -ETIMEDOUT;
	}

	/* Always stop the receiver: leaving it running is a ~50 mA leak. */
	err = nrf_modem_gnss_stop();
	if (err) {
		LOG_WRN("gnss_stop failed (%d)", err);
	}

	if (ret == 0) {
		out->lat_e7 = (int32_t)(pvt.latitude  * 1e7);
		out->lon_e7 = (int32_t)(pvt.longitude * 1e7);
		out->alt_cm = (int32_t)(pvt.altitude  * 100.0f);
		out->accuracy_dm = (uint16_t)(pvt.accuracy * 10.0f);
		out->ttff_s = (uint32_t)((k_uptime_get() - start_uptime_ms) / 1000);
		out->valid = true;

		last_fix = *out;

		LOG_INF("fix: lat=%d.%07d lon=%d.%07d acc=%u.%u m ttff=%u s",
			out->lat_e7 / 10000000, abs(out->lat_e7 % 10000000),
			out->lon_e7 / 10000000, abs(out->lon_e7 % 10000000),
			out->accuracy_dm / 10, out->accuracy_dm % 10,
			out->ttff_s);
	}

	return ret;
}

void gnss_ctrl_last_fix(struct gnss_fix *out)
{
	if (out != NULL) {
		*out = last_fix;
	}
}

bool gnss_ctrl_was_blocked(void)
{
	return blocked;
}
