#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>
#include "settings/immortal.h"

namespace larecomp::settings {

class CinematicSettings final : public rex::SettingOwner {
 public:
  CinematicSettings() : SettingOwner("larecomp") {}

  REX_SETTING(bool, mcla_cam_cut_blur_fix, true, "TRAILER TOOLKIT (for A/B pictures): on a frame where the scene's camera cuts, the game's motion blur "
    "gets this frame's own matrices as the previous frame's, so it does not smear the frame (E5: the first frame "
    "after a cut). Off: the game's constants as they are.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_cam_log, 0, "TRAILER TOOLKIT: while a scene's camera is running, log it every this many frames: scene time, position, direction, field of "
    "view. 0: only the cuts.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_capture_audio, true, "TRAILER TOOLKIT capture mode: during a take the game's audio output is replaced by one that mixes exactly one video frame of sound "
    "per frame and writes it to audio.wav in the take's folder; the real device is given back when the take ends. The capture driver is put in "
    "front of the audio output when such a take starts, not before (a game with no take has the stock output). Off: nothing is installed, the "
    "real output plays during the take, in real time, and no WAV is made. Read when a take starts.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_capture_audio_surround, false, "TRAILER TOOLKIT capture mode: the take's WAV has the six channels of the game's 5.1 mix (fl fr fc lfe bl br) instead of the stereo "
    "fold the real output plays. Read when a take starts.", rex::kDebugOnly);

  REX_SETTING(i32, mcla_capture_audio_timeout_ms, 2000, "TRAILER TOOLKIT capture mode: how long the game waits at its frame gate for the mixer's frames of one video frame before it goes on "
    "with a frame of silence for each that has not come (counted in the audio summary; three in a row stop the waiting for the take).", rex::Range{50, 60000}, rex::kDebugOnly);

  REX_SETTING(f64, mcla_capture_delay_ms, 0.0, "TRAILER TOOLKIT capture mode: the readback of each frame sleeps this long (a slow writer, in milliseconds).", rex::Range{0.0, 5000.0}, rex::kDebugOnly);

  REX_SETTING(std::string, mcla_capture_dir, "captures", "TRAILER TOOLKIT capture mode: the folder the take folders go in (a relative path is relative to the game's folder). Read when a "
    "take starts.", rex::kDebugOnly);

  REX_SETTING(std::string, mcla_capture_ffmpeg, "ffmpeg", "TRAILER TOOLKIT capture mode (sink ffmpeg): the ffmpeg program, a full path or a name on PATH.", rex::kDebugOnly);

  REX_SETTING(std::string, mcla_capture_ffmpeg_args, "-c:v ffv1 -level 3 -g 1 -slices 24 -pix_fmt bgr0", "TRAILER TOOLKIT capture mode (sink ffmpeg): ffmpeg's output options, between the raw input and video.mkv.", rex::kDebugOnly);

  REX_SETTING(f64, mcla_capture_fps, 0.0, "TRAILER TOOLKIT capture mode: a take at this many frames a second, from the next update: the game steps by exactly 1 / this "
    "whatever a frame takes to render, every frame is read back and the game waits for it before the next update. 0 ends the take. "
    "10 to 1000. A scene's capture key does the same.", rex::Range{0.0, 1000.0}, rex::kDebugOnly);

  REX_SETTING(i32, mcla_capture_gate_timeout_ms, 20000, "TRAILER TOOLKIT capture mode: how long the game waits at the frame gate before it goes on (counted in the summary; three in a "
    "row switch the waiting off for the take).", rex::Range{100, 600000}, rex::kDebugOnly);

  REX_SETTING(i32, mcla_capture_lag, 0, "TRAILER TOOLKIT capture mode: how many frames the game may run ahead of the readback. 0: it waits for frame k to be read back "
    "before it starts update k + 1.", rex::Range{0, 2}, rex::kDebugOnly);

  REX_SETTING(i32, mcla_capture_log_every, 30, "TRAILER TOOLKIT capture mode: a [cap] line with the costs of a frame every this many frames (0: the summary only).", rex::Range{0, 100000}, rex::kDebugOnly);

  REX_SETTING(std::string, mcla_capture_luma, "", "RT noise capture: replace the PNG sequence with lossless luma for full or x,y,width,height and three color previews. Empty keeps the PNG sink.", rex::kDebugOnly);

  REX_SETTING(std::string, mcla_capture_buffers, "", "Visual comparison buffers to write during a take: all or comma-separated final,hdr,ao,refl,gi,class,motion,reflhit,roadrefl,vol,shafts. Empty or off disables buffer capture.", rex::kDebugOnly);
  REX_SETTING(i32, mcla_capture_buffer_every, 1, "Visual comparison buffers: record the first take frame and every Nth frame after it.", rex::Range{1, 10000}, rex::kDebugOnly);
  REX_SETTING(std::string, mcla_capture_buffer_region, "full", "Visual comparison buffers: full or rects (the bounding rectangle of the scene capture key's rects option).", rex::kDebugOnly);

  REX_SETTING(i32, mcla_capture_memory_mb, 2048, "TRAILER TOOLKIT capture mode: the hard cap, in MiB, on the memory a take holds (its GPU readback buffers and its ring, one frame "
    "each). The ring is cut first, then the GPU buffers; a take that does not fit with 2 + 2 frames does not run, and the log says what "
    "to change (a lower render scale). Nothing grows with the take's length or the writer's speed. Read when a take starts.", rex::Range{64, 65536}, rex::kDebugOnly);

  REX_SETTING(std::string, mcla_capture_name, "", "TRAILER TOOLKIT capture mode: the take's folder name when the scene gives none (a name that is taken gets -2, -3 ...). Empty: "
    "the scene's own name, else take. Read when a take starts.", rex::kDebugOnly);

  REX_SETTING(std::string, mcla_capture_out, "", "TRAILER TOOLKIT capture mode: the take folder itself, exactly (it has to be new or empty); the take then ignores the dir and "
    "the name. Empty: <dir>/<name>. Read when a take starts.", rex::kDebugOnly);

  REX_SETTING(i32, mcla_capture_ram_floor_mb, 1024, "TRAILER TOOLKIT capture mode: free physical memory, in MiB, that has to stay free. A take whose buffers would take it lower is cut "
    "down or refused at its start, and a take that sees it fall below stops with an error (an 8K take used up the PC's RAM). 0: no floor.", rex::Range{0, 65536}, rex::kDebugOnly);

  REX_SETTING(i32, mcla_capture_ring, 4, "TRAILER TOOLKIT capture mode: host buffers between the readback and the writer (2 to 8): the most frames that wait for the writer. "
    "Fewer are used when mcla_capture_memory_mb or the free memory says so. Read when a take starts.", rex::Range{2, 8}, rex::kDebugOnly);

  REX_SETTING(std::string, mcla_capture_sink, "none", "TRAILER TOOLKIT capture mode: what happens to a frame once it is read back: none (counted and dropped), ffmpeg (a pipe to the "
    "program in mcla_capture_ffmpeg: FFV1 in video.mkv), png (ffmpeg writes one PNG for every frame) or raw (video.bgra, B8G8R8A8 "
    "frames). A scene's sink wins. Read when a take starts.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_capture_statelog, false, "TRAILER TOOLKIT capture mode: write state.csv in the take (the game's state for every frame: the scene's statelog on does the "
    "same). Read when a take starts.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_capture_virtual_clock, true, "TRAILER TOOLKIT capture mode: during a take the upscaler's history rule and the UI movies' pacing read frames x step instead of "
    "the wall clock. Off, a slow capture makes the upscaler start its history again on every frame.", rex::kDebugOnly);

  REX_SETTING(f64, mcla_cin_hud_alpha, 1.0, "TRAILER TOOLKIT: how much of the game's own UI (HUD, map, phone, prompts, gamertags, world icons) the picture shows, "
    "from 0 to 1. 1 is the game's frame and costs nothing; 0 is the 3D picture with no UI; in between is a fade. The pause "
    "menu, the DEBUG menu, the race pause, the map and the F3 card are never faded. Frames whose UI the renderer cannot "
    "keep apart from the picture (loading, movies) show it whole. Plume renderer only. Live; a scene's hud keys write it.", rex::Range{0.0, 1.0}, rex::kDebugOnly);

  REX_SETTING(bool, mcla_director, false, "TRAILER TOOLKIT DIRECTOR (debug): the game listens on 127.0.0.1 (loopback only, a token in the info file) for an agent's commands and runs the same "
    "commands as the Director panel: transport, scene edits, the free camera, cars, stills, takes (documentation/DIRECTOR-PROTOCOL.md). Live: on starts the listener, off stops it. "
    "A scene cannot set it. Off: nothing listens and nothing runs.", rex::kDebugOnly);

  REX_SETTING(std::string, mcla_director_info, "", "TRAILER TOOLKIT DIRECTOR: a file name; while the listener is up the game writes director.json there (port, token, pid, slot, scene) so a tool can find it, "
    "and removes it at exit. Empty: no file.", rex::kDebugOnly);

  REX_SETTING(std::string, mcla_director_root, "", "TRAILER TOOLKIT DIRECTOR: the folder the Director's commands may write to (stills, takes, routes, proposals), as relative paths without '..'. "
    "A scene's own folder is always allowed too. Empty: the game's working folder.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_director_panel, false, "TRAILER TOOLKIT DIRECTOR PANEL (debug): the Director's panel over the picture: a timeline with the scene's keys to drag, the free camera's keys by "
    "framing, rigs with sliders, cars to spawn and place, routes to record or drop and edit, the take's render button, and the routes and the camera's path drawn in the world. "
    "Every action is a command of the Director and a line of the scene file. Live; the key Ctrl+F12 (ui.bind.mcla_director) flips it. Needs mcla_director (the panel offers to switch it on).", rex::kDebugOnly);

  REX_SETTING(f64, mcla_fixed_step_fps, 0.0, "TRAILER TOOLKIT: the game steps by exactly 1 / this many frames a second, whatever the frame takes (the engine timer is given "
    "the game's own tick count for it); the FPS limit, the step cap and frame pacing stand aside. No take, no frame gate. 0: off. "
    "10 to 1000.", rex::Range{0.0, 1000.0}, rex::kDebugOnly);

  REX_SETTING(bool, mcla_freecam, false, "TRAILER TOOLKIT: the free camera. The pad flies the camera (left stick moves, right stick looks, triggers up and down, LB and RB tilt, "
    "D-pad zooms and sets the speed, a stick click is fast or slow) and the game gets no pad input but Start, so the car does not drive. "
    "Back + A records the flight, Back + X is home. Off: the game's own camera.", rex::kDebugOnly);

  REX_SETTING(f64, mcla_freecam_inertia, 0.25, "TRAILER TOOLKIT: how long the free camera takes to settle after the sticks move, in seconds (the turn and the tilt take half). 0: none.", rex::Range{0.0, 2.0}, rex::kDebugOnly);

  REX_SETTING(u32, mcla_freecam_log, 0, "TRAILER TOOLKIT: while the free camera flies, log its pose every this many frames ([fly] lines). 0: only the events.", rex::kDebugOnly);

  REX_SETTING(f64, mcla_freecam_look, 70.0, "TRAILER TOOLKIT: how fast the free camera turns at a full right stick, in degrees a second.", rex::Range{5.0, 360.0}, rex::kDebugOnly);

  REX_SETTING(std::string, mcla_freecam_out, "", "TRAILER TOOLKIT: the file the next free camera recording is written to (replaced when it exists). Empty: freecam\\flight-<date>-<time>.cam.csv "
    "next to the game.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_freecam_record, false, "TRAILER TOOLKIT: record the free camera's flight into a raw track file (one sample a game update; ports\\scripts\\scene_cam.py turns it "
    "into a scene's camera track). On starts a file, off ends it. Back + A on the pad flips it. Only records while the free camera flies.", rex::kDebugOnly);

  REX_SETTING(f64, mcla_freecam_speed, 12.0, "TRAILER TOOLKIT: how fast the free camera flies at a full stick, in metres a second (L3 four times as fast, R3 a quarter, D-pad left and right "
    "step it).", rex::Range{0.5, 500.0}, rex::kDebugOnly);

  REX_SETTING(std::string, mcla_scene, "", "TRAILER TOOLKIT: run the scene file at this path: a full path, or a name looked up next to the game and in its scenes "
    "folder (.scene may be left off). Hour, weather, traffic, time scale and settings follow the scene's timeline; the file is "
    "read again when it changes. Empty: off.", rex::kDebugOnly);

  REX_SETTING(i32, mcla_scene_restart, 0, "TRAILER TOOLKIT: a change of this number runs the armed scene again from its start.", rex::kDebugOnly);

  REX_SETTING(i32, mcla_scene_seed, 1, "TRAILER TOOLKIT repeatable scenes: while a scene runs, the game's random generators (the simulation's, the cosmetic one, the car configuration's) are given a state "
    "from this seed and the scene's update number at the start of every update, and the renderer's noise (the occlusion's, the contact shadows', the sun shafts', "
    "the shadow filter's, the temporal upscalers' jitter) follows the scene's update number (while the clock is held by mcla_scene_t, the number of the held time): the same scene gives the same pictures in every run. 0: off, the "
    "game's own random numbers and the renderer's frame count. Read at every update of a scene.", rex::Range{0, 2147483647}, rex::kDebugOnly);

  REX_SETTING(bool, mcla_scene_seed_each_frame, true, "TRAILER TOOLKIT repeatable scenes: true: the generators are given their state at every update of the scene (a thread that draws a different number of times in "
    "one run cannot carry the difference into the next update); false: once, at the scene's first update. Read at every update of a scene.", rex::kDebugOnly);

  REX_SETTING(f64, mcla_scene_t, -1.0, "TRAILER TOOLKIT: 0 or more holds the scene's clock at this many seconds on every update (stills, scrubbing); -1 lets "
    "it run.", rex::Range{-1.0, 86400.0}, rex::kDebugOnly);

  REX_SETTING(std::string, mcla_state_log, "", "TRAILER TOOLKIT: write one CSV row per game update (the player's and the scene's cars, the camera, the hour and weather, the "
    "clocks, the scene clock, the random number state) to this .csv file, or to state.csv in this folder, for take_diff.py. Rows "
    "are written while the player has a car. A scene's `statelog on` asks for the same. Empty: off.", rex::kDebugOnly);

};

inline CinematicSettings& g_cinematic = larecomp::Leak<CinematicSettings>();

inline auto& mcla_cam_cut_blur_fix = g_cinematic.mcla_cam_cut_blur_fix;
inline auto& mcla_cam_log = g_cinematic.mcla_cam_log;
inline auto& mcla_capture_audio = g_cinematic.mcla_capture_audio;
inline auto& mcla_capture_audio_surround = g_cinematic.mcla_capture_audio_surround;
inline auto& mcla_capture_audio_timeout_ms = g_cinematic.mcla_capture_audio_timeout_ms;
inline auto& mcla_capture_delay_ms = g_cinematic.mcla_capture_delay_ms;
inline auto& mcla_capture_dir = g_cinematic.mcla_capture_dir;
inline auto& mcla_capture_ffmpeg = g_cinematic.mcla_capture_ffmpeg;
inline auto& mcla_capture_ffmpeg_args = g_cinematic.mcla_capture_ffmpeg_args;
inline auto& mcla_capture_fps = g_cinematic.mcla_capture_fps;
inline auto& mcla_capture_gate_timeout_ms = g_cinematic.mcla_capture_gate_timeout_ms;
inline auto& mcla_capture_lag = g_cinematic.mcla_capture_lag;
inline auto& mcla_capture_log_every = g_cinematic.mcla_capture_log_every;
inline auto& mcla_capture_luma = g_cinematic.mcla_capture_luma;
inline auto& mcla_capture_buffers = g_cinematic.mcla_capture_buffers;
inline auto& mcla_capture_buffer_every = g_cinematic.mcla_capture_buffer_every;
inline auto& mcla_capture_buffer_region = g_cinematic.mcla_capture_buffer_region;
inline auto& mcla_capture_memory_mb = g_cinematic.mcla_capture_memory_mb;
inline auto& mcla_capture_name = g_cinematic.mcla_capture_name;
inline auto& mcla_capture_out = g_cinematic.mcla_capture_out;
inline auto& mcla_capture_ram_floor_mb = g_cinematic.mcla_capture_ram_floor_mb;
inline auto& mcla_capture_ring = g_cinematic.mcla_capture_ring;
inline auto& mcla_capture_sink = g_cinematic.mcla_capture_sink;
inline auto& mcla_capture_statelog = g_cinematic.mcla_capture_statelog;
inline auto& mcla_capture_virtual_clock = g_cinematic.mcla_capture_virtual_clock;
inline auto& mcla_cin_hud_alpha = g_cinematic.mcla_cin_hud_alpha;
inline auto& mcla_director = g_cinematic.mcla_director;
inline auto& mcla_director_info = g_cinematic.mcla_director_info;
inline auto& mcla_director_root = g_cinematic.mcla_director_root;
inline auto& mcla_director_panel = g_cinematic.mcla_director_panel;
inline auto& mcla_fixed_step_fps = g_cinematic.mcla_fixed_step_fps;
inline auto& mcla_freecam = g_cinematic.mcla_freecam;
inline auto& mcla_freecam_inertia = g_cinematic.mcla_freecam_inertia;
inline auto& mcla_freecam_log = g_cinematic.mcla_freecam_log;
inline auto& mcla_freecam_look = g_cinematic.mcla_freecam_look;
inline auto& mcla_freecam_out = g_cinematic.mcla_freecam_out;
inline auto& mcla_freecam_record = g_cinematic.mcla_freecam_record;
inline auto& mcla_freecam_speed = g_cinematic.mcla_freecam_speed;
inline auto& mcla_scene = g_cinematic.mcla_scene;
inline auto& mcla_scene_restart = g_cinematic.mcla_scene_restart;
inline auto& mcla_scene_seed = g_cinematic.mcla_scene_seed;
inline auto& mcla_scene_seed_each_frame = g_cinematic.mcla_scene_seed_each_frame;
inline auto& mcla_scene_t = g_cinematic.mcla_scene_t;
inline auto& mcla_state_log = g_cinematic.mcla_state_log;

}  // namespace larecomp::settings
