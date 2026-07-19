#!/usr/bin/env bash
set -euo pipefail

{
    printf 'cwd=%s app=%s compat=%s vr_runtime=%s vr_override=%s vr_paths=%s args=' \
        "$PWD" "${SteamAppId:-}" "${STEAM_COMPAT_DATA_PATH:-}" \
        "${PROTON_VR_RUNTIME:-}" "${VR_OVERRIDE:-}" "${VR_PATHREG_OVERRIDE:-}"
    printf '<%s>' "$@"
    printf '\n'
} >>"${STANDABLE_TEST_LOG:?}"
