# Compatibility

## Supported game

Only the researched x64 Steam executable is supported:

```text
OLGame.exe SHA-256
06c1c37c7f1d0bd0296328ff977ff847bee82f878fb4d79f4282a026aef68a96
```

The launcher validates it before creating a game process. Other storefronts,
patched executables, DLC-specific scenarios and arbitrary game updates are not
certified. The mod must not disable this check to accommodate an unknown build.

## Current limitations

- This is a preview, not a fully headset-accepted stable release.
- Reported scene-specific glass visibility, carpet/material artifacts, unusual
  scripted interactions and live headset frame pacing remain open.
- The default night-vision filter is a full-view color/visibility effect. It
  does not simulate camera optics, battery consumption or native camera HUD.
- The physical camcorder has unresolved integrated visibility/publication cases.
  It is disabled unless `experimental.physical_camcorder=true`.
- Native palm/IK and menu tests do not establish controller fit for every headset.
  Hand collision uses a palm proxy, not individual physical finger colliders.
- Touch, Index, Vive and WMR bindings exist. Face-button actions are not currently
  bound on Vive/WMR profiles; use Touch-compatible controllers for full controls.
  Index menu binding is runtime-dependent; Escape remains available.
- The desktop mirror is not a headset performance benchmark.

## Troubleshooting

Run `OutlastVRLauncher.exe --verify` first. Invalid settings and unsupported game
builds are explicit errors. Logs are written to `logs\OutlastVRSinglePass.log`
beside the mod DLL. Include that log, mod version, headset/runtime, game hash
and reproduction steps when reporting a problem; remove personal paths first.

Do not install this over another injector or replace `d3d9.dll`. Disable
conflicting overlays before diagnosing rendering issues. No game files or
saves are included in GitHub releases.
