#!/usr/bin/env bash
set -euo pipefail

bootstrap_script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
driver_root="$(cd -- "$bootstrap_script_dir/.." && pwd -P)"
keep_state=0
purge_state=0

while (($#)); do
    case "$1" in
        --standable-root)
            driver_root="${2:-}"
            shift 2
            ;;
        --keep-state)
            keep_state=1
            shift
            ;;
        --purge-state)
            purge_state=1
            shift
            ;;
        -h|--help)
            cat <<'EOF'
Usage: ./scripts/uninstall.sh [--standable-root PATH] [--purge-state]

Unregisters the Linux bridge, restores the exact original SteamVR manifest,
and removes only files owned by the bridge. Standable settings, saved poses,
resources, the original executable, and driver_standable.dll are preserved.
EOF
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 2
            ;;
    esac
done

[[ -d "$driver_root" ]] || { echo "Standable folder does not exist: $driver_root" >&2; exit 2; }
driver_root="$(cd -- "$driver_root" && pwd -P)"
installed_script_dir="$driver_root/scripts"

# Repair executes this script from a temporary bundle. In that case the fresh
# bundled helpers must win over same-named legacy installed scripts. A normal
# installed uninstall still uses the helpers in the Standable scripts folder.
if [[ "$bootstrap_script_dir" != "$installed_script_dir" && -f "$bootstrap_script_dir/manifest-manager.sh" ]]; then
    manifest_manager="$bootstrap_script_dir/manifest-manager.sh"
else
    manifest_manager="$installed_script_dir/manifest-manager.sh"
    [[ -f "$manifest_manager" ]] || manifest_manager="$bootstrap_script_dir/manifest-manager.sh"
fi
if [[ "$bootstrap_script_dir" != "$installed_script_dir" && -f "$bootstrap_script_dir/find-steamvr.sh" ]]; then
    find_steamvr="$bootstrap_script_dir/find-steamvr.sh"
else
    find_steamvr="$installed_script_dir/find-steamvr.sh"
    [[ -f "$find_steamvr" ]] || find_steamvr="$bootstrap_script_dir/find-steamvr.sh"
fi

state_dir=""
if [[ -f "$manifest_manager" ]]; then
    state_dir="$(bash "$manifest_manager" state-dir "$driver_root")"
fi

if [[ -f "$find_steamvr" ]]; then
    steamvr_root="$(bash "$find_steamvr" 2>/dev/null || true)"
    if [[ -n "$steamvr_root" && -x "$steamvr_root/bin/vrpathreg.sh" ]]; then
        "$steamvr_root/bin/vrpathreg.sh" removedriver "$driver_root" >/dev/null 2>&1 || true
    fi
fi

state_root="${XDG_STATE_HOME:-$HOME/.local/state}/standable-linux-bridge"
pid_file="$state_root/dashboard.pid"
if [[ -f "$pid_file" ]]; then
    read -r dashboard_pid <"$pid_file" || true
    if [[ "${dashboard_pid:-}" =~ ^[1-9][0-9]*$ && -e "/proc/$dashboard_pid/exe" ]]; then
        executable="$(readlink -f "/proc/$dashboard_pid/exe" 2>/dev/null || true)"
        if [[ "$executable" == "$driver_root/bin/linux64/standable_dashboard_overlay" ]]; then
            kill "$dashboard_pid" 2>/dev/null || true
        fi
    fi
    rm -f -- "$pid_file"
fi

if [[ -f "$manifest_manager" ]]; then
    bash "$manifest_manager" restore "$driver_root" || {
        echo "WARNING: The original manifest could not be restored automatically." >&2
        echo "The current manifest was left in place to avoid deleting unknown user data." >&2
    }
else
    echo "WARNING: Manifest manager is missing; the current manifest was left untouched." >&2
fi

restore_or_remove() {
    local relative="$1" expected_hash="${2:-}" destination="$driver_root/$relative"
    local original="${state_dir:+$state_dir/original-files/$relative}"
    local absent="${state_dir:+$state_dir/absent-files/$relative}"

    if [[ -n "$original" && -f "$original" ]]; then
        mkdir -p "$(dirname -- "$destination")"
        cp -a -- "$original" "$destination"
        return 0
    fi
    if [[ -n "$absent" && -f "$absent" ]]; then
        if [[ -e "$destination" ]]; then
            if [[ -z "$expected_hash" || ! -f "$destination" || "$(sha256sum "$destination" | awk '{print $1}')" == "$expected_hash" ]]; then
                rm -f -- "$destination"
            else
                echo "Preserved modified bridge file: $destination" >&2
            fi
        fi
        return 0
    fi
    return 1
}

if [[ -n "$state_dir" && -f "$state_dir/installed-files.tsv" ]]; then
    while IFS=$'\t' read -r expected_hash relative; do
        [[ -n "$relative" ]] || continue
        [[ "$relative" == "driver.vrdrivermanifest" ]] && continue
        restore_or_remove "$relative" "$expected_hash" || true
    done <"$state_dir/installed-files.tsv"
else
    # Older bridge versions only created timestamped backups. The oldest
    # backup is the pre-bridge snapshot: files present there are restored;
    # bridge paths absent there are removed.
    backup_root="$state_root/backups"
    legacy_backup=""
    if [[ -d "$backup_root" ]]; then
        legacy_backup="$(find "$backup_root" -mindepth 1 -maxdepth 1 -type d -printf '%f\t%p\n' 2>/dev/null | sort | head -n 1 | cut -f2-)"
    fi
    fallback_files=(
        VERSION
        README-LINUX.md
        THIRD_PARTY_NOTICES.md
        bin/linux64/driver_standable.so
        bin/linux64/standable_dashboard_overlay
        bin/win64/standable_bridge_host.exe
        bin/win64/steam_api64.dll
        share/standable-linux-bridge/driver.vrdrivermanifest
        scripts/bridge-installer.sh
        scripts/bridge-manager.sh
        scripts/diagnose.sh
        scripts/enable-dashboard.sh
        scripts/find-steamvr.sh
        scripts/install.sh
        scripts/manifest-manager.sh
        scripts/repair.sh
        scripts/runtime-common.sh
        scripts/source-install.sh
        scripts/standable-bridge-launcher.sh
        scripts/uninstall.sh
        scripts/update.sh
        scripts/verify-artifacts.sh
    )
    for relative in "${fallback_files[@]}"; do
        destination="$driver_root/$relative"
        if [[ -n "$legacy_backup" && -f "$legacy_backup/$relative" ]]; then
            mkdir -p "$(dirname -- "$destination")"
            cp -a -- "$legacy_backup/$relative" "$destination"
        else
            rm -f -- "$destination"
        fi
    done
fi

rmdir "$driver_root/share/standable-linux-bridge" "$driver_root/share" 2>/dev/null || true
rmdir "$driver_root/bin/linux64" 2>/dev/null || true

if ((purge_state)); then
    keep_state=0
fi
if ((keep_state == 0)) && [[ -n "$state_dir" && -d "$state_dir" ]]; then
    rm -rf -- "$state_dir"
fi

echo "Standable Linux Bridge was removed."
echo "The original SteamVR manifest and any pre-existing files were restored."
