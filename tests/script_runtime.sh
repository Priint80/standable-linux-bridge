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
installer_tree="$temporary/installer"
remote_installer_tree="$temporary/remote-installer"
curl_log="$temporary/curl.log"

mkdir -p "$driver_root" "$steamvr_root/bin" "$steam_root/legacycompat" "$(dirname -- "$runner")"
cp -a "$overlay/." "$driver_root/"
mkdir -p "$driver_root/saves"
printf '{\n  "Show in SteamVR Dashboard": false\n}\n' >"$driver_root/saves/settings.json"
touch "$driver_root/Standable.exe" \
    "$driver_root/openvr_api.dll" \
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
grep -Fq "vr_runtime=$steamvr_root" "$log"
grep -Fq "vr_override=$steamvr_root" "$log"
grep -Fq "vr_paths=$XDG_DATA_HOME/standable-linux-bridge/openvr/openvrpaths.vrpath" "$log"
grep -Eq '"Show in SteamVR Dashboard"[[:space:]]*:[[:space:]]*true' "$driver_root/saves/settings.json"
dashboard_backup_count="$(find "$XDG_STATE_HOME/standable-linux-bridge/settings-backups" -type f -name 'settings-*.json' | wc -l)"
((dashboard_backup_count == 1))

openvr_paths="$XDG_DATA_HOME/standable-linux-bridge/openvr/openvrpaths.vrpath"
[[ -f "$openvr_paths" ]]
grep -Fq "\"$steamvr_root\"" "$openvr_paths"
grep -Fq "\"$steam_root/config\"" "$openvr_paths"
grep -Fq "\"$steam_root/logs\"" "$openvr_paths"
python3 - "$openvr_paths" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    document = json.load(handle)
assert document["version"] == 1
assert len(document["runtime"]) == 1
PY

export STANDABLE_TEST_LOG="$registry_log"
mkdir -p "$installer_tree/dist"
cp install.sh "$installer_tree/install.sh"
(cd -- "$overlay" && zip -q -r "$installer_tree/dist/Standable-Linux-Bridge-Overlay.zip" .)
(cd -- "$installer_tree/dist" && sha256sum Standable-Linux-Bridge-Overlay.zip > SHA256SUMS)
bash "$installer_tree/install.sh" --standable-root "$driver_root"

mkdir -p "$remote_installer_tree/bin"
cp install.sh "$remote_installer_tree/install.sh"
cp tests/fake-curl.sh "$remote_installer_tree/bin/curl"
chmod 0755 "$remote_installer_tree/bin/curl"
STANDABLE_TEST_DIST="$installer_tree/dist" \
STANDABLE_TEST_CURL_LOG="$curl_log" \
PATH="$remote_installer_tree/bin:$PATH" \
    bash "$remote_installer_tree/install.sh" \
        --standable-root "$driver_root" \
        --no-register
grep -q '/releases/latest/download/Standable-Linux-Bridge-Overlay.zip' "$curl_log"
grep -q '/main/dist/Standable-Linux-Bridge-Overlay.zip' "$curl_log"

bash "$driver_root/scripts/update.sh" \
    --overlay-dir "$overlay" \
    --no-register
bash "$driver_root/scripts/uninstall.sh" >/dev/null
grep -q "args=<adddriver><$driver_root>" "$registry_log"
grep -q "args=<removedriver><$driver_root>" "$registry_log"
backup_count="$(find "$XDG_STATE_HOME/standable-linux-bridge/backups" -mindepth 1 -maxdepth 1 -type d | wc -l)"
((backup_count == 3))

echo "PASS: Proton/OpenVR setup, dashboard enablement, prefix sharing, UI launch, bundled/repository install, update, and registration"
