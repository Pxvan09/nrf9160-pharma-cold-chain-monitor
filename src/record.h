/*
 * record.h - on-flash telemetry record (40 bytes, 8-byte aligned)
 *
 * Layout is hand-packed with zero padding and pinned by _Static_assert
 * so a field reorder cannot silently orphan previously stored data.
 * Integer-only (centi-Celsius, centi-%RH) for exact, endian-stable
 * flash round-trips and CRC verification.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RECORD_H__
#define RECORD_H__

#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------- */
/* Flags (record.flags bitmask)                                    */
/* -------------------------------------------------------------- */
#define REC_FLAG_TIME_VALID   (1u << 0) /* ts_ms is network-synced  */
#define REC_FLAG_AMB_VALID    (1u << 1) /* amb_cc / hum / press OK  */
#define REC_FLAG_PROBE_VALID  (1u << 2) /* probe_cc is valid        */
#define REC_FLAG_FIX_VALID    (1u << 3) /* lat/lon GNSS fix present */
#define REC_FLAG_EXC_LOW      (1u << 4) /* below low  threshold     */
#define REC_FLAG_EXC_HIGH     (1u << 5) /* above high threshold     */
#define REC_FLAG_DROPPED      (1u << 6) /* gap: record(s) lost      */

/* Sentinel: probe not fitted / not read */
#define REC_TEMP_INVALID  ((int16_t)0x8000)

/* -------------------------------------------------------------- */
/* On-flash record - 40 bytes, _Static_assert-pinned              */
/* -------------------------------------------------------------- */
struct sample_record {
    int64_t  ts_ms;     /* 0  UNIX epoch milliseconds              */
    uint32_t seq;       /* 8  monotonic counter, never resets      */
    int32_t  lat_e7;    /* 12 latitude  × 1e7 (1.1 cm resolution) */
    int32_t  lon_e7;    /* 16 longitude × 1e7                      */
    uint32_t press_pa;  /* 20 pressure in Pascals                  */
    int16_t  amb_cc;    /* 24 ambient temperature, centi-Celsius   */
    int16_t  probe_cc;  /* 26 probe temperature,  centi-Celsius    */
    uint16_t hum_cpct;  /* 28 humidity, centi-percent-RH           */
    uint16_t batt_mv;   /* 30 battery voltage, millivolts          */
    uint16_t flags;     /* 32 REC_FLAG_* bitmask                   */
    uint16_t rsv16;     /* 34 reserved, write 0                    */
    uint32_t rsv32;     /* 36 reserved, write 0                    */
};                      /* 40 bytes total                          */

_Static_assert(sizeof(struct sample_record) == 40,
               "sample_record layout changed - update NVS and cloud schema");

#endif /* RECORD_H__ */
