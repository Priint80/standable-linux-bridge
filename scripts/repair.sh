#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
driver_root="$(cd -- "$script_dir/.." && pwd -P)"
manifest_manager="$script_dir/manifest-manager.sh"
repo="${STANDABLE_BRIDGE_REPO:-Priint80/standable-linux-bridge}"
branch="${STANDABLE_BRIDGE_BRANCH:-main}"
source_checkout="${STANDABLE_BRIDGE_SOURCE_CHECKOUT:-}"

while (($#)); do
    case "$1" in
        --standable-root) driver_root="${2:-}"; shift 2 ;;
        --repo) repo="${2:-}"; shift 2 ;;
        --branch) branch="${2:-}"; shift 2 ;;
        --source-checkout) source_checkout="${2:-}"; shift 2 ;;
        -h|--help)
            cat <<'EOF'
Usage: ./scripts/repair.sh [options]

Repair rebases the saved source checkout onto its corresponding remote branch,
builds a fresh overlay, uninstalls the current bridge, and installs that build.
When no source checkout is available, it reinstalls from the saved repository
and branch instead.
EOF
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

[[ -d "$driver_root" ]] || { echo "Standable folder does not exist: $driver_root" >&2; exit 2; }
driver_root="$(cd -- "$driver_root" && pwd -P)"

state_dir=""
if [[ -x "$manifest_manager" ]]; then
    state_dir="$(bash "$manifest_manager" state-dir "$driver_root")"
    metadata="$state_dir/metadata.env"
    if [[ -f "$metadata" ]]; then
        source "$metadata"
        repo="${STANDABLE_BRIDGE_REPO:-$repo}"
        branch="${STANDABLE_BRIDGE_BRANCH:-$branch}"
        source_checkout="${STANDABLE_BRIDGE_SOURCE_CHECKOUT:-$source_checkout}"
    fi
fi

[[ "$branch" =~ ^[A-Za-z0-9._/-]+$ && "$branch" != *..* ]] || {
    echo "Saved branch name is invalid: $branch" >&2
    exit 3
}

temporary="$(mktemp -d "${TMPDIR:-/tmp}/standable-bridge-repair.XXXXXX")"
cleanup() { rm -rf -- "$temporary"; }
trap cleanup EXIT
cp -a -- "$script_dir/uninstall.sh" "$temporary/uninstall.sh"
cp -a -- "$script_dir/bridge-installer.sh" "$temporary/bridge-installer.sh"
chmod 0755 "$temporary"/*.sh

if [[ -n "$source_checkout" && -d "$source_checkout/.git" ]]; then
    source_checkout="$(cd -- "$source_checkout" && pwd -P)"
    current_branch="$(git -C "$source_checkout" branch --show-current)"
    [[ -n "$current_branch" ]] || {
        echo "The saved source checkout is in detached-HEAD state." >&2
        exit 4
    }
    if [[ "$current_branch" != "$branch" ]]; then
        echo "Saved repair branch is $branch, but the checkout is currently on $current_branch." >&2
        echo "Switch the checkout to the intended branch before repairing." >&2
        exit 4
    fi

    echo "Refreshing source checkout from origin/$branch"
    git -C "$source_checkout" fetch --prune origin "$branch"
    git -C "$source_checkout" rebase --autostash "origin/$branch"

    echo "Building a fresh bridge overlay"
    make -C "$source_checkout" overlay
    overlay="$source_checkout/build/overlay"
    [[ -f "$overlay/VERSION" ]] || { echo "Fresh overlay build is incomplete" >&2; exit 5; }

    bash "$temporary/uninstall.sh" --standable-root "$driver_root" --keep-state
    bash "$source_checkout/install.sh" \
        --standable-root "$driver_root" \
        --overlay-dir "$overlay" \
        --repo "$repo" \
        --branch "$branch" \
        --source-checkout "$source_checkout"
else
    echo "No source checkout is recorded; reinstalling from $repo branch $branch"
    bash "$temporary/uninstall.sh" --standable-root "$driver_root" --keep-state
    bash "$temporary/bridge-installer.sh" \
        --standable-root "$driver_root" \
        --repo "$repo" \
        --branch "$branch" \
        --update
fi

echo "Standable Linux Bridge repair completed."
