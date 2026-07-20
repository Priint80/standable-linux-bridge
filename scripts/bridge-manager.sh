#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
command -v python3 >/dev/null 2>&1 || {
    echo "Python 3 is required for the graphical Standable Linux Bridge manager." >&2
    exit 2
}

exec python3 - "$script_dir" "$@" <<'PY'
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import subprocess
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request
from typing import Callable, Iterable

APP_NAME = "Standable Linux Bridge"
APP_ID = 2370570
SCRIPT_DIR = Path(sys.argv[1]).resolve()
ARGUMENTS = sys.argv[2:]
BASE_ROOT = SCRIPT_DIR.parent
IS_SOURCE = BASE_ROOT.joinpath("Makefile").is_file() and BASE_ROOT.joinpath("install.sh").is_file()
DEFAULT_REPOSITORY = "Priint80/standable-linux-bridge"
STATE_ROOT = Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local/state")) / "standable-linux-bridge"


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8").strip()
    except OSError:
        return ""


def read_json(path: Path) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    return value if isinstance(value, dict) else {}


def state_dir_for(root: Path) -> Path:
    digest = hashlib.sha256(str(root.resolve()).encode() + b"\0").hexdigest()[:24]
    return STATE_ROOT / "installations" / digest


def is_standable_root(candidate: Path) -> bool:
    return (
        candidate.joinpath("Standable.exe").is_file()
        and candidate.joinpath("driver.vrdrivermanifest").is_file()
        and candidate.joinpath("bin/win64/driver_standable.dll").is_file()
    )


def parse_vdf_paths(path: Path) -> list[Path]:
    content = read_text(path)
    found: list[Path] = []
    for match in re.finditer(r'"path"\s+"([^"]+)"', content, flags=re.IGNORECASE):
        value = match.group(1).replace("\\\\", "\\")
        if value:
            found.append(Path(value).expanduser())
    return found


def parse_installdir(path: Path) -> str:
    content = read_text(path)
    match = re.search(r'"installdir"\s+"([^"]+)"', content, flags=re.IGNORECASE)
    return match.group(1) if match else "Standable Full Body Estimation"


def unique_paths(paths: Iterable[Path]) -> list[Path]:
    result: list[Path] = []
    seen: set[str] = set()
    for path in paths:
        try:
            normalized = str(path.expanduser().resolve(strict=False))
        except OSError:
            normalized = str(path.expanduser())
        if normalized in seen:
            continue
        seen.add(normalized)
        result.append(Path(normalized))
    return result


def steam_roots() -> list[Path]:
    home = Path.home()
    roots: list[Path] = [
        Path(os.environ["STEAM_ROOT"]) if os.environ.get("STEAM_ROOT") else Path("/__missing__"),
        home / ".local/share/Steam",
        home / ".steam/steam",
        home / ".steam/root",
        home / ".var/app/com.valvesoftware.Steam/data/Steam",
    ]

    # Common removable-drive layouts, including /mnt/<uuid>/SteamLibrary.
    for base in (
        Path("/mnt"),
        Path("/media") / os.environ.get("USER", ""),
        Path("/run/media") / os.environ.get("USER", ""),
    ):
        if not base.is_dir():
            continue
        try:
            children = list(base.iterdir())
        except OSError:
            continue
        for child in children:
            roots.extend((child, child / "SteamLibrary", child / "Steam"))

    expanded = unique_paths(roots)
    vdfs = [root / "steamapps/libraryfolders.vdf" for root in expanded]
    for vdf in vdfs:
        if vdf.is_file():
            expanded.extend(parse_vdf_paths(vdf))
    return [root for root in unique_paths(expanded) if root.joinpath("steamapps").is_dir()]


@dataclass(frozen=True)
class InstallationInfo:
    root: Path
    source: str
    kind: str
    version: str
    commit: str
    repository: str
    branch: str
    metadata: dict[str, object]

    @property
    def bridge_present(self) -> bool:
        return self.kind in {"managed", "legacy"}

    @property
    def label(self) -> str:
        if self.kind == "managed":
            suffix = f"v{self.version}" if self.version else "managed"
        elif self.kind == "legacy":
            suffix = f"legacy v{self.version}" if self.version else "legacy bridge"
        else:
            suffix = "original Standable"
        return f"{self.root}  —  {suffix}"


def inspect_installation(root: Path, source: str) -> InstallationInfo | None:
    try:
        root = root.expanduser().resolve(strict=False)
    except OSError:
        root = root.expanduser()
    if not is_standable_root(root):
        return None

    state_dir = state_dir_for(root)
    metadata = read_json(state_dir / "metadata.json")
    native = root.joinpath("bin/linux64/driver_standable.so").is_file()
    managed = native and bool(metadata) and state_dir.joinpath("original-driver.vrdrivermanifest").is_file()
    kind = "managed" if managed else "legacy" if native else "original"
    version = read_text(root / "VERSION")
    commit = str(metadata.get("commit", "")) if isinstance(metadata.get("commit", ""), str) else ""
    repository = (
        str(metadata.get("repository", DEFAULT_REPOSITORY))
        if isinstance(metadata.get("repository", ""), str)
        else DEFAULT_REPOSITORY
    )
    branch = str(metadata.get("branch", "main")) if isinstance(metadata.get("branch", ""), str) else "main"
    return InstallationInfo(root, source, kind, version, commit, repository, branch, metadata)


def discover_installations() -> list[InstallationInfo]:
    candidates: list[tuple[Path, str]] = []

    for candidate in (BASE_ROOT, BASE_ROOT.parent, Path.cwd(), Path.cwd().parent):
        candidates.append((candidate, "current location"))

    installations_dir = STATE_ROOT / "installations"
    if installations_dir.is_dir():
        try:
            state_entries = list(installations_dir.iterdir())
        except OSError:
            state_entries = []
        for state_dir in state_entries:
            recorded = read_text(state_dir / "driver-root")
            if recorded:
                candidates.append((Path(recorded), "saved installation"))

    for steam_root in steam_roots():
        appmanifest = steam_root / f"steamapps/appmanifest_{APP_ID}.acf"
        installdir = parse_installdir(appmanifest) if appmanifest.is_file() else "Standable Full Body Estimation"
        candidates.append((steam_root / "steamapps/common" / installdir, "Steam library"))
        candidates.append((steam_root / "steamapps/common/Standable Full Body Estimation", "Steam library"))

    results: list[InstallationInfo] = []
    seen: set[str] = set()
    for candidate, source in candidates:
        info = inspect_installation(candidate, source)
        if info is None:
            continue
        key = str(info.root)
        if key in seen:
            continue
        seen.add(key)
        results.append(info)

    rank = {"managed": 3, "legacy": 2, "original": 1}
    results.sort(
        key=lambda item: (rank[item.kind], item.source == "saved installation", item.version),
        reverse=True,
    )
    return results


def git_value(*arguments: str) -> str:
    try:
        return subprocess.check_output(
            ["git", "-C", str(BASE_ROOT), *arguments],
            text=True,
            stderr=subprocess.DEVNULL,
            timeout=4,
        ).strip()
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired):
        return ""


def source_identity() -> dict[str, str]:
    return {
        "version": read_text(BASE_ROOT / "VERSION"),
        "commit": git_value("rev-parse", "HEAD") if IS_SOURCE else "",
        "branch": git_value("branch", "--show-current") if IS_SOURCE else "",
        "repository": os.environ.get("STANDABLE_BRIDGE_REPO", DEFAULT_REPOSITORY),
    }


def fetch_remote_identity(repository: str, branch: str) -> dict[str, str]:
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        return {}
    if not branch or not re.fullmatch(r"[A-Za-z0-9._/-]+", branch):
        return {}

    owner, name = repository.split("/", 1)
    encoded_branch = urllib.parse.quote(branch, safe="")
    request = urllib.request.Request(
        f"https://api.github.com/repos/{owner}/{name}/commits/{encoded_branch}",
        headers={
            "Accept": "application/vnd.github+json",
            "User-Agent": "standable-linux-bridge-manager",
        },
    )
    result: dict[str, str] = {}
    try:
        with urllib.request.urlopen(request, timeout=4) as response:
            document = json.load(response)
        if isinstance(document, dict) and isinstance(document.get("sha"), str):
            result["commit"] = document["sha"]
    except (OSError, urllib.error.URLError, json.JSONDecodeError):
        pass

    version_request = urllib.request.Request(
        f"https://api.github.com/repos/{owner}/{name}/contents/VERSION?ref={encoded_branch}",
        headers={
            "Accept": "application/vnd.github.raw+json",
            "User-Agent": "standable-linux-bridge-manager",
        },
    )
    try:
        with urllib.request.urlopen(version_request, timeout=4) as response:
            result["version"] = response.read(128).decode("utf-8", errors="replace").strip()
    except (OSError, urllib.error.URLError):
        pass
    return result


def version_key(value: str) -> tuple[int, int, int, str]:
    match = re.match(r"^(\d+)\.(\d+)\.(\d+)(.*)$", value.strip())
    if not match:
        return (0, 0, 0, value)
    return (int(match.group(1)), int(match.group(2)), int(match.group(3)), match.group(4))


def action_environment() -> dict[str, str]:
    environment = dict(os.environ)
    if not IS_SOURCE:
        return environment
    identity = source_identity()
    environment["STANDABLE_BRIDGE_SOURCE_CHECKOUT"] = str(BASE_ROOT)
    if identity["branch"]:
        environment["STANDABLE_BRIDGE_BRANCH"] = identity["branch"]
    if identity["commit"]:
        environment["STANDABLE_BRIDGE_COMMIT"] = identity["commit"]
    return environment


def command_for(action: str, root: Path) -> list[str]:
    installed_scripts = root / "scripts"
    identity = source_identity()
    repository = identity["repository"] or DEFAULT_REPOSITORY
    branch = identity["branch"] or "main"

    if IS_SOURCE:
        if action == "install":
            return ["bash", str(SCRIPT_DIR / "source-install.sh"), "--standable-root", str(root)]
        if action == "update":
            return [
                "bash",
                str(SCRIPT_DIR / "source-install.sh"),
                "--update",
                "--standable-root",
                str(root),
            ]
        if action == "repair":
            return [
                "bash",
                str(SCRIPT_DIR / "repair.sh"),
                "--standable-root",
                str(root),
                "--source-checkout",
                str(BASE_ROOT),
                "--repo",
                repository,
                "--branch",
                branch,
            ]
        if action == "uninstall":
            installed = installed_scripts / "uninstall.sh"
            script = installed if installed.is_file() else SCRIPT_DIR / "uninstall.sh"
            return ["bash", str(script), "--standable-root", str(root)]

    if action == "install":
        return [
            "bash",
            str(installed_scripts / "bridge-installer.sh"),
            "--standable-root",
            str(root),
        ]
    if action == "update":
        return ["bash", str(installed_scripts / "update.sh")]
    if action == "repair":
        return ["bash", str(installed_scripts / "repair.sh")]
    if action == "uninstall":
        return ["bash", str(installed_scripts / "uninstall.sh")]
    raise ValueError(f"Unknown action: {action}")


def self_test() -> int:
    fake = Path("/tmp/Standable Full Body Estimation")
    expected = {
        "install": "source-install.sh" if IS_SOURCE else "bridge-installer.sh",
        "update": "source-install.sh" if IS_SOURCE else "update.sh",
        "repair": "repair.sh",
        "uninstall": "uninstall.sh",
    }
    for action, name in expected.items():
        command = command_for(action, fake)
        if len(command) < 2 or Path(command[1]).name != name:
            print(f"manager self-test failed for {action}: {command}", file=sys.stderr)
            return 1
    if parse_installdir(Path("/__missing__")) != "Standable Full Body Estimation":
        print("manager self-test failed for appmanifest fallback", file=sys.stderr)
        return 1
    if version_key("1.12.3") <= version_key("1.9.9"):
        print("manager self-test failed for version ordering", file=sys.stderr)
        return 1
    expected_state = hashlib.sha256(str(fake.resolve()).encode() + b"\0").hexdigest()[:24]
    if state_dir_for(fake).name != expected_state:
        print("manager self-test failed for installation state identity", file=sys.stderr)
        return 1
    print("bridge manager self-test passed")
    return 0


if "--self-test" in ARGUMENTS:
    raise SystemExit(self_test())

try:
    import tkinter as tk
    from tkinter import filedialog, messagebox
except ImportError:
    print(
        "Python Tk support is not installed. Use source-install.sh, update.sh, repair.sh, "
        "or uninstall.sh directly.",
        file=sys.stderr,
    )
    raise SystemExit(3)


def rgb(value: str) -> tuple[int, int, int]:
    value = value.lstrip("#")
    return tuple(int(value[index:index + 2], 16) for index in (0, 2, 4))  # type: ignore[return-value]


def hex_color(value: tuple[int, int, int]) -> str:
    return "#" + "".join(f"{max(0, min(255, channel)):02x}" for channel in value)


def blend(first: str, second: str, amount: float) -> str:
    a = rgb(first)
    b = rgb(second)
    return hex_color(
        tuple(round(a[index] + (b[index] - a[index]) * amount) for index in range(3))
    )


def rounded_points(x1: float, y1: float, x2: float, y2: float, radius: float) -> list[float]:
    radius = min(radius, (x2 - x1) / 2, (y2 - y1) / 2)
    return [
        x1 + radius, y1,
        x2 - radius, y1,
        x2, y1,
        x2, y1 + radius,
        x2, y2 - radius,
        x2, y2,
        x2 - radius, y2,
        x1 + radius, y2,
        x1, y2,
        x1, y2 - radius,
        x1, y1 + radius,
        x1, y1,
    ]


class RoundedPanel(tk.Canvas):
    def __init__(
        self,
        parent: tk.Misc,
        *,
        fill: str,
        border: str,
        radius: int = 18,
        height: int = 100,
        padding: int = 18,
    ) -> None:
        parent_bg = str(parent.cget("bg"))
        super().__init__(
            parent,
            bg=parent_bg,
            highlightthickness=0,
            bd=0,
            height=height,
        )
        self.fill = fill
        self.border = border
        self.radius = radius
        self.padding = padding
        self.inner = tk.Frame(self, bg=fill, bd=0, highlightthickness=0)
        self._window = self.create_window(padding, padding, anchor="nw", window=self.inner)
        self.bind("<Configure>", self._redraw)

    def _redraw(self, _event: object | None = None) -> None:
        width = max(2, self.winfo_width())
        height = max(2, self.winfo_height())
        self.delete("panel")
        self.create_polygon(
            rounded_points(1, 1, width - 1, height - 1, self.radius),
            smooth=True,
            splinesteps=24,
            fill=self.fill,
            outline=self.border,
            width=1,
            tags="panel",
        )
        self.tag_lower("panel")
        self.coords(self._window, self.padding, self.padding)
        self.itemconfigure(
            self._window,
            width=max(1, width - self.padding * 2),
            height=max(1, height - self.padding * 2),
        )

    def set_colors(self, *, fill: str | None = None, border: str | None = None) -> None:
        if fill is not None:
            self.fill = fill
            self.inner.configure(bg=fill)
        if border is not None:
            self.border = border
        self._redraw()


class RoundedButton(tk.Canvas):
    def __init__(
        self,
        parent: tk.Misc,
        text: str,
        command: Callable[[], None],
        *,
        width: int = 112,
        height: int = 38,
        fill: str = "#aab2ff",
        hover: str = "#c5caff",
        foreground: str = "#11131b",
        disabled_fill: str = "#33394b",
        radius: int = 12,
    ) -> None:
        super().__init__(
            parent,
            width=width,
            height=height,
            bg=str(parent.cget("bg")),
            highlightthickness=0,
            bd=0,
        )
        self._text = text
        self._command = command
        self._normal = fill
        self._hover = hover
        self._foreground = foreground
        self._disabled_fill = disabled_fill
        self._current = fill
        self._target = fill
        self._enabled = True
        self._radius = radius
        self._animation = 0
        self.configure(cursor="hand2")
        self.bind("<Configure>", lambda _event: self._draw())
        self.bind("<Enter>", lambda _event: self._animate_to(self._hover))
        self.bind("<Leave>", lambda _event: self._animate_to(self._normal))
        self.bind("<ButtonRelease-1>", self._activate)
        self._draw()

    def _draw(self) -> None:
        width = max(2, self.winfo_width())
        height = max(2, self.winfo_height())
        self.delete("all")
        self.create_polygon(
            rounded_points(1, 1, width - 1, height - 1, self._radius),
            smooth=True,
            splinesteps=24,
            fill=self._current if self._enabled else self._disabled_fill,
            outline="",
        )
        self.create_text(
            width / 2,
            height / 2,
            text=self._text,
            fill=self._foreground if self._enabled else "#8990a3",
            font=("Sans", 9, "bold"),
        )

    def set_text(self, value: str) -> None:
        self._text = value
        self._draw()

    def set_enabled(self, enabled: bool) -> None:
        self._enabled = enabled
        self.configure(cursor="hand2" if enabled else "arrow")
        self._draw()

    def _animate_to(self, target: str) -> None:
        if not self._enabled:
            return
        self._animation += 1
        animation = self._animation
        start = self._current
        self._target = target

        def step(index: int = 1) -> None:
            if animation != self._animation:
                return
            self._current = blend(start, target, index / 8)
            self._draw()
            if index < 8:
                self.after(14, lambda: step(index + 1))

        step()

    def _activate(self, _event: object) -> None:
        if self._enabled:
            self._command()


class BridgeManager(tk.Tk):
    background = "#0b0e16"
    surface = "#151a27"
    field = "#202738"
    border = "#2b3348"
    text = "#f1f3f8"
    muted = "#a9b0c3"
    accent = "#aab2ff"
    accent_hover = "#c7cbff"
    destructive = "#eda7b7"
    success = "#78d6ad"
    warning = "#efc07b"
    error = "#ef8f9f"

    def __init__(self) -> None:
        super().__init__()
        self.title(APP_NAME)
        self.geometry("900x790")
        self.minsize(790, 710)
        self.configure(bg=self.background)
        self.option_add("*Font", "Sans 10")
        self.output_queue: queue.Queue[tuple[str, object]] = queue.Queue()
        self.worker: threading.Thread | None = None
        self.discovery_worker: threading.Thread | None = None
        self.remote_generation = 0
        self.detected: list[InstallationInfo] = []
        self.status_info: InstallationInfo | None = None
        self.busy = False
        self.spinner_index = 0
        self.root_value = tk.StringVar(value="")
        self.status_value = tk.StringVar(value="Scanning Steam libraries…")
        self.detected_value = tk.StringVar(
            value="Looking for Standable and existing bridge installs…"
        )
        self.root_value.trace_add("write", self._path_changed)
        self._path_refresh_id: str | None = None
        self._build()
        self.after(75, self._drain_output)
        self.after(100, self._animate_status)
        self.after(160, self._start_discovery)

    def _build(self) -> None:
        content = tk.Frame(self, bg=self.background)
        content.pack(fill="both", expand=True, padx=34, pady=(26, 26))

        header = tk.Frame(content, bg=self.background)
        header.pack(fill="x", pady=(0, 16))
        tk.Label(
            header,
            text="Standable Linux Bridge",
            bg=self.background,
            fg=self.text,
            font=("Sans", 24, "bold"),
        ).pack(anchor="w")
        tk.Label(
            header,
            text="Native SteamVR interoperability and maintenance",
            bg=self.background,
            fg=self.muted,
            font=("Sans", 10),
        ).pack(anchor="w", pady=(5, 0))

        location = RoundedPanel(
            content,
            fill=self.surface,
            border=self.border,
            radius=20,
            height=132,
            padding=18,
        )
        location.pack(fill="x", pady=(0, 12))
        tk.Label(
            location.inner,
            text="Standable installation",
            bg=self.surface,
            fg=self.text,
            font=("Sans", 11, "bold"),
        ).pack(anchor="w")

        location_row = tk.Frame(location.inner, bg=self.surface)
        location_row.pack(fill="x", pady=(9, 7))
        field = RoundedPanel(
            location_row,
            fill=self.field,
            border="#3a435b",
            radius=13,
            height=44,
            padding=2,
        )
        field.pack(side="left", fill="x", expand=True)
        self.path_entry = tk.Entry(
            field.inner,
            textvariable=self.root_value,
            bg=self.field,
            fg=self.text,
            insertbackground=self.text,
            disabledbackground=self.field,
            disabledforeground=self.muted,
            relief="flat",
            bd=0,
            highlightthickness=0,
            font=("Sans", 9),
        )
        self.path_entry.pack(fill="both", expand=True, padx=12)
        self.browse_button = RoundedButton(
            location_row,
            "Browse",
            self._browse,
            width=88,
            height=42,
            fill=self.accent,
            hover=self.accent_hover,
        )
        self.browse_button.pack(side="left", padx=(9, 0))
        self.rescan_button = RoundedButton(
            location_row,
            "Rescan",
            self._start_discovery,
            width=88,
            height=42,
            fill="#343c55",
            hover="#424c68",
            foreground=self.text,
        )
        self.rescan_button.pack(side="left", padx=(8, 0))

        detected_row = tk.Frame(location.inner, bg=self.surface)
        detected_row.pack(fill="x")
        tk.Label(
            detected_row,
            textvariable=self.detected_value,
            bg=self.surface,
            fg=self.muted,
            font=("Sans", 8),
        ).pack(side="left", fill="x", expand=True)
        self.choose_button = RoundedButton(
            detected_row,
            "Choose detected",
            self._choose_detected,
            width=126,
            height=28,
            fill="#2d3449",
            hover="#3b445e",
            foreground=self.text,
            radius=10,
        )
        self.choose_button.pack(side="right")
        self.choose_button.set_enabled(False)

        self.banner = RoundedPanel(
            content,
            fill="#171d2a",
            border="#313a52",
            radius=18,
            height=76,
            padding=15,
        )
        self.banner.pack(fill="x", pady=(0, 12))
        self.banner_title = tk.Label(
            self.banner.inner,
            text="Checking installation…",
            bg=self.banner.fill,
            fg=self.text,
            font=("Sans", 10, "bold"),
        )
        self.banner_title.pack(anchor="w")
        self.banner_detail = tk.Label(
            self.banner.inner,
            text="Reading the selected Standable folder and bridge metadata.",
            bg=self.banner.fill,
            fg=self.muted,
            font=("Sans", 8),
            justify="left",
        )
        self.banner_detail.pack(anchor="w", pady=(4, 0))

        actions = tk.Frame(content, bg=self.background)
        actions.pack(fill="x", pady=(0, 10))
        actions.grid_columnconfigure(0, weight=1, uniform="actions")
        actions.grid_columnconfigure(1, weight=1, uniform="actions")
        self.action_buttons: list[RoundedButton] = []
        definitions = [
            ("Install", "Build and install the selected branch.", "install", False),
            ("Update", "Install the newest build for this installation.", "update", False),
            ("Repair", "Rebase the saved branch, uninstall, and reinstall.", "repair", False),
            ("Uninstall", "Restore the original manifest and remove bridge files.", "uninstall", True),
        ]
        for index, (title, description, action, danger) in enumerate(definitions):
            card = RoundedPanel(
                actions,
                fill=self.surface,
                border=self.border,
                radius=18,
                height=122,
                padding=17,
            )
            card.grid(
                row=index // 2,
                column=index % 2,
                sticky="nsew",
                padx=(0, 6) if index % 2 == 0 else (6, 0),
                pady=6,
            )
            tk.Label(
                card.inner,
                text=title,
                bg=self.surface,
                fg=self.text,
                font=("Sans", 12, "bold"),
            ).pack(anchor="w")
            tk.Label(
                card.inner,
                text=description,
                bg=self.surface,
                fg=self.muted,
                justify="left",
                wraplength=330,
                font=("Sans", 8),
            ).pack(anchor="w", pady=(4, 9))
            normal = self.destructive if danger else self.accent
            hover = "#f4bec9" if danger else self.accent_hover
            button = RoundedButton(
                card.inner,
                title,
                lambda selected=action: self._start(selected),
                width=108,
                height=34,
                fill=normal,
                hover=hover,
            )
            button.pack(anchor="w")
            self.action_buttons.append(button)

        status = tk.Frame(content, bg=self.background)
        status.pack(fill="x", pady=(2, 8))
        self.indicator = tk.Label(
            status,
            text="●",
            bg=self.background,
            fg=self.accent,
            font=("Sans", 12),
        )
        self.indicator.pack(side="left")
        tk.Label(
            status,
            textvariable=self.status_value,
            bg=self.background,
            fg=self.muted,
            font=("Sans", 9),
        ).pack(side="left", padx=(8, 0))

        log_panel = RoundedPanel(
            content,
            fill="#080b12",
            border="#20283a",
            radius=18,
            height=132,
            padding=12,
        )
        log_panel.pack(fill="both", expand=True)
        self.log = tk.Text(
            log_panel.inner,
            bg="#080b12",
            fg="#cfd5e7",
            insertbackground=self.text,
            relief="flat",
            bd=0,
            highlightthickness=0,
            wrap="word",
            font=("Monospace", 8),
            state="disabled",
        )
        self.log.pack(fill="both", expand=True)
        self.log.bind(
            "<MouseWheel>",
            lambda event: self.log.yview_scroll(int(-event.delta / 120), "units"),
        )
        self.log.bind("<Button-4>", lambda _event: self.log.yview_scroll(-3, "units"))
        self.log.bind("<Button-5>", lambda _event: self.log.yview_scroll(3, "units"))

    def _animate_status(self) -> None:
        if self.busy:
            frames = ("◐", "◓", "◑", "◒")
            self.indicator.configure(
                text=frames[self.spinner_index % len(frames)],
                fg=self.accent,
            )
            self.spinner_index += 1
        else:
            colors = (
                self.accent,
                blend(self.accent, self.background, 0.18),
                blend(self.accent, self.background, 0.34),
                blend(self.accent, self.background, 0.18),
            )
            self.indicator.configure(
                text="●",
                fg=colors[self.spinner_index % len(colors)],
            )
            self.spinner_index += 1
        self.after(180 if self.busy else 360, self._animate_status)

    def _path_changed(self, *_args: object) -> None:
        if self._path_refresh_id is not None:
            self.after_cancel(self._path_refresh_id)
        self._path_refresh_id = self.after(450, self._refresh_selected_status)

    def _browse(self) -> None:
        selected = filedialog.askdirectory(title="Select Standable Full Body Estimation")
        if selected:
            self.root_value.set(selected)

    def _start_discovery(self) -> None:
        if self.discovery_worker and self.discovery_worker.is_alive():
            return
        self.detected_value.set(
            "Scanning Steam libraries, removable drives, and saved installations…"
        )
        self.status_value.set("Scanning for Standable…")
        self.rescan_button.set_enabled(False)

        def run() -> None:
            try:
                installs = discover_installations()
                self.output_queue.put(("discover", installs))
            except Exception as error:
                self.output_queue.put(("discover_error", str(error)))

        self.discovery_worker = threading.Thread(target=run, daemon=True)
        self.discovery_worker.start()

    def _choose_detected(self) -> None:
        if not self.detected:
            return
        popup = tk.Toplevel(self)
        popup.title("Detected Standable installations")
        popup.configure(bg=self.background)
        popup.geometry("760x360")
        popup.minsize(620, 260)
        frame = tk.Frame(popup, bg=self.background)
        frame.pack(fill="both", expand=True, padx=24, pady=22)
        tk.Label(
            frame,
            text="Detected installations",
            bg=self.background,
            fg=self.text,
            font=("Sans", 17, "bold"),
        ).pack(anchor="w", pady=(0, 12))
        for info in self.detected:
            panel = RoundedPanel(
                frame,
                fill=self.surface,
                border=self.border,
                radius=15,
                height=58,
                padding=12,
            )
            panel.pack(fill="x", pady=5)
            button = RoundedButton(
                panel.inner,
                info.label,
                lambda selected=info: self._select_detected(selected, popup),
                width=680,
                height=34,
                fill="#2c344a",
                hover="#3b4662",
                foreground=self.text,
                radius=11,
            )
            button.pack(fill="x")

    def _select_detected(
        self,
        info: InstallationInfo,
        popup: tk.Toplevel | None = None,
    ) -> None:
        self.root_value.set(str(info.root))
        if popup is not None:
            popup.destroy()

    def _refresh_selected_status(self) -> None:
        raw = self.root_value.get().strip()
        if not raw:
            self._set_banner(
                "neutral",
                "No Standable installation selected",
                "Automatic detection is still available, or use Browse to select the original Steam folder.",
            )
            self._set_action_availability(None)
            return
        info = inspect_installation(Path(raw), "selected")
        self.status_info = info
        if info is None:
            self._set_banner(
                "error",
                "This is not a Standable installation",
                "The folder must contain Standable.exe, driver.vrdrivermanifest, and bin/win64/driver_standable.dll.",
            )
            self._set_action_availability(None)
            return

        self._set_action_availability(info)
        local = source_identity() if IS_SOURCE else {}
        self._apply_version_status(info, local, {})
        self.remote_generation += 1
        generation = self.remote_generation

        repository = info.repository or str(local.get("repository", DEFAULT_REPOSITORY))
        branch = info.branch or str(local.get("branch", "main"))

        def check() -> None:
            remote = fetch_remote_identity(repository, branch)
            self.output_queue.put(("remote", (generation, info.root, local, remote)))

        threading.Thread(target=check, daemon=True).start()

    def _apply_version_status(
        self,
        info: InstallationInfo,
        local: dict[str, str],
        remote: dict[str, str],
    ) -> None:
        available = local if IS_SOURCE else remote
        available_version = available.get("version", "")
        available_commit = available.get("commit", "")
        remote_commit = remote.get("commit", "")

        if info.kind == "original":
            self._set_banner(
                "neutral",
                "Standable is ready for installation",
                "No Linux bridge files were detected. Install will preserve the original Standable files and create rollback state.",
            )
            return

        if info.kind == "legacy":
            version = f" v{info.version}" if info.version else ""
            self._set_banner(
                "warning",
                f"Legacy bridge installation detected{version}",
                "This installation has bridge files but no complete ownership/manifest record. Use Repair to migrate it safely.",
            )
            return

        outdated = False
        reasons: list[str] = []
        if info.commit and available_commit and info.commit != available_commit:
            outdated = True
            reasons.append(
                f"installed {info.commit[:8]} • available {available_commit[:8]}"
            )
        elif (
            info.version
            and available_version
            and version_key(info.version) < version_key(available_version)
        ):
            outdated = True
            reasons.append(
                f"installed v{info.version} • available v{available_version}"
            )
        elif not info.commit and available_commit:
            reasons.append("installed build predates commit tracking")

        if (
            IS_SOURCE
            and local.get("commit")
            and remote_commit
            and local["commit"] != remote_commit
        ):
            reasons.append(
                f"checkout {local['commit'][:8]} • remote {remote_commit[:8]}"
            )

        if outdated:
            self._set_banner(
                "warning",
                "Your Standable Linux Bridge is outdated",
                "Update available: " + " • ".join(reasons),
            )
        elif reasons and "predates commit tracking" in reasons[0]:
            self._set_banner(
                "warning",
                "Bridge version needs verification",
                "The installed build predates commit tracking. Repair will rebuild it and create complete version metadata.",
            )
        elif (
            IS_SOURCE
            and local.get("commit")
            and remote_commit
            and local["commit"] != remote_commit
        ):
            self._set_banner(
                "warning",
                "The selected development branch changed online",
                "Pull the branch before installing, or use Repair to fetch and rebase it automatically.",
            )
        else:
            version = f"v{info.version}" if info.version else "installed"
            commit = f" • {info.commit[:8]}" if info.commit else ""
            self._set_banner(
                "success",
                "Standable Linux Bridge is up to date",
                f"{version}{commit} • original manifest backup and managed installation metadata found.",
            )

    def _set_banner(self, kind: str, title: str, detail: str) -> None:
        palette = {
            "neutral": ("#171d2a", "#343e58", self.accent),
            "success": ("#13251f", "#2d6752", self.success),
            "warning": ("#2a2116", "#76572d", self.warning),
            "error": ("#2a171d", "#7a3444", self.error),
        }
        fill, border, title_color = palette.get(kind, palette["neutral"])
        self.banner.set_colors(fill=fill, border=border)
        self.banner_title.configure(text=title, bg=fill, fg=title_color)
        self.banner_detail.configure(text=detail, bg=fill, fg=self.muted)

    def _set_action_availability(self, info: InstallationInfo | None) -> None:
        if info is None:
            enabled = (False, False, False, False)
        elif info.kind == "original":
            enabled = (True, False, False, False)
        else:
            enabled = (True, True, True, True)
        for button, state in zip(self.action_buttons, enabled):
            button.set_enabled(state and not self.busy)

    def _validated_root(self) -> Path | None:
        raw = self.root_value.get().strip()
        if not raw:
            messagebox.showerror(
                APP_NAME,
                "Select the Standable Full Body Estimation folder first.",
            )
            return None
        root = Path(raw).expanduser()
        if not is_standable_root(root):
            messagebox.showerror(
                APP_NAME,
                "That folder is not the original Standable installation.",
            )
            return None
        return root.resolve()

    def _start(self, action: str) -> None:
        if self.worker and self.worker.is_alive():
            return
        root = self._validated_root()
        if root is None:
            return
        info = inspect_installation(root, "selected")
        if action == "uninstall" and not messagebox.askyesno(
            APP_NAME,
            "Remove the Linux bridge and restore the original SteamVR manifest?\n\nSettings and saved poses are preserved.",
        ):
            return
        if action == "repair" and info is not None and info.kind == "original":
            messagebox.showinfo(
                APP_NAME,
                "No bridge installation exists yet. Use Install instead.",
            )
            return
        command = command_for(action, root)
        if not Path(command[1]).is_file():
            messagebox.showerror(
                APP_NAME,
                f"Required maintenance script is missing:\n{command[1]}",
            )
            return
        self._set_busy(True)
        self.status_value.set(f"{action.capitalize()} in progress…")
        self._append(f"$ {' '.join(command)}\n")
        self.worker = threading.Thread(
            target=self._run,
            args=(action, command, action_environment()),
            daemon=True,
        )
        self.worker.start()

    def _run(
        self,
        action: str,
        command: list[str],
        environment: dict[str, str],
    ) -> None:
        try:
            process = subprocess.Popen(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
                start_new_session=True,
                env=environment,
            )
            assert process.stdout is not None
            for line in process.stdout:
                self.output_queue.put(("line", line))
            status = process.wait()
        except OSError as error:
            self.output_queue.put(
                ("line", f"Could not start maintenance command: {error}\n")
            )
            status = 127
        self.output_queue.put(("done", (action, status)))

    def _drain_output(self) -> None:
        try:
            while True:
                kind, value = self.output_queue.get_nowait()
                if kind == "line":
                    self._append(str(value))
                elif kind == "discover":
                    self.rescan_button.set_enabled(True)
                    self.detected = list(value)  # type: ignore[arg-type]
                    if self.detected:
                        count = len(self.detected)
                        self.detected_value.set(
                            f"Detected {count} Standable installation{'s' if count != 1 else ''} across Steam libraries and saved state."
                        )
                        self.choose_button.set_enabled(True)
                        current = self.root_value.get().strip()
                        if not current or inspect_installation(
                            Path(current), "current"
                        ) is None:
                            self.root_value.set(str(self.detected[0].root))
                        else:
                            self._refresh_selected_status()
                    else:
                        self.detected_value.set(
                            "No Standable installation was found automatically. Use Browse to select it."
                        )
                        self.choose_button.set_enabled(False)
                        self._refresh_selected_status()
                    self.status_value.set("Ready")
                elif kind == "discover_error":
                    self.rescan_button.set_enabled(True)
                    self.detected_value.set(
                        "Automatic detection could not complete. Browse still works."
                    )
                    self.status_value.set("Ready")
                    self._append(f"Detection warning: {value}\n")
                elif kind == "remote":
                    generation, root, local, remote = value  # type: ignore[misc]
                    if (
                        generation == self.remote_generation
                        and self.status_info is not None
                        and self.status_info.root == root
                    ):
                        self._apply_version_status(self.status_info, local, remote)
                elif kind == "done":
                    action, status = value  # type: ignore[misc]
                    self._set_busy(False)
                    if status == 0:
                        self.status_value.set(
                            f"{str(action).capitalize()} completed"
                        )
                        self._append(
                            f"\n{str(action).capitalize()} completed successfully.\n"
                        )
                        self._start_discovery()
                        messagebox.showinfo(
                            APP_NAME,
                            f"{str(action).capitalize()} completed successfully.",
                        )
                    else:
                        self.status_value.set(f"{str(action).capitalize()} failed")
                        messagebox.showerror(
                            APP_NAME,
                            "The operation failed. Details are shown in the log.",
                        )
        except queue.Empty:
            pass
        self.after(75, self._drain_output)

    def _append(self, text: str) -> None:
        self.log.configure(state="normal")
        self.log.insert("end", text)
        self.log.see("end")
        self.log.configure(state="disabled")

    def _set_busy(self, busy: bool) -> None:
        self.busy = busy
        self.path_entry.configure(state="disabled" if busy else "normal")
        self.browse_button.set_enabled(not busy)
        self.rescan_button.set_enabled(not busy)
        self.choose_button.set_enabled(not busy and bool(self.detected))
        self._set_action_availability(self.status_info)


BridgeManager().mainloop()
PY
