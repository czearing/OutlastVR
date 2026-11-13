# Testing

Use the [build guide](building.md) to configure and run CTest. Hardware and
campaign checks are separate from automated tests.

## Unattended acceptance

Configure GPU and supported-retail tests, then build before running:

```powershell
python tools\unattended_acceptance.py `
  --build .\build `
  --module .\build\bin\OutlastVRSinglePass.dll `
  --retail "C:\Games\Outlast\Binaries\Win64\OLGame.exe" `
  --output "C:\OutlastVR-evidence\run001" `
  --cycles 2 --session-cycles 64
```

Use a new output directory and a clean, committed source tree. Do not run a
competing build that changes the inputs. The runner requires Windows, Git,
CTest, the built fixtures and available memory/disk space; it reports resource
requirements before testing.

The runner does not launch Outlast, load saves, change settings or register a
runtime. It checks every configured test for completion and records the source
revision and input hashes. Retain `acceptance.json`, CTest XML and individual
logs together.

| Check | Coverage |
| --- | --- |
| CPU tests | Configuration, input recovery, ownership and lifecycle rules |
| GPU tests | Eye pixels, draw counts, shaders and graphics-state restoration |
| Production DLL fixtures | Hook arguments, retail replay, reset and XR submission |
| Repeated XR sessions | Creation, teardown, tracking loss and resource cleanup |
| Negative controls | Exact expected failure codes and independent diagnostics |

Crashes, timeouts and missing dependencies do not count as passing negative
controls. `automated_passed` records automated results, not headset or campaign
approval. Mock runtimes and retained captures cover only the scenarios exercised.
The transient-effect fixture checks synthetic radial distortion, two-axis offsets,
flash tint and recovery. It does not establish the retail hit effect's appearance.

## Headset playtest

- Install a fresh ZIP beside the game and in a separate folder. Check launcher
  verification, normal Steam launch, updating and uninstalling.
- Check both eyes during videos, loading, gameplay, menus, notes, cutscenes,
  death/restart and save/load. Check text placement and menu-pointer alignment.
- During checkpoint reloads, check that both eyes stay black with no loading
  artwork or pointer, then return to gameplay. Menus and videos must remain visible.
- Check height, scale and controls after recentering, focus loss, headset
  removal and controller reconnection. Include seated and left-handed modes.
- Pick up notes, folders and batteries with both pickup settings. Confirm each
  inventory event occurs once and movement returns afterward.
- Check doors, vaults, ladders and hiding. After leaving a locker, verify that
  enemies still animate, detect the player and resume their behavior.
- Move each hand through its range, near walls and during scripted animations.
  Check tracking recovery and that rendered shadows follow the tracked hands.
- Compare night vision with the original game in the same scene. Check
  illumination, color, glass, transparent materials and particles in both eyes.
- Take repeated hits with night vision on and off. Check flash, distortion,
  camera motion and recovery in both eyes, including a hit followed by death/reload.
- Exercise each available headset/controller/runtime combination. Include
  hybrid-GPU laptops where available.
- Run extended sessions with repeated save/load cycles. Record headset frame
  times, memory growth, stalls and crashes.

With the physical camcorder explicitly enabled, also check both holding hands,
release/regrab, nearby interactions, tracking recovery, lens/screen alignment,
night vision, battery depletion, camera HUD and scripted confiscation.

## Performance routes

Use the reported slow water checkpoint as the primary route. Record the save,
chapter, start/end landmarks and camera direction so another tester can repeat
it. Do not substitute a dry corridor or a shader fixture for that checkpoint.

| Route | Conditions to compare |
| --- | --- |
| Reported water checkpoint | Face across water, look down at it, then traverse the same flooded path; night vision off/on |
| Flooded sewers | Stationary water view and movement past water, nearby geometry and enemies |
| TV room near the wheelchair patient | Repeat the same head/hand movement; check tracking, shadows and frame times |
| Courtyard | Rain, particles, distant geometry and rapid head turns |
| Enemy encounter | Repeated hits with night vision off/on, recovery, then death/checkpoint reload |
| Runtime transitions | Dashboard open/close, headset removal/resume and menu/gameplay transitions |

Keep the headset refresh rate, runtime, per-eye resolution, graphics settings,
save and route unchanged between builds. Record CPU/GPU, driver, headset,
connection method, DLL hash and settings hash. Disable recording, overlays and
extra injectors for the baseline; test their overhead separately. Do not change
quality or add foveation/upscaling to make an optimization pass.

After 30 seconds of warmup, capture at least 120 seconds per steady route.
Repeat baseline/candidate in ABBA order twice. Also retain the first cold pass
to expose shader compilation or streaming stalls. Measure encounters and
reloads separately from steady gameplay; do not average black loading frames
into water performance.

The steady-route acceptance target is 15% CPU and GPU headroom at p95, each
stage within one refresh interval at p99, and no more than 1% dropped or
reprojected display intervals. Display-interval p99 must stay within 10% of
the refresh interval; a work or display interval longer than two refresh
budgets fails the steady-route stall gate. CPU/GPU work overlaps: do not add their times,
and do not count intentional `xrWaitFrame` pacing as CPU rendering work.

| Refresh | Frame budget | p95 work target |
| --- | --- | --- |
| 72 Hz | 13.89 ms | 11.81 ms |
| 80 Hz | 12.50 ms | 10.63 ms |
| 90 Hz | 11.11 ms | 9.44 ms |
| 120 Hz | 8.33 ms | 7.08 ms |

Retain the runtime's original timing capture. A normalized CSV for
`tools\diagnostics\verify_frame_budget.py` must have the exact header
`frame_id,elapsed_ms,app_cpu_ms,app_gpu_ms,dropped,reprojected`, with one row
per display interval, including misses. Preserve frame identities, use 0/1
flags, and exclude pacing waits from application CPU work. For example:

```powershell
python tools\diagnostics\verify_frame_budget.py water.csv --refresh-hz 90
```

The grader checks the supplied trace, not its provenance, visual correctness
or campaign coverage. Recheck both eyes for native color, refraction, transparency,
shadows and hit recovery before accepting a faster build.

The GPU fixtures `test_transparent_overdraw_stereo` and
`test_transparent_hit_overdraw` exercise 64 alpha-blended full-screen layers at
1024x512 per eye, scene-atlas sampling and hit-style distortion. Each reports
CPU submission and GPU timestamps separately, checks eye pixels, caller state,
exact occlusion counts and one native draw/two instances per submission.
Discard the explicitly marked cold sample when comparing warmed timings.
These are synthetic mechanism benchmarks, not retail water or headset FPS.
The normal runtime's `FRAME_PERFORMANCE` batches are diagnostic summaries;
their averages/maxima are not a substitute for the complete headset trace.

## Release record

Record the version, hardware/runtime, settings, scenes exercised and results.
Keep failures open in the issue tracker; carry player-relevant known issues
into that version's release notes. Do not turn a single passing scene or
automated run into a claim of full campaign or headset support.

Captures and logs may contain personal paths or game data. Redact logs before
sharing and keep private game assets out of the repository.
