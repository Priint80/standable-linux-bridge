# Standable Linux Bridge

An unofficial interoperability bridge that lets native Linux SteamVR host the original Standable 3.0.3 Windows tracker provider.

This repository contains only newly written bridge code. It does not include Standable, Valve libraries, or any other proprietary application files. You must install and own Standable through Steam.

## Install

Requirements:

- Linux x86-64 with native SteamVR
- X11 or a Wayland desktop with XWayland
- `libX11` and `libXtst`; `libXcomposite` is recommended
- Steam running and signed in to an account that owns Standable
- the original **Standable Full Body Estimation** Steam installation
- Proton Experimental or Proton Hotfix installed through Steam
- `bash`, `curl`, `unzip`, and `sha256sum`
- Python 3 with Tk support for the graphical manager; the CLI remains available without Tk

Close SteamVR, clone the repository, and open the maintenance window:

```bash
git clone https://github.com/Priint80/standable-linux-bridge.git
cd standable-linux-bridge
./scripts/bridge-manager.sh
```

Select the original **Standable Full Body Estimation** folder and choose **Install**.

CLI installation is still available:

```bash
./install.sh
```

If auto-detection does not find the app:

```bash
./install.sh --standable-root "/path/to/Standable Full Body Estimation"
```

The installer verifies the bridge distribution, copies only bridge-owned files, keeps the original Standable executable, Windows provider, settings, resources, and saved poses untouched, and registers the native driver with SteamVR.

## Dashboard behavior

After SteamVR starts, Standable appears as its own dashboard tab. A native Linux companion creates the tab, captures the unchanged Proton/XWayland Standable window, uploads it through a persistent OpenGL texture, and forwards SteamVR pointer events into Wine.

The dashboard input path deliberately uses the original Standable UI rather than recreating its controls. Controller motion is mapped to the captured Wine surface. Button delivery focuses the top-level Standable window and synchronizes each XTEST press before its release so Wine cannot observe only the hover or release half of a click.

The original Windows **Show in SteamVR Dashboard** preference is disabled to avoid a second blank Proton dashboard entry.

## Update, repair, and uninstall

Run the graphical manager from the installed Standable folder:

```bash
./scripts/bridge-manager.sh
```

It provides four actions:

- **Install** — installs and registers the native bridge
- **Update** — installs the newest packaged build while preserving installation provenance
- **Repair** — rebases the recorded source checkout against its corresponding branch, builds a fresh overlay, runs uninstall, and reinstalls it; when no persistent checkout exists, it clones that recorded branch into a temporary build tree
- **Uninstall** — unregisters the driver, restores the exact original `driver.vrdrivermanifest`, restores pre-existing files, and removes only bridge-owned files

CLI equivalents:

```bash
./scripts/update.sh
./scripts/repair.sh
./scripts/uninstall.sh
```

Every installation stores an exact original-manifest backup and per-file ownership record under:

```text
~/.local/state/standable-linux-bridge/installations/
```

Timestamped pre-update snapshots remain under:

```text
~/.local/state/standable-linux-bridge/backups/
```

The managed SteamVR manifest preserves the original driver name and other front-end fields, changes only the platform-hosting fields needed by native Linux SteamVR, and creates a matching native binary alias when the original driver name requires one.

## Troubleshooting

Generate a privacy-redacted support report from the installed Standable folder:

```bash
./scripts/diagnose.sh > standable-linux-diagnostics.txt 2>&1
```

Home and Standable paths are redacted by default. Exact paths can be included for private local debugging with:

```bash
./scripts/diagnose.sh --full-paths
```

Runtime logs are written to:

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

See [README-LINUX.md](README-LINUX.md) for packaged-overlay instructions and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the runtime design.

## What the bridge preserves

The original Windows DLL remains responsible for authentication, calibration, tracker creation, pose estimation, settings, UI communication, display names, tracker properties, and resource identities. The helper mirrors physical OpenVR devices into the original provider and relays its tracker registrations, properties, and exact `DriverPose_t` output back to native SteamVR. It does not reimplement or alter Standable's tracking math.

Linux SteamVR does not ship the Windows SDK redistributable expected by the original DLL. The bridge-owned `steam_api64.dll` exports only the four functions imported by that DLL, connects to Proton's authenticated `steamclient64.dll`, and refuses initialization unless the signed-in account reports ownership of App 2370570. No Valve binary is bundled and no ownership check is bypassed.

## Build and test

Build requirements are `g++`, GNU `make`, `binutils`, `zip`, and Zig. GCC builds the native provider and dashboard companion; Zig cross-compiles the Windows helper and Steam adapter.

```bash
make ZIG=/path/to/zig
make release ZIG=/path/to/zig
make dist ZIG=/path/to/zig
```

Release products:

```text
build/Standable-Linux-Bridge-Overlay.zip
build/Standable-Linux-Bridge-Source-v1.3.5.zip
```

`make test` covers OpenVR factory negotiation, authenticated loopback transport, provider initialization, tracker registration/properties/pose relay, the dashboard companion, Proton selection, OpenVR runtime handoff, prefix setup, Steam-client discovery, UI launch, managed-manifest behavior, installation, update, and driver registration. `make verify` checks architectures, exports, dependencies, privacy-safe debug output, maintenance scripts, and overlay layout.

For an integration check against a local original installation:

```bash
make integration \
  ZIG=/path/to/zig \
  ORIGINAL_ROOT="/path/to/Standable Full Body Estimation"
```

The current compatibility target is the supplied Standable 3.0.3 driver with SHA-256:

```text
56f923ad96b46ba9f5b3c158ae0bcfeebe2bebc582b98968f397b67b2eeac9bf
```

A native SteamVR, Proton, and headset session is still required to verify the revised trigger-click path against the real Standable UI before release.
