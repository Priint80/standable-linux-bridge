#!/usr/bin/env bash

standable_find_primary_steam_root() {
    local candidate
    for candidate in \
        "${STEAM_ROOT:-}" \
        "$HOME/.local/share/Steam" \
        "$HOME/.steam/steam" \
        "$HOME/.steam/root" \
        "$HOME/.var/app/com.valvesoftware.Steam/data/Steam"; do
        [[ -n "$candidate" && -d "$candidate/steamapps" ]] || continue
        (cd -- "$candidate" && pwd -P)
        return 0
    done
    return 1
}

standable_find_steamclient64() {
    local data_root="$1"
    local candidate runner_root
    runner_root="$(cd -- "$(dirname -- "$STANDABLE_RUNNER_PATH")" && pwd -P)"
    for candidate in \
        "$STANDABLE_STEAM_CLIENT_ROOT/legacycompat/steamclient64.dll" \
        "$runner_root/steamclient64.dll" \
        "$runner_root/files/share/default_pfx/drive_c/Program Files (x86)/Steam/steamclient64.dll" \
        "$data_root/compatdata/2370570/pfx/drive_c/Program Files (x86)/Steam/steamclient64.dll" \
        "$data_root/compatdata/2370570/pfx/drive_c/Program Files/Steam/steamclient64.dll" \
        "$data_root/wineprefix/drive_c/Program Files (x86)/Steam/steamclient64.dll"; do
        if [[ -f "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

standable_runner_fingerprint() {
    local digest
    digest="$(sha256sum "$STANDABLE_RUNNER_PATH" | awk '{print $1}')"
    printf '%s\n' "${STANDABLE_RUNNER_KIND}-${digest:0:16}"
}

standable_rotate_log() {
    local path="$1" max_bytes="${2:-5242880}" size=0
    [[ -f "$path" ]] || return 0
    size="$(stat -c '%s' "$path" 2>/dev/null || printf '0')"
    if [[ "$size" =~ ^[0-9]+$ ]] && ((size > max_bytes)); then
        mv -f -- "$path" "$path.1"
    fi
}

standable_select_runner() {
    local steamvr_root="$1"
    local inferred_library primary_root candidate root
    inferred_library="$(dirname -- "$(dirname -- "$(dirname -- "$steamvr_root")")")"
    primary_root="$(standable_find_primary_steam_root 2>/dev/null || true)"

    STANDABLE_RUNNER_KIND=""
    STANDABLE_RUNNER_PATH=""
    STANDABLE_STEAM_CLIENT_ROOT="${primary_root:-$inferred_library}"

    if [[ -n "${STANDABLE_PROTON:-}" ]]; then
        [[ -x "$STANDABLE_PROTON" ]] || {
            echo "STANDABLE_PROTON is not executable: $STANDABLE_PROTON" >&2
            return 1
        }
        STANDABLE_RUNNER_KIND="proton"
        STANDABLE_RUNNER_PATH="$STANDABLE_PROTON"
        return 0
    fi

    declare -a roots=()
    [[ -n "$primary_root" ]] && roots+=("$primary_root")
    [[ "$inferred_library" != "$primary_root" ]] && roots+=("$inferred_library")

    for root in "${roots[@]}"; do
        for candidate in \
            "$root/steamapps/common/Proton - Experimental/proton" \
            "$root/steamapps/common/Proton Hotfix/proton"; do
            if [[ -x "$candidate" ]]; then
                STANDABLE_RUNNER_KIND="proton"
                STANDABLE_RUNNER_PATH="$candidate"
                return 0
            fi
        done
    done

    for root in "${roots[@]}"; do
        while IFS= read -r candidate; do
            if [[ -x "$candidate" ]]; then
                STANDABLE_RUNNER_KIND="proton"
                STANDABLE_RUNNER_PATH="$candidate"
                return 0
            fi
        done < <(
            {
                find "$root/compatibilitytools.d" -mindepth 2 -maxdepth 2 -type f -name proton -print 2>/dev/null
                find "$root/steamapps/common" -mindepth 2 -maxdepth 2 -type f -path '*/Proton*/proton' -print 2>/dev/null
            } | sort -Vr
        )
    done

    if [[ -n "${STANDABLE_WINE:-}" ]]; then
        if [[ -x "$STANDABLE_WINE" ]]; then
            candidate="$STANDABLE_WINE"
        else
            candidate="$(command -v -- "$STANDABLE_WINE" 2>/dev/null || true)"
        fi
        [[ -n "$candidate" ]] || {
            echo "STANDABLE_WINE was not found: $STANDABLE_WINE" >&2
            return 1
        }
        STANDABLE_RUNNER_KIND="wine"
        STANDABLE_RUNNER_PATH="$candidate"
        return 0
    fi

    for candidate in wine64 wine; do
        if command -v "$candidate" >/dev/null 2>&1; then
            STANDABLE_RUNNER_KIND="wine"
            STANDABLE_RUNNER_PATH="$(command -v "$candidate")"
            return 0
        fi
    done

    echo "No Proton or Wine runner was found. Install Proton in Steam or set STANDABLE_PROTON." >&2
    return 1
}

standable_configure_runner() {
    local data_root="$1"
    local driver_root="$2"

    mkdir -p "$data_root"
    export SteamAppId=2370570
    export SteamGameId=2370570
    export STEAM_COMPAT_APP_ID=2370570
    export WINEDEBUG="${WINEDEBUG:--all}"
    export PROTON_LOG="${PROTON_LOG:-0}"

    if [[ "$STANDABLE_RUNNER_KIND" == "proton" ]]; then
        export STEAM_COMPAT_CLIENT_INSTALL_PATH="$STANDABLE_STEAM_CLIENT_ROOT"
        export STEAM_COMPAT_DATA_PATH="$data_root/compatdata/2370570"
        export STEAM_COMPAT_INSTALL_PATH="$driver_root"
        mkdir -p "$STEAM_COMPAT_DATA_PATH"
        STANDABLE_RUN_COMMAND=("$STANDABLE_RUNNER_PATH" run)
    else
        export WINEPREFIX="$data_root/wineprefix"
        mkdir -p "$WINEPREFIX"
        STANDABLE_RUN_COMMAND=("$STANDABLE_RUNNER_PATH")
    fi
}
