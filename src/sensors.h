/*
 * sensors.h - sensor abstraction (BME280 ambient + optional PT1000 probe)
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef SENSORS_H__
#define SENSORS_H__

#include "record.h"

/**
 * @brief Initialise all fitted sensors.
 * @return 0 on success, negative errno on failure.
 */
int sensors_init(void);

/**
 * @brief Perform a single-shot sample from all fitted sensors.
 *
 * Populates rec->amb_cc, rec->probe_cc, rec->hum_cpct, rec->press_pa
 * and sets the corresponding REC_FLAG_* bits in rec->flags.
 * Fields for unfitted sensors are set to REC_TEMP_INVALID / 0.
 *
 * @param rec  Record to populate (must be pre-zeroed by caller).
 * @return 0 on success, negative errno if primary sensor failed.
 */
int sensors_sample(struct sample_record *rec);

#endif /* SENSORS_H__ */
