#!/usr/bin/env bash
set -euo pipefail

root="${1:?usage: verify-artifacts.sh /path/to/overlay-or-driver-root [--integrated]}"
mode="${2:-}"
native="$root/bin/linux64/driver_standable.so"
helper="$root/bin/win64/standable_bridge_host.exe"
steam_api_bridge="$root/bin/win64/steam_api64.dll"

[[ -f "$native" ]] || { echo "missing $native" >&2; exit 1; }
[[ -f "$helper" ]] || { echo "missing $helper" >&2; exit 1; }
[[ -f "$steam_api_bridge" ]] || { echo "missing $steam_api_bridge" >&2; exit 1; }
[[ -f "$root/VERSION" ]] || { echo "missing VERSION" >&2; exit 1; }
[[ -f "$root/README-LINUX.md" ]] || { echo "missing README-LINUX.md" >&2; exit 1; }
for script in find-steamvr.sh runtime-common.sh standable-bridge-launcher.sh install.sh update.sh uninstall.sh diagnose.sh bridge-installer.sh; do
    [[ -x "$root/scripts/$script" ]] || { echo "missing executable scripts/$script" >&2; exit 1; }
    bash -n "$root/scripts/$script"
done

file "$native" | grep -q 'ELF 64-bit.*x86-64'
readelf --dyn-syms --wide "$native" | grep -q ' HmdDriverFactory$'
if [[ "$(nm -D --defined-only "$native" | awk '{print $3}')" != "HmdDriverFactory" ]]; then
    echo "Linux driver exports symbols other than HmdDriverFactory" >&2
    exit 1
fi
ldd "$native" >/dev/null
file "$helper" | grep -q 'PE32+ executable.*x86-64'
file "$steam_api_bridge" | grep -q 'PE32+ executable.*x86-64'
objdump -p "$helper" | grep -q 'DLL Name: WS2_32.dll'
if objdump -p "$helper" | grep -Eq 'DLL Name: (libstdc\+\+|libgcc|libwinpthread)'; then
    echo "Windows helper has an unexpected MinGW runtime dependency" >&2
    exit 1
fi
for symbol in \
    SteamAPI_GetHSteamUser \
    SteamInternal_ContextInit \
    SteamInternal_FindOrCreateUserInterface \
    SteamInternal_SteamAPI_Init; do
    objdump -p "$steam_api_bridge" | grep -q " $symbol$"
done
if objdump -p "$steam_api_bridge" | grep -qi 'DLL Name: steam_api64.dll'; then
    echo "Steam API bridge unexpectedly imports itself" >&2
    exit 1
fi

if [[ "$mode" == "--integrated" ]]; then
    [[ -f "$root/driver.vrdrivermanifest" ]] || { echo "missing original manifest" >&2; exit 1; }
    grep -q '"name": "standable"' "$root/driver.vrdrivermanifest"
    printf '%s  %s\n' \
        '56f923ad96b46ba9f5b3c158ae0bcfeebe2bebc582b98968f397b67b2eeac9bf' \
        "$root/bin/win64/driver_standable.dll" | sha256sum --check --status
    printf '%s  %s\n' \
        '105c096d733c5628c9a79944caeb4622a3e7c4b8bd676e86ae992272f4c33c62' \
        "$root/Standable.exe" | sha256sum --check --status
fi

echo "PASS: Linux bridge artifacts, exports, dependencies, scripts, and layout"
