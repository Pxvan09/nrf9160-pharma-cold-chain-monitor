/*
 * IoT Pharma Monitoring Device - excursion detection and MKT
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "excursion.h"
#include "app_config.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <math.h>
#include <stdlib.h>   /* abs() - used in the log formatting below */

LOG_MODULE_REGISTER(excursion, CONFIG_PHARMA_LOG_LEVEL);

/* ------------------------------------------------------------------ *
 * record_compliance_temp() lives here because "which sensor is the
 * auditable one" is a compliance decision, not a storage detail.
 * ------------------------------------------------------------------ */
bool record_compliance_temp(const struct sample_record *rec, int16_t *out)
{
	if (rec == NULL || out == NULL) {
		return false;
	}

#if APP_COMPLIANCE_SRC_PROBE
	if (rec->flags & REC_FLAG_PROBE_VALID) {
		*out = rec->probe_cc;
		return true;
	}
#else
	if (rec->flags & REC_FLAG_AMB_VALID) {
		*out = rec->amb_cc;
		return true;
	}
#endif
	return false;
}

/* ------------------------------------------------------------------ */

static struct {
	bool     alarm_active;
	uint16_t consec_out;
	uint32_t events;
	uint32_t time_low_s;
	uint32_t time_high_s;
	int16_t  min_cc;
	int16_t  max_cc;
	uint32_t samples;

	/*
	 * MKT accumulator.
	 *
	 * MUST be double. At +5 C the exponent is -dH/(R*T) = -36.0, so each
	 * term is exp(-36.0) ~ 2.4e-16. Single precision has a smallest
	 * normal of ~1.2e-38 and only 24 bits of mantissa: summing thousands
	 * of 1e-16 terms in float loses catastrophic precision and can
	 * underflow outright at cold temperatures. The Cortex-M33 has only a
	 * single-precision FPU so this is a software double, but it runs
	 * once per sample (every 5 minutes), which is free.
	 */
	double   mkt_sum;
} st;

static struct k_mutex lock;

void excursion_init(void)
{
	k_mutex_init(&lock);

	st.alarm_active = false;
	st.consec_out   = 0U;
	st.events       = 0U;
	st.time_low_s   = 0U;
	st.time_high_s  = 0U;
	st.min_cc       = INT16_MAX;
	st.max_cc       = INT16_MIN;
	st.samples      = 0U;
	st.mkt_sum      = 0.0;

	LOG_INF("profile %s: window %d.%02d C .. %d.%02d C, debounce %d samples",
		APP_PROFILE_NAME,
		APP_TEMP_LO_CC / 100, abs(APP_TEMP_LO_CC % 100),
		APP_TEMP_HI_CC / 100, abs(APP_TEMP_HI_CC % 100),
		APP_EXCURSION_DEBOUNCE);
}

uint16_t excursion_update(const struct sample_record *rec, bool *alarm_edge)
{
	uint16_t flags = 0U;
	int16_t  t_cc;
	bool     out_of_window = false;
	double   kelvin;

	if (alarm_edge != NULL) {
		*alarm_edge = false;
	}

	if (!record_compliance_temp(rec, &t_cc)) {
		/* No auditable temperature this cycle. Do not touch the
		 * statistics: a missing sample must not look like a good one.
		 */
		LOG_WRN("sample seq %u has no compliance temperature", rec->seq);
		return 0U;
	}

	k_mutex_lock(&lock, K_FOREVER);

	/* ---- range check -------------------------------------------- */
	if (t_cc < APP_TEMP_LO_CC) {
		flags |= REC_FLAG_EXC_LOW;
		st.time_low_s += APP_SAMPLE_PERIOD_S;
		out_of_window = true;
	} else if (t_cc > APP_TEMP_HI_CC) {
		flags |= REC_FLAG_EXC_HIGH;
		st.time_high_s += APP_SAMPLE_PERIOD_S;
		out_of_window = true;
	}

#if APP_HUM_CHECK
	if ((rec->hum_cpct != REC_HUM_INVALID) &&
	    (rec->hum_cpct < APP_HUM_LO_CPCT || rec->hum_cpct > APP_HUM_HI_CPCT)) {
		flags |= REC_FLAG_EXC_HUM;
	}
#endif

	/* ---- debounce and latch ------------------------------------- */
	if (out_of_window) {
		if (st.consec_out < UINT16_MAX) {
			st.consec_out++;
		}

		if (st.consec_out == APP_EXCURSION_DEBOUNCE) {
			/*
			 * Rising edge of a confirmed excursion. Fires exactly
			 * once per event because consec_out keeps counting
			 * past the threshold and is only reset on return to
			 * the window.
			 */
			st.events++;
			if (alarm_edge != NULL) {
				*alarm_edge = true;
			}
			LOG_WRN("EXCURSION CONFIRMED: %d.%02d C outside [%d.%02d, %d.%02d] (event %u)",
				t_cc / 100, abs(t_cc % 100),
				APP_TEMP_LO_CC / 100, abs(APP_TEMP_LO_CC % 100),
				APP_TEMP_HI_CC / 100, abs(APP_TEMP_HI_CC % 100),
				st.events);
		}

		if (st.consec_out >= APP_EXCURSION_DEBOUNCE) {
			/* Latched for the rest of the shipment: an excursion
			 * that already happened cannot be undone by the
			 * temperature coming back into range.
			 */
			st.alarm_active = true;
		}
	} else {
		st.consec_out = 0U;
	}

	if (st.alarm_active) {
		flags |= REC_FLAG_ALARM;
	}

	/* ---- min / max ---------------------------------------------- */
	if (t_cc < st.min_cc) {
		st.min_cc = t_cc;
	}
	if (t_cc > st.max_cc) {
		st.max_cc = t_cc;
	}

	/* ---- MKT accumulation --------------------------------------- */
	kelvin = ((double)t_cc / 100.0) + 273.15;
	if (kelvin > 1.0) {   /* guard against absurd sensor faults */
		st.mkt_sum += exp(-APP_MKT_DELTA_H_J_PER_MOL /
				  (APP_GAS_CONST_J_PER_MOL_K * kelvin));
		st.samples++;
	}

	k_mutex_unlock(&lock);
	return flags;
}

bool excursion_alarm_active(void)
{
	return st.alarm_active;
}

void excursion_get_stats(struct excursion_stats *out)
{
	if (out == NULL) {
		return;
	}

	k_mutex_lock(&lock, K_FOREVER);

	out->alarm_active = st.alarm_active;
	out->events       = st.events;
	out->time_low_s   = st.time_low_s;
	out->time_high_s  = st.time_high_s;
	out->min_cc       = st.min_cc;
	out->max_cc       = st.max_cc;
	out->samples      = st.samples;
	out->mkt_valid    = false;
	out->mkt_cc       = 0;

	if (st.samples > 0U && st.mkt_sum > 0.0) {
		double mean = st.mkt_sum / (double)st.samples;
		double denom = -log(mean);

		if (denom > 1e-9) {
			double mkt_k = (APP_MKT_DELTA_H_J_PER_MOL /
					APP_GAS_CONST_J_PER_MOL_K) / denom;
			double mkt_c = mkt_k - 273.15;

			if (mkt_c > -273.0 && mkt_c < 300.0) {
				/* round-half-away-from-zero to centi-C */
				out->mkt_cc = (int16_t)(mkt_c * 100.0 +
						(mkt_c >= 0.0 ? 0.5 : -0.5));
				out->mkt_valid = true;
			}
		}
	}

	k_mutex_unlock(&lock);
}
