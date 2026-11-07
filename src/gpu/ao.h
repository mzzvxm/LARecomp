#pragma once
// ===========================================================================
// MCLA GPU renderer -- screen-space ambient occlusion (feature-parity/AO-DESIGN.md)
// ===========================================================================
// The game has baked vertex occlusion in the world and a contact shadow under cars, and nothing that
// darkens creases, kerbs and wheel arches. This adds a screen-space term of its own, GTAO after
// Intel's XeGTAO (MIT) written as pixel shaders, applied as a weighted multiply on the scene colour.
//
// WHERE: at the game's mid-scene depth resolve (the first resolve per frame of the main view's depth,
// outside EndTiling, into 078B0000): after every opaque draw of the main view, before the soft
// particles, the rain, the fog quad and the post chain. The depth is complete there and is read
// straight from the EDRAM depth target, guest sample 0 (the sample the game's own particle and fog
// reads see).
//
// This file is built in steps. Step 0 (the census, below) only watches: it counts the main view's
// draws around the trigger, keeps the light constants the design needs (PS c26 ambient, PS c39 sun
// colour, VS c16 sun direction), reads the camera at the game's motion blur draw and logs all of it,
// one line per census frame, so that a run can settle the design's CHECK items before any shader
// exists (mcla_gpu_ao_census, off by default).
//
// Threading: render thread only.
// ===========================================================================

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "constants.h"
#include "edram.h"
#include "records.h"
#include "resolve.h"
#include "temporal_camera.h"
#include "temporal_identities.h"

namespace mcla::gpu {

class AmbientOcclusion {
 public:
  // The destination of the game's mid-scene depth resolve in the retail build (k_24_8_FLOAT,
  // 1280x720, the product the soft particles and the fog quad read), as the other identities of
  // temporal_identities.h: a different build gives another address and then there is no AO, never a
  // wrong picture.
  static constexpr uint32_t kTriggerDestination = 0x078B0000;

  // What the Scene tells of a prepared draw of the main view (temporal_identities.h MatchMainView).
  struct MainDraw {
    temporal::MainMatch match;
    bool writes_depth = false;  // the pipeline writes depth
  };

  // Why a depth resolve is or is not the trigger (one bit each for what failed).
  enum Refusal : uint32_t {
    kRefusedNotDepthCopy = 1u << 0,   // not a copy of an EDRAM depth target
    kRefusedNotMainDepth = 1u << 1,   // the source is not the main view's depth identity
    kRefusedEndTiling = 1u << 2,      // inside EndTiling (the end-of-scene copies)
    kRefusedNotFirst = 1u << 3,       // not the first qualifying resolve of the frame
    kRefusedAfterAnchor = 1u << 4,    // the anchor draw or the composite marker came first
    kRefusedDestination = 1u << 5,    // not into kTriggerDestination
    kRefusedClearsDepth = 1u << 6,    // the same call clears the depth after the copy
    kRefusedRectangle = 1u << 7,      // the rectangle is not the whole picture from the origin
  };

  AmbientOcclusion() = default;
  AmbientOcclusion(const AmbientOcclusion&) = delete;
  AmbientOcclusion& operator=(const AmbientOcclusion&) = delete;

  // The Scene opened a frame's first list: reads the settings.
  void BeginFrame();
  // True while the draw hooks have anything to do (the census is on).
  bool wants_draws() const { return census_every_ != 0; }

  // A prepared draw into the main view's identities (not the pause view's), with the draw's own
  // banks, before the draw is recorded.
  void OnMainDraw(const MainDraw& draw, const ConstantBanks::Banks& banks);
  // The game's motion blur draw: the camera of this frame, which the next frame's trigger uses.
  void OnAnchor(const ConstantBanks::Banks& banks);
  // A depth clear of the identity `key` (the main view's depth is cleared at the start of the main
  // view; its z says which end of the range is far).
  void OnDepthClear(const EdramKey& key, float z);
  // A resolve, after ResolveCache::Execute made its copy.
  void OnResolve(const ResolveRecord& record, const ResolveCache::Summary& summary);
  // The scene is composed: the UI starts.
  void OnComposite() { composite_seen_ = true; }
  // A guest present: the frame boundary. Prints the census frame.
  void OnPresent();

 private:
  // The light constants of a draw, as the raw big-endian dwords of the registers: PS c26.xyz (the
  // ambient, gLightAmbient), PS c39.xyz (the sun colour, gLightColor), VS c16.xyz (the sun
  // direction, gLightPosDir). Compared as bytes; decoded for the log and the shaders.
  struct LightRaw {
    uint32_t dwords[9] = {};
    bool operator==(const LightRaw& o) const;
  };
  struct LightSeen {
    LightRaw raw;
    uint32_t draws = 0;
  };
  static constexpr size_t kLightSlots = 8;

  // One resolve of the main view's depth, for the census.
  struct DepthResolveNote {
    uint32_t caller = 0;
    uint32_t flags = 0;
    EdramKey source;
    uint32_t dest_address = 0;
    uint32_t dest_format = 0;
    uint32_t dest_width = 0;
    uint32_t dest_height = 0;
    int32_t rect[4] = {};
    uint32_t host_sample = 0;
    uint32_t host_samples = 0;
    bool inside_end_tiling = false;
    bool clears_depth = false;
    uint32_t refused = 0;
    uint32_t draws_before = 0;
    bool anchor_before = false;
  };

  // The trigger test of AO-DESIGN.md 2.2: 0 = this resolve is the trigger.
  uint32_t Judge(const ResolveRecord& record, const ResolveCache::Summary& summary) const;
  // The light constants most main view draws carried before the trigger (the last one's when none
  // repeats); false when no draw has been seen.
  bool PickLight(LightRaw* out, uint32_t* draws_with_it) const;
  static void DecodeLight(const LightRaw& raw, float ambient[3], float sun[3], float dir[3]);
  void Census();

  // Settings.
  uint32_t census_every_ = 0;

  // The frame.
  uint64_t frame_ = 0;
  bool anchor_seen_ = false;
  bool composite_seen_ = false;
  bool play_view_ = false;            // the frame had main view draws at the play samples
  uint32_t qualifying_resolves_ = 0;  // resolves that passed every test but "the first"
  uint32_t draws_before_ = 0;         // main view draws before the first qualifying resolve
  uint32_t color_draws_before_ = 0;
  uint32_t draws_after_ = 0;          // ... after it, before the anchor
  uint32_t depth_writes_after_ = 0;   // ... of them, drawing into the main depth with depth writes
  uint32_t draws_at_anchor_ = 0;      // ... after the anchor (the UI's depth-only draw, 1x)
  uint32_t view4x_draws_ = 0;
  std::array<LightSeen, kLightSlots> lights_{};
  uint32_t light_count_ = 0;
  uint32_t light_overflow_ = 0;       // draws with a value past the table's room
  uint32_t light_draws_ = 0;
  LightRaw first_light_;
  LightRaw last_light_;
  float depth_clear_z_ = -1.0f;
  uint32_t depth_clears_ = 0;
  std::vector<DepthResolveNote> depth_resolves_;
  uint32_t other_resolves_ = 0;

  // The camera: this frame's (read at the anchor) and the frame before's.
  temporal::Camera camera_;
  bool have_camera_ = false;
  temporal::Camera previous_;
  bool have_previous_ = false;
  uint64_t previous_frame_ = 0;
  temporal::Reject reject_ = temporal::Reject::kNone;
  uint32_t rejects_logged_ = 0;
  // The previous frame's lens against this frame's (CHECK 7): relative change of the vertical field
  // of view, the aspect, near and far, then the distance the camera moved and the cosine between
  // the two forward vectors.
  bool lens_delta_valid_ = false;
  double lens_delta_[6] = {};

  // The census.
  uint32_t census_frames_logged_ = 0;
  uint32_t census_anomalies_logged_ = 0;
  bool frame_had_main_view_ = false;
};

}  // namespace mcla::gpu
