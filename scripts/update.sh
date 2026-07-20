#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
driver_root="$(cd -- "$script_dir/.." && pwd -P)"
installer="$script_dir/bridge-installer.sh"
manifest_manager="$script_dir/manifest-manager.sh"
repo="${STANDABLE_BRIDGE_REPO:-Priint80/standable-linux-bridge}"
branch="${STANDABLE_BRIDGE_BRANCH:-main}"
source_checkout="${STANDABLE_BRIDGE_SOURCE_CHECKOUT:-}"

if [[ -x "$manifest_manager" ]]; then
    state_dir="$(bash "$manifest_manager" state-dir "$driver_root")"
    metadata="$state_dir/metadata.env"
    if [[ -f "$metadata" ]]; then
        # Written by scripts/install.sh with shell-escaped values only.
        source "$metadata"
        repo="${STANDABLE_BRIDGE_REPO:-$repo}"
        branch="${STANDABLE_BRIDGE_BRANCH:-$branch}"
        source_checkout="${STANDABLE_BRIDGE_SOURCE_CHECKOUT:-$source_checkout}"
    fi
fi

export STANDABLE_BRIDGE_REPO="$repo"
export STANDABLE_BRIDGE_BRANCH="$branch"
export STANDABLE_BRIDGE_SOURCE_CHECKOUT="$source_checkout"

if [[ -x "$installer" ]]; then
    # The packaged installer accepts repository selection but keeps branch and
    # checkout provenance through the environment for the registration step.
    exec "$installer" --update --standable-root "$driver_root" --repo "$repo" "$@"
fi

source_installer="$driver_root/install.sh"
if [[ -x "$source_installer" ]]; then
    echo "Source checkout detected; using its installer."
    exec "$source_installer" --update --standable-root "$driver_root" --repo "$repo" "$@"
fi

echo "Missing updater engine: $installer" >&2
echo "Run ./install.sh from a source checkout, or reinstall the bridge." >&2
exit 2
