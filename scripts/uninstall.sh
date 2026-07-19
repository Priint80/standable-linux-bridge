#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
driver_root="$(cd -- "$script_dir/.." && pwd -P)"
steamvr_root="$(bash "$script_dir/find-steamvr.sh")"

"$steamvr_root/bin/vrpathreg.sh" removedriver "$driver_root"
echo "Unregistered: $driver_root"
echo "No files or Standable settings were deleted."
