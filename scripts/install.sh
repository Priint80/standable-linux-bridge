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
    "$script_dir"/*.sh

driver_name="$(bash "$manifest_manager" install "$driver_root" "$manifest_template")"
expected_native="$driver_root/bin/linux64/driver_${driver_name}.so"
[[ -f "$expected_native" ]] || {
    echo "Managed manifest expects a native driver binary that is missing: $expected_native" >&2
    exit 3
}

# Save enough provenance for Update and Repair to stay on the corresponding
# repository branch instead of silently switching an installation to main.
state_dir="$(bash "$manifest_manager" state-dir "$driver_root")"
repo="${STANDABLE_BRIDGE_REPO:-Priint80/standable-linux-bridge}"
branch="${STANDABLE_BRIDGE_BRANCH:-main}"
source_checkout="${STANDABLE_BRIDGE_SOURCE_CHECKOUT:-}"
if [[ -z "$source_checkout" ]]; then
    for candidate in "$driver_root/standable-linux-bridge" "$PWD"; do
        if [[ -d "$candidate/.git" && -f "$candidate/Makefile" ]]; then
            source_checkout="$(cd -- "$candidate" && pwd -P)"
            break
        fi
    done
fi
if [[ -n "$source_checkout" && -d "$source_checkout/.git" ]] && command -v git >/dev/null 2>&1; then
    detected_branch="$(git -C "$source_checkout" branch --show-current 2>/dev/null || true)"
    [[ -n "$detected_branch" ]] && branch="$detected_branch"
fi
mkdir -p "$state_dir"
{
    printf 'STANDABLE_BRIDGE_REPO=%q\n' "$repo"
    printf 'STANDABLE_BRIDGE_BRANCH=%q\n' "$branch"
    printf 'STANDABLE_BRIDGE_SOURCE_CHECKOUT=%q\n' "$source_checkout"
    if [[ -f "$driver_root/VERSION" ]]; then
        printf 'STANDABLE_BRIDGE_VERSION=%q\n' "$(tr -d '\r\n' <"$driver_root/VERSION")"
    fi
} >"$state_dir/metadata.env"

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
