#!/usr/bin/env bash
set -euo pipefail

declare -a steam_roots=()
declare -a candidates=()

[[ -n "${STEAM_ROOT:-}" ]] && steam_roots+=("$STEAM_ROOT")
steam_roots+=(
    "$HOME/.local/share/Steam"
    "$HOME/.steam/steam"
    "$HOME/.steam/root"
    "$HOME/.var/app/com.valvesoftware.Steam/data/Steam"
)

[[ -n "${STEAMVR_ROOT:-}" ]] && candidates+=("$STEAMVR_ROOT")
for root in "${steam_roots[@]}"; do
    candidates+=("$root/steamapps/common/SteamVR")
    library_file="$root/steamapps/libraryfolders.vdf"
    [[ -f "$library_file" ]] || continue
    while IFS= read -r library; do
        [[ -n "$library" ]] && candidates+=("$library/steamapps/common/SteamVR")
    done < <(sed -nE 's/^[[:space:]]*"path"[[:space:]]*"([^"]+)".*/\1/p' "$library_file")
done

declare -A visited=()
for candidate in "${candidates[@]}"; do
    [[ -n "$candidate" ]] || continue
    [[ -z "${visited[$candidate]:-}" ]] || continue
    visited[$candidate]=1
    if [[ -x "$candidate/bin/vrpathreg.sh" ]]; then
        (cd -- "$candidate" && pwd -P)
        exit 0
    fi
done

echo "SteamVR was not found. Set STEAMVR_ROOT to its installation directory." >&2
exit 1
