# Menu Cameras

Replace the camera shot behind the front-end menus (the city view behind the boot and initial menus) with your own. Source: `src/mc_engine/menu_camera.cpp`. The data file is not part of a mod folder: it is read from the process working directory.

## File

`menu_cameras.txt` in the working directory. The game re-reads it when its modification time changes (checked at most once a second), so shots can be tuned with the menu on screen. If the file is removed the stock shots come back.

Line formats:

```text
cam   <slot> <px> <py> <pz> <lx> <ly> <lz> [fov_degrees] [hour] [weather]
stock <slot>
```

| Field | Meaning |
|---|---|
| `slot` | Shot slot, `0` to `63`. An out-of-range slot is skipped and logged. |
| `px py pz` | Camera position. |
| `lx ly lz` | Look-at point. |
| `fov_degrees` | Optional. `0` or omitted keeps the game's field of view for that frame. |
| `hour` | Optional, 0 to 24. Lights the shot at that hour. Omitted leaves the clock alone. |
| `weather` | Optional: `nice`, `cloudy`, `stormy` or `foggy`. Omitted leaves the sky alone. |
| `stock <slot>` | Reserves a place in the rotation and shows the game's own camera when it comes up. |

`#` and `;` start a comment. Any other line is ignored, so a dump can interleave commented context.

Log on load: `[MenuCam] loaded N custom shot(s) + M stock step(s) from menu_cameras.txt`.

## Options

All are cvars, set in `larecomp.toml` or on the command line. None has a pause menu row. Full rows in [Settings Reference](Settings-Reference#mclamenu-camera-8).

| Option | Default | Effect |
|---|---|---|
| `menu_cam_custom` | `false` | Place the menu camera at the shot from `menu_cameras.txt` matching the current slot. Slots without a line keep the stock camera. |
| `menu_cam_only_custom` | `false` | Remap whatever slot the game picks onto the lines present in the file so stock cameras never appear. Needs `menu_cam_custom`. |
| `menu_cam_lock` | `false` | Always show `menu_cam_slot` instead of letting the game rotate. This is the on/off switch; the slot alone locks nothing. |
| `menu_cam_slot` | `0` | Which shot `menu_cam_lock` pins, 0 to 63. |
| `menu_cam_cycle` | `0.0` | Seconds between shots, rotating through every line in the file. Range 0 to 600. `0` is no rotation. Ignored while `menu_cam_lock` is on. The game itself does not rotate the front-end camera; without this the boot screen stays on one shot. |
| `menu_cam_record` | `false` | Capture the game's own menu shots into the file as `cam` lines, sampling whenever the camera moves 25 or more units, using the next free slot number. Turn it off after the menus have cycled. |
| `menu_cam_diag` | `false` | Log the camera handed to the renderer once per mode change, and every shot placed. Turn on if a shot does not show or points the wrong way. |
| `menu_cam_dump` | command | Append the current spot to `menu_cameras.txt` as a `cam` line. Fly to the spot first with `debug_cam = free`; without it the player car's position is used. |

## Workflow

1. Set `debug_cam = free` and fly to a spot.
2. Run `menu_cam_dump` to append a `cam` line.
3. Set `menu_cam_custom = true`. Edit the line while the menu is visible.
4. Use `menu_cam_cycle` for a rotation, or `menu_cam_lock` with `menu_cam_slot` to pin one.

## Log lines

| Message | Meaning |
|---|---|
| `[MenuCam] <cam|stock> slot N out of range (0-63), skipped` | Slot outside the table. |
| `[MenuCam] dump: no position available (load a game first)` | Dump with no car or free camera. |
| `[MenuCam] dump: cannot write <file>` | File not writable. |
| `[MenuCam] record: cannot open <file> for append` | File not writable. |
| `[MenuCam] placing slot N at x y z` | A custom shot is being placed (shown with `menu_cam_diag`). |

## Not documented in the source

- Valid hour values beyond "hour 0-24".
- Which slot numbers the game uses for which menu. The code states that the slot table the game probes holds no data at runtime, so slots are not tied to named locations.
