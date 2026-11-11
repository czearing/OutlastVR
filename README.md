# Outlast VR

[![Build](https://github.com/czearing/OutlastVR/actions/workflows/build.yml/badge.svg?branch=master)](https://github.com/czearing/OutlastVR/actions/workflows/build.yml)
[![Downloads](https://img.shields.io/github/downloads/czearing/OutlastVR/total)](https://github.com/czearing/OutlastVR/releases)

An unofficial OpenXR VR mod for **Outlast** on Windows. Tracked head and hands,
controller interaction, and single-pass stereo rendering.

**Preview release.** The default experience uses a thumbstick-toggled full-view
night-vision filter. The physical camcorder is experimental and disabled by
default. See [known issues](https://github.com/czearing/OutlastVR/blob/master/docs/compatibility.md)
before installing.

## Requirements

- The supported **64-bit Steam version of Outlast** (not Outlast 2 or The Trials).
- Windows 10/11, a DirectX 12-capable GPU, and an OpenXR headset/runtime.
- Set your headset software as the active OpenXR runtime before launching.
- Use Outlast's default gamepad layout (A).

The launcher checks your game version before starting.

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

Default right-handed bindings:

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
| Hold both stick clicks for one second | Recenter without a keyboard |

Button names follow your controller's OpenXR bindings. Index/Vive/WMR profile
support varies; see [compatibility](https://github.com/czearing/OutlastVR/blob/master/docs/compatibility.md).

On Vive/WMR profiles without active face buttons, click the stick/pad upward
for primary or downward for secondary. Horizontal/center clicks retain
sprint/filter. If the runtime exposes no menu binding, press both secondary
actions together to pause. These fallbacks do not require a system-menu button.

## Configuration

Edit `OutlastVR.ini` beside the launcher, then restart the game.

The `[vr]` settings include `snap_turn_degrees` (0 for smooth, 30 for snap),
`seated`, `left_handed`, `toggle_sprint`, `movement_reference` (`game`, `head`, `main_hand`,
`off_hand`), `world_scale` (0.5..2) and `vignette_strength` (0..1).
Left-handed mode swaps the controls. Seated mode uses button crouching.
With `toggle_sprint=true`, click again or stop moving to stop sprinting.

```ini
[experimental]
physical_camcorder=false
```

Leave this off for normal play. The experimental camcorder uses right-handed
controls and the game's night vision. The default filter does not use batteries.

## Development

[Building and testing](https://github.com/czearing/OutlastVR/blob/master/docs/building.md) · [Contributing](https://github.com/czearing/OutlastVR/blob/master/CONTRIBUTING.md) ·
[Report a bug](https://github.com/czearing/OutlastVR/issues/new/choose)

MIT-licensed original code. See [third-party notices](THIRD_PARTY_NOTICES.md).
Not affiliated with or endorsed by Red Barrels.
