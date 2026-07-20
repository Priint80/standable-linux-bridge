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
If no persistent checkout is recorded, Repair clones the saved branch into a
temporary source tree and performs the same clean rebuild.
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
        # Written by scripts/install.sh with shell-escaped values only.
        source "$metadata"
        repo="${STANDABLE_BRIDGE_REPO:-$repo}"
        branch="${STANDABLE_BRIDGE_BRANCH:-$branch}"
        source_checkout="${STANDABLE_BRIDGE_SOURCE_CHECKOUT:-$source_checkout}"
    fi
fi

[[ "$repo" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]] || {
    echo "Saved repository is invalid: $repo" >&2
    exit 3
}
[[ "$branch" =~ ^[A-Za-z0-9._/-]+$ && "$branch" != *..* && "$branch" != /* && "$branch" != */ ]] || {
    echo "Saved branch name is invalid: $branch" >&2
    exit 3
}
for command in git make bash; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "Repair requires $command." >&2
        exit 3
    }
done

temporary="$(mktemp -d "${TMPDIR:-/tmp}/standable-bridge-repair.XXXXXX")"
cleanup() { rm -rf -- "$temporary"; }
trap cleanup EXIT
cp -a -- "$script_dir/uninstall.sh" "$temporary/uninstall.sh"
chmod 0755 "$temporary/uninstall.sh"

persistent_checkout=0
if [[ -n "$source_checkout" && -d "$source_checkout/.git" ]]; then
    persistent_checkout=1
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
else
    source_checkout="$temporary/source"
    echo "Cloning $repo branch $branch for repair"
    git clone --depth 1 --single-branch --branch "$branch" \
        "https://github.com/$repo.git" "$source_checkout"
fi

echo "Building a fresh bridge overlay"
make -C "$source_checkout" overlay
overlay="$source_checkout/build/overlay"
[[ -f "$overlay/VERSION" && -x "$overlay/scripts/install.sh" ]] || {
    echo "Fresh overlay build is incomplete" >&2
    exit 5
}

bash "$temporary/uninstall.sh" --standable-root "$driver_root" --keep-state

export STANDABLE_BRIDGE_REPO="$repo"
export STANDABLE_BRIDGE_BRANCH="$branch"
if ((persistent_checkout)); then
    export STANDABLE_BRIDGE_SOURCE_CHECKOUT="$source_checkout"
else
    # Do not persist a temporary path. The branch remains recorded and the next
    # Repair can create another clean temporary checkout.
    export STANDABLE_BRIDGE_SOURCE_CHECKOUT=""
fi
bash "$source_checkout/install.sh" \
    --standable-root "$driver_root" \
    --overlay-dir "$overlay" \
    --repo "$repo"

echo "Standable Linux Bridge repair completed."
