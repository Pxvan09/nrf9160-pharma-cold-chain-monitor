# IoT Pharma Monitoring Device — nRF9160 firmware

Cold-chain monitor for medical payloads (organ and blood samples, vaccines,
biologics, instruments) shipped worldwide under extreme conditions.

Measures temperature, pressure, humidity and location; buffers every sample to
flash so coverage gaps cannot put holes in the record; forwards to AWS IoT Core
over LTE-M / NB-IoT with TLS; raises immediate excursion alarms; and computes
Mean Kinetic Temperature for the end-of-shipment quality report.

---

## 1. READ THIS FIRST — two things that will bite you

### 1.1 Your BME280 cannot measure ultra-cold

The BME280's operating range is **−40 … +85 °C**. Checked against real
cold-chain windows:

| Payload window | BME280 | PT1000 + MAX31865 |
|---|---|---|
| Controlled room temp 15…25 °C | OK | OK |
| Refrigerated 2…8 °C | OK | OK |
| Frozen −25…−15 °C | OK | OK |
| **Ultra-cold −80…−60 °C** | **FAIL** | OK |
| **Dry ice −78.5 °C** | **FAIL** | OK |
| **Cryogenic (LN2 vapour)** | **FAIL** | OK |

If your shipments include frozen/ultra-cold legs, the BME280 must **not** be
the compliance sensor. Fit the PT1000 RTD probe
(`overlays/rtd_probe.overlay`) and set `APP_COMPLIANCE_SRC_PROBE 1`.
`sensors_init()` refuses to run if the selected profile is outside the fitted
sensor's range — deliberately, because silently under-reporting an ultra-cold
excursion is the worst failure this product can have.

### 1.2 "Continuous real-time tracking" and "long battery life" are mutually exclusive

Measured-parameter power budget (full derivation in `docs/POWER_BUDGET.md`):

| Profile | Sample / Publish / GNSS | Average current | Life on 2700 mAh |
|---|---|---|---|
| **A — compliance logger** | 5 min / 60 min / 6 h | **44 µA** | **~7 years** |
| **B — near-real-time** | 5 min / 15 min / 15 min | 631 µA | ~6 months |
| **C — continuous GNSS** | 1 min / 1 min / always on | 51 mA | **~2.2 days** |

**GNSS dominates, not cellular.** In profile A, GNSS is 52 % of the entire
budget while the cellular uplink is 41 % and the PSM floor only 6 %. Continuous
GNSS tracking alone is ~50 mA and flattens an 18650 in two days.

The default build is profile A. Change `APP_SAMPLE_PERIOD_S`,
`APP_PUBLISH_PERIOD_S`, `APP_GNSS_PERIOD_S` in `src/app_config.h` — and re-run
the budget before promising a battery life to a customer.

---

## 2. SDK version matrix — check this before your first build

This firmware targets **nRF Connect SDK v2.7.0 or newer** (verified against the
v2.8 API set; also correct for v2.9.x and v3.x).

The `aws_iot` library was re-based onto the MQTT-helper library in v2.7.0. If
you are on v2.6 or older, these differ and the build fails — or worse, TLS
silently fails at runtime:

| Item | NCS ≤ 2.6 | **NCS ≥ 2.7 (this code)** |
|---|---|---|
| Security tag Kconfig | `CONFIG_AWS_IOT_SEC_TAG` | `CONFIG_MQTT_HELPER_SEC_TAG` |
| Init signature | `aws_iot_init(&cfg, cb)` | `aws_iot_init(cb)` |
| "Ready" event | `AWS_IOT_EVT_READY` | `AWS_IOT_EVT_CONNECTED` |
| Extra subscriptions | `aws_iot_subscription_topics_add()` | `aws_iot_application_topics_set()` |
| MQTT keepalive | app calls `aws_iot_ping()` | helper thread handles it |
| TX buffer Kconfig | `CONFIG_AWS_IOT_MQTT_RX_TX_BUFFER_LEN` | `CONFIG_MQTT_HELPER_RX_TX_BUFFER_SIZE` |
| Board target | `nrf9160dk_nrf9160_ns` | `nrf9160dk/nrf9160/ns` |

Also version-sensitive:

- `CONFIG_LTE_AUTO_INIT_AND_CONNECT` was **removed** in v2.6 — do not add it.
- `lte_lc_init_and_connect_async()` is deprecated; this code uses
  `lte_lc_connect_async()`.
- `lte_lc_init()` is deprecated — "no need to call this function anymore".
- **NCS 3.x only:** `lte_lc` was modularised. If PSM/eDRX features are missing
  at runtime, add to `prj.conf`:
  ```ini
  CONFIG_LTE_LC_PSM_MODULE=y
  CONFIG_LTE_LC_EDRX_MODULE=y
  CONFIG_LTE_LC_RAI_MODULE=y
  ```
- GNSS assistance event was renamed `NRF_MODEM_GNSS_EVT_AGPS_REQ` →
  `NRF_MODEM_GNSS_EVT_AGNSS_REQ`. This code does not use it, so either is fine.

**If something does not compile, the fix is almost always a Kconfig rename.**
Use the nRF Connect VS Code extension's Kconfig GUI to search the symbol, and
check the installed header directly:
`<ncs>/nrf/include/net/aws_iot.h`, `<ncs>/nrf/include/modem/lte_lc.h`.

---

## 3. Hardware you need

Everything below assumes you already have the DK and sensors.

| Item | Notes |
|---|---|
| nRF9160 DK (PCA10090) | Any revision; the code reads its capacity at runtime |
| IoT SIM | Must have **NB-IoT and/or LTE-M** at your test location |
| Bosch BME280 breakout | I²C, address 0x76 (SDO→GND) |
| PT1000 + MAX31865 | **Only if** frozen/ultra-cold profiles are needed |
| Micro-USB cable | Program, power, UART |
| Nordic PPK2 | For the power validation step — strongly recommended |

### Wiring — BME280 (verified against upstream Zephyr board devicetree)

The Arduino I²C bus on the nRF9160 DK is **`i2c2`**:

| BME280 | nRF9160 DK |
|---|---|
| VIN | 3V3 (see voltage note in the overlay) |
| GND | GND |
| SDA | **P0.30** — Arduino header D14 / "SDA" |
| SCL | **P0.31** — Arduino header D15 / "SCL" |
| SDO | GND → address 0x76 |
| CSB | VCC → forces I²C mode; do not leave floating |

> Do **not** switch the DK's VDD to 3 V just to suit the sensor. Nordic
> documents that 3 V GPIO under load degrades LTE RF performance. Keep VDD at
> 1.8 V and power the breakout from the DK's 3V3 pin if it level-shifts.

---

## 4. Build, provision, run

### Step 0 — prove the link before touching this code

Do not skip this. Flash Nordic's `at_client` sample and register by hand:

```
AT+CFUN=1
AT%XSYSTEMMODE?
AT+CEREG=2
AT+CEREG?        <- expect +CEREG: 2,1  or  2,5   (1=home, 5=roaming)
AT+CESQ          <- signal quality
AT%XMONITOR      <- band, operator, RSRP
```

If you cannot register here, every later failure is your SIM/carrier/coverage,
not the firmware. This single step saves days.

### Step 1 — AWS IoT setup

```bash
# endpoint -> CONFIG_AWS_IOT_BROKER_HOST_NAME
aws iot describe-endpoint --endpoint-type iot:Data-ATS

# thing -> CONFIG_AWS_IOT_CLIENT_ID_STATIC
aws iot create-thing --thing-name pharma-tracker-001

# certificate
aws iot create-keys-and-certificate --set-as-active \
  --certificate-pem-outfile device_cert.pem \
  --public-key-outfile pub_key.pem \
  --private-key-outfile priv_key.pem \
  --query certificateArn

# policy (development only - tighten before production)
cat > policy.json <<'EOF'
{ "Version": "2012-10-17",
  "Statement": [{ "Effect": "Allow", "Action": "iot:*", "Resource": "*" }] }
EOF
aws iot create-policy --policy-name pharma-dev --policy-document file://policy.json
aws iot attach-policy --target <cert-arn> --policy-name pharma-dev
aws iot attach-thing-principal --principal <cert-arn> --thing-name pharma-tracker-001
```

### Step 2 — provision certificates to the modem

TLS runs **inside the modem**, so credentials live in the modem keystore under
a security tag, never in your binary.

```bash
pip3 install -r <ncs>/nrf/scripts/requirements-extra.txt

nrfcredstore <serial-port> list
nrfcredstore <serial-port> write 42 CLIENT_CERT   device_cert.pem
nrfcredstore <serial-port> write 42 CLIENT_KEY    priv_key.pem
nrfcredstore <serial-port> write 42 ROOT_CA_CERT  AmazonRootCA1.pem
```

`42` must equal `CONFIG_MQTT_HELPER_SEC_TAG` in `prj.conf`.

> Best practice: use `nrfcredstore generate` to create the key **on the modem**
> and sign a CSR, so the private key never leaves the device. The command above
> is the simpler path for bring-up.

### Step 3 — configure

Edit `prj.conf`:

```ini
CONFIG_AWS_IOT_BROKER_HOST_NAME="xxxxx-ats.iot.eu-central-1.amazonaws.com"
CONFIG_AWS_IOT_CLIENT_ID_STATIC="pharma-tracker-001"
CONFIG_MQTT_HELPER_SEC_TAG=42
```

Edit `src/app_config.h` — pick your cold-chain profile and cadence.

### Step 4 — build and flash

```bash
west build -p always -b nrf9160dk/nrf9160/ns .
west flash
```

Ultra-cold build with the RTD probe:

```bash
west build -p always -b nrf9160dk/nrf9160/ns . -- \
  -DEXTRA_DTC_OVERLAY_FILE=overlays/rtd_probe.overlay \
  -DEXTRA_CONF_FILE=overlays/rtd_probe.conf
```

### Step 5 — watch it work

Serial @115200. Expected boot sequence:

```
IoT Pharma Monitoring Device  v1.0.0
profile REFRIG_2_8  sample 300s  publish 3600s  gnss 21600s
NVS: 8 sectors x 4096 B, capacity 651 records (54 h @ 300 s)
ambient sensor 'bme280@76' ready
profile REFRIG_2_8: window 2.00 C .. 8.00 C, debounce 2 samples
GNSS ready (single fix, 60 s retry)
thing 'pharma-tracker-001'
  telemetry -> pharma/pharma-tracker-001/telemetry
attaching to LTE (this can take minutes on first boot)
LTE registered (roaming)
PSM granted: TAU 3600 s, active 0 s
network time obtained
connected to AWS IoT
seq 1: 5.24 C, 41.30 %RH, 101325 Pa, 4180 mV
published 298 bytes to pharma/... (confirmed)
```

In the AWS console → IoT Core → MQTT test client, subscribe to `pharma/#`.

---

## 5. Architecture

```
                      ┌──────────── nRF9160 ────────────┐
 BME280 ──I²C(i2c2)──►│                                  │
 PT1000 ──SPI(spi3)──►│  sensors.c ──► excursion.c ──►   │
                      │                 (MKT, alarms)    │
 GNSS (in-SiP) ──────►│  gnss_ctrl.c                     │
                      │        │                         │
                      │        ▼                         │
                      │  log_store.c  (NVS flash ring)   │  ← every sample
                      │        │        store-and-forward│    persisted first
                      │        ▼                         │
                      │  payload.c (integer JSON)        │
                      │        │                         │
                      │        ▼                         │
                      │  cloud_client.c ──TLS/MQTT──────►│──► AWS IoT Core
                      └──────────────────────────────────┘
```

### Design decisions worth knowing

**Single-threaded scheduler.** `main.c` drives everything from monotonic uptime
deadlines. No races between sampling, GNSS and publishing; reproducible
ordering; trivial to audit in a design review. Async work (modem, MQTT) only
sets flags or gives semaphores.

**Store-before-send, release-on-PUBACK.** Samples are committed to flash before
any transmit attempt and released only after the broker's QoS-1 PUBACK. A
publish that was merely "sent" is not evidence of delivery and must not clear
the audit buffer. Validated by simulation across 8 scenarios including reboot
recovery and ring overflow.

**Integer-only payloads, no floats, no heap on the data path.** Every quantity
is a scaled integer (centi-°C, centi-%, Pa, 1e-7°, mV). Deterministic and
exactly reproducible for audit, no float formatter in the image, and no
soft-double on a single-precision FPU. Measured: 10 records = 1316 B in a
1536 B buffer, 219 B headroom.

**Invalid readings are omitted, never faked.** A failed sensor produces an
absent JSON field, so the cloud can distinguish "sensor failed" from a genuine
0.00 °C.

**Timestamps backfilled.** A shipment starting in a warehouse with no coverage
still logs from minute one, using uptime and a `TIME_VALID=0` flag. When
network time arrives, buffered records are corrected in place.

**Excursions latch; MKT uses `double`.** An excursion that happened cannot be
undone by drifting back into range. MKT terms are ~2.4e-16 at 5 °C — `float`
would underflow, so the accumulator is `double`.

---

## 6. Validation checklist

Work through in order. Do not skip 4.

| # | Test | Pass criterion |
|---|---|---|
| 1 | Bench boot | Capacity, sensor and profile lines appear; no errors |
| 2 | Sensor accuracy | BME280 within ±1 °C of a reference thermometer |
| 3 | Cloud round-trip | Message visible in MQTT test client; `"confirmed"` in log |
| 4 | **Power** | PPK2 on the isolated SiP supply; see below |
| 5 | Offline buffering | Pull antenna 30 min → reconnect → **zero gaps**, `drop=0` |
| 6 | Reboot recovery | Power-cycle mid-buffer → `next_seq` recovers, no duplicates |
| 7 | Excursion | Warm probe out of window → alarm after 2 samples, SNS fires |
| 8 | Cold soak | Fridge 2–8 °C then freezer: no modem brownout on TX |
| 9 | Soak | 72 h run: no leaks, no reconnect storms, `dropped == 0` |

**Test 4 in detail** — this is the one everyone gets wrong:

1. Put the DK in **nRF ONLY / IF MCU DISCONNECT** mode (SW1 on v0.8.x,
   **SW6** on v1.1.0).
2. Power from the **external supply** (P28 / **P21**), **not USB** — USB rail
   noise corrupts the measurement.
3. Break the SiP supply path at **P24** (v0.8.x) / **P22** (v1.1.0) and insert
   the PPK2 in series.
4. Build with the production power block in `prj.conf` uncommented
   (`CONFIG_LOG=n`, `CONFIG_SERIAL=n`). **An enabled UART console alone costs
   far more than the 2.7 µA PSM floor** and will invalidate the budget.
5. Capture a full PSM → wake → sample → TX → PSM cycle.

Expect: ~2.7 µA floor, ~26 mA for ~2.5 s during publish, ~50 mA during a GNSS
fix. If the floor is tens of µA, the network refused PSM — check the
`PSM granted:` log line.

---

## 7. Known gaps / hardening backlog

Honest list of what is **not** in this version:

- **Watchdog.** No `CONFIG_WATCHDOG` task supervision yet. Add before field
  deployment — a wedged modem thread currently requires a power cycle.
- **FOTA.** `CONFIG_AWS_FOTA` is not enabled. Add with AWS IoT Jobs once the
  base is stable; you cannot physically reach a shipped device.
- **Downlink commands.** `cloud_cmd_received()` logs only. Any remote change to
  the alarm window on a regulated device must be authenticated and audited —
  deliberately left as an explicit integration step, not a silent remote-write.
- **RAI.** The nRF9160 is 3GPP Rel-13; AS-RAI needs Rel-14 (nRF9151/9161).
  Only PSM is used here. On an nRF9151 add `CONFIG_LTE_LC_RAI_MODULE=y` and
  expect ~60 % less connected-mode energy.
- **A-GPS / P-GPS.** Not wired up. The budget assumes assisted 10 s fixes;
  **unassisted cold fixes are ~4.5× more expensive** (125 µA vs 44 µA average).
  Add nRF Cloud A-GNSS or P-GPS before trusting the battery numbers.
- **Ring capacity.** Default `storage_partition` gives ~651 records ≈ 54 h of
  offline buffering at 5-min sampling. For long ocean legs, enlarge the
  partition or move the ring to the DK's external 8 MB QSPI flash.
- **Sensor calibration.** No calibration/traceability record. EN 12830 requires
  it for a compliance instrument.

---

## 8. File map

```
pharma_tracker/
├── CMakeLists.txt
├── Kconfig                          app log-level symbol
├── prj.conf                         ← EDIT: endpoint, thing name, sec tag
├── boards/
│   └── nrf9160dk_nrf9160_ns.overlay BME280 on i2c2 (P0.30/P0.31)
├── overlays/
│   ├── rtd_probe.overlay            PT1000 + MAX31865 for ultra-cold
│   └── rtd_probe.conf
├── docs/
│   └── POWER_BUDGET.md              full derivation of every number above
└── src/
    ├── app_config.h                 ← EDIT: profile, cadence, thresholds
    ├── record.h                     40-byte on-flash record (BUILD_ASSERT)
    ├── log_store.{h,c}              NVS ring, store-and-forward
    ├── sensors.{h,c}                BME280 + PT1000 + battery via %XVBAT
    ├── gnss_ctrl.{h,c}              single-fix GNSS, always stopped after
    ├── excursion.{h,c}              debounce, latch, MKT
    ├── payload.{h,c}                integer-only JSON, bounded buffer
    ├── cloud_client.{h,c}           AWS IoT, PUBACK-confirmed publish
    └── main.c                       single-threaded scheduler
```
