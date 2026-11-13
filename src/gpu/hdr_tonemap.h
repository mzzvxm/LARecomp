#pragma once
// ===========================================================================
// MCLA GPU renderer -- the scene above paper white: the tone map drawn again into a float image
// ===========================================================================
// HDR stage 3 (feature-parity/HDR-PLAN.md). The game's post chain ends with a tone map: one
// full-screen pixel shader that reads the scene (16-bit float, 1 = the game's white), the motion
// blur and the bloom, divides by the adapted luminance, bends the luminance with a Reinhard curve
// that reaches its white at `White`, adds the bloom, grades (saturation, colour balance,
// contrast), applies the game's encode and writes 8-bit code values to the UI target (colour
// k_8_8_8_8, 1x), clipped at 1. Everything the sun, the sky at dusk, the headlights and the
// neon signs have above that white is lost there.
//
// With HDR output (HdrScene's float image under the UI) the draw is made twice. The game's own
// draw goes to the target as always (the guest's frame, the underlay and every readback are what
// they were). Right after it the same draw -- same vertex shader, vertex and index buffers,
// textures, constants and viewport -- is drawn into `scene_hdr` (R16G16B16A16_FLOAT, the target's
// region) with the shader's HDR variant (shaders.h kHdrVariant; the pack builder changes the
// luminance curve to continue above the knee as an exponential shoulder towards the display's
// peak and writes the encode unclamped, NaN-free). Then every draw the game makes into the same
// target until the composite marker -- the halos and the night glows -- is drawn into `scene_hdr`
// too, with the same shader and blend and the float format: an additive glow adds in code values
// as it does in 8 bits, without the clip. At the marker `scene_hdr` is the scene as the game
// would have made it with no clipping; FrameLayers captures it with the frame, and the HDR
// present composes it under the UI where it would use the 8-bit underlay (HdrScene::Compose).
//
// What a frame needs, and what it does when it cannot have it (counted, logged once per reason,
// and the frame keeps stage 2's picture: the scene at paper white, nothing is ever black):
//   - the variant pack and its pipelines (a missing one is built in the background: the first
//     frames after a start may lack the HDR scene, as a draw waits for its pipeline);
//   - the tone map recognised: the pixel shader with an HDR variant, drawn with one of the vertex
//     shaders the variant was built for, into a single-sampled R8G8B8A8 target, blending off, writing
//     RGB, no alpha test, no depth or stencil test that could reject, its scissor and viewport
//     starting at the origin (the float image is where they overlap);
//   - no anti-aliasing (mcla_gpu_aa fxaa/smaa runs on the 8-bit target at the marker; its luma
//     passes were written for 8-bit values, so the HDR scene is off while it is on -- unless a
//     temporal upscaler is on, which holds those two off, DrawFacts::aa_held_off);
//   - every later draw into the target before the marker reproducible: the same target (one
//     colour output, one sample), blending as the game has it, no depth or stencil test that could
//     reject. A
//     draw that cannot be reproduced but only adds light (colour ADD with destination ONE) is
//     left out of the float scene -- its glow is missing there, nothing else changes; any other
//     draw ends the frame's HDR scene. A colour clear of the target ends it too;
//   - the marker on the same target at the size the tone map covered.
//
// The shoulder: Reinhard extended, Ld = Ls (1 + Ls / W) / (1 + Ls), is the game's curve; up to the
// knee (0.6 of its white, in its linear units) it is used as it is. Above it an exponential
// approach to the peak (the display's, in the same units: where the viewing transfer -- the
// game's encode, the display's gamma table, the sRGB decode -- reaches peak / paper white) takes
// over with the same value and a slope that is the game curve's there times
// mcla_gpu_hdr_highlights, and at least what reaches the game's white at its white; never under
// the game's curve up to its white; with the peak at or below the game's white there is no
// shoulder (the curve clipped at its white). The algorithm is the fork's (its compute pass,
// patches/rexglue-hdr-scene-mcla.patch); the shader side is hlsl/hdr_tonemap_shoulder.hlsli.
//
// Cost (RTX-class GPU at 3x, estimated from the fork's 0.2 ms for its compute pass): one more
// full-screen pixel pass with the tone map's reads, plus about as many small draws as the post
// chain has after it. Memory: the float image, 7.4 MB at 1x and 66 MB at 3x, made at the first
// tone map and released after kReleaseAfterFrames guest frames without an HDR scene.
//
// Threading: Initialize/Shutdown on the lifecycle thread (before the render thread starts and
// after it stops), Initialize also on the render thread at a frame boundary when HDR output is
// turned on after the start (Renderer::SetUpHdrScene); the rest on the render thread.
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>

#include <plume_render_interface.h>

#include "constants.h"
#include "draw_state.h"
#include "edram.h"
#include "frame_layers.h"
#include "hdr.h"
#include "host_passes.h"
#include "pipelines.h"
#include "post.h"
#include "scale.h"
#include "shaders.h"
#include "viewport.h"

namespace mcla::gpu {

class PassTiming;

class HdrToneMap : public SceneImageSource {
 public:
  // Where the HDR curve leaves the game's (the game's linear units).
  static constexpr float kKnee = 0.6f;
  // Guest frames without an HDR scene after which the image is released (20 s at 60 fps).
  static constexpr uint32_t kReleaseAfterFrames = 1200;

  // What the Scene does for a draw it asked about.
  enum class Action : uint8_t {
    kNone,
    kToneMap,  // the tone map: drawn again with the HDR variant (starts the frame's float scene)
    kAfter,    // a draw into the same target before the marker: drawn again as it is
  };

  enum class Cause : uint8_t {
    kAa,            // mcla_gpu_aa is on
    kTarget,        // the tone map's target is not a single-sampled R8G8B8A8 one
    kState,         // blending on, a mask without RGB, an alpha test, or depth/stencil in use
    kCoverage,      // the tone map's scissor or viewport does not start at the origin
    kPipeline,      // a pipeline could not be made (one still being made is not an error)
    kImage,         // the float image could not be made
    kUpload,        // no upload memory for the tone map's constants
    kAfterTarget,   // a later draw into the target that cannot be drawn again (MRT, MSAA, depth)
    kClear,         // a clear of the target after the tone map
    kMarker,        // the marker came on another target, at another size, or not at all
    kCount,
  };

  // What Classify needs of a prepared draw.
  struct DrawFacts {
    const ShaderStore::Entry* vs = nullptr;
    const ShaderStore::Entry* ps = nullptr;  // null: no pixel shader
    const PipelineKey* key = nullptr;        // as the Scene built it
    EdramKey color0;                         // the draw's colour 0 identity
    uint32_t host_samples = 1;               // of its targets
    Scale scale;                             // of its targets
    const XenosState* state = nullptr;
    const plume::RenderViewport* viewport = nullptr;  // host pixels
    const uint8_t* ps_bank = nullptr;        // the guest's pixel constant bank (big-endian)
    // An upscaler is on, which holds mcla_gpu_aa's FXAA and SMAA off (TemporalSystem::HoldsAaOff):
    // they do not run at the marker, so the setting is not a reason to keep the float scene off.
    bool aa_held_off = false;
  };

  HdrToneMap() = default;
  ~HdrToneMap() override;
  HdrToneMap(const HdrToneMap&) = delete;
  HdrToneMap& operator=(const HdrToneMap&) = delete;

  // After the FrameLayers, the PostAa and the ShaderStore. The store's HDR pack may not be loaded
  // yet (a shader pack attached after the renderer started: Renderer::OnShaderPackAttached loads
  // it); active() is false until it is, and the HDR output stays at stage 2 meanwhile (or for good
  // when the pack is not usable: the Renderer logged why).
  // CHECK: a first run whose packs are built after the renderer is up (AttachShaderPacks): the
  // variants load on that thread while the render thread idles; the float scene should start with
  // the first guest frames, no restart needed ("shader pack: N HDR scene variants" in the log).
  bool Initialize(HostPasses& passes, ShaderStore& shaders, FrameLayers& layers, PostAa& post,
                  PassTiming& pass_timing);
  // GPU idle, before HostPasses shuts down.
  void Shutdown();
  // Initialize ran (and Shutdown did not).
  bool ready() const { return initialized_; }
  // HDR output went off (render thread, at a frame boundary): the float image is released and a
  // scene under way is dropped; the frames that carried it lose it (the epoch moves). Nothing runs
  // while the layers are inactive (active()), so this is only for the memory and a clean start the
  // next time.
  void Release();

  // HDR output is on, its UI layers are active and the variants are loaded: what the Scene checks
  // for each draw.
  bool active() const {
    return initialized_ && layers_ != nullptr && layers_->active() && shaders_ != nullptr &&
           shaders_->has_hdr_variants();
  }

  // Whether a frame made now can carry the float scene: active(), and no FXAA or SMAA that would
  // run at the marker (an upscaler that holds them off, `aa_held_off`, lets it through; this is
  // Classify's kAa test). The scene's resolve comes before the tone map draw that starts the float
  // scene, so this is the most the resolve can know: the draw's own checks (target, state,
  // coverage, pipeline) can still refuse the frame afterwards. Render thread.
  bool FloatSceneLikely(bool aa_held_off) const {
    return active() && post_ != nullptr &&
           (aa_held_off || post_->CurrentMode() == PostAa::Mode::kOff);
  }

  // Once per present, before the frame's draws: the display's levels and the game's display gamma
  // table (the green channel, `ramp[256]`, 10-bit values over 1023; null: the identity), for the
  // viewing transfer the peak is inverted through.
  void SetDisplay(const HdrLevels& levels, const float* ramp);

  // A guest frame's first list was opened.
  void BeginFrame();

  // ---- The Scene's hooks (render thread) ----------------------------------------------------
  // Whether a draw with this packed blend (PipelineKey::blend[0]) only adds light: colour ADD with
  // destination ONE.
  static bool AddsLight(uint32_t packed_blend);

  // Whether a draw with this pixel shader could be of interest (the tone map's shader, or any draw
  // while a float scene is under way): the Scene builds the facts for Classify only then.
  bool Wants(const ShaderStore::Entry* ps) const {
    return phase_ == Phase::kPending || (ps != nullptr && ps->hdr_variant);
  }

  // A draw about to be prepared. kToneMap and kAfter ask the Scene to look the pipeline up with
  // VariantKey and, once the draw is recorded, to record the draw again (Scene::RecordHdrDuplicate)
  // into Image(action), then call Duplicated (or Failed). `facts.ps_bank` is read only for the
  // tone map.
  Action Classify(const DrawFacts& facts);
  // The key of the second draw: the first's with the float format, no depth and no stencil, and
  // for the tone map the HDR variant of its pixel shader.
  void VariantKey(Action action, const PipelineKey& key, PipelineKey* out) const;
  // The 16 bytes the tone map's variant reads after the shared constants (Classify read the bank).
  const HdrSceneConstants& constants() const { return constants_; }
  // The float image for the second draw of `action`, made (at the tone map's region) when it is
  // the tone map; null (logged) when it could not be made.
  HostImage* Image(Action action);
  // The Scene recorded the second draw (render pass closed afterwards). For the tone map this is
  // what starts the frame's float scene.
  void Duplicated(Action action);
  // The second draw could not be made or recorded: for the tone map nothing starts (`waiting`: its
  // pipeline is being made in the background, counted and not logged); after it, a draw that only
  // adds light (`adds_light`) is left out of the float scene and any other ends it.
  void Failed(Action action, Cause cause, bool waiting, bool adds_light);
  // The colour clear of the UI target `key` (before it is recorded): ends the float scene.
  void OnColorClear(const EdramKey& key);
  // The composite marker, the render pass closed, on `target` whose region is `width` x `height`
  // host pixels (FrameLayers::Region): the float scene is ready when it matches. The image goes
  // to SHADER_READ.
  void OnComposite(plume::RenderCommandList* list, const EdramKey& target, uint32_t width,
                   uint32_t height);
  // The guest's present: counts the frames without a float scene, for the release.
  void OnGuestPresent();

  // ---- SceneImageSource ---------------------------------------------------------------------
  const HostImage* FinishedScene(uint32_t width, uint32_t height, uint32_t* epoch) override;
  uint32_t SceneEpoch() const override { return epoch_; }

  uint64_t HeldBytes() const;
  std::string TakeSummary();

 private:
  enum class Phase : uint8_t { kIdle, kPending, kReady, kFailed };

  Action ClassifyToneMap(const DrawFacts& facts);
  void NoteAfter(const DrawFacts& facts, bool reproducible);
  // The peak in the game's linear units for the levels and the table.
  float PeakInGameUnits(float gamma) const;
  float ViewingTransfer(float game_linear, float gamma) const;
  void Fail(Cause cause, const std::string& detail);

  HostPasses* passes_ = nullptr;
  ShaderStore* shaders_ = nullptr;
  FrameLayers* layers_ = nullptr;
  PostAa* post_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  bool initialized_ = false;

  HdrLevels levels_;
  std::array<float, 257> ramp_{};  // the display table's green channel, one extra for the last step
  bool have_ramp_ = false;

  HdrSceneConstants constants_{};
  // The tone map Classify accepted, until the Scene has recorded its second draw (Duplicated).
  bool candidate_valid_ = false;
  EdramKey candidate_target_;
  uint32_t candidate_guest_width_ = 0;
  uint32_t candidate_guest_height_ = 0;
  uint32_t candidate_host_width_ = 0;
  uint32_t candidate_host_height_ = 0;

  HostImage scene_;
  Phase phase_ = Phase::kIdle;
  EdramKey target_;                // the tone map's target
  uint32_t guest_width_ = 0;       // the tone map's region, guest pixels
  uint32_t guest_height_ = 0;
  uint32_t host_width_ = 0;        // and in host pixels
  uint32_t host_height_ = 0;
  uint32_t epoch_ = 1;             // changes whenever the image's content or identity does
  bool ready_this_frame_ = false;
  uint32_t idle_frames_ = 0;

  std::array<bool, size_t(Cause::kCount)> cause_logged_{};
  bool recognised_logged_ = false;
  bool glow_logged_ = false;
  bool vs_logged_ = false;
  std::unordered_set<uint64_t> after_logged_;

  struct Stats {
    uint64_t tone_maps = 0;     // tone map draws drawn again
    uint64_t after = 0;         // later draws drawn again
    uint64_t skipped_glows = 0; // later draws that only add light, left out
    uint64_t ready = 0;         // frames whose float scene reached the marker
    uint64_t captured = 0;      // ... and was taken by a frame's layers
    uint64_t waiting = 0;       // a tone map left out while its pipeline is being made
    std::array<uint64_t, size_t(Cause::kCount)> failed{};
  };
  Stats stats_;
};

}  // namespace mcla::gpu
