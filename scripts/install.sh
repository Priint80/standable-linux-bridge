#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
driver_root="$(cd -- "$script_dir/.." && pwd -P)"
manifest_manager="$script_dir/manifest-manager.sh"
manifest_template="$driver_root/share/standable-linux-bridge/driver.vrdrivermanifest"

required=(
    "$driver_root/driver.vrdrivermanifest"
    "$driver_root/Standable.exe"
    "$driver_root/bin/win64/driver_standable.dll"
    "$driver_root/bin/win64/standable_bridge_host.exe"
    "$driver_root/bin/win64/steam_api64.dll"
    "$driver_root/bin/linux64/driver_standable.so"
    "$driver_root/bin/linux64/standable_dashboard_overlay"
    "$manifest_manager"
)
for path in "${required[@]}"; do
    [[ -f "$path" ]] || {
        echo "Missing required bridge file: $path" >&2
        echo "Run the top-level installer or extract the complete Linux overlay first." >&2
        exit 2
    }
done

chmod 0755 "$driver_root/bin/linux64/driver_standable.so" \
    "$driver_root/bin/linux64/standable_dashboard_overlay" \
    "$driver_root/bin/win64/standable_bridge_host.exe" \
    "$script_dir"/*.sh "$script_dir"/*.py

driver_name="$(bash "$manifest_manager" install "$driver_root" "$manifest_template")"
expected_native="$driver_root/bin/linux64/driver_${driver_name}.so"
[[ -f "$expected_native" ]] || {
    echo "Managed manifest expects a native driver binary that is missing: $expected_native" >&2
    exit 3
}

steamvr_root="$(bash "$script_dir/find-steamvr.sh")"
if ! bash "$script_dir/enable-dashboard.sh" --if-present; then
    echo "WARNING: The duplicate Windows dashboard entry could not be disabled automatically." >&2
fi

# Refresh this exact path so stale registrations cannot remain ahead of it.
"$steamvr_root/bin/vrpathreg.sh" removedriver "$driver_root" >/dev/null 2>&1 || true
"$steamvr_root/bin/vrpathreg.sh" adddriver "$driver_root"

echo "Standable Linux Bridge is installed and registered."
echo "Restart SteamVR, then open Standable from the dashboard."
echo "Maintenance: ./scripts/bridge-manager.sh"
