# Settings

Edit `OutlastVR.ini` beside the launcher while the game is closed. Changes take
effect on the next launch.

Press **F10** for a visible in-game settings panel. Nausea distortion, bloom,
film grain, visual body follow, follow deadzone, pickup radius and vignette update
without restarting. Panel changes last for this session only; edit the INI to
change startup defaults. The effects switches adjust native settings, not a
replacement color filter. Disabling an effect does not guarantee its producer
render pass is skipped.

F10 works from either the game window or **Outlast VR - Desktop**. Use Up/Down
and Left/Right/Enter, point and trigger, or click a setting. Only live player
controls appear: no read-only startup rows, technical paths or experiments.
Rows, values and limits still come from the INI definitions, filtered by live
support. Paging appears automatically if live controls outgrow one page.

The table uses `section.key`: write `key=value` under `[section]` in the INI.
`empty` means leave the value blank. Relative paths are resolved from the
folder containing the INI.

| Setting | Default | Description |
| --- | --- | --- |
| `vr.snap_turn_degrees` | `0` | `0` for smooth turning; `15` to `90` for snap turning. |
| `vr.seated` | `false` | Keep eye height at the native camera and use button crouching. |
| `vr.left_handed` | `false` | Swap gameplay actions and movement/turning sticks. |
| `vr.toggle_sprint` | `false` | Click to start sprinting; click again or stop moving to stop. |
| `vr.movement_reference` | `game` | Movement direction: `game`, `head`, `main_hand` or `off_hand`. |
| `vr.world_scale` | `1` | World size, from `0.5` to `2`. Larger values make the world appear larger. |
| `vr.vignette_strength` | `0` | Peripheral dimming during movement, from `0` to `1`. `0` disables it. |
| `vr.hit_distortion` | `false` | Restore native hit-induced image rotation and zoom. Damage tint and other effects remain enabled. |
| `vr.bloom` | `true` | Native bloom. Disabling preserves depth-of-field limits and unrelated packed settings. |
| `vr.film_grain` | `true` | Native artistic grain strength. Script/movie timing and stock low-level dithering remain unchanged. |
| `vr.body_follow` | `true` | Turn the visual skeleton toward the headset after passing the deadzone; does not change native actor yaw or the chosen movement reference. |
| `vr.body_follow_deadzone` | `35` | Head-to-body angle that starts visual catch-up, from `10` to `90` degrees. Small glances do not turn the body. |
| `vr.hand_pickup_radius` | `0.30` | Item-to-primary-hand radius in metres, from `0.1` to `0.6`. Native body reach, eligibility and visibility checks still apply. |

For example, to enable 30-degree snap turning:

```ini
[vr]
snap_turn_degrees=30
```

Hand-relative movement stops if the directional hand loses tracking.
Pickup settings do not bypass the game's inventory checks or scripted events.
Hand pickups select the nearest admissible item, without requiring headset gaze.
Walls still block pickup; increasing the radius does not enable remote grabbing.

Ordinary unlocked doors open without the player's scripted door animation or
camera repositioning. The native door actor still owns movement, collision,
sound and events. Scripted, locked and unsupported doors retain native controls.
Experimental hand-driven door manipulation is not included.

## Advanced overrides

These optional keys remain supported for existing configurations, but are not
included in the shipped INI or F10 menu. Normally the game and loader are found
automatically; add a section/key only when an override is actually needed.

| Setting | Default | Description |
| --- | --- | --- |
| `game.executable` | `empty` | Path to `OLGame.exe`. Blank checks beside the launcher, then `Binaries\Win64` beneath it. |
| `vr.openxr_loader` | `empty` | Custom OpenXR loader path. Blank uses the bundled loader and active runtime. |
| `experimental.physical_camcorder` | `false` | Opt into the held camcorder in place of full-view night vision. |
| `experimental.instant_pickups` | `true` | Complete inventory pickups after native admission. `false` retains native animations and completion. |

Experimental options may change between releases. Keep the physical camcorder
off unless you intend to test it; check your version's
[release notes](https://github.com/czearing/OutlastVR/releases) before enabling it.
