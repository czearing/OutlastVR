# Troubleshooting

Start with `OutlastVRLauncher.exe --verify`. It checks the game build, mod and
OpenXR loader without launching the game.

## Startup errors

- **Game not found:** extract beside `Binaries\Win64\OLGame.exe`, or set
  `game.executable` in [settings](settings.md).
- **Unsupported executable:** compare your game build with
  [compatibility](compatibility.md). Do not bypass the version check.
- **Loader or runtime error:** extract every file from the release ZIP and set
  your headset software as the active OpenXR runtime. Start it before the mod.
- **Invalid setting:** use the key, type and range shown in the error and the
  settings reference.

## Height or facing direction

Recenter with F8, or hold both stick clicks for one second. For seated play,
enable seated mode and use button crouching.

## Performance or display problems

Lower the rendering resolution in your headset runtime, then restart the game.
Use the runtime's headset frame-time display when comparing settings. The
desktop mirror shows the left eye, not a stereo preview or headset frame-time
measurement.

Disable other injectors and conflicting overlays before reproducing a crash
or rendering problem. Do not replace `d3d9.dll` or copy Windows system DLLs
into the game folder.

## Report a problem

Check your version's [release notes](https://github.com/czearing/OutlastVR/releases)
and [existing issues](https://github.com/czearing/OutlastVR/issues), then
[open a bug report](https://github.com/czearing/OutlastVR/issues/new/choose).
Include:

- Mod version, headset/controllers, OpenXR runtime and GPU/driver.
- Steps to reproduce, scene/checkpoint, expected result and actual result.
- Relevant INI settings, including experimental options.
- An excerpt from `logs\OutlastVRSinglePass.log` beside the mod DLL.

Remove personal paths from logs. Do not upload game files, extracted assets
or saves.
