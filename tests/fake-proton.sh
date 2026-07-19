#!/usr/bin/env bash
set -euo pipefail

{
    printf 'cwd=%s app=%s compat=%s args=' "$PWD" "${SteamAppId:-}" "${STEAM_COMPAT_DATA_PATH:-}"
    printf '<%s>' "$@"
    printf '\n'
} >>"${STANDABLE_TEST_LOG:?}"
