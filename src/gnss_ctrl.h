/*
 * gnss_ctrl.h - motion-gated GNSS single-fix controller
 *
 * One GNSS fix costs ~21 uplinks of energy. Gating fixes on motion is
 * the highest-leverage power optimisation in this firmware.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef GNSS_CTRL_H__
#define GNSS_CTRL_H__

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Initialise GNSS subsystem. Call once at boot.
 * @return 0 on success, negative errno on failure.
 */
int gnss_init(void);

/**
 * @brief Request a single GNSS fix (non-blocking, result via callback).
 *
 * The modem time-multiplexes LTE and GNSS. Do not call while an active
 * LTE data transfer is in progress.
 *
 * @return 0 if fix started, -EBUSY if already in progress.
 */
int gnss_request_fix(void);

/**
 * @brief Copy the most recent valid fix into *lat_e7 / *lon_e7.
 *
 * @param lat_e7  Latitude  × 1e7 (output).
 * @param lon_e7  Longitude × 1e7 (output).
 * @return true if a valid cached fix was available, false otherwise.
 */
bool gnss_get_last_fix(int32_t *lat_e7, int32_t *lon_e7);

/**
 * @brief Return true if the cached fix is less than max_age_s seconds old.
 * @param max_age_s  Freshness threshold in seconds.
 */
bool gnss_is_fix_fresh(uint32_t max_age_s);

#endif /* GNSS_CTRL_H__ */
