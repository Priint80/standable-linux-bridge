#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
driver_root="$(cd -- "$script_dir/.." && pwd -P)"
settings_path=""
if_present=0
quiet=0

usage() {
    cat <<'EOF'
Usage: ./scripts/enable-dashboard.sh [options]

Configures Standable to use the native Linux dashboard companion and disables
the original Windows dashboard mirror, which does not render reliably in Proton.

Options:
  --settings PATH  Use a specific Standable settings.json file
  --if-present     Succeed when the settings file has not been created yet
  --quiet          Suppress informational output
  -h, --help       Show this help
EOF
}

while (($#)); do
    case "$1" in
        --settings) settings_path="${2:-}"; shift 2 ;;
        --if-present) if_present=1; shift ;;
        --quiet) quiet=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

settings_path="${settings_path:-$driver_root/saves/settings.json}"
say() {
    ((quiet)) || printf '%s\n' "$*"
}

if [[ ! -f "$settings_path" ]]; then
    if ((if_present)); then
        say "Standable settings have not been created yet; native dashboard configuration will be applied later."
        exit 0
    fi
    echo "Standable settings were not found: $settings_path" >&2
    exit 3
fi

true_pattern='"Show in SteamVR Dashboard"[[:space:]]*:[[:space:]]*true'
false_pattern='"Show in SteamVR Dashboard"[[:space:]]*:[[:space:]]*false'
if grep -Eq "$false_pattern" "$settings_path"; then
    say "Standable's native Linux dashboard is already configured."
    exit 0
fi

match_count="$(grep -Ec "$true_pattern" "$settings_path" || true)"
if [[ "$match_count" != "1" ]]; then
    echo "Could not safely locate the Standable dashboard setting in: $settings_path" >&2
    exit 4
fi

state_root="${XDG_STATE_HOME:-$HOME/.local/state}/standable-linux-bridge"
backup_dir="$state_root/settings-backups"
backup="$backup_dir/settings-$(date -u +%Y%m%dT%H%M%S.%NZ).json"
temporary="$settings_path.dashboard-new.$$"
cleanup() {
    [[ -f "$temporary" ]] && rm -f -- "$temporary"
}
trap cleanup EXIT

mkdir -p "$backup_dir"
cp -p -- "$settings_path" "$backup"
cp -p -- "$settings_path" "$temporary"
sed -E "s/($true_pattern)/\"Show in SteamVR Dashboard\": false/" "$settings_path" >"$temporary"

if ! grep -Eq "$false_pattern" "$temporary" || grep -Eq "$true_pattern" "$temporary"; then
    echo "Dashboard setting verification failed; the original settings file was not changed." >&2
    exit 5
fi

mv -f -- "$temporary" "$settings_path"
say "Enabled Standable's native Linux dashboard and disabled the duplicate Windows dashboard entry."
say "Settings backup: $backup"
