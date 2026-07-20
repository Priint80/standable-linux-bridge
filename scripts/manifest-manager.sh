#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: manifest-manager.sh COMMAND DRIVER_ROOT [TEMPLATE]

Commands:
  state-dir       Print the persistent state directory for this installation
  install         Back up the original manifest and install a Linux-safe manifest
  restore         Restore the exact original manifest
  driver-name     Print the preserved SteamVR driver name
EOF
}

command_name="${1:-}"
driver_root="${2:-}"
template="${3:-}"
[[ -n "$command_name" && -n "$driver_root" ]] || { usage >&2; exit 2; }
[[ -d "$driver_root" ]] || { echo "Standable folder does not exist: $driver_root" >&2; exit 2; }
driver_root="$(cd -- "$driver_root" && pwd -P)"

state_root="${XDG_STATE_HOME:-$HOME/.local/state}/standable-linux-bridge"
installation_id="$(printf '%s\0' "$driver_root" | sha256sum | awk '{print substr($1,1,24)}')"
state_dir="$state_root/installations/$installation_id"
original_manifest="$state_dir/original-driver.vrdrivermanifest"
managed_hash="$state_dir/managed-driver.sha256"
generated_binary="$state_dir/generated-native-binary"
manifest="$driver_root/driver.vrdrivermanifest"

atomic_copy() {
    local source="$1" destination="$2" mode="${3:-0644}" temporary
    mkdir -p "$(dirname -- "$destination")"
    temporary="$destination.bridge-new.$$"
    install -m "$mode" "$source" "$temporary"
    mv -f -- "$temporary" "$destination"
}

read_driver_name() {
    local source="$1"
    python3 - "$source" <<'PY'
import json
import re
import sys

try:
    with open(sys.argv[1], encoding="utf-8") as handle:
        document = json.load(handle)
except (OSError, json.JSONDecodeError):
    document = {}
name = document.get("name", "standable")
if not isinstance(name, str) or not re.fullmatch(r"[a-z0-9_.-]+", name):
    name = "standable"
print(name)
PY
}

case "$command_name" in
    state-dir)
        printf '%s\n' "$state_dir"
        ;;

    driver-name)
        source="$manifest"
        [[ -f "$original_manifest" ]] && source="$original_manifest"
        read_driver_name "$source"
        ;;

    install)
        command -v python3 >/dev/null 2>&1 || {
            echo "python3 is required to manage the SteamVR driver manifest" >&2
            exit 3
        }
        [[ -f "$manifest" ]] || {
            echo "The original driver.vrdrivermanifest is missing from the Standable folder" >&2
            exit 4
        }
        mkdir -p "$state_dir"
        printf '%s\n' "$driver_root" >"$state_dir/driver-root"
        if [[ ! -f "$original_manifest" ]]; then
            cp -a -- "$manifest" "$original_manifest"
        fi

        temporary="$(mktemp "$state_dir/manifest.XXXXXX")"
        trap 'rm -f -- "${temporary:-}"' EXIT
        python3 - "$original_manifest" "$template" "$temporary" <<'PY'
import json
import os
import re
import sys

original_path, template_path, output_path = sys.argv[1:]

def load(path):
    if not path or not os.path.isfile(path):
        return {}
    try:
        with open(path, encoding="utf-8") as handle:
            value = json.load(handle)
    except (OSError, json.JSONDecodeError):
        return {}
    return value if isinstance(value, dict) else {}

original = load(original_path)
fallback = load(template_path)
name = original.get("name", fallback.get("name", "standable"))
if not isinstance(name, str) or not re.fullmatch(r"[a-z0-9_.-]+", name):
    raise SystemExit("The original SteamVR driver name is invalid")

# Preserve every recognized front-end/presence field from the original manifest.
# Only the platform-hosting fields are normalized for the native Linux provider.
managed = dict(original or fallback)
managed["name"] = name
managed["directory"] = ""
managed["alwaysActivate"] = True
managed["resourceOnly"] = False

with open(output_path, "w", encoding="utf-8", newline="\n") as handle:
    json.dump(managed, handle, indent=2, ensure_ascii=False)
    handle.write("\n")
PY
        chmod 0644 "$temporary"
        atomic_copy "$temporary" "$manifest" 0644
        rm -f -- "$temporary"
        trap - EXIT

        driver_name="$(read_driver_name "$manifest")"
        standard_binary="$driver_root/bin/linux64/driver_standable.so"
        expected_binary="$driver_root/bin/linux64/driver_${driver_name}.so"
        if [[ "$expected_binary" != "$standard_binary" && ! -e "$expected_binary" ]]; then
            [[ -f "$standard_binary" ]] || {
                echo "Native driver binary is missing: $standard_binary" >&2
                exit 5
            }
            ln -s "$(basename -- "$standard_binary")" "$expected_binary"
            printf '%s\n' "$expected_binary" >"$generated_binary"
        fi
        sha256sum "$manifest" | awk '{print $1}' >"$managed_hash"
        printf '%s\n' "$driver_name"
        ;;

    restore)
        if [[ ! -f "$original_manifest" ]]; then
            echo "No original manifest backup exists for this installation; leaving the current manifest untouched" >&2
            exit 6
        fi
        atomic_copy "$original_manifest" "$manifest" 0644
        if [[ -f "$generated_binary" ]]; then
            read -r path <"$generated_binary" || true
            if [[ -n "${path:-}" && -L "$path" ]]; then
                rm -f -- "$path"
            fi
            rm -f -- "$generated_binary"
        fi
        rm -f -- "$managed_hash"
        ;;

    *)
        usage >&2
        exit 2
        ;;
esac
