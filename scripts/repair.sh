#!/usr/bin/env bash
set -euo pipefail

repair_script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
driver_root="$(cd -- "$repair_script_dir/.." && pwd -P)"
repo="${STANDABLE_BRIDGE_REPO:-Priint80/standable-linux-bridge}"
branch="${STANDABLE_BRIDGE_BRANCH:-main}"
source_checkout="${STANDABLE_BRIDGE_SOURCE_CHECKOUT:-}"
repo_explicit=0
branch_explicit=0
source_explicit=0

while (($#)); do
    case "$1" in
        --standable-root)
            driver_root="${2:-}"
            shift 2
            ;;
        --repo)
            repo="${2:-}"
            repo_explicit=1
            shift 2
            ;;
        --branch)
            branch="${2:-}"
            branch_explicit=1
            shift 2
            ;;
        --source-checkout)
            source_checkout="${2:-}"
            source_explicit=1
            shift 2
            ;;
        -h|--help)
            cat <<'EOF'
Usage: ./scripts/repair.sh [options]

Repair rebases the saved source checkout onto its corresponding remote branch,
builds a fresh overlay, uninstalls the current bridge, and installs that build.
If no persistent checkout is recorded, Repair clones the saved branch into a
temporary source tree and performs the same clean rebuild.

Explicit --repo, --branch, and --source-checkout options override saved install
metadata for this repair.
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
driver_scripts="$driver_root/scripts"
manifest_manager="$driver_scripts/manifest-manager.sh"
[[ -x "$manifest_manager" ]] || manifest_manager="$repair_script_dir/manifest-manager.sh"
uninstall_script="$driver_scripts/uninstall.sh"
[[ -x "$uninstall_script" ]] || uninstall_script="$repair_script_dir/uninstall.sh"

metadata_value() {
    local file="$1" key="$2"
    python3 - "$file" "$key" <<'PY'
import json
import sys

try:
    with open(sys.argv[1], encoding="utf-8") as handle:
        document = json.load(handle)
except (OSError, json.JSONDecodeError):
    raise SystemExit(0)
value = document.get(sys.argv[2], "") if isinstance(document, dict) else ""
if isinstance(value, str):
    print(value, end="")
PY
}

state_dir=""
if [[ -x "$manifest_manager" ]]; then
    state_dir="$(bash "$manifest_manager" state-dir "$driver_root")"
    metadata="$state_dir/metadata.json"
    if [[ -f "$metadata" ]]; then
        saved_repo="$(metadata_value "$metadata" repository)"
        saved_branch="$(metadata_value "$metadata" branch)"
        saved_checkout="$(metadata_value "$metadata" source_checkout)"
        if ((repo_explicit == 0)) && [[ -n "$saved_repo" ]]; then
            repo="$saved_repo"
        fi
        if ((branch_explicit == 0)) && [[ -n "$saved_branch" ]]; then
            branch="$saved_branch"
        fi
        if ((source_explicit == 0)); then
            source_checkout="$saved_checkout"
        fi
    fi
fi

[[ "$repo" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]] || {
    echo "Repair repository is invalid: $repo" >&2
    exit 3
}
[[ "$branch" =~ ^[A-Za-z0-9._/-]+$ && "$branch" != *..* && "$branch" != /* && "$branch" != */ ]] || {
    echo "Repair branch name is invalid: $branch" >&2
    exit 3
}

required_commands=(bash git make g++ zig curl strip install python3 sha256sum)
declare -a missing_commands=()
for command_name in "${required_commands[@]}"; do
    command -v "$command_name" >/dev/null 2>&1 || missing_commands+=("$command_name")
done
if ((${#missing_commands[@]})); then
    printf 'Missing repair/build dependencies: %s\n' "${missing_commands[*]}" >&2
    if command -v pacman >/dev/null 2>&1; then
        cat >&2 <<'EOF'
Install the required Arch/CachyOS packages with:
  sudo pacman -S --needed base-devel zig curl git python
Then run Repair again.
EOF
    else
        echo "Install the missing commands with your distribution's package manager, then run Repair again." >&2
    fi
    exit 3
fi

[[ -f "$uninstall_script" ]] || {
    echo "No compatible uninstall script is available for repair." >&2
    exit 4
}

temporary="$(mktemp -d "${TMPDIR:-/tmp}/standable-bridge-repair.XXXXXX")"
cleanup() { rm -rf -- "$temporary"; }
trap cleanup EXIT
cp -a -- "$uninstall_script" "$temporary/uninstall.sh"
chmod 0755 "$temporary/uninstall.sh"

persistent_checkout=0
if [[ -n "$source_checkout" && -d "$source_checkout/.git" ]]; then
    persistent_checkout=1
    source_checkout="$(cd -- "$source_checkout" && pwd -P)"
    current_branch="$(git -C "$source_checkout" branch --show-current)"
    [[ -n "$current_branch" ]] || {
        echo "The selected source checkout is in detached-HEAD state." >&2
        exit 4
    }
    if [[ "$current_branch" != "$branch" ]]; then
        echo "Repair branch is $branch, but the checkout is currently on $current_branch." >&2
        echo "Switch the checkout to the intended branch or pass the matching --branch." >&2
        exit 4
    fi

    echo "Refreshing source checkout from origin/$branch"
    git -C "$source_checkout" fetch --prune origin "$branch"
    git -C "$source_checkout" rebase --autostash "origin/$branch"
else
    if [[ -n "$source_checkout" && $source_explicit -eq 1 ]]; then
        echo "The selected source checkout is not a Git repository: $source_checkout" >&2
        exit 4
    fi
    source_checkout="$temporary/source"
    echo "Cloning $repo branch $branch for repair"
    git clone --depth 1 --single-branch --branch "$branch" \
        "https://github.com/$repo.git" "$source_checkout"
fi

commit="$(git -C "$source_checkout" rev-parse HEAD 2>/dev/null || true)"

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
[[ -n "$commit" ]] && export STANDABLE_BRIDGE_COMMIT="$commit"
if ((persistent_checkout)); then
    export STANDABLE_BRIDGE_SOURCE_CHECKOUT="$source_checkout"
else
    # Do not persist a temporary path. The branch and exact commit remain
    # recorded, and the next Repair can create another clean temporary checkout.
    export STANDABLE_BRIDGE_SOURCE_CHECKOUT=""
fi
bash "$source_checkout/install.sh" \
    --standable-root "$driver_root" \
    --overlay-dir "$overlay" \
    --repo "$repo"

echo "Standable Linux Bridge repair completed."
