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

command -v make >/dev/null 2>&1 || {
    echo "A source installation requires GNU make." >&2
    exit 3
}
[[ -x "$source_root/install.sh" && -f "$source_root/Makefile" ]] || {
    echo "This script must be run from a Standable Linux Bridge source checkout." >&2
    exit 3
}

branch="${STANDABLE_BRIDGE_BRANCH:-}"
commit="${STANDABLE_BRIDGE_COMMIT:-}"
if command -v git >/dev/null 2>&1 && [[ -d "$source_root/.git" ]]; then
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
exec "$source_root/install.sh" "${arguments[@]}"
