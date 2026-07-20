# Standable Full Body Estimation — Linux Bridge

This overlay adds a native Linux SteamVR driver while keeping Standable 3.0.3's original Windows provider and UI as the tracker solver. The original DLL still performs authentication, calibration, pose estimation, tracker creation, settings, UI communication, and front-end property generation.

## Install the overlay

Requirements:

- Linux x86-64 and native SteamVR
- glibc 2.34 or newer with a current libstdc++ runtime
- `libX11` and `libXtst`; `libXcomposite` is recommended
- Steam running and signed in to the account that owns Standable
- Proton Experimental or Proton Hotfix installed through Steam
- the original Standable 3.0.3 Steam installation
- Python 3 with Tk support for the graphical manager

1. Close SteamVR.
2. Extract this overlay directly into the original **Standable Full Body Estimation** folder.
3. From that folder, open the maintenance window:

   ```bash
   ./scripts/bridge-manager.sh
   ```

4. Choose **Install**.
5. Start SteamVR. The bridge launches the original Standable UI and native dashboard companion automatically. Proton's private prefix can take longer to initialize on its first launch.

CLI installation remains available:

```bash
./scripts/install.sh
```

The completed folder includes:

```text
Standable Full Body Estimation/
├── Standable.exe
├── driver.vrdrivermanifest
├── VERSION
├── bin/
│   ├── linux64/
│   │   ├── driver_standable.so
│   │   └── standable_dashboard_overlay
│   └── win64/
│       ├── driver_standable.dll
│       ├── standable_bridge_host.exe
│       └── steam_api64.dll
├── resources/
├── saves/
├── share/standable-linux-bridge/
│   └── driver.vrdrivermanifest
└── scripts/
```

The overlay does not replace `driver_standable.dll`, `Standable.exe`, resources, settings, or saved poses.

## Managed SteamVR manifest

Installation saves an exact copy of the original `driver.vrdrivermanifest` before writing the native-Linux-compatible manifest. It preserves the original driver name and other front-end fields, then normalizes only the directory, activation, and resource-only fields needed by native SteamVR.

If the preserved driver name requires a different Linux binary filename, the installer creates a matching alias to `driver_standable.so`. Uninstall restores the original manifest byte-for-byte and removes the generated alias.

## SteamVR dashboard

`standable_dashboard_overlay` creates the real native SteamVR dashboard tab, captures the unchanged Standable window through X11/XComposite, uploads frames through a persistent OpenGL texture, and forwards controller pointer input into Wine.

The observed live failure before this revision was specific: controller motion moved the virtual mouse and hover highlights appeared, but trigger clicks did not activate controls. The revised input path keeps pointer coordinates on the captured render surface, focuses the top-level Standable window for activation, synchronizes successful XTEST button events, and holds each press briefly before SteamVR can deliver its release.

The original Windows **Show in SteamVR Dashboard** preference is disabled so SteamVR shows one functional Standable tab instead of a duplicate blank Proton entry.

On Wayland, Proton should use XWayland. A native-Wayland Wine window cannot be duplicated silently through a portable API.

To reapply the dashboard configuration manually:

```bash
./scripts/enable-dashboard.sh
```

Restart SteamVR after changing it. A working input session logs `XTest=yes`, `SteamVR pointer events received`, and `pointer button=1` entries in the dashboard log.

## Maintenance

Open the graphical manager with:

```bash
./scripts/bridge-manager.sh
```

- **Update** installs the newest packaged bridge build.
- **Repair** reads the saved repository and branch. It rebases a persistent source checkout when available, or temporarily clones that branch, builds a fresh overlay, runs uninstall, and reinstalls the build.
- **Uninstall** unregisters the bridge, restores the exact original manifest and any pre-existing files, and removes only bridge-owned files.

CLI equivalents:

```bash
./scripts/update.sh
./scripts/repair.sh
./scripts/uninstall.sh
```

Backups and ownership records are stored beneath:

```text
~/.local/state/standable-linux-bridge/
```

## Troubleshooting

Create a privacy-redacted support report with:

```bash
./scripts/diagnose.sh > standable-linux-diagnostics.txt 2>&1
```

Use `--full-paths` only for private local debugging. The default report replaces the home directory with `~`, replaces the Standable installation with `<standable-root>`, and limits the SteamVR registration section to Standable-related entries.

Bridge logs are stored at:

```text
~/.local/state/standable-linux-bridge/bridge.log
~/.local/state/standable-linux-bridge/ui.log
~/.local/state/standable-linux-bridge/dashboard.log
```

Useful overrides:

```bash
STEAMVR_ROOT=/custom/path/to/SteamVR ./scripts/install.sh
STANDABLE_PROTON="/path/to/Proton - Experimental/proton" ./scripts/install.sh
```

The runner override must also be present in SteamVR's environment when it starts. `STANDABLE_AUTOSTART_UI=0` disables automatic UI startup. `STANDABLE_AUTOSTART_DASHBOARD=0` disables only the native dashboard companion. `STANDABLE_DASHBOARD_FPS=20` controls capture rate from 1–60 FPS.

## Compatibility and integrity

This build targets the supplied Standable 3.0.3 driver with SHA-256:

```text
56f923ad96b46ba9f5b3c158ae0bcfeebe2bebc582b98968f397b67b2eeac9bf
```

The bridge-owned Steam adapter loads Proton's authenticated `steamclient64.dll` and verifies ownership of Standable App 2370570. No Valve DLL is bundled, and the adapter does not forge or bypass ownership state.

The transport accepts only session-matched UDP packets on fixed loopback ports at `127.0.0.1`. The revised trigger-click path still needs a real SteamVR, Proton, and headset validation before release.
