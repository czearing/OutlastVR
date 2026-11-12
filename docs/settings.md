# Settings

Edit `OutlastVR.ini` beside the launcher while the game is closed. Changes take
effect on the next launch.

The table uses `section.key`: write `key=value` under `[section]` in the INI.
`empty` means leave the value blank. Relative paths are resolved from the
folder containing the INI.

| Setting | Default | Description |
| --- | --- | --- |
| `game.executable` | `empty` | Path to `OLGame.exe`. Blank checks beside the launcher, then `Binaries\Win64` beneath it. |
| `vr.openxr_loader` | `empty` | Custom OpenXR loader path. Blank uses the bundled loader and active runtime. |
| `vr.snap_turn_degrees` | `0` | `0` for smooth turning; `15` to `90` for snap turning. |
| `vr.seated` | `false` | Keep eye height at the native camera and use button crouching. |
| `vr.left_handed` | `false` | Swap gameplay actions and movement/turning sticks. |
| `vr.toggle_sprint` | `false` | Click to start sprinting; click again or stop moving to stop. |
| `vr.movement_reference` | `game` | Movement direction: `game`, `head`, `main_hand` or `off_hand`. |
| `vr.world_scale` | `1` | World size, from `0.5` to `2`. Larger values make the world appear larger. |
| `vr.vignette_strength` | `0` | Peripheral dimming during movement, from `0` to `1`. `0` disables it. |
| `experimental.physical_camcorder` | `false` | Opt into the held camcorder in place of full-view night vision. |
| `experimental.instant_pickups` | `true` | Complete inventory pickups after native admission. `false` retains native animations and completion. |

For example, to enable 30-degree snap turning:

```ini
[vr]
snap_turn_degrees=30
```

Hand-relative movement stops if the directional hand loses tracking.
Pickup settings do not bypass the game's inventory checks or scripted events.

Experimental options may change between releases. Keep the physical camcorder
off unless you intend to test it; check your version's
[release notes](https://github.com/czearing/OutlastVR/releases) before enabling it.
