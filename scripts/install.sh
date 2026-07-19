#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
driver_root="$(cd -- "$script_dir/.." && pwd -P)"

required=(
    "$driver_root/driver.vrdrivermanifest"
    "$driver_root/Standable.exe"
    "$driver_root/bin/win64/driver_standable.dll"
    "$driver_root/bin/win64/standable_bridge_host.exe"
    "$driver_root/bin/win64/steam_api64.dll"
    "$driver_root/bin/linux64/driver_standable.so"
)
for path in "${required[@]}"; do
    [[ -f "$path" ]] || {
        echo "Missing: $path" >&2
        echo "Extract the Linux overlay into the original Standable driver folder first." >&2
        exit 2
    }
done

chmod 0755 "$driver_root/bin/linux64/driver_standable.so" \
    "$driver_root/bin/win64/standable_bridge_host.exe" "$script_dir"/*.sh

steamvr_root="$(bash "$script_dir/find-steamvr.sh")"
"$steamvr_root/bin/vrpathreg.sh" adddriver "$driver_root"

echo "Registered Standable Linux bridge: $driver_root"
echo "Start or restart SteamVR. The bridge and Standable UI will start automatically."
echo "Run ./scripts/diagnose.sh if the trackers do not appear."
echo "Future updates: ./scripts/update.sh"
