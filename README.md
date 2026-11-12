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
3. Start your headset runtime, then run `OutlastVRLauncher.exe`.

Alternatively, extract anywhere and set `[game] executable` in `OutlastVR.ini`
to your `OLGame.exe` path. `OutlastVRLauncher.exe --verify` checks compatibility
without launching the game.

## Player guide

| Guide | Contents |
| --- | --- |
| [Controls](https://github.com/czearing/OutlastVR/blob/master/docs/controls.md) | Bindings, handedness and recentering |
| [Settings](https://github.com/czearing/OutlastVR/blob/master/docs/settings.md) | Comfort options, defaults and experimental features |
| [Troubleshooting](https://github.com/czearing/OutlastVR/blob/master/docs/troubleshooting.md) | Startup checks, performance and bug reports |

## Updating and uninstalling

Close the game before updating. Back up `OutlastVR.ini`, extract the new release,
then restore your settings. Check the release notes for any new options.

The mod does not replace the game executable. Launch through Steam to play
without VR. To uninstall, remove only the files supplied by the mod ZIP.

## Development

[Build from source](https://github.com/czearing/OutlastVR/blob/master/docs/building.md) ·
[Testing](https://github.com/czearing/OutlastVR/blob/master/docs/testing.md) ·
[Contributing](https://github.com/czearing/OutlastVR/blob/master/CONTRIBUTING.md)

MIT-licensed original code. See [third-party notices](THIRD_PARTY_NOTICES.md).
Not affiliated with or endorsed by Red Barrels.
