# Architecture

## Runtime split

Native SteamVR loads `bin/linux64/driver_standable.so`. This provider mirrors connected device poses, selected properties, and OpenVR events over a session-scoped UDP channel bound only to `127.0.0.1`.

`scripts/standable-bridge-launcher.sh` selects Proton or Wine, prepares a private prefix for Steam App 2370570, and launches `standable_bridge_host.exe` and the original `Standable.exe` in that same prefix. The helper runs from its own `bin/win64` directory so normal Windows dependency resolution finds the bridge adapter beside it.

Before Proton starts, the launcher writes a bridge-owned `openvrpaths.vrpath` containing the discovered native SteamVR runtime and Steam config/log directories. It exports `VR_PATHREG_OVERRIDE`, `VR_OVERRIDE`, and `PROTON_VR_RUNTIME`, allowing Proton to populate its Windows OpenVR path registry and `vrclient` runtime even when the host user's default OpenVR path file is missing or stale.

## Native dashboard companion

`bin/linux64/standable_dashboard_overlay` is launched alongside the original UI. It loads SteamVR's native `bin/linux64/vrclient.so`, initializes as `VRApplication_Overlay`, and calls `CreateDashboardOverlay` to own the Standable dashboard tab. This follows the same native OpenVR application architecture used by Linux dashboard tools such as OVR Advanced Settings, without copying their GPL implementation.

The companion follows the lifetime of `vrserver`, not the short-lived Proton launcher process. This keeps the dashboard handle alive when Proton hands the Windows helper off to Wine and returns from its launcher command.

The companion discovers the Proton Standable window by its X11 title and class while explicitly excluding the headless `standable_bridge_host.exe` helper. It uses XComposite's named window pixmap when available and falls back to direct `XGetImage`. If Wine's top-level surface is black, it probes large visible child render surfaces and switches capture and input to the first usable one. It converts and scales the image to at most 1280×720, rejects transient blank captures, uploads frames into a persistent GLX/OpenGL texture, and submits that texture only while the Standable dashboard page is active. If GLX is unavailable, raw OpenVR image submissions are serialized using their asynchronous completion events so their backing buffers remain valid. SteamVR mouse and scroll events are translated back into X11 events for the unchanged UI.

The original Windows `Show in SteamVR Dashboard` preference is forced off by the Linux launcher. The native companion owns the sole functional Standable tab; leaving the Windows preference enabled creates a second blank dashboard entry under Proton.

This design intentionally targets X11/XWayland. Standard Wayland capture is permission-mediated and does not offer a portable silent API for duplicating an arbitrary application window. Proton's default Linux window path is XWayland; compositor-specific screencopy or portal capture can be added later without changing the dashboard-facing OpenVR layer.

The helper loads the unchanged `driver_standable.dll`, calls its exported `HmdDriverFactory`, and initializes `IServerTrackedDeviceProvider_004`. Its OpenVR compatibility host implements the interfaces requested by the original provider:

- server-driver host device registration and pose callbacks
- property reads, writes, and property containers
- settings, resources, driver log, driver input, and driver manager
- raw tracked poses and OpenVR event polling

The original provider registers its tracker objects and calculates their poses. The helper captures those registrations, activations, property writes, and pose updates. The native provider creates one relay object for each original tracker and forwards the original `DriverPose_t` values without changing their math.

## Steam authentication boundary

The original Windows DLL imports four Steamworks entry points from `steam_api64.dll`. Native Linux SteamVR does not ship that Windows SDK redistributable, so the bridge provides a narrow, newly written compatibility adapter exporting only those four symbols.

The adapter:

1. loads the `steamclient64.dll` made available inside the Proton prefix;
2. obtains a supported `ISteamClient` interface;
3. connects an API pipe to the running, signed-in Steam user;
4. obtains `ISteamApps008` and checks ownership of App 2370570;
5. forwards the original DLL's requested Steamworks interfaces through that authenticated client.

No Valve library is stored in this repository or release. Tracker relay objects are not registered until the unchanged original DLL authenticates, initializes, and registers them. The bridge neither fabricates ownership state nor synthesizes trackers after an authentication failure.

## Transport safety

Both endpoints bind fixed loopback addresses and reject packets unless all of these match:

- source address is `127.0.0.1`
- source port is the expected peer port
- protocol magic and version are valid
- per-SteamVR-start 64-bit session ID matches
- payload size matches the datagram
- FNV-1a payload checksum matches

The native endpoint marks all relayed trackers disconnected if the helper stops responding. The helper exits if the native SteamVR provider disappears. Logs rotate at 5 MiB to keep failures from growing state files indefinitely.

## Installation and updates

The top-level installer auto-detects Steam library folders, selects the bundled distribution or downloads the latest GitHub release, verifies `SHA256SUMS`, rejects unsafe archive paths, and copies only an explicit allow-list of bridge-owned files. If a release is unavailable, it can retrieve the same verified fallback from `dist/` in the repository. Existing bridge files are backed up before atomic replacement. It never overwrites or packages the proprietary Standable DLL, UI, resources, settings, or saved poses.

The installed `scripts/update.sh` reuses the same installer engine. GitHub Actions builds and tests every push; a versioned push creates immutable overlay, source, and checksum release assets.

## Preserved original behavior

The original application folder remains the resource root, so tracker profiles, render models, icons, sounds, localization, settings, custom poses, and named-pipe UI behavior remain available. The native companion mirrors the original UI rather than reimplementing it. The bridge changes only platform hosting, dashboard presentation, OpenVR runtime discovery, and pose transport.
