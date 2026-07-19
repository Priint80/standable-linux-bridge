#!/usr/bin/env bash
set -euo pipefail

overlay="${1:?usage: script_runtime.sh /path/to/overlay}"
temporary="$(mktemp -d /tmp/standable-bridge-test.XXXXXX)"
trap 'rm -rf "$temporary"' EXIT

driver_root="$temporary/Standable Full Body Estimation"
steam_root="$temporary/Steam Library"
steamvr_root="$steam_root/steamapps/common/SteamVR"
runner="$temporary/Proton Test/proton"
log="$temporary/runner.log"
registry_log="$temporary/registry.log"

mkdir -p "$driver_root" "$steamvr_root/bin" "$steam_root/legacycompat" "$(dirname -- "$runner")"
cp -a "$overlay/." "$driver_root/"
touch "$driver_root/Standable.exe" \
    "$driver_root/driver.vrdrivermanifest" \
    "$driver_root/bin/win64/driver_standable.dll" \
    "$steam_root/legacycompat/steamclient64.dll"
cp tests/fake-proton.sh "$runner"
chmod 0755 "$runner"

cp tests/fake-proton.sh "$steamvr_root/bin/vrpathreg.sh"
chmod 0755 "$steamvr_root/bin/vrpathreg.sh"

export STEAMVR_ROOT="$steamvr_root"
export STANDABLE_PROTON="$runner"
export STANDABLE_TEST_LOG="$log"
export XDG_STATE_HOME="$temporary/state"
export XDG_DATA_HOME="$temporary/data"

bash "$driver_root/scripts/standable-bridge-launcher.sh" \
    --session 99112233 --native-port 42470 --helper-port 42471

for attempt in {1..100}; do
    grep -q 'Standable.exe' "$log" 2>/dev/null && break
    sleep 0.01
done

grep -q 'app=2370570' "$log"
grep -q 'args=<run><cmd.exe></c><exit>' "$log"
grep -q "cwd=$driver_root/bin/win64" "$log"
grep -q 'standable_bridge_host.exe><--session><99112233><--native-port><42470><--helper-port><42471>' "$log"
grep -q 'Standable.exe' "$log"

export STANDABLE_TEST_LOG="$registry_log"
bash install.sh \
    --overlay-dir "$overlay" \
    --standable-root "$driver_root"
bash "$driver_root/scripts/update.sh" \
    --overlay-dir "$overlay" \
    --no-register
bash "$driver_root/scripts/uninstall.sh" >/dev/null
grep -q "args=<adddriver><$driver_root>" "$registry_log"
grep -q "args=<removedriver><$driver_root>" "$registry_log"

echo "PASS: Proton selection, prefix sharing, Steam client discovery, UI launch, install, update, and registration"
