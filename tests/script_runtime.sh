#!/usr/bin/env bash
set -euo pipefail

overlay="${1:?usage: script_runtime.sh /path/to/overlay}"
temporary="$(mktemp -d /tmp/standable-bridge-test.XXXXXX)"
dashboard_parent_pid=""
dashboard_process_pid=""
cleanup() {
    if [[ -n "$dashboard_parent_pid" ]]; then
        kill "$dashboard_parent_pid" 2>/dev/null || true
    fi
    if [[ -n "$dashboard_process_pid" ]]; then
        kill "$dashboard_process_pid" 2>/dev/null || true
        wait "$dashboard_process_pid" 2>/dev/null || true
    fi
    rm -rf "$temporary"
}
trap cleanup EXIT

driver_root="$temporary/Standable Full Body Estimation"
steam_root="$temporary/Steam Library"
steamvr_root="$steam_root/steamapps/common/SteamVR"
runner="$temporary/Proton Test/proton"
log="$temporary/runner.log"
registry_log="$temporary/registry.log"
installer_tree="$temporary/installer"
remote_installer_tree="$temporary/remote-installer"
source_checkout="$driver_root/standable-linux-bridge"
curl_log="$temporary/curl.log"
original_manifest="$temporary/original-driver.vrdrivermanifest"

mkdir -p "$driver_root" "$steamvr_root/bin" "$steam_root/legacycompat" "$(dirname -- "$runner")"
cp -a "$overlay/." "$driver_root/"
mkdir -p "$driver_root/saves"
printf '{\n  "Show in SteamVR Dashboard": true\n}\n' >"$driver_root/saves/settings.json"
touch "$driver_root/Standable.exe" \
    "$driver_root/openvr_api.dll" \
    "$driver_root/bin/win64/driver_standable.dll" \
    "$steam_root/legacycompat/steamclient64.dll"
cat >"$driver_root/driver.vrdrivermanifest" <<'JSON'
{
  "alwaysActivate": false,
  "name": "standable",
  "directory": "bin/win64",
  "resourceOnly": false,
  "displayName": "Standable",
  "iconPath": "resources/icons/standable.png"
}
JSON
cp -a "$driver_root/driver.vrdrivermanifest" "$original_manifest"
cp tests/fake-proton.sh "$runner"
chmod 0755 "$runner"

cp tests/fake-proton.sh "$steamvr_root/bin/vrpathreg.sh"
chmod 0755 "$steamvr_root/bin/vrpathreg.sh"

export STEAMVR_ROOT="$steamvr_root"
export STANDABLE_PROTON="$runner"
export STANDABLE_TEST_LOG="$log"
export XDG_STATE_HOME="$temporary/state"
export XDG_DATA_HOME="$temporary/data"

sleep 30 &
dashboard_parent_pid="$!"
export STANDABLE_DASHBOARD_PARENT_PID="$dashboard_parent_pid"

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
[[ -f "$XDG_STATE_HOME/standable-linux-bridge/dashboard.log" ]]
grep -q 'native dashboard companion starting' "$XDG_STATE_HOME/standable-linux-bridge/dashboard.log"
dashboard_process_pid="$(cat "$XDG_STATE_HOME/standable-linux-bridge/dashboard.pid")"
kill -0 "$dashboard_process_pid"
kill "$dashboard_parent_pid"
wait "$dashboard_parent_pid" 2>/dev/null || true
dashboard_parent_pid=""
for attempt in {1..400}; do
    if ! kill -0 "$dashboard_process_pid" 2>/dev/null; then
        break
    fi
    sleep 0.01
done
if kill -0 "$dashboard_process_pid" 2>/dev/null; then
    echo "dashboard companion survived its lifetime parent" >&2
    exit 1
fi
wait "$dashboard_process_pid" 2>/dev/null || true
dashboard_process_pid=""
grep -Eq '"Show in SteamVR Dashboard"[[:space:]]*:[[:space:]]*false' "$driver_root/saves/settings.json"
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

"$driver_root/scripts/bridge-manager.sh" --self-test
test -f "$driver_root/share/standable-linux-bridge/driver.vrdrivermanifest"
python3 - "$driver_root/driver.vrdrivermanifest" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    manifest = json.load(handle)
assert manifest["name"] == "standable"
assert manifest["directory"] == ""
assert manifest["alwaysActivate"] is True
assert manifest["resourceOnly"] is False
assert manifest["displayName"] == "Standable"
assert manifest["iconPath"] == "resources/icons/standable.png"
PY
manifest_state="$(bash "$driver_root/scripts/manifest-manager.sh" state-dir "$driver_root")"
cmp -s "$original_manifest" "$manifest_state/original-driver.vrdrivermanifest"
test -f "$manifest_state/metadata.env"
test -f "$manifest_state/installed-files.tsv"

diagnostics="$temporary/diagnostics.txt"
bash "$driver_root/scripts/diagnose.sh" >"$diagnostics" 2>&1
grep -Fq '<standable-root>' "$diagnostics"
if grep -Fq "$driver_root" "$diagnostics"; then
    echo "privacy-redacted diagnostics exposed the Standable path" >&2
    exit 1
fi
grep -Fq 'exact original SteamVR manifest backup is present' "$diagnostics"

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

mkdir -p "$source_checkout/scripts" "$source_checkout/dist"
cp install.sh "$source_checkout/install.sh"
cp scripts/update.sh "$source_checkout/scripts/update.sh"
cp "$installer_tree/dist/Standable-Linux-Bridge-Overlay.zip" "$source_checkout/dist/Standable-Linux-Bridge-Overlay.zip"
cp "$installer_tree/dist/SHA256SUMS" "$source_checkout/dist/SHA256SUMS"
chmod 0755 "$source_checkout/install.sh" "$source_checkout/scripts/update.sh"
(
    cd -- "$source_checkout"
    bash ./scripts/update.sh --no-register
)

bash "$driver_root/scripts/update.sh" \
    --overlay-dir "$overlay" \
    --no-register
bash "$driver_root/scripts/uninstall.sh" >/dev/null
cmp -s "$original_manifest" "$driver_root/driver.vrdrivermanifest"
grep -q "args=<adddriver><$driver_root>" "$registry_log"
grep -q "args=<removedriver><$driver_root>" "$registry_log"
backup_count="$(find "$XDG_STATE_HOME/standable-linux-bridge/backups" -mindepth 1 -maxdepth 1 -type d | wc -l)"
((backup_count == 4))

echo "PASS: Proton/OpenVR setup, dashboard lifetime, managed manifest, graphical maintenance, redacted diagnostics, updates, rollback, and registration"
