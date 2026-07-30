#!/usr/bin/env bash
# setup.sh  -  creates every missing file in the nrf9160-pharma-monitor project
# Run from inside /a/nrf9160-pharma-monitor/
set -euo pipefail

say() { printf '\n\033[1;36m>>> %s\033[0m\n' "$1"; }
ok()  { printf '    \033[0;32mOK\033[0m  %s\n' "$1"; }

say "Fixing src/ structure"
mv src/test_logic.c tests/test_logic.c 2>/dev/null || true
ok "test_logic.c -> tests/"

# ============================================================
say "Creating src/record.h"
# ============================================================
cat > src/record.h << 'HEREDOC'
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
HEREDOC
ok "record.h"

# ============================================================
say "Creating src/app_config.h"
# ============================================================
cat > src/app_config.h << 'HEREDOC'
/*
 * app_config.h - compile-time configuration for the pharma tracker
 *
 * All timing in seconds. All temperatures in centi-Celsius (1/100 °C).
 * Change the active profile block to match your shipment type.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef APP_CONFIG_H__
#define APP_CONFIG_H__

/* ============================================================ */
/* Temperature profile — uncomment exactly ONE                   */
/* ============================================================ */

/* Refrigerated pharmaceuticals: vaccines, biologics, insulin    */
#define APP_PROFILE_ID       1
#define APP_PROFILE_NAME     "REFRIG_2_8"
#define APP_TEMP_LO_CC       200     /*  +2.00 °C in centi-Celsius */
#define APP_TEMP_HI_CC       800     /*  +8.00 °C in centi-Celsius */

/* Controlled room temperature
#define APP_PROFILE_ID       0
#define APP_PROFILE_NAME     "CRT_15_25"
#define APP_TEMP_LO_CC       1500
#define APP_TEMP_HI_CC       2500
*/

/* Frozen (-25 to -15 °C) - requires PT1000 probe overlay
#define APP_PROFILE_ID       2
#define APP_PROFILE_NAME     "FROZEN_M25_M15"
#define APP_TEMP_LO_CC       (-2500)
#define APP_TEMP_HI_CC       (-1500)
*/

/* Ultra-cold (-80 to -60 °C) - requires PT1000 probe overlay
#define APP_PROFILE_ID       3
#define APP_PROFILE_NAME     "ULTRACOLD_M80_M60"
#define APP_TEMP_LO_CC       (-8000)
#define APP_TEMP_HI_CC       (-6000)
*/

/* ============================================================ */
/* Timing                                                        */
/* ============================================================ */
#define APP_SAMPLE_PERIOD_S      300    /* sense every 5 min        */
#define APP_PUBLISH_PERIOD_S     3600   /* batch uplink every 1 h   */
#define APP_GNSS_PERIOD_S        21600  /* GNSS fix every 6 h       */

/* ============================================================ */
/* Storage / payload                                             */
/* ============================================================ */
#define APP_STORE_MAX_RECORDS    512    /* ring buffer hard cap      */
#define APP_PUBLISH_BATCH_MAX    10     /* records per MQTT message  */
#define APP_PAYLOAD_BUF_SIZE     1536   /* bytes, sized in README    */

/* ============================================================ */
/* Excursion detection                                           */
/* ============================================================ */
/* N consecutive out-of-band samples before alarm fires.
 * At 5-min cadence: 3 × 300 s = 15 min confirmation window.
 * Rejects door-opens; real excursions always take longer.      */
#define APP_EXC_CONFIRM_COUNT    3

/* ============================================================ */
/* Firmware identity                                             */
/* ============================================================ */
#define APP_FW_VERSION           "1.0.0"
#define APP_MQTT_TOPIC_FMT       "pharma/%s/telemetry"
#define APP_SHADOW_TOPIC_FMT     "$aws/things/%s/shadow/update"

/* ============================================================ */
/* Logging level (0=off 1=err 2=wrn 3=inf 4=dbg)               */
/* ============================================================ */
#ifndef CONFIG_PHARMA_LOG_LEVEL
#define CONFIG_PHARMA_LOG_LEVEL  3
#endif

#endif /* APP_CONFIG_H__ */
HEREDOC
ok "app_config.h"

# ============================================================
say "Creating src/ncs_compat.h"
# ============================================================
cat > src/ncs_compat.h << 'HEREDOC'
/*
 * ncs_compat.h - nRF Connect SDK version compatibility shims
 *
 * The ONLY APIs in this project that have changed signature across NCS
 * releases are collected here.  If a build fails with a signature error,
 * this is the single file to check.
 *
 * Verified drift points:
 *
 *  1. lte_lc_init()
 *       NCS < 2.6 : required before connecting.
 *       NCS >= 2.6: NO LONGER NEEDED (removed/no-op).
 *       lte_lc_init_and_connect_async() DEPRECATED - use
 *       lte_lc_connect_async() instead.
 *
 *  2. Release Assistance Indication socket option
 *       NCS < 2.6 : SO_RAI_NO_DATA / SO_RAI_LAST / SO_RAI_ONE_RESP
 *                   (each its own optname)
 *       NCS >= 2.6: single SO_RAI optname whose *value* selects behaviour.
 *       Getting this wrong silently costs ~60% of connected-mode energy.
 *
 *  3. aws_iot_init()
 *       NCS < 2.7 : aws_iot_init(const struct aws_iot_config *, handler)
 *       NCS >= 2.7: aws_iot_init(handler)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef NCS_COMPAT_H__
#define NCS_COMPAT_H__

#include <zephyr/kernel.h>

#if __has_include(<ncs_version.h>)
#  include <ncs_version.h>
#  define PT_NCS_AT_LEAST(maj, min) \
     ((NCS_VERSION_MAJOR > (maj)) || \
      (NCS_VERSION_MAJOR == (maj) && NCS_VERSION_MINOR >= (min)))
#else
   /* Assume modern SDK when ncs_version.h is absent */
#  define PT_NCS_AT_LEAST(maj, min) 1
#endif

/* ---- 1. LTE init ---- */
#include <modem/lte_lc.h>

static inline int pt_lte_init(void)
{
#if PT_NCS_AT_LEAST(2, 6)
    return 0; /* no-op: lte_lc_init() removed in NCS 2.6 */
#else
    return lte_lc_init();
#endif
}

/* ---- 2. Release Assistance Indication ---- */
#include <zephyr/net/socket.h>

static inline int pt_socket_rai(int sock, bool last)
{
#if defined(SO_RAI)
    /* NCS >= 2.6 consolidated option */
    int val = last ? RAI_LAST : RAI_ONE_RESP;
    return setsockopt(sock, SOL_SOCKET, SO_RAI, &val, sizeof(val));
#elif defined(SO_RAI_LAST)
    /* Legacy: option name encodes the behaviour */
    int opt = last ? SO_RAI_LAST : SO_RAI_ONE_RESP;
    return setsockopt(sock, SOL_SOCKET, opt, NULL, 0);
#else
    ARG_UNUSED(sock);
    ARG_UNUSED(last);
    return -ENOTSUP;
#endif
}

#endif /* NCS_COMPAT_H__ */
HEREDOC
ok "ncs_compat.h"

# ============================================================
say "Creating src/sensors.h"
# ============================================================
cat > src/sensors.h << 'HEREDOC'
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
HEREDOC
ok "sensors.h"

# ============================================================
say "Creating src/log_store.h"
# ============================================================
cat > src/log_store.h << 'HEREDOC'
/*
 * log_store.h - NVS flash ring buffer with backfill cursor
 *
 * Sampling never blocks on connectivity. The device records first and
 * transmits opportunistically. Oldest records are evicted when full
 * and the drop is counted so loss is never silent.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOG_STORE_H__
#define LOG_STORE_H__

#include <stddef.h>
#include "record.h"

/**
 * @brief Mount the NVS partition and recover state.
 * @return 0 on success, negative errno on failure.
 */
int log_store_init(void);

/**
 * @brief Append one record to the ring buffer.
 *
 * If the buffer is full the oldest unconsumed record is evicted and
 * REC_FLAG_DROPPED is set in the new record so the gap is visible
 * in the cloud audit trail.
 *
 * @param rec  Record to store.
 * @return 0 on success, negative errno on failure.
 */
int log_store_put(const struct sample_record *rec);

/**
 * @brief Peek at the next batch of unconsumed records.
 *
 * Does NOT advance the backfill cursor. Call log_store_consume() after
 * a confirmed cloud delivery (PUBACK) to advance.
 *
 * @param recs   Output array.
 * @param max    Maximum number of records to return.
 * @param count  Actual number placed in recs.
 * @return 0 on success, negative errno on failure.
 */
int log_store_get_batch(struct sample_record *recs, size_t max,
                        size_t *count);

/**
 * @brief Advance the backfill cursor by count records.
 *
 * Call only after confirmed PUBACK. Never call on failed delivery
 * or the records are permanently lost.
 *
 * @param count  Number of records to mark consumed.
 */
void log_store_consume(size_t count);

/**
 * @brief Return the number of unconsumed records waiting to be sent.
 * @return Count, or negative errno on failure.
 */
int log_store_pending(void);

#endif /* LOG_STORE_H__ */
HEREDOC
ok "log_store.h"

# ============================================================
say "Creating src/excursion.h"
# ============================================================
cat > src/excursion.h << 'HEREDOC'
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
HEREDOC
ok "excursion.h"

# ============================================================
say "Creating src/payload.h"
# ============================================================
cat > src/payload.h << 'HEREDOC'
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
HEREDOC
ok "payload.h"

# ============================================================
say "Creating src/gnss_ctrl.h"
# ============================================================
cat > src/gnss_ctrl.h << 'HEREDOC'
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
HEREDOC
ok "gnss_ctrl.h"

# ============================================================
say "Creating src/cloud_client.h"
# ============================================================
cat > src/cloud_client.h << 'HEREDOC'
/*
 * cloud_client.h - AWS IoT Core MQTT/TLS connection manager
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef CLOUD_CLIENT_H__
#define CLOUD_CLIENT_H__

#include <stddef.h>
#include <stdbool.h>

/**
 * @brief Initialise the cloud client. Call once after LTE is registered.
 * @return 0 on success, negative errno on failure.
 */
int cloud_client_init(void);

/**
 * @brief Establish the MQTT/TLS connection to AWS IoT Core.
 *
 * Blocks until connected or until an internal timeout.
 *
 * @return 0 on success, negative errno on failure.
 */
int cloud_client_connect(void);

/**
 * @brief Publish a payload to the telemetry topic.
 *
 * Delivers with QoS 1 (at-least-once). Waits for PUBACK.
 * After PUBACK, sets SO_RAI so the modem releases RRC immediately.
 *
 * @param payload  JSON string to publish.
 * @param len      Length of payload (bytes).
 * @return 0 on PUBACK, negative errno on failure.
 */
int cloud_client_publish(const char *payload, size_t len);

/**
 * @brief Publish an AWS Device Shadow update.
 *
 * @param payload  JSON shadow document.
 * @param len      Length of payload (bytes).
 * @return 0 on success, negative errno on failure.
 */
int cloud_client_publish_shadow(const char *payload, size_t len);

/**
 * @brief Return true if the MQTT connection is currently established.
 */
bool cloud_client_is_connected(void);

/**
 * @brief Gracefully disconnect from AWS IoT Core.
 */
void cloud_client_disconnect(void);

#endif /* CLOUD_CLIENT_H__ */
HEREDOC
ok "cloud_client.h"

# ============================================================
say "Creating CMakeLists.txt"
# ============================================================
cat > CMakeLists.txt << 'HEREDOC'
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(pharma_tracker
    VERSION 1.0.0
    DESCRIPTION "Cellular cold-chain monitor for pharmaceutical shipments"
    LANGUAGES C
)

target_sources(app PRIVATE
    src/main.c
    src/sensors.c
    src/gnss_ctrl.c
    src/log_store.c
    src/excursion.c
    src/payload.c
    src/cloud_client.c
)
HEREDOC
ok "CMakeLists.txt"

# ============================================================
say "Creating Kconfig"
# ============================================================
cat > Kconfig << 'HEREDOC'
mainmenu "IoT Pharma Monitor (nRF9160)"
source "Kconfig.zephyr"
HEREDOC
ok "Kconfig"

# ============================================================
say "Creating .gitignore"
# ============================================================
cat > .gitignore << 'HEREDOC'
# Build output
build/
twister-out*/
*.o
*.elf
*.hex
*.map
*.bin

# West workspace (not part of this repo)
.west/
zephyr/
modules/
bootloader/
nrf/

# Credentials - NEVER commit these
certs/
*.pem
*.key
*.crt
*private*

# OS / editor
.DS_Store
Thumbs.db
.vscode/
*.swp

# Python
__pycache__/
*.pyc
HEREDOC
ok ".gitignore"

# ============================================================
say "Creating .gitattributes"
# ============================================================
cat > .gitattributes << 'HEREDOC'
# Normalise line endings to LF in the repository.
# Git Bash on Windows will otherwise warn about CRLF conversion on every file.
* text=auto eol=lf
*.sh text eol=lf
*.py text eol=lf
HEREDOC
ok ".gitattributes"

# ============================================================
say "Creating LICENSE"
# ============================================================
cat > LICENSE << 'HEREDOC'
Apache License
Version 2.0, January 2004
http://www.apache.org/licenses/

Copyright 2026 Pavan Kulkarni

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
HEREDOC
ok "LICENSE"

# ============================================================
say "Creating boards/nrf9160dk_nrf9160_ns.overlay"
# ============================================================
cat > boards/nrf9160dk_nrf9160_ns.overlay << 'HEREDOC'
/*
 * Board overlay for the nRF9160 DK (PCA10090)
 *
 * Enables the BME280 environment sensor on the Arduino I2C bus.
 * The Arduino I2C bus on the nRF9160 DK maps to i2c2:
 *   SDA = P0.30  (Arduino header D14)
 *   SCL = P0.31  (Arduino header D15)
 *
 * Wiring:
 *   BME280 SDA  -> P0.30 (DK Arduino D14 / SDA)
 *   BME280 SCL  -> P0.31 (DK Arduino D15 / SCL)
 *   BME280 SDO  -> GND   (I2C address 0x76)
 *   BME280 CSB  -> VIN   (selects I2C mode)
 */
&arduino_i2c {
    status = "okay";
    clock-frequency = <I2C_BITRATE_STANDARD>;

    bme280: bme280@76 {
        compatible = "bosch,bme280";
        reg = <0x76>;
    };
};
HEREDOC
ok "boards/nrf9160dk_nrf9160_ns.overlay"

# ============================================================
say "Creating backend/README.md"
# ============================================================
cat > backend/README.md << 'HEREDOC'
# AWS Backend

Full setup instructions in the project RUNBOOK.md.

## Quick reference

| Resource         | Name                      |
|------------------|---------------------------|
| IoT Thing        | pharma-tracker-001        |
| MQTT topic       | pharma/+/telemetry        |
| DynamoDB state   | pharma_device_state       |
| DynamoDB audit   | pharma_audit_log          |
| Timestream DB    | pharma                    |
| Timestream table | telemetry                 |
| Lambda           | pharma_excursion_handler  |
| SNS topic        | pharma-alerts             |

## IoT Rule SQL

```sql
SELECT
  topic(2)       AS device_id,
  seq AS seq, ts AS ts,
  t   AS temp_c,  h  AS hum_pct,
  p   AS press_pa, lat AS lat, lon AS lon,
  b   AS batt_mv, pr AS profile, f AS flags
FROM 'pharma/+/telemetry'
```

## Test without hardware

```bash
aws iot-data publish \
  --topic 'pharma/pharma-tracker-001/telemetry' \
  --cli-binary-format raw-in-base64-out \
  --payload '{"seq":1,"ts":1735689600000,"t":12.5,"h":45.6,
              "p":101325,"lat":48.7758,"lon":9.1829,
              "b":3987,"pr":1,"f":1}'
```

`t=12.5` is above the 2-8 C window -> Lambda fires an SNS excursion alert.
HEREDOC
ok "backend/README.md"

# ============================================================
say "Creating docs/POWER_BUDGET.md"
# ============================================================
cat > docs/POWER_BUDGET.md << 'HEREDOC'
# Power Budget — nRF9160 Pharma Tracker

All figures derived from Nordic Semiconductor's published per-state data.
Replace with measured PPK2 values after hardware bring-up.

## Key energy costs

| Event          | Current    | Duration | Energy per event |
|----------------|-----------|----------|-----------------|
| PSM sleep      | 2.7 µA    | ongoing  | 2.7 µA floor    |
| Uplink + RAI   | ~26 mA    | ~2.5 s   | 0.018 mAh       |
| GNSS fix       | ~45 mA    | ~30 s    | 0.375 mAh       |

**Critical insight:** 1 GNSS fix = 20.8 uplinks.
GNSS is the dominant consumer. Motion-gate every fix.

## Battery life estimates (2700 mAh usable, 18650)

| Scenario                           | I_avg   | Life      |
|------------------------------------|---------|-----------|
| 15 min reports, GNSS every 6 h    | 137 µA  | 2.24 yr   |
| 15 min reports, GNSS every 2 h    | 262 µA  | 1.17 yr   |
| 60 s reports, GNSS every 15 min   | 2.62 mA | 43 days   |

## NVS buffer capacity

Record = 40 B + 8 B NVS overhead = 48 B/record.
32 kB partition / 4 kB sector = 8 sectors, 7 usable (1 for GC).
85 records/sector × 7 = **595 records**.

| Cadence | Offline capacity |
|---------|-----------------|
| 15 min  | 148 h (6.2 days) |
| 5 min   | 49.6 h           |
| 60 s    | 9.9 h            |

Target was 72 h offline. Met at 15-min cadence with 2× margin.
HEREDOC
ok "docs/POWER_BUDGET.md"

# ============================================================
say "Creating overlays/rtd_probe.conf"
# ============================================================
cat > overlays/rtd_probe.conf << 'HEREDOC'
# Extra Kconfig for PT1000 RTD probe via MAX31865
# Use when APP_PROFILE is FROZEN or ULTRACOLD
# Build: west build -- -DEXTRA_CONF_FILE=overlays/rtd_probe.conf \
#                      -DDTC_OVERLAY_FILE=overlays/rtd_probe.overlay
CONFIG_SPI=y
CONFIG_MAX31865=y
HEREDOC
ok "overlays/rtd_probe.conf"

# ============================================================
say "Creating overlays/rtd_probe.overlay"
# ============================================================
cat > overlays/rtd_probe.overlay << 'HEREDOC'
/* PT1000 RTD probe via MAX31865 SPI interface
 * Required for frozen / ultra-cold profiles (below -40 C).
 * BME280 and SHT4x both stop working below -40 C.
 *
 * PT1000 Callendar-Van Dusen (full 4-term, mandatory below 0 C):
 *   R(t) = R0*(1 + A*t + B*t^2 + C*(t-100)*t^3)  [t < 0]
 *   R0=1000, A=3.9083e-3, B=-5.775e-7, C=-4.183e-12
 *
 * At t=-70 C: R=723.345 ohm, sensitivity=4.00 ohm/C
 * MAX31865 15-bit, RREF=4300: LSB=0.131 ohm -> 0.033 C/LSB (3x margin)
 */
&arduino_spi {
    status = "okay";
    max31865: max31865@0 {
        compatible = "maxim,max31865";
        reg = <0>;
        spi-max-frequency = <1000000>;
        resistance-at-zero = <1000>;    /* PT1000: R0 = 1000 ohm */
        reference-resistance = <4300>;  /* RREF = 4300 ohm        */
        label = "MAX31865";
    };
};
HEREDOC
ok "overlays/rtd_probe.overlay"

# ============================================================
say "Creating tests/README.md"
# ============================================================
cat > tests/README.md << 'HEREDOC'
# Host-side logic tests

Compiles and runs the pure-logic modules on the host with minimal Zephyr
stubs. No hardware or SDK required.

Catches:
- Record layout regressions (sizeof != 40)
- JSON malformation (unbalanced braces)
- Buffer overflow handling (-ENOMEM, never truncate)
- Excursion state machine correctness
- MKT calculation vs reference implementation

## Run

```bash
cd tests
gcc -I. -I../src -DCONFIG_PHARMA_LOG_LEVEL=3 \
    test_logic.c ../src/payload.c ../src/excursion.c \
    -o test_logic -lm
./test_logic
```

Expected: 18/18 PASS

## What it found

The missing `CONFIG_FPU=y` / `CONFIG_NEWLIB_LIBC=y` in prj.conf was
discovered here: `exp()` and `log()` (used for MKT) appeared as undefined
symbols at link time, invisible to code review.
HEREDOC
ok "tests/README.md"

# ============================================================
say "Verifying structure"
# ============================================================
echo
echo "Project tree:"
find . -not -path './.git/*' -not -name '*.pyc' | sort | sed 's|^\./||' | sed 's|[^/]*/|  |g'

echo
echo "src/ headers:"
ls src/*.h

echo
echo "src/ sources:"
ls src/*.c | grep -v test_logic

echo
say "Done. Next: run git_push.sh to commit and push."
HEREDOC
ok "setup.sh complete"
