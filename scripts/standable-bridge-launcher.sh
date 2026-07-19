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
standable_rotate_log "$bridge_log"
standable_rotate_log "$ui_log"
exec >>"$bridge_log" 2>&1

printf '\n[%(%Y-%m-%d %H:%M:%S)T] bridge session=%s starting\n' -1 "$session"

helper="$driver_root/bin/win64/standable_bridge_host.exe"
original_driver="$driver_root/bin/win64/driver_standable.dll"
steam_api_bridge="$driver_root/bin/win64/steam_api64.dll"
ui="$driver_root/Standable.exe"
for required in "$helper" "$original_driver" "$steam_api_bridge" "$ui"; do
    [[ -f "$required" ]] || { echo "Missing required file: $required"; exit 3; }
done

steamvr_root="$(bash "$script_dir/find-steamvr.sh")"
standable_select_runner "$steamvr_root"
standable_configure_runner "$data_root" "$driver_root"
echo "SteamVR: $steamvr_root"
echo "Runner: $STANDABLE_RUNNER_KIND ($STANDABLE_RUNNER_PATH)"

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
