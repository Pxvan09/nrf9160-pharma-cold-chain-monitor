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
