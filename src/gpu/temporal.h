#pragma once
// ===========================================================================
// MCLA GPU renderer -- the groundwork of the temporal upscalers (F4 step 1a)
// ===========================================================================
// feature-parity/F4-DESIGN.md sections 0, 2, 3 and 6.1, as the census of the real game corrected
// them (feature-parity/CENSUS-FINDINGS.md section 3). One component, owned by the Renderer and
// called by the Scene, for the frames that have a 3D scene. At ratio 1 (the scene is drawn at the
// size it is shown at) it does:
//
//   Jitter.   A Halton(2,3) sub-pixel offset is added to SharedConstants::pos_offset, on the copy
//             of the shared constants a draw is about to upload (the memo keeps the unjittered
//             block), of the draws that go to the main view: the scene's colour and depth, found by
//             their exact EDRAM identity at 2x samples (the game's own layout, always; the host's
//             samples are fewer under an upscaler: msaa_level.h; 1x only for a game made without
//             MSAA, temporal_identities.h), only before the
//             anchor and the composite marker, and only world draws (clipping and the viewport
//             transform on, not a rectangle list, a viewport as wide as the target). Everything
//             else keeps its position: the shadow maps, the reflections, the post chain, the UI.
//   Anchor.   The game's motion blur draw, found by its pinned shader pair and checked
//             structurally (its fetches 0 to 2, its colour target, c62). Its pixel shader constants
//             give the camera (temporal_camera.h), checked, and the cut state machine: a camera
//             cut, a change of size, a gap of more than 100 ms, a frame without an anchor before
//             it, or no usable previous matrix for the world gives `reset`.
//   State.    Per guest frame: whether the jitter is armed (the anchor was accepted in the frame
//             before), the jitter and its phase, the sizes, the camera, the anchor's bound
//             textures, the counts of draws; and the states of the run in the log.
//
// Step 1b (temporal_mv.h) adds to it, at the anchor, the passes that keep the scene's depth and make
// the motion vectors from this system's own camera history; the backend steps read `frame()`, `previous_camera()` and `motion().kept()` below.
//
// Step 2 (temporal_backend.h, temporal_ffx.h, temporal_dlss.h) adds the backends: with an upscaler
// chosen (mcla_gpu_upscaler: FSR 3.1, FSR 4, DLSS) and able to run, the system is on and the frame
// goes
//
//   BeginFrame   the chosen upscaler is asked for a context (render and output size of the last
//                accepted frame: ratio 1, "Native AA"); the jitter is armed only while the context is
//                ready, so a frame the upscaler cannot take is plain and never shakes.
//   anchor       after the depth and the vectors, the colour product the anchor reads, the depth and
//                the vectors go through the backend into the output image; a stand-in in the texture
//                cache (textures.h Override) makes every fetch of that product for the rest of the
//                frame read the output: the anchor's own fetch (patched in its constants here), the
//                tone map, the glow and the UI draw on it.
//   failure      a frame that cannot be upscaled is plain (no stand-in); three armed frames in a row
//                that could not be, or one failed dispatch or context, and the backend is off for the
//                session (one log line), the jitter stops, FXAA and SMAA come back.
//   AA           FXAA and SMAA are held off while an upscaler is chosen and can run. MSAA is held off
//                too: the host's level is 1 while an upscaler runs and the player's level comes back
//                when it stops (msaa_level.h; the game itself always draws its 2 sample layout).
//
// The post chain before the anchor. The game's first reads of the scene colour (the guest's post
// function makes a half-size downsample of it, then a quarter-size one, the log-luminance chain of
// the exposure, the bright pass and the bloom's blurs from those; the half-size image is also the
// motion blur's half-size layer) run before the anchor, so before the upscale, and would read the
// raw jittered scene: a glint smaller than a render pixel lands on one in some jitter phases and
// not in others, its bloom pulses with the jitter, and no upscaler can steady what is already in
// the bloom. While the scene is drawn smaller than the output (at the output's size the jitter
// does not do this), those reads are given the previous frame's upscaled picture instead
// (StandInForPostChain): the same stand-in as after the anchor, set at the frame's first main view
// draw, while the output image still holds the frame before. What they make is one frame late: the
// bloom, the exposure and the motion blur's half-size layer. The scene colour the anchor's own
// fetch and the tone map read is this frame's. A camera cut shows the view before it in the cut
// frame's bloom.
//
// The game double-buffers the scene colour: it draws the scene into one of a pair of resolve
// products (the same format, size and guest memory range apart) and alternates them every frame.
// The product this frame's reads name is the anchor's from two frames ago, not the last frame's,
// so the stand-in covers the last two products the anchor read (scene_colors_) and a read of
// either gets the picture. It ends where the anchor draw begins (BeforeTextures): the anchor's
// own fetch is then bound plain, and the anchor's stand-in, this frame's picture, takes over
// after the upscale. The summary counts the frames it was set in, the frames something read it
// before the anchor, and those reads: a run with the stand-in set and no read served means the
// reads name a product it does not cover.
//
// Step 3 (F4-DESIGN.md 4, stage U3) draws the scene smaller than the output: with an upscaler in a
// mode below 100% the main view's colour and depth are made at a scale of their own (edram.h
// SceneScale: the images of the screen's over the mode's ratio, render_scale.h ChooseSceneScale).
// The render size is that scale over the guest's size, the output size the anchor's (an image of
// the screen), the jitter sequence follows the ratio and the scene's textures are sampled sharper
// (scene.cpp SceneBias).
//
// Changing the path (live). The path is the upscaler (off, FSR 3.1, FSR 4, DLSS), its mode and the
// render scale (the scale of the images of the screen, the upscaler's output); the scene's scale,
// the upscaler's context, the jitter's phase count, the history and the hold on FXAA and SMAA all
// belong to it. The game starts on the path the start decided (gpu.h GetStartUpscale, which made
// the scene's first targets, and the start's render scale, render_scale.h); after that the settings
// are read every frame, and when they ask for another path:
//   - Off is taken at the next frame boundary: the scene's targets are made again at the images of
//     the screen's scale (EdramTargets::SetScales), the temporal system turns itself off, the
//     context goes after 10 s (a quick switch back finds it).
//   - A path with a context is made first, and the old path runs meanwhile (make before break):
//     the backend is asked for a second context at the sizes the new path will have (the guest's
//     size at the new scene scale and at the output's; Prepare), and when it says it is ready
//     (PreparedReady: FSR 0.3 to 0.6 s, DLSS about 8 s) the switch is made at the next frame
//     boundary, before the frame's first draw: the scene's targets at the new scale, the context
//     taken over (Commit; the old one retires behind the frames that may use it), the jitter, the
//     history (it restarts: the scale changed and the new context's first dispatch resets) and the
//     AA hold follow the new path from that frame on, whole. A frame is never half on the old path.
//   - The render scale (mcla_gpu_resolution_scale, 0 = the screen's, the FSR1 preset) is part of the
//     path, and so is the output of an upscaler: while a temporal upscaler is the active choice the
//     scale is the screen's whatever the settings say (render_scale.h ChooseUpscalerOutputScale: the
//     window's 16:9 area, which is what the upscaler targets), and the scene is drawn at the mode's
//     share of it. ReadRenderScale reads it with the window's size at every frame boundary, so a
//     fullscreen toggle or a resized window moves the screen's scale and a preset's, and turning an
//     upscaler on or off moves the output between the settings' scale and the screen's. A scale is
//     taken once it has stood for kScaleSettle (a window dragged through many sizes, a row stepped
//     through many values, make no targets and no contexts for the steps in between), except that a
//     scale that moves because an upscaler went on or off is taken at once (the context is then made
//     for the output it will have, not for the old one first), then like any other change: with no
//     upscaler at the next boundary, with one after its new context (the output is another size) is
//     ready. EdramTargets::SetScales moves the scene's targets and every image of the screen
//     together, so that one switch covers a mode and a render scale changed at once.
//   - What is made per use rather than as a target follows in the first frame: the upscaler's output
//     image, the depth and the vectors, the frame layers, the HDR images and the post passes' images
//     are asked for at their sizes by the draws and the passes.
//   - A switch with the system already on goes on with the run (the camera, the cut rules) and
//     draws its first frame armed, upscaled by the new context. One from off has its first frame
//     plain, like the first frame after a pause (the anchor of a frame is accepted before one is
//     armed).
//   - If the targets cannot be made at the new scale (a 3x render scale is 4K: it can fail on a card
//     with little memory; so can an upscaler's output in a large window) nothing changes, it is
//     logged, the pause menu's RENDER SCALE row says "TOO BIG" (gpu.h GetRenderScaleStatus) and the
//     change is asked again after a few seconds, then less often (a failed creation also asks the
//     video memory budget to release what is idle).
//   - DLSS takes one render size per mode for an output size, and the scene is drawn on the scale grid
//     (render_scale.h ChooseSceneScale: multiples of 1/20 of the guest's 1280x720, 0.5x at the least),
//     so a mode can be unable to run at an output: Ultra Performance at 1280x720 or 2560x1440. That
//     is not a failure of DLSS. The backend says which modes cannot run at the output the path is made
//     for (TemporalBackend::AskModes and Modes; the pause menu's DLSS mode row marks them "(N/A)"), and
//     UpdatePath asks for a mode that can: the setting's own when it runs; else the DLSS mode that runs
//     now, if it still can at that output (a mode picked in the menu that cannot run keeps what runs);
//     else the nearest one that can, toward higher quality first (a start or a render scale change
//     that left the setting's mode unable to run: Ultra Performance becomes Performance); else, when
//     no DLSS mode can, no upscaler at that output. The setting itself is never changed. One log line
//     says which and why (ResolveDlssMode).
//   - One case cannot keep the old path upscaled: from one DLSS mode to another. Streamline's API is
//     serialised and the new feature's creation holds it for seconds (TemporalBackend::Busy), so
//     those frames are plain and not jittered, at the old scene scale, until the feature exists.
//   - A choice that cannot run until the game is started again (DLSS in a Vulkan game started
//     without Streamline: gpu.h UpscalerAvailability::dlss_after_restart) changes nothing: the
//     choice is saved, the pause menu says RESTART, and the renderer keeps the path that runs, the
//     upscaler and the mode the scene is drawn for, as they were when it was picked, until the
//     restart. (Not the setting's own mode: the mode row on screen is the chosen upscaler's now.)
//     Picking something that can run drops it, and the switch is made as above.
// Each switch is logged: from, to, and how long the new path took to run its first frame. The
// host MSAA level follows the path once it has settled (UpscalerNext, PathSettled; msaa_level.h).
//
// With no upscaler chosen (or one this PC cannot run) no hook of the Scene does anything beyond one
// branch, no constant changes, no draw is added. With one on, the system jitters, reads the camera
// and logs: "[gpu] temporal: ..." lines, one for each change of what the frames are (no 3D scene,
// 3D scene at 4x samples, anchor missing or refused, running) and a summary when the game exits.
//
// Threading: render thread only.
// ===========================================================================

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "constants.h"
#include "draw_decode.h"
#include "draw_state.h"
#include "edram.h"
#include "render_scale.h"
#include "scale.h"
#include "shaders.h"
#include "temporal_backend.h"
#include "temporal_camera.h"
#include "temporal_identities.h"
#include "temporal_mv.h"
#include "viewport.h"

namespace mcla::gpu {

class ResolveCache;
class TextureCache;

class TemporalSystem {
 public:
  // Phases of the sequence at ratio 1: 8 x ratio^2, rounded up (F4-DESIGN.md 3); at another
  // ratio the runtime's own count (FFX's query for the sizes) or that formula (JitterPhases).
  static constexpr uint32_t kPhasesAtRatio1 = 8;
  static constexpr uint32_t kMaxPhases = 128;
  // A longer gap between two anchors resets the history; the frame time is clamped to this.
  static constexpr double kMaxGapMs = 100.0;
  // Frames that were jittered, saw the main view, and had no accepted anchor, before the jitter is
  // switched off (until the upscaler is turned off and on): a safety net, the census saw none.
  static constexpr uint32_t kDisarmAfterMisses = 3;
  // A new state of the frames is logged once it has lasted this many frames.
  static constexpr uint32_t kModeDebounce = 5;
  // Frames in a row with the post chain's stand-in set and no read before the anchor served by it,
  // before a warning (once): the reads name a scene colour product the stand-in does not cover.
  static constexpr uint32_t kPostUnservedWarn = 120;

  // The kinds of main view draw: colour and depth, colour only (the pass "k_2_10_10_10_FLOAT@1024
  // pitch 1280 2x | no depth" of the census, under 22 draws a frame), depth only (the occlusion
  // draws, 256 a frame at dusk and night).
  enum Kind : uint32_t { kColorDepth, kColorOnly, kDepthOnly, kKinds };
  // Why a main view draw is not a world draw (a draw may have several).
  enum GateBit : uint32_t {
    kGateClipOff = 1u << 0,        // clipping disabled: a screen-space draw
    kGateNoViewport = 1u << 1,     // the viewport transform is off
    kGateRectList = 1u << 2,       // a rectangle list (the clears, the fog quad)
    kGateNarrow = 1u << 3,         // a viewport narrower than the target
  };
  // Why the history starts again (a bit each).
  enum ResetBit : uint32_t {
    kResetFirst = 1u << 0,       // no accepted anchor in the frame before (first, a menu, a pause)
    kResetCut = 1u << 1,         // the camera cut rule fired
    kResetSize = 1u << 2,        // the frame's size or the scene's scale changed
    kResetTime = 1u << 3,        // more than kMaxGapMs since the previous anchor
    kResetNoPrevious = 1u << 4,  // the world's previous matrix (c69) is not usable
  };
  static constexpr uint32_t kResetReasons = 5;
  // What the anchor test said of the frame.
  enum class AnchorStatus : uint8_t {
    kNotSeen,         // the frame has no anchor draw (or the system did not see it prepared)
    kAccepted,
    kNoMainView,      // an anchor draw but no main view draw before it
    kView4x,          // the main view is at 4x samples: the pause menu, the map
    kFetches,         // fetches 0 to 2 are not the colour, stencil and depth textures at one size
    kTarget,          // its colour target is not the half-size float image
    kFrameSize,       // temporal_camera.h Reject
    kConstants,
    kViewProjection,
    kViewInverse,
    kLens,
    kCount,
  };

  // What the Scene tells of a prepared draw, after its texture tables are filled and before its
  // constants are uploaded.
  struct DrawFacts {
    const DrawView* view = nullptr;
    const XenosState* state = nullptr;
    const ConstantBanks::Banks* banks = nullptr;  // the draw's own constants
    const ShaderStore::Entry* ps = nullptr;       // null: depth only
    const ViewportInfo* viewport = nullptr;
    Scale scale;                                  // of the draw's targets
    const EdramKey* color0 = nullptr;             // null: no colour output
    const EdramKey* depth = nullptr;              // null: no depth target
  };

  // The textures the anchor draw binds: what the motion vector pass reads (F4-DESIGN.md 2.3), as
  // the draw's own fetch constants and the table the shaders index (SharedConstants::texture_2d,
  // the bindless slot and the gamma bit).
  struct AnchorTextures {
    bool bound = false;
    uint32_t fetch[3][6] = {};  // the six dwords of fetch slots 0 (scene colour), 1 (stencil view
                                // of the depth, vehicle ID in its stencil byte >> 2), 2 (depth, 1 near)
    uint32_t table[3] = {};
  };

  // Everything of the frame being recorded or, between EndFrame and the next BeginFrame, of the
  // last one.
  struct FrameState {
    uint64_t frame = 0;  // guest frames since the switch was turned on
    uint32_t slot = 0;   // the frame's upload slot

    // Jitter. `jitter` is what this frame's gated draws were moved by, in host pixels of the main
    // view, +y down (zero when not armed). The projection of the vertex shader epilogue shifts
    // the image by +x to the right and +y down by that much.
    bool armed = false;
    uint32_t jitter_index = 0;  // 0 .. phase_count - 1
    uint32_t phase_count = kPhasesAtRatio1;
    float jitter[2] = {};
    double ratio = 1.0;  // output size over render size, per axis: 1 until the scene is drawn smaller

    // The main view (2x samples, before the anchor).
    Scale scale = Scale::None();  // of the first main view draw's targets
    uint32_t samples = 0;         // guest samples of the first one: 2 (1 for a game made without MSAA)
    std::array<uint32_t, kKinds> main_draws{};
    std::array<uint32_t, kKinds> jittered{};
    uint32_t view4x_draws = 0;    // draws into the main view's 4x identity
    bool composite_seen = false;
    // The stencil IDs (bit k, 0 to 23) the main view's draws wrote before the anchor: the vehicles
    // whose matrices the vector pass may use (vehicle_motion.h).
    uint32_t stencil_ids = 0;

    // The anchor.
    uint32_t anchors = 0;         // anchor draws seen (a second one is ignored)
    bool anchor_seen = false;
    bool anchor_ok = false;       // accepted: `camera` and the fields below are valid
    AnchorStatus anchor_status = AnchorStatus::kNotSeen;
    temporal::Camera camera;
    uint32_t reset = 0;           // ResetBit: this frame starts the history again
    temporal::Cut cut;            // the cut test against the frame before (when there was one)
    float frame_time_ms = 1000.0f / 60.0f;  // between this anchor and the one before, 1 to 100
    uint32_t guest_size[2] = {};   // c62: the frame in guest pixels
    uint32_t render_size[2] = {};  // the main view in host pixels (guest size at the scene's scale)
    // What the upscaler makes (F4-DESIGN.md 4): the anchor draws into an image of the screen, whose
    // scale is the output's however much smaller the scene was drawn.
    Scale output_scale = Scale::None();
    uint32_t output_size[2] = {};  // the guest size at output_scale
    AnchorTextures anchor_textures;

    // The upscaler (step 2).
    Upscaler upscaler = Upscaler::kOff;  // the chosen one when this PC can run it, else off
    bool backend_ready = false;          // its context exists for the sizes of the last frame
    bool upscaled = false;               // the output is made and the stand-in set for this frame
    uint32_t anchor_color_slot = 0;      // the bindless slot the anchor's colour fetch reads instead
    // The post chain before the anchor reads the previous frame's output (StandInForPostChain).
    bool post_checked = false;           // the frame's first main view draw decided
    bool post_on_previous = false;       // the stand-in is set for the post chain (until the anchor)
    bool post_counted = false;           // the reads it served are counted (CountPostStandIn)
    uint32_t post_products = 0;          // scene colour products the stand-in covers
    uint64_t post_binds_at_set = 0;      // TextureCache::override_binds() when it was set
  };

  // True while an upscaler was chosen and able to run at the start of the frame being recorded (or
  // frame generation asked for the vectors, below): the Scene's hooks do nothing otherwise.
  bool active() const { return enabled_; }

  // Frame generation (frame_generation.h) wants each frame's depth and motion vectors, camera and cut
  // state with or without an upscaler. While true and no upscaler runs, the system is on for them
  // only ("vectors only"): it finds the anchor, reads the camera and keeps the depth and the vectors,
  // and moves nothing: no jitter, no stand-in, no hold on FXAA, SMAA or MSAA, no context. With an
  // upscaler running it changes nothing (the upscaler keeps the system on). Taken at the next frame
  // boundary (BeginFrame). Render thread.
  void RequestVectors(bool wanted) { vectors_wanted_ = wanted; }

  // The passes of step 1b (temporal_mv.h) need the device; nothing is made until the first anchor.
  // `textures` and `resolve`: the stand-in for the scene colour and the product it replaces.
  // Probes the upscaler backend (a worker thread; nothing on this one). `edram`: the scene's
  // targets, whose scale a change of path moves.
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes,
                  PassTiming& pass_timing, TextureCache& textures, ResolveCache& resolve,
                  EdramTargets& edram);
  // GPU idle, before HostPasses and the BindlessHeap shut down.
  void Shutdown();

  // The Renderer opened the frame's first list (frame slot `slot`, `list` the open list, nothing
  // recorded in it yet and outside a render pass). Reads the upscaler's settings, and at a frame
  // boundary changes the path they ask for once it is ready (see above).
  void BeginFrame(uint32_t slot, plume::RenderCommandList* list);
  // A draw of the frame about to have its textures bound (before FillTextureTables): the anchor
  // draw ends the stand-in the post chain was given, so that its own colour fetch is bound plain
  // (the anchor's stand-in, with this frame's picture, is set after the upscale; a frame whose
  // anchor is not upscaled keeps the plain binding).
  void BeforeTextures(const DrawRecord& record) {
    if (frame_.post_on_previous && record.ps_identity == temporal::kAnchorPs &&
        record.vs_identity == temporal::kAnchorVs) {
      DropPostStandIn();
    }
  }
  // A prepared draw of the frame, after FillTextureTables and before UploadConstants: finds the
  // main view's draws and moves them (`shared` is the draw's own copy), finds the anchor and reads
  // the camera. True when this draw is the anchor, it was taken and its passes are due: the Scene
  // then ends its render pass, calls RecordAnchorPasses and forgets its list state.
  bool OnDraw(const DrawFacts& facts, SharedConstants& shared);
  // The anchor's passes (depth, motion vectors, the checks' copies) into `list`, outside any render
  // pass. False when nothing was recorded.
  // With an upscaler running it also records the upscale and sets the stand-in for the scene colour.
  bool RecordAnchorPasses(plume::RenderCommandList* list);
  // The anchor draw's own constants after RecordAnchorPasses: its colour fetch reads the upscaler's
  // output when there is one this frame.
  void PatchAnchor(SharedConstants& shared) const;
  // The host MSAA level follows the path (msaa_level.h): 1 while an upscaler runs, the player's
  // otherwise. UpscalerNext: the path UpdatePath is steering the frames to (`next_`: what the
  // settings ask for once DLSS's size rules and a choice that waits for a restart are applied) is
  // an upscaler. It is true before the path has changed and false before it is taken off, so the
  // level's pipelines are made meanwhile. PathSettled: the start's path was taken and no path or
  // render scale change is waiting or being made (UpdatePath has said so this frame), so the targets
  // may be remade for the level without meeting the path's own remake. Render thread.
  bool UpscalerNext() const { return next_ != Upscaler::kOff; }
  bool PathSettled() const { return path_adopted_ && !change_pending_ && !stage_.active; }
  // D8: whether FXAA and SMAA are held off this frame (an upscaler is chosen and can run). Logs
  // once per change when `aa_asked` (mcla_gpu_aa is not off).
  bool HoldsAaOff(bool aa_asked);
  // The composite marker: the UI starts.
  void OnComposite();
  // The guest's present: the frame ends, the state machine moves on, the log.
  void EndFrame();
  // The renderer shuts down: the last summary.
  void Finish();

  // The frame being recorded (valid fields as documented above), or the last one.
  const FrameState& frame() const { return frame_; }
  // The camera of the frame before when its anchor was accepted and this frame follows it
  // directly (null: the history starts again).
  const temporal::Camera* previous_camera() const { return ran_last_ ? &last_camera_ : nullptr; }
  // The depth and motion vector images kept for the last frame (temporal_mv.h Kept: sizes, formats).
  const TemporalMotion& motion() const { return motion_; }
  // The cars' shadow decals' mask (vehicle_motion_mask.h): the Scene draws the decals again into it
  // while an upscaler runs (scene.cpp PrepareShadowMask, RecordShadowMask).
  ShadowMask& shadow_mask() { return motion_.shadow_mask(); }
  // The video memory the temporal path holds now: the upscaler's output image, the depth and the
  // vectors at the render size, and the runtime's own context as it reports it (about 500 MB at
  // 4K for FSR). Render thread (the backend's status is read under its lock).
  uint64_t HeldBytes() const;

 private:
  using Clock = std::chrono::steady_clock;

  // What the frames are, for the log.
  enum class Mode : uint8_t { kNoScene, kView4x, kNoAnchor, kRunning, kCount };

  struct Window {
    uint64_t frames = 0;
    uint64_t armed = 0;
    uint64_t modes[size_t(Mode::kCount)] = {};
    uint64_t anchor_status[size_t(AnchorStatus::kCount)] = {};
    uint64_t resets = 0;
    uint64_t reset_reasons[kResetReasons] = {};
    uint64_t cut_distance = 0;  // camera cuts by clause (a cut may have several)
    uint64_t cut_turn = 0;
    uint64_t cut_fov = 0;
    // The upscaler: frames it ran in, and the frames with an upscaler chosen that it did not run in.
    uint64_t upscaled = 0;
    uint64_t upscale_plain_not_ready = 0;  // no context yet (or the sizes changed): plain, no jitter
    uint64_t upscale_plain_no_vectors = 0;
    uint64_t upscale_plain_no_color = 0;   // the colour product could not be had as an input
    uint64_t upscale_resets = 0;           // upscaled frames the history started again in
    uint64_t post_stand_in_set = 0;        // frames the post chain was offered the previous output
    uint64_t post_on_previous = 0;         // ... of them, frames a read before the anchor got it
    uint64_t post_reads_served = 0;        // reads before the anchor the stand-in served
    // The last accepted camera, for the line.
    bool have_camera = false;
    temporal::Camera camera;
    uint32_t render_size[2] = {};
  };

  void SetEnabled(bool enabled);
  // At a frame boundary: the system goes on or off for RequestVectors.
  void SyncVectors();
  // On for the vectors alone: nothing is jittered or replaced.
  bool VectorsOnly() const { return vectors_on_ && effective_ == Upscaler::kOff; }
  void OnMainView(const DrawFacts& facts, const temporal::MainMatch& match, SharedConstants& shared);
  bool OnAnchor(const DrawFacts& facts, const SharedConstants& shared);
  static uint32_t EvaluateGate(const XenosState& state, const DrawRecord& record,
                               const ViewportInfo& viewport, Scale scale);
  // Structure of the anchor: its fetches and its target. Fills `*width`, `*height` (the fetches'
  // size) and `*why` on a refusal.
  AnchorStatus CheckAnchor(const DrawFacts& facts, uint32_t* width, uint32_t* height,
                           std::string* why) const;
  static Mode ModeOf(const FrameState& frame);
  void NoteMode(Mode mode, const FrameState& frame);
  void Report();
  void ResetRun();

  bool enabled_ = false;
  bool vectors_wanted_ = false;  // RequestVectors
  bool vectors_on_ = false;      // SyncVectors acted on it
  bool frame_open_ = false;  // BeginFrame ran and no present has ended the frame yet
  uint64_t frame_number_ = 0;
  FrameState frame_;

  // The run: what the frames before this one left.
  bool ran_last_ = false;    // the previous frame's anchor was accepted
  temporal::Camera last_camera_;
  // The camera's last step (position now minus position before), what the cut test predicts the
  // next one from; unknown at the first step of a run.
  double last_step_[3] = {};
  bool last_step_known_ = false;
  Scale last_scale_ = Scale::None();
  Clock::time_point last_anchor_time_{};
  uint32_t jitter_index_ = 0;  // advances in frames whose anchor was accepted
  uint32_t misses_ = 0;
  bool disarmed_ = false;

  // Step 1b: the passes and what they keep (temporal_mv.h), and what the anchor told them.
  TemporalMotion motion_;
  TemporalMotion::Frame pending_;
  bool pending_ready_ = false;

  // Step 2: the upscaler. `wanted_` is what the settings and this PC's answer make of the player's
  // choice, read at BeginFrame; `effective_` and `mode_in_use_` are the path the frames follow.
  void ReadUpscalerChoice();
  // Whether `upscaler` has a backend here and the PC's answer says it can run now.
  bool CanRun(Upscaler upscaler, const UpscalerAvailability& available) const;
  // The live change of path (above). UpdatePath runs at a frame boundary: it takes the start's path
  // the first time, then compares the path with what the settings want and prepares, switches or
  // waits. SwitchPath makes the switch (false: the scene's targets could not be made, nothing
  // changed). The stage is the new path while its context is being made.
  void UpdatePath(plume::RenderCommandList* list);
  // The render scale the settings ask for now (the cvars and the window's size, gpu.h
  // WantedRenderScale; the screen's while a temporal upscaler is the active choice, UpscalerOwnsOutput),
  // read at every frame boundary; UpdatePath takes it once it has stood.
  void ReadRenderScale();
  // A temporal upscaler is the active choice, so its output (the screen's, the window's 16:9 area) is
  // the render scale: the upscaler the settings and this PC's answer make of the player's choice
  // (`wanted_`), except while DLSS waits for a restart (`restart_pending_`), when the path that runs
  // stays and so does its output.
  bool UpscalerOwnsOutput() const;
  // The output an upscaler has in this window: the screen's scale now (the pre-warm makes the contexts
  // a switch will ask for), `fallback` while no window size is known.
  Scale UpscalerOutputScale(Scale fallback) const;
  // DLSS's first-time work done in the background while the game runs some other way (temporal_backend.h
  // Prewarm): called at every frame boundary, one atomic read when there is nothing to do. It waits
  // for the guest's frame size and kPrewarmDelay after it was first known, so that the first seconds
  // of the game's own start-up are left alone, then issues one step at a time.
  void PrewarmDlss();
  void AdoptStartPath();
  bool SwitchPath(plume::RenderCommandList* list, Upscaler to, UpscaleMode mode,
                  const BackendRequest* request, Scale output);
  void CancelStage(const char* why);
  // The guest's frame size: the last accepted anchor's, else the game's screen; false until known.
  // `quiet`: a poll (PrewarmDlss) that must not log the game's unset screen size as an error.
  bool GuestFrameSize(uint32_t out[2], bool quiet = false) const;
  // The context the path (`upscaler`, `mode`, images of the screen at `output`) needs for a guest
  // frame of `guest` pixels: the render size at the scene's scale for that mode and the output size
  // at the images of the screen's.
  BackendRequest RequestFor(Upscaler upscaler, UpscaleMode mode, const uint32_t guest[2],
                            Scale output) const;
  // What each DLSS mode would draw the scene at for an output of `output` (the same render sizes
  // RequestFor gives), for the backend's question which of them DLSS takes (AskModes).
  ModeSizes DlssModeSizes(const uint32_t guest[2], Scale output) const;
  // The DLSS mode the path is to have at `output`'s sizes `sizes` when the settings ask for `wanted`
  // (see "Changing the path": the mode itself, else the one that runs now, else the nearest that can
  // run). `*any` is false when no DLSS mode can run there. Logs, once for a change of the answer,
  // when `wanted` cannot run. A mode the backend does not know yet counts as able to run: its
  // context makes the check itself (temporal_dlss.cpp MakeContext) and the answer comes next frame.
  UpscaleMode ResolveDlssMode(UpscaleMode wanted, const ModeSizes& sizes, bool* any);
  // The jitter sequence's length for `ratio` (output over render): `from_runtime` is what the
  // upscaler's context says for its sizes (0: none), else 8 x ratio^2 rounded up; 1 to kMaxPhases.
  static uint32_t JitterPhases(double ratio, uint32_t from_runtime);
  void MakeRequest(BackendRequest* request) const;
  // The upscale of the anchor's frame: false when this frame is plain (the reason counted). A real
  // failure turns the upscaler off for the session.
  bool RecordUpscale(plume::RenderCommandList* list);
  // The first main view draw of an armed frame: the post chain's reads before the anchor get the
  // previous frame's upscaled picture when there is one that fits (see the header comment).
  void StandInForPostChain();
  // The product the anchor read as the scene colour in a frame it upscaled: the stand-in of the
  // next frames' post chains covers the last two (the game alternates a pair).
  void NoteSceneColor(const void* product, uint32_t page, uint64_t frame);
  // The reads the stand-in served before the anchor are counted, once for the frame.
  void CountPostStandIn();
  // The anchor did not upscale the frame: what the post chain was given goes, the tone map and
  // the rest read this frame's own product.
  void DropPostStandIn();
  void FailUpscaler(const std::string& why);
  void ReleaseUpscaler();
  void ClearStandIn();

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  HostPasses* passes_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  TextureCache* textures_ = nullptr;
  ResolveCache* resolve_ = nullptr;
  // The upscaler backends (each null in a build without its runtime): FidelityFX for FSR 3.1 and
  // FSR 4, Streamline for DLSS. Both are probed; the chosen one runs.
  std::unique_ptr<TemporalBackend> ffx_;
  std::unique_ptr<TemporalBackend> dlss_;
  TemporalBackend* BackendFor(Upscaler upscaler) const {
    return upscaler == Upscaler::kDlss ? dlss_.get()
           : (upscaler == Upscaler::kFsr31 || upscaler == Upscaler::kFsr4) ? ffx_.get()
                                                                           : nullptr;
  }
  EdramTargets* edram_ = nullptr;
  UpscalerSettings settings_;
  // The path the frames follow: the upscaler (off when none runs) and the mode the scene was drawn
  // for. Changed only at a frame boundary (SwitchPath).
  Upscaler effective_ = Upscaler::kOff;
  // The path UpdatePath steers to: `wanted_` after DLSS's size rules, or `effective_` while a choice
  // waits for a restart. Equal to `effective_` once the path has settled; kOff until UpdatePath has
  // taken the start's path (UpscalerNext).
  Upscaler next_ = Upscaler::kOff;
  // What the settings ask for, with this PC's answer applied (an upscaler it cannot run is off;
  // see restart_pending_ for the one that can run from the next start).
  Upscaler wanted_ = Upscaler::kOff;
  UpscaleMode wanted_mode_ = UpscaleMode::kNativeAa;
  // The chosen upscaler can only run from the next start (DLSS in a Vulkan game started without it):
  // set by ReadUpscalerChoice, acted on by UpdatePath, which then wants the path that runs
  // (effective_ and mode_in_use_, the mode it was drawn for when DLSS was picked: they only change
  // in SwitchPath) instead of `wanted_`. `hold_reported_`: the log line for it was written.
  bool restart_pending_ = false;
  bool hold_reported_ = false;
  bool path_adopted_ = false;  // the start's path was taken (AdoptStartPath)
  Clock::time_point prewarm_wait_since_{};  // the guest's size was first known with a pre-warm armed
  static constexpr std::chrono::seconds kPrewarmDelay{5};
  // The new path whose context is being made while the old one runs.
  struct Stage {
    bool active = false;
    Upscaler upscaler = Upscaler::kOff;
    UpscaleMode mode = UpscaleMode::kNativeAa;
    Scale output;  // the images of the screen it was made for
    BackendRequest request;
  };
  Stage stage_;
  // A change is on: the settings ask for another path than the one running. When it was first seen,
  // for the log; where the scene's targets could not be made, when to try again.
  bool change_pending_ = false;
  Clock::time_point change_seen_{};
  Clock::time_point retry_after_{};
  // How long a change whose targets could not be made waits before it is tried again: it doubles
  // with each failure, up to kSwitchRetryMax, and starts again after a switch is made.
  std::chrono::seconds retry_delay_{3};
  uint64_t retry_key_ = 0;  // the change (path and render scale) retry_after_ and retry_delay_ are for
  bool switch_failure_logged_ = false;
  // The answer ResolveDlssMode last logged (the mode asked for, the mode it came to, the output),
  // 0 when the mode asked for runs: a line is written when it changes.
  uint64_t dlss_refusal_key_ = 0;
  // The render scale the settings ask for (ReadRenderScale): the scale, what it came from (for the
  // log), and when it last changed. A change is taken once it has stood for kScaleSettle.
  Scale render_wanted_ = Scale::None();
  RenderScaleChoice render_choice_;
  Clock::time_point render_wanted_since_{};
  bool render_invalid_logged_ = false;
  // ReadRenderScale's last answer to UpscalerOwnsOutput: a scale that moves with it is not a drag.
  bool render_upscaling_ = false;
  // `render_wanted_` moved with an upscaler going on or off: taken without kScaleSettle.
  bool render_wanted_at_once_ = false;
  static constexpr std::chrono::milliseconds kScaleSettle{400};
  // The line that says when the new path first ran, once: set by a switch to a path with an
  // upscaler, written by its first upscaled frame (`switch_asked_`: when the setting changed).
  bool switch_log_pending_ = false;
  Clock::time_point switch_asked_{};
  uint32_t choice_key_ = 0;
  bool choice_reported_ = false;
  bool ready_reported_ = false;
  bool busy_reported_ = false;       // the backend's runtime is occupied (Busy), as last said
  HostImage output_;                 // the upscaler's output: R16G16B16A16_FLOAT, output size
  uint32_t last_render_size_[2] = {};  // of the last accepted frame
  uint32_t last_output_size_[2] = {};  // ... and what the upscaler made of it
  // The mode the scene is drawn for (the path's), whatever the setting says now. Native AA when no
  // upscaler runs (the scene is at the output's size).
  UpscaleMode mode_in_use_ = UpscaleMode::kNativeAa;
  bool request_made_ = false;        // a context was asked for and not released
  BackendRequest request_;
  Clock::time_point off_since_{};
  bool stand_in_set_ = false;
  uint64_t last_dispatch_frame_ = ~uint64_t(0);
  // What the last upscaled frames set as the anchor's stand-in: the scene colour products replaced
  // (newest first; the game alternates two, NoteSceneColor), with the frame of each and its page,
  // and the output's scale (the output image is output_). The products are only compared.
  struct SceneColor {
    const void* product = nullptr;
    uint32_t page = 0;
    uint64_t frame = 0;
  };
  static constexpr size_t kSceneColorProducts = 2;
  // A product older than this many frames is not trusted any more (a frame in between that was
  // not upscaled, a size change): the pair's other buffer was last read two frames ago.
  static constexpr uint64_t kSceneColorMaxAge = 2;
  std::array<SceneColor, kSceneColorProducts> scene_colors_{};
  Scale scene_color_scale_ = Scale::None();
  bool post_stand_in_reported_ = false;
  uint32_t post_unserved_run_ = 0;  // frames in a row the stand-in was set and nothing read it
  bool post_unserved_reported_ = false;
  uint32_t upscale_misses_ = 0;      // armed frames in a row the upscaler did not take
  bool aa_hold_reported_ = false;
  std::array<bool, 4> plain_logged_{};

  // The log.
  Window window_;
  Clock::time_point window_start_{};
  Mode shown_mode_ = Mode::kCount;    // kCount: none shown yet
  Mode pending_mode_ = Mode::kCount;
  uint32_t pending_frames_ = 0;
  std::array<bool, size_t(AnchorStatus::kCount)> status_logged_{};
  uint32_t reset_lines_ = 0;
  double logged_lens_[3] = {};                 // field of view (rad), near, far of the last line
  uint32_t lens_lines_ = 0;
};

}  // namespace mcla::gpu
