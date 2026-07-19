# Standable Full Body Estimation — Linux Bridge

This overlay adds a native Linux SteamVR driver while keeping Standable 3.0.3's original Windows provider and UI as the tracker solver. The original DLL still performs authentication, calibration, pose estimation, tracker creation, settings, and UI communication.

## Install the overlay manually

Requirements:

- Linux x86-64 and native SteamVR
- glibc 2.34 or newer with a current libstdc++ runtime
- Steam running and signed in to the account that owns Standable
- Proton Experimental or Proton Hotfix installed through Steam
- the original Standable 3.0.3 Steam installation

1. Close SteamVR.
2. Extract this overlay directly into the original **Standable Full Body Estimation** folder.
3. From that folder, run:

   ```bash
   ./scripts/install.sh
   ```

4. Start SteamVR. The bridge launches the original Standable UI automatically, and Standable appears as a dashboard tab. Proton's private prefix can take longer to initialize on the first launch.

The completed folder should include:

```text
Standable Full Body Estimation/
├── Standable.exe
├── driver.vrdrivermanifest
├── VERSION
├── bin/
│   ├── linux64/driver_standable.so
│   └── win64/
│       ├── driver_standable.dll
│       ├── standable_bridge_host.exe
│       └── steam_api64.dll
├── resources/
├── saves/
└── scripts/
```

The overlay does not replace `driver_standable.dll`, `Standable.exe`, resources, settings, or saved poses.

## SteamVR dashboard

Standable 3.0.3 already contains its own SteamVR dashboard renderer. The bridge passes the native SteamVR runtime, config, and log locations into the private Proton prefix so the unchanged UI can connect through `openvr_api.dll`. Installation also enables the existing **Show in SteamVR Dashboard** setting, preserving a timestamped settings backup if it needed to change the value.

To enable that setting again manually:

```bash
./scripts/enable-dashboard.sh
```

Close and restart SteamVR after changing it. If the tab is missing, run `./scripts/diagnose.sh`; its report now shows the source and Proton OpenVR path files, `vrclient_x64.dll`, the dashboard setting, and relevant UI log lines.

## Update

From this folder, run:

```bash
./scripts/update.sh
```

For a private GitHub repository, authenticate with `gh auth login` or provide `GITHUB_TOKEN`. Backups of replaced bridge files are stored under `~/.local/state/standable-linux-bridge/backups/`.

## Troubleshooting

Create a complete local report with:

```bash
./scripts/diagnose.sh > standable-linux-diagnostics.txt 2>&1
```

Bridge logs are stored at:

```text
~/.local/state/standable-linux-bridge/bridge.log
~/.local/state/standable-linux-bridge/ui.log
```

Useful overrides:

```bash
STEAMVR_ROOT=/custom/path/to/SteamVR ./scripts/install.sh
STANDABLE_PROTON="/path/to/Proton - Experimental/proton" ./scripts/install.sh
```

The runner override must also be present in SteamVR's environment when it starts. Set `STANDABLE_AUTOSTART_UI=0` in SteamVR's environment to disable automatic UI startup.

To unregister the driver without deleting files or settings:

```bash
./scripts/uninstall.sh
```

## Compatibility and integrity

This build targets the supplied Standable 3.0.3 driver with SHA-256:

```text
56f923ad96b46ba9f5b3c158ae0bcfeebe2bebc582b98968f397b67b2eeac9bf
```

Linux SteamVR does not need to contain `steam_api64.dll`. The overlay's newly written adapter loads Proton's authenticated `steamclient64.dll` and verifies that the signed-in Steam account owns Standable App 2370570. No Valve DLL is bundled, and the adapter does not forge or bypass ownership state.

The bridge transport accepts only session-matched UDP packets on fixed loopback ports at `127.0.0.1`. Offline binary, protocol, provider-relay, installer, and updater tests pass. A real SteamVR + headset + Proton session is still required for the first live hardware validation.
