# Compatibility

## Game build

Outlast VR targets the **64-bit Steam version of Outlast**. The launcher accepts
`Binaries\Win64\OLGame.exe` with this SHA-256:

```text
06c1c37c7f1d0bd0296328ff977ff847bee82f878fb4d79f4282a026aef68a96
```

Other executables are rejected before launch. Outlast 2 and The Outlast Trials
are separate games and are not supported.

## Windows and graphics

- Windows 10 or 11, 64-bit.
- A DirectX 12-capable GPU.
- An OpenXR runtime configured as active in your headset software.

The mod uses the GPU selected by the OpenXR runtime. Restart the game after
changing runtimes or headsets.

## Controllers

OpenXR bindings are included for Oculus Touch, Valve Index, HTC Vive and
Windows Mixed Reality controllers. The active runtime determines which
interaction profile is available.

See [controls](controls.md) for button names and trackpad fallbacks.

## Verify your setup

Run `OutlastVRLauncher.exe --verify` to check the game executable, mod and
OpenXR loader without starting Outlast. For errors, see
[troubleshooting](troubleshooting.md).
