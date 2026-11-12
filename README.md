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
| Right grip | Grab / interact |
| Either trigger | Select pointed menu items |
| Right primary / secondary | Jump or confirm / crouch or back |
| Left primary / secondary | Notes / reload battery |
| Left menu button | Pause |
| F8 | Recenter |
| Hold both stick clicks for one second | Recenter without a keyboard |

Button names follow your controller's OpenXR bindings. Index/Vive/WMR profile
support varies; see [compatibility](https://github.com/czearing/OutlastVR/blob/master/docs/compatibility.md).

VR hints use controller-neutral action names, currently in English. Primary means
the lower face button (A/X on Touch, A on Index); secondary means the upper face
button (B/Y on Touch, B on Index). Hints follow left-handed mode.

On Vive/WMR profiles without active face buttons, click the stick/pad upward
for primary or downward for secondary. Horizontal/center clicks retain
sprint/filter. If the runtime exposes no menu binding, press both secondary
actions together to pause. These fallbacks do not require a system-menu button.

## Configuration

Edit `OutlastVR.ini` beside the launcher, then restart the game.

The `[vr]` settings include `snap_turn_degrees` (0 for smooth, 30 for snap),
`seated`, `left_handed`, `toggle_sprint`, `movement_reference` (`game`, `head`, `main_hand`,
`off_hand`), `world_scale` (0.5..2) and `vignette_strength` (0..1).
Left-handed mode swaps gameplay controls. Either trigger can select menu items.
Seated mode uses button crouching.
Inventory pickups complete immediately; doors and other scripted interactions
keep their native behavior.
With `toggle_sprint=true`, click again or stop moving to stop sprinting.

```ini
[experimental]
physical_camcorder=false
instant_pickups=true
```

Leave `physical_camcorder` off for normal play. The experimental camcorder can
be grabbed with either hand near its chest holster and uses the game's night
vision. Its grip is reserved while held, so grabbing it does not also interact
with a door/item. Both-handed controls are covered by automated tests; physical
visibility, optical alignment and comfort still require headset acceptance.
The default filter does not use batteries.

Set `instant_pickups=false` to retain native inventory pickup animations and
completion. Native item admission, inventory events and scripted interactions
are not bypassed by either setting.

## Development

[Building and testing](https://github.com/czearing/OutlastVR/blob/master/docs/building.md) · [Contributing](https://github.com/czearing/OutlastVR/blob/master/CONTRIBUTING.md) ·
[Report a bug](https://github.com/czearing/OutlastVR/issues/new/choose)

The [unattended acceptance runner](https://github.com/czearing/OutlastVR/blob/master/docs/testing.md#unattended-acceptance)
checks actual DLL/GPU output without launching Outlast. It cannot certify
headset ergonomics, performance or campaign completion.

MIT-licensed original code. See [third-party notices](THIRD_PARTY_NOTICES.md).
Not affiliated with or endorsed by Red Barrels.
