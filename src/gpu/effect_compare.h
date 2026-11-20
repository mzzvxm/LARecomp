#pragma once
// ===========================================================================
// MCLA GPU renderer -- the compare keys: which effect they flip, and the flip itself
// ===========================================================================
// A graphics effect (the bounce light, the contact shadows ...) is hard to judge from memory. The compare
// keys (mc_engine/display/effect_compare.h, F1 and F12 until they are rebound) flip ONE chosen effect off and on again at the
// same spot, so the difference can be seen. They are a DEVELOPER TOOL: a build has them only with LARECOMP_DEV_TOOLS
// (CMake option -DLARECOMP_DEV_TOOLS=ON, off by default). Without it nothing flips an effect: State below is
// the inert one, whose answers are constants, so the readers that ask it compile to the plain setting. This header is the
// shared, light half that the settings' readers include: the list of effects, and the flip as the readers see it.
//
// The flip is not a setting. It never writes a cvar, so the saved setting stays what it was (the pause
// menu's row, larecomp.toml, the launcher) and nothing needs to be put back; the readers ask here instead:
//   LiveText::Get() (live_text.h)      the string settings of the occlusion, the contact shadows, the sun
//                                      shafts, the bounce light and the car self-reflections
//   shadow_filter::CurrentMode()       the sun shadow filter
//   WantedFeatures() (pipelines)  the bloom size fix
// and take `off` (or the text of an on value) while the flip says so. The flip is dropped as soon as the
// setting it flips is changed (by the pause menu, F4 or the console), or another effect is selected.
//
// The rules that need the settings' texts (what is on now, what a press does, the label) are in
// effect_compare_rules.h, which effect_compare_test.cpp checks without a renderer.
// ===========================================================================

#include <atomic>
#include <cstdint>
#include <string_view>

namespace mcla::gpu::compare {

// The effects the key can flip. Add one here, in kEffects (same order), in the readers above and in
// effect_compare_test.cpp.
enum class Effect : uint8_t {
  kBounceLight,
  kContactShadows,
  kSunShafts,
  kAmbientOcclusion,
  kShadowFilter,
  kCarSelfRefl,
  kBloomFix,
  kNone = 0xFF,
};
inline constexpr int kEffectCount = 7;

// What the flip says about an effect: nothing, forced off, or forced on.
enum class Force : uint8_t { kNone, kOff, kOn };

struct EffectInfo {
  std::string_view choice;   // the value of mcla_compare_effect
  std::string_view cvar;     // the setting it flips
  std::string_view label;    // the words of the label and the pause row (capitals)
  std::string_view on_text;  // the text a string setting is read as while the flip forces it on; "" for a switch
};

// `on_text` is `ultra` for the graphics upgrades, which every owner reads as its strongest level (`on` for the car
// self-reflections, whose one level it is), `default` for
// the sun shafts (their setting is a strength, shafts_level.h: the forced-on shafts are at the default
// strength; the old `high` would read as that too, but as a legacy value), and `soft` for the shadow filter.
inline constexpr EffectInfo kEffects[kEffectCount] = {
    {"bounce", "mcla_gpu_ssgi", "BOUNCE LIGHT", "ultra"},
    {"contact", "mcla_gpu_contact_shadows", "CONTACT SHADOWS", "ultra"},
    {"shafts", "mcla_gpu_sun_shafts", "SUN SHAFTS", "default"},
    {"occlusion", "mcla_gpu_ao", "AMBIENT OCCLUSION", "ultra"},
    {"filter", "mcla_gpu_shadow_filter", "SHADOW FILTER", "soft"},
    {"selfrefl", "mcla_gpu_car_selfrefl", "SELF-REFLECTIONS", "on"},
    {"bloom", "mcla_gpu_bloom_scale", "BLOOM FIX", ""},
};

constexpr const EffectInfo& Info(Effect effect) {
  return kEffects[static_cast<int>(effect)];
}

// The effect a value of mcla_compare_effect names; the bounce light (the default) for anything else.
constexpr Effect ParseEffect(std::string_view choice) {
  for (int i = 0; i < kEffectCount; ++i) {
    if (kEffects[i].choice == choice) {
      return static_cast<Effect>(i);
    }
  }
  return Effect::kBounceLight;
}

// The effect whose setting is `cvar`; kNone for any other setting (nearly all of them).
constexpr Effect EffectOfCvar(std::string_view cvar) {
  for (int i = 0; i < kEffectCount; ++i) {
    if (kEffects[i].cvar == cvar) {
      return static_cast<Effect>(i);
    }
  }
  return Effect::kNone;
}

#if defined(LARECOMP_DEV_TOOLS)

// The one flip there is: one chosen effect, forced one way. Any thread: the key's thread writes it, the
// render thread reads it.
class State {
 public:
  Force ForcedFor(Effect effect) const {
    if (effect == Effect::kNone) {
      return Force::kNone;
    }
    const uint16_t word = word_.load(std::memory_order_acquire);
    return static_cast<Effect>(word >> 8) == effect ? static_cast<Force>(word & 0xFF) : Force::kNone;
  }
  // The effect the flip is on, kNone when there is none.
  Effect Flipped() const {
    const uint16_t word = word_.load(std::memory_order_acquire);
    return (word & 0xFF) == 0 ? Effect::kNone : static_cast<Effect>(word >> 8);
  }
  void Set(Effect effect, Force force) {
    const bool none = effect == Effect::kNone || force == Force::kNone;
    word_.store(none ? uint16_t(0) : uint16_t((uint16_t(effect) << 8) | uint16_t(force)),
                std::memory_order_release);
  }
  void Clear() { Set(Effect::kNone, Force::kNone); }

  // A switch (a bool setting) as the flip makes it.
  bool Apply(Effect effect, bool saved) const {
    switch (ForcedFor(effect)) {
      case Force::kOff: return false;
      case Force::kOn: return true;
      default: return saved;
    }
  }

 private:
  std::atomic<uint16_t> word_{0};  // effect << 8 | force; 0 is no flip
};

#else  // no developer tools: nothing flips an effect

// The same questions with the same answers a State that was never set gives, as constants: the settings' readers
// get the saved setting and the compiler drops the question.
class State {
 public:
  constexpr Force ForcedFor(Effect) const { return Force::kNone; }
  constexpr Effect Flipped() const { return Effect::kNone; }
  constexpr void Set(Effect, Force) const {}
  constexpr void Clear() const {}
  constexpr bool Apply(Effect, bool saved) const { return saved; }
};

#endif

// The one flip of the process.
inline State& Global() {
  static State state;
  return state;
}

}  // namespace mcla::gpu::compare
