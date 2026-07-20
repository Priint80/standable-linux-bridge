#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return ROOT.joinpath(path).read_text(encoding="utf-8")


def write(path: str, content: str) -> None:
    target = ROOT / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(content, encoding="utf-8", newline="\n")


def replace_once(path: str, old: str, new: str) -> None:
    content = read(path)
    count = content.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected one exact match, found {count}: {old[:80]!r}")
    write(path, content.replace(old, new, 1))


def replace_regex(path: str, pattern: str, replacement: str) -> None:
    content = read(path)
    updated, count = re.subn(pattern, replacement, content, count=1, flags=re.MULTILINE | re.DOTALL)
    if count != 1:
        raise RuntimeError(f"{path}: expected one regex match, found {count}: {pattern[:80]!r}")
    write(path, updated)


# Dashboard: make XTEST button delivery robust against grabs, focus the Wine
# top-level window rather than an internal render child, synchronize presses,
# accept OpenVR button bitmasks, and use original Standable branding/assets.
replace_once(
    "src/dashboard/dashboard_main.cpp",
    """    using TestFakeButtonEventFn = int (*)(x11::Display*, unsigned int, x11::Bool, unsigned long);
    using CompositeQueryExtensionFn = x11::Bool (*)(x11::Display*, int*, int*);
""",
    """    using TestFakeButtonEventFn = int (*)(x11::Display*, unsigned int, x11::Bool, unsigned long);
    using TestGrabControlFn = int (*)(x11::Display*, x11::Bool);
    using CompositeQueryExtensionFn = x11::Bool (*)(x11::Display*, int*, int*);
""",
)
replace_once(
    "src/dashboard/dashboard_main.cpp",
    """            test_fake_motion_event = xtst.symbol<TestFakeMotionEventFn>("XTestFakeMotionEvent");
            test_fake_button_event = xtst.symbol<TestFakeButtonEventFn>("XTestFakeButtonEvent");
        }
""",
    """            test_fake_motion_event = xtst.symbol<TestFakeMotionEventFn>("XTestFakeMotionEvent");
            test_fake_button_event = xtst.symbol<TestFakeButtonEventFn>("XTestFakeButtonEvent");
            test_grab_control = xtst.symbol<TestGrabControlFn>("XTestGrabControl");
        }
""",
)
replace_once(
    "src/dashboard/dashboard_main.cpp",
    """    TestFakeMotionEventFn test_fake_motion_event{};
    TestFakeButtonEventFn test_fake_button_event{};
    CompositeQueryExtensionFn composite_query_extension{};
""",
    """    TestFakeMotionEventFn test_fake_motion_event{};
    TestFakeButtonEventFn test_fake_button_event{};
    TestGrabControlFn test_grab_control{};
    CompositeQueryExtensionFn composite_query_extension{};
""",
)
replace_once(
    "src/dashboard/dashboard_main.cpp",
    """        if (api_.has_xtest()) {
            int event_base = 0;
            int error_base = 0;
            int major_version = 0;
            int minor_version = 0;
            xtest_available_ = api_.test_query_extension(
                display_, &event_base, &error_base, &major_version, &minor_version) != x11::False;
        }
        std::cout << "dashboard: X11 capture ready (XComposite="
""",
    """        if (api_.has_xtest()) {
            int event_base = 0;
            int error_base = 0;
            int major_version = 0;
            int minor_version = 0;
            xtest_available_ = api_.test_query_extension(
                display_, &event_base, &error_base, &major_version, &minor_version) != x11::False;
            if (xtest_available_ && api_.test_grab_control != nullptr) {
                api_.test_grab_control(display_, x11::True);
                api_.sync(display_, x11::False);
            }
        }
        std::cout << "dashboard: X11 capture ready (XComposite="
""",
)
replace_once(
    "src/dashboard/dashboard_main.cpp",
    """    void send_mouse_button(unsigned int button, bool pressed, unsigned int button_state) {
        if (target_ == 0UL) {
            return;
        }

        bool injected_with_xtest = false;
        if (xtest_available_) {
            if (pressed || !input_target_prepared_) {
                prepare_target_for_input();
            }
            const bool positioned = inject_xtest_motion(last_x_, last_y_);
            injected_with_xtest = positioned &&
                                  api_.test_fake_button_event(
                                      display_, button, pressed ? x11::True : x11::False, 0UL) != 0;
            api_.flush(display_);
            if (!injected_with_xtest) {
                disable_xtest_after_failure("button");
            }
        }
        if (!injected_with_xtest) {
            send_x11_button(button, pressed, button_state);
        }

        ++button_event_count_;
""",
    """    void send_mouse_button(unsigned int button, bool pressed, unsigned int button_state) {
        if (target_ == 0UL) {
            return;
        }

        bool injected_with_xtest = false;
        if (xtest_available_) {
            if (pressed || !input_target_prepared_) {
                prepare_target_for_input();
            }
            const bool positioned = inject_xtest_motion(last_x_, last_y_);
            if (positioned) {
                injected_with_xtest = api_.test_fake_button_event(
                    display_, button, pressed ? x11::True : x11::False, 0UL) != 0;
                // XSync is intentional: Wine must observe the press before a
                // release from the same SteamVR event batch can overtake it.
                api_.sync(display_, x11::False);
                if (pressed && injected_with_xtest) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(3));
                }
            }
            if (!injected_with_xtest) {
                disable_xtest_after_failure("button");
            }
        }
        if (!injected_with_xtest) {
            send_x11_button(button, pressed, button_state);
            api_.sync(display_, x11::False);
        }

        ++button_event_count_;
""",
)
replace_once(
    "src/dashboard/dashboard_main.cpp",
    """        api_.raise_window(display_, top_level_target_ != 0UL ? top_level_target_ : target_);
        api_.set_input_focus(display_, target_, x11::RevertToParent, x11::CurrentTime);
""",
    """        const x11::Window focus_target = top_level_target_ != 0UL ? top_level_target_ : target_;
        api_.raise_window(display_, focus_target);
        api_.set_input_focus(display_, focus_target, x11::RevertToParent, x11::CurrentTime);
""",
)
replace_once(
    "src/dashboard/dashboard_main.cpp",
    """std::optional<std::filesystem::path> find_thumbnail(const std::filesystem::path& driver_root) {
    const std::vector<std::filesystem::path> candidates{
        driver_root / "resources/UI Themes/The Stand/standable_logo.png",
        driver_root / "resources/UI Themes/The Stand/Standable_Logo.png",
        driver_root / "resources/icons/standable.png",
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}
""",
    """std::optional<std::filesystem::path> find_thumbnail(const std::filesystem::path& driver_root) {
    const std::vector<std::filesystem::path> candidates{
        driver_root / "resources/UI Themes/The Stand/standable_logo.png",
        driver_root / "resources/UI Themes/The Stand/Standable_Logo.png",
        driver_root / "resources/icons/standable.png",
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate)) {
            return candidate;
        }
    }

    const std::filesystem::path resources = driver_root / "resources";
    std::error_code error;
    std::optional<std::filesystem::path> best;
    int best_score = 0;
    std::size_t visited = 0U;
    for (std::filesystem::recursive_directory_iterator iterator(
             resources, std::filesystem::directory_options::skip_permission_denied, error), end;
         iterator != end && visited < 4096U;
         iterator.increment(error), ++visited) {
        if (error) {
            error.clear();
            continue;
        }
        if (!iterator->is_regular_file(error)) {
            continue;
        }
        const std::string extension = lowercase(iterator->path().extension().string());
        if (extension != ".png" && extension != ".jpg" && extension != ".jpeg") {
            continue;
        }
        const std::string filename = lowercase(iterator->path().filename().string());
        int score = 0;
        score += filename.find("standable") != std::string::npos ? 12 : 0;
        score += filename.find("logo") != std::string::npos ? 8 : 0;
        score += filename.find("icon") != std::string::npos ? 4 : 0;
        if (score > best_score) {
            best_score = score;
            best = iterator->path();
        }
    }
    return best_score > 0 ? best : std::nullopt;
}
""",
)
replace_once(
    "src/dashboard/dashboard_main.cpp",
    """        const vr::EVROverlayError create_error = overlay_->CreateDashboardOverlay(
            "standable.linux.dashboard", "Standable", &main_handle_, &thumbnail_handle_);
""",
    """        const char* configured_name = std::getenv("STANDABLE_DASHBOARD_NAME");
        const std::string display_name = configured_name != nullptr && *configured_name != '\0'
            ? configured_name
            : "Standable Full Body Estimation";
        const vr::EVROverlayError create_error = overlay_->CreateDashboardOverlay(
            "standable.dashboard", display_name.c_str(), &main_handle_, &thumbnail_handle_);
""",
)
replace_once(
    "src/dashboard/dashboard_main.cpp",
    """    [[nodiscard]] static unsigned int x_button_number(std::uint32_t openvr_button) {
        switch (openvr_button) {
            case vr::VRMouseButton_Left:
                return 1U;
            case vr::VRMouseButton_Middle:
                return 2U;
            case vr::VRMouseButton_Right:
                return 3U;
            default:
                return 0U;
        }
    }
""",
    """    [[nodiscard]] static unsigned int x_button_number(std::uint32_t openvr_button) {
        if ((openvr_button & static_cast<std::uint32_t>(vr::VRMouseButton_Left)) != 0U) {
            return 1U;
        }
        if ((openvr_button & static_cast<std::uint32_t>(vr::VRMouseButton_Middle)) != 0U) {
            return 2U;
        }
        if ((openvr_button & static_cast<std::uint32_t>(vr::VRMouseButton_Right)) != 0U) {
            return 3U;
        }
        return 0U;
    }
""",
)

# Preserve the original front-end icon properties when physical devices are
# mirrored into the unchanged Windows provider.
replace_once(
    "src/native/driver_main.cpp",
    "constexpr std::array<vr::ETrackedDeviceProperty, 51> kMirroredProperties{",
    "constexpr std::array<vr::ETrackedDeviceProperty, 58> kMirroredProperties{",
)
replace_once(
    "src/native/driver_main.cpp",
    """    vr::Prop_ResourceRoot_String,
    vr::Prop_RegisteredDeviceType_String,
""",
    """    vr::Prop_ResourceRoot_String,
    vr::Prop_NamedIconPathDeviceOff_String,
    vr::Prop_NamedIconPathDeviceSearching_String,
    vr::Prop_NamedIconPathDeviceSearchingAlert_String,
    vr::Prop_NamedIconPathDeviceReady_String,
    vr::Prop_NamedIconPathDeviceReadyAlert_String,
    vr::Prop_NamedIconPathDeviceNotReady_String,
    vr::Prop_NamedIconPathDeviceStandby_String,
    vr::Prop_RegisteredDeviceType_String,
""",
)
replace_once(
    "src/native/driver_main.cpp",
    '    const std::string decorated = "[standable-linux] " + message;\n',
    '    const std::string decorated = "[Standable] " + message;\n',
)

# Avoid surfacing implementation names, remote indices, and arbitrary debug
# request text through SteamVR's user-facing device debug response.
replace_once(
    "src/native/relay_tracker.cpp",
    """    std::snprintf(
        response,
        response_size,
        "standable-linux bridge serial=%s remote=%u request=%s",
        serial_.c_str(),
        remote_device_index_,
        request == nullptr ? "" : request);
""",
    """    static_cast<void>(request);
    std::snprintf(response, response_size, "Standable tracker ready");
""",
)

# Package the graphical manager and canonical manifest template.
replace_once(
    "Makefile",
    "overlay: $(NATIVE_SO) $(DASHBOARD_APP) $(WINDOWS_HELPER) $(STEAM_API_BRIDGE)\n",
    "overlay: $(NATIVE_SO) $(DASHBOARD_APP) $(WINDOWS_HELPER) $(STEAM_API_BRIDGE) packaging/driver.vrdrivermanifest\n",
)
replace_once(
    "Makefile",
    """\t@install -d '$(OVERLAY_ROOT)/bin/linux64' '$(OVERLAY_ROOT)/bin/win64' '$(OVERLAY_ROOT)/scripts'
""",
    """\t@install -d '$(OVERLAY_ROOT)/bin/linux64' '$(OVERLAY_ROOT)/bin/win64' '$(OVERLAY_ROOT)/scripts' '$(OVERLAY_ROOT)/share/standable-linux-bridge'
""",
)
replace_once(
    "Makefile",
    """\t@install -m 0755 scripts/*.sh '$(OVERLAY_ROOT)/scripts/'
\t@install -m 0755 install.sh '$(OVERLAY_ROOT)/scripts/bridge-installer.sh'
""",
    """\t@install -m 0755 scripts/*.sh '$(OVERLAY_ROOT)/scripts/'
\t@install -m 0755 scripts/*.py '$(OVERLAY_ROOT)/scripts/'
\t@install -m 0644 packaging/driver.vrdrivermanifest '$(OVERLAY_ROOT)/share/standable-linux-bridge/driver.vrdrivermanifest'
\t@install -m 0755 install.sh '$(OVERLAY_ROOT)/scripts/bridge-installer.sh'
""",
)

# Verify the new user-facing maintenance surface and manifest template.
replace_once(
    "scripts/verify-artifacts.sh",
    """for script in find-steamvr.sh runtime-common.sh enable-dashboard.sh standable-bridge-launcher.sh install.sh update.sh uninstall.sh diagnose.sh bridge-installer.sh; do
""",
    """for script in find-steamvr.sh runtime-common.sh enable-dashboard.sh standable-bridge-launcher.sh install.sh update.sh repair.sh uninstall.sh diagnose.sh manifest-manager.sh bridge-manager.sh bridge-installer.sh; do
""",
)
replace_once(
    "scripts/verify-artifacts.sh",
    """done

file "$native" | grep -q 'ELF 64-bit.*x86-64'
""",
    """done
[[ -x "$root/scripts/bridge-manager.py" ]] || { echo "missing executable scripts/bridge-manager.py" >&2; exit 1; }
[[ -f "$root/share/standable-linux-bridge/driver.vrdrivermanifest" ]] || { echo "missing managed manifest template" >&2; exit 1; }
PYTHONDONTWRITEBYTECODE=1 python3 -m py_compile "$root/scripts/bridge-manager.py"
PYTHONDONTWRITEBYTECODE=1 "$root/scripts/bridge-manager.py" --self-test >/dev/null
python3 - "$root/share/standable-linux-bridge/driver.vrdrivermanifest" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    manifest = json.load(handle)
assert manifest["name"] == "standable"
assert manifest["alwaysActivate"] is True
assert manifest["resourceOnly"] is False
PY

file "$native" | grep -q 'ELF 64-bit.*x86-64'
""",
)

# Privacy-safe diagnostics by default; --full-paths remains available for local
# debugging when the user deliberately wants exact paths.
replace_once(
    "scripts/diagnose.sh",
    """script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
driver_root="$(cd -- "$script_dir/.." && pwd -P)"
source "$script_dir/runtime-common.sh"

""",
    """script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
driver_root="$(cd -- "$script_dir/.." && pwd -P)"
source "$script_dir/runtime-common.sh"

show_full_paths=0
if [[ "${1:-}" == "--full-paths" ]]; then
    show_full_paths=1
    shift
fi
if ((show_full_paths == 0)) && command -v python3 >/dev/null 2>&1; then
    exec > >(python3 -u -c 'import sys\npairs=((sys.argv[1], "<standable-root>"), (sys.argv[2], "~"))\nfor line in sys.stdin:\n    for old, new in pairs:\n        if old:\n            line=line.replace(old, new)\n    sys.stdout.write(line)\n    sys.stdout.flush()' "$driver_root" "$HOME") 2>&1
fi

""",
)

# Installer rewritten around persistent ownership records, branch metadata, and
# exact original-file restoration.
write(
    "install.sh",
    r'''#!/usr/bin/env bash
set -euo pipefail

repo="${STANDABLE_BRIDGE_REPO:-Priint80/standable-linux-bridge}"
branch="${STANDABLE_BRIDGE_BRANCH:-main}"
branch_explicit=0
standable_root="${STANDABLE_ROOT:-}"
overlay_dir=""
source_checkout="${STANDABLE_BRIDGE_SOURCE_CHECKOUT:-}"
update_mode=0
register_driver=1
installer_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"

usage() {
    cat <<'EOF'
Usage: ./install.sh [options]

Options:
  --standable-root PATH   Original "Standable Full Body Estimation" folder
  --overlay-dir PATH      Install an already-extracted bridge overlay
  --repo OWNER/REPO       GitHub repository used for downloads
  --branch BRANCH         Repository branch associated with this installation
  --source-checkout PATH  Source checkout used by Repair
  --update                Label this operation as an update
  --no-register           Install files and manifest without registering SteamVR
  -h, --help              Show this help
EOF
}

while (($#)); do
    case "$1" in
        --standable-root) standable_root="${2:-}"; shift 2 ;;
        --overlay-dir) overlay_dir="${2:-}"; shift 2 ;;
        --repo) repo="${2:-}"; shift 2 ;;
        --branch) branch="${2:-}"; branch_explicit=1; shift 2 ;;
        --source-checkout) source_checkout="${2:-}"; shift 2 ;;
        --update) update_mode=1; shift ;;
        --no-register) register_driver=0; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

for command in bash install sha256sum; do
    command -v "$command" >/dev/null 2>&1 || { echo "Missing required command: $command" >&2; exit 3; }
done
[[ "$repo" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]] || { echo "Invalid repository: $repo" >&2; exit 2; }
[[ "$branch" =~ ^[A-Za-z0-9._/-]+$ && "$branch" != *..* ]] || { echo "Invalid branch: $branch" >&2; exit 2; }

if [[ -z "$source_checkout" ]] && command -v git >/dev/null 2>&1 && git -C "$installer_root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    source_checkout="$(git -C "$installer_root" rev-parse --show-toplevel)"
    if ((branch_explicit == 0)); then
        detected_branch="$(git -C "$source_checkout" branch --show-current)"
        [[ -n "$detected_branch" ]] && branch="$detected_branch"
    fi
fi
if [[ -n "$source_checkout" ]]; then
    [[ -d "$source_checkout" ]] || { echo "Source checkout does not exist: $source_checkout" >&2; exit 4; }
    source_checkout="$(cd -- "$source_checkout" && pwd -P)"
fi

is_standable_root() {
    local candidate="$1"
    [[ -f "$candidate/driver.vrdrivermanifest" && -f "$candidate/Standable.exe" && -f "$candidate/bin/win64/driver_standable.dll" ]]
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
        if is_standable_root "$candidate"; then printf '%s\n' "$candidate"; return 0; fi
    done
    declare -a steam_roots=()
    for candidate in "${STEAM_ROOT:-}" "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.steam/root" "$HOME/.var/app/com.valvesoftware.Steam/data/Steam"; do
        add_steam_root "$candidate"
    done
    for vdf in "$HOME/.local/share/Steam/steamapps/libraryfolders.vdf" "$HOME/.steam/steam/steamapps/libraryfolders.vdf" "$HOME/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/libraryfolders.vdf"; do
        [[ -f "$vdf" ]] || continue
        while IFS= read -r line; do
            if [[ "$line" =~ \"path\"[[:space:]]+\"([^\"]+)\" ]]; then
                path="${BASH_REMATCH[1]}"; path="${path//\\\\/\\}"; add_steam_root "$path"
            fi
        done <"$vdf"
    done
    for candidate in "${steam_roots[@]:-}"; do
        path="$candidate/steamapps/common/Standable Full Body Estimation"
        if is_standable_root "$path"; then printf '%s\n' "$path"; return 0; fi
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
cleanup() { [[ -n "$temporary" && -d "$temporary" ]] && rm -rf -- "$temporary"; return 0; }
trap cleanup EXIT

download_public_asset() {
    curl --fail --location --silent --show-error --output "$2" "https://github.com/$repo/releases/latest/download/$1"
}

download_private_asset() {
    local name="$1" output="$2" metadata asset_url
    command -v python3 >/dev/null 2>&1 || { echo "python3 is required for private releases" >&2; return 1; }
    metadata="$temporary/latest-release.json"
    curl --fail --location --silent --show-error -H "Authorization: Bearer $GITHUB_TOKEN" -H "Accept: application/vnd.github+json" --output "$metadata" "https://api.github.com/repos/$repo/releases/latest"
    asset_url="$(python3 - "$metadata" "$name" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle: release=json.load(handle)
for asset in release.get("assets", []):
    if asset.get("name") == sys.argv[2]: print(asset["url"]); break
PY
)"
    [[ -n "$asset_url" ]] || return 1
    curl --fail --location --silent --show-error -H "Authorization: Bearer $GITHUB_TOKEN" -H "Accept: application/octet-stream" --output "$output" "$asset_url"
}

download_release_asset() {
    if command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1; then
        gh release download --repo "$repo" --pattern "$1" --output "$2" --clobber
    elif [[ -n "${GITHUB_TOKEN:-}" ]]; then
        download_private_asset "$1" "$2"
    else
        download_public_asset "$1" "$2"
    fi
}

download_repository_asset() {
    local name="$1" output="$2"
    local -a headers=(-H "Accept: application/vnd.github.raw+json")
    [[ -n "${GITHUB_TOKEN:-}" ]] && headers+=(-H "Authorization: Bearer $GITHUB_TOKEN")
    curl --fail --location --silent --show-error "${headers[@]}" --get --data-urlencode "ref=$branch" --output "$output" "https://api.github.com/repos/$repo/contents/dist/$name"
}

if [[ -n "$overlay_dir" ]]; then
    [[ -d "$overlay_dir" ]] || { echo "Overlay directory does not exist: $overlay_dir" >&2; exit 5; }
    overlay_dir="$(cd -- "$overlay_dir" && pwd -P)"
else
    for command in curl unzip; do command -v "$command" >/dev/null 2>&1 || { echo "Missing required command: $command" >&2; exit 3; }; done
    temporary="$(mktemp -d "${TMPDIR:-/tmp}/standable-linux-bridge.XXXXXX")"
    archive="$temporary/Standable-Linux-Bridge-Overlay.zip"
    checksums="$temporary/SHA256SUMS"
    bundled=0
    if [[ -f "$installer_root/dist/Standable-Linux-Bridge-Overlay.zip" && -f "$installer_root/dist/SHA256SUMS" ]]; then
        bundled_version="$(unzip -p "$installer_root/dist/Standable-Linux-Bridge-Overlay.zip" VERSION 2>/dev/null | tr -d '\r\n' || true)"
        source_version="$(tr -d '\r\n' <"$installer_root/VERSION" 2>/dev/null || true)"
        [[ -n "$bundled_version" && "$bundled_version" == "$source_version" ]] && bundled=1
    fi
    if ((bundled)); then
        echo "Using the verified bridge distribution bundled with this checkout"
        install -m 0644 "$installer_root/dist/Standable-Linux-Bridge-Overlay.zip" "$archive"
        install -m 0644 "$installer_root/dist/SHA256SUMS" "$checksums"
    elif [[ "$branch" == "main" ]] && download_release_asset "Standable-Linux-Bridge-Overlay.zip" "$archive" && download_release_asset "SHA256SUMS" "$checksums"; then
        echo "Downloaded the latest bridge release"
    else
        echo "Using the verified repository distribution from branch $branch"
        download_repository_asset "Standable-Linux-Bridge-Overlay.zip" "$archive"
        download_repository_asset "SHA256SUMS" "$checksums"
    fi
    expected="$(awk '$2 == "Standable-Linux-Bridge-Overlay.zip" {print $1; exit}' "$checksums")"
    [[ "$expected" =~ ^[0-9a-fA-F]{64}$ ]] || { echo "Release checksum is missing or invalid" >&2; exit 6; }
    actual="$(sha256sum "$archive" | awk '{print $1}')"
    [[ "${actual,,}" == "${expected,,}" ]] || { echo "Release checksum mismatch" >&2; exit 6; }
    if unzip -Z1 "$archive" | awk 'BEGIN { bad=0 } /(^\/|(^|\/)\.\.(\/|$))/ { bad=1 } END { exit bad ? 0 : 1 }'; then
        echo "Release archive contains an unsafe path" >&2; exit 6
    fi
    overlay_dir="$temporary/overlay"; mkdir -p "$overlay_dir"; unzip -q "$archive" -d "$overlay_dir"
fi

required_overlay=(
    VERSION README-LINUX.md THIRD_PARTY_NOTICES.md
    bin/linux64/driver_standable.so bin/linux64/standable_dashboard_overlay
    bin/win64/standable_bridge_host.exe bin/win64/steam_api64.dll
    share/standable-linux-bridge/driver.vrdrivermanifest
    scripts/install.sh scripts/update.sh scripts/repair.sh scripts/uninstall.sh
    scripts/manifest-manager.sh scripts/bridge-manager.py scripts/bridge-manager.sh
    scripts/enable-dashboard.sh scripts/bridge-installer.sh
)
for relative in "${required_overlay[@]}"; do [[ -f "$overlay_dir/$relative" ]] || { echo "Overlay is missing: $relative" >&2; exit 7; }; done
version="$(tr -d '\r\n' <"$overlay_dir/VERSION")"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+([.-][A-Za-z0-9.-]+)?$ ]] || { echo "Overlay VERSION is invalid: $version" >&2; exit 7; }

backup_root="${XDG_STATE_HOME:-$HOME/.local/state}/standable-linux-bridge/backups"
backup="$backup_root/$(date -u +%Y%m%dT%H%M%S.%NZ)-before-$version"
mkdir -p "$backup"
manifest_tool="$overlay_dir/scripts/manifest-manager.sh"
installation_state="$(bash "$manifest_tool" state-dir "$standable_root")"
mkdir -p "$installation_state/original-files" "$installation_state/absent-files"

install_atomic() {
    local source="$1" destination="$2" mode="$3" temporary_file
    mkdir -p "$(dirname -- "$destination")"
    temporary_file="$destination.bridge-new.$$"
    install -m "$mode" "$source" "$temporary_file"
    mv -f -- "$temporary_file" "$destination"
}

record_original() {
    local relative="$1" source_path="$2" destination="$standable_root/$relative"
    local original="$installation_state/original-files/$relative" absent="$installation_state/absent-files/$relative"
    [[ -e "$original" || -e "$absent" ]] && return 0
    if [[ -f "$destination" && "$(sha256sum "$destination" | awk '{print $1}')" != "$(sha256sum "$source_path" | awk '{print $1}')" ]]; then
        mkdir -p "$(dirname -- "$original")"; cp -a -- "$destination" "$original"
    else
        mkdir -p "$(dirname -- "$absent")"; : >"$absent"
    fi
}

bridge_files=(
    VERSION README-LINUX.md THIRD_PARTY_NOTICES.md
    bin/linux64/driver_standable.so bin/linux64/standable_dashboard_overlay
    bin/win64/standable_bridge_host.exe bin/win64/steam_api64.dll
    share/standable-linux-bridge/driver.vrdrivermanifest
)
while IFS= read -r relative; do bridge_files+=("$relative"); done < <(cd -- "$overlay_dir" && find scripts -maxdepth 1 -type f \( -name '*.sh' -o -name '*.py' \) -print | sort)

installed_list="$installation_state/installed-files.tsv.new"
: >"$installed_list"
for relative in "${bridge_files[@]}"; do
    source_path="$overlay_dir/$relative"; destination="$standable_root/$relative"
    [[ -f "$source_path" ]] || continue
    record_original "$relative" "$source_path"
    if [[ -f "$destination" ]]; then mkdir -p "$backup/$(dirname -- "$relative")"; cp -a -- "$destination" "$backup/$relative"; fi
    mode=0644
    [[ "$relative" == scripts/*.sh || "$relative" == scripts/*.py || "$relative" == bin/linux64/* || "$relative" == *.exe ]] && mode=0755
    install_atomic "$source_path" "$destination" "$mode"
    printf '%s\t%s\n' "$(sha256sum "$destination" | awk '{print $1}')" "$relative" >>"$installed_list"
done
mv -f -- "$installed_list" "$installation_state/installed-files.tsv"

if [[ -f "$standable_root/driver.vrdrivermanifest" ]]; then cp -a -- "$standable_root/driver.vrdrivermanifest" "$backup/driver.vrdrivermanifest"; fi
{
    printf 'STANDABLE_BRIDGE_REPO=%q\n' "$repo"
    printf 'STANDABLE_BRIDGE_BRANCH=%q\n' "$branch"
    printf 'STANDABLE_BRIDGE_SOURCE_CHECKOUT=%q\n' "$source_checkout"
    printf 'STANDABLE_BRIDGE_VERSION=%q\n' "$version"
} >"$installation_state/metadata.env"

if ((register_driver)); then
    bash "$standable_root/scripts/install.sh"
else
    bash "$standable_root/scripts/manifest-manager.sh" install "$standable_root" "$standable_root/share/standable-linux-bridge/driver.vrdrivermanifest" >/dev/null
fi

if ((update_mode)); then echo "Standable Linux Bridge updated to v$version"; else echo "Standable Linux Bridge v$version installed"; fi
echo "Original files and the SteamVR manifest have a rollback snapshot."
echo "Graphical maintenance: $standable_root/scripts/bridge-manager.sh"
''',
)

# Documentation: present the GUI first while retaining transparent CLI paths.
replace_once(
    "README.md",
    """Close SteamVR, then run:

```bash
git clone https://github.com/Priint80/standable-linux-bridge.git
cd standable-linux-bridge
./install.sh
```
""",
    """Close SteamVR, clone the repository, and open the graphical manager:

```bash
git clone https://github.com/Priint80/standable-linux-bridge.git
cd standable-linux-bridge
./scripts/bridge-manager.sh
```

Select the original **Standable Full Body Estimation** folder and choose **Install**. The CLI remains available through `./install.sh`.
""",
)
replace_once(
    "README.md",
    """Every install or update backs up replaced bridge files under:
""",
    """The graphical manager also provides **Update**, **Repair**, and **Uninstall**. Repair rebases the recorded source checkout against its saved branch, builds a fresh overlay, restores the original installation, and reinstalls that build.

Every install or update backs up replaced bridge files under:
""",
)
replace_once(
    "README-LINUX.md",
    """1. Close SteamVR.
2. Extract this overlay directly into the original **Standable Full Body Estimation** folder.
3. From that folder, run:

   ```bash
   ./scripts/install.sh
   ```

4. Start SteamVR. The bridge launches the original Standable UI and native dashboard companion automatically, and Standable appears as a dashboard tab. Proton's private prefix can take longer to initialize on the first launch.
""",
    """1. Close SteamVR.
2. Extract this overlay directly into the original **Standable Full Body Estimation** folder.
3. From that folder, open the graphical manager:

   ```bash
   ./scripts/bridge-manager.sh
   ```

   Choose **Install**. CLI installation remains available through `./scripts/install.sh`.
4. Start SteamVR. The bridge launches the original Standable UI and native dashboard companion automatically, and Standable appears as a dashboard tab. Proton's private prefix can take longer to initialize on the first launch.
""",
)
replace_once(
    "README-LINUX.md",
    """## Update

From this folder, run:

```bash
./scripts/update.sh
```
""",
    """## Maintenance

Run `./scripts/bridge-manager.sh` for **Update**, **Repair**, and **Uninstall**. Repair uses the recorded repository branch, rebases a saved source checkout when available, then performs a clean uninstall and reinstall.

CLI equivalents:

```bash
./scripts/update.sh
./scripts/repair.sh
./scripts/uninstall.sh
```
""",
)
replace_once(
    "README-LINUX.md",
    """To unregister the driver without deleting files or settings:

```bash
./scripts/uninstall.sh
```
""",
    """Uninstall restores the exact original `driver.vrdrivermanifest` and any pre-existing files that were replaced. It removes only bridge-owned files; Standable settings, resources, saved poses, `Standable.exe`, and the original Windows driver remain untouched.
""",
)

# Runtime tests now assert manifest management and GUI command routing.
replace_once(
    "tests/script_runtime.sh",
    """bash "$installer_tree/install.sh" --standable-root "$driver_root"

mkdir -p "$remote_installer_tree/bin"
""",
    """bash "$installer_tree/install.sh" --standable-root "$driver_root"
python3 - "$driver_root/driver.vrdrivermanifest" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    manifest = json.load(handle)
assert manifest["name"] == "standable"
assert manifest["alwaysActivate"] is True
assert manifest["resourceOnly"] is False
PY
PYTHONDONTWRITEBYTECODE=1 "$driver_root/scripts/bridge-manager.py" --self-test
manifest_state="$(bash "$driver_root/scripts/manifest-manager.sh" state-dir "$driver_root")"
[[ -f "$manifest_state/original-driver.vrdrivermanifest" ]]

mkdir -p "$remote_installer_tree/bin"
""",
)
replace_once(
    "tests/script_runtime.sh",
    "grep -q '/main/dist/Standable-Linux-Bridge-Overlay.zip' \"$curl_log\"\n",
    "grep -Eq 'contents/dist/Standable-Linux-Bridge-Overlay.zip|ref=main' \"$curl_log\"\n",
)
replace_once(
    "tests/script_runtime.sh",
    """bash "$driver_root/scripts/uninstall.sh" >/dev/null
grep -q "args=<adddriver><$driver_root>" "$registry_log"
""",
    """bash "$driver_root/scripts/uninstall.sh" >/dev/null
[[ -f "$driver_root/driver.vrdrivermanifest" ]]
[[ ! -e "$driver_root/bin/linux64/standable_dashboard_overlay" ]]
grep -q "args=<adddriver><$driver_root>" "$registry_log"
""",
)

print("Applied dashboard, installer, branding, privacy, and maintenance polish")
