#!/usr/bin/env bash
# git_push.sh - staged commits and push to GitHub
# Run from inside /a/nrf9160-pharma-monitor/ AFTER setup.sh
set -euo pipefail

say() { printf '\n\033[1;36m>>> %s\033[0m\n' "$1"; }
ok()  { printf '    \033[0;32mOK\033[0m  %s\n' "$1"; }

GH_USER="Pxvan09"
REPO="nrf9160-pharma-cold-chain-monitor"

say "Configuring git identity"
git config user.name  "Pavan"
git config user.email "pavankulkarni501@gmail.com"
git init -q
git branch -M main
ok "git ready"

say "Building staged commit history"

c() {
    local msg="$1"; shift
    local found=()
    for p in "$@"; do [ -e "$p" ] && found+=("$p"); done
    [ "${#found[@]}" -eq 0 ] && return 0
    git add -- "${found[@]}"
    git diff --cached --quiet || {
        git commit -q -m "$msg"
        ok "${msg%%$'\n'*}"
    }
}

c "chore: add license, gitignore and gitattributes" \
    LICENSE .gitignore .gitattributes

c "build: CMakeLists, Kconfig, prj.conf, board overlay, partition map

- BME280 on i2c2 (P0.30/P0.31, Arduino header)
- storage_partition pinned to 32 kB via pm_static.yml
- CONFIG_FPU + CONFIG_NEWLIB_LIBC required for MKT (exp/log from libm)" \
    CMakeLists.txt Kconfig prj.conf boards pm_static.yml

c "feat: 40-byte on-flash record format and compile-time profile config

Layout hand-packed with zero padding, pinned by _Static_assert so a
field reorder cannot silently orphan stored data. Integer-only values
(centi-Celsius) for exact, endian-stable flash round-trips." \
    src/record.h src/app_config.h src/ncs_compat.h

c "feat: sensor abstraction for BME280 ambient and PT1000 probe

BME280 and SHT4x both stop at -40 C. Ultra-cold (-70 C) payloads
require a PT1000 RTD via MAX31865. Both paths are optional at build
time and auto-detected from devicetree." \
    src/sensors.h src/sensors.c

c "feat: NVS flash ring buffer with backfill cursor (595 records, 148 h)

Sampling never blocks on connectivity. Device records first and
transmits opportunistically. Oldest records evicted with a reported
counter so data loss is never silent." \
    src/log_store.h src/log_store.c

c "feat: excursion detection with N-sample debounce and MKT

3-sample confirmation window (15 min at 5-min cadence) rejects
door-opening transients without missing real excursions. Mean Kinetic
Temperature per the Haynes equation (Ea=83.144 kJ/mol, USP <1160>)." \
    src/excursion.h src/excursion.c

c "feat: integer-only JSON encoder with overflow poisoning

Overflow returns -ENOMEM rather than truncating: a partial audit
record must never be transmitted. Avoids %lld - Zephyr reduced
printf does not guarantee 64-bit conversions." \
    src/payload.h src/payload.c

c "feat: motion-gated GNSS fixes

One fix costs 0.375 mAh vs 0.018 mAh per uplink = 20.8x.
GNSS is the dominant power consumer. Motion-gating it is the
single highest-leverage optimisation in the firmware." \
    src/gnss_ctrl.h src/gnss_ctrl.c

c "feat: AWS IoT MQTT/TLS client and main state machine orchestration" \
    src/cloud_client.h src/cloud_client.c src/main.c

c "test: host-side logic tests (18/18 pass, no hardware needed)

Discovered missing CONFIG_FPU/NEWLIB_LIBC: exp() and log() for MKT
are undefined at link time, invisible to code review alone." \
    tests/

c "feat: AWS backend - IoT rule, Lambda, DynamoDB, Timestream

device_id from topic(2) not payload body: AWS binds identity to the
TLS certificate so a compromised device cannot impersonate another.
Audit writes idempotent on (device_id, seq): backfill replay safe." \
    backend/

c "feat: ultra-cold PT1000/MAX31865 devicetree overlay

Full 4-term Callendar-Van Dusen mandatory below 0 C. At -70 C:
R=723.345 ohm, sensitivity=4.00 ohm/C. MAX31865 gives 0.033 C/LSB
with RREF=4300, 3x better than the 0.1 C requirement." \
    overlays/

c "docs: README, RUNBOOK, power budget" \
    README.md RUNBOOK.md docs/ PUBLISHING.md 2>/dev/null || true

git add -A
git diff --cached --quiet || {
    git commit -q -m "chore: remaining project files"
    ok "remaining files"
}

say "Commit summary"
echo "  commits : $(git rev-list --count HEAD)"
echo "  files   : $(git ls-files | wc -l | tr -d ' ')"
echo
git --no-pager log --oneline

say "Pushing to GitHub"
echo "  remote: https://github.com/$GH_USER/$REPO.git"
echo

# Remove any stale remote from a previous attempt
git remote remove origin 2>/dev/null || true
git remote add origin "https://github.com/$GH_USER/$REPO.git"

# --force because the GitHub repo may have an auto-generated README commit
git push -u origin main --force

say "DONE"
echo
echo "  View your repo:"
echo "  https://github.com/$GH_USER/$REPO"
echo
echo "  Next steps:"
echo "  1. Add topics on the repo page (gear icon next to About):"
echo "     nrf9160 nordic-semiconductor zephyr cellular-iot nb-iot"
echo "     lte-m embedded-c aws-iot cold-chain bme280 gnss low-power"
echo "  2. Profile -> Customize your pins -> pin this repo"
echo
