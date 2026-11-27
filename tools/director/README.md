# Work with an agent in Director mode

Build and set up LARecomp as usual. From the checkout, run these commands with
your executable path and Python 3.10 or newer.

~~~sh
python -I -B tools/director/scene_tool.py launch --exe path/to/larecomp.exe
python -I -B tools/director/scene_tool.py slots --game-dir path/to
python -I -B tools/director/scene_tool.py cmd help --game-dir path/to
~~~

Launch leaves the game running. Enter free roam before previewing or rendering.
Use `--scene FILE` at launch to arm a scene, or load it later from the panel.
The game writes the connection file beside the executable. Launch puts logs and
capture output under `director-work/` there. If you start the game yourself, add
`--mcla_director=true --mcla_director_info=director.json`. The tools search the
current directory and `--game-dir`. Use `--attach` with the info file or its
directory to select a specific instance. Give the agent that path and keep
the file and token local.

Point the agent at this folder, the [scene authoring guide](../../documentation/SCENE-AUTHORING.md)
and the [Director protocol](../../documentation/DIRECTOR-PROTOCOL.md).

- **You lead in the panel.** Open it with Ctrl+F12 or DEBUG > TOOLS > DIRECTOR PANEL.
  Frame the camera, place cars and make timeline keys. Ask the agent to lint
  the saved scene or suggest edits.
- **The agent leads.** Have it edit the scene file so the game reloads it, or
  send `scene.propose` so you can review the change in the panel.
- **The agent works alone.** It can create a scene from a recipe, lint it, preview
  frames and render a take in the running game.

~~~sh
python -I -B tools/director/scene_tool.py new my-shot --recipe test-fixed-camera --dir path/to/director-work
python -I -B tools/director/scene_tool.py lint path/to/director-work/my-shot.scene
python -I -B tools/director/scene_tool.py preview path/to/director-work/my-shot.scene --game-dir path/to --from 7 --to 7.1 --max-shots 1
python -I -B tools/director/scene_tool.py render path/to/director-work/my-shot.scene --game-dir path/to --from 7 --to 8 --sink raw
~~~

Preview writes stills and `preview.json`. Pillow adds contact sheets, maps and
picture diffs when it is visible to Python. The `ffmpeg` and `png` render sinks
use the game's FFmpeg, normally from PATH. Choose it with
`launch --ffmpeg FILE` or the game's `mcla_capture_ffmpeg` setting.

Build an optional atlas with `scene_tool.py atlas build --game GAME_DATA_DIR`.
Use the directory containing your own `xarchive_cache.rpf`. The result stays
under `~/.larecomp/director/atlas`. For another location outside the checkout,
use `--out` and pass the resulting atlas folder to `query --atlas DIR`.

Run all offline checks with `python -I -B tools/director/scene_tool.py selftest`.
The output labels the optional Pillow checks.
