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
