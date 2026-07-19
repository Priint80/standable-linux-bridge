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
    "$driver_root/bin/win64/driver_standable.dll" \
    "$driver_root/bin/win64/standable_bridge_host.exe" \
    "$driver_root/bin/win64/steam_api64.dll" \
    "$driver_root/bin/linux64/driver_standable.so"; do
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
echo

steamvr_root="$(bash "$script_dir/find-steamvr.sh" 2>/dev/null || true)"
if [[ -n "$steamvr_root" ]]; then
    echo "SteamVR: $steamvr_root"
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
    else
        echo "MISSING: compatible Proton or Wine runner"
    fi
    echo
    echo "SteamVR registered drivers:"
    "$steamvr_root/bin/vrpathreg.sh" show 2>&1 | sed 's/^/  /'
else
    echo "MISSING: SteamVR (set STEAMVR_ROOT if it is installed in a custom location)"
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
else
    echo "  No UI log yet."
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
