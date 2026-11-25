#pragma once
// ===========================================================================
// MCLA GPU renderer -- the ray traced car scene's MATERIALS: what a hit on a car's surface is made of (RT-HIT-SHADING)
// ===========================================================================
// WHAT WAS WRONG. A reflection ray that hits ANOTHER car (the opponent beside the player's door) took the history's colour where the camera sees that part and,
// everywhere else (nearly always: the flank that faces the player's paint is hidden from the camera), a STAND-IN: the mean colour of the car around its body centre,
// made dark (hlsl/car_rt_refl_cs.hlsl OtherCarColour, car_rt_refl_rules.h FindOtherCarColour). The owner's words (2026-11-20): "just renders a blob in the shape of the
// other car all of a single color. Not a proper car render." This is the replacement: a hit on a car is SHADED from the car's own material.
//
// WHAT THE SCENE HAS (the survey, from the code of this branch; nothing was run)
//   - per mesh (car_rt_scene.cpp): positions (float3), normals (float3, from the declaration's NORMAL or smooth), triangle-list indices; keyed by a fingerprint of the draw's
//     own guest bytes (car_frame_rules.h MeshKeyOf) or, for a walked car, by the buffers' addresses (car_model_rt_rules.h GeometryKey). Every instance of a model shares
//     one bottom level structure. NO texture coordinates were kept (now: a half2 a vertex, in a shared buffer parallel to the normals, ReadTexcoords).
//   - per instance (the table, binding 1): the mesh's first index and vertex, the mask and one flag, and the draw's place or the car's record. NO material: the draw's
//     shader (Draw::ps / OtherDraw::ps, an identity) was kept for the mask and nothing else. For a car the camera does not draw (a remembered kit, a walked car) the draw's
//     shader is the kit's (KitDraw::ps), a walked part's is 0.
//   - the car's paint: another car's colour in the history, or a car-level record (OtherCar::paint, the customisation record's colour A raised to a gamma).
//   - the vertex format of a car: position float3 at offset 0, normal a packed 2_10_10_10 or float3, TEXCOORD0 a half2 (k_16_16_FLOAT, 0x002C235F; the walker's
//     declaration says type 1 = Half2 for the UV), strides 12..36 (perf/RT-MODEL-SURVEY.md 2.3).
//   - the textures a draw binds are the draw's fetch constants (DrawView::FetchGroup, six dwords a slot) and the bindless slots the scene made of them
//     (SharedConstants::texture_2d, the slot of fetch constant i at index i). The shader of each class reads its own fetch slots (read off the translated shaders,
//     shaderpack/baseline/hlsl-rpf/cars/*.hlsl, and the identity list perf/out/night-probe-ids.txt):
//       customisable paint (439B193A3F675533 PS_CarPaint, 0E00D4C21054B61F Env, 97E9746FBFAD389B NormalMapped): 0 dpFront (the 512x256 dual paraboloid environment map,
//         4 levels), 1 dpSpecFront (its 64x32 blur), 2 noise, 3 VINYL (the livery: rgba, over the paint where its alpha says so), 4 scrape (damage), 13 the sun shadow collector;
//         NO diffuse texture: the colour is lerp(DL3, DL2, (N.V)^DL1.w), DL2 and DL3 the pixel constants c102 and c103 (c104 and c105 for the plain xCarPaint), the
//         environment's fresnel DL1.y + (DL1.z - DL1.y) (1 - N.V)^DL1.x (c101).
//       chrome (2D8CA0430FF88ED7): 0 dpFront, 13 shadow. Glass (4594B239C3B63D42): 0, 1, detail mask 2, damage 3. Lights (CC710F7164C151DE): 0, 1, normal 2, ILLUM 3.
//       textured parts: DIFFUSE at fetch slot 2 (xBumpSpec, grills, calipers), 1 (interior trims, engine bay, underbody, rubber, badges, subframe), 0 (textured plastic).
//   - residency: a texture stays in the TextureCache while a draw names it, and for kEvictFrames (600) frames after (the budget's Trim may take an idle one sooner); a car the
//     camera does not draw is NOT kept alive by anything. So the reflection pass never binds a texture by a slot it remembers: the host asks the cache again every frame
//     (TextureCache::Peek2D: the slot of a host texture that EXISTS and is uploaded, or none; it creates, decodes and uploads nothing, and marks the texture used so that
//     the budget keeps it while a car beside the player needs it). A part whose texture is gone shades with its class colour.
//
// THE RECORD (the GPU rows, hlsl/car_rt_hit_shade.hlsli; the host: Material, MakeRows)
//   binding 4  ByteAddressBuffer         every mesh's texture coordinates, one uint a vertex (half2: u in the low half), the same ranges as the normals
//   binding 5  StructuredBuffer<uint4>   kFrameRows rows of the frame's lighting, then kMaterialRows rows an instance, in the instances' order
//   frame rows   0 xyz the direction to the sun in VIEW space (unit), w flags (kFrameShade, kFrameShadow, kFrameEnv, kFrameSun); 1 xyz the sun's colour (PS c39 of the
//                car's own draws, in the paint's units), w the sun shadow ray's reach in metres; 2 xyz the ambient (PS c26), w its gain; 3 x the environment map's slot (bit 31
//                the gamma bit), y the blurred map's slot, z the night factor (from the sun's height where the frame knows the sun, from the sun's colour where not: RT-HIT-SHADING-5), w the
//                specular gain; 4 xyz the sky ambient (PS c57 ambYPos), w the overall gain; 5, 6, 7 the WORLD x, y and z axes in view space (the environment map is in world axes; the pass
//                works in view space), row 5's w the hit's detail (RT-HIT-SHADING-5)
//   instance rows 0 x class | flags << 4 | fresnel min (8 bits) << 8 | fresnel max << 16 | fresnel power x 16 << 24, y the diffuse texture's bindless slot (0xFFFFFFFF none;
//                bit 31 the gamma bit), z paint colour A (R10G10B10, 0..2), w paint colour B; 1 x the paint's flop exponent (float bits), y the mip bias of the texture at
//                the mesh's texel density (float bits), z the emissive scale (float bits), w the glow's flags (bit 0, kRecordGlowDriven: the scale is the game's own lamps' for this lens, and the
//                night factor does not multiply it: car_rt_lights_rules.h, LU-8; 0: the scale is the lens class's 1 and the frame's night factor multiplies it)
//   class 0 (kNone) is the answer "this draw's shader is not one the table knows": the pass keeps today's stand-in for it. Every record is rebuilt every frame (a few hundred
//   instances x 32 bytes = 27 KiB), so a changed paint, a texture that came or went, a car that left cost nothing to keep right.
//
// THE SHADING (hlsl/car_rt_hit_shade.hlsli CarHitShade; ShadeReference below is its twin and the unit test's reference), for a hit the history does not show:
//   albedo   paint: lerp(B, A, (N.V)^flop), over by the livery (vinyl) where its alpha says so; textured parts: the diffuse texture (bilinear in two mip levels, the
//            level from the cone's footprint at the hit and the mesh's texel density, wrap addressing, the game's PWL gamma where the fetch asks) times the tint (A); the rest:
//            the tint, which is the class colour where the draw's constants give none
//   light    ambient x a hemisphere term (down-facing surfaces are darker) + the sun's colour x N.L x its visibility: the game's own sun cascades at the hit's world point
//            (SunCascades, CascadeLit: the shade of buildings, trees and bridges, with the collector's fade) and one ray towards the sun against the whole scene (reach kSunReach
//            metres: a car's own shadow on itself and on its neighbour, which the cascades do not hold), the darker of the two (SunVisibility; RT-HIT-SHADING part two). Where the
//            frame has no cascades (the volumetric light off, night, rain) the ray alone.
//   spec     the game's environment map read at the reflected direction (turned into WORLD axes: the paraboloid mapping of EnvUv, decoded from xChrome::PS_Chrome), at a mip
//            level of the class's gloss and the cone, times the fresnel (the paint's own DL1 numbers; the class's defaults otherwise) and the class's gain
//   emissive the lights' and the lit interior's own glow, scaled by the night factor (1 - the sun's brightness, between two thresholds)
//   units    everything is in the paint's units, and the pass multiplies it by 2^exp_bias (g_push.up.w) to be in the history's, as the paint colours the pass already made up
//   blend    where the camera sees the part the history's colour is the answer (exact); between, TraceRay's weight by the share of the hit the history shows
//            (colour = shaded + (seen - shaded) x share): no seam where the share moves, because the share is a smooth cover of the history's footprint
//
// FALLBACKS: a class 0 draw, a mesh with no texture coordinates, a texture that is not resident, a missing environment map (the ambient stands in for it), a device with no
// frame record -- each takes the next rule down, and the last is today's stand-in. Vulkan traces them exactly as the rest of the scene does (the bindings are
// the API-neutral descriptor builder's; the shader compiles to SPIR-V with the others).
//
// WHAT IT COSTS (an estimate, not measured: CHECK on the first run, PassTiming's "car rt refl" and "car rt refl opp"): only the HIDDEN hits on cars shade (the shader skips the
// work where the history shows the part), at most a few tens of thousands a frame at 4K (the cars beside the player are a tenth of the vehicle texels, 2 to 4 rays each); a
// shaded hit is the normal and the UV (6 loads), the material (2), the frame (8, uniform), a diffuse bilinear in two levels (8) and an environment bilinear in two levels (8),
// one shadow ray: about 25 loads and a ray, about 0.5 microsecond on an RTX class card, so 0.01 to 0.03 ms. The host: the table is 32 bytes an instance written per frame
// (a few hundred instances), the cache asked for the distinct textures of the frame (about 30), a texture coordinate read once a mesh (4 bytes a vertex: about 0.3 MiB a car).
// Memory: 4 bytes a vertex on top of 24 (the scene's cap counts 28), 27 KiB a frame slot for the records.
//
// THE TEST PLAN (the SHOWCASE-RT scenes: ports/scripts/scenes/showcase-rt-*, perf/out/showcase-rt; pairs of the same scene, day and night):
//   A  RT others off        mcla_gpu_car_selfrefl_rt_parts 7: the cars in the scene and out of the reflection: the paint reflects the environment only
//   B  today's stand-in     mcla_gpu_car_rt_hit_shading 0
//   C  shaded               mcla_gpu_car_rt_hit_shading 1 (the default)
//   and the vanilla: mcla_gpu_car_selfrefl 0. Look at: the neighbour's flank in the player's door (paint colour, the gradient of the flop, the glass dark and bright at the rim,
//   the tyres black, the chrome bright, the lamps lit at night); the seam where the history's colour meets the shaded one (a visible line across a panel: the gain
//   mcla_gpu_car_rt_hit_gain, the ambient mcla_gpu_car_rt_hit_ambient); a livery car; a traffic car (grey class colours: no paint record) and a walked car (class colours, paint).
//   Exclusive cost runs (nothing else on the GPU, lockrun): B against C at 4K DLAA, the passes' timings, 600 frames each, the scene with 4 cars beside the player.
//
// CHECK (nothing here was run; each is a line of the first run's log, "[car rt] materials ...", or a picture):
//   1. the pixel constants the table reads (c101, c102, c103 for the paint; c26, c39, c57 for the light): the log prints the first paint's A, B, fresnel and the frame's light
//      next to the customisation record's colours for the same car (the record's colour raised to the gamma should be near A or B);
//   2. the units of the environment map against the paint's (the pass multiplies the paint's units by 2^exp_bias; the map is read as the paint reads it, unscaled): if the
//      reflected flank is too bright or dark against the history's, mcla_gpu_car_rt_hit_gain moves it;
//   3. the paraboloid's axis and sign (EnvUv): a flank that reflects the sky upside down or the horizon on the wrong side;
//   4. the sun's colour at night (the night factor), and whether c39 holds the moon there;
//   5. the diffuse slots per shader (ShaderRule::diffuse_fetch) and the tint registers (Source), from the translated shaders, not from a run;
//   6. whether the vinyl's alpha is coverage (the paint draw's own math says so: r8.x = saturate(r5.z - r9.w) mixes the paint out where the vinyl is opaque) and whether
//      the livery is a resolve product the cache can hand out (Peek2D takes products too);
//   7. the mesh texture coordinates' format in the draws' declarations (half2 expected; a mesh with another format has no texture and shades with the class colour);
//   8. the cost, and the unresolved-texture count in the log (textures the cache no longer holds for a car that is only remembered).
//
// This header includes nothing of the renderer; car_rt_material_rules_test.cpp builds on its own.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "car_dump_rules.h"
#include "car_model_walk_rules.h"
#include "rt_hit_frame.h"

namespace mcla::gpu::car_rt_material {

// CITY-RT M2 H4: the pieces every hit shading shares moved to rt_hit_frame.h (the frame's rows, the cascades, the environment map's mapping, the packing, the shared numbers); they are named
// here again, unchanged, so that the car files and tests read what they always read.
using rt_hit::Bits;
using rt_hit::CascadeLit;
using rt_hit::CascadesUsable;
using rt_hit::Dot3;
using rt_hit::EnvDirection;
using rt_hit::EnvUv;
using rt_hit::FloatToHalf;
using rt_hit::FrameInput;
using rt_hit::FrameLight;
using rt_hit::FromBits;
using rt_hit::MakeFrameRows;
using rt_hit::NightFactor;
using rt_hit::NightFromSunHeight;
using rt_hit::PackRgb10;
using rt_hit::SunCascades;
using rt_hit::SunVisibility;
using rt_hit::Tuning;
using rt_hit::UnpackRgb10;
using rt_hit::WorldPoint;
using rt_hit::kCameraRow;
using rt_hit::kCascadeFirstRow;
using rt_hit::kCascadeHeaderRow;
using rt_hit::kCascadeNormalOffset;
using rt_hit::kCascadeRightRow;
using rt_hit::kCascadeRowsEach;
using rt_hit::kCascadeTopRow;
using rt_hit::kConeLodScale;
using rt_hit::kEmissiveGain;
using rt_hit::kFillShare;
using rt_hit::kFrameCascade;
using rt_hit::kFrameEnv;
using rt_hit::kFrameRows;
using rt_hit::kFrameShade;
using rt_hit::kFrameShadow;
using rt_hit::kFrameSun;
using rt_hit::kGammaBit;
using rt_hit::kMaxCascades;
using rt_hit::kNightHigh;
using rt_hit::kNightLow;
using rt_hit::kNightSunHigh;
using rt_hit::kNightSunLow;
using rt_hit::kNoSlot;
using rt_hit::kSunReach;
using rt_hit::kTintMax;

// ---------------------------------------------------------------------------
// classes
// ---------------------------------------------------------------------------

// hlsl/car_rt_hit_shade.hlsli CAR_MAT_CLASS_*.
enum class Class : uint8_t {
  kNone = 0,   // the draw's shader is not one the table knows: today's stand-in
  kPaint = 1,
  kGlass = 2,
  kTyre = 3,
  kChrome = 4,
  kLight = 5,
  kInterior = 6,
  kOther = 7,
};
inline constexpr uint32_t kClassCount = 8;

inline const char* ClassName(Class c) {
  switch (c) {
    case Class::kPaint: return "paint";
    case Class::kGlass: return "glass";
    case Class::kTyre: return "tyre";
    case Class::kChrome: return "chrome";
    case Class::kLight: return "light";
    case Class::kInterior: return "interior";
    case Class::kOther: return "other";
    default: return "none";
  }
}

// The numbers of a class when the draw's constants say nothing (the shader keeps its own copy, hlsl/car_rt_hit_shade.hlsli kClass*; change one, change the other):
// the fresnel of the environment's reflection (min, max, power), the mip level the reflection reads the environment at (a rough surface reads a blurrier level),
// the gain of the reflection and of the diffuse, and the class colour (a grey) where no constant and no texture gives one.
struct ClassDefaults {
  float fresnel_min;
  float fresnel_max;
  float fresnel_power;
  float env_lod;
  float spec_gain;
  float diffuse_gain;
  float grey;
};
inline constexpr std::array<ClassDefaults, kClassCount> kDefaults = {{
    /* none     */ {0.04f, 0.30f, 5.0f, 1.5f, 0.5f, 1.0f, 0.30f},
    /* paint    */ {0.05f, 0.60f, 4.0f, 0.5f, 1.0f, 1.0f, 0.40f},
    /* glass    */ {0.04f, 1.00f, 5.0f, 0.5f, 1.0f, 1.0f, 0.03f},
    /* tyre     */ {0.02f, 0.10f, 5.0f, 3.0f, 0.3f, 1.0f, 0.04f},
    /* chrome   */ {0.70f, 1.00f, 5.0f, 0.7f, 1.0f, 0.1f, 0.90f},
    /* light    */ {0.04f, 0.90f, 5.0f, 0.5f, 1.0f, 0.3f, 0.50f},
    /* interior */ {0.02f, 0.15f, 5.0f, 2.5f, 0.3f, 1.0f, 0.15f},
    /* other    */ {0.04f, 0.30f, 5.0f, 1.5f, 0.5f, 1.0f, 0.30f},
}};
inline const ClassDefaults& DefaultsOf(Class c) { return kDefaults[std::min<uint32_t>(uint32_t(c), kClassCount - 1)]; }

// RT-HIT-SHADING-5, by class (none, paint, glass, tyre, chrome, light, interior, other; the shader keeps its own copy, kClassAmbient and kClassGlint):
//   ambient  the share of the sky's ambient the class keeps: a tyre sits in its wheel arch, an interior or an underside (engine bay, subframe) is under the body, a grille and the other
//            textured parts are set back
//   glint    the share of the clear coat's mirror image of the sun and the lamps the class shows: the paint's coat, glass and chrome fully, a lamp's lens most, rubber and the interior's matt
//            trim not at all
inline constexpr std::array<float, kClassCount> kClassAmbient = {1.0f, 1.0f, 1.0f, 0.8f, 1.0f, 1.0f, 0.45f, 0.85f};
inline constexpr std::array<float, kClassCount> kClassGlint = {0.5f, 1.0f, 1.0f, 0.0f, 1.0f, 0.8f, 0.0f, 0.3f};
inline float ClassAmbient(Class c) { return kClassAmbient[std::min<uint32_t>(uint32_t(c), kClassCount - 1)]; }
inline float ClassGlint(Class c) { return kClassGlint[std::min<uint32_t>(uint32_t(c), kClassCount - 1)]; }

// The gamma a customisation record's colour is raised to for a walked car's paint (the paint's units; car_model_rt_rules.h kPaintGamma is the same number: the record's colour is
// the one the player picked, the shader's constants are the colour linear).
inline constexpr float kWalkedPaintGamma = 2.0f;

// ---------------------------------------------------------------------------
// RT-HIT-SHADING-5: the hit's own detail (the numbers; "the hit's own detail" below has the design and the functions, hlsl/car_rt_hit_shade.hlsli the shader's copy)
// ---------------------------------------------------------------------------
// At night the sky's fill is this share of the sun's colour (kFillShare by day): a city night's sky glows, and a flank the street lamps do not reach is not black.
inline constexpr float kNightFillShare = 0.4f;
// RT-HIT-SHADING-6: at night a street lamp over the street lights the hit as well (the glints below are its image in the coat; this is its light on the paint): a surface that faces the lamp,
// straight up, takes this share of the sun's colour times N.up, by the sky's openness (a hit under another car's roof is in its shade) and the frame's detail. A vertical flank has none of it, a roof, a
// bonnet or a shoulder has it: the night's flank was one even colour (the fill by a hemisphere term of 0.6 at every vertical normal), now its top is lit and its sill is not, so the car has a shape.
// 0 at day: the hour's night factor is the only thing that brings it in.
inline constexpr float kLampDiffuse = 0.6f;
// The clear coat's Schlick fresnel (a dielectric of the index 1.5: F0 0.04, power 5) at the hit's own view of the reflected ray; chrome keeps its own (high) fresnel.
inline constexpr float kCoatF0 = 0.04f;
inline constexpr float kCoatPower = 5.0f;
// The coat's mirror image of the sun and of a street lamp: the light's colour (the sun's, c39) times this radiance times the lobe pow(cos(angle between the reflected ray and the light),
// power) times the fresnel. A light's image in a mirror is as bright as the light, which is far brighter than a surface it lights (the sun's disc is ~10^5 times a lit white wall's
// radiance; the units here hold a few tens of it and the picture's own exposure clips the rest). The sun's lobe is a few degrees wide (the coat is smooth), a lamp's wider (a lamp is
// a near area light high over the street, and there are several).
inline constexpr float kSunRadiance = 24.0f;
inline constexpr float kSunGlintPower = 160.0f;
inline constexpr float kLampRadiance = 40.0f;
inline constexpr float kLampGlintPower = 14.0f;
// The sky's openness at the hit (one ray towards the sky, kOpenReach metres, against every car of the scene): the share of the ambient a hit keeps where it is closed in (between two cars,
// under a sill) is kOpenMin, and 1 where the sky is open. The ray's direction is normal + up spread over a cone of tangent kOpenJitter, kept kOpenMinAbove above the tangent plane.
inline constexpr float kOpenMin = 0.45f;
inline constexpr float kOpenReach = 2.5f;
inline constexpr float kOpenJitter = 0.35f;
inline constexpr float kOpenMinAbove = 0.2f;
// The grid (cells a metre) of the hash that spreads the sky ray over its cone: the hit's place in view space.
inline constexpr float kOpenHashCells = 100.0f;
// The contact darkening: from the ground up to kContactHeight metres the ambient and the reflection fall to kContactMin of themselves (the sill, the lower lip of a bumper, a tyre's
// contact patch: the ground and the car's own underside shade them). The hit's height above ITS car's ground is taken from the car's record (car_rt_instances_rules.h CarRow: the
// centre is the mean of the car's draws' origins raised kCentreRise = 0.6, which the shader's kCarCentreRise repeats; the origins are on the road).
inline constexpr float kContactMin = 0.35f;
inline constexpr float kContactHeight = 0.5f;
inline constexpr float kCarCentreRise = 0.6f;

// ---------------------------------------------------------------------------
// the shaders of the cars' draws, by identity
// ---------------------------------------------------------------------------

// Where a draw's constants hold its base colour (ps = the pixel bank, registers as the translated shaders' packoffsets).
enum class Source : uint8_t {
  kNone,         // no colour in the constants: the class's grey
  kPaintCustom,  // xCarPaintCustomizable: A = c102 (DL2), B = c103 (DL3), fresnel (power, min, max) = c101.xyz (DL1), flop = c101.w
  kPaintPlain,   // xCarPaint: A = c104, B = c105, fresnel = c103.xyz, flop = c103.w
  kBaseColour,   // xBumpSpec: baseColor = c100 (the diffuse's multiplier)
};

struct ShaderRule {
  uint64_t ps;
  Class cls;
  int8_t diffuse_fetch;  // the fetch slot of the albedo texture (the paint's: the vinyl); -1: none
  Source source;
  float grey;            // the class colour of this shader where the constants give none (0: the class's)
  uint8_t emissive;      // 1: the shader glows (the lit interior, the lamps' illumination map)
  uint8_t env;           // 1: fetch slot 0 is the sharp environment map and 1 its blur; 2: slot 0 alone; 0: neither (the shader reads other textures there)
};

inline constexpr uint32_t kVinylFetch = 3;

// The identities are the runtime's own (FNV-1a 64 of the microcode, perf/out/night-probe-ids.txt, shaderpack/baseline/hlsl-rpf/cars): the car shaders of the main pass.
inline constexpr ShaderRule kShaders[] = {
    // paint
    {0x439B193A3F675533ull, Class::kPaint, int8_t(kVinylFetch), Source::kPaintCustom, 0.0f, 0, 1},   // xCarPaintCustomizable::PS_CarPaint
    {0x0E00D4C21054B61Full, Class::kPaint, int8_t(kVinylFetch), Source::kPaintCustom, 0.0f, 0, 1},   // ... PS_CarPaintEnv
    {0x97E9746FBFAD389Bull, Class::kPaint, int8_t(kVinylFetch), Source::kPaintCustom, 0.0f, 0, 1},   // xCarPaintNormalMapped
    {0x1C3CD52A0C7D098Dull, Class::kPaint, int8_t(kVinylFetch), Source::kPaintPlain, 0.0f, 0, 1},    // xCarPaint::PS_CarPaint
    {0x98B440946246749Cull, Class::kPaint, int8_t(kVinylFetch), Source::kNone, 0.0f, 0, 1},          // xCarPaint::PS_CarPaintLowLod
    {0x9A6BE351ED46662Dull, Class::kPaint, -1, Source::kNone, 0.0f, 0, 1},                           // xAmbientCarPaint (traffic)
    {0x1D816E0FF96F3A96ull, Class::kOther, -1, Source::kNone, 0.04f, 0, 0},                          // the hood's carbon fibre
    {0x671ACC476AD15C70ull, Class::kOther, -1, Source::kNone, 0.04f, 0, 0},                          // xCarbonFiber
    // chrome and metal
    {0x2D8CA0430FF88ED7ull, Class::kChrome, -1, Source::kNone, 0.9f, 0, 2},                          // xChrome::PS_Chrome
    {0x01A189CAD7C67F3Dull, Class::kChrome, -1, Source::kNone, 0.9f, 0, 2},                          // xBumpedAlphaChrome
    {0xC979F31ED88E3733ull, Class::kChrome, -1, Source::kNone, 0.8f, 0, 0},                          // xAnisotropicBilletMetal
    {0xE73C5CBD49916AC5ull, Class::kChrome, -1, Source::kNone, 0.7f, 0, 0},                          // xBumpedBrushedMetal
    {0x227F3ADC394FAC7Cull, Class::kChrome, -1, Source::kNone, 0.5f, 0, 0},                          // xBrushedMetal::PS_BrakeRotor
    {0x282CBC6535F9FFE0ull, Class::kChrome, -1, Source::kNone, 0.5f, 0, 0},                          // xWheelBrakeRotor
    {0xFEA1FE53978F67DBull, Class::kChrome, -1, Source::kNone, 0.7f, 0, 1},                          // xRimMain::PS_Multi
    {0xDD36871AE5C3D6B1ull, Class::kChrome, -1, Source::kNone, 0.7f, 0, 1},                          // xRimNormalMapped
    // glass
    {0x4594B239C3B63D42ull, Class::kGlass, -1, Source::kNone, 0.0f, 0, 1},                           // xCarGlass::PS_GlassFront
    {0x67F040F3F685FD68ull, Class::kGlass, -1, Source::kNone, 0.01f, 0, 2},                          // xBlackOpaqueGlass
    {0x6C1A3BE6B0509155ull, Class::kGlass, -1, Source::kNone, 0.0f, 0, 1},                           // xAmbientCarGlass
    {0x7540C66A0992F547ull, Class::kGlass, -1, Source::kNone, 0.0f, 0, 0},                           // xbumpedglass::PS_Light
    {0xFD705246ABE541BCull, Class::kGlass, -1, Source::kNone, 0.0f, 0, 0},                           // xCarGlass::PS_InteriorGlass
    {0x4ECA6B7EB23F204Cull, Class::kGlass, -1, Source::kNone, 0.0f, 0, 0},                           // xCarGlass::PS_InteriorGrime
    // lights
    {0xCC710F7164C151DEull, Class::kLight, 3, Source::kNone, 0.0f, 1, 1},                            // xCarLight::PS_Light (the illumination map at 3)
    {0x25E3BB37714FA645ull, Class::kLight, -1, Source::kNone, 0.0f, 1, 0},                           // xAmbientCarLight
    // tyres
    {0xA02CCC061B63E702ull, Class::kTyre, 1, Source::kNone, 0.0f, 0, 0},                             // xWheelRubber (true)
    {0xC23C6801E808AA66ull, Class::kTyre, 1, Source::kNone, 0.0f, 0, 0},                             // xWheelRubber (false)
    {0x937BE0C2E37D96EBull, Class::kTyre, 0, Source::kNone, 0.0f, 0, 0},                             // xAmbientTire
    // the interior and the underside
    {0xDBEFD3025AE7FBF3ull, Class::kInterior, 1, Source::kNone, 0.0f, 0, 0},                         // xInteriorTrim
    {0x319C349F0E80A3BFull, Class::kInterior, 1, Source::kNone, 0.0f, 0, 0},                         // xInteriorTrimAlpha
    {0xF3BDDFE587757966ull, Class::kInterior, 1, Source::kNone, 0.0f, 1, 0},                         // xInteriorTexturedEmissive
    {0xFEF70CD74462696Full, Class::kInterior, 1, Source::kNone, 0.0f, 1, 0},                         // xInteriorTexturedEmissiveAlpha
    {0x95CF3B9D2E138E64ull, Class::kInterior, 1, Source::kNone, 0.0f, 0, 0},                         // xEngineBay
    {0xB4B8F4758C254A51ull, Class::kInterior, 1, Source::kNone, 0.0f, 0, 0},                         // xUnderbody
    {0x4BD3EC5B94BF20FBull, Class::kInterior, 1, Source::kNone, 0.0f, 0, 0},                         // xBumperSubframe
    // textured parts
    {0x611497E6F65FED14ull, Class::kOther, 2, Source::kBaseColour, 0.0f, 0, 1},                      // xBumpSpec
    {0x8F31933E41A20C72ull, Class::kOther, 2, Source::kNone, 0.0f, 0, 1},                            // xBumpSpecAlpha / xGrillOpaque / xGrill (true)
    {0xD503874C5169957Eull, Class::kOther, 2, Source::kNone, 0.0f, 0, 1},                            // ... (false)
    {0xE2840C92C4F972E5ull, Class::kOther, 2, Source::kNone, 0.0f, 0, 1},                            // xGrill spec map
    {0x3B7A83D2E9BB0ADBull, Class::kOther, 0, Source::kNone, 0.0f, 0, 0},                            // xTexturedPlastic
    {0x54B4603EB343B00Aull, Class::kOther, 2, Source::kNone, 0.0f, 0, 1},                            // xbrakecaliper
    {0x7131548A98EBAB3Bull, Class::kOther, 2, Source::kNone, 0.0f, 0, 1},                            // xAmbientBumpSpec (traffic)
    {0xCAAD03E622169A59ull, Class::kOther, 1, Source::kNone, 0.0f, 0, 0},                            // xRimBadge
    {0xDECB07B5DF61B200ull, Class::kOther, 0, Source::kNone, 0.0f, 0, 0},                            // xLicensePlate (the plate's background)
    {0x89407CF17D1F661Eull, Class::kOther, 1, Source::kNone, 0.0f, 0, 0},                            // xBumpSpecSkinned
    {0xED8E9C0C93BF122Cull, Class::kOther, -1, Source::kNone, 0.01f, 0, 0},                          // xblackmatte
    {0xF4A36E05B01E2ADFull, Class::kOther, -1, Source::kNone, 0.01f, 0, 0},                          // xAmbientBlackMatte
    {0x62AAACC73460BCB0ull, Class::kOther, -1, Source::kNone, 0.0f, 0, 0},                           // xAmbientVehicle
};
inline constexpr size_t kShaderCount = sizeof(kShaders) / sizeof(kShaders[0]);

// A hash index over the table (open addressing, built once): every vehicle draw of a frame asks, and a few hundred of them is no place for a linear scan.
class ShaderIndex {
 public:
  ShaderIndex() {
    slots_.fill(nullptr);
    for (size_t i = 0; i < kShaderCount; ++i) {
      size_t at = Home(kShaders[i].ps);
      while (slots_[at] != nullptr) {
        at = (at + 1) % kSlots;
      }
      slots_[at] = &kShaders[i];
    }
  }
  const ShaderRule* Find(uint64_t ps) const {
    size_t at = Home(ps);
    for (size_t probes = 0; probes < kSlots && slots_[at] != nullptr; ++probes) {
      if (slots_[at]->ps == ps) {
        return slots_[at];
      }
      at = (at + 1) % kSlots;
    }
    return nullptr;
  }

 private:
  static constexpr size_t kSlots = 256;
  static_assert(kShaderCount * 2 <= kSlots, "the index stays under half full");
  static size_t Home(uint64_t ps) { return size_t(((ps ^ (ps >> 29) ^ (ps >> 47)) * 0x9E3779B97F4A7C15ull) >> 56) % kSlots; }
  std::array<const ShaderRule*, kSlots> slots_;
};

// The rule of a pixel shader identity, or null where the table does not know it (the identity 0 is no shader).
inline const ShaderRule* FindShader(uint64_t ps_identity) {
  static const ShaderIndex index;
  return ps_identity == 0 ? nullptr : index.Find(ps_identity);
}

// ---------------------------------------------------------------------------
// a draw's material, as the host holds it
// ---------------------------------------------------------------------------

// Flags of a material and of a record (the row's bits 4..7 of word x).
inline constexpr uint8_t kFlagPaint = 1;     // the tints are the paint's gradient: A where the surface faces the camera, B at the rim (the flop's exponent)
inline constexpr uint8_t kFlagDecal = 2;     // the diffuse texture is a decal (the vinyl): its alpha says how much of it covers the paint
inline constexpr uint8_t kFlagUv = 4;        // the mesh has texture coordinates (the host sets it from the mesh)
inline constexpr uint8_t kFlagEmissive = 8;  // the surface glows (scaled by the night factor)

// A texture fetch as the host knows it: the six dwords of the fetch constant (what the cache's key is made of) and the size (for the mip level).
struct FetchInfo {
  bool texture = false;
  uint32_t dwords[6] = {};
  uint16_t width = 0;
  uint16_t height = 0;
  bool operator==(const FetchInfo& o) const { return texture == o.texture && std::memcmp(dwords, o.dwords, sizeof(dwords)) == 0; }
};

struct Material {
  Class cls = Class::kNone;
  uint8_t flags = 0;
  float tint_a[3] = {1.0f, 1.0f, 1.0f};  // the paint's colour A (or the diffuse's multiplier, or the class colour), the paint's units
  float tint_b[3] = {1.0f, 1.0f, 1.0f};
  bool has_fresnel = false;
  float fresnel[3] = {};                 // min, max, power (the paint's DL1): has_fresnel false: the class's
  float flop = 0.0f;                     // the paint's exponent of N.V between A and B (0: A everywhere)
  float emissive = 0.0f;                 // scale of the glow at night
  uint8_t env = 0;                       // ShaderRule::env: the draw's fetch slots 0 (and 1) are the environment map (the frame's light takes the slots from such a draw)
  float grey = 0.0f;                     // > 0: the tints are 1 (the diffuse texture gives the colour) and this grey is the surface's colour where the texture is not resident (MakeRows)
  FetchInfo diffuse;                     // the texture, where the draw has one the table names
  bool Known() const { return cls != Class::kNone; }
};

// What the draw's own constants say, from the pixel bank (raw guest bytes: 256 registers of four big-endian floats).
inline bool ReadRegister(const uint8_t* bank, uint32_t reg, float out[4]) {
  if (!bank || reg >= car_dump::kRegisterCount) {
    return false;
  }
  for (uint32_t c = 0; c < 4; ++c) {
    out[c] = car_dump::BankFloat(bank, reg, c);
    if (!std::isfinite(out[c])) {
      return false;
    }
  }
  return true;
}

inline float ClampTint(float v) { return std::clamp(std::isfinite(v) ? v : 0.0f, 0.0f, kTintMax); }

// The material of a draw: `ps_identity` and the pixel bank say the class and the colours, `fetch(slot)` (a callable returning FetchInfo) the diffuse texture's fetch constant.
template <class FetchFn>
inline Material Capture(uint64_t ps_identity, const uint8_t* ps_bank, FetchFn&& fetch) {
  Material m;
  const ShaderRule* rule = FindShader(ps_identity);
  if (!rule || rule->cls == Class::kNone) {
    return m;
  }
  m.cls = rule->cls;
  m.env = rule->env;
  const ClassDefaults& defaults = DefaultsOf(m.cls);
  const float grey = rule->grey > 0.0f ? rule->grey : defaults.grey;
  for (int c = 0; c < 3; ++c) {
    m.tint_a[c] = m.tint_b[c] = grey;
  }
  float a[4], b[4], f[4];
  switch (rule->source) {
    case Source::kPaintCustom:
      if (ReadRegister(ps_bank, 102, a) && ReadRegister(ps_bank, 103, b) && ReadRegister(ps_bank, 101, f)) {
        m.flags |= kFlagPaint;
        for (int c = 0; c < 3; ++c) {
          m.tint_a[c] = ClampTint(a[c]);
          m.tint_b[c] = ClampTint(b[c]);
          m.fresnel[c] = f[c == 0 ? 1 : c == 1 ? 2 : 0];  // (min, max, power) from DL1's (power, min, max)
        }
        m.has_fresnel = true;
        m.flop = std::clamp(f[3], 0.0f, 16.0f);
      }
      break;
    case Source::kPaintPlain:
      if (ReadRegister(ps_bank, 104, a) && ReadRegister(ps_bank, 105, b) && ReadRegister(ps_bank, 103, f)) {
        m.flags |= kFlagPaint;
        for (int c = 0; c < 3; ++c) {
          m.tint_a[c] = ClampTint(a[c]);
          m.tint_b[c] = ClampTint(b[c]);
          m.fresnel[c] = f[c == 0 ? 1 : c == 1 ? 2 : 0];
        }
        m.has_fresnel = true;
        m.flop = std::clamp(f[3], 0.0f, 16.0f);
      }
      break;
    case Source::kBaseColour:
      if (ReadRegister(ps_bank, 100, a)) {
        for (int c = 0; c < 3; ++c) {
          m.tint_a[c] = m.tint_b[c] = ClampTint(a[c]);
        }
      }
      break;
    case Source::kNone:
      break;
  }
  if (m.has_fresnel) {
    // A constant that is no fresnel (a min above the max, a power over 15) is not trusted: the class's numbers stand.
    if (!(m.fresnel[0] >= 0.0f && m.fresnel[1] <= 1.0f && m.fresnel[0] <= m.fresnel[1] && m.fresnel[2] >= 0.0f && m.fresnel[2] < 15.9f)) {
      m.has_fresnel = false;
    }
  }
  if ((rule->emissive != 0)) {
    m.flags |= kFlagEmissive;
    m.emissive = 1.0f;
  }
  if (rule->diffuse_fetch >= 0) {
    const FetchInfo info = fetch(uint32_t(rule->diffuse_fetch));
    if (info.texture) {
      m.diffuse = info;
      if (m.cls == Class::kPaint) {
        m.flags |= kFlagDecal;
      } else if (rule->source == Source::kNone) {
        // The texture is the surface's colour: the class's grey would darken it twice. The grey is what the surface shows where the texture is not resident.
        m.grey = grey;
        for (int c = 0; c < 3; ++c) {
          m.tint_a[c] = m.tint_b[c] = 1.0f;
        }
      }
    }
  }
  return m;
}

// ---------------------------------------------------------------------------
// a car the camera does not draw: the class of a part of its model data
// ---------------------------------------------------------------------------

// The class of a walked geometry by its part slot (car_model_walk_rules.h: the 55 part slots in the order of the game's table, then the body, the tyre, the rim and a traffic
// car's whole model). The model data has no shader to ask, so this is the part's usual material.
inline Class ClassOfPart(uint32_t part) {
  namespace cw = car_model_walk;
  switch (part) {
    case cw::kBodyPart: return Class::kPaint;
    case cw::kTyrePart: return Class::kTyre;
    case cw::kRimPart: return Class::kChrome;
    case cw::kModelPart: return Class::kPaint;
    case 11: case 12: case 13: return Class::kLight;                        // headlight, taillight, taillight_b
    case 16: case 23: case 24: return Class::kChrome;                       // blower, the exhausts
    case 14: case 15: case 5: return Class::kOther;                         // front grill, wheelie bar, intercooler
    default: break;
  }
  if (part >= 44 && part <= 53) {
    return Class::kInterior;                                                // interior, seats, door interiors, stereo, steering wheel, gauge
  }
  if (part >= 25 && part <= 37) {
    return Class::kOther;                                                   // arms, shocks, guides, axle, sub-bumpers: the underside's dark metal
  }
  if (part == 54) {
    return Class::kOther;                                                   // the occluder
  }
  return Class::kPaint;                                                     // the hood, the bumpers, the fenders, the doors, the spoilers, the wide body, the top
}

// The material of a walked part: its class, and the paint's colours (the customisation record's colour A and B raised to `gamma`, the paint's units) for a paint part.
inline Material WalkedMaterial(uint32_t part, bool has_paint, const float paint_a[3], const float paint_b[3], float gamma) {
  Material m;
  m.cls = ClassOfPart(part);
  const ClassDefaults& defaults = DefaultsOf(m.cls);
  for (int c = 0; c < 3; ++c) {
    m.tint_a[c] = m.tint_b[c] = defaults.grey;
  }
  if (m.cls == Class::kPaint && has_paint && paint_a && paint_b) {
    m.flags |= kFlagPaint;
    for (int c = 0; c < 3; ++c) {
      m.tint_a[c] = ClampTint(std::pow(std::clamp(paint_a[c], 0.0f, 1.0f), gamma));
      m.tint_b[c] = ClampTint(std::pow(std::clamp(paint_b[c], 0.0f, 1.0f), gamma));
    }
  }
  if (m.cls == Class::kLight) {
    m.flags |= kFlagEmissive;
    m.emissive = 1.0f;
  }
  return m;
}

// ---------------------------------------------------------------------------
// packing: half floats, colours, the records
// ---------------------------------------------------------------------------
inline uint32_t PackUv(float u, float v) { return uint32_t(FloatToHalf(u)) | (uint32_t(FloatToHalf(v)) << 16); }
inline void UnpackUv(uint32_t packed, float* u, float* v) {
  *u = car_dump::HalfToFloat(uint16_t(packed & 0xFFFFu));
  *v = car_dump::HalfToFloat(uint16_t(packed >> 16));
}


inline constexpr uint32_t kMaterialRows = 2;
// Word w of an instance's second row: bit 0 says the emissive scale in word z is the game's lamps' (car_rt_lights_rules.h kGlowDrivenBit): no night factor on it.
inline constexpr uint32_t kRecordGlowDriven = 1u;

// What the host adds to a draw's material for one instance: the mesh's texture coordinates and their density.
struct InstanceMaterial {
  Material material;
  bool has_uv = false;
  float uv_density = 0.0f;  // texture units a metre of the mesh (MeshUvDensity)
  // LU-8 (car_rt_lights_rules.h): a lens whose lamps the game's light points say. `glow_driven`: `glow` (0..1) replaces the material's emissive scale and the night factor does not multiply it.
  bool glow_driven = false;
  float glow = 0.0f;
};

inline uint32_t Byte01(float v, float scale) { return uint32_t(std::clamp(std::lround(std::clamp(v, 0.0f, scale) / scale * 255.0f), 0L, 255L)); }

// The mip bias of a texture at a mesh's density: log2 of the texels a metre of the mesh has; the level to read is this plus log2 of the footprint in metres.
inline float LodBase(float uv_density, uint32_t texture_edge) {
  if (!(uv_density > 0.0f) || texture_edge == 0) {
    return 0.0f;
  }
  return std::log2(std::max(uv_density, 1e-6f) * float(texture_edge));
}

// The record of one instance (kMaterialRows rows of four words). `slot` is the diffuse's bindless slot as the cache gave it (kNoSlot: none, or not resident).
inline void MakeRows(const InstanceMaterial& im, uint32_t slot, uint32_t rows[kMaterialRows][4]) {
  const Material& m = im.material;
  uint8_t flags = m.flags;
  if (im.has_uv) {
    flags |= kFlagUv;
  }
  const ClassDefaults& defaults = DefaultsOf(m.cls);
  const float fmin = m.has_fresnel ? m.fresnel[0] : defaults.fresnel_min;
  const float fmax = m.has_fresnel ? m.fresnel[1] : defaults.fresnel_max;
  const float fpow = m.has_fresnel ? m.fresnel[2] : defaults.fresnel_power;
  rows[0][0] = uint32_t(m.cls) | (uint32_t(flags & 0xFu) << 4) | (Byte01(fmin, 1.0f) << 8) | (Byte01(fmax, 1.0f) << 16) |
               (uint32_t(std::clamp(std::lround(std::clamp(fpow, 0.0f, 15.9f) * 16.0f), 0L, 255L)) << 24);
  rows[0][1] = slot;
  float tint_a[3] = {m.tint_a[0], m.tint_a[1], m.tint_a[2]};
  float tint_b[3] = {m.tint_b[0], m.tint_b[1], m.tint_b[2]};
  if (m.grey > 0.0f && slot == kNoSlot) {
    // a textured surface whose texture the cache does not hold shows the class's colour
    for (int c = 0; c < 3; ++c) {
      tint_a[c] = tint_b[c] = m.grey;
    }
  }
  rows[0][2] = PackRgb10(tint_a);
  rows[0][3] = PackRgb10(tint_b);
  const uint32_t edge = std::max<uint32_t>(m.diffuse.width, m.diffuse.height);
  rows[1][0] = Bits(m.flop);
  rows[1][1] = Bits(slot != kNoSlot && im.has_uv ? LodBase(im.uv_density, edge) : 0.0f);
  rows[1][2] = Bits(im.glow_driven ? std::clamp(im.glow, 0.0f, 1.0f) : m.emissive);
  rows[1][3] = im.glow_driven ? kRecordGlowDriven : 0u;
}

// The record read back (what the shader decodes): for the unit test.
struct RowsDecoded {
  Class cls = Class::kNone;
  uint8_t flags = 0;
  float fresnel_min = 0.0f, fresnel_max = 0.0f, fresnel_power = 0.0f;
  uint32_t slot = kNoSlot;
  float tint_a[3] = {}, tint_b[3] = {};
  float flop = 0.0f, lod_base = 0.0f, emissive = 0.0f;
  bool glow_driven = false;
};
inline RowsDecoded DecodeRows(const uint32_t rows[kMaterialRows][4]) {
  RowsDecoded d;
  d.cls = Class(rows[0][0] & 0xFu);
  d.flags = uint8_t((rows[0][0] >> 4) & 0xFu);
  d.fresnel_min = float((rows[0][0] >> 8) & 0xFFu) / 255.0f;
  d.fresnel_max = float((rows[0][0] >> 16) & 0xFFu) / 255.0f;
  d.fresnel_power = float((rows[0][0] >> 24) & 0xFFu) / 16.0f;
  d.slot = rows[0][1];
  UnpackRgb10(rows[0][2], d.tint_a);
  UnpackRgb10(rows[0][3], d.tint_b);
  d.flop = FromBits(rows[1][0]);
  d.lod_base = FromBits(rows[1][1]);
  d.emissive = FromBits(rows[1][2]);
  d.glow_driven = (rows[1][3] & kRecordGlowDriven) != 0;
  return d;
}


// ---------------------------------------------------------------------------
// texture coordinates
// ---------------------------------------------------------------------------

// The formats a TEXCOORD0 of a car's declaration comes in (the packed Xenon decltype, car_dump::PositionFormatOf's table): a half2 or a float2; the wider ones' first two components.
inline bool UvFormat(uint32_t packed_decltype, car_dump::PositionFormat* format) {
  const car_dump::PositionFormat f = car_dump::PositionFormatOf(packed_decltype);
  if (f == car_dump::PositionFormat::kHalf2 || f == car_dump::PositionFormat::kFloat2 || f == car_dump::PositionFormat::kFloat3 || f == car_dump::PositionFormat::kHalf4 ||
      f == car_dump::PositionFormat::kFloat4) {
    *format = f;
    return true;
  }
  return false;
}

// The (u, v) at `be` (the guest's bytes of the element). False for a format that is no texture coordinate or a value that is not finite.
inline bool DecodeUv(const uint8_t* be, car_dump::PositionFormat format, float* u, float* v) {
  switch (format) {
    case car_dump::PositionFormat::kHalf2:
    case car_dump::PositionFormat::kHalf4:
      *u = car_dump::HalfToFloat(car_dump::LoadBe16(be));
      *v = car_dump::HalfToFloat(car_dump::LoadBe16(be + 2));
      break;
    case car_dump::PositionFormat::kFloat2:
    case car_dump::PositionFormat::kFloat3:
    case car_dump::PositionFormat::kFloat4: {
      const uint32_t a = car_dump::LoadBe32(be);
      const uint32_t b = car_dump::LoadBe32(be + 4);
      std::memcpy(u, &a, 4);
      std::memcpy(v, &b, 4);
      break;
    }
    default:
      return false;
  }
  return std::isfinite(*u) && std::isfinite(*v);
}

// The bytes an element of `format` takes in a vertex.
inline uint32_t UvBytes(car_dump::PositionFormat format) { return car_dump::PositionBytes(format); }

// The texel density of a mesh: the square root of the texture area over the world area of its triangles, summed (so a few huge triangles do not decide it): texture units a
// metre. 0 when the mesh has no texture coordinates or no area. `positions` x y z a vertex, `uvs` u v a vertex, `indices` three a triangle.
inline float MeshUvDensity(const std::vector<float>& positions, const std::vector<float>& uvs, const std::vector<uint32_t>& indices) {
  const size_t vertices = positions.size() / 3;
  if (vertices == 0 || uvs.size() != vertices * 2) {
    return 0.0f;
  }
  double world = 0.0, texture = 0.0;
  for (size_t t = 0; t + 3 <= indices.size(); t += 3) {
    const uint32_t a = indices[t], b = indices[t + 1], c = indices[t + 2];
    if (a >= vertices || b >= vertices || c >= vertices) {
      continue;
    }
    double e1[3], e2[3];
    for (int k = 0; k < 3; ++k) {
      e1[k] = double(positions[size_t(b) * 3 + k]) - double(positions[size_t(a) * 3 + k]);
      e2[k] = double(positions[size_t(c) * 3 + k]) - double(positions[size_t(a) * 3 + k]);
    }
    const double cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2], cz = e1[0] * e2[1] - e1[1] * e2[0];
    const double area = 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
    const double u1 = double(uvs[size_t(b) * 2]) - double(uvs[size_t(a) * 2]), v1 = double(uvs[size_t(b) * 2 + 1]) - double(uvs[size_t(a) * 2 + 1]);
    const double u2 = double(uvs[size_t(c) * 2]) - double(uvs[size_t(a) * 2]), v2 = double(uvs[size_t(c) * 2 + 1]) - double(uvs[size_t(a) * 2 + 1]);
    const double uv_area = 0.5 * std::fabs(u1 * v2 - u2 * v1);
    if (!std::isfinite(area) || !std::isfinite(uv_area)) {
      continue;
    }
    world += area;
    texture += uv_area;
  }
  if (!(world > 1e-12) || !(texture > 0.0)) {
    return 0.0f;
  }
  return float(std::clamp(std::sqrt(texture / world), 1e-4, 1e5));
}

// The mip level (as a float: the shader blends two levels) of a texture at a hit: the footprint's size in texels, from the cone's width in metres, the mesh's density and the
// texture's larger edge, clamped to the texture's levels.
inline float FootprintLod(float footprint_metres, float lod_base, uint32_t levels) {
  const float lod = std::log2(std::max(footprint_metres, 1e-4f)) + lod_base;
  return std::clamp(lod, 0.0f, float(std::max<uint32_t>(levels, 1) - 1u));
}

// ---------------------------------------------------------------------------
// RT-HIT-SHADING-5: the hit's own detail
// ---------------------------------------------------------------------------
// WHAT THE rthit3 PICTURES SHOWED (perf\out\SHOWCASE-RT.txt, showcase-rt-day-b-zoom.png): the neighbour reads in the black door now (a sharp 9.9 px edge by day, the colour right, 1.53 times the
// environment's brightness) but it is FLAT, a uniform orange with a dark window strip: the contrast ratio (the coefficient of variation of the luminance over the interior of the pixels the
// neighbour changes, against the environment's reflection it replaced) is 0.75 by day and 0.24 at night, wanted at least 1. The reflected car's hidden flank was albedo x one light, so over a
// flank the only variation was the flop's gradient and the hemisphere term (0.6 at every vertical normal). The environment's reflection it replaced has the buildings, the sky, and at night
// the street lamp's glint (a coefficient of variation of 1.05 by day, 1.81 at night). The reflected car needs light and shade of its own. What this section adds, all of it to a hit on another
// car the history does not show (so the part the camera sees keeps the game's own colour), each by the class of the material, all scaled by Tuning::detail (mcla_gpu_car_rt_hit_detail: 0 is the
// shading as it was):
//   1. THE SKY'S OPENNESS. One ray from the hit towards the sky (normal + up, jittered over a cone of tangent kOpenJitter by a hash of the hit's place), kOpenReach metres, against every car of the
//      scene: where it meets a car the hit is closed in (the flank between two cars below the neighbour's roof, the underside of a sill) and keeps kOpenMin of its ambient and its reflection,
//      where it meets nothing the sky is open and the hit keeps all of it (OpenMultiplier). The edge of the neighbour's shade on a flank follows the neighbour's own shape, so the player's
//      roof line is a gradient on the other car's door. A hit the sky ray is neutral for (open 1) is as it was: the calibration of the exposure (car_rt_hit_rules.h census) compares the
//      model with the history on the paint the camera sees, mostly open surfaces, so the model's level there does not move and the gain stays.
//   2. THE CONTACT DARKENING (ContactMultiplier): from the ground up to kContactHeight metres the ambient and the reflection fall to kContactMin. The height is that of the hit over its car's
//      ground (the car's record's centre in view space and the world's up: the origins of a car's draws are on the road, the centre is raised kCarCentreRise); -1 where the pass has no record
//      for the car (no darkening).
//   3. THE CLEAR COAT'S IMAGE OF THE SUN AND OF A STREET LAMP: the paint is glossy under a smooth coat, so a light shows in it as a small bright lobe where the reflected ray (the ray that hit the
//      car, mirrored in the car's own normal) points at the light, by the coat's Schlick fresnel at the ray's own view (CoatFresnel: 0.04 head on, 0.2 at 25 degrees from the plane). By day
//      the light is the sun (GlintLobe(R . to_sun), kSunGlintPower; the visibility the shade already computed); at night it is a lamp high over the street (R . up, a wider lobe) by the
//      sky's openness. A curved body has such a point along every shoulder and roof edge: a bright streak on a few percent of a car's pixels at 10 to 20 times its bulk, which is what the
//      environment's reflection of a lamp was (the heavy tail of the night's 1.81).
//   4. THE HOUR. The sun's light is gone at night and the street lamps (the glints, the sky's fill raised from a fifth to kNightFillShare of the sun's colour) stand in for it; the lamps' glow
//      (emissive) follows the sun's height too (NightFromSunHeight: the volumetric light's own rule), not the draws' c39 colour, which does not change between 14 h and 23 h.
//   5. THE CLASS (kClassAmbient, kClassGlint): a tyre sits in its arch, the interior and the underside are under the body, rubber and trim are matt; glass, paint and chrome show the coat's lights.
//   6. THE LAMP'S LIGHT AT NIGHT (RT-HIT-SHADING-6, kLampDiffuse). rthit4 (night b): the reflected car read (the median 1.43 times the environment's) but its contrast was 0.91 where 1 was wanted: the
//      night's flank had no light, only the fill by a hemisphere term, so the whole car was one dim colour and the lamp's glint was its only structure (a few sparkles on the roof rail), while the environment it
//      replaced has the street lamps as bright blobs (the coefficient of variation of the luminance over the door is 1.65 there, 1.02 by day). A street lamp lights a car, not only its image in the coat: at night
//      what faces up (a roof, a bonnet, a shoulder) takes kLampDiffuse of the sun's colour times N.up, by the sky's openness (the neighbour's roof line shades the flank under it), by the detail. A vertical flank
//      takes none; a roof takes the fill and the lamp (about four times a flank), a shoulder at 45 degrees about three times. By day it is 0 (the night factor is the only way in), so the day's pictures do not move.
//      The exposure's calibration (census) is against the history's own paint, so the level of the lit top and the dim flank is the history's on average; what the lamp changes is their ratio.
// CHECK (nothing was run): every number above is a first guess from the physics, not a fit: kSunRadiance, kLampRadiance, the lobes' powers, kOpenMin, kContactMin, kNightFillShare. The run's pictures
// say; mcla_gpu_car_rt_hit_detail moves the glints live (and 0 gives the old shading). The street lamps' real places (the game's lamp list, vol_lamp_list.h, which exists only while the
// volumetric light is on) would give real glints: a lamp straight overhead is the stand-in. The ground's own contact shadow in the reflection (the road under the neighbour in the player's
// door, the environment map's lit road today) needs a ground plane in the scene and is not here.

inline float CoatFresnel(float nv) { return kCoatF0 + (1.0f - kCoatF0) * std::pow(std::clamp(1.0f - nv, 0.0f, 1.0f), kCoatPower); }
inline float GlintLobe(float cos_angle, float power) { return std::pow(std::clamp(cos_angle, 0.0f, 1.0f), power); }

// The share of the ambient and the reflection a hit keeps at `height` metres over the ground of its car (kContactMin at the ground to 1 from kContactHeight up; a height under 0 is no
// knowledge: 1).
inline float ContactMultiplier(float height) {
  if (!(height >= 0.0f)) {
    return 1.0f;
  }
  const float t = std::clamp(height / kContactHeight, 0.0f, 1.0f);
  return kContactMin + (1.0f - kContactMin) * (t * t * (3.0f - 2.0f * t));
}
// ... and by the sky's openness (0 closed in, 1 open).
inline float OpenMultiplier(float openness) { return kOpenMin + (1.0f - kOpenMin) * std::clamp(openness, 0.0f, 1.0f); }

// A small integer hash (PCG, the shader's copy) and the two numbers in [0, 1) the sky ray's jitter takes from the hit's place in view space (cells of 1 / kOpenHashCells metres).
inline uint32_t Pcg(uint32_t v) {
  const uint32_t state = v * 747796405u + 2891336453u;
  const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}
inline void OpenHash(const float hit[3], float* a, float* b) {
  const int32_t x = int32_t(std::floor(hit[0] * kOpenHashCells));
  const int32_t y = int32_t(std::floor(hit[1] * kOpenHashCells));
  const int32_t z = int32_t(std::floor(hit[2] * kOpenHashCells));
  const uint32_t h = Pcg(uint32_t(x) + Pcg(uint32_t(y) + Pcg(uint32_t(z))));
  *a = float(h & 0xFFFFu) / 65536.0f;
  *b = float(h >> 16) / 65536.0f;
}

// The direction of the sky ray from a hit with the unit `normal` (facing the ray) and the world's `up` (view space): normal + up (up where the two cancel, a surface that faces straight down: it
// then keeps its ray over its own tangent plane, which is on the ground side), spread over a disc of tangent kOpenJitter by the two numbers (angle a, radius sqrt(b): even over the disc), kept kOpenMinAbove over the
// tangent plane (a lift as ConeDirection's). The shader's CarMatSkyDirection is the same arithmetic.
inline void SkyRayDirection(const float normal[3], const float up[3], float a, float b, float out[3]) {
  float d[3];
  for (int c = 0; c < 3; ++c) {
    d[c] = normal[c] + up[c];
  }
  const float length = std::sqrt(Dot3(d, d));
  for (int c = 0; c < 3; ++c) {
    d[c] = length > 0.3f ? d[c] / length : up[c];
  }
  const bool use_y = std::fabs(d[1]) < 0.9f;  // the view's axes, as ConeDirection's frame: never parallel to d, whatever way the camera is rolled
  const float helper[3] = {use_y ? 0.0f : 1.0f, use_y ? 1.0f : 0.0f, 0.0f};
  float t[3] = {helper[1] * d[2] - helper[2] * d[1], helper[2] * d[0] - helper[0] * d[2], helper[0] * d[1] - helper[1] * d[0]};
  const float tl = std::sqrt(Dot3(t, t));
  for (int c = 0; c < 3; ++c) {
    t[c] /= std::max(tl, 1e-12f);
  }
  const float bt[3] = {d[1] * t[2] - d[2] * t[1], d[2] * t[0] - d[0] * t[2], d[0] * t[1] - d[1] * t[0]};
  const float angle = 6.283185307179586f * a;
  const float radius = kOpenJitter * std::sqrt(b);
  float e[3];
  for (int c = 0; c < 3; ++c) {
    e[c] = d[c] + (t[c] * std::cos(angle) + bt[c] * std::sin(angle)) * radius;
  }
  const float el = std::sqrt(Dot3(e, e));
  for (int c = 0; c < 3; ++c) {
    e[c] /= el;
  }
  const float above = Dot3(e, normal);
  if (above < kOpenMinAbove) {
    for (int c = 0; c < 3; ++c) {
      e[c] += (kOpenMinAbove - above) * normal[c];
    }
    const float fl = std::sqrt(Dot3(e, e));
    for (int c = 0; c < 3; ++c) {
      e[c] /= fl;
    }
  }
  for (int c = 0; c < 3; ++c) {
    out[c] = e[c];
  }
}

// ---------------------------------------------------------------------------
// the shading (hlsl/car_rt_hit_shade.hlsli CarHitShade: the same arithmetic)
// ---------------------------------------------------------------------------

struct ShadeInput {
  Class cls = Class::kOther;
  float tint_a[3] = {1, 1, 1};
  float tint_b[3] = {1, 1, 1};
  float flop = 0.0f;
  bool paint = false;
  float fresnel[3] = {0.04f, 0.3f, 5.0f};
  float texel[4] = {1, 1, 1, 1};  // the diffuse sample (rgb, alpha); with `textured` false it is not used
  bool textured = false;
  bool decal = false;
  float emissive = 0.0f;
  float normal[3] = {0, 1, 0};      // unit, facing the ray
  float ray[3] = {0, 0, 1};         // the ray's unit direction
  float to_sun[3] = {0, 1, 0};
  float up[3] = {0, 1, 0};          // the world's up in the same space
  float sun_visible = 1.0f;
  float env[3] = {0, 0, 0};         // the environment map's colour at the reflected direction
  bool env_known = true;
  FrameLight light;
  float night = 0.0f;               // the frame's night factor (the lamps' glow unless `glow_driven`; with sun_known also the sun's end and the sky's night fill)
  bool glow_driven = false;         // LU-8 (car_rt_lights_rules.h): the lens's record is glow-driven: `emissive` is the game's lamps' scale and the night factor does not multiply it (the shader's glowNight is 1)
  bool sun_known = true;            // the frame knows the sun's direction (and so the hour: `night` is the sun's height's)
  float open = 1.0f;                // RT-HIT-SHADING-5: the sky's openness at the hit (the sky ray's answer: 0 closed in, 1 open or not asked)
  float height = -1.0f;             // metres over the ground of the hit's car; under 0: not known
  Tuning tuning;
};


// The surface's albedo: the paint's gradient over the livery, or the texture times the tint, or the tint.
inline void Albedo(const ShadeInput& in, float nv, float out[3]) {
  float base[3];
  if (in.paint && in.flop > 0.0f) {
    const float t = std::pow(std::max(std::clamp(nv, 0.0f, 1.0f), 1e-4f), in.flop);
    for (int c = 0; c < 3; ++c) {
      base[c] = in.tint_b[c] + (in.tint_a[c] - in.tint_b[c]) * t;
    }
  } else {
    for (int c = 0; c < 3; ++c) {
      base[c] = in.tint_a[c];
    }
  }
  if (in.textured) {
    if (in.decal) {
      const float cover = std::clamp(in.texel[3], 0.0f, 1.0f);
      for (int c = 0; c < 3; ++c) {
        base[c] = base[c] + (in.texel[c] - base[c]) * cover;
      }
    } else {
      for (int c = 0; c < 3; ++c) {
        base[c] *= in.texel[c];
      }
    }
  }
  for (int c = 0; c < 3; ++c) {
    out[c] = base[c];
  }
}

// The colour of the hit in the paint's units (the pass multiplies it by 2^exp_bias).
inline void ShadeReference(const ShadeInput& in, float out[3]) {
  const ClassDefaults& d = DefaultsOf(in.cls);
  const float detail = std::max(in.tuning.detail, 0.0f);
  const float strength = std::min(detail, 1.0f);
  const float nv = std::clamp(-Dot3(in.normal, in.ray), 0.0f, 1.0f);
  float albedo[3];
  Albedo(in, nv, albedo);
  const float hemisphere = std::clamp(0.6f + 0.4f * Dot3(in.normal, in.up), 0.2f, 1.0f);
  const float night = in.sun_known ? in.night : 0.0f;  // the sun's end, the night fill and the lamps' images need the hour from the sun's height
  const float day_sun = 1.0f - night;
  const float glow_night = in.glow_driven ? 1.0f : in.night;  // a lens the game's lamps drive glows by its lamps' level alone (the shader's glowNight)
  const float ndl = std::clamp(Dot3(in.normal, in.to_sun), 0.0f, 1.0f);
  float reflected[3];
  const float twice = 2.0f * Dot3(in.normal, in.ray);
  for (int c = 0; c < 3; ++c) {
    reflected[c] = in.ray[c] - twice * in.normal[c];
  }
  const float fresnel = in.fresnel[0] + (in.fresnel[1] - in.fresnel[0]) * std::pow(std::max(1.0f - nv, 1e-4f), std::max(in.fresnel[2], 0.01f));
  // what the hit keeps of its ambient and its reflection: the class, the sky's openness, the contact with the ground
  const float class_ambient = 1.0f + (ClassAmbient(in.cls) - 1.0f) * strength;
  const float ground = (1.0f + (OpenMultiplier(in.open) - 1.0f) * strength) * (1.0f + (ContactMultiplier(in.height) - 1.0f) * strength);
  const float fill_share = kFillShare + (kNightFillShare - kFillShare) * night;
  // RT-HIT-SHADING-6: the lamp's light on the hit (the sun's colour times this), only at night, only facing up, by the sky's openness, scaled by the detail (0: as it was)
  const float lamp_light = kLampDiffuse * night * std::max(Dot3(in.normal, in.up), 0.0f) * in.open * strength;
  const float glint_gain = detail > 0.0f ? ClassGlint(in.cls) * detail * (in.cls == Class::kChrome ? fresnel : CoatFresnel(nv)) : 0.0f;
  const float sun_lobe = glint_gain > 0.0f && ndl > 0.0f ? GlintLobe(Dot3(reflected, in.to_sun), kSunGlintPower) : 0.0f;
  const float lamp_lobe = glint_gain > 0.0f && night > 0.0f ? GlintLobe(Dot3(reflected, in.up), kLampGlintPower) : 0.0f;
  for (int c = 0; c < 3; ++c) {
    const float fill = std::max(in.light.ambient[c], in.light.sun[c] * fill_share);
    const float light = fill * in.tuning.ambient * hemisphere * class_ambient * ground + in.light.sun[c] * ndl * in.sun_visible * day_sun + in.light.sun[c] * lamp_light;
    const float env = in.env_known ? in.env[c] : in.light.ambient[c] * (0.6f + 0.4f * Dot3(reflected, in.up));
    const float tint = in.cls == Class::kChrome ? albedo[c] : 1.0f;
    float colour = albedo[c] * light * d.diffuse_gain + env * fresnel * d.spec_gain * in.tuning.spec * tint * ground;
    colour += in.light.sun[c] * (kSunRadiance * sun_lobe * in.sun_visible * day_sun + kLampRadiance * lamp_lobe * night * in.open) * glint_gain * tint;
    colour += albedo[c] * in.emissive * kEmissiveGain * glow_night;
    out[c] = std::max(colour, 0.0f) * in.tuning.gain;
  }
}

// What a hit shows where the history shows `visible` of it: the shaded colour where it does not, the history's where it does, no seam between (TraceRay's blend).
inline float HitColour(float shaded, float seen, float visible) {
  return shaded + (seen - shaded) * std::clamp(visible, 0.0f, 1.0f);
}

// A line for the log: the material in words.
inline std::string Describe(const Material& m, uint64_t ps) {
  const auto three = [](const float v[3]) {
    char text[64];
    std::snprintf(text, sizeof(text), "%.3f %.3f %.3f", v[0], v[1], v[2]);
    return std::string(text);
  };
  char ident[32];
  std::snprintf(ident, sizeof(ident), "%016llX", (unsigned long long)ps);
  std::string out = std::string("ps ") + ident + " class " + ClassName(m.cls) + ", tint A (" + three(m.tint_a) + ") B (" + three(m.tint_b) + ")";
  if (m.has_fresnel) {
    out += ", fresnel from the draw (" + three(m.fresnel) + ")";
  }
  out += ", flop " + std::to_string(m.flop).substr(0, 5);
  out += m.diffuse.texture ? ", diffuse " + std::to_string(m.diffuse.width) + "x" + std::to_string(m.diffuse.height) : std::string(", no diffuse");
  return out;
}

}  // namespace mcla::gpu::car_rt_material
