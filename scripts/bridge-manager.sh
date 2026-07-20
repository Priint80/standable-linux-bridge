#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
command -v python3 >/dev/null 2>&1 || {
    echo "Python 3 is required for the graphical Standable Linux Bridge manager." >&2
    exit 2
}
if ! python3 -c 'import tkinter' >/dev/null 2>&1; then
    echo "Python Tk support is not installed. Use install.sh, update.sh, repair.sh, or uninstall.sh directly." >&2
    exit 3
fi
exec python3 "$script_dir/bridge-manager.py" "$@"
