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
