#pragma once
// ===========================================================================
// MCLA GPU renderer -- what the temporal system recognises by identity
// ===========================================================================
// One table for the shader pairs and EDRAM targets that the temporal groundwork (F4-DESIGN.md 2.1,
// section 3) and the HDR work name. Every value was read from LARecomp itself, in a census of the
// real game (feature-parity/CENSUS-FINDINGS.md, 34,301 frames, day, dusk, night, a race, the pause
// menu, scene scale 3): the shader identities are LARecomp's own (FNV-1a 64 of the normalised
// microcode, native_gfx/shader_identity.h), not the fork's hash.
//
// A different game build or a changed shader pack gives other values: nothing here fails loudly,
// the frame is simply not temporal (temporal.h says how each case shows in the log). The game's
// MSAA mode is read from the keys (below): 2x, the console's, which the native renderer always runs
// (OFF and the upscalers are fewer host samples, msaa_level.h), or 1x for a game made without MSAA,
// which the native renderer no longer does: the 1x identities stay recognised but are not exercised.
// ===========================================================================

#include <cstddef>
#include <cstdint>

#include "draw_state.h"
#include "edram.h"

namespace mcla::gpu::temporal {

// The game's motion blur draw, the "anchor": once in every 3D frame, between the scene and the
// tone map, also with MOTION BLUR and FULLSCREEN BLUR off (census 1.1, 2).
inline constexpr uint64_t kAnchorVs = 0x95C121D46C9C1B5Full;
inline constexpr uint64_t kAnchorPs = 0x0F681CC854B766D4ull;

// The tone map: one draw a frame, a rectangle list into the UI target, reading the scene colour
// the anchor read. Not used by the temporal groundwork; the HDR work (B) reads the same pair.
inline constexpr uint64_t kToneMapVs = 0xF097B3E9236CB05Aull;
inline constexpr uint64_t kToneMapPs = 0xBC5F0EED0A746DEAull;

// The main view, the 3D scene: colour k_2_10_10_10_FLOAT at EDRAM tile 1024 and D24FS8 depth at
// tile 0, both at the pitch of the screen, 2x samples in play, 1x when the game runs without MSAA
// (census round 2, cen2-drive-msaaoff: the same shaders, fetches, target and viewport; the native
// renderer does not start a game that way any more, msaa_level.h). The pause
// menu, the race pause and the map draw the world into the same keys at 4x samples, also without
// MSAA (round 2: "scene 4x: 18"): the samples tell the play view from the pause view (View, below).
// Both are the main view for the temporal system and the scene's scale (EdramTargets::IsSceneKey), but
// each has a run of its own: the camera, the cut state and the history of one never meet the other's.
// The pause view's UI pass draws into a 4x k_8_8_8_8 colour target, which is no float colour, so none
// of its draws is the main view's; it may draw with the same 4x depth key, which is then told from the
// world's by EdramTargets::DepthLayer as the 1x one is. Other D24FS8 and
// float-colour targets (the reflections, the cascades) have other pitches or bases. The UI phase
// draws into k_8_8_8_8@720 and a 1x depth at the same base and pitch (census 3.4): the colour is
// no float, so its draws are not the main view's; but the UI's depth-only draw (one a frame, the
// stencil mask of the rounded panels) lands in the main depth key at 1x, and only the order
// separates it: it comes after the anchor, which the temporal system checks (temporal.cpp
// OnMainView), and a frame of no 3D scene may hold it alone, so depth-only draws never say that
// a frame has a 3D scene (temporal.cpp, Colored).
// CHECK: the pause menu and the map at 4x with the game's MSAA off (round 2 has the 18-frame 4x block
// after the start press, not a pause menu), and that no other pass draws float colour at tile 1024
// or D24FS8 at tile 0 and pitch 1280 at 1x before the anchor (round 2's counts say only the UI's
// depth-only draw does, after it).
// CHECK: that the pause view's UI pass draws with the 4x depth key or none (the log line "[gpu]
// edram: ... made at" for D24FS8@0 pitch 1280 4x with layer 1 says it shares it).
inline constexpr uint16_t kMainPitch = 1280;
inline constexpr uint16_t kMainColorBase = 1024;
inline constexpr uint16_t kMainDepthBase = 0;
inline constexpr uint32_t kPlaySamples = 2;
inline constexpr uint32_t kPlaySamplesNoMsaa = 1;
inline constexpr uint32_t kPauseSamples = 4;

// Samples the play view has (the game's MSAA mode), as against the pause view's.
inline bool IsPlaySamples(uint32_t samples) {
  return samples == kPlaySamples || samples == kPlaySamplesNoMsaa;
}
inline bool IsMainSamples(uint32_t samples) {
  return IsPlaySamples(samples) || samples == kPauseSamples;
}

// The two kinds of main view: the 3D scene in play, and the world drawn behind the pause menu, the
// race pause and the GPS map. The temporal system keeps one run (camera, cut state, history) at a
// time and starts it again when the kind changes (temporal.h, "Views").
enum class View : uint8_t { kPlay, kPause, kCount };
inline constexpr size_t kViewCount = size_t(View::kCount);
// The view a main view draw of `samples` guest samples (1, 2 or 4) belongs to.
inline View ViewOfSamples(uint32_t samples) {
  return samples == kPauseSamples ? View::kPause : View::kPlay;
}
inline const char* ViewName(View view) {
  return view == View::kPause ? "pause/map view" : "game view";
}

// Guest samples (1, 2 or 4) when `key` is the main view's colour, else 0.
inline uint32_t MainColorSamples(const EdramKey& key) {
  const bool same = key.depth == 0 && key.base_tiles == kMainColorBase && key.pitch == kMainPitch &&
                    key.format == uint8_t(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
  return same && IsMainSamples(key.samples) ? key.samples : 0u;
}

// Guest samples (1, 2 or 4) when `key` is the main view's depth, else 0.
inline uint32_t MainDepthSamples(const EdramKey& key) {
  const bool same = key.depth == 1 && key.base_tiles == kMainDepthBase && key.pitch == kMainPitch &&
                    key.format == uint8_t(xe::DepthRenderTargetFormat::kD24FS8);
  return same && IsMainSamples(key.samples) ? key.samples : 0u;
}

// What a draw's targets say of the main view. A draw is the main view's when its colour 0 and
// depth (each only when it has one) are its identities, at the same samples.
struct MainMatch {
  uint32_t samples = 0;  // 1, 2 or 4; 0: not the main view
  bool color = false;    // it has a colour target
  bool depth = false;    // it has a depth target
  explicit operator bool() const { return samples != 0; }
};

inline MainMatch MatchMainView(const EdramKey* color0, const EdramKey* depth) {
  if (!color0 && !depth) {
    return {};
  }
  const uint32_t color_samples = color0 ? MainColorSamples(*color0) : 0u;
  const uint32_t depth_samples = depth ? MainDepthSamples(*depth) : 0u;
  if ((color0 && color_samples == 0) || (depth && depth_samples == 0)) {
    return {};
  }
  if (color0 && depth && color_samples != depth_samples) {
    return {};
  }
  MainMatch m;
  m.samples = color0 ? color_samples : depth_samples;
  m.color = color0 != nullptr;
  m.depth = depth != nullptr;
  return m;
}

}  // namespace mcla::gpu::temporal
