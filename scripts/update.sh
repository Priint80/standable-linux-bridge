#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
driver_root="$(cd -- "$script_dir/.." && pwd -P)"
installer="$script_dir/bridge-installer.sh"

[[ -x "$installer" ]] || {
    echo "Missing executable updater engine: $installer" >&2
    exit 2
}

exec "$installer" --update --standable-root "$driver_root" "$@"
