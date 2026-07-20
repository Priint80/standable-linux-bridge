#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
driver_root="$(cd -- "$script_dir/.." && pwd -P)"
installer="$script_dir/bridge-installer.sh"
source_builder="$script_dir/source-install.sh"
source_installer="$driver_root/install.sh"
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
    # Installed copy: this parent is the actual Standable folder and the
    # packaged updater selects the current release distribution.
    exec "$installer" --update --standable-root "$driver_root" --repo "$repo" "$@"
fi

if [[ -x "$source_builder" && -f "$driver_root/Makefile" ]]; then
    # Full source checkout: build the current files instead of selecting the
    # bundled release ZIP, which may share a version number but contain older binaries.
    echo "Source checkout detected; building its current overlay."
    exec "$source_builder" --update --repo "$repo" "$@"
fi

if [[ -x "$source_installer" ]]; then
    # Compatibility fallback for an older or intentionally minimal source
    # checkout that does not contain the source-build wrapper yet.
    echo "Legacy source checkout detected; using its packaged installer."
    exec "$source_installer" --update --repo "$repo" "$@"
fi

echo "Missing updater engine: $installer" >&2
echo "Run ./scripts/source-install.sh from a source checkout, or reinstall the bridge." >&2
exit 2
