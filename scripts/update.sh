#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
driver_root="$(cd -- "$script_dir/.." && pwd -P)"
installer="$script_dir/bridge-installer.sh"

if [[ -x "$installer" ]]; then
    exec "$installer" --update --standable-root "$driver_root" "$@"
fi

source_installer="$driver_root/install.sh"
if [[ -x "$source_installer" ]]; then
    echo "Source checkout detected; using its installer."
    exec "$source_installer" --update "$@"
fi

echo "Missing updater engine: $installer" >&2
echo "Run ./install.sh from a source checkout, or run this script from the installed Standable folder." >&2
exit 2
