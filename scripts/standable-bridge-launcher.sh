#!/usr/bin/env bash
set -euo pipefail

session=""
native_port=""
helper_port=""
while (($#)); do
    case "$1" in
        --session) session="${2:-}"; shift 2 ;;
        --native-port) native_port="${2:-}"; shift 2 ;;
        --helper-port) helper_port="${2:-}"; shift 2 ;;
        *) echo "Unknown bridge option: $1" >&2; exit 2 ;;
    esac
done

[[ "$session" =~ ^[1-9][0-9]*$ ]] || { echo "Invalid --session" >&2; exit 2; }
[[ "$native_port" =~ ^[0-9]+$ ]] && ((native_port > 0 && native_port <= 65535)) || {
    echo "Invalid --native-port" >&2; exit 2;
}
[[ "$helper_port" =~ ^[0-9]+$ ]] && ((helper_port > 0 && helper_port <= 65535)) || {
    echo "Invalid --helper-port" >&2; exit 2;
}

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
driver_root="$(cd -- "$script_dir/.." && pwd -P)"
source "$script_dir/runtime-common.sh"

state_root="${XDG_STATE_HOME:-$HOME/.local/state}/standable-linux-bridge"
data_root="${XDG_DATA_HOME:-$HOME/.local/share}/standable-linux-bridge"
mkdir -p "$state_root" "$data_root"
bridge_log="$state_root/bridge.log"
ui_log="$state_root/ui.log"
dashboard_log="$state_root/dashboard.log"
standable_rotate_log "$bridge_log"
standable_rotate_log "$ui_log"
standable_rotate_log "$dashboard_log"
exec >>"$bridge_log" 2>&1

printf '\n[%(%Y-%m-%d %H:%M:%S)T] bridge session=%s starting\n' -1 "$session"

helper="$driver_root/bin/win64/standable_bridge_host.exe"
original_driver="$driver_root/bin/win64/driver_standable.dll"
steam_api_bridge="$driver_root/bin/win64/steam_api64.dll"
dashboard="$driver_root/bin/linux64/standable_dashboard_overlay"
ui="$driver_root/Standable.exe"
openvr_api="$driver_root/openvr_api.dll"
for required in "$helper" "$original_driver" "$steam_api_bridge" "$dashboard" "$ui" "$openvr_api"; do
    [[ -f "$required" ]] || { echo "Missing required file: $required"; exit 3; }
done

steamvr_root="$(bash "$script_dir/find-steamvr.sh")"
standable_select_runner "$steamvr_root"
standable_configure_runner "$data_root" "$driver_root" "$steamvr_root"
echo "SteamVR: $steamvr_root"
echo "Runner: $STANDABLE_RUNNER_KIND ($STANDABLE_RUNNER_PATH)"
echo "OpenVR paths: $STANDABLE_OPENVR_PATHS"

if [[ -f "$driver_root/saves/settings.json" ]]; then
    if bash "$script_dir/enable-dashboard.sh" --quiet; then
        :
    else
        echo "WARNING: The duplicate Windows dashboard entry could not be disabled automatically."
    fi
fi

exec 9>"$state_root/bridge.lock"
if ! flock -w 20 9; then
    echo "Another bridge helper did not release its lock within 20 seconds"
    exit 5
fi

prefix_marker="$data_root/.prefix-ready-$(standable_runner_fingerprint)"
if [[ ! -f "$prefix_marker" ]]; then
    echo "Initializing the private Windows prefix (first run only)"
    (cd -- "$driver_root" && "${STANDABLE_RUN_COMMAND[@]}" cmd.exe /c exit)
    touch "$prefix_marker"
fi

steamclient="$(standable_find_steamclient64 "$data_root" 2>/dev/null || true)"
if [[ -n "$steamclient" ]]; then
    echo "Authenticated Steam client runtime: $steamclient"
else
    echo "WARNING: steamclient64.dll was not visible before launch; Proton may still provide it as a builtin."
fi

if [[ "${STANDABLE_AUTOSTART_UI:-1}" != "0" ]]; then
    (
        exec 9>&-
        exec 8>"$state_root/ui.lock"
        flock -n 8 || exit 0
        printf '\n[%(%Y-%m-%d %H:%M:%S)T] Standable UI starting\n' -1 >>"$ui_log"
        cd -- "$driver_root"
        set +e
        "${STANDABLE_RUN_COMMAND[@]}" "$ui" >>"$ui_log" 2>&1
        ui_status=$?
        set -e
        printf '[%(%Y-%m-%d %H:%M:%S)T] Standable UI exited with status %s\n' -1 "$ui_status" >>"$ui_log"
    ) &
    echo "$!" >"$state_root/ui-launcher.pid"
fi

if [[ "${STANDABLE_AUTOSTART_DASHBOARD:-1}" != "0" ]]; then
    dashboard_fps="${STANDABLE_DASHBOARD_FPS:-20}"
    if [[ ! "$dashboard_fps" =~ ^[1-9][0-9]*$ ]] || ((dashboard_fps > 60)); then
        echo "WARNING: invalid STANDABLE_DASHBOARD_FPS=$dashboard_fps; using 20"
        dashboard_fps=20
    fi

    dashboard_parent_pid="${STANDABLE_DASHBOARD_PARENT_PID:-}"
    dashboard_parent_source="environment override"
    if [[ ! "$dashboard_parent_pid" =~ ^[1-9][0-9]*$ ]]; then
        dashboard_parent_pid="$(pgrep -o -x vrserver 2>/dev/null || true)"
        dashboard_parent_source="vrserver"
    fi
    if [[ ! "$dashboard_parent_pid" =~ ^[1-9][0-9]*$ ]]; then
        dashboard_parent_pid="$$"
        dashboard_parent_source="bridge launcher fallback"
        echo "WARNING: vrserver PID was not found; dashboard lifetime falls back to the bridge launcher"
    fi
    echo "Dashboard lifetime parent: pid=$dashboard_parent_pid ($dashboard_parent_source)"

    (
        exec 9>&-
        exec 7>"$state_root/dashboard.lock"
        flock -n 7 || exit 0
        printf '\n[%(%Y-%m-%d %H:%M:%S)T] native dashboard companion starting; lifetime parent pid=%s\n' \
            -1 "$dashboard_parent_pid"
        steamvr_library_path="$steamvr_root/bin/linux64:$steamvr_root/bin/linux64/qt/lib"
        export LD_LIBRARY_PATH="$steamvr_library_path${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
        exec "$dashboard" \
            --steamvr-root "$steamvr_root" \
            --driver-root "$driver_root" \
            --parent-pid "$dashboard_parent_pid" \
            --fps "$dashboard_fps"
    ) >>"$dashboard_log" 2>&1 &
    dashboard_pid="$!"
    echo "$dashboard_pid" >"$state_root/dashboard.pid"
    echo "Native dashboard companion launched (pid=$dashboard_pid)"
fi

echo "Starting original Standable provider host"
set +e
(
    cd -- "$(dirname -- "$helper")"
    "${STANDABLE_RUN_COMMAND[@]}" "$helper" \
        --session "$session" \
        --native-port "$native_port" \
        --helper-port "$helper_port"
)
status=$?
set -e
echo "Bridge helper exited with status $status"
exit "$status"
