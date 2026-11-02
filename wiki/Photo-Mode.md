# Photo Mode

Photo mode and the photo album are the game's own; LARecomp adds fixes and a trace. Source: `src/mc_engine/hooks/photo_mode.cpp`, `src/mc_engine/hooks/photo_album.cpp`. There is no mod-facing file format for photos in this repository.

## What is fixed

The game takes its picture on the CPU: it locks the front buffer and encodes it as a JPEG. The picture therefore needs the GPU's resolve copied back to guest memory. A global readback would stall every resolve of every frame, so LARecomp asks for a readback of the two front buffers for a few frames around each shot.

The album is paced separately: every Flash movie steps once per rendered frame, so above 30 FPS the gallery would play fast; the `ui_movie_pacing` cvar (a string, default `PHOTOALBUMMOVIE`, in `src/mc_engine/hooks/ui.cpp`) paces it and leaves captures alone.

## Options

All three are cvars with no pause menu row. Full rows in [Settings Reference](Settings-Reference#mclaphotomode-3).

| Option | Default | Effect |
|---|---|---|
| `photo_auto_readback` | `true` | Fix for previews that show an old frame or garbage. Leave on. |
| `photo_album_keep_new_preview` | `true` | While choosing the slot for a new photo, the album shows the photo just taken instead of the last saved one the cursor passed over. |
| `photo_readback_debug` | `false` | Logs album state-machine phase transitions and the front buffer addresses the readback is armed for. |

`photo_ui_trace` (default `true`, category `MCLA/Diagnostics`) logs the UI statechart while the album scene is up (photo mode, the album, the Rate My Ride snapshots). It is silent everywhere else.

## Troubleshooting

- Preview is stale or garbage: confirm `photo_auto_readback` is `true`. Turn on `photo_readback_debug` to see whether the resolve that fills the front buffer lands in the armed range.
- Gallery plays too fast above 30 FPS: paced by `ui_movie_pacing`; see [Settings Reference](Settings-Reference#mclaui-12).
