/*
 * excursion.h - temperature excursion detection, debounce, and MKT
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EXCURSION_H__
#define EXCURSION_H__

#include <stdint.h>
#include <stdbool.h>
#include "record.h"

/** Statistics snapshot returned by excursion_get_stats(). */
struct excursion_stats {
    uint32_t samples;       /* total samples processed              */
    uint32_t events;        /* number of confirmed excursion events */
    int16_t  min_cc;        /* minimum temperature seen, centi-°C  */
    int16_t  max_cc;        /* maximum temperature seen, centi-°C  */
    uint32_t time_high_s;   /* cumulative seconds above high limit  */
    uint32_t time_low_s;    /* cumulative seconds below low  limit  */
    bool     alarm_active;  /* excursion alarm currently raised     */
    bool     mkt_valid;     /* mkt_cc is a valid result             */
    int32_t  mkt_cc;        /* Mean Kinetic Temperature, centi-°C  */
};

/**
 * @brief Initialise the excursion module. Call once at boot.
 */
void excursion_init(void);

/**
 * @brief Process one sample record.
 *
 * Updates internal state. If this sample triggers or clears an alarm
 * edge, *alarm_edge is set true so the caller can publish immediately
 * instead of waiting for the next scheduled uplink.
 *
 * @param rec        Completed sample record.
 * @param alarm_edge Set to true on alarm state *change* (raise or clear).
 * @return flags bitmask (REC_FLAG_EXC_LOW / REC_FLAG_EXC_HIGH).
 */
uint16_t excursion_update(const struct sample_record *rec, bool *alarm_edge);

/**
 * @brief Copy current statistics into *out (thread-safe snapshot).
 * @param out  Destination buffer, must not be NULL.
 */
void excursion_get_stats(struct excursion_stats *out);

/**
 * @brief Reset all statistics (e.g. new shipment start).
 */
void excursion_reset(void);

#endif /* EXCURSION_H__ */
