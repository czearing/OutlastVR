# Outlast VR

[![Build](https://github.com/czearing/OutlastVR/actions/workflows/build.yml/badge.svg?branch=master)](https://github.com/czearing/OutlastVR/actions/workflows/build.yml)
[![Downloads](https://img.shields.io/github/downloads/czearing/OutlastVR/total)](https://github.com/czearing/OutlastVR/releases)

An unofficial OpenXR mod for **Outlast**, with tracked head and hands,
motion-controller input and single-pass stereo rendering.

Requires the supported 64-bit Steam executable, Windows 10/11, a DirectX 12 GPU
and an active OpenXR runtime. See [compatibility](https://github.com/czearing/OutlastVR/blob/master/docs/compatibility.md)
for the accepted game build and controller profiles.

## Install

1. Download the Windows ZIP from [Releases](https://github.com/czearing/OutlastVR/releases).
   Read that version's release notes for changes and known issues.
2. Extract **all** files beside `Outlast\Binaries\Win64\OLGame.exe`.
3. Start your headset runtime, then launch Outlast normally through Steam or
   `OLGame.exe`. The included `d3d9.dll` loads VR automatically.

The **Outlast VR - Desktop** window shows the final left eye for desktop viewing
and recording. In OBS, use Window Capture for that window or capture the display.
It copies the existing VR image; it does not render the world a second time.
Back up any existing third-party `d3d9.dll` before installing; do not overwrite
another injector without keeping a rollback.

Alternatively, extract anywhere and set `[game] executable` in `OutlastVR.ini`
to your `OLGame.exe` path. Run `OutlastVRLauncher.exe` to start VR from that
folder; `OutlastVRLauncher.exe --verify` checks compatibility without launching.

## Player guide

| Guide | Contents |
| --- | --- |
| [Controls](https://github.com/czearing/OutlastVR/blob/master/docs/controls.md) | Bindings, handedness and recentering |
| [Settings](https://github.com/czearing/OutlastVR/blob/master/docs/settings.md) | Comfort options, defaults and experimental features |
| [Troubleshooting](https://github.com/czearing/OutlastVR/blob/master/docs/troubleshooting.md) | Startup checks, performance and bug reports |

## Updating and uninstalling

Close the game before updating. Back up `OutlastVR.ini`, extract the new release,
then restore your settings. Check the release notes for any new options.

The mod does not replace the game executable. To launch normally without VR,
rename the mod's `d3d9.dll` to `d3d9.dll.disabled` and do not use the VR launcher.
To uninstall, remove only the files supplied by the mod ZIP and restore any
previously backed-up injector.

## Development

[Build from source](https://github.com/czearing/OutlastVR/blob/master/docs/building.md) ·
[Testing](https://github.com/czearing/OutlastVR/blob/master/docs/testing.md) ·
[Contributing](https://github.com/czearing/OutlastVR/blob/master/CONTRIBUTING.md)

MIT-licensed original code. See [third-party notices](THIRD_PARTY_NOTICES.md).
Not affiliated with or endorsed by Red Barrels.
