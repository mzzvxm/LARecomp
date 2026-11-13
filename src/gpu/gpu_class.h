#pragma once
// ===========================================================================
// MCLA GPU renderer -- the GPU class and the `auto` value of the graphics upgrades
// ===========================================================================
// The graphics upgrades (sharper shadows and reflections, contact shadows, sun shafts, bounce
// light, ...) are live settings whose default is `auto`: the renderer picks the value from how
// strong the GPU is. This file is that rule, in two parts.
//
// The class of the GPU, found once when the device opens (Initialize, called from
// Device::Open next to the "[gpu] ... uma=" line) and logged with the reason:
//   kLow   a GPU that shares memory with the CPU (D3D12's UMA flag; Vulkan's integrated type): the
//          integrated GPU of a desktop CPU, the APU of a handheld such as the Steam Deck, and a
//          software or virtual adapter. Nothing heavy runs there.
//   kMid   a discrete GPU with less dedicated memory than kMidBelowBytes (about 6 GiB).
//   kHigh  everything else.
// mcla_gpu_class (auto | low | mid | high, debug only, live) forces a class for tests.
//
// The `auto` table: for each effect, the value `auto` stands for in each class (kEffects below, the
// table of GFX-UPGRADES-DESIGN.md section 9). An effect's package keeps its own cvar and its own
// values; at the place it reads the cvar it calls
//     std::string mode = gpu_class::Resolve(gpu_class::Effect::kSunShafts, REXCVAR_GET(mcla_gpu_sun_shafts));
// and gets the value to act on: the table's cell for `auto`, the user's own choice unchanged for
// anything else (so `off` and `game` are never overridden). Its pause row shows what `auto` comes
// to, like the low latency row's "AUTO (ON)":
//     case 0: return gpu_class::AutoLabel(gpu_class::Effect::kSunShafts);   // "AUTO (HIGH)"
//
// One setting is not a string table: the city LOD scale (lod_city_scale, a number whose 0 is `auto`),
// with its own rule below (CityLodScaleFor, CityLodLabelFor).
//
// Everything that decides something is a pure inline function here (Classify, Cell, ResolveFor,
// AutoLabelFor, CityLodScaleFor), and this header includes nothing of Plume or ReXGlue:
// gpu_class_test.cpp builds on its own and checks every cell of the table and the class rule.
// Current(), Resolve(), AutoLabel() and the CityLod... pair (gpu_class.cpp) only add the class this
// process found.
//
// The cells are a first guess from the costs in the design file; the main session corrects a cell
// from the measured cost of its package, and the owner tunes the threshold (design question 1).
// ===========================================================================

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace plume {
struct RenderDeviceDescription;
struct RenderDeviceCapabilities;
}  // namespace plume

namespace mcla::gpu::gpu_class {

enum class GpuClass : uint8_t { kLow, kMid, kHigh };

// One entry per graphics upgrade that has an `auto` value. Add an entry here, in kEffects (same
// order) and in gpu_class_test.cpp.
enum class Effect : uint8_t {
  kShadowFilter,          // mcla_gpu_shadow_filter      auto, game, soft, pcss
  kShadowResolution,      // mcla_gpu_shadow_resolution  auto, game, high, ultra
  kEnvResolution,         // mcla_gpu_env_resolution     auto, game, high, ultra
  kReflectionResolution,  // mcla_gpu_reflection_resolution  auto, game, high, ultra
  kContactShadows,        // mcla_gpu_contact_shadows    auto, off, low, high
  kSunShafts,             // mcla_gpu_sun_shafts         auto, off, low, high
  kDawnShadows,           // mcla_gpu_dawn_shadows       auto, off, on
  kSsgi,                  // mcla_gpu_ssgi               auto, off, low, high
};
inline constexpr int kEffectCount = 8;

// What `auto` stands for, per class (the table of GFX-UPGRADES-DESIGN.md section 9).
struct EffectInfo {
  std::string_view cvar;
  std::string_view low, mid, high;
};
inline constexpr EffectInfo kEffects[kEffectCount] = {
    {"mcla_gpu_shadow_filter", "game", "soft", "soft"},
    {"mcla_gpu_shadow_resolution", "game", "high", "high"},
    {"mcla_gpu_env_resolution", "game", "game", "high"},
    {"mcla_gpu_reflection_resolution", "game", "high", "high"},
    {"mcla_gpu_contact_shadows", "off", "low", "high"},
    {"mcla_gpu_sun_shafts", "off", "low", "high"},
    {"mcla_gpu_dawn_shadows", "off", "on", "on"},
    {"mcla_gpu_ssgi", "off", "off", "low"},
};

// A discrete GPU with less dedicated memory than this is kMid. A nominal 6 GB card reports a little
// under 6144 MiB (5.9 to 6.1 GiB in practice) and a 4 GB card 4096 MiB, so the limit is 5.5 GiB: a 6 GB
// card is kHigh, a 4 GB one kMid. A first guess for the owner to tune.
inline constexpr uint64_t kMidBelowBytes = 5632ull << 20;

// --- the pure rules ----------------------------------------------------------------------------

constexpr std::string_view ClassName(GpuClass c) {
  switch (c) {
    case GpuClass::kLow:
      return "low";
    case GpuClass::kMid:
      return "mid";
    default:
      return "high";
  }
}

// "low", "mid" or "high" (exactly); false for anything else, `auto` included.
inline bool ParseClass(std::string_view text, GpuClass* out) {
  if (text == "low") {
    *out = GpuClass::kLow;
    return true;
  }
  if (text == "mid") {
    *out = GpuClass::kMid;
    return true;
  }
  if (text == "high") {
    *out = GpuClass::kHigh;
    return true;
  }
  return false;
}

constexpr const EffectInfo& Info(Effect effect) { return kEffects[static_cast<int>(effect)]; }

// The cvar of an effect, for logs.
constexpr std::string_view CvarName(Effect effect) { return Info(effect).cvar; }

// What `auto` stands for in a class.
constexpr std::string_view Cell(Effect effect, GpuClass c) {
  const EffectInfo& info = Info(effect);
  switch (c) {
    case GpuClass::kLow:
      return info.low;
    case GpuClass::kMid:
      return info.mid;
    default:
      return info.high;
  }
}

// `auto` -> the cell; anything else comes back unchanged.
inline std::string ResolveFor(GpuClass c, Effect effect, std::string_view user_value) {
  if (user_value == "auto") {
    return std::string(Cell(effect, c));
  }
  return std::string(user_value);
}

// Upper case (ASCII), for the pause menu's capitals.
inline std::string Upper(std::string_view text) {
  std::string out(text);
  for (char& ch : out) {
    if (ch >= 'a' && ch <= 'z') {
      ch = static_cast<char>(ch - 'a' + 'A');
    }
  }
  return out;
}

// "AUTO (X)" for any display name X: the helper for a row whose values have names of their own.
inline std::string AutoLabelOf(std::string_view shown) {
  return "AUTO (" + Upper(shown) + ")";
}

// "AUTO (HIGH)": the pause row's label for `auto`, with the cell's value in capitals.
inline std::string AutoLabelFor(GpuClass c, Effect effect) {
  return AutoLabelOf(Cell(effect, c));
}

// --- the city LOD scale -------------------------------------------------------------------------
// lod_city_scale (the pause menu's CITY LOD, the launcher's City LOD) is how far a city sector keeps
// its detailed model, as a multiple of the game's own 300 m. 0 stands for `auto`; any other value is
// the player's own and is used as it is (an older larecomp.toml may hold 0.1, 5 or 10).
// Measured (perf BACKLOG-opt.md, the LOD line): a sector is drawn detailed when it is loaded and nearer
// than 300 m x the scale x a camera factor (up to 1.5), and loading stops at the 600 m streaming
// radius, so 2 already makes every loaded sector detailed and 5 and 10 add nothing. It costs about
// +6% CPU frame time on a fast CPU and +36 MB, so `auto` is 2 where the GPU class is Mid or High and
// the game's own 1 on Low (integrated GPUs, handhelds, software adapters).
// The class is only known once Plume has opened the device (Initialize); with the Xenia renderer
// (debug only) it never is, and until it is known `auto` is the game's own 1 too.
inline constexpr double kCityLodAutoGame = 1.0;
inline constexpr double kCityLodAutoFull = 2.0;

// What `auto` stands for: `known` is whether the class has been found (Known()), `c` the class.
constexpr double CityLodAutoFor(bool known, GpuClass c) {
  return (known && c != GpuClass::kLow) ? kCityLodAutoFull : kCityLodAutoGame;
}

// The scale to apply for the cvar's value: the player's own if it is above 0, else what `auto` comes to.
constexpr double CityLodScaleFor(bool known, GpuClass c, double user_value) {
  return user_value > 0.0 ? user_value : CityLodAutoFor(known, c);
}

// "2X", "0.5X", "1.5X": a scale as the pause row shows it.
inline std::string CityLodScaleText(double scale) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%gX", scale);
  return buf;
}

// The pause row's label for the cvar's value: "AUTO (2X)" for 0 (what `auto` comes to now), else the
// scale itself.
inline std::string CityLodLabelFor(bool known, GpuClass c, double user_value) {
  if (user_value > 0.0) {
    return CityLodScaleText(user_value);
  }
  return AutoLabelOf(CityLodScaleText(CityLodAutoFor(known, c)));
}

// What the class rule looks at, as plain values (gpu_class.cpp fills it from the device).
struct AdapterFacts {
  bool uma = false;         // the GPU shares memory with the CPU (D3D12 UMA; Vulkan integrated with host-visible local memory)
  bool integrated = false;  // the backend names the device type integrated (Vulkan; D3D12 never says)
  bool software = false;    // a virtual or CPU device, or a software rasteriser by name
  uint64_t dedicated_bytes = 0;  // dedicated video memory (the largest device-local heap on Vulkan)
};

struct Verdict {
  GpuClass cls = GpuClass::kHigh;
  std::string why;  // one phrase for the log line
};

inline Verdict Classify(const AdapterFacts& facts) {
  if (facts.software) {
    return {GpuClass::kLow, "a software or virtual adapter"};
  }
  if (facts.uma) {
    return {GpuClass::kLow, "UMA: the GPU shares memory with the CPU (an integrated GPU or an APU)"};
  }
  if (facts.integrated) {
    return {GpuClass::kLow, "the graphics API calls it an integrated GPU"};
  }
  if (facts.dedicated_bytes == 0) {
    return {GpuClass::kLow, "no dedicated video memory is reported"};
  }
  const std::string have = std::to_string(facts.dedicated_bytes >> 20) + " MiB of dedicated memory";
  const std::string limit = std::to_string(kMidBelowBytes >> 20) + " MiB";
  if (facts.dedicated_bytes < kMidBelowBytes) {
    return {GpuClass::kMid, "discrete GPU with " + have + ", below " + limit};
  }
  return {GpuClass::kHigh, "discrete GPU with " + have + ", at least " + limit};
}

// --- this process's class (gpu_class.cpp) ------------------------------------------------------

// Finds the class from the device Plume made, logs the "[gpu] class: ..." line and the values
// `auto` comes to. Device::Open calls it once the device exists (again if a second backend is
// tried; the last call stands). mcla_gpu_class overrides it from then on, live.
void Initialize(const plume::RenderDeviceDescription& description,
                const plume::RenderDeviceCapabilities& caps);

// The class in force: mcla_gpu_class if it is not `auto`, else what Initialize found; kHigh before
// Initialize (nothing is drawn before the device exists). Any thread, cheap.
GpuClass Current();

// Whether Current() is a class this process really found: false until Initialize has run, and for the
// whole run with the Xenia renderer (debug only), which makes its own device and never calls it. A
// setting whose `auto` is not the same on every class acts as on the weakest while this is false, so
// that a player is never given the strong class's cost before it is known. Any thread, cheap.
bool Known();

// ResolveFor / AutoLabelFor for Current(). Any thread.
std::string Resolve(Effect effect, std::string_view user_value);
std::string AutoLabel(Effect effect);

// CityLodScaleFor / CityLodLabelFor for Known() and Current(). Any thread.
double CityLodScale(double user_value);
std::string CityLodLabel(double user_value);

}  // namespace mcla::gpu::gpu_class
