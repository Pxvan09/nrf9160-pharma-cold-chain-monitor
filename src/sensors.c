/*
 * IoT Pharma Monitoring Device - sensor abstraction
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sensors.h"
#include "app_config.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <nrf_modem_at.h>

LOG_MODULE_REGISTER(sensors, CONFIG_PHARMA_LOG_LEVEL);

/* ------------------------------------------------------------------ *
 * Devicetree binding. Both sensors are optional.
 * ------------------------------------------------------------------ */
#define HAS_AMBIENT   DT_NODE_EXISTS(DT_ALIAS(env_sensor))
#define HAS_PROBE     DT_NODE_EXISTS(DT_ALIAS(probe_sensor))

#if HAS_AMBIENT
static const struct device *const dev_amb = DEVICE_DT_GET(DT_ALIAS(env_sensor));
#endif
#if HAS_PROBE
static const struct device *const dev_probe = DEVICE_DT_GET(DT_ALIAS(probe_sensor));
#endif

static bool amb_ok;
static bool probe_ok;

/* ------------------------------------------------------------------ *
 * Fixed-point conversion helpers.
 *
 * Zephyr sensor_value is {val1 = integer part, val2 = millionths}. For
 * negative readings BOTH members are negative, so plain truncating
 * integer division keeps the sign correct and we stay clear of any
 * floating point in the data path.
 * ------------------------------------------------------------------ */

/* value -> hundredths of the base unit (e.g. C -> centi-C) */
static inline int32_t sv_to_centi(const struct sensor_value *v)
{
	return (v->val1 * 100) + (v->val2 / 10000);
}

/* kPa -> Pa */
static inline int32_t sv_kpa_to_pa(const struct sensor_value *v)
{
	return (v->val1 * 1000) + (v->val2 / 1000);
}

static inline int16_t clamp_i16(int32_t v)
{
	if (v > INT16_MAX) {
		return INT16_MAX;
	}
	if (v < INT16_MIN + 1) {
		return INT16_MIN + 1;   /* keep INT16_MIN as the invalid marker */
	}
	return (int16_t)v;
}

/* ------------------------------------------------------------------ */

int sensors_init(void)
{
#if HAS_AMBIENT
	if (device_is_ready(dev_amb)) {
		amb_ok = true;
		LOG_INF("ambient sensor '%s' ready", dev_amb->name);
	} else {
		LOG_ERR("ambient sensor present in DT but not ready");
	}
#else
	LOG_WRN("no ambient sensor in devicetree");
#endif

#if HAS_PROBE
	if (device_is_ready(dev_probe)) {
		probe_ok = true;
		LOG_INF("RTD probe '%s' ready", dev_probe->name);
	} else {
		LOG_ERR("RTD probe present in DT but not ready");
	}
#else
	LOG_INF("no RTD probe in this build");
#endif

	/*
	 * Fail loudly if the selected cold-chain profile cannot physically
	 * be measured by the hardware that is actually fitted. Shipping a
	 * device that silently under-reports an ultra-cold excursion is the
	 * worst possible failure mode for this product.
	 */
#if APP_COMPLIANCE_SRC_PROBE
	if (!probe_ok) {
		LOG_ERR("profile %s needs the PT1000 probe, but none is present",
			APP_PROFILE_NAME);
		return -ENODEV;
	}
#else
	if (APP_TEMP_LO_CC < -4000) {
		LOG_ERR("profile %s goes below -40 C: BME280 is out of range.",
			APP_PROFILE_NAME);
		LOG_ERR("Fit the PT1000 probe and set APP_COMPLIANCE_SRC_PROBE=1.");
		return -ERANGE;
	}
	if (!amb_ok) {
		LOG_ERR("compliance source is ambient, but BME280 is not ready");
		return -ENODEV;
	}
#endif

	return 0;
}

int sensors_read(struct sensor_reading *out)
{
	int sources = 0;

	if (out == NULL) {
		return -EINVAL;
	}

	/* Start from explicit "no data" so a failed read can never be
	 * mistaken for a plausible measurement.
	 */
	out->amb_cc      = REC_TEMP_INVALID;
	out->probe_cc    = REC_TEMP_INVALID;
	out->hum_cpct    = REC_HUM_INVALID;
	out->press_pa    = REC_PRESS_INVALID;
	out->amb_valid   = false;
	out->probe_valid = false;

#if HAS_AMBIENT
	if (amb_ok) {
		struct sensor_value t, p, h;
		int rc = sensor_sample_fetch(dev_amb);

		if (rc) {
			LOG_WRN("BME280 fetch failed (%d)", rc);
		} else {
			int rt = sensor_channel_get(dev_amb,
						    SENSOR_CHAN_AMBIENT_TEMP, &t);
			int rp = sensor_channel_get(dev_amb,
						    SENSOR_CHAN_PRESS, &p);
			int rh = sensor_channel_get(dev_amb,
						    SENSOR_CHAN_HUMIDITY, &h);

			if (rt == 0) {
				out->amb_cc = clamp_i16(sv_to_centi(&t));
				out->amb_valid = true;
				sources++;
			}
			if (rp == 0) {
				int32_t pa = sv_kpa_to_pa(&p);

				if (pa > 0) {
					out->press_pa = (uint32_t)pa;
				}
			}
			if (rh == 0) {
				int32_t hc = sv_to_centi(&h);

				if (hc >= 0 && hc <= 10000) {
					out->hum_cpct = (uint16_t)hc;
				}
			}
		}
	}
#endif

#if HAS_PROBE
	if (probe_ok) {
		struct sensor_value t;
		int rc = sensor_sample_fetch(dev_probe);

		if (rc) {
			LOG_WRN("RTD fetch failed (%d)", rc);
		} else if (sensor_channel_get(dev_probe,
					      SENSOR_CHAN_AMBIENT_TEMP, &t) == 0) {
			out->probe_cc = clamp_i16(sv_to_centi(&t));
			out->probe_valid = true;
			sources++;
		}
	}
#endif

	if (sources == 0) {
		LOG_ERR("no temperature source produced a reading");
		return -EIO;
	}
	return 0;
}

int sensors_battery_mv(uint16_t *mv)
{
	int voltage = 0;
	int rc;

	if (mv == NULL) {
		return -EINVAL;
	}

	/*
	 * %XVBAT returns the SiP supply voltage in millivolts. Using the
	 * modem's own measurement avoids a resistor divider and a wasted
	 * SAADC channel on the custom PCB.
	 *
	 * The '%' of the response prefix must be escaped as '%%'.
	 */
	rc = nrf_modem_at_scanf("AT%XVBAT", "%%XVBAT: %d", &voltage);
	if (rc != 1) {
		LOG_WRN("XVBAT read failed (%d)", rc);
		return -EIO;
	}

	if (voltage < 0 || voltage > UINT16_MAX) {
		return -ERANGE;
	}

	*mv = (uint16_t)voltage;
	return 0;
}

bool sensors_probe_present(void)
{
	return probe_ok;
}
