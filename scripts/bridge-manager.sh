#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
command -v python3 >/dev/null 2>&1 || {
    echo "Python 3 is required for the graphical Standable Linux Bridge manager." >&2
    exit 2
}

exec python3 - "$script_dir" "$@" <<'PY'
from __future__ import annotations

import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
from typing import Callable

APP_NAME = "Standable Linux Bridge"
SCRIPT_DIR = Path(sys.argv[1]).resolve()
ARGUMENTS = sys.argv[2:]
BASE_ROOT = SCRIPT_DIR.parent
IS_SOURCE = BASE_ROOT.joinpath("Makefile").is_file() and BASE_ROOT.joinpath("install.sh").is_file()


def discover_root() -> Path | None:
    candidates = [
        BASE_ROOT,
        Path.cwd(),
        Path.home() / ".local/share/Steam/steamapps/common/Standable Full Body Estimation",
        Path.home() / ".steam/steam/steamapps/common/Standable Full Body Estimation",
        Path.home() / ".var/app/com.valvesoftware.Steam/data/Steam/steamapps/common/Standable Full Body Estimation",
    ]
    for candidate in candidates:
        if candidate.joinpath("Standable.exe").is_file() and candidate.joinpath(
            "bin/win64/driver_standable.dll"
        ).is_file():
            return candidate.resolve()
    return None


def command_for(action: str, root: Path) -> list[str]:
    installed_scripts = root / "scripts"
    if action == "install":
        installer = BASE_ROOT / "install.sh" if IS_SOURCE else installed_scripts / "bridge-installer.sh"
        return ["bash", str(installer), "--standable-root", str(root)]
    if action == "update":
        return ["bash", str(installed_scripts / "update.sh")]
    if action == "repair":
        return ["bash", str(installed_scripts / "repair.sh")]
    if action == "uninstall":
        return ["bash", str(installed_scripts / "uninstall.sh")]
    raise ValueError(f"Unknown action: {action}")


def action_environment() -> dict[str, str]:
    environment = dict(os.environ)
    if not IS_SOURCE:
        return environment
    environment["STANDABLE_BRIDGE_SOURCE_CHECKOUT"] = str(BASE_ROOT)
    try:
        branch = subprocess.check_output(
            ["git", "-C", str(BASE_ROOT), "branch", "--show-current"],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        branch = ""
    if branch:
        environment["STANDABLE_BRIDGE_BRANCH"] = branch
    return environment


def self_test() -> int:
    fake = Path("/tmp/Standable Full Body Estimation")
    expected = {
        "install": "install.sh" if IS_SOURCE else "bridge-installer.sh",
        "update": "update.sh",
        "repair": "repair.sh",
        "uninstall": "uninstall.sh",
    }
    for action, name in expected.items():
        command = command_for(action, fake)
        if len(command) < 2 or Path(command[1]).name != name:
            print(f"manager self-test failed for {action}: {command}", file=sys.stderr)
            return 1
    print("bridge manager self-test passed")
    return 0


if "--self-test" in ARGUMENTS:
    raise SystemExit(self_test())

try:
    import tkinter as tk
    from tkinter import filedialog, messagebox, scrolledtext
except ImportError:
    print(
        "Python Tk support is not installed. Use install.sh, update.sh, repair.sh, "
        "or uninstall.sh directly.",
        file=sys.stderr,
    )
    raise SystemExit(3)


class BridgeManager(tk.Tk):
    background = "#0d1018"
    surface = "#171b27"
    raised = "#202638"
    text = "#eef0f7"
    muted = "#aab0c2"
    accent = "#aab2ff"
    accent_hover = "#c0c5ff"
    destructive = "#e8a9b7"

    def __init__(self) -> None:
        super().__init__()
        self.title(APP_NAME)
        self.geometry("790x610")
        self.minsize(690, 540)
        self.configure(bg=self.background)
        self.output_queue: queue.Queue[tuple[str, object]] = queue.Queue()
        self.worker: threading.Thread | None = None
        detected = discover_root()
        self.root_value = tk.StringVar(value=str(detected) if detected else "")
        self.status_value = tk.StringVar(value="Ready")
        self._build()
        self.after(75, self._drain_output)

    def _build(self) -> None:
        header = tk.Frame(self, bg=self.background)
        header.pack(fill="x", padx=34, pady=(28, 18))
        tk.Label(
            header,
            text="Standable Linux Bridge",
            bg=self.background,
            fg=self.text,
            font=("Sans", 23, "bold"),
        ).pack(anchor="w")
        tk.Label(
            header,
            text="Native SteamVR interoperability and maintenance",
            bg=self.background,
            fg=self.muted,
            font=("Sans", 11),
        ).pack(anchor="w", pady=(6, 0))

        location = tk.Frame(self, bg=self.surface, padx=20, pady=18)
        location.pack(fill="x", padx=34, pady=(0, 14))
        tk.Label(
            location,
            text="Standable installation",
            bg=self.surface,
            fg=self.text,
            font=("Sans", 11, "bold"),
        ).pack(anchor="w")
        row = tk.Frame(location, bg=self.surface)
        row.pack(fill="x", pady=(10, 0))
        self.path_entry = tk.Entry(
            row,
            textvariable=self.root_value,
            bg=self.raised,
            fg=self.text,
            insertbackground=self.text,
            relief="flat",
            font=("Sans", 10),
        )
        self.path_entry.pack(side="left", fill="x", expand=True, ipady=9)
        self._button(row, "Browse", self._browse, compact=True).pack(side="left", padx=(10, 0))

        actions = tk.Frame(self, bg=self.background)
        actions.pack(fill="x", padx=34, pady=(0, 14))
        actions.grid_columnconfigure(0, weight=1)
        actions.grid_columnconfigure(1, weight=1)
        self.action_buttons: list[tk.Button] = []
        definitions = [
            ("Install", "Add the native driver and dashboard bridge.", "install", False),
            ("Update", "Install the newest build for this installation.", "update", False),
            ("Repair", "Rebase the saved branch, uninstall, and reinstall.", "repair", False),
            ("Uninstall", "Restore the original manifest and remove bridge files.", "uninstall", True),
        ]
        for index, (title, description, action, danger) in enumerate(definitions):
            card = tk.Frame(actions, bg=self.surface, padx=18, pady=16)
            card.grid(
                row=index // 2,
                column=index % 2,
                sticky="nsew",
                padx=(0 if index % 2 == 0 else 7, 7 if index % 2 == 0 else 0),
                pady=7,
            )
            tk.Label(card, text=title, bg=self.surface, fg=self.text, font=("Sans", 12, "bold")).pack(anchor="w")
            tk.Label(
                card,
                text=description,
                bg=self.surface,
                fg=self.muted,
                justify="left",
                wraplength=300,
                font=("Sans", 9),
            ).pack(anchor="w", pady=(5, 12))
            button = self._button(card, title, lambda selected=action: self._start(selected), danger=danger)
            button.pack(anchor="w")
            self.action_buttons.append(button)

        status = tk.Frame(self, bg=self.background)
        status.pack(fill="x", padx=34, pady=(2, 8))
        self.indicator = tk.Label(status, text="●", bg=self.background, fg=self.accent, font=("Sans", 12))
        self.indicator.pack(side="left")
        tk.Label(
            status,
            textvariable=self.status_value,
            bg=self.background,
            fg=self.muted,
            font=("Sans", 10),
        ).pack(side="left", padx=(8, 0))

        self.log = scrolledtext.ScrolledText(
            self,
            height=9,
            bg="#090b11",
            fg="#cfd4e5",
            insertbackground=self.text,
            relief="flat",
            font=("Monospace", 9),
            state="disabled",
        )
        self.log.pack(fill="both", expand=True, padx=34, pady=(0, 28))

    def _button(
        self,
        parent: tk.Misc,
        label: str,
        command: Callable[[], None],
        *,
        compact: bool = False,
        danger: bool = False,
    ) -> tk.Button:
        normal = self.destructive if danger else self.accent
        hover = "#f1bec9" if danger else self.accent_hover
        button = tk.Button(
            parent,
            text=label,
            command=command,
            bg=normal,
            fg="#11131b",
            activebackground=hover,
            activeforeground="#11131b",
            relief="flat",
            bd=0,
            cursor="hand2",
            font=("Sans", 9, "bold"),
            padx=13 if compact else 17,
            pady=8,
        )
        button.bind("<Enter>", lambda _event: button.configure(bg=hover))
        button.bind("<Leave>", lambda _event: button.configure(bg=normal))
        return button

    def _browse(self) -> None:
        selected = filedialog.askdirectory(title="Select Standable Full Body Estimation")
        if selected:
            self.root_value.set(selected)

    def _validated_root(self) -> Path | None:
        raw = self.root_value.get().strip()
        if not raw:
            messagebox.showerror(APP_NAME, "Select the Standable Full Body Estimation folder first.")
            return None
        root = Path(raw).expanduser()
        if not root.joinpath("Standable.exe").is_file() or not root.joinpath(
            "bin/win64/driver_standable.dll"
        ).is_file():
            messagebox.showerror(APP_NAME, "That folder is not the original Standable installation.")
            return None
        return root.resolve()

    def _start(self, action: str) -> None:
        if self.worker and self.worker.is_alive():
            return
        root = self._validated_root()
        if root is None:
            return
        if action == "uninstall" and not messagebox.askyesno(
            APP_NAME,
            "Remove the Linux bridge and restore the original SteamVR manifest? "
            "Settings and saved poses are preserved.",
        ):
            return
        command = command_for(action, root)
        if not Path(command[1]).is_file():
            messagebox.showerror(APP_NAME, f"Required maintenance script is missing:\n{command[1]}")
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

    def _run(self, action: str, command: list[str], environment: dict[str, str]) -> None:
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
            self.output_queue.put(("line", f"Could not start maintenance command: {error}\n"))
            status = 127
        self.output_queue.put(("done", (action, status)))

    def _drain_output(self) -> None:
        try:
            while True:
                kind, value = self.output_queue.get_nowait()
                if kind == "line":
                    self._append(str(value))
                elif kind == "done":
                    action, status = value
                    self._set_busy(False)
                    if status == 0:
                        self.status_value.set(f"{str(action).capitalize()} completed")
                        messagebox.showinfo(APP_NAME, f"{str(action).capitalize()} completed successfully.")
                    else:
                        self.status_value.set(f"{str(action).capitalize()} failed")
                        messagebox.showerror(APP_NAME, "The operation failed. Details are shown in the log.")
        except queue.Empty:
            pass
        self.after(75, self._drain_output)

    def _append(self, text: str) -> None:
        self.log.configure(state="normal")
        self.log.insert("end", text)
        self.log.see("end")
        self.log.configure(state="disabled")

    def _set_busy(self, busy: bool) -> None:
        state = "disabled" if busy else "normal"
        for button in self.action_buttons:
            button.configure(state=state)
        self.path_entry.configure(state=state)
        self.indicator.configure(text="◌" if busy else "●")


BridgeManager().mainloop()
PY
