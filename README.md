# Outlast VR

[![Build](https://github.com/czearing/OutlastVR/actions/workflows/build.yml/badge.svg)](https://github.com/czearing/OutlastVR/actions/workflows/build.yml)
[![Downloads](https://img.shields.io/github/downloads/czearing/OutlastVR/total)](https://github.com/czearing/OutlastVR/releases)

An unofficial OpenXR VR mod for **Outlast** on Windows. Tracked head and hands,
controller interaction, and single-pass stereo rendering: one instanced world
submission for both eyes, not two scene renders.

**Preview release.** The default experience uses a thumbstick-toggled full-view
night-vision filter. The physical camcorder is experimental and disabled by
default. Automated checks do not certify every scene or headset.

## Requirements

- The supported **64-bit Steam version of Outlast** (not Outlast 2 or The Trials).
- Windows 10/11, a DirectX 12-capable GPU, and an OpenXR headset/runtime.
- Set your headset software as the active OpenXR runtime before launching.
- Use Outlast's default gamepad layout (A).

Unsupported executables are rejected before hooks are installed. See
[compatibility and known issues](https://github.com/czearing/OutlastVR/blob/main/docs/compatibility.md).

## Install

1. Download the Windows ZIP from [Releases](https://github.com/czearing/OutlastVR/releases).
2. Extract **all** files beside `Outlast\Binaries\Win64\OLGame.exe`.
3. Start your headset runtime, then run `OutlastVRLauncher.exe`.

Alternatively, extract anywhere and set `[game] executable` in `OutlastVR.ini`
to your `OLGame.exe` path. `OutlastVRLauncher.exe --verify` checks compatibility
without launching the game.

The launcher does not replace the game executable. To play normally, launch
Outlast through Steam. To uninstall, remove only the extracted mod files.

## Controls

| Input | Action |
| --- | --- |
| Left stick / right stick | Move / turn |
| Right stick click | Toggle full-view night-vision filter |
| Left stick click | Sprint |
| Right trigger | Interact; select pointed menu items |
| Right primary / secondary | Jump or confirm / crouch or back |
| Left primary / secondary | Notes / reload battery |
| Left menu button | Pause |
| F8 | Recenter |

Button names follow your controller's OpenXR bindings. Index/Vive/WMR profile
support varies; see [compatibility](https://github.com/czearing/OutlastVR/blob/main/docs/compatibility.md).

## Configuration

Edit `OutlastVR.ini` beside the launcher, then restart the game.

```ini
[experimental]
physical_camcorder=false
```

Leave this off for the simple filter and no held-camera capture/rendering.
Opting in enables the work-in-progress grip-held camcorder and routes right
stick click to its native night vision. It is not recommended for normal play.
The simple filter is a visibility effect, not the game's battery-powered camera.

## Development

[Building and testing](https://github.com/czearing/OutlastVR/blob/main/docs/building.md) · [Contributing](https://github.com/czearing/OutlastVR/blob/main/CONTRIBUTING.md) ·
[Report a bug](https://github.com/czearing/OutlastVR/issues/new/choose)

MIT-licensed original code. See [third-party notices](THIRD_PARTY_NOTICES.md).
Not affiliated with or endorsed by Red Barrels.
