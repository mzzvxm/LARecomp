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
//          software or virtual adapter. Nothing heavy runs there. One named exception: AMD's Strix
//          Halo integrated GPUs (Radeon 8060S, 8050S, 8040S) are kMid (kIntegratedMidTable below).
//   kMid   a discrete GPU with less dedicated memory than kMidBelowBytes (about 6 GiB), or one whose
//          name is in the speed table (kSpeedTable below) as a family at most this fast: GTX 10xx
//          (the 1080 and 1080 Ti too) and 16xx, RX 4xx/5xx, Vega, Arc A3xx and the like, whatever
//          their memory.
//   kHigh  everything else.
// The table can also put a very slow discrete card (GT 1030, RX 550) in kLow; it never raises a class.
// The log line says which rule chose: "by adapter type", "by memory" or "by name: GTX 1060 -> mid".
// mcla_gpu_class (auto | low | mid | high, debug only, live) forces a class for tests; it wins over all
// of the above.
//
// The `auto` table: for each effect, the value `auto` stands for in each class (kEffects below, the
// table of GFX-UPGRADES-DESIGN.md section 9). An effect's package keeps its own cvar and its own
// values; at the place it reads the cvar it calls
//     std::string mode = gpu_class::Resolve(gpu_class::Effect::kSunShafts, setting_text);
// where setting_text is the cvar's text as the render thread may read it (live_text.h: LiveText; the
// pause menu's thread writes the cvar's own string).
// and gets the value to act on: the table's cell for `auto`, the user's own choice unchanged for
// anything else (so `off` and `game` are never overridden). Its pause row shows what `auto` comes
// to, like the low latency row's "AUTO (ON)":
//     case 0: return gpu_class::AutoLabel(gpu_class::Effect::kSunShafts);   // "AUTO (DEFAULT)"
//
// Five settings are not a string table: the numbers whose `auto` is a number too (the city LOD scale, the traffic LOD scale, the traffic
// range and the pedestrian and parked car densities), with their own table and rule below (kScales, ScaleFor, ScaleLabelFor; the city LOD
// keeps its old names CityLodScaleFor and CityLodLabelFor).
//
// The ray traced effects have a second axis, how well the card traces rays (an RTX 3060 and an RTX 5080 are both High): the RT tier (none / weak /
// strong) and the `auto` table of those effects (columns none / weak / strong) are rt_tier.h, which reads this file's class and name rules.
//
// Everything that decides something is a pure inline function here (Classify, Cell, ResolveFor,
// AutoLabelFor, ScaleFor), and this header includes nothing of Plume or ReXGlue:
// gpu_class_test.cpp builds on its own and checks every cell of the table and the class rule.
// Current(), Resolve(), AutoLabel(), ScaleInForce() / ScaleLabel() and the CityLod... pair (gpu_class.cpp)
// only add the class this process found.
//
// The cells are a first guess from the costs in the design file; the main session corrects a cell
// from the measured cost of its package, and the owner tunes the threshold (design question 1).
// The cells hold the words a larecomp.toml, a command line and the pause menu all use: no graphics setting has a level called low
// (owner, 2026-11-14), so two quality levels are high / ultra, three are medium / high / ultra and one is on.
// ===========================================================================

#include <algorithm>
#include <cstddef>
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
  kContactShadows,        // mcla_gpu_contact_shadows    auto, off, high, ultra
  kSunShafts,             // mcla_gpu_sun_shafts         auto, off, light, default, strong, stronger, strongest (shafts_level.h)
  kSsgi,                  // mcla_gpu_ssgi               auto, off, high, ultra
  kTrafficShadows,        // mcla_gpu_traffic_shadows    auto, off, near, all (traffic_shadow.h)
  kCarSelfRefl,           // mcla_gpu_car_selfrefl       auto, off, on (car_selfrefl.h)
  kAmbientOcclusion,      // mcla_gpu_ao                 auto, off, high, ultra
  kHeadlights,            // mcla_gpu_headlights         auto, off, on (headlight_rig.h)
  kLightShadows,          // mcla_gpu_light_shadows      auto, off, medium, high, ultra (local_shadow.h; was mcla_gpu_headlight_shadows)
  kVolumetrics,           // mcla_gpu_volumetrics        auto, off, medium, high, ultra (volumetrics_level.h)
};
inline constexpr int kEffectCount = 13;

// What `auto` stands for, per class (the table of GFX-UPGRADES-DESIGN.md section 9).
struct EffectInfo {
  std::string_view cvar;
  std::string_view low, mid, high;
};
inline constexpr EffectInfo kEffects[kEffectCount] = {
    // Contact hardening on Mid and High (owner, 2026-11-19: auto picks PCSS where the search runs; on Low it stays the game's own, and a hand set
    // pcss there is soft: shadow_filter.h PcssAvailable). Kept only if the cost of pcss against soft on Mid is small (the check plan measures it).
    {"mcla_gpu_shadow_filter", "game", "pcss", "pcss"},
    {"mcla_gpu_shadow_resolution", "game", "high", "high"},
    {"mcla_gpu_env_resolution", "game", "game", "high"},
    {"mcla_gpu_reflection_resolution", "game", "high", "high"},
    {"mcla_gpu_contact_shadows", "off", "high", "ultra"},
    {"mcla_gpu_sun_shafts", "off", "default", "default"},  // a strength, not a quality: DEFAULT where the GPU is not integrated
    // Bounce light: high on the High class only. Measured at half size above 1080 lines (perf/ao-half-size):
    // high (the one that was called low) costs about 0.75 ms and ultra about 1.1 ms at 4K on an RTX 5080. It was off on every
    // class until that cost was known (owner, 2026-11-13); the owner agreed on 2026-11-14 to high on High, off on Mid
    // and Low. This branch waits for the owner's look of the high level.
    {"mcla_gpu_ssgi", "off", "off", "high"},
    // Traffic and AI cars cast into the sun shadow cascades: the nearest few on a discrete GPU (each car adds cascade draws,
    // about 4 x a car's low-LOD draws, on the render thread), none on an integrated one (traffic_shadow.h).
    {"mcla_gpu_traffic_shadows", "off", "near", "near"},
    // The player's car paint reflects its own car (the wing in the trunk lid, a mirror in the door). The cost at 3840x2160 on
    // an RTX 5080 is about 0.1 ms a frame (final cost runs, perf/out/FXCOST-B6.txt: +0.107 ms for the specular occlusion alone,
    // +0.095 ms for the occlusion and the reflection of the car's own colour: the same), so the setting has one level, `on`, and
    // every discrete card gets it; an integrated GPU does without.
    {"mcla_gpu_car_selfrefl", "off", "on", "on"},
    // Ambient occlusion, from the costs in AO-DESIGN.md section 5 (measured 0.09 ms for ultra at 720p on
    // the RTX 5080, estimated 1.0 ms at 4K there): ultra is about 3 ms at 1080p on a GTX 1050-class
    // card (Mid), high about 1 ms, so Mid gets high and keeps a frame at 60 FPS; the Low class (shared
    // memory, 2-CU iGPU: the scene alone is 100+ ms at 2x) gets nothing, as every other effect does.
    {"mcla_gpu_ao", "off", "high", "ultra"},
    // The cars' headlights as real lights at night (headlight_rig.h): one extra term in the lit shaders of the pixels the beams reach (about 40 ALU there,
    // the early-out box keeps every other pixel to a few), so the setting has one level, `on`: every card but the integrated one (Low: the scene alone is
    // 100+ ms there) gets it. The cost is the owner's to measure (perf/HEADLIGHT-LIGHT-RUNPLAN.md), a cell is corrected from it.
    {"mcla_gpu_headlights", "off", "on", "on"},
    // The local lights cast shadows (the headlights now, the street lamps later; local_shadow.h): the main view's opaque draws are drawn again, depth only, into a map of 768 (medium), 1024 (high)
    // or 1280 (ultra) texels a view (HL-SHADOW-QUALITY: a texel stays 1.3 to 1.9 cm in the world at 10 m from the lamps). THREE LEVELS (the three level shape, local_shadow_rules.h kThreeLevelShape, owner 2026-11-20, after the cost runs of perf/out/HL-PER-LAMP.txt): medium
    // shadows the player's car's headlights and the nearest other car's with one view for each car (2 views, 9 MiB); high gives the player's car a view for each HEADLAMP (a pole in front of
    // a car casts a shadow from each of its two lamps) and the nearest other car one view (3 views, 24 MiB); ultra shadows the next nearest car's too and gives all three cars a view for each
    // lamp (6 views of 1280, 75 MiB). Mid gets medium, High gets ultra, shared memory nothing (the scene alone is 100+ ms there). The cost of a view is about 0.02 ms of graphics card time
    // (the size barely counts) and 0.05 to 0.1 ms of render thread time (a pass over the frame's draws), measured on the RTX 5080 (the headlights runs of 2026-11-20); a view for each lamp
    // instead of one for the car adds 0.05 to 0.15 ms of graphics card time and 0.04 to 0.16 ms of render thread time there, and a weaker PC pays several times that, so the lamps' views
    // are for the levels above Mid's cell.
    {"mcla_gpu_light_shadows", "off", "medium", "ultra"},
    // Volumetric light (light beams under the lamps and headlights, fog and sun shafts: volumetrics.h; VOLUMETRICS-SURVEY.md section 4,
    // estimated 0.13 / 0.19 / 0.30 ms at 4K and 0.06 / 0.12 / 0.23 ms at 1080p on an RTX 5080 for medium / high / ultra, about 0.5 ms
    // for medium and 1.0 ms for high at 1080p on a GTX 1050-class card): medium on Mid, high on High, nothing on shared memory. Ultra is
    // a deliberate pick: its cost is not "no visible loss" for the weakest cards of the High class (6 GB GTX 1060-class).
    {"mcla_gpu_volumetrics", "off", "medium", "high"},
};

// A discrete GPU with less dedicated memory than this is kMid. A nominal 6 GB card reports a little
// under 6144 MiB (5.9 to 6.1 GiB in practice) and a 4 GB card 4096 MiB, so the limit is 5.5 GiB: a 6 GB
// card is kHigh, a 4 GB one kMid. A first guess for the owner to tune.
inline constexpr uint64_t kMidBelowBytes = 5632ull << 20;

// The ray quality of the sun shafts is not a setting. The blur marches each ray over the sky image (half the picture, capped at 960x540: shafts_image.h) in this many
// steps, chosen by the class: 64 on High, 32 on Mid (and on Low, where the shafts are off unless the player turns a
// strength on by hand: the cheaper one). The debug cvar mcla_gpu_sun_shafts_steps overrides it (shafts_level.h Steps).
inline constexpr uint32_t kSunShaftStepsMid = 32;
inline constexpr uint32_t kSunShaftStepsHigh = 64;

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

// The ray steps of the sun shafts' blur for a class (see kSunShaftStepsMid).
constexpr uint32_t SunShaftSteps(GpuClass c) {
  return c == GpuClass::kHigh ? kSunShaftStepsHigh : kSunShaftStepsMid;
}

// "AUTO (ULTRA)": the pause row's label for `auto`, with the cell's value in capitals. The stored word is the word the player sees
// (no graphics setting has a level called low: the ambient occlusion, the contact shadows and the bounce light are off / high / ultra,
// the car self-reflections off / on), so the cell is shown as it is.
inline std::string AutoLabelFor(GpuClass c, Effect effect) {
  return AutoLabelOf(Cell(effect, c));
}

// --- the numeric `auto` settings: LOD distances and densities ------------------------------------
// Five settings are numbers, not words, and their `auto` is a number too: how far a city sector keeps its detailed model
// (lod_city_scale), how far a traffic car does (lod_traffic_scale), how far from you traffic is kept (traffic_unspawn_dist) and how
// many pedestrians and parked cars there are (ped_density_scale, parked_car_scale). Each one's own value is used as it is; `auto`
// (0 for the first three, a negative number for the two densities, where 0 is "none") is the table's cell for the class (kScales).
// The pause rows say what `auto` comes to ("AUTO (3X)"), like the other AUTO rows.
//
// What the cells stand for (owner, 2026-11-20: "bump the defaults of all LOD related settings, as high as appropriate for the
// hardware class"; the old defaults were one value for every card: 1x, 250 m, 0.5 and 0.5, and the city LOD 2 / 1):
//   * CITY LOD is the game's 300 m times the scale (a camera factor of up to 1.5 on top), and the city is loaded to 600 m, so 2 shows every
//     loaded sector in detail and more adds nothing (a ceiling). It costs about +6% of the render thread's time on a fast CPU and +36 MB
//     (perf BACKLOG-opt.md, the LOD line), so the class with shared memory keeps the game's own 1.
//   * TRAFFIC LOD is a multiple of the game's distances for a car: at 1x the most detailed model ends at about 40 m, the middle one at about
//     155 m, and a car further out than that has the coarsest model, up to the 500 m where the game stops drawing it (TRAFFICPROBE-LOG.txt:
//     244 cars at LOD 0 under 39 m, 336 at LOD 1 from 40 to 154 m, 11 at LOD 2 from 156 m). At 4K a car at 40 m is more than 200 pixels
//     long, so the step to the middle model shows. 3x puts the first step at 120 m and the second at 465 m, where almost nothing is left to see
//     (3.2x and more only moves the second step past the cull). The cost is render thread time, a draw call for every part of each
//     detailed car (about +0.3 ms for 2x on an RTX 5080 at 189 FPS, perf\out\LOD.txt lod-rtx1x-t2) and almost no graphics card time (0.2 to
//     0.4 ms for 0.5x against 1x on a 2-CU integrated GPU of 39 ms, the fork's measurement).
//   * TRAFFIC RANGE is the radius beyond which a traffic car is removed: the game's is 400 m (cars are drawn to 500 m), the old default
//     250 m. More cars are alive and simulated far away; 400 is the original and the ceiling that matters.
//   * PEDESTRIANS and PARKED CARS are multiples of the game's own density (1 is the original; its pools are 64 pedestrians and 50
//     parked cars, so more than 1 adds little): the ceiling is 1.
// CHECK: these cells are estimates from the measurements named above: the traffic LOD's cost at 3x and the cost of the two densities and the
// traffic range were never measured, and no weak CPU was measured with them. perf\scripts\chain-lod-defaults.ps1 measures every cell against the
// old value per class (render thread, graphics card, memory, hitches, pictures); a cell is corrected from it, here and in gpu_class_test.cpp.
// The class is only known once Plume has opened the device (Initialize); with the Xenia renderer (debug only) it never is, and until it
// is known `auto` is the Low cell (the weakest class's, as for every setting whose auto differs between classes).
inline constexpr double kCityLodAutoGame = 1.0;
inline constexpr double kCityLodAutoFull = 2.0;

enum class Scale : uint8_t {
  kCityLod,        // lod_city_scale         0 = auto   a multiple of the game's 300 m
  kTrafficLod,     // lod_traffic_scale      0 = auto   a multiple of the game's traffic car distances
  kTrafficRange,   // traffic_unspawn_dist   0 = auto   metres (100 to 600 when set by hand)
  kPedDensity,     // ped_density_scale      below 0 = auto   a multiple of the game's density (0 is none)
  kParkedDensity,  // parked_car_scale       below 0 = auto   a multiple of the game's density (0 is none)
};
inline constexpr int kScaleCount = 5;

struct ScaleInfo {
  std::string_view cvar;
  double low, mid, high;  // what `auto` stands for per class
  bool zero_is_auto;      // 0 is `auto` too (no value of the setting is 0); else only a number below 0 is
  char unit;              // the row's suffix: 'X' (a multiple) or 'M' (metres)
};

inline constexpr ScaleInfo kScales[kScaleCount] = {
    {"lod_city_scale", kCityLodAutoGame, kCityLodAutoFull, kCityLodAutoFull, true, 'X'},
    {"lod_traffic_scale", 1.5, 2.0, 3.0, true, 'X'},
    {"traffic_unspawn_dist", 250.0, 300.0, 400.0, true, 'M'},
    {"ped_density_scale", 0.5, 0.75, 1.0, false, 'X'},
    {"parked_car_scale", 0.5, 0.75, 1.0, false, 'X'},
};

// The traffic range is held to the menu's range when it is set by hand (the setting accepts 0 to 600 so that 0 can be `auto`).
inline constexpr double kTrafficRangeMin = 100.0;
inline constexpr double kTrafficRangeMax = 600.0;

constexpr const ScaleInfo& ScaleOf(Scale scale) { return kScales[static_cast<int>(scale)]; }

constexpr std::string_view ScaleCvar(Scale scale) { return ScaleOf(scale).cvar; }

// What `auto` stands for in a class.
constexpr double ScaleCell(Scale scale, GpuClass c) {
  const ScaleInfo& info = ScaleOf(scale);
  switch (c) {
    case GpuClass::kLow:
      return info.low;
    case GpuClass::kMid:
      return info.mid;
    default:
      return info.high;
  }
}

// What `auto` stands for now: `known` is whether the class has been found (Known()), `c` the class; until it is found, the Low cell.
constexpr double ScaleAutoFor(Scale scale, bool known, GpuClass c) {
  return ScaleCell(scale, known ? c : GpuClass::kLow);
}

// Whether the cvar's value is `auto`: below 0 (and 0 where it is no value), and a value that is no number.
constexpr bool IsAutoValue(Scale scale, double user_value) {
  return ScaleOf(scale).zero_is_auto ? !(user_value > 0.0) : !(user_value >= 0.0);
}

// The value to apply for the cvar's value: the player's own, or what `auto` comes to.
constexpr double ScaleFor(Scale scale, bool known, GpuClass c, double user_value) {
  if (IsAutoValue(scale, user_value)) {
    return ScaleAutoFor(scale, known, c);
  }
  if (scale == Scale::kTrafficRange) {
    return std::clamp(user_value, kTrafficRangeMin, kTrafficRangeMax);
  }
  return user_value;
}

// "2X", "0.5X", "1.5X", "400M": a value as the pause row shows it.
inline std::string ScaleText(Scale scale, double value) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%g%c", value, ScaleOf(scale).unit);
  return buf;
}

// The pause row's label for the cvar's value: "AUTO (3X)" for `auto` (what it comes to now), else the value itself.
inline std::string ScaleLabelFor(Scale scale, bool known, GpuClass c, double user_value) {
  if (IsAutoValue(scale, user_value)) {
    return AutoLabelOf(ScaleText(scale, ScaleAutoFor(scale, known, c)));
  }
  return ScaleText(scale, ScaleFor(scale, known, c, user_value));
}

// The city LOD scale is the first of them (the names the pause row and the tests have always used).
constexpr double CityLodAutoFor(bool known, GpuClass c) { return ScaleAutoFor(Scale::kCityLod, known, c); }

// The scale to apply for the cvar's value: the player's own if it is above 0, else what `auto` comes to.
constexpr double CityLodScaleFor(bool known, GpuClass c, double user_value) {
  return ScaleFor(Scale::kCityLod, known, c, user_value);
}

// "2X", "0.5X", "1.5X": a scale as the pause row shows it.
inline std::string CityLodScaleText(double scale) { return ScaleText(Scale::kCityLod, scale); }

// The pause row's label for the cvar's value: "AUTO (2X)" for 0 (what `auto` comes to now), else the scale itself.
inline std::string CityLodLabelFor(bool known, GpuClass c, double user_value) {
  return ScaleLabelFor(Scale::kCityLod, known, c, user_value);
}

// --- speed: the table of GPU names ---------------------------------------------------------------
// The memory rule below cannot see speed (perf\COMPAT-AUDIT.md F4, GFX-COST-AUDIT.md section 14 and R2):
// `kHigh` is any discrete card with 5.5 GiB or more, so an 8 GB RX 580, a GTX 1070 or a 6 GB GTX 1660
// Ti would get the RTX 5080's `auto` row (contact shadows ultra, sun shafts default, ambient occlusion
// ultra). kSpeedTable is a short list of GPU families, matched on the adapter's name, that CAPS the
// class. The rules of the table:
//   * It only lowers a class (a name never raises one), and only where the name is known. A GPU the
//     table does not know keeps the memory rule, so a card missed stays as it is today (the safe
//     failure) and a future GPU needs no entry.
//   * A row applies only to its own vendor (the PCI vendor id Plume reports: 0x10DE NVIDIA, 0x1002
//     AMD, 0x8086 Intel). Plume gives the vendor and not the PCI device id, so the model is read from
//     the name; names vary ("NVIDIA GeForce GTX 1060 6GB", "Radeon RX 580 Series", "AMD Radeon RX 580
//     Series (RADV POLARIS10)", "Intel(R) Arc(TM) A380 Graphics"), so the match is on a normalised
//     copy of the name (NormalizeName) and on the model number after a series word ("gtx", "rx", "a").
//   * Integrated GPUs and APUs are never reached by this table: UMA, the integrated type and the
//     software names decide Low before it (Classify). Their names are not listed, on purpose: "AMD
//     Radeon Graphics" is also what Mesa prints for a new discrete card it has no name for. The one
//     exception is its own table below (kIntegratedMidTable), read only for an integrated GPU.
//   * The tiers are rough public 1080p averages (TechPowerUp's and Tom's Hardware's reviews, pcbuildhelper's
//     index and a FurMark chart were looked at in 2026-11), taken against an RTX 2060 as 100: a card that is
//     the RTX 2060's speed or more keeps its memory class. The low end of High is therefore the RTX 2060 (also the RX 5600 XT and
//     RX 6600, about 105 to 120; Vega 64, about 100 to 105; an Arc A580 about 110). The GTX 1080 and 1080 Ti are
//     the exception to the paper speed (about 100 and 125): Pascal does worse than that on this renderer's
//     compute-heavy effects (the owner's call, 2026-11-14), so they are capped at Mid with the rest of the GTX 10 series.
//     Mid is about 55 to 95 (GTX 1060 6 GB about 65, RX 580 about 70, GTX 1660 Super and RTX 3050
//     about 85, GTX 1070 about 80, RX 5500 XT about 70, RX 6500 XT about 60, Arc A380 about 45).
//     Low is about 35 and under, near the Steam Deck's 1.6 TFLOPs and under the GTX 1050 (GT 1030,
//     MX150 to MX350, RX 550). The numbers are rounded and partly from memory of those sources, so a row is
//     a judgement of the generation, not a measurement of this game; the owner sets the cut-offs.
// The class decides the players' default look, so a row here is the owner's call: the table is data,
// each row carries its reason, and gpu_class_test.cpp checks every row and the names around the cut-offs.

struct SpeedRow {
  uint32_t vendor;          // PCI vendor id the row applies to
  std::string_view word;    // a whole word (or words) the normalised name must contain; "" = none
  std::string_view series;  // the word that precedes the model number ("gtx", "rx"); "" = no number rule
  unsigned lo, hi;          // the model number after `series`, inclusive
  GpuClass cap;             // the highest class the family is allowed (kLow or kMid)
  std::string_view family;  // for the log line
};

inline constexpr uint32_t kVendorAmd = 0x1002;
inline constexpr uint32_t kVendorNvidia = 0x10DE;
inline constexpr uint32_t kVendorIntel = 0x8086;

// A card is capped by the LOWEST cap of the rows its name matches, so rows may overlap (RX 550 is in
// the Polaris row and in the RX 540/550 row). Every row has an example in gpu_class_test.cpp.
inline constexpr SpeedRow kSpeedTable[] = {
    // --- NVIDIA ---
    // GeForce GT (GT 710 to GT 1030) and the lower MX laptop chips (MX110 to MX350, a GT 1030 under another
    // name): about 0.5 to 1 TFLOPs on a 64-bit bus, at best half a GTX 1050 and in the Steam Deck's (Low)
    // range. Their 2 or 4 GB would already be Mid; this says Low.
    {kVendorNvidia, "", "gt", 0, 99999, GpuClass::kLow, "GeForce GT"},
    {kVendorNvidia, "", "mx", 100, 399, GpuClass::kLow, "GeForce MX 1xx to 3xx"},
    // MX450 to MX570 (about GTX 1050 to 1650 Max-Q speed): Mid. They have 2 GB, so Mid by memory too; the row
    // keeps one that reports more from being High.
    {kVendorNvidia, "", "mx", 400, 999, GpuClass::kMid, "GeForce MX 4xx and up"},
    // GTX 4xx to 1080 Ti (Fermi, Kepler, Maxwell, Pascal): a GTX 1060 6 GB is about 65 and a GTX 1070 about
    // 80 against an RTX 2060 at 100, and these chips have no ray tracing or mesh shaders. The GTX 1080 (about
    // 100) and 1080 Ti (about 125) are at or above the RTX 2060 on paper, but Pascal does worse than its paper
    // speed on this renderer's compute-heavy effects, so they are Mid too (the owner's call, 2026-11-14): the
    // cut-off is 1099, which is the end of the GTX 10 series (both cards read as 1080 after "gtx").
    {kVendorNvidia, "", "gtx", 0, 1099, GpuClass::kMid, "GTX 4xx to 1080 Ti (Maxwell, Pascal)"},
    // GTX 1650 to 1660 Ti (Turing without RT cores): a 1660 Ti is about 90, a 1650 about 55: under the
    // RTX 2060 and without the RT/tensor units, so Mid.
    {kVendorNvidia, "", "gtx", 1600, 1699, GpuClass::kMid, "GTX 16xx (Turing without RT)"},
    // RTX 2050 and RTX 3050 (desktop 6 and 8 GB, laptop): about 60 to 85, the speed of a GTX 1660 Super,
    // which is capped above. The rest of RTX 20 and up (2060 and over, RX 5700 and over) is not listed.
    {kVendorNvidia, "", "rtx", 2050, 2050, GpuClass::kMid, "RTX 2050"},
    {kVendorNvidia, "", "rtx", 3050, 3050, GpuClass::kMid, "RTX 3050"},

    // --- AMD ---
    // RX 540 and RX 550 (Polaris 12, 1.2 TFLOPs, 64-bit or 128-bit): the same class as a GT 1030: Low.
    {kVendorAmd, "", "rx", 540, 559, GpuClass::kLow, "RX 540 and RX 550"},
    // RX 460 to RX 590 (Polaris): an RX 580 is about 70 against an RTX 2060 at 100, an 8 GB card with the
    // GTX 1060's speed, so Mid, not High. Includes the RX 560 and the rebadges (RX 590 GME).
    {kVendorAmd, "", "rx", 400, 599, GpuClass::kMid, "RX 4xx and 5xx (Polaris)"},
    // Vega 56 and 64 (Vega 10) and Vega Frontier: about 90 and 100. The name does not always carry the
    // number (RADV: "AMD Radeon RX Vega"), so any Vega is Mid; Vega 64 is the one card this puts a notch low.
    // Vega inside an APU (Vega 3 to 11) is integrated and Low before this row.
    {kVendorAmd, "vega", "", 0, 0, GpuClass::kMid, "Vega"},
    // RX 5300 and RX 5500 (RDNA 1, 4 or 8 GB): an RX 5500 XT is about 70, the speed of an RX 580. The RX 5600
    // (about 105) and RX 5700 (about 120 to 135) are the RDNA 1 cards that stay on the memory rule.
    {kVendorAmd, "", "rx", 5300, 5599, GpuClass::kMid, "RX 5300 and RX 5500 (RDNA 1)"},
    // RX 6300 to RX 6500 XT (Navi 24, 4 GB, an 8 GB variant exists): an RX 6500 XT is about 60 and only a
    // 64-bit bus. The RX 6600 (about 120) and up stay on the memory rule.
    {kVendorAmd, "", "rx", 6300, 6599, GpuClass::kMid, "RX 6300 to RX 6500 XT (Navi 24)"},
    // Radeon R9 (GCN 1 to 3: R9 290, 380, 390 with 4 and 8 GB, Fury): GTX 970-980 speed, at best.
    {kVendorAmd, "r9", "", 0, 0, GpuClass::kMid, "Radeon R9"},

    // --- Intel ---
    // Arc A310 to A380 and the A3xxM laptop chips (about 25 to 45): an A380 has 6 GB, so the memory rule
    // alone would call it High. Arc A580 and A750 (about 110 to 140), A770 and the B-series are not listed.
    {kVendorIntel, "arc", "a", 300, 399, GpuClass::kMid, "Arc A3xx"},
    // Intel's integrated graphics (UHD, HD Graphics, Iris): UMA already says Low; this is the name as a
    // second check, because these names are never a discrete card.
    {kVendorIntel, "uhd", "", 0, 0, GpuClass::kLow, "Intel UHD Graphics"},
    {kVendorIntel, "hd graphics", "", 0, 0, GpuClass::kLow, "Intel HD Graphics"},
    {kVendorIntel, "iris", "", 0, 0, GpuClass::kLow, "Intel Iris"},
};
inline constexpr int kSpeedRows = static_cast<int>(sizeof(kSpeedTable) / sizeof(kSpeedTable[0]));

// The one exception to "every integrated GPU is Low": GPUs that share memory with the CPU but are
// about a discrete card's speed. Read by Classify only for an integrated or UMA adapter (never a
// software one), and it can only say Mid: it is matched like kSpeedTable's rows (vendor-gated, on the
// normalised name), so a name not in it keeps Low. Add a name only for an integrated GPU that is
// measured or documented at a discrete mid-range card's speed; every other integrated GPU stays
// Low (the 2-CU RDNA 2 of the Ryzen 7000/9000 desktop chips, Radeon 780M / 880M / 890M, Intel
// Arc 140V and the like are on the Steam Deck's side of the line, not this one).
inline constexpr SpeedRow kIntegratedMidTable[] = {
    // AMD Strix Halo (Ryzen AI Max): 40, 32 and 16 compute units of RDNA 3.5 on a wide memory bus, about an
    // RTX 4060's speed (Radeon 8060S 40 CUs, 8050S 32 CUs, 8040S 16 CUs). Windows names the GPU "AMD
    // Radeon(TM) 8060S Graphics"; Mesa's RADV may print only "AMD Radeon Graphics (RADV GFX1151)", so the
    // chip's own name (gfx1151: every Strix Halo part) is a row too.
    {kVendorAmd, "8060s", "", 0, 0, GpuClass::kMid, "Radeon 8060S (Strix Halo)"},
    {kVendorAmd, "8050s", "", 0, 0, GpuClass::kMid, "Radeon 8050S (Strix Halo)"},
    {kVendorAmd, "8040s", "", 0, 0, GpuClass::kMid, "Radeon 8040S (Strix Halo)"},
    {kVendorAmd, "gfx1151", "", 0, 0, GpuClass::kMid, "Strix Halo (RADV GFX1151)"},
};
inline constexpr int kIntegratedMidRows =
    static_cast<int>(sizeof(kIntegratedMidTable) / sizeof(kIntegratedMidTable[0]));

// The name as the table reads it: lower case, "(R)" and "(TM)" dropped, everything that is not a letter or
// a digit one space ("NVIDIA GeForce GTX 1060 6GB" -> "nvidia geforce gtx 1060 6gb"; "Intel(R) Arc(TM) A380
// Graphics" -> "intel arc a380 graphics"; "AMD Radeon RX 580 Series (RADV POLARIS10)" -> "amd radeon rx 580
// series radv polaris10").
inline std::string NormalizeName(std::string_view name) {
  const auto at = [&](std::size_t i, std::string_view mark) {
    if (i + mark.size() > name.size()) {
      return false;
    }
    for (std::size_t k = 0; k < mark.size(); ++k) {
      const char c = name[i + k];
      if ((c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) != mark[k]) {
        return false;
      }
    }
    return true;
  };
  std::string out;
  bool gap = false;  // a separator is pending: written before the next letter or digit
  for (std::size_t i = 0; i < name.size();) {
    if (at(i, "(tm)")) {
      i += 4;
      continue;
    }
    if (at(i, "(r)")) {
      i += 3;
      continue;
    }
    char c = name[i++];
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      if (gap && !out.empty()) {
        out += ' ';
      }
      gap = false;
      out += c;
    } else {
      gap = true;  // a space, a dash, a bracket, a byte of a symbol such as the (R) sign
    }
  }
  return out;
}

// What a row found in a name.
struct SpeedMatch {
  int row = -1;      // index into kSpeedTable; -1 = no row applies
  std::string text;  // what matched, upper case: "GTX 1060", "VEGA", "A380" (for the log line)
};

namespace speed_detail {

// Whether `word` occurs in `norm` as whole words.
inline bool HasWord(std::string_view norm, std::string_view word) {
  if (word.empty()) {
    return true;
  }
  const std::string haystack = " " + std::string(norm) + " ";
  const std::string needle = " " + std::string(word) + " ";
  return haystack.find(needle) != std::string::npos;
}

// The leading digits of `text` as a number (0 and false when there are none or they are over 5).
inline bool LeadingNumber(std::string_view text, unsigned* out) {
  unsigned value = 0;
  std::size_t n = 0;
  while (n < text.size() && text[n] >= '0' && text[n] <= '9' && n < 6) {
    value = value * 10 + static_cast<unsigned>(text[n] - '0');
    ++n;
  }
  if (n == 0 || n > 5) {
    return false;
  }
  *out = value;
  return true;
}

// The first model number that follows the word `series` in `norm`: "gtx 1060" and "gtx1060" give 1060, and
// "1060m" or "5600xt" the digits before the letters. The word must start a word ("gtx" is not found in
// "xgtx"), and a longer word is not the series ("gt" is not "gtx").
inline bool SeriesNumber(std::string_view norm, std::string_view series, unsigned* number) {
  std::size_t pos = 0;
  while (pos < norm.size()) {
    std::size_t end = norm.find(' ', pos);
    if (end == std::string_view::npos) {
      end = norm.size();
    }
    const std::string_view token = norm.substr(pos, end - pos);
    if (token.substr(0, series.size()) == series) {
      const std::string_view rest = token.substr(series.size());
      if (rest.empty()) {
        if (end < norm.size()) {  // "gtx" "1060": the number is the next word
          const std::size_t next_end = std::min(norm.find(' ', end + 1), norm.size());
          if (LeadingNumber(norm.substr(end + 1, next_end - end - 1), number)) {
            return true;
          }
        }
      } else if (rest[0] >= '0' && rest[0] <= '9' && LeadingNumber(rest, number)) {
        return true;  // "gtx1060"
      }
    }
    pos = end + 1;
  }
  return false;
}

}  // namespace speed_detail

// The row of `rows` with the LOWEST cap that fits this adapter (the first one on a tie), or row -1. The
// row is an index into `rows`.
inline SpeedMatch MatchRows(const SpeedRow* rows, int count, uint32_t vendor, std::string_view name) {
  SpeedMatch best;
  if (name.empty()) {
    return best;
  }
  const std::string norm = NormalizeName(name);
  for (int i = 0; i < count; ++i) {
    const SpeedRow& row = rows[i];
    if (row.vendor != vendor || !speed_detail::HasWord(norm, row.word)) {
      continue;
    }
    std::string text;
    if (!row.series.empty()) {
      unsigned number = 0;
      if (!speed_detail::SeriesNumber(norm, row.series, &number) || number < row.lo || number > row.hi) {
        continue;
      }
      // "GTX 1060", "RX 580", but "A380": a one-letter series is written without a space
      text = std::string(row.series) + (row.series.size() > 1 ? " " : "") + std::to_string(number);
    } else {
      text = std::string(row.word);
    }
    if (best.row >= 0 && row.cap >= rows[best.row].cap) {
      continue;
    }
    best.row = i;
    best.text = Upper(text);
  }
  return best;
}

// The row of kSpeedTable that caps this adapter, or row -1.
inline SpeedMatch MatchSpeedTable(uint32_t vendor, std::string_view name) {
  return MatchRows(kSpeedTable, kSpeedRows, vendor, name);
}

// The row of kIntegratedMidTable that names this integrated GPU, or row -1.
inline SpeedMatch MatchIntegratedMid(uint32_t vendor, std::string_view name) {
  return MatchRows(kIntegratedMidTable, kIntegratedMidRows, vendor, name);
}

// What the class rule looks at, as plain values (gpu_class.cpp fills it from the device).
//
// The cues that tell a fast discrete card from a slow one of the same memory, and why only the table is in
// the rule (the others stay as notes; perf\COMPAT-AUDIT.md F4):
//   * No ray tracing hardware (caps.raytracing false) would cap a card at kMid: a generation cue with no
//     table, but a driver that reports false on a card that has it would drop a fast card, and RDNA 1 (the
//     fast RX 5700 XT) has none. The flag is only logged ("[gpu] class: ... ray tracing yes/no").
//   * A timing probe at the start would measure the real speed, but an idle card's clocks are low, the first
//     use of a pipeline includes the driver's compile, the class could change from start to start, and the
//     thresholds need the logs of weak cards (BACKLOG O1) that nobody has sent yet.
struct AdapterFacts {
  bool uma = false;         // the GPU shares memory with the CPU (D3D12 UMA; Vulkan integrated with host-visible local memory)
  bool integrated = false;  // the backend names the device type integrated (Vulkan; D3D12 never says)
  bool software = false;    // a virtual or CPU device, or a software rasteriser by name
  uint64_t dedicated_bytes = 0;  // dedicated video memory (the largest device-local heap on Vulkan)
  // Whether the backend reports ray tracing (D3D12 DXR tier 1.0 or more; Vulkan's ray tracing pipeline
  // and acceleration structures). The class rule does not use it: it is in the "[gpu] class:" line.
  bool raytracing = false;
  // The adapter's description as the API gives it, and its PCI vendor id (Plume's RenderDeviceVendor):
  // what the speed table reads. Empty and 0 when unknown, which leaves the memory rule alone.
  std::string_view name;
  uint32_t vendor_id = 0;
};

// Which rule chose the class, for the log line.
enum class Rule : uint8_t {
  kAdapterType,  // software, UMA, integrated, or no dedicated memory: nothing heavy runs
  kMemory,       // the dedicated video memory (a discrete card), no table row lowered it
  kName,         // the speed table lowered the class the memory rule gave, or a named integrated GPU is Mid
};

struct Verdict {
  GpuClass cls = GpuClass::kHigh;
  std::string why;  // one phrase for the log line: starts with "by adapter type:", "by memory:" or "by name:"
  Rule rule = Rule::kMemory;
};

inline Verdict Classify(const AdapterFacts& facts) {
  if (facts.software) {
    return {GpuClass::kLow, "by adapter type: a software or virtual adapter", Rule::kAdapterType};
  }
  if (facts.uma || facts.integrated) {
    // Shared memory is Low, except the few named integrated GPUs that are as fast as a mid-range card
    // (kIntegratedMidTable: AMD's Strix Halo, about an RTX 4060). Every other integrated GPU stays Low.
    const SpeedMatch fast = MatchIntegratedMid(facts.vendor_id, facts.name);
    if (fast.row >= 0) {
      const SpeedRow& row = kIntegratedMidTable[fast.row];
      return {row.cap,
              "by name: " + fast.text + " -> " + std::string(ClassName(row.cap)) + " (" + std::string(row.family) +
                  "; shares memory with the CPU like any integrated GPU, which would be low, but is about a mid-range card's speed)",
              Rule::kName};
    }
    if (facts.uma) {
      return {GpuClass::kLow, "by adapter type: UMA, the GPU shares memory with the CPU (an integrated GPU or an APU)",
              Rule::kAdapterType};
    }
    return {GpuClass::kLow, "by adapter type: the graphics API calls it an integrated GPU", Rule::kAdapterType};
  }
  if (facts.dedicated_bytes == 0) {
    return {GpuClass::kLow, "by adapter type: no dedicated video memory is reported", Rule::kAdapterType};
  }
  const std::string have = std::to_string(facts.dedicated_bytes >> 20) + " MiB of dedicated memory";
  const std::string limit = std::to_string(kMidBelowBytes >> 20) + " MiB";
  Verdict by_memory;
  if (facts.dedicated_bytes < kMidBelowBytes) {
    by_memory = {GpuClass::kMid, "by memory: discrete GPU with " + have + ", below " + limit, Rule::kMemory};
  } else {
    by_memory = {GpuClass::kHigh, "by memory: discrete GPU with " + have + ", at least " + limit, Rule::kMemory};
  }

  // The speed table caps the class the memory gave; it never raises it.
  const SpeedMatch match = MatchSpeedTable(facts.vendor_id, facts.name);
  if (match.row < 0) {
    return by_memory;
  }
  const SpeedRow& row = kSpeedTable[match.row];
  if (row.cap >= by_memory.cls) {
    return by_memory;  // the memory already says as little (or less): the name changes nothing
  }
  return {row.cap,
          "by name: " + match.text + " -> " + std::string(ClassName(row.cap)) + " (" + std::string(row.family) +
              "; the memory alone says " + std::string(ClassName(by_memory.cls)) + ": " + have + ")",
          Rule::kName};
}

// --- this process's class (gpu_class.cpp) ------------------------------------------------------

// What Initialize reads from the device, as the rule's plain values (the software names, the type, the memory, the ray tracing flag, the name and the
// vendor id). `name` points into `description`, which must outlive the result. rt_tier.cpp reads the same facts.
AdapterFacts FactsFor(const plume::RenderDeviceDescription& description,
                      const plume::RenderDeviceCapabilities& caps);

// Finds the class from the device Plume made, logs the "[gpu] class: ..." line and the values
// `auto` comes to. Device::Open calls it once the device exists (again if a second backend is
// tried; the last call stands). mcla_gpu_class overrides it from then on, live.
void Initialize(const plume::RenderDeviceDescription& description,
                const plume::RenderDeviceCapabilities& caps);

// The class in force: mcla_gpu_class if it is not `auto`, else what Initialize found; kHigh before
// Initialize (nothing is drawn before the device exists). Any thread, cheap.
GpuClass Current();

// Whether mcla_gpu_class forces the class (Current() is then the forced one, not what the adapter said), and the class the adapter alone said
// (kHigh before Initialize). The ray tracing tier follows a forced class in a test (rt_tier.h). Any thread, cheap.
bool ClassForced();
GpuClass ClassDetected();

// Whether Current() is a class this process really found: false until Initialize has run, and for the
// whole run with the Xenia renderer (debug only), which makes its own device and never calls it. A
// setting whose `auto` is not the same on every class acts as on the weakest while this is false, so
// that a player is never given the strong class's cost before it is known. Any thread, cheap.
bool Known();

// ResolveFor / AutoLabelFor for Current(). Any thread.
std::string Resolve(Effect effect, std::string_view user_value);
std::string AutoLabel(Effect effect);

// ScaleFor / ScaleLabelFor for Known() and Current(): the value to apply for a numeric setting's value, and the pause row's label
// for it. Any thread, cheap (the hooks call ScaleInForce on every traffic car of every frame).
double ScaleInForce(Scale scale, double user_value);
std::string ScaleLabel(Scale scale, double user_value);

// CityLodScaleFor / CityLodLabelFor for Known() and Current(). Any thread.
double CityLodScale(double user_value);
std::string CityLodLabel(double user_value);

}  // namespace mcla::gpu::gpu_class
