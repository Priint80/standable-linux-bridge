#!/usr/bin/env bash
set -u

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
driver_root="$(cd -- "$script_dir/.." && pwd -P)"
source "$script_dir/runtime-common.sh"

echo "Standable Linux bridge diagnostics"
echo "Driver root: $driver_root"
echo

for path in \
    "$driver_root/driver.vrdrivermanifest" \
    "$driver_root/Standable.exe" \
    "$driver_root/openvr_api.dll" \
    "$driver_root/bin/win64/driver_standable.dll" \
    "$driver_root/bin/win64/standable_bridge_host.exe" \
    "$driver_root/bin/win64/steam_api64.dll" \
    "$driver_root/bin/linux64/driver_standable.so" \
    "$driver_root/bin/linux64/standable_dashboard_overlay"; do
    if [[ -f "$path" ]]; then
        echo "OK: $path"
    else
        echo "MISSING: $path"
    fi
done

if [[ -f "$driver_root/bin/win64/driver_standable.dll" ]]; then
    echo "Original DLL SHA-256: $(sha256sum "$driver_root/bin/win64/driver_standable.dll" | awk '{print $1}')"
fi
if [[ -f "$driver_root/bin/linux64/driver_standable.so" ]]; then
    file "$driver_root/bin/linux64/driver_standable.so"
    ldd "$driver_root/bin/linux64/driver_standable.so" 2>&1 | sed 's/^/  /'
fi
if [[ -f "$driver_root/bin/linux64/standable_dashboard_overlay" ]]; then
    file "$driver_root/bin/linux64/standable_dashboard_overlay"
    ldd "$driver_root/bin/linux64/standable_dashboard_overlay" 2>&1 | sed 's/^/  /'
    "$driver_root/bin/linux64/standable_dashboard_overlay" --self-test 2>&1 | sed 's/^/  /'
fi
echo

echo "Desktop session: XDG_SESSION_TYPE=${XDG_SESSION_TYPE:-<unset>} DISPLAY=${DISPLAY:-<unset>} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-<unset>}"
for library in libX11.so.6 libXcomposite.so.1 libXtst.so.6 libGL.so.1; do
    if ldconfig -p 2>/dev/null | grep -Fq "$library"; then
        echo "OK: dashboard capture/texture runtime: $library"
    else
        echo "MISSING: dashboard capture/texture runtime: $library"
    fi
done
echo

steamvr_root="$(bash "$script_dir/find-steamvr.sh" 2>/dev/null || true)"
if [[ -n "$steamvr_root" ]]; then
    echo "SteamVR: $steamvr_root"
    if [[ -f "$steamvr_root/bin/linux64/vrclient.so" ]]; then
        echo "OK: native SteamVR client: $steamvr_root/bin/linux64/vrclient.so"
    else
        echo "MISSING: native SteamVR client: $steamvr_root/bin/linux64/vrclient.so"
    fi
    if standable_select_runner "$steamvr_root" 2>/dev/null; then
        echo "Runner: $STANDABLE_RUNNER_KIND ($STANDABLE_RUNNER_PATH)"
        data_root="${XDG_DATA_HOME:-$HOME/.local/share}/standable-linux-bridge"
        standable_configure_runner "$data_root" "$driver_root"
        steamclient="$(standable_find_steamclient64 "$data_root" 2>/dev/null || true)"
        if [[ -n "$steamclient" ]]; then
            echo "Steam client runtime: $steamclient"
        else
            echo "NOT YET VISIBLE: steamclient64.dll (run Steam once and let Proton initialize the prefix)"
        fi
        bridge_openvr_paths="$data_root/openvr/openvrpaths.vrpath"
        prefix_openvr_paths="$data_root/compatdata/2370570/pfx/drive_c/users/steamuser/AppData/Local/openvr/openvrpaths.vrpath"
        vrclient="$data_root/compatdata/2370570/pfx/drive_c/vrclient/bin/vrclient_x64.dll"
        for label_and_path in \
            "Bridge OpenVR paths|$bridge_openvr_paths" \
            "Proton OpenVR paths|$prefix_openvr_paths" \
            "Proton vrclient|$vrclient"; do
            label="${label_and_path%%|*}"
            path="${label_and_path#*|}"
            if [[ -f "$path" ]]; then
                echo "OK: $label: $path"
            else
                echo "MISSING: $label: $path"
            fi
        done
    else
        echo "MISSING: compatible Proton or Wine runner"
    fi
    echo
    echo "SteamVR registered drivers:"
    "$steamvr_root/bin/vrpathreg.sh" show 2>&1 | sed 's/^/  /'
else
    echo "MISSING: SteamVR (set STEAMVR_ROOT if it is installed in a custom location)"
fi

echo
settings="$driver_root/saves/settings.json"
if [[ ! -f "$settings" ]]; then
    echo "Dashboard setting: MISSING ($settings)"
elif grep -Eq '"Show in SteamVR Dashboard"[[:space:]]*:[[:space:]]*true' "$settings"; then
    echo "Dashboard setting: duplicate Windows dashboard entry is enabled (run ./scripts/enable-dashboard.sh)"
elif grep -Eq '"Show in SteamVR Dashboard"[[:space:]]*:[[:space:]]*false' "$settings"; then
    echo "Dashboard setting: native Linux dashboard mode configured"
else
    echo "Dashboard setting: key not found"
fi

state_root="${XDG_STATE_HOME:-$HOME/.local/state}/standable-linux-bridge"
echo
echo "Bridge log: $state_root/bridge.log"
if [[ -f "$state_root/bridge.log" ]]; then
    tail -n 100 "$state_root/bridge.log" | sed 's/^/  /'
else
    echo "  No bridge log yet."
fi
echo
echo "UI log: $state_root/ui.log"
if [[ -f "$state_root/ui.log" ]]; then
    tail -n 60 "$state_root/ui.log" | sed 's/^/  /'
    echo
    echo "Recent dashboard/OpenVR UI lines:"
    dashboard_lines="$(tail -n 500 "$state_root/ui.log" | grep -Ei '\[VRDashMirror\]|OpenVR|CreateDashboardOverlay|SetOverlay|dashboard' | tail -n 80 || true)"
    if [[ -n "$dashboard_lines" ]]; then
        printf '%s\n' "$dashboard_lines" | sed 's/^/  /'
    else
        echo "  No dashboard-related UI lines yet."
    fi
else
    echo "  No UI log yet."
fi

echo
echo "Native dashboard log: $state_root/dashboard.log"
if [[ -f "$state_root/dashboard.log" ]]; then
    tail -n 120 "$state_root/dashboard.log" | sed 's/^/  /'
else
    echo "  No native dashboard log yet. Restart SteamVR after installing this version."
fi

for log in \
    "$HOME/.local/share/Steam/logs/vrserver.txt" \
    "$HOME/.steam/steam/logs/vrserver.txt" \
    "$HOME/.var/app/com.valvesoftware.Steam/data/Steam/logs/vrserver.txt"; do
    [[ -f "$log" ]] || continue
    echo
    echo "Recent SteamVR bridge lines: $log"
    tail -n 500 "$log" | grep -Ei 'standable-linux|standable_bridge|driver_standable' | tail -n 80 | sed 's/^/  /'
    break
done
