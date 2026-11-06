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
// A different game build, a changed shader pack or another MSAA mode gives other values: nothing
// here fails loudly, the frame is simply not temporal (temporal.h says how each case shows in the
// log).
// ===========================================================================

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
// tile 0, both at the pitch of the screen, 2x samples in play. The pause menu and the map draw
// the world into the same keys at 4x samples. Other D24FS8 and float-colour targets (the
// reflections, the cascades) have other pitches or bases, and the UI phase draws into
// k_8_8_8_8@720 and a 1x depth at the same base and pitch (census 3.4): so the match is exact,
// samples included.
inline constexpr uint16_t kMainPitch = 1280;
inline constexpr uint16_t kMainColorBase = 1024;
inline constexpr uint16_t kMainDepthBase = 0;
inline constexpr uint32_t kPlaySamples = 2;
inline constexpr uint32_t kPauseSamples = 4;

// Guest samples (2 or 4) when `key` is the main view's colour, else 0.
inline uint32_t MainColorSamples(const EdramKey& key) {
  const bool same = key.depth == 0 && key.base_tiles == kMainColorBase && key.pitch == kMainPitch &&
                    key.format == uint8_t(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
  return same && (key.samples == kPlaySamples || key.samples == kPauseSamples) ? key.samples : 0u;
}

// Guest samples (2 or 4) when `key` is the main view's depth, else 0.
inline uint32_t MainDepthSamples(const EdramKey& key) {
  const bool same = key.depth == 1 && key.base_tiles == kMainDepthBase && key.pitch == kMainPitch &&
                    key.format == uint8_t(xe::DepthRenderTargetFormat::kD24FS8);
  return same && (key.samples == kPlaySamples || key.samples == kPauseSamples) ? key.samples : 0u;
}

// What a draw's targets say of the main view. A draw is the main view's when its colour 0 and
// depth (each only when it has one) are its identities, at the same samples.
struct MainMatch {
  uint32_t samples = 0;  // 2 or 4; 0: not the main view
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
