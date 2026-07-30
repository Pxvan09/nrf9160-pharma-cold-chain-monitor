#!/usr/bin/env python3
"""
IoT Pharma Monitoring Device - excursion detection Lambda

Triggered by an AWS IoT Core rule on every telemetry message. Responsible
for the parts of cold-chain compliance that deliberately do NOT live on
the device:

  * Mean Kinetic Temperature (MKT)
  * cumulative time-out-of-range
  * excursion alerting

WHY THESE LIVE IN THE CLOUD, NOT ON THE DEVICE
----------------------------------------------
A regulated audit calculation must be reproducible and revisable. If MKT
were computed on the device, changing the calculation (or correcting a
bug in it) would require a firmware update to a fleet you cannot reach,
and historical data could never be recomputed. Keeping the device as a
faithful recorder of raw samples and doing all derived math server-side
means the full history can be recomputed at any time from the immutable
record stream. The device's only job is to never lose a sample.

SPDX-License-Identifier: Apache-2.0
"""

import json
import math
import os
import time
from decimal import Decimal

import boto3

# ----------------------------------------------------------------------
# Configuration (set as Lambda environment variables)
# ----------------------------------------------------------------------
ALERT_TOPIC_ARN = os.environ["ALERT_TOPIC_ARN"]
STATE_TABLE = os.environ.get("STATE_TABLE", "pharma_device_state")
AUDIT_TABLE = os.environ.get("AUDIT_TABLE", "pharma_audit_log")

sns = boto3.client("sns")
ddb = boto3.resource("dynamodb")

# Temperature profiles. MUST stay numerically identical to
# app_config.h on the device, otherwise device-side and cloud-side
# excursion decisions can disagree - which is an audit finding.
#
#   id : (name, low_celsius, high_celsius)
PROFILES = {
    0: ("AMBIENT",   15.0,  25.0),
    1: ("CHILLED",    2.0,   8.0),
    2: ("FROZEN",   -25.0, -15.0),
    3: ("ULTRACOLD", -80.0, -60.0),
}

# Arrhenius constant for MKT, per USP <1160> / ICH Q1A.
# Ea = 83.144 kJ/mol is the conventional pharmaceutical value.
EA_J_PER_MOL = 83144.0
R_GAS = 8.314472  # J/(mol*K)


def mean_kinetic_temperature(temps_c):
    """
    Mean Kinetic Temperature per the Haynes equation.

        MKT = (Ea/R) / -ln( (1/n) * sum( exp(-Ea/(R*T_i)) ) )

    with T_i in kelvin. MKT is NOT the arithmetic mean: it weights higher
    temperatures far more heavily, because degradation kinetics are
    exponential in temperature. A shipment that sits at 2 C for 23 h and
    spikes to 25 C for 1 h has an MKT well above its arithmetic mean --
    which is exactly the point of the metric.

    Returns MKT in Celsius, or None if no usable samples.
    """
    kelvins = [t + 273.15 for t in temps_c if t is not None and t > -273.15]
    if not kelvins:
        return None

    acc = sum(math.exp(-EA_J_PER_MOL / (R_GAS * tk)) for tk in kelvins)
    if acc <= 0.0:
        return None

    mkt_k = (EA_J_PER_MOL / R_GAS) / (-math.log(acc / len(kelvins)))
    return mkt_k - 273.15


def handler(event, _context):
    """
    Expected event shape (produced by the IoT rule in README.md):

        {
          "device_id": "pharma-tracker-001",
          "seq": 1234,
          "ts": 1735689600000,
          "temp_c": 5.23,
          "hum_pct": 45.6,
          "press_pa": 101325,
          "lat": 48.7758210,
          "lon": 9.1829340,
          "batt_mv": 3987,
          "profile": 1,
          "flags": 1
        }
    """
    device_id = event["device_id"]
    seq = int(event["seq"])
    temp_c = event.get("temp_c")
    profile_id = int(event.get("profile", 1))

    name, lo, hi = PROFILES.get(profile_id, PROFILES[1])

    # ---- 1. Immutable audit record -----------------------------------
    # Written first, unconditionally, before any judgement is made about
    # it. The audit trail must contain the sample even if downstream
    # processing later fails.
    audit = ddb.Table(AUDIT_TABLE)
    audit.put_item(
        Item={
            "device_id": device_id,
            "seq": seq,
            "ts": int(event["ts"]),
            "temp_c": Decimal(str(temp_c)) if temp_c is not None else None,
            "profile": name,
            "ingested_at": int(time.time() * 1000),
        },
        # Never overwrite an existing (device_id, seq). A replayed or
        # duplicated backfill message must not mutate history.
        ConditionExpression="attribute_not_exists(device_id) "
                            "AND attribute_not_exists(seq)",
    )

    # ---- 2. Gap detection --------------------------------------------
    # The device emits a strictly monotonic seq. Any jump means samples
    # were lost -- a data-integrity event that must be recorded, not
    # silently tolerated.
    state = ddb.Table(STATE_TABLE)
    prev = state.get_item(Key={"device_id": device_id}).get("Item", {})
    prev_seq = int(prev.get("last_seq", seq - 1))

    gap = seq - prev_seq - 1
    if gap > 0:
        sns.publish(
            TopicArn=ALERT_TOPIC_ARN,
            Subject=f"DATA GAP: {device_id}",
            Message=json.dumps({
                "device": device_id,
                "missing_samples": gap,
                "between_seq": [prev_seq, seq],
            }),
        )

    # ---- 3. Excursion evaluation -------------------------------------
    excursion = False
    if temp_c is not None:
        excursion = temp_c < lo or temp_c > hi

    time_oor_s = int(prev.get("time_oor_s", 0))
    if excursion:
        # Accumulate out-of-range dwell time using the actual inter-sample
        # interval, not an assumed cadence -- the device switches cadence
        # adaptively, so assuming a fixed period would corrupt the total.
        prev_ts = int(prev.get("last_ts", event["ts"]))
        time_oor_s += max(0, (int(event["ts"]) - prev_ts) // 1000)

        sns.publish(
            TopicArn=ALERT_TOPIC_ARN,
            Subject=f"COLD-CHAIN EXCURSION: {device_id}",
            Message=json.dumps({
                "device": device_id,
                "profile": name,
                "window_c": [lo, hi],
                "measured_c": temp_c,
                "seq": seq,
                "ts": event["ts"],
                "lat": event.get("lat"),
                "lon": event.get("lon"),
                "cumulative_time_out_of_range_s": time_oor_s,
            }),
        )

    # ---- 4. Update current state -------------------------------------
    state.put_item(Item={
        "device_id": device_id,
        "last_seq": seq,
        "last_ts": int(event["ts"]),
        "last_temp_c": Decimal(str(temp_c)) if temp_c is not None else None,
        "profile": name,
        "in_excursion": excursion,
        "time_oor_s": time_oor_s,
        "batt_mv": int(event.get("batt_mv", 0)),
        "lat": Decimal(str(event["lat"])) if event.get("lat") else None,
        "lon": Decimal(str(event["lon"])) if event.get("lon") else None,
    })

    return {"ok": True, "excursion": excursion, "gap": gap}
