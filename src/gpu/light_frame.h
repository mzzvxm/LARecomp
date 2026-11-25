#pragma once
// ===========================================================================
// MCLA GPU renderer -- ONE light description and the frame's lights: THE FROZEN INTERFACE (perf/LIGHT-UNIFY.md section 2, package LU-1 = step U1)
// ===========================================================================
// Replaces light_set.h (the GB-SPIKE interface, which is now a thin include of light_convert.h). Today the same lights are described in seven shapes (the headlight rig's
// Headlight and its 672 byte block, VolLamp, the game's GameLamp, light_set.h's Light, local_shadow's Light and Group, the lamp-shadow block) and picked four times; every
// consumer wrote its own adapter. This header is the one record they all read:
//
//   Light        what ONE light is: where, which way, how wide, how much (the colour is already times the strength, the fade and the night factor), WHICH FALLOFF the numbers
//                mean (the law), who made it, what it may be used for (the role mask), who it is (a stable 64 bit id), how it moves, where its shadow comes from (ShadowRef).
//   LightFrame   what a frame has: up to kMaxLights lights, each role's ranking, the shadow views and the atlas, the cars' light state (CarLights), the GPU buffer's handles.
//   GpuLight     the 64 byte twin of a Light the deferred light pass reads (four float4), one buffer a frame (GpuLightBuffer).
//
// The producer is the LightManager (light_manager.{h,cpp}, LU-2): the only writer. Consumers get `const LightFrame&`. Everything here is PLAIN DATA (no renderer, no guest, no
// SDK), so a test or a mock fills a LightFrame the way volumetrics_types.h's VolInputs is filled.
//
// WHAT IS NOT HERE: the laws (the falloff each `law` names, light_laws.h and its HLSL twin hlsl/light_laws.hlsli), the ids (light_id.h), and the conversions from the
// producers that exist today (light_convert.h). The picker, the budgets and the shadow atlas are LU-3 and LU-5.
//
// UNITS AND CONVENTIONS (the volume's, the rig's and light_set.h's, now stated once)
//   world        the game's: y up, metres; positions and directions are world space; `dir` is a unit vector (the way the light points).
//   colour       scene-referred linear light of the main view (0 to about 2 for a headlight, a street lamp's is rgb x I with I 20 to 100), ALREADY times the light's strength,
//                its fade and the night factor: a light that fades is a dimmer one. Where a consumer needs another unit it asks a law (light_laws.h VolumeUnits).
//   the cone     `cos_outer` the cosine of the outer half angle (the term is 0 beyond it), `cos_inner` of the inner one (full inside), smooth between; cos_outer <= -0.999
//                is a POINT light (no cone; kind kPoint).
//   range        the reach in metres: the term is 0 at and beyond it (a lamp's is where the game's term is 1 per cent: sqrt(100 I), kept between 8 and 120 m).
//   time         a light's fades run on the scene clock in seconds (LU-3); `state` and the three weights below say where they are.
//   slots        a light keeps its index in LightFrame::lights while it lives ("slot-stable").
// ===========================================================================

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "light_id.h"  // kNoLightId
#include "local_shadow_types.h"

namespace larecomp::gpu {

// ---------------------------------------------------------------------------
// the enums
// ---------------------------------------------------------------------------

enum class LightKind : uint8_t { kSpot = 0, kPoint = 1 };

// Who made the light, for the picker's per-source tuning and the debug lines.
enum class LightSource : uint8_t {
  kHeadlight = 0,  // a car's headlight (headlight_rig.h)
  kLamp = 1,       // a street lamp or a window the game does not draw as a light volume (the game's lamp list)
  kNeon = 2,       // a car's underglow (GB-NEON: neon_light.h; the game has no light data for its neon SIGNS, only for the cars' own)
  kPolice = 3,
  kMod = 4,        // lights.bin of a mod (mod_lights.cpp)
  kOther = 5,
};

// Which falloff the numbers mean (light_laws.h; the three the game and the renderer have today, perf/LIGHT-UNIFY.md 1.3):
//   kHeadlight  the lit shaders' headlight term, calibrated by eye: kPeak 2.0 x 1/(1+(d/D)^2) x (1-(d/range)^2)^2 x cone x surface wrap x low beam cut x near fade; law_a = D.
//   kGameLamp   the game's own lamp (PSMultiLight): colour x min(1/I, 1/d^2) x cone x N.L; law_a = I (the plateau's radius squared).
//   kOmni       the game's lamp without a cone (police, a point lamp): the same distance law and N.L; law_a = the plateau's radius squared.
//   kNeon       the cars' underglow (GB-NEON-TUNE): colour x w(d)^2 / (d^2 + near^2) x a wrap of N.L, w = saturate(1 - (d / range)^4): a smooth window that is 0 AND flat at `range`, so
//               the reach test, the light's bounds and the culling are one radius and there is no edge there; law_a = the near radius squared (the volume's lamp law, with a wrap).
enum class LightLaw : uint8_t { kHeadlight = 0, kGameLamp = 1, kOmni = 2, kNeon = 3 };

enum class ShadowKind : uint8_t { kNone = 0, kAtlas = 1, kRtMask = 2 };

// What a view of the shadow atlas is of its light (ShadowFrame::view_layer): kSingle a view that stands alone (a headlamp's), kStatic the map of the light's static casters and
// kLive the map of the moving ones (a street lamp's two layers: the live entry FOLLOWS its static entry, same matrix, the other tile).
enum class ShadowLayer : uint8_t { kSingle = 0, kStatic = 1, kLive = 2 };

enum class FadeState : uint8_t { kSteady = 0, kIn = 1, kOut = 2, kCoast = 3 };

// The three rankings a frame holds (LightFrame::order): the lights that add surface light (the forward headlight block, the deferred pass), the lights in the volume and the
// lights with shadow views.
enum class LightRole : uint8_t { kSurface = 0, kVolume = 1, kShadow = 2 };
inline constexpr uint32_t kRoleCount = 3;

// A light's role MASK (Light::roles): what it may be used for. Bit 3 is the ray traced shadows (no ranking of its own: the first K of the shadow ranking).
inline constexpr uint8_t kRoleMaskSurface = 1u << 0;
inline constexpr uint8_t kRoleMaskVolume = 1u << 1;
inline constexpr uint8_t kRoleMaskShadow = 1u << 2;
inline constexpr uint8_t kRoleMaskRt = 1u << 3;
inline constexpr uint8_t kRoleMaskAll = kRoleMaskSurface | kRoleMaskVolume | kRoleMaskShadow | kRoleMaskRt;
// A light the game's own shaders already light with (a street lamp): consumers that ADD surface light skip it; the volume, the shadows and the RT use it.
inline constexpr uint8_t kRoleMaskGameLit = kRoleMaskVolume | kRoleMaskShadow | kRoleMaskRt;
constexpr uint8_t RoleBit(LightRole role) { return uint8_t(1u << uint32_t(role)); }

inline constexpr uint32_t kLightFlagPlayer = 1u << 0;          // the player's car's lamp (the rig's is_player)
inline constexpr uint32_t kLightFlagLightsGeometry = 1u << 1;  // the light also lights the surface that carries it (a lamp's own housing: usually off)
inline constexpr uint32_t kLightFlagGameLit = 1u << 2;         // the game's own shaders already light with this one: no surface ADD (see kRoleMaskGameLit)
// The light has a ray traced visibility channel (ShadowRef::mask_channel; rt_lights_rules.h): the deferred light pass reads it ON TOP OF whatever the atlas says (the product: a light whose atlas
// view holds a pole and whose ray holds the wall is shadowed by both), and where it is the only source of visibility a light the game lights is still only ever TAKEN AWAY from, never added.
inline constexpr uint32_t kLightFlagRtVisibility = 1u << 3;
// Flags 16 and up are CPU side only: GpuLight carries the low 16 bits.
inline constexpr uint32_t kLightFlagsGpuMask = 0xFFFFu;

// ---------------------------------------------------------------------------
// the limits
// ---------------------------------------------------------------------------

inline constexpr uint32_t kMaxLights = 64;         // what a frame holds (the game's list has 64 spots in a night frame, at most 83: the pick keeps the best)
inline constexpr uint32_t kMaxSurfaceLights = 32;  // the deferred light pass's budget (the first of order[kSurface])
// A light has at most two views (a car's two headlamps; a street lamp's static + live pair counts as two entries): this is the room for them (hl-per-lamp's local_shadow_types.h
// calls the same number kMaxShadowViews; it is spelt out here so that the two headers can be in one program).
inline constexpr uint32_t kMaxLightShadowViews = 8;
inline constexpr uint32_t kMaxCars = 8;            // the cars whose lights say something about their own look (CarLights)
inline constexpr uint32_t kNoLightSlot = 0xFFFFFFFFu;  // a bindless slot that is not there

// ---------------------------------------------------------------------------
// the shadow reference of a light
// ---------------------------------------------------------------------------

// Where a light's visibility comes from. `kAtlas`: the entries [first_view, first_view + view_count) of ShadowFrame (consecutive, at most two: one per headlamp, or a static +
// live pair); `kRtMask`: the ray traced shadows' channel `mask_channel` (rt_mask_contract.h: the planes, the values, the assignment and the reader's checks); `kNone`: unshadowed. THE ONE COMBINE RULE (light_laws.h CombineShadow): the views of a light are
// AVERAGED (each is one lamp of a car: a pole both lamps light casts two half-dark shadows), the layers of a view are MULTIPLIED (static x live), and the result is blended by
// the weight: lit = lerp(1, visibility, weight).
struct ShadowRef {
  ShadowKind kind = ShadowKind::kNone;
  uint8_t view_count = 0;  // 0..2
  uint8_t first_view = 0;  // index into ShadowFrame::views
  uint8_t mask_channel = 0;  // kRtMask
  float weight = 1.0f;     // 0..1: how much of the shadow shows (it fades in and out with the slot, 0.35 s)
  float reach = 0.0f;      // metres the shadow is made for (a lamp's sqrt(8 I) clamped 14..30; a headlight's range)
};

// ---------------------------------------------------------------------------
// the light
// ---------------------------------------------------------------------------

struct Light {                       // slot-stable: a light keeps its index in LightFrame::lights while it lives
  float pos[3] = {0.0f, 0.0f, 0.0f};
  float range = 0.0f;                // the term is 0 at and beyond it; 0: the light is off
  float dir[3] = {0.0f, -1.0f, 0.0f};
  float cos_outer = -1.0f;           // -1: a point light
  float color[3] = {0.0f, 0.0f, 0.0f};  // rgb x intensity x strength x fade x night, scene units
  float cos_inner = -1.0f;
  float vel[3] = {0.0f, 0.0f, 0.0f};    // m/s (0 for a lamp): the volume's moving-beam rule and the picker's coast
  float vol_weight = 1.0f;           // 0..1: the volume's own fade (its budget differs from the surface's: three roles, three clocks)
  float offset[3] = {0.0f, 0.0f, 0.0f};  // headlights: the vector from `pos` (the middle of the pair) to the car's RIGHT lamp, the left is the opposite; 0: one view
  float law_a = 0.0f;                // the law's one parameter: a headlight's falloff distance D, a lamp's plateau radius squared I
  LightKind kind = LightKind::kSpot;
  LightSource source = LightSource::kOther;
  LightLaw law = LightLaw::kGameLamp;
  FadeState state = FadeState::kSteady;
  uint8_t roles = kRoleMaskAll;      // kRoleMask*: what this light may be used for
  uint8_t pad[3] = {0, 0, 0};
  uint32_t flags = 0;                // kLightFlag*
  uint32_t car_key = 0;              // the rig's track key (0: not a car's)
  uint64_t id = 0;                   // identity across frames (light_id.h); kNoLightId: unused slot
  float importance = 0.0f;           // the picker's score this frame (consumers take prefixes by it)
  ShadowRef shadow;
};

static_assert(std::is_trivially_copyable_v<Light> && std::is_standard_layout_v<Light>);
static_assert(sizeof(Light) == 120 && alignof(Light) == 8, "Light is 120 bytes");
static_assert(offsetof(Light, pos) == 0 && offsetof(Light, range) == 12 && offsetof(Light, dir) == 16 && offsetof(Light, cos_outer) == 28);
static_assert(offsetof(Light, color) == 32 && offsetof(Light, cos_inner) == 44 && offsetof(Light, vel) == 48 && offsetof(Light, vol_weight) == 60);
static_assert(offsetof(Light, offset) == 64 && offsetof(Light, law_a) == 76 && offsetof(Light, kind) == 80 && offsetof(Light, source) == 81);
static_assert(offsetof(Light, law) == 82 && offsetof(Light, state) == 83 && offsetof(Light, roles) == 84 && offsetof(Light, flags) == 88);
static_assert(offsetof(Light, car_key) == 92 && offsetof(Light, id) == 96 && offsetof(Light, importance) == 104 && offsetof(Light, shadow) == 108);
static_assert(sizeof(ShadowRef) == 12 && offsetof(ShadowRef, weight) == 4 && offsetof(ShadowRef, reach) == 8);

// ---------------------------------------------------------------------------
// what a car's lights say about its own look
// ---------------------------------------------------------------------------

// RT hit shading and the G-buffer's emissives want to know whether a car's lens glows (head, brake, tail, turn, reverse, police) WITHOUT a light being added to the surface:
// instead of a night factor guess (car_rt_material_rules.h). The bits and the four levels come from the game's own light points (headlight_points.h: 2.0 is on).
inline constexpr uint32_t kCarLightHead = 1u << 0;
inline constexpr uint32_t kCarLightHiBeam = 1u << 1;
inline constexpr uint32_t kCarLightFog = 1u << 2;
inline constexpr uint32_t kCarLightBrake = 1u << 3;
inline constexpr uint32_t kCarLightTail = 1u << 4;
inline constexpr uint32_t kCarLightTurnL = 1u << 5;
inline constexpr uint32_t kCarLightTurnR = 1u << 6;
inline constexpr uint32_t kCarLightReverse = 1u << 7;
inline constexpr uint32_t kCarLightPoliceA = 1u << 8;
inline constexpr uint32_t kCarLightPoliceB = 1u << 9;
inline constexpr uint32_t kCarLightPoliceC = 1u << 10;
// The four levels: where each lamp family's brightness goes in CarLights::level.
inline constexpr uint32_t kCarLevelHead = 0, kCarLevelBrake = 1, kCarLevelTailTurn = 2, kCarLevelPolice = 3;

struct CarLights {
  uint32_t car_key = 0;      // the rig's track key (0: unused)
  uint32_t vehicle = 0;      // the vehicle's guest address (0: unknown: traffic's points have none)
  uint32_t bits = 0;         // kCarLight*
  float level[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // head, brake, tail / turn, police: 0..1 as the game's point intensities say (2.0 = on = 1)
};
static_assert(sizeof(CarLights) == 28 && offsetof(CarLights, level) == 12);

// ---------------------------------------------------------------------------
// the shadow views of a frame
// ---------------------------------------------------------------------------

// The atlas the views are in, as the replay publishes it (local_shadow.h AtlasInfo): a bindless depth texture (the depth plane viewed as one float on both APIs) and the size
// classes of its tiles.
struct ShadowAtlasInfo {
  bool valid = false;
  uint32_t slot = kNoLightSlot;     // the bindless slot of the atlas
  uint32_t sampler = kNoLightSlot;  // a point sampler's slot, kNoLightSlot: the readers Load()
  uint32_t width = 0, height = 0;   // texels
  uint32_t tile_small = 0, tile_large = 0;  // the side of a small and a large tile in texels (512 / 1024), 0: none of that size
};

// The views of a frame, as of NOW (before the trigger: the maps of the last frame, remapped to these ids; after it: this frame's). `views` are the 96 byte LocalShadowView, kept
// field for field (the forward headlight block and the volume's VolShadowView read them byte for byte); `view_light` says whose each is (a light's id, not the rig's index:
// LocalShadowView::light_index stays the rig's, for the legacy projections). A light's entries are CONSECUTIVE (ShadowRef::first_view, view_count) and are either one or two
// kSingle entries (the views to average) or one kStatic entry followed by its kLive twin (the layers to multiply: they count as ONE view).
struct ShadowFrame {
  ShadowAtlasInfo atlas;
  std::array<LocalShadowView, kMaxLightShadowViews> views{};
  std::array<uint64_t, kMaxLightShadowViews> view_light{};
  std::array<ShadowLayer, kMaxLightShadowViews> view_layer{};
  uint32_t count = 0;
};

// ---------------------------------------------------------------------------
// the GPU twin: GpuLight, 64 bytes, one buffer a frame
// ---------------------------------------------------------------------------

// four float4:
//   row0 pos.xyz, range        row1 dir.xyz, cos_outer        row2 color.xyz, cos_inner
//   row3 uint packed0, float law_a, uint packed1, float shadow_weight
// packed0: bits 0-1 kind | 2-4 source | 5-6 law | 7-10 role mask | 11-15 zero | 16-31 flags (the low 16 bits of Light::flags)
// packed1: bits 0-7 first_view | 8-15 view_count | 16-23 shadow kind | 24-31 mask_channel
// What the GPU does NOT carry: vel, vol_weight (the volume's colour has it folded in: light_convert.h ToVolLamp), offset, state, car_key, id, importance, the shadow reach, flags
// 16 and up. VolLamp is the same four rows (its pad[0] is the near radius squared); the volume's own shader keeps its layout.
struct alignas(16) GpuLight {
  float row0[4];
  float row1[4];
  float row2[4];
  uint32_t packed0;
  float law_a;
  uint32_t packed1;
  float shadow_weight;
};
static_assert(sizeof(GpuLight) == 64 && alignof(GpuLight) == 16, "GpuLight is four float4");
static_assert(offsetof(GpuLight, row0) == 0 && offsetof(GpuLight, row1) == 16 && offsetof(GpuLight, row2) == 32);
static_assert(offsetof(GpuLight, packed0) == 48 && offsetof(GpuLight, law_a) == 52 && offsetof(GpuLight, packed1) == 56 && offsetof(GpuLight, shadow_weight) == 60);
static_assert(std::is_trivially_copyable_v<GpuLight> && std::is_standard_layout_v<GpuLight>);

// The bit layout (the HLSL twin's LL_* defines are these: light_frame_test.cpp compares them).
inline constexpr uint32_t kGpuKindShift = 0, kGpuKindMask = 0x3u;
inline constexpr uint32_t kGpuSourceShift = 2, kGpuSourceMask = 0x7u;
inline constexpr uint32_t kGpuLawShift = 5, kGpuLawMask = 0x3u;
inline constexpr uint32_t kGpuRolesShift = 7, kGpuRolesMask = 0xFu;
inline constexpr uint32_t kGpuFlagsShift = 16, kGpuFlagsMask = 0xFFFFu;
inline constexpr uint32_t kGpuFirstViewShift = 0, kGpuViewCountShift = 8, kGpuShadowKindShift = 16, kGpuMaskChannelShift = 24;

inline GpuLight PackGpuLight(const Light& l) {
  GpuLight g{};
  for (int i = 0; i < 3; ++i) {
    g.row0[i] = l.pos[i];
    g.row1[i] = l.dir[i];
    g.row2[i] = l.color[i];
  }
  g.row0[3] = l.range;
  g.row1[3] = l.cos_outer;
  g.row2[3] = l.cos_inner;
  g.packed0 = ((uint32_t(l.kind) & kGpuKindMask) << kGpuKindShift) | ((uint32_t(l.source) & kGpuSourceMask) << kGpuSourceShift) |
              ((uint32_t(l.law) & kGpuLawMask) << kGpuLawShift) | ((uint32_t(l.roles) & kGpuRolesMask) << kGpuRolesShift) |
              ((l.flags & kGpuFlagsMask) << kGpuFlagsShift);
  g.law_a = l.law_a;
  g.packed1 = (uint32_t(l.shadow.first_view) << kGpuFirstViewShift) | (uint32_t(l.shadow.view_count) << kGpuViewCountShift) |
              (uint32_t(l.shadow.kind) << kGpuShadowKindShift) | (uint32_t(l.shadow.mask_channel) << kGpuMaskChannelShift);
  g.shadow_weight = l.shadow.weight;
  return g;
}

// The light the GPU sees: every field GpuLight carries, the rest at its default (the round trip of light_frame_test.cpp).
inline Light UnpackGpuLight(const GpuLight& g) {
  Light l;
  for (int i = 0; i < 3; ++i) {
    l.pos[i] = g.row0[i];
    l.dir[i] = g.row1[i];
    l.color[i] = g.row2[i];
  }
  l.range = g.row0[3];
  l.cos_outer = g.row1[3];
  l.cos_inner = g.row2[3];
  l.kind = LightKind((g.packed0 >> kGpuKindShift) & kGpuKindMask);
  l.source = LightSource((g.packed0 >> kGpuSourceShift) & kGpuSourceMask);
  l.law = LightLaw((g.packed0 >> kGpuLawShift) & kGpuLawMask);
  l.roles = uint8_t((g.packed0 >> kGpuRolesShift) & kGpuRolesMask);
  l.flags = (g.packed0 >> kGpuFlagsShift) & kGpuFlagsMask;
  l.law_a = g.law_a;
  l.shadow.first_view = uint8_t((g.packed1 >> kGpuFirstViewShift) & 0xFFu);
  l.shadow.view_count = uint8_t((g.packed1 >> kGpuViewCountShift) & 0xFFu);
  l.shadow.kind = ShadowKind((g.packed1 >> kGpuShadowKindShift) & 0xFFu);
  l.shadow.mask_channel = uint8_t((g.packed1 >> kGpuMaskChannelShift) & 0xFFu);
  l.shadow.weight = g.shadow_weight;
  return l;
}

// The buffer a frame writes once at BeginFrame, readable by pixel AND compute stages: a 16 byte header and kMaxLights rows (the lights in slot order; the rows past `count`
// are zero, which is a light of no range).
struct alignas(16) GpuLightHeader {
  uint32_t count = 0;       // how many slots of the frame's `lights` are in use (the highest used slot + 1)
  uint32_t frame = 0;
  uint32_t generation = 0;  // as LightFrame::generation
  uint32_t reserved = 0;
};
struct GpuLightBuffer {
  GpuLightHeader header;
  std::array<GpuLight, kMaxLights> lights{};
};
static_assert(sizeof(GpuLightHeader) == 16 && sizeof(GpuLightBuffer) == 16 + 64 * kMaxLights && sizeof(GpuLightBuffer) % 16 == 0);
static_assert(offsetof(GpuLightBuffer, lights) == 16);

// Where the frame's GPU data is: the lights' buffer (a StructuredBuffer<GpuLight> over `lights_offset` bytes into the bindless buffer `lights_slot`, or an upload ring's span; the
// header first) and the shadow view table (kMaxLightShadowViews rows of the 96 byte LocalShadowView, `views_offset` bytes in). kNoLightSlot: not uploaded (the manager is off, or
// the frame is empty).
struct GpuLightHandles {
  uint32_t lights_slot = kNoLightSlot;
  uint32_t lights_offset = 0;
  uint32_t views_slot = kNoLightSlot;
  uint32_t views_offset = 0;
};

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

struct LightFrame {
  uint32_t frame = 0;       // the Scene's frame serial (low 32 bits)
  uint32_t generation = 0;  // changes when the budget or the atlas changes: drop what was cached from an older one
  std::array<Light, kMaxLights> lights{};
  uint32_t count = 0;       // slots in use: lights[0 .. count) (a slot whose `id` is kNoLightId is a hole)
  // Per role (LightRole: surface, volume, shadow) the slots, best first; a consumer takes a prefix of its role's list.
  std::array<std::array<uint8_t, kMaxLights>, kRoleCount> order{};
  // How many each role wants this frame: the budget's `wanted` plus the fading spares (so order[role][0 .. role_count[role]) are the lights to use).
  std::array<uint8_t, kRoleCount> role_count{};
  ShadowFrame shadow;
  std::array<CarLights, kMaxCars> cars{};
  uint32_t car_count = 0;
  GpuLightHandles gpu;

  // The light of rank `rank` in a role's ranking (the caller keeps rank < role_count).
  const Light& Ranked(LightRole role, uint32_t rank) const { return lights[order[uint32_t(role)][rank]]; }
  uint32_t RoleCount(LightRole role) const { return role_count[uint32_t(role)]; }
  // The slot of the light with this id, or -1 (a linear look: 64 slots at most).
  int32_t Find(uint64_t id) const {
    for (uint32_t i = 0; i < count && i < kMaxLights; ++i) {
      if (id != kNoLightId && lights[i].id == id) {
        return int32_t(i);
      }
    }
    return -1;
  }
};

}  // namespace larecomp::gpu
