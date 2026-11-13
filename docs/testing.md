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

## Release record

Record the version, hardware/runtime, settings, scenes exercised and results.
Keep failures open in the issue tracker; carry player-relevant known issues
into that version's release notes. Do not turn a single passing scene or
automated run into a claim of full campaign or headset support.

Captures and logs may contain personal paths or game data. Redact logs before
sharing and keep private game assets out of the repository.
