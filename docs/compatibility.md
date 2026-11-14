# Compatibility

## Game build

Outlast VR targets **64-bit Outlast**. Compatibility is determined by the
native code and memory layout, not an exact executable SHA-256, store label or
version string. Both the launcher and EXE/Steam autoload validate the native
ABI before installing hooks.

The current profile, `outlast-win64-native-1`, accepts the recovered native
layout even when these differ:

- PE timestamps/checksum and the unused DOS stub.
- Non-executable resources, fixed debug metadata and PDB identifiers.
- Raw section file offsets, unmapped padding, certificates and packaging overlays.

When that fast profile does not match, `outlast-win64-contracts-1` discovers
native bindings instead of imposing whole-section hashes or fixed addresses.
It checks complete primary/chained function bodies, native type-name anchors,
import identities, referenced globals and the D3D9 dispatch relationship.
Address fields are normalized; member offsets and scalar constants are not.
Absolute address fields require real loader relocations.

Native hooks, globals, interior input callsites and texture-allocation ownership
use the resolved addresses. Changes outside the required contracts can be
accepted, including moved native functions with consistent references and
unwind records. Missing, conflicting or ambiguous bindings fail before hooks
are installed. Changed renderer structure layouts still need validated
contracts; there is no unsafe force-load switch.

The reference executable used for reverse engineering and regression tests has
this SHA-256 (**reference identity, not an allowlist**):

```text
06c1c37c7f1d0bd0296328ff977ff847bee82f878fb4d79f4282a026aef68a96
```

Automated tests cover metadata/file-layout variants, unrelated native changes,
relocated hook bodies and callers, and rejection of changed rendering, layout,
dispatch, initial-global and malformed-PE contracts. Native discovery also
recovers independently checked functions and a policy global from the installed
alternate `OLGame_R.exe`; that mixed native/managed image is **not supported as
a playable build** because its complete startup/renderer ABI is not validated.

These are offline checks, not gameplay certification of every historical
Steam/GOG release. Additional retail editions remain unconfirmed until their
own native bindings and gameplay are validated.

Only Win64 is supported. Outlast 2 and The Outlast Trials are separate games and
are not supported.

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
