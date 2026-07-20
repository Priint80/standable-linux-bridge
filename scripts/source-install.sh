#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
source_root="$(cd -- "$script_dir/.." && pwd -P)"
standable_root=""
declare -a passthrough=()

while (($#)); do
    case "$1" in
        --standable-root)
            standable_root="${2:-}"
            shift 2
            ;;
        -h|--help)
            cat <<'EOF'
Usage: ./scripts/source-install.sh [--standable-root PATH] [installer options]

Builds a fresh overlay from the current source checkout and installs that exact
build. This avoids accidentally installing the older bundled release ZIP while
developing on a branch.
EOF
            exit 0
            ;;
        *)
            passthrough+=("$1")
            shift
            ;;
    esac
done

[[ -f "$source_root/install.sh" && -f "$source_root/Makefile" ]] || {
    echo "This script must be run from a Standable Linux Bridge source checkout." >&2
    exit 3
}

# Fail before starting a partial build and give the graphical manager a short,
# useful message instead of exposing a late '/bin/bash: zig: command not found'.
required_commands=(make g++ zig curl git strip install python3 sha256sum)
declare -a missing_commands=()
for command_name in "${required_commands[@]}"; do
    command -v "$command_name" >/dev/null 2>&1 || missing_commands+=("$command_name")
done
if ((${#missing_commands[@]})); then
    printf 'Missing source-build dependencies: %s\n' "${missing_commands[*]}" >&2
    if command -v pacman >/dev/null 2>&1; then
        cat >&2 <<'EOF'
Install the required Arch/CachyOS packages with:
  sudo pacman -S --needed base-devel zig curl git python
Then run Install, Update, or Repair again.
EOF
    elif command -v apt-get >/dev/null 2>&1; then
        cat >&2 <<'EOF'
Install the required compiler tools, Zig, curl, Git, and Python 3 with your package manager, then retry.
EOF
    else
        echo "Install the missing commands with your distribution's package manager, then retry." >&2
    fi
    exit 3
fi

branch="${STANDABLE_BRIDGE_BRANCH:-}"
commit="${STANDABLE_BRIDGE_COMMIT:-}"
if [[ -d "$source_root/.git" ]]; then
    [[ -n "$branch" ]] || branch="$(git -C "$source_root" branch --show-current 2>/dev/null || true)"
    [[ -n "$commit" ]] || commit="$(git -C "$source_root" rev-parse HEAD 2>/dev/null || true)"
fi
export STANDABLE_BRIDGE_SOURCE_CHECKOUT="$source_root"
[[ -n "$branch" ]] && export STANDABLE_BRIDGE_BRANCH="$branch"
[[ -n "$commit" ]] && export STANDABLE_BRIDGE_COMMIT="$commit"

echo "Building the current bridge checkout"
make -C "$source_root" overlay
overlay="$source_root/build/overlay"
[[ -f "$overlay/VERSION" && -x "$overlay/scripts/install.sh" ]] || {
    echo "The source overlay build is incomplete." >&2
    exit 4
}

arguments=(--overlay-dir "$overlay")
[[ -n "$standable_root" ]] && arguments+=(--standable-root "$standable_root")
arguments+=("${passthrough[@]}")
exec bash "$source_root/install.sh" "${arguments[@]}"
