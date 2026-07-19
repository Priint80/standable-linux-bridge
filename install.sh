#!/usr/bin/env bash
set -euo pipefail

repo="${STANDABLE_BRIDGE_REPO:-Priint80/standable-linux-bridge}"
standable_root="${STANDABLE_ROOT:-}"
overlay_dir=""
update_mode=0
register_driver=1
installer_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"

usage() {
    cat <<'EOF'
Usage: ./install.sh [options]

Options:
  --standable-root PATH  Original "Standable Full Body Estimation" folder
  --overlay-dir PATH     Install an already-extracted bridge overlay
  --repo OWNER/REPO      GitHub repository used for downloads
  --update               Label this operation as an update
  --no-register          Install files without registering the SteamVR driver
  -h, --help             Show this help

For a private GitHub repository, authenticate with `gh auth login` first or
provide GITHUB_TOKEN. Public releases require only curl, unzip, and sha256sum.
EOF
}

while (($#)); do
    case "$1" in
        --standable-root) standable_root="${2:-}"; shift 2 ;;
        --overlay-dir) overlay_dir="${2:-}"; shift 2 ;;
        --repo) repo="${2:-}"; shift 2 ;;
        --update) update_mode=1; shift ;;
        --no-register) register_driver=0; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

for command in bash install sha256sum; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "Missing required command: $command" >&2
        exit 3
    }
done

is_standable_root() {
    local candidate="$1"
    [[ -f "$candidate/driver.vrdrivermanifest" && \
       -f "$candidate/Standable.exe" && \
       -f "$candidate/bin/win64/driver_standable.dll" ]]
}

add_steam_root() {
    local candidate="$1" existing
    [[ -n "$candidate" && -d "$candidate/steamapps" ]] || return 0
    for existing in "${steam_roots[@]:-}"; do
        [[ "$existing" == "$candidate" ]] && return 0
    done
    steam_roots+=("$candidate")
}

discover_standable_root() {
    local candidate vdf line path checkout_parent
    checkout_parent="$(cd -- "$installer_root/.." && pwd -P)"
    for candidate in "$PWD" "$installer_root" "$checkout_parent"; do
        if is_standable_root "$candidate"; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done

    declare -a steam_roots=()
    for candidate in \
        "${STEAM_ROOT:-}" \
        "$HOME/.local/share/Steam" \
        "$HOME/.steam/steam" \
        "$HOME/.steam/root" \
        "$HOME/.var/app/com.valvesoftware.Steam/data/Steam"; do
        add_steam_root "$candidate"
    done

    for vdf in \
        "$HOME/.local/share/Steam/steamapps/libraryfolders.vdf" \
        "$HOME/.steam/steam/steamapps/libraryfolders.vdf" \
        "$HOME/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/libraryfolders.vdf"; do
        [[ -f "$vdf" ]] || continue
        while IFS= read -r line; do
            if [[ "$line" =~ \"path\"[[:space:]]+\"([^\"]+)\" ]]; then
                path="${BASH_REMATCH[1]}"
                path="${path//\\\\/\\}"
                add_steam_root "$path"
            fi
        done <"$vdf"
    done

    for candidate in "${steam_roots[@]:-}"; do
        path="$candidate/steamapps/common/Standable Full Body Estimation"
        if is_standable_root "$path"; then
            printf '%s\n' "$path"
            return 0
        fi
    done
    return 1
}

if [[ -n "$standable_root" ]]; then
    [[ -d "$standable_root" ]] || { echo "Standable folder does not exist: $standable_root" >&2; exit 4; }
    standable_root="$(cd -- "$standable_root" && pwd -P)"
else
    standable_root="$(discover_standable_root || true)"
fi
if [[ -z "$standable_root" ]] || ! is_standable_root "$standable_root"; then
    echo "Could not locate the original Standable Steam folder." >&2
    echo "Re-run with: --standable-root '/path/to/Standable Full Body Estimation'" >&2
    exit 4
fi

temporary=""
cleanup() {
    if [[ -n "$temporary" && -d "$temporary" ]]; then
        rm -rf -- "$temporary"
    fi
    return 0
}
trap cleanup EXIT

download_public_asset() {
    local name="$1" output="$2"
    curl --fail --location --silent --show-error \
        --output "$output" \
        "https://github.com/$repo/releases/latest/download/$name"
}

download_private_asset() {
    local name="$1" output="$2" metadata asset_url
    command -v python3 >/dev/null 2>&1 || {
        echo "python3 is required when downloading a private release with GITHUB_TOKEN" >&2
        return 1
    }
    metadata="$temporary/latest-release.json"
    curl --fail --location --silent --show-error \
        -H "Authorization: Bearer $GITHUB_TOKEN" \
        -H "Accept: application/vnd.github+json" \
        --output "$metadata" \
        "https://api.github.com/repos/$repo/releases/latest"
    asset_url="$(python3 - "$metadata" "$name" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    release = json.load(handle)
for asset in release.get("assets", []):
    if asset.get("name") == sys.argv[2]:
        print(asset["url"])
        break
PY
)"
    [[ -n "$asset_url" ]] || { echo "Release asset not found: $name" >&2; return 1; }
    curl --fail --location --silent --show-error \
        -H "Authorization: Bearer $GITHUB_TOKEN" \
        -H "Accept: application/octet-stream" \
        --output "$output" "$asset_url"
}

download_release_asset() {
    local name="$1" output="$2"
    if command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1; then
        gh release download --repo "$repo" --pattern "$name" --output "$output" --clobber
    elif [[ -n "${GITHUB_TOKEN:-}" ]]; then
        download_private_asset "$name" "$output"
    else
        download_public_asset "$name" "$output"
    fi
}

download_repository_asset() {
    local name="$1" output="$2"
    if command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1; then
        gh api \
            -H "Accept: application/vnd.github.raw+json" \
            "repos/$repo/contents/dist/$name?ref=main" >"$output"
    elif [[ -n "${GITHUB_TOKEN:-}" ]]; then
        curl --fail --location --silent --show-error \
            -H "Authorization: Bearer $GITHUB_TOKEN" \
            -H "Accept: application/vnd.github.raw+json" \
            --output "$output" \
            "https://api.github.com/repos/$repo/contents/dist/$name?ref=main"
    else
        curl --fail --location --silent --show-error \
            --output "$output" \
            "https://raw.githubusercontent.com/$repo/main/dist/$name"
    fi
}

if [[ -n "$overlay_dir" ]]; then
    [[ -d "$overlay_dir" ]] || { echo "Overlay directory does not exist: $overlay_dir" >&2; exit 5; }
    overlay_dir="$(cd -- "$overlay_dir" && pwd -P)"
else
    for command in curl unzip; do
        command -v "$command" >/dev/null 2>&1 || {
            echo "Missing required command for release download: $command" >&2
            exit 3
        }
    done
    temporary="$(mktemp -d "${TMPDIR:-/tmp}/standable-linux-bridge.XXXXXX")"
    archive="$temporary/Standable-Linux-Bridge-Overlay.zip"
    checksums="$temporary/SHA256SUMS"
    if [[ -f "$installer_root/dist/Standable-Linux-Bridge-Overlay.zip" && \
          -f "$installer_root/dist/SHA256SUMS" ]]; then
        echo "Using the verified bridge distribution bundled with this checkout"
        install -m 0644 "$installer_root/dist/Standable-Linux-Bridge-Overlay.zip" "$archive"
        install -m 0644 "$installer_root/dist/SHA256SUMS" "$checksums"
    else
        echo "Downloading the latest bridge release from $repo"
        if download_release_asset "Standable-Linux-Bridge-Overlay.zip" "$archive" && \
           download_release_asset "SHA256SUMS" "$checksums"; then
            echo "Downloaded the latest GitHub release"
        else
            echo "Latest release unavailable; using the repository distribution"
            download_repository_asset "Standable-Linux-Bridge-Overlay.zip" "$archive"
            download_repository_asset "SHA256SUMS" "$checksums"
        fi
    fi
    expected="$(awk '$2 == "Standable-Linux-Bridge-Overlay.zip" {print $1; exit}' "$checksums")"
    [[ "$expected" =~ ^[0-9a-fA-F]{64}$ ]] || { echo "Release checksum is missing or invalid" >&2; exit 6; }
    actual="$(sha256sum "$archive" | awk '{print $1}')"
    [[ "${actual,,}" == "${expected,,}" ]] || { echo "Release checksum mismatch" >&2; exit 6; }
    if unzip -Z1 "$archive" | awk 'BEGIN { bad=0 } /(^\/|(^|\/)\.\.(\/|$))/ { bad=1 } END { exit bad ? 0 : 1 }'; then
        echo "Release archive contains an unsafe path" >&2
        exit 6
    fi
    overlay_dir="$temporary/overlay"
    mkdir -p "$overlay_dir"
    unzip -q "$archive" -d "$overlay_dir"
fi

required_overlay=(
    "VERSION"
    "README-LINUX.md"
    "bin/linux64/driver_standable.so"
    "bin/win64/standable_bridge_host.exe"
    "bin/win64/steam_api64.dll"
    "scripts/install.sh"
    "scripts/update.sh"
    "scripts/enable-dashboard.sh"
    "scripts/bridge-installer.sh"
)
for relative in "${required_overlay[@]}"; do
    [[ -f "$overlay_dir/$relative" ]] || {
        echo "Overlay is missing: $relative" >&2
        exit 7
    }
done

version="$(tr -d '\r\n' <"$overlay_dir/VERSION")"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+([.-][A-Za-z0-9.-]+)?$ ]] || {
    echo "Overlay VERSION is invalid: $version" >&2
    exit 7
}

backup_root="${XDG_STATE_HOME:-$HOME/.local/state}/standable-linux-bridge/backups"
backup="$backup_root/$(date -u +%Y%m%dT%H%M%S.%NZ)-before-$version"
mkdir -p "$backup"

install_atomic() {
    local source="$1" destination="$2" mode="$3" temporary_file
    mkdir -p "$(dirname -- "$destination")"
    temporary_file="$destination.bridge-new.$$"
    install -m "$mode" "$source" "$temporary_file"
    mv -f -- "$temporary_file" "$destination"
}

bridge_files=(
    "VERSION"
    "README-LINUX.md"
    "bin/linux64/driver_standable.so"
    "bin/win64/standable_bridge_host.exe"
    "bin/win64/steam_api64.dll"
)
while IFS= read -r relative; do
    bridge_files+=("$relative")
done < <(cd -- "$overlay_dir" && find scripts -maxdepth 1 -type f -name '*.sh' -print | sort)

for relative in "${bridge_files[@]}"; do
    source_path="$overlay_dir/$relative"
    destination="$standable_root/$relative"
    [[ -f "$source_path" ]] || continue
    if [[ -f "$destination" ]]; then
        mkdir -p "$backup/$(dirname -- "$relative")"
        cp -a -- "$destination" "$backup/$relative"
    fi
    mode=0644
    [[ "$relative" == scripts/*.sh || "$relative" == bin/linux64/* || "$relative" == *.exe ]] && mode=0755
    install_atomic "$source_path" "$destination" "$mode"
done

if ((register_driver)); then
    bash "$standable_root/scripts/install.sh"
fi

if ((update_mode)); then
    echo "Standable Linux bridge updated to v$version"
else
    echo "Standable Linux bridge v$version installed"
fi
echo "Driver folder: $standable_root"
echo "Backup of replaced bridge files: $backup"
