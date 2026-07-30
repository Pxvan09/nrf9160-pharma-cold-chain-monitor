/*
 * payload.h - integer-only JSON encoder for MQTT telemetry payloads
 *
 * All values transmitted as integers (centi-units) to avoid %f printf
 * dependency and to keep values exactly representable in the audit trail.
 * Overflow returns -ENOMEM rather than truncating: a partial audit record
 * must never be transmitted.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PAYLOAD_H__
#define PAYLOAD_H__

#include <stddef.h>
#include "record.h"
#include "excursion.h"

/**
 * @brief Build a telemetry JSON payload for one batch of records.
 *
 * @param buf        Output buffer.
 * @param buf_len    Size of buf in bytes.
 * @param device_id  Null-terminated device identifier string.
 * @param recs       Array of records to include.
 * @param n_recs     Number of records (must be >= 1).
 * @param stats      Excursion stats snapshot, or NULL to omit.
 * @param n_stats    Ignored (reserved for future expansion, pass 0).
 * @param out_len    Set to strlen of the resulting JSON on success.
 * @return 0 on success, -EINVAL on bad args, -ENOMEM if buf too small.
 */
int payload_build_telemetry(char *buf, size_t buf_len,
                            const char *device_id,
                            const struct sample_record *recs, size_t n_recs,
                            const struct excursion_stats *stats,
                            size_t n_stats,
                            size_t *out_len);

/**
 * @brief Build an AWS Device Shadow update payload.
 *
 * @param buf        Output buffer.
 * @param buf_len    Size of buf in bytes.
 * @param stats      Current excursion statistics.
 * @param batt_mv    Battery voltage in millivolts.
 * @param rssi       Signal strength indicator (dBm magnitude, 0-255).
 * @param profile_id Active temperature profile ID.
 * @param out_len    Set to strlen of the resulting JSON on success.
 * @return 0 on success, -EINVAL on bad args, -ENOMEM if buf too small.
 */
int payload_build_shadow(char *buf, size_t buf_len,
                         const struct excursion_stats *stats,
                         uint16_t batt_mv, uint8_t rssi,
                         uint8_t profile_id,
                         size_t *out_len);

#endif /* PAYLOAD_H__ */
