# Building and testing

## Toolchain

- Windows x64, CMake 3.24+, Ninja and Git.
- MinGW-w64 GCC (the CI uses MSYS2 UCRT64).
- Windows 10/11 SDK, including `d3d9on12.h`.
- Python 3.10+ and the diagnostic test dependencies.

Run in a shell with `gcc`, `g++`, `cmake`, `ninja` and `python` on PATH:

```powershell
python -m pip install -r tools\diagnostics\requirements.txt
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
cmake --install build --prefix dist\OutlastVR --component Runtime
```

MinHook and the OpenXR loader are fetched at pinned commits during configuration.
No sibling repositories, developer-specific paths or retail assets are needed.
If SDK discovery fails, set `-DOLVR_WINDOWS_SDK_UM=<SDK Include\version\um>`.
Build outputs and diagnostic data must never be committed.

## Optional hardware and retail tests

Add `-DOLVR_GPU_TESTS=ON` for actual D3D12 GPU tests. These refuse software
adapters and do not run on GitHub's hosted runners. They do not launch the game.
`-DOLVR_RETAIL_EXECUTABLE=<absolute OLGame.exe>` enables additional local offline
tests against the supported executable. `-DOLVR_HERO_FIXTURE=<private rig file>`
enables rig-dependent tests. Neither input is redistributable or included in CI.

`-DOLVR_BUILD_DIAGNOSTICS=ON` builds the developer-only headset-free probe and
the native draw, shader-rejection, replay and soft-particle diagnostic tools.
These tools require explicit private fixtures or a new capture directory;
they are built but not invoked by the default test suite.
Its capture workflow is intentionally separate from normal gameplay and release
packaging. Retain its full captures privately; never replace actual native-scene
or physical-headset checks with unit-test results.

## Layout

- `src/`: runtime, launcher, stereo renderer, input and experimental camera.
- `tests/native/`: C++ regression and optional hardware/retail tests.
- `tests/python/`: independent image/evidence-verifier regressions.
- `tools/diagnostics/`: offline shader/evidence analysis, not release binaries.
- `config/`: shipped default settings.
- `cmake/`: shared test definitions.
- `.github/workflows/`: build/test/package and tag-triggered release automation.

## Releases

Every pull request and push builds the production binaries, runs CPU/Python
tests, checks a clean package, and uploads a checksummed ZIP. Version tags
`vX.Y.Z` publish a release only after those checks pass. Tags containing a suffix
(for example `v0.1.0-rc.1`) are marked prerelease. The numeric part must match
the CMake project version. Do not tag a stable release until headset acceptance
and known regressions have been reviewed.

For a committed local build, run `.\tools\package.ps1` and then
`python tools\verify_package.py dist\OutlastVR-0.1.0-win64`.
The verifier checks the exact file list, content hashes, default camcorder gate,
standalone DLL loading and launcher help without launching the game.
