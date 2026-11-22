#pragma once
// ===========================================================================
// MCLA GPU renderer -- ray traced VISIBILITY of the top K lights: the rules (perf/RT-DIRECT-SURVEY.md sections 4 O1 + O2, 5 and 10 D1; RT-LIGHTS-D1 part 1)
// ===========================================================================
// WHAT IT IS. The deferred light pass (local_lights.h) multiplies a light's term by the visibility the shadow atlas gives it (a raster replay of the main view's draws). This adds the second
// provider the light frame's `ShadowKind::kRtMask` reserved a place for: ONE ray from every pixel to every light of the top K, against the top level (the city's structures and the cars), traced
// at the occlusion's size into a mask image (three lights an image: the filter's alpha is its history length), filtered by RtDenoise (our temporal + a-trous), read by the light pass. This
// header is everything of it that needs no renderer and no GPU (rt_lights_test.cpp runs it): the settings' words, which lights get a ray and in which channel, the rows the trace reads, the table
// the light pass reads, and the rule that composes the visibility with the light.
//
// THE COMPOSITION (survey section 5; the identity is what makes it safe to leave in):
//   a light WE own (a headlight, neon, a police or mod lamp)         adds   term x V                              V = V_atlas x V_rt
//   a light the GAME lights (a street lamp: its grid already added it)  adds  -term x (1 - V)                    NEVER a positive term: with V = 1 the pixel is the game's, bit for bit
//   V_atlas is 1 for a light with no atlas views (RT off, or the light is not in the atlas's pick) and V_rt is 1 for a light with no channel (RT off, or the light is not in the top K): so
//   the atlas stays the provider where the RT is not there, and the product (not a replacement) is what two partial caster sets need (the structures hold the city's shells and the nearest cars; the
//   atlas holds what the main view drew: poles, signs, foliage). Both seeing the same occluder only darkens a penumbra twice (CHECK C8). `Combine`, `SubtractShare` and `Apply` below are the rule;
//   local_lights_rules.h PixelLightRt and hlsl/local_light_ps.hlsl are its pixel paths and local_lights_test / rt_lights_test hold the two to this.
//
// THE TOP K. Candidates are the lights the pass draws this frame (headlights, neon, the cars' lamps, and the street lamps that the atlas picked or that this class picked for itself); a light is
// eligible when it has the RT role (light_frame.h kRoleMaskRt: the survey's "no ranking of its own: the first K of the shadow ranking"), a range and a colour. The ranking is CLASS first (a
// headlight, then neon, then a street lamp, then any other: the survey's "headlights, neon if present, the nearest lamps"), then by what the light adds to the view (Score: its colour over its
// distance squared plus its plateau), a light that held a channel last frame scoring KeepBonus more (the picker's hysteresis: a channel that changes owner loses its history). A light keeps its
// channel while it is in the pick (AssignChannels); the group of three channels whose owners changed starts its history again.
//
// THE CHANNELS. A channel number is `image x 3 + component` (rgb of an RGBA16F image; the alpha is RtDenoise's history length), so K lights fill ceil(K / 3) images: 1, 2 or 3 (kMaxLights 9). The
// number rides in ShadowRef::mask_channel (GpuLight packed1 bits 24-31) and the light's flag kLightFlagRtVisibility says the light reads it: the atlas's kind and views of the same light are untouched.
// CHECK (C15, memory): three lights per image means RtDenoise's four images (a ping-pong pair and a history pair) per group; a 4 channel visibility consumer or R8 images would halve it. Part 2's
// first measurement is the cost and the bytes of K = 3 at 1080 and 4K; HeldBytes() is what vram_budget.h asks.
//
// Pure: no renderer, no guest, no SDK. Namespace larecomp::gpu::rt_lights.
// ===========================================================================

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

#include "light_frame.h"
#include "light_id.h"

namespace larecomp::gpu::rt_lights {

// ---------------------------------------------------------------------------
// the numbers (hlsl/rt_lights_common.hlsli and hlsl/rt_mask_read.hlsli have the twins: RTL_*, RTM_*)
// ---------------------------------------------------------------------------

inline constexpr uint32_t kChannelsPerImage = 3;
inline constexpr uint32_t kMaxImages = 3;
inline constexpr uint32_t kMaxLights = kChannelsPerImage * kMaxImages;   // 9: K at most
inline constexpr uint32_t kNoChannel = 0xFFu;
inline constexpr uint32_t kDefaultK = 3;                                  // one image
inline constexpr uint32_t kUltraK = 6;                                    // two images
inline constexpr uint32_t kMaxRays = 4;
inline constexpr float kKeepBonus = 1.25f;                                // a light that holds a channel scores this much more than a rival that does not
inline constexpr float kMinNdotL = 0.02f;                                 // a surface that faces the light by less takes none of it: no ray (the term is 0 there)
inline constexpr float kMinAbove = 0.08f;                                 // a ray is kept this far (cosine) above the surface's tangent plane
inline constexpr float kLiftFloor = 0.006f;                               // the ray's origin is lifted along the normal by this + three pixels' footprint, at most kLiftCap
inline constexpr float kLiftPixels = 3.0f;
inline constexpr float kLiftCap = 0.4f;
inline constexpr float kMaxRangeMetres = 700.0f;                          // a pixel farther than this is not judged (the structures are the far level of detail there)
inline constexpr float kDepthSigma = 0.05f;                               // the mask's upsample: a traced texel counts as the pixel's when its depth is within this share of the pixel's
inline constexpr float kRoughness = 1.0f;                                 // the denoiser's guide: diffuse
// The disc radius of a light by class (metres; mcla_gpu_rt_lights_radius overrides it) and how far short of the light a ray ends (the lamp's own head and arm are in the structures:
// CHECK C10): a headlight's lens is 14 cm across, a street lamp's head 30 to 50 cm, a neon tube is longer than it is thick.
inline constexpr float kRadiusHeadlight = 0.07f, kRadiusNeon = 0.45f, kRadiusLamp = 0.25f, kRadiusOther = 0.12f;
inline constexpr float kStopHeadlight = 0.35f, kStopNeon = 0.15f, kStopLamp = 0.50f, kStopOther = 0.20f;
inline constexpr float kMaxRadius = 2.0f;

// ---------------------------------------------------------------------------
// the channels
// ---------------------------------------------------------------------------

constexpr uint32_t ImageOf(uint32_t channel) { return channel / kChannelsPerImage; }
constexpr uint32_t ComponentOf(uint32_t channel) { return channel % kChannelsPerImage; }
constexpr uint32_t ChannelOf(uint32_t image, uint32_t component) { return image * kChannelsPerImage + component; }
constexpr bool ValidChannel(uint32_t channel) { return channel < kMaxLights; }
// How many images K lights fill.
constexpr uint32_t ImagesFor(uint32_t lights) { return (std::min(lights, kMaxLights) + kChannelsPerImage - 1) / kChannelsPerImage; }
// The group (image) bit of a channel, for the mask of groups whose history starts again.
constexpr uint32_t GroupBit(uint32_t channel) { return 1u << ImageOf(channel); }
static_assert(kMaxLights == 9 && ImageOf(8) == 2 && ComponentOf(8) == 2 && ChannelOf(2, 2) == 8 && ImagesFor(1) == 1 && ImagesFor(3) == 1 && ImagesFor(4) == 2 && ImagesFor(9) == 3);

// ---------------------------------------------------------------------------
// the settings' words
// ---------------------------------------------------------------------------

// mcla_gpu_rt_lights: "auto" (the default of the final design: on where the RT SHADOWS row's level is on), "off", "on" (the debug switch: the pass runs whatever that row says, where the device traces).
enum class Mode : uint8_t { kOff = 0, kAuto = 1, kOn = 2 };

inline Mode ParseMode(std::string_view text) {
  std::string t(text);
  std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r' || t.back() == '\n')) {
    t.pop_back();
  }
  while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) {
    t.erase(t.begin());
  }
  if (t == "off" || t == "0" || t == "false" || t == "no" || t == "none") {
    return Mode::kOff;
  }
  if (t == "on" || t == "1" || t == "true" || t == "yes" || t == "force") {
    return Mode::kOn;
  }
  return Mode::kAuto;  // "auto", "", "-1", anything else: follow the RT SHADOWS row
}
constexpr const char* ModeName(Mode m) { return m == Mode::kOff ? "off" : (m == Mode::kOn ? "on" : "auto"); }

// Whether the setting wants the pass this frame: `rt_shadows_on` = the RT SHADOWS row's level (gpu class and its own word) is not off.
constexpr bool Wanted(Mode mode, bool rt_shadows_on) { return mode == Mode::kOn || (mode == Mode::kAuto && rt_shadows_on); }

// K: the setting (0 = the level's number) clamped to the channels; the level's number is one image (3) and, at the largest light shadows level, two (6).
constexpr uint32_t AutoK(bool ultra) { return ultra ? kUltraK : kDefaultK; }
constexpr uint32_t ClampK(int setting, bool ultra) {
  return setting <= 0 ? AutoK(ultra) : std::min<uint32_t>(uint32_t(setting), kMaxLights);
}
constexpr uint32_t ClampRays(int setting) { return uint32_t(std::clamp(setting, 1, int(kMaxRays))); }
// mcla_gpu_rt_lights_view: 0 off, 1 the filtered visibility as a picture (the light pass draws it for the lights that have a channel), 2 the raw (unfiltered) one.
inline constexpr int kViewOff = 0, kViewFiltered = 1, kViewRaw = 2;
constexpr int ClampView(int setting) { return std::clamp(setting, kViewOff, kViewRaw); }
// mcla_gpu_rt_lights_check: the gate's identity test. 1: the pass runs (and costs) but every visibility is 1 (the picture must equal the pass off, bit for bit); 2: every visibility is 0 (the
// lights with a channel vanish, a game's lamp is taken away in full).
inline constexpr int kCheckOff = 0, kCheckVisible = 1, kCheckBlocked = 2;
constexpr int ClampCheck(int setting) { return std::clamp(setting, kCheckOff, kCheckBlocked); }

// ---------------------------------------------------------------------------
// which lights
// ---------------------------------------------------------------------------

// The survey's order: a headlight, then neon, then a street lamp, then anything else (police, rear lamps, mods).
enum class Class : uint8_t { kHeadlight = 0, kNeon = 1, kLamp = 2, kOther = 3 };
constexpr Class ClassOf(const Light& l) {
  switch (l.source) {
    case LightSource::kHeadlight: return Class::kHeadlight;
    case LightSource::kNeon: return Class::kNeon;
    case LightSource::kLamp: return Class::kLamp;
    default: return Class::kOther;
  }
}
constexpr const char* ClassName(Class c) {
  switch (c) {
    case Class::kHeadlight: return "headlight";
    case Class::kNeon: return "neon";
    case Class::kLamp: return "street lamp";
    case Class::kOther: return "other";
  }
  return "?";
}

inline float Peak(const Light& l) { return std::max({l.color[0], l.color[1], l.color[2]}); }
inline bool FiniteLight(const Light& l) {
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(l.pos[i]) || !std::isfinite(l.dir[i]) || !std::isfinite(l.color[i]) || !std::isfinite(l.offset[i])) {
      return false;
    }
  }
  return std::isfinite(l.range) && std::isfinite(l.cos_outer) && std::isfinite(l.law_a);
}
// A light the ray pass may trace for: it has an id, the RT role, a range and a colour, numbers a ray can use.
inline bool Eligible(const Light& l) {
  return l.id != kNoLightId && (l.roles & kRoleMaskRt) != 0 && l.range > 0.0f && Peak(l) > 0.0f && FiniteLight(l);
}
// What a light adds to the view: its colour over its distance squared plus its plateau (a street lamp's law_a is I, the plateau's radius squared; a headlight's is its falloff distance).
inline float Score(const Light& l, const float eye[3]) {
  if (!Eligible(l)) {
    return 0.0f;
  }
  float d2 = 0.0f;
  for (int i = 0; i < 3; ++i) {
    d2 += (l.pos[i] - eye[i]) * (l.pos[i] - eye[i]);
  }
  return Peak(l) / (d2 + std::max(l.law_a, 1.0f));
}

// A candidate: where it stands in the caller's array (the pass's selection), and what it scores.
struct Pick {
  std::array<uint32_t, kMaxLights> index{};   // into the caller's array, best first
  std::array<uint64_t, kMaxLights> id{};
  uint32_t count = 0;
};

// The top `k` of `lights[0 .. n)`: eligible ones, by class, then by score (a light in `previous` scoring KeepBonus more), then by id (a stable order for equal scores). Two lights with one id (a
// light handed over twice) count once. `previous` = the ids that held a channel last frame.
inline Pick SelectTopK(const Light* lights, uint32_t n, const float eye[3], const uint64_t* previous, uint32_t previous_count, uint32_t k) {
  struct Cand {
    uint32_t index;
    uint64_t id;
    Class cls;
    float score;
  };
  std::array<Cand, 64> cands{};
  uint32_t count = 0;
  const auto held = [&](uint64_t id) {
    for (uint32_t i = 0; i < previous_count; ++i) {
      if (previous[i] == id) {
        return true;
      }
    }
    return false;
  };
  for (uint32_t i = 0; i < n && count < cands.size(); ++i) {
    const Light& l = lights[i];
    if (!Eligible(l)) {
      continue;
    }
    bool seen = false;
    for (uint32_t j = 0; j < count && !seen; ++j) {
      seen = cands[j].id == l.id;
    }
    if (seen) {
      continue;
    }
    const float score = Score(l, eye) * (held(l.id) ? kKeepBonus : 1.0f);
    cands[count++] = Cand{i, l.id, ClassOf(l), score};
  }
  std::sort(cands.begin(), cands.begin() + count, [](const Cand& a, const Cand& b) {
    if (a.cls != b.cls) {
      return a.cls < b.cls;
    }
    if (a.score != b.score) {
      return a.score > b.score;
    }
    return a.id < b.id;
  });
  Pick out;
  const uint32_t take = std::min({k, count, kMaxLights});
  for (uint32_t i = 0; i < take; ++i) {
    out.index[out.count] = cands[i].index;
    out.id[out.count] = cands[i].id;
    ++out.count;
  }
  return out;
}

// ---------------------------------------------------------------------------
// the channels' owners
// ---------------------------------------------------------------------------

// Who holds each channel (0: free).
struct Assignment {
  std::array<uint64_t, kMaxLights> id{};
  friend bool operator==(const Assignment&, const Assignment&) = default;
};

struct Assigned {
  Assignment now;
  std::array<uint8_t, kMaxLights> channel_of_wanted{};   // for wanted[i]: its channel (kNoChannel when no room: never with n <= limit)
  uint32_t changed_groups = 0;                           // bit g: a channel of image g has another owner than last frame (or none): that group's history starts again
};

// A light that held a channel keeps it while it is wanted; the others take the lowest free channel below `limit` (= ImagesFor(K) x 3: the group count follows K). A channel at or above
// `limit` is let go (K went down). `wanted[0 .. n)` is the pick's ids, best first, n <= kMaxLights, no duplicate.
inline Assigned AssignChannels(const Assignment& previous, const uint64_t* wanted, uint32_t n, uint32_t limit) {
  Assigned out;
  out.channel_of_wanted.fill(uint8_t(kNoChannel));
  limit = std::min(limit, kMaxLights);
  n = std::min(n, kMaxLights);
  std::array<bool, kMaxLights> taken{};
  std::array<bool, kMaxLights> placed{};
  for (uint32_t i = 0; i < n; ++i) {
    for (uint32_t c = 0; c < limit; ++c) {
      if (wanted[i] != 0 && previous.id[c] == wanted[i] && !taken[c]) {
        out.now.id[c] = wanted[i];
        out.channel_of_wanted[i] = uint8_t(c);
        taken[c] = true;
        placed[i] = true;
        break;
      }
    }
  }
  for (uint32_t i = 0; i < n; ++i) {
    if (placed[i] || wanted[i] == 0) {
      continue;
    }
    for (uint32_t c = 0; c < limit; ++c) {
      if (!taken[c]) {
        out.now.id[c] = wanted[i];
        out.channel_of_wanted[i] = uint8_t(c);
        taken[c] = true;
        break;
      }
    }
  }
  for (uint32_t c = 0; c < kMaxLights; ++c) {
    if (out.now.id[c] != previous.id[c]) {
      out.changed_groups |= GroupBit(c);
    }
  }
  return out;
}

// The channel an id holds in an assignment, or kNoChannel.
inline uint32_t ChannelFor(const Assignment& a, uint64_t id) {
  if (id == 0) {
    return kNoChannel;
  }
  for (uint32_t c = 0; c < kMaxLights; ++c) {
    if (a.id[c] == id) {
      return c;
    }
  }
  return kNoChannel;
}
inline uint32_t Used(const Assignment& a) {
  uint32_t n = 0;
  for (uint64_t id : a.id) {
    n += id != 0 ? 1u : 0u;
  }
  return n;
}

// ---------------------------------------------------------------------------
// the light's radius and the stop short
// ---------------------------------------------------------------------------

constexpr float DefaultRadius(Class c) {
  return c == Class::kHeadlight ? kRadiusHeadlight : (c == Class::kNeon ? kRadiusNeon : (c == Class::kLamp ? kRadiusLamp : kRadiusOther));
}
constexpr float StopShort(Class c) {
  return c == Class::kHeadlight ? kStopHeadlight : (c == Class::kNeon ? kStopNeon : (c == Class::kLamp ? kStopLamp : kStopOther));
}
// mcla_gpu_rt_lights_radius: 0 = the class's own, else metres (kept to 0 .. kMaxRadius), for every class.
inline float RadiusOf(double setting, Class c) {
  return setting > 0.0 ? float(std::min(setting, double(kMaxRadius))) : DefaultRadius(c);
}

// ---------------------------------------------------------------------------
// the rows the trace reads (hlsl/rt_lights_trace_cs.hlsl; a 64 byte header, then the lights)
// ---------------------------------------------------------------------------

inline constexpr uint32_t kGpuFlagSpot = 1u << 0;   // the cone applies (a point light has none)
inline constexpr uint32_t kGpuFlagPair = 1u << 1;   // two lamps: the ray goes to one of pos +- offset (a car's two headlamps; the views of the atlas are averaged, so are the rays)

// One light as the trace reads it: four float4, world space (the shader moves it to the top level's view space with the world frame's axes).
struct alignas(16) GpuRtLight {
  float row0[4];   // pos.xyz, range
  float row1[4];   // dir.xyz, cos_outer
  float row2[4];   // offset.xyz (the right lamp of a pair, from pos), radius
  float stop_short;
  uint32_t channel;
  uint32_t flags;   // kGpuFlag*
  uint32_t reserved;
};
static_assert(sizeof(GpuRtLight) == 64 && alignof(GpuRtLight) == 16 && std::is_trivially_copyable_v<GpuRtLight>);
static_assert(offsetof(GpuRtLight, row0) == 0 && offsetof(GpuRtLight, row1) == 16 && offsetof(GpuRtLight, row2) == 32 && offsetof(GpuRtLight, stop_short) == 48 && offsetof(GpuRtLight, channel) == 52 &&
              offsetof(GpuRtLight, flags) == 56);

inline GpuRtLight MakeGpuRtLight(const Light& l, uint32_t channel, double radius_setting) {
  GpuRtLight g{};
  const Class cls = ClassOf(l);
  for (int i = 0; i < 3; ++i) {
    g.row0[i] = l.pos[i];
    g.row1[i] = l.dir[i];
    g.row2[i] = l.offset[i];
  }
  g.row0[3] = l.range;
  g.row1[3] = l.cos_outer;
  g.row2[3] = RadiusOf(radius_setting, cls);
  g.stop_short = StopShort(cls);
  g.channel = channel;
  const bool pair = std::fabs(l.offset[0]) + std::fabs(l.offset[1]) + std::fabs(l.offset[2]) > 1.0e-4f;
  g.flags = (l.kind == LightKind::kSpot ? kGpuFlagSpot : 0u) | (pair ? kGpuFlagPair : 0u);
  return g;
}

inline constexpr uint32_t kFlagStats = 1u << 0;          // a census frame: the counters are added to
inline constexpr uint32_t kFlagCheckVisible = 1u << 1;   // mcla_gpu_rt_lights_check 1
inline constexpr uint32_t kFlagCheckBlocked = 1u << 2;   // mcla_gpu_rt_lights_check 2
inline constexpr uint32_t kFlagBlue = 1u << 3;           // the shared blue noise (mcla_gpu_noise_blue); off: the first versions' gradient noise (a comparison only)

// The header: row 0 the image's size, the frame, the flags; row 1 the lights and rays; row 2 the camera's lens and the range; row 3 reserved.
struct alignas(16) GpuRtHeader {
  uint32_t width, height, frame, flags;
  uint32_t light_count, rays, ray_mask, reserved0;
  float tan_x, tan_y, max_range, reserved1;
  uint32_t reserved2[4];
};
static_assert(sizeof(GpuRtHeader) == 64 && std::is_trivially_copyable_v<GpuRtHeader>);

inline constexpr uint32_t kRowsHeaderBytes = 64;
inline constexpr uint32_t kRowsBytes = kRowsHeaderBytes + kMaxLights * uint32_t(sizeof(GpuRtLight));   // 640
inline constexpr uint32_t kRowsAlloc = 1024;
static_assert(kRowsAlloc >= kRowsBytes && kRowsBytes == 640);

// ---------------------------------------------------------------------------
// the table the light pass reads (local_lights_rules.h appends it to the pass's buffer; hlsl/rt_mask_read.hlsli reads it)
// ---------------------------------------------------------------------------

inline constexpr uint32_t kMaskFlagValid = 1u << 0;        // the images are this frame's: a light's channel is to be read
inline constexpr uint32_t kMaskFlagRawView = 1u << 1;      // the debug view shows the raw images (mcla_gpu_rt_lights_view 2)
inline constexpr uint32_t kMaskFlagView = 1u << 2;         // the light pass draws the visibility as a picture instead of the light (mcla_gpu_rt_lights_view 1 or 2)

struct alignas(16) GpuRtMaskTable {
  uint32_t images, flags, width, height;     // the mask images' count, the flags, their size in texels (the traced size)
  uint32_t slot[3];                          // the filtered images' bindless slots (kNoSlot: not there)
  uint32_t lin_slot;                         // the occlusion's linear depth image at the same size (the upsample's guide); kNoSlot: nearest
  uint32_t raw_slot[3];                      // the unfiltered images' slots (the debug view)
  uint32_t reserved0;
  float depth_sigma;
  uint32_t reserved1[3];
};
static_assert(sizeof(GpuRtMaskTable) == 64 && std::is_trivially_copyable_v<GpuRtMaskTable>);
static_assert(offsetof(GpuRtMaskTable, slot) == 16 && offsetof(GpuRtMaskTable, lin_slot) == 28 && offsetof(GpuRtMaskTable, raw_slot) == 32 && offsetof(GpuRtMaskTable, depth_sigma) == 48);

// What the light pass is handed of a frame the ray pass ran.
struct MaskFrame {
  bool valid = false;
  uint32_t images = 0;
  uint32_t width = 0, height = 0;
  std::array<uint32_t, kMaxImages> slot{kNoLightSlot, kNoLightSlot, kNoLightSlot};
  std::array<uint32_t, kMaxImages> raw_slot{kNoLightSlot, kNoLightSlot, kNoLightSlot};
  uint32_t lin_slot = kNoLightSlot;
  int view = kViewOff;
};

// The table of a frame with no masks: nothing valid, every slot absent (the light pass reads no channel).
inline GpuRtMaskTable EmptyMaskTable() {
  GpuRtMaskTable t{};
  t.slot[0] = t.slot[1] = t.slot[2] = kNoLightSlot;
  t.raw_slot[0] = t.raw_slot[1] = t.raw_slot[2] = kNoLightSlot;
  t.lin_slot = kNoLightSlot;
  return t;
}

inline GpuRtMaskTable BuildMaskTable(const MaskFrame& f) {
  if (!f.valid || f.images == 0 || f.images > kMaxImages || f.width == 0 || f.height == 0) {
    return EmptyMaskTable();
  }
  for (uint32_t i = 0; i < f.images; ++i) {
    if (f.slot[i] == kNoLightSlot) {
      return EmptyMaskTable();   // a table is valid only if every image a light can name is there
    }
  }
  GpuRtMaskTable t = EmptyMaskTable();
  t.images = f.images;
  t.width = f.width;
  t.height = f.height;
  for (uint32_t i = 0; i < f.images; ++i) {
    t.slot[i] = f.slot[i];
    t.raw_slot[i] = f.raw_slot[i];
  }
  t.lin_slot = f.lin_slot;
  t.depth_sigma = kDepthSigma;
  t.flags = kMaskFlagValid | (f.view != kViewOff ? kMaskFlagView : 0u) | (f.view == kViewRaw ? kMaskFlagRawView : 0u);
  return t;
}
inline bool MaskTableValid(const GpuRtMaskTable& t) { return (t.flags & kMaskFlagValid) != 0 && t.images != 0 && t.width != 0 && t.height != 0; }

// The bytes the masks hold: three RGBA16F-class images a group at the traced size, the guides (R32F depth twice, RGBA16F normal, RG16F hit) once and RtDenoise's four RGBA16F images a group
// (rt_denoise_rules.h ImageBytes), kBytesPerPixel 8: a group = 5 x 8, the guides 8 + 8 + 8 + 4 = 28.
constexpr uint64_t HeldBytes(uint32_t width, uint32_t height, uint32_t images) {
  return uint64_t(width) * height * (uint64_t(images) * 5u * 8u + 28u);
}

// ---------------------------------------------------------------------------
// the composition (THE rule: the twin of the light pass's pixel paths)
// ---------------------------------------------------------------------------

// A visibility as the pass uses it: a number in [0, 1]; NaN or an infinity (a bad texel, a filter's overshoot) is 1 (lit: it takes nothing away).
inline float CleanVisibility(float v) {
  if (!std::isfinite(v)) {
    return 1.0f;
  }
  return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}
// The atlas's visibility times the ray's (each 1 where the light has none).
inline float Combine(float atlas, float rt) { return CleanVisibility(atlas) * CleanVisibility(rt); }
// How much of a game's lamp's term a visibility takes away: term x (1 - V), always in [0, term] (a term that is negative or not a number takes nothing). The pass's colour is minus this.
inline float SubtractShare(float term, float visibility) {
  if (!std::isfinite(term) || !(term > 0.0f)) {
    return 0.0f;
  }
  return term * (1.0f - CleanVisibility(visibility));
}
// The colour a light adds (rgb): a light the game lights is TAKEN AWAY from (-term x (1 - V), never positive), a light we own is added (term x V). With V = 1 a game's lamp adds exactly nothing (a
// negative zero: the blend ADD leaves the colour as it was) and an owned lamp adds its term unchanged: the identity.
inline void Apply(bool game_lit, const float term[3], float visibility, float out[3]) {
  for (int i = 0; i < 3; ++i) {
    out[i] = game_lit ? -SubtractShare(term[i], visibility) : term[i] * CleanVisibility(visibility);
  }
}

// ---------------------------------------------------------------------------
// the pass's layout, push block and counters (rt_lights.cpp builds it; hlsl/rt_lights_trace_cs.hlsl declares it)
// ---------------------------------------------------------------------------

// The compute layout: the bindless texture set (space 0: the occlusion's linear depth image), this pass's own set (space 1), the ray tracing set of the car scene (space 2: the top level and the world frame's
// rows) and the push block at (b0, space 3).
inline constexpr uint32_t kTextureSet = 0, kPassSet = 1, kSceneSet = 2, kPushSpace = 3;
// The pass's set: t0 the rows, u1 u2 u3 the signal images (a group of three lights each), u4 the depth guide, u5 the normal guide, u6 the hit and share guide, u7 the counters.
inline constexpr uint32_t kBindRows = 0, kBindSignal0 = 1, kBindSignal1 = 2, kBindSignal2 = 3, kBindDepth = 4, kBindNormal = 5, kBindHit = 6, kBindStats = 7;
inline constexpr uint32_t kSetViews = 8;   // one view for each descriptor: the rows, three signals, three guides, the counters (D3D12 sizes a set by this count)
inline constexpr uint32_t kGroup = 8;      // numthreads(8, 8, 1)
constexpr uint32_t Groups(uint32_t extent) { return (extent + kGroup - 1) / kGroup; }
struct Push {
  uint32_t slots[4];   // x the occlusion's linear depth image's bindless slot, y the number of signal images this frame writes
};
static_assert(sizeof(Push) == 16);

// The counters (hlsl kStat*): pixels, surfaces judged, pixels some light reaches, (pixel, light) pairs, rays, rays the city blocked, rays a car blocked, pairs whose visibility is under one half.
inline constexpr uint32_t kStatPixels = 0, kStatWorld = 1, kStatCovered = 2, kStatPairs = 3, kStatRays = 4, kStatBlockedCity = 5, kStatBlockedCar = 6, kStatBlockedPairs = 7;
inline constexpr uint32_t kStatWords = 8;
inline constexpr uint32_t kStatBytes = kStatWords * 4;

// The filter: RtDenoise's diffuse profile over the plain guides, two a-trous iterations (steps 1 and 2: the penumbra of a lamp's disc is a few pixels wide; part 2 tunes it and measures it).
inline constexpr uint32_t kDenoiseIterations = 2;
inline constexpr uint32_t kStatsEvery = 10;           // a census frame: the counters are added to
inline constexpr uint32_t kReadbackEveryFrames = 600;
inline constexpr uint32_t kReleaseAfterFrames = 120;  // the images go back after this many frames without a run (2 s at 60)
inline constexpr uint32_t kFirstDenoiseInstance = 2;  // RtDenoise instances 0 (the reflections') and 1 (the per-pixel bounce light's) are taken; the three groups take 2, 3 and 4

// The camera cut: the last run's pose is no history when the camera moved farther than this in a frame, or turned by more than about 45 degrees.
inline constexpr double kCutMetres = 25.0;
inline constexpr double kCutCosine = 0.7;
inline bool CameraCut(const double prev_position[3], const double position[3], const double prev_forward[3], const double forward[3]) {
  double d2 = 0.0;
  double dot = 0.0;
  for (int i = 0; i < 3; ++i) {
    d2 += (position[i] - prev_position[i]) * (position[i] - prev_position[i]);
    dot += prev_forward[i] * forward[i];
  }
  return d2 > kCutMetres * kCutMetres || dot < kCutCosine;
}

// ---------------------------------------------------------------------------
// whether the pass runs (the reasons, for the census: gi_pixel_rules.h Why's shape)
// ---------------------------------------------------------------------------

enum class Why : uint8_t { kRuns = 0, kOff, kNoDevice, kNoScene, kNoWorld, kNoDepth, kNoLights, kPipelines, kResources, kCount };
constexpr const char* WhyName(Why w) {
  switch (w) {
    case Why::kRuns: return "runs";
    case Why::kOff: return "the setting is off";
    case Why::kNoDevice: return "the device does not trace rays";
    case Why::kNoScene: return "no top level this frame";
    case Why::kNoWorld: return "the world frame's rows are not this frame's";
    case Why::kNoDepth: return "no linear depth or camera";
    case Why::kNoLights: return "no light has a ray";
    case Why::kPipelines: return "the pipelines are not made yet";
    case Why::kResources: return "the images or the buffer could not be made";
    case Why::kCount: break;
  }
  return "?";
}
struct Facts {
  bool wanted = false;
  bool device_ok = false;
  bool scene_ok = false;
  bool world_ok = false;
  bool depth_ok = false;
  uint32_t lights = 0;
  bool pipelines_ready = false;
  bool resources_ok = false;
};
// The first reason that applies, in the order a frame would find it. Nothing is asked for (no structures, no pipelines, no images) until every reason before it holds.
constexpr Why Decide(const Facts& f) {
  if (!f.wanted) return Why::kOff;
  if (!f.device_ok) return Why::kNoDevice;
  if (!f.scene_ok) return Why::kNoScene;
  if (!f.world_ok) return Why::kNoWorld;
  if (!f.depth_ok) return Why::kNoDepth;
  if (f.lights == 0) return Why::kNoLights;
  if (!f.pipelines_ready) return Why::kPipelines;
  if (!f.resources_ok) return Why::kResources;
  return Why::kRuns;
}

}  // namespace larecomp::gpu::rt_lights
