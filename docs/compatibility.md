# Compatibility

## Supported game

The supported version is the x64 Steam executable:

```text
OLGame.exe SHA-256
06c1c37c7f1d0bd0296328ff977ff847bee82f878fb4d79f4282a026aef68a96
```

The launcher checks this before starting the game. Other storefronts, patched
executables and DLC-specific scenarios are not supported or tested.

## Headsets and GPUs

Use the headset software's active OpenXR runtime. The mod uses the GPU selected
by that runtime and requires DirectX 12. Restart the game after changing the
runtime or headset.

Touch, Index, Vive and WMR bindings are provided. Not every headset/runtime/
controller combination has been tested. On controllers without face buttons,
click the stick/pad up or down for primary/secondary actions. If no menu binding
is available, press both secondary actions to pause.

## Current limitations

- This is a preview. Campaign progression, headset performance and controller
  alignment still need playtesting.
- Reported glass and security-office material artifacts have been corrected.
  Other scenes and scripted interactions still need playtesting.
- The default night-vision filter does not use batteries or display the native
  camera HUD.
- The physical camcorder has visibility issues and is disabled by default.
  Enabling it requires right-handed controls.
- Interaction uses the game's controls, not free physics grabbing or finger
  collision. Settings are edited in the INI; there is no in-VR settings menu.
- The desktop mirror shows the left eye.

## Troubleshooting

Run `OutlastVRLauncher.exe --verify` first. Invalid settings and unsupported game
builds are explicit errors. Logs are written to `logs\OutlastVRSinglePass.log`
beside the mod DLL. Include that log, mod version, headset/runtime, game hash
and reproduction steps when reporting a problem; remove personal paths first.

Do not install this over another injector or replace `d3d9.dll`. Disable
conflicting overlays before diagnosing rendering issues. No game files or
saves are included in GitHub releases.
