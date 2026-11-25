#pragma once
// ===========================================================================
// MCLA GPU renderer -- anisotropic filtering: what the setting says and which samplers take it (pure rules)
// ===========================================================================
// mcla_gpu_anisotropy is live: Samplers::FrameSettingChanged reads it once a frame, and a change makes the scene forget the
// sampler slots it memoized, so the next draw of every texture asks Samplers::Get again, which makes a sampler for the new
// level (the description's key holds the level the sampler takes, so the old samplers are not touched). What this holds, and
// nothing else (no renderer, no ReXGlue: aniso_rules_test.cpp checks it alone):
//
//   ParseText   the setting's word as a level: `game` (or anything that is not a level) is kGame, off / 1 / 1x is off, 2, 4, 8
//               and 16 (with or without a trailing x, any case, any spaces) are those levels.
//   Eligible    which of the game's samplers the setting is allowed to change: only a trilinear sampler with a mip chain to
//               filter between. A sampler the game made point-sampled, nearest-mip or base-map-only stays as it is: forcing
//               anisotropy on it would turn a deliberate nearest-neighbour or a HUD's base-map lookup into a blur.
//   Effective   the level a sampler gets: the setting's for an eligible sampler, the game's own otherwise.
//   MaxAnisotropy  the host's maximum anisotropy for a level (1, 2, 4, 8, 16).
//
// The levels are the values of rex::xenos::AnisoFilter (the fetch constant's field): samplers.cpp checks them with static_asserts.
// ===========================================================================

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace mcla::gpu::aniso {

// A level, as the fetch constant's field holds it.
inline constexpr int32_t kGame = -1;     // no override: the game's own level
inline constexpr int32_t kDisabled = 0;  // off
inline constexpr int32_t kMax1 = 1;      // the game's "1:1": enabled, at a ratio of 1
inline constexpr int32_t kMax2 = 2;
inline constexpr int32_t kMax4 = 3;
inline constexpr int32_t kMax8 = 4;
inline constexpr int32_t kMax16 = 5;

// The setting's word: kGame for `game` and for any word that is not a level; kDisabled for off, 0, 1 (an anisotropy of 1 is
// none); the levels 2, 4, 8, 16 for those words with an optional trailing x.
inline int32_t ParseText(std::string_view text) {
  std::string v;
  for (unsigned char c : text) {
    if (!std::isspace(c)) v.push_back(char(std::tolower(c)));
  }
  if (!v.empty() && v.back() == 'x') v.pop_back();
  if (v == "off" || v == "0" || v == "1") return kDisabled;
  if (v == "2") return kMax2;
  if (v == "4") return kMax4;
  if (v == "8") return kMax8;
  if (v == "16") return kMax16;
  return kGame;
}

// The word the log prints.
inline const char* Name(int32_t level) {
  switch (level) {
    case kGame: return "game";
    case kDisabled: return "off";
    case kMax2: return "2x";
    case kMax4: return "4x";
    case kMax8: return "8x";
    case kMax16: return "16x";
  }
  return "?";
}

// The sampler the setting may change: a trilinear one (magnification, minification and mip filter all linear) with a mip chain
// to filter between (the sampled range has more than one level) that is not base-map-only.
constexpr bool Eligible(uint32_t mip_min, uint32_t mip_max, bool mip_base_map, bool mag_linear, bool min_linear, bool mip_linear) {
  return mip_max > mip_min && !mip_base_map && mag_linear && min_linear && mip_linear;
}

// The level a sampler is made with: `override_` (a level, or kGame) for an eligible sampler, the game's own `game_level` for any other.
constexpr uint32_t Effective(int32_t override_, bool eligible, uint32_t game_level) {
  return override_ >= 0 && eligible ? uint32_t(override_) : game_level;
}

// The host's maximum anisotropy: 1 for off and for 1:1, then 2, 4, 8, 16.
constexpr uint32_t MaxAnisotropy(uint32_t level) {
  return level == 0 ? 1u : (1u << (level - 1));
}

}  // namespace mcla::gpu::aniso
