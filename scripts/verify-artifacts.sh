#!/usr/bin/env bash
set -euo pipefail

root="${1:?usage: verify-artifacts.sh /path/to/overlay-or-driver-root [--integrated]}"
mode="${2:-}"
native="$root/bin/linux64/driver_standable.so"
dashboard="$root/bin/linux64/standable_dashboard_overlay"
helper="$root/bin/win64/standable_bridge_host.exe"
steam_api_bridge="$root/bin/win64/steam_api64.dll"
manifest_template="$root/share/standable-linux-bridge/driver.vrdrivermanifest"

[[ -f "$native" ]] || { echo "missing $native" >&2; exit 1; }
[[ -x "$dashboard" ]] || { echo "missing executable $dashboard" >&2; exit 1; }
[[ -f "$helper" ]] || { echo "missing $helper" >&2; exit 1; }
[[ -f "$steam_api_bridge" ]] || { echo "missing $steam_api_bridge" >&2; exit 1; }
[[ -f "$root/VERSION" ]] || { echo "missing VERSION" >&2; exit 1; }
[[ -f "$root/README-LINUX.md" ]] || { echo "missing README-LINUX.md" >&2; exit 1; }
[[ -f "$root/THIRD_PARTY_NOTICES.md" ]] || { echo "missing THIRD_PARTY_NOTICES.md" >&2; exit 1; }
[[ -f "$manifest_template" ]] || { echo "missing managed manifest template" >&2; exit 1; }
for script in \
    find-steamvr.sh \
    runtime-common.sh \
    enable-dashboard.sh \
    standable-bridge-launcher.sh \
    install.sh \
    source-install.sh \
    update.sh \
    repair.sh \
    uninstall.sh \
    diagnose.sh \
    manifest-manager.sh \
    bridge-manager.sh \
    bridge-installer.sh; do
    [[ -x "$root/scripts/$script" ]] || { echo "missing executable scripts/$script" >&2; exit 1; }
    bash -n "$root/scripts/$script"
done

command -v python3 >/dev/null 2>&1 || { echo "python3 is required by the bridge manager" >&2; exit 1; }
"$root/scripts/bridge-manager.sh" --self-test >/dev/null
python3 - "$manifest_template" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as handle:
    manifest = json.load(handle)
assert manifest["name"] == "standable"
assert manifest["directory"] == ""
assert manifest["alwaysActivate"] is True
assert manifest["resourceOnly"] is False
PY

native_file="$(file "$native")"
[[ "$native_file" == *"ELF 64-bit"* && "$native_file" == *"x86-64"* ]] || {
    echo "native driver has the wrong architecture: $native_file" >&2
    exit 1
}
native_symbols="$(readelf --dyn-syms --wide "$native")"
grep -q ' HmdDriverFactory$' <<<"$native_symbols" || {
    echo "native driver does not export HmdDriverFactory" >&2
    exit 1
}
native_exports="$(nm -D --defined-only "$native" | awk '{print $3}')"
if [[ "$native_exports" != "HmdDriverFactory" ]]; then
    echo "Linux driver exports symbols other than HmdDriverFactory" >&2
    exit 1
fi
native_dependencies="$(ldd "$native" 2>&1)"
if grep -Fq 'not found' <<<"$native_dependencies"; then
    echo "native driver has a missing runtime dependency" >&2
    printf '%s\n' "$native_dependencies" >&2
    exit 1
fi
native_strings="$(strings "$native")"
grep -Fq 'Standable tracker ready' <<<"$native_strings" || {
    echo "native driver is missing the privacy-safe tracker debug response" >&2
    exit 1
}
if grep -Fq 'standable-linux bridge serial=' <<<"$native_strings"; then
    echo "native driver still exposes bridge internals through DebugRequest" >&2
    exit 1
fi

dashboard_file="$(file "$dashboard")"
[[ "$dashboard_file" == *"ELF 64-bit"* && "$dashboard_file" == *"x86-64"* ]] || {
    echo "dashboard companion has the wrong architecture: $dashboard_file" >&2
    exit 1
}
dashboard_dependencies="$(ldd "$dashboard" 2>&1)"
if grep -Fq 'not found' <<<"$dashboard_dependencies"; then
    echo "dashboard companion has a missing runtime dependency" >&2
    printf '%s\n' "$dashboard_dependencies" >&2
    exit 1
fi
"$dashboard" --self-test >/dev/null

helper_file="$(file "$helper")"
[[ "$helper_file" == *"PE32+ executable"* && "$helper_file" == *"x86-64"* ]] || {
    echo "Windows helper has the wrong architecture: $helper_file" >&2
    exit 1
}
steam_bridge_file="$(file "$steam_api_bridge")"
[[ "$steam_bridge_file" == *"PE32+ executable"* && "$steam_bridge_file" == *"x86-64"* ]] || {
    echo "Steam API bridge has the wrong architecture: $steam_bridge_file" >&2
    exit 1
}
helper_headers="$(objdump -p "$helper")"
grep -q 'DLL Name: WS2_32.dll' <<<"$helper_headers" || {
    echo "Windows helper is missing WS2_32.dll" >&2
    exit 1
}
if grep -Eq 'DLL Name: (libstdc\+\+|libgcc|libwinpthread)' <<<"$helper_headers"; then
    echo "Windows helper has an unexpected MinGW runtime dependency" >&2
    exit 1
fi
steam_bridge_headers="$(objdump -p "$steam_api_bridge")"
for symbol in \
    SteamAPI_GetHSteamUser \
    SteamInternal_ContextInit \
    SteamInternal_FindOrCreateUserInterface \
    SteamInternal_SteamAPI_Init; do
    grep -q " $symbol$" <<<"$steam_bridge_headers" || {
        echo "Steam API bridge is missing export: $symbol" >&2
        exit 1
    }
done
if grep -qi 'DLL Name: steam_api64.dll' <<<"$steam_bridge_headers"; then
    echo "Steam API bridge unexpectedly imports itself" >&2
    exit 1
fi

if [[ "$mode" == "--integrated" ]]; then
    [[ -f "$root/driver.vrdrivermanifest" ]] || { echo "missing original manifest" >&2; exit 1; }
    grep -Eq '"name"[[:space:]]*:[[:space:]]*"standable"' "$root/driver.vrdrivermanifest"
    printf '%s  %s\n' \
        '56f923ad96b46ba9f5b3c158ae0bcfeebe2bebc582b98968f397b67b2eeac9bf' \
        "$root/bin/win64/driver_standable.dll" | sha256sum --check --status
    printf '%s  %s\n' \
        '105c096d733c5628c9a79944caeb4622a3e7c4b8bd676e86ae992272f4c33c62' \
        "$root/Standable.exe" | sha256sum --check --status
fi

echo "PASS: Linux driver, dashboard input polish, maintenance UI, manifest safety, dependencies, scripts, and layout"
