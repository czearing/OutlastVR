# Controls

Use Outlast's default gamepad layout **A**. The bindings below assume
right-handed mode.

| Input | Action |
| --- | --- |
| Left stick / right stick | Move / turn |
| Left stick click | Sprint |
| Right stick click | Toggle night vision |
| Right grip | Grab / interact |
| Either trigger | Select the pointed menu item |
| Right primary / secondary | Jump or confirm / crouch or back |
| Left primary / secondary | Notes / reload battery |
| Left menu button | Pause |
| F8 | Recenter |
| F10 | Open / close live VR settings |
| Hold both stick clicks for one second | Recenter |

**Primary** is A/X on Touch or A on Index. **Secondary** is B/Y on Touch or B
on Index. In-game VR hints use these action names in English.

## Handedness and comfort

Left-handed mode swaps gameplay actions and sticks, not the physical hand
models. Either trigger still selects menu items.

Physical crouching is available while standing. In seated mode, use button
crouching. Turning, movement direction and sprint behavior are configured in
[settings](settings.md).

## Controllers without face buttons

Click the stick or trackpad upward for primary, or downward for secondary.
Horizontal and center clicks retain sprint or night vision for that hand.
If the runtime provides no menu binding, press both secondary actions together
to pause.

## Interactions and night vision

Grip uses the game's interaction rules; items are not free physics objects.
Inventory pickups complete immediately unless native pickup animations are
selected in settings. Bring the primary hand within 30 cm of an inventory item,
then grip; looking directly at it is not required. The radius is adjustable, but
native reach, visibility and inventory restrictions remain. Ordinary unlocked doors
open through the game's own door simulation without the player's lean, reach
animation or scripted camera repositioning. Locks, occupied/scripted doors,
ladders and hiding retain their native behavior. Experimental hand-driven door
manipulation is not included.

Default night vision uses the retail postprocessing and IR light without
battery consumption or the camera HUD.

With the experimental physical camcorder enabled, grip near its chest holster
with either hand to hold it. That grip is reserved for the camera, so it does not
also interact with a nearby door or item. This mode uses native camera
night vision and batteries. This release does not change the existing
experimental camera behavior; leave it disabled for normal full-view night vision.

## Live settings and desktop mirror

F10 opens an opaque, room-anchored settings panel. Use arrow keys and Enter,
click a row on the desktop, or point either controller and press its trigger.
Left/right adjusts the selected value; Escape closes the panel. Gameplay input
is suppressed while it is open. Changes apply for the session, not to the INI.
The desktop shows the final left-eye image, including settings, rather than a
blank window. OpenXR controls frame pacing; desktop vsync is not a second wait.
