# Testing

See [building](building.md) for automated tests. Hardware tests are separate;
a passing test suite does not establish headset compatibility.

## Unattended acceptance

Configure GPU and supported-retail tests as described in [building](building.md),
then build before running:

```powershell
python tools\unattended_acceptance.py `
  --build .\build `
  --module .\build\bin\OutlastVRSinglePass.dll `
  --retail "C:\Games\Steam\steamapps\common\Outlast\Binaries\Win64\OLGame.exe" `
  --output "C:\OutlastVR-evidence\run001" `
  --cycles 2 --session-cycles 64
```

Use a new output directory for each run. The standard-library runner does not
launch the game, load saves, change settings, register an OpenXR runtime or
install dependencies. It requires Windows, Git, CTest, the built fixtures and
at least 3 GiB free physical RAM / 4 GiB available commit; it stops rather than
reconfigure the shared machine. Run it without a competing build modifying its
inputs. Logs and a failure report survive interrupted/failed tests.

The runner requires the GPU/retail roster, checks every discovered CTest case
for completion (no skips), and verifies the configured DLL/game paths. The
report binds results to module, retail and test-executable SHA-256 hashes and
records the source revision/dirty state. It rechecks hashes at the end. Use a
clean committed source tree for release evidence; a revision with dirty source
is not an attestation that the DLL was built from that commit.

Additional actual-DLL cases cover render-thread handoff/overlap, Canvas/Scaleform
passive-HUD ownership, animated camera stabilization, floor startup, comfort/handedness,
snap-turn recovery, changing IPD, input/focus recovery, native reset retry and
world/flat transitions. These use fresh processes. Separately, the session
component repeats creation, teardown, tracking loss, reference changes and
restart scenarios **inside one process**, with independent GPU eye-pixel
checks and exact owned XR handle/haptic cleanup. This is not a production-DLL
long-session memory-leak certificate. Interactive native movie classification,
world-anchored menu placement and controller-pointer usability still require
the separate native/headset checks.

Deliberately broken mono/dual draws, UI/flat projections, camera stabilization,
comfort scale, snap turning and stale generated indices must fail with their
specific independent pixel/draw oracle diagnostics and exact expected exit
codes. A random crash, timeout, missing DLL or unsupported GPU is a failure of
acceptance, not a successful negative control.

`acceptance.json` reports `automated_passed`, never automatic
`production_ready=true`. Retain the report, CTest XML and individual logs
together. They can contain local paths; redact personal paths before sharing.

## What the evidence establishes

| Evidence | Establishes | Does not establish |
| --- | --- | --- |
| CPU/contract tests | Config validation, grip ownership/rearm, native completion gating and lifecycle policies | Physical tracking or engine execution |
| Isolated GPU tests | Actual eye pixels, draw/instance counts, shader/state restoration and index-cache invalidation | Whole-scene or campaign visual correctness |
| Production DLL fixture | Real exported initialization/hooks, supported retail replay, native reset and GPU submission contracts | A running game, arbitrary script progression or consumer-runtime support |
| Retained native captures | Only the captured binary, scene, actions and settings actually observed | Modified-binary execution or unvisited content |
| Headset/campaign playtests | Observed ergonomics, controller alignment, visuals, progression and frame times | Untested headsets, scenes or settings |

This follows the complementary stereo proof of draw/eye-production counts,
submitted eye pose/FOV/subimages and actual eye images. Distinct texture handles
or a successful OpenXR call alone are not pixel evidence.

### Comparative mod bug checks

External reports are investigation leads, not proof of the same defect here.
[UEVR #453](https://github.com/praydog/UEVR/issues/453) describes nested XInput
forwarder hooks losing one-shot input; our native gamepad entry hook is not that
multi-DLL hook layout. [UEVR #436](https://github.com/praydog/UEVR/pull/436)
describes stale poses when game frame numbers freeze/restart; our XR session
samples the current runtime-predicted display time rather than indexing a pose
queue by the game's frame number. Do not copy Windows DLLs or change registry
settings as speculative fixes.

Controller reconnection reports such as
[UEVR #200](https://github.com/praydog/UEVR/issues/200) prompted independent
action-availability checks. `test_input_analog_recovery` covers both hands,
trigger/grip actions, focus loss, explicit suspension, whole-hand inactivity
and action-only inactivity while poses remain valid. A held or unseen press
must remain suppressed until an **active** pressure sample falls to 40% or
below; an inactive action's synthetic zero is not a physical release. The
regression exercises the actual menu-press and native-use mapping consumers,
independent actions/hands and malformed active values, not just self-counters.

[Nomai VR #565](https://github.com/Raicuparta/nomai-vr/issues/565) reports
reading-tool jitter, while [REFramework #1100](https://github.com/praydog/REFramework/issues/1100)
reports a hand stuck during/after a scripted carry animation. These motivate
the separate native camera/wrist ownership, scripted-state and hand-alignment
checks; passing raw XR input tests does not establish those behaviors.
[Nomai VR #570](https://github.com/Raicuparta/nomai-vr/issues/570) reports reversed
page directions in left-handed mode. Keep gameplay handedness separate from
menu pointer coordinates, and include both hands in the physical menu checklist.
These reports do not establish new Outlast defects or physical acceptance.

Primary-source testing guidance:

- [Khronos OpenXR CTS](https://github.com/KhronosGroup/OpenXR-CTS):
  runtime conformance testing, not application or campaign certification.
- [Khronos API layers](https://github.com/KhronosGroup/OpenXR-SDK-Source/tree/main/src/api_layers):
  API dump/core validation can diagnose API misuse, not comfort or scene visuals.
- [xr-sim](https://github.com/phunkaeg/xr-sim) and its
  [testing guide](https://github.com/phunkaeg/xr-sim/blob/main/docs/TESTING.md):
  deterministic Windows developer runtime, not a conformant consumer runtime.
  Its native D3D12 image/metadata support is distinct from its richer D3D11 PNG
  compositor capture. Our D3D9-on-12 fixtures use their own actual GPU readback.
  If independently evaluating an external runtime, use process-local
  `XR_RUNTIME_JSON`, not a global runtime replacement.

## Feature flights and release gates

`[experimental] physical_camcorder=false` remains the default. Camera ownership
and native resource policies are automated, including both hands; visibility,
lens/screen alignment, battery/HUD behavior in arbitrary scripted sequences and
comfort still require physical acceptance. Never enable it silently in an
existing user's INI.

`[experimental] instant_pickups=true` preserves the preview's immediate
completion after native admission. `false` leaves original native completion
and animations intact. Tests check disabled-flight byte preservation and that
completion cannot run through missing bindings. This does not certify every
item type or scripted pickup; include both settings in playtests.

Stable release remains blocked until the checklist below is accepted on actual
hardware, including representative campaign progression and extended sessions.
Keep experimental features opt-in and publish prereleases while these gates
remain open. Do not equate simulation, historical captures, passing CI or
desktop timing with a complete/perfect VR port.

## Playtest checklist

- Install from a fresh release ZIP, both beside the game and in a separate
  folder. Check launcher verification, normal Steam launch and uninstall.
- Check both eyes during startup videos, loading, gameplay, pause/options,
  notes, cutscenes, death/restart and save/load. Flat screens should have depth;
  neither eye should be missing, cropped, misaligned or showing the other eye.
- Check stereo scale and alignment after headset removal, focus changes,
  recentering and controller reconnection.
- Test the active OpenXR runtime on each available headset/controller pair.
  Include hybrid-GPU laptops where available. Restart after changing runtimes
  or headsets.
- Navigate menus and pick up folders, notes and batteries using controllers.
  Check both `instant_pickups` settings; verify inventory/profile updates happen
  once and movement returns. Check doors, vaults, ladders, hiding and scripted
  sequences. Gripping the experimental camera must not also use a nearby item.
- Check hand placement through animations, tracking loss, death and loading.
  Test physical crouch, button crouch and seated mode for floor-height drift.
- Check glass, transparency, floor materials and particles in both eyes with
  the filter off and on.
- Check smooth/snap turning, handedness, movement reference and recentering.
  Losing focus must not leave movement or buttons held.
- Run an extended session with repeated save/load cycles. Watch headset frame
  times, memory use, stalls and crashes. The desktop mirror is not a headset
  frame-time measurement.
- With the physical camcorder explicitly enabled, check each holding hand,
  release/regrab, focus/tracking recovery, native night vision, battery depletion,
  camera HUD, notes/menus, scripted confiscation and scene transitions. Check
  lens occlusion, screen legibility and optical direction on the actual headset.

## Reporting results

Include the mod version, headset/controllers, OpenXR runtime, GPU/driver,
resolution/refresh rate, settings and scene/save location. Describe the steps
and expected result. Attach the mod log after removing personal paths.
