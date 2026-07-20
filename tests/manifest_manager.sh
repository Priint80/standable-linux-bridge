#!/usr/bin/env bash
set -euo pipefail

manager="${1:-scripts/manifest-manager.sh}"
template="${2:-packaging/driver.vrdrivermanifest}"
scripts_dir="$(cd -- "$(dirname -- "$manager")" && pwd -P)"
temporary="$(mktemp -d /tmp/standable-manifest-test.XXXXXX)"
cleanup() { rm -rf -- "$temporary"; }
trap cleanup EXIT

export XDG_STATE_HOME="$temporary/state"

# In a real source checkout, the graphical Install action must route through
# source-install.sh so the current branch is built instead of reusing dist/.
bash "$scripts_dir/bridge-manager.sh" --self-test >/dev/null

make_root() {
    local root="$1"
    mkdir -p "$root/bin/linux64"
    printf 'native-driver\n' >"$root/bin/linux64/driver_standable.so"
    cat >"$root/driver.vrdrivermanifest" <<'JSON'
{
  "alwaysActivate": false,
  "name": "standable_original",
  "directory": "bin/win64",
  "resourceOnly": false,
  "displayName": "Standable Full Body Estimation",
  "iconPath": "resources/icons/standable.png"
}
JSON
}

root="$temporary/Standable Full Body Estimation"
make_root "$root"
cp -a "$root/driver.vrdrivermanifest" "$temporary/original-manifest"

driver_name="$(bash "$manager" install "$root" "$template")"
[[ "$driver_name" == "standable_original" ]]
[[ -L "$root/bin/linux64/driver_standable_original.so" ]]
[[ "$(readlink -f "$root/bin/linux64/driver_standable_original.so")" == "$(readlink -f "$root/bin/linux64/driver_standable.so")" ]]

python3 - "$root/driver.vrdrivermanifest" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    manifest = json.load(handle)
assert manifest["name"] == "standable_original"
assert manifest["displayName"] == "Standable Full Body Estimation"
assert manifest["iconPath"] == "resources/icons/standable.png"
assert manifest["directory"] == ""
assert manifest["alwaysActivate"] is True
assert manifest["resourceOnly"] is False
PY

state_dir="$(bash "$manager" state-dir "$root")"
cmp -s "$temporary/original-manifest" "$state_dir/original-driver.vrdrivermanifest"
bash "$manager" restore "$root"
cmp -s "$temporary/original-manifest" "$root/driver.vrdrivermanifest"
[[ ! -e "$root/bin/linux64/driver_standable_original.so" && ! -L "$root/bin/linux64/driver_standable_original.so" ]]

conflict_root="$temporary/Conflicting Standable"
make_root "$conflict_root"
cp -a "$conflict_root/driver.vrdrivermanifest" "$temporary/conflict-original"
printf 'unrelated-driver\n' >"$conflict_root/bin/linux64/driver_standable_original.so"
conflict_hash="$(sha256sum "$conflict_root/bin/linux64/driver_standable_original.so" | awk '{print $1}')"

if bash "$manager" install "$conflict_root" "$template" >/dev/null 2>&1; then
    echo "manifest manager accepted a conflicting native driver alias" >&2
    exit 1
fi
cmp -s "$temporary/conflict-original" "$conflict_root/driver.vrdrivermanifest"
[[ "$(sha256sum "$conflict_root/bin/linux64/driver_standable_original.so" | awk '{print $1}')" == "$conflict_hash" ]]

echo "PASS: source GUI routing, manifest identity, alias creation, exact restoration, and conflict preservation"
