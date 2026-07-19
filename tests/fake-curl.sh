#!/usr/bin/env bash
set -euo pipefail

output=""
url=""
while (($#)); do
    case "$1" in
        --output) output="${2:-}"; shift 2 ;;
        -H|--header) shift 2 ;;
        --fail|--location|--silent|--show-error) shift ;;
        *) url="$1"; shift ;;
    esac
done

[[ -n "$output" && -n "$url" && -n "${STANDABLE_TEST_DIST:-}" ]] || exit 2
printf '%s\n' "$url" >>"${STANDABLE_TEST_CURL_LOG:?}"

case "$url" in
    https://github.com/*/releases/latest/download/*)
        exit 22
        ;;
    https://raw.githubusercontent.com/*/main/dist/Standable-Linux-Bridge-Overlay.zip)
        cp "$STANDABLE_TEST_DIST/Standable-Linux-Bridge-Overlay.zip" "$output"
        ;;
    https://raw.githubusercontent.com/*/main/dist/SHA256SUMS)
        cp "$STANDABLE_TEST_DIST/SHA256SUMS" "$output"
        ;;
    *)
        exit 23
        ;;
esac
