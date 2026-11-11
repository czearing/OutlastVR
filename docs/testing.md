# Testing

See [building](building.md) for automated tests. Hardware tests are separate;
a passing test suite does not establish headset compatibility.

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
  Check doors, vaults, ladders, hiding and scripted sequences.
- Check hand placement through animations, tracking loss, death and loading.
  Test physical crouch, button crouch and seated mode for floor-height drift.
- Check glass, transparency, floor materials and particles in both eyes with
  the filter off and on.
- Check smooth/snap turning, handedness, movement reference and recentering.
  Losing focus must not leave movement or buttons held.
- Run an extended session with repeated save/load cycles. Watch headset frame
  times, memory use, stalls and crashes. The desktop mirror is not a headset
  frame-time measurement.

## Reporting results

Include the mod version, headset/controllers, OpenXR runtime, GPU/driver,
resolution/refresh rate, settings and scene/save location. Describe the steps
and expected result. Attach the mod log after removing personal paths.
