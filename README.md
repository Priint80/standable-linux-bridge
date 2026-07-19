# Standable Linux Bridge

An unofficial interoperability bridge that lets native Linux SteamVR host the original Standable 3.0.3 Windows tracker provider.

This repository contains only newly written bridge code. It does not include Standable, Valve libraries, or any other proprietary application files. You must install and own Standable through Steam.

## Install

Requirements:

- Linux x86-64 with native SteamVR
- Steam running and signed in to an account that owns Standable
- the original **Standable Full Body Estimation** Steam installation
- Proton Experimental or Proton Hotfix installed through Steam
- `bash`, `curl`, `unzip`, and `sha256sum`

Close SteamVR, then run:

```bash
git clone https://github.com/Priint80/standable-linux-bridge.git
cd standable-linux-bridge
./install.sh
```

The installer finds Standable across your Steam library folders, verifies the bundled distribution (or downloads the latest release), installs only bridge-owned files, keeps the original DLL and UI untouched, and registers the driver with SteamVR.

After SteamVR starts, Standable appears as its own dashboard tab. The bridge enables Standable's existing dashboard preference and gives Proton the native SteamVR OpenVR runtime paths; the dashboard itself is still rendered by the original Standable UI.

If auto-detection does not find the app:

```bash
./install.sh --standable-root "/path/to/Standable Full Body Estimation"
```

This repository is public, so first installs and later updates need no GitHub authentication. The checkout also contains a verified fallback distribution. If you use a private fork, authenticate with `gh auth login` or provide `GITHUB_TOKEN` before an online update.

## Update

From the installed **Standable Full Body Estimation** folder, or from a source checkout cloned inside that folder:

```bash
./scripts/update.sh
```

The source checkout automatically uses its top-level installer. The installed copy uses its packaged updater engine.

Every install or update backs up replaced bridge files under:

```text
~/.local/state/standable-linux-bridge/backups/
```

## Troubleshooting

Generate a report from the installed Standable folder:

```bash
./scripts/diagnose.sh > standable-linux-diagnostics.txt 2>&1
```

Runtime logs are written to:

```text
~/.local/state/standable-linux-bridge/bridge.log
~/.local/state/standable-linux-bridge/ui.log
```

Useful overrides:

```bash
STEAMVR_ROOT=/custom/path/to/SteamVR ./scripts/install.sh
STANDABLE_PROTON="/path/to/Proton - Experimental/proton" ./scripts/install.sh
```

See [README-LINUX.md](README-LINUX.md) for the packaged-overlay instructions and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the runtime design.

## What the bridge preserves

The original Windows DLL remains responsible for authentication, calibration, tracker creation, pose estimation, settings, and UI communication. The helper mirrors physical OpenVR devices into the original provider and relays its tracker registrations, properties, and exact `DriverPose_t` output back to native SteamVR. It does not reimplement or alter Standable's tracking math.

Linux SteamVR legitimately does not ship the Windows SDK redistributable expected by the original DLL. The bridge-owned `steam_api64.dll` exports only the four functions imported by that DLL, connects to Proton's real `steamclient64.dll`, and refuses initialization unless the signed-in account reports ownership of App 2370570. No Valve binary is bundled and no ownership check is bypassed.

## Build and test

Build requirements are `g++`, GNU `make`, `binutils`, `zip`, and Zig. GCC builds the native provider; Zig cross-compiles the Windows helper and Steam adapter.

```bash
make ZIG=/path/to/zig
make release ZIG=/path/to/zig
make dist ZIG=/path/to/zig
```

Release products:

```text
build/Standable-Linux-Bridge-Overlay.zip
build/Standable-Linux-Bridge-Source-v1.2.1.zip
```

`make test` covers OpenVR factory negotiation, authenticated loopback transport, provider initialization, tracker registration/properties/pose relay, Proton selection, OpenVR runtime handoff, dashboard enablement, prefix setup, Steam-client discovery, UI launch, install, update, and driver registration. `make verify` checks binary architectures, exports, dependencies, scripts, and overlay layout.

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

The offline suite passes. A native SteamVR, Proton, and headset session is still needed for the first end-to-end hardware validation.
