# RUNBOOK — IoT Pharma Monitoring Device

How to build, provision, run and verify the device. Follow the phases in order:
each one eliminates a class of failure, so if a later phase misbehaves you
already know the earlier layers are sound.

---

## 1. System flowchart

### 1.1 End-to-end data path

```
┌──────────────────────────── DEVICE (nRF9160) ────────────────────────────┐
│                                                                          │
│   BME280 ──I²C(P0.30/P0.31)──┐                                           │
│   (T / P / RH)               │                                           │
│                              ▼                                           │
│   PT1000+MAX31865 ──SPI──► sensors.c ──► struct sample_record (40 B)     │
│   (optional, < -40 °C)        │                                          │
│                               ▼                                          │
│                        excursion.c  ── band check, debounce, MKT         │
│                               │                                          │
│                               ▼                                          │
│   GNSS (in-SiP) ──► gnss_ctrl.c ──► log_store.c  (NVS ring, 595 recs)   │
│                                          │                               │
│                                          ▼                               │
│                                     payload.c  (integer JSON)            │
│                                          │                               │
│                                          ▼                               │
│                                   cloud_client.c ──TLS 1.2 / MQTT──┐     │
└────────────────────────────────────────────────────────────────────┼─────┘
                                                                     ▼
                                                          LTE-M / NB-IoT
                                                                     │
                                                                     ▼
                                                          ┌──────────────────┐
                                                          │  AWS IoT Core    │
                                                          │  pharma/+/telem  │
                                                          └────────┬─────────┘
                                                                   │ Rule
                                              ┌────────────────────┼──────────────┐
                                              ▼                    ▼              ▼
                                        Lambda              Timestream        (S3 archive)
                                     (MKT, gaps,           (time-series)
                                      excursions)                │
                                     ┌──────┴──────┐             ▼
                                     ▼             ▼          Grafana
                                DynamoDB          SNS        (dashboards,
                              (state+audit)     (alerts)      geomap)
```

### 1.2 Firmware main loop

```
                          ┌──────────┐
                          │   BOOT   │
                          └────┬─────┘
                               ▼
                   ┌───────────────────────┐
                   │ nrf_modem_lib_init()  │
                   │ log_store_init() NVS  │
                   │ sensors_init()        │
                   │ excursion_init()      │
                   └───────────┬───────────┘
                               ▼
                   ┌───────────────────────┐
                   │ lte_lc_connect_async()│◄──── retry w/ backoff
                   └───────────┬───────────┘
                               ▼
                        ┌──────────────┐
                        │  REGISTERED? │──no──► keep buffering to NVS
                        └──────┬───────┘         (device never blocks
                               │ yes              on connectivity)
                               ▼
              ╔════════════════════════════════════╗
              ║        MAIN CYCLE (repeat)         ║
              ╚════════════════════════════════════╝
                               │
          ┌────────────────────┼────────────────────┐
          ▼                    ▼                    ▼
  ┌───────────────┐   ┌────────────────┐   ┌────────────────┐
  │ SAMPLE TIMER  │   │ PUBLISH TIMER  │   │  GNSS TIMER    │
  │  every 300 s  │   │  every 3600 s  │   │  every 21600 s │
  └───────┬───────┘   └────────┬───────┘   └────────┬───────┘
          ▼                    ▼                    ▼
   read sensors         pull batch (≤10)      single fix
          │             from NVS ring         (motion-gated)
          ▼                    │                    │
   excursion_update()          ▼                    ▼
          │             payload_build_        cache lat/lon
          ▼             telemetry()           into next record
   ┌──────────────┐            │
   │ alarm edge?  │            ▼
   └──────┬───────┘     MQTT publish QoS1
          │ yes                │
          ▼                    ▼
   PUBLISH IMMEDIATELY   ┌───────────┐
   (bypass timer)        │  PUBACK?  │
          │              └─────┬─────┘
          │                yes │  no
          ▼                    ▼   └──► keep records, retry
   append to NVS ring    mark records
          │              consumed, free slots
          ▼                    │
   ┌──────────────┐            ▼
   │ ring full?   │      SO_RAI(LAST) ──► modem releases RRC
   └──────┬───────┘            │          immediately
          │ yes                ▼
          ▼               enter PSM (2.7 µA)
   drop OLDEST,                │
   increment `dropped`         └──────► back to MAIN CYCLE
   (counter is reported,
    so loss is never silent)
```

**Design invariant:** sampling and storage never depend on connectivity.
The device records first and transmits opportunistically. A shipment that
spends 40 hours out of coverage still produces a complete audit trail.

---

## 2. Phase 0 — Prerequisites

| Item | Requirement |
|---|---|
| nRF Connect SDK | **v2.7 – v3.x** (see §6 if you are on ≤2.6) |
| Toolchain | Installed via nRF Connect for Desktop → Toolchain Manager |
| Hardware | nRF9160 DK (PCA10090) |
| SIM | NB-IoT **or** LTE-M enabled, activated, correct APN |
| Sensor | BME280 breakout, I²C |
| AWS | Account + region chosen |

---

## 3. Phase 1 — Prove the radio works (before any of this firmware)

**Do not skip this.** If the modem cannot register, no application firmware
can help, and you will waste days debugging the wrong layer.

```
Flash the stock NCS sample:  nrf/samples/cellular/at_client
Open nRF Connect → Serial Terminal @115200
```

```
AT+CFUN=1
AT%XSYSTEMMODE?
AT+CEREG=2
AT+CEREG?        →  MUST show +CEREG: 2,1  (home) or 2,5 (roaming)
AT+CESQ          →  check signal quality
AT%XMONITOR      →  operator, band, RSRP
```

| Symptom | Cause |
|---|---|
| `+CEREG: 2,0` forever | No SIM / not activated / no NB-IoT coverage |
| `+CEREG: 2,2` cycling | Searching — wrong band or weak signal; try LTE-M |
| `+CME ERROR` | Modem firmware too old — update via Programmer |

**Gate: do not proceed until you see `2,1` or `2,5`.**

---

## 4. Phase 2 — Provision AWS credentials into the modem

TLS runs *inside* the modem, so credentials live in the modem keystore under
a **security tag**, never in your application binary.

1. AWS IoT Core → **Manage → Things → Create** → name it `pharma-tracker-001`
2. Auto-generate certificates. Download **device cert**, **private key**,
   **Amazon Root CA 1**
3. Create + attach the IoT policy (see `backend/README.md` §5)
4. Copy the **endpoint**: IoT Core → Settings → *Device data endpoint*
5. With `at_client` still flashed: nRF Connect → **Cellular Monitor →
   Certificate Manager**
   - CA certificate ← Amazon Root CA 1
   - Client certificate ← device cert
   - Private key ← private key
   - Security tag: **42**
   - Click **Update certificates**

> Provision with the modem deactivated (`AT+CFUN=4`). Writing credentials
> while the modem is registered can fail silently.

---

## 5. Phase 3 — Configure and build

Edit `prj.conf`:

```ini
CONFIG_AWS_IOT_BROKER_HOST_NAME="xxxxx-ats.iot.eu-central-1.amazonaws.com"
CONFIG_AWS_IOT_CLIENT_ID_STATIC="pharma-tracker-001"
CONFIG_AWS_IOT_SEC_TAG=42
```

Select the temperature profile in `src/app_config.h` (exactly one):

```c
#define APP_PROFILE_REFRIG    1   /* 2 … 8 °C     — vaccines, biologics */
/* #define APP_PROFILE_CRT    1      15 … 25 °C   — controlled room temp */
/* #define APP_PROFILE_FROZEN 1     -25 … -15 °C  — needs PT1000 probe   */
/* #define APP_PROFILE_ULTRA  1     -80 … -60 °C  — needs PT1000 probe   */
```

Build and flash:

```bash
west build -p always -b nrf9160dk/nrf9160/ns .
west flash
```

**Verify the flash map before trusting the buffer numbers:**

```bash
grep STORAGE_PARTITION build/pm.config     # expect SIZE=0x8000 (32 kB)
```

---

## 6. Phase 4 — Wiring

| BME280 | nRF9160 DK | Note |
|---|---|---|
| VIN | 3V3 | |
| GND | GND | |
| SDA | **P0.30** (Arduino D14 / SDA) | `i2c2` |
| SCL | **P0.31** (Arduino D15 / SCL) | `i2c2` |
| SDO | GND | → address **0x76** (VDD → 0x77) |
| CSB | VIN | ties the part into I²C mode |

If your breakout has no CSB pin it is I²C-only; ignore that row.

---

## 7. Phase 5 — Verify operation

Expected boot log:

```
*** Booting nRF Connect SDK ***
[00:00:00] <inf> pharma: IoT Pharma Monitor v1.0.0  profile REFRIG_2_8 [200..800 cC]
[00:00:00] <inf> log_store: NVS mounted, 8 sectors, capacity 595 records
[00:00:00] <inf> sensors: BME280 ready
[00:00:02] <inf> pharma: LTE searching...
[00:00:21] <inf> pharma: LTE registered (roaming)
[00:00:23] <inf> cloud: connected to AWS IoT
[00:05:00] <inf> pharma: sample seq=1 amb=2340 cC  hum=4512  p=101325
[01:00:00] <inf> cloud: published 10 records, PUBACK ok
```

Then, in AWS IoT Core → **MQTT test client**, subscribe to `pharma/#`.

**Forced-excursion test:** hold the BME280 in your hand. It rises above 8 °C,
and after the debounce window the device publishes immediately (out of band)
and SNS sends an alert.

---

## 8. Troubleshooting

| Symptom | Diagnosis |
|---|---|
| `undefined reference to 'exp'` at link | `CONFIG_FPU=y` / `CONFIG_NEWLIB_LIBC=y` missing (already patched in `prj.conf`) |
| `nvs_mount failed (-2)` | `storage_partition` unresolved — check `pm_static.yml` is in the app root |
| `BME280 not ready` | Wrong I²C address (0x76 vs 0x77), CSB floating, or wrong bus |
| Registers, but AWS never connects | sec_tag mismatch, wrong endpoint, or policy not attached to the cert |
| `-ENOMEM` from payload | Lower `APP_PUBLISH_BATCH_MAX` |
| Fix never obtained | GNSS needs open sky; first cold fix can take 5–15 min |
| High current in PSM | Disable `CONFIG_LOG` / UART for production measurement |

---

## 9. NCS version note

`cloud_client.c` calls `aws_iot_init(handler)` — the **NCS ≥ 2.7** signature.
On **NCS ≤ 2.6** it takes two arguments:

```c
/* NCS <= 2.6 */
err = aws_iot_init(NULL, aws_evt_handler);
```

`src/ncs_compat.h` documents this and the two other verified drift points
(`lte_lc_init()`, `SO_RAI`).
