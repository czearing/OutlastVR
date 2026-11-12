# Building

## Requirements

- Windows x64, CMake 3.24+, Ninja and Git.
- MinGW-w64 GCC. CI uses MSYS2 UCRT64.
- Windows 10/11 SDK with `d3d9on12.h`.
- Python 3.10+.

## Build and test

Run in a shell with `gcc`, `g++`, `cmake`, `ninja` and `python` on PATH:

```powershell
python -m pip install -r tools\diagnostics\requirements.txt
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

CMake fetches MinHook and the OpenXR loader at pinned revisions. If SDK discovery
fails, set `-DOLVR_WINDOWS_SDK_UM=<SDK Include\version\um>`.

## Optional test inputs

| CMake option | Purpose |
| --- | --- |
| `OLVR_GPU_TESTS=ON` | Enable real D3D12 hardware tests. |
| `OLVR_RETAIL_EXECUTABLE=<absolute OLGame.exe>` | Enable offline tests against the supported executable. |
| `OLVR_HERO_FIXTURE=<rig file>` | Enable tests using a locally retained native rig. |
| `OLVR_BUILD_DIAGNOSTICS=ON` | Build developer-only probe and rendering diagnostics. |

Pass these with `-D` when configuring. GPU tests are not run on GitHub's hosted
runners. Retail inputs and rig captures are private and must not be distributed.
Diagnostic tools are not included in release packages. See [testing](testing.md)
for unattended and headset checks.

## Package a release

Commit the source changes first. Set `$version` to the intended release version,
without a leading `v`. Its numeric part must match the CMake project version.

```powershell
.\tools\package.ps1 -Version $version
python tools\verify_package.py "dist\OutlastVR-$version-win64"
```

Packaging records the source commit, pins bundled documentation links to that
revision, and writes a ZIP with a checksum. The verifier checks the inventory,
file hashes, documentation revision and standalone DLL loading without starting
the game.

For a release, add `releases\<version>.json` and use the required
[release-note template](../releases/template.md). The tag `v<version>` publishes
only after CI passes; a version suffix marks the release as a prerelease.
Complete the [headset checklist](testing.md#headset-playtest) before promoting
a stable release.

## Repository layout

| Directory | Contents |
| --- | --- |
| `src` | Runtime, launcher, stereo rendering and input |
| `config` | Shipped INI defaults |
| `tests` | Native and Python regressions |
| `tools` | Packaging, release notes and diagnostics |
| `cmake` | Test registration |
| `.github` | CI, release automation and issue forms |
