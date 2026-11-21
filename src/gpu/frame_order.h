#pragma once
// ===========================================================================
// MCLA GPU renderer -- THE FRAME'S ORDER: one table of every pass that makes or reads the shared screen and light data (LIGHT-UNIFY U9, package LU-9)
// ===========================================================================
// perf/LIGHT-UNIFY.md section 5.3 asked for the trigger order "as a table in frame_order.h so a test can read it", replacing the comment block that stood in Scene::OnResolve. This is that
// table (kRows below). It says, for each pass, in the order the renderer runs them:
//   WHEN      the phase: the frame's start (the wants are declared and read), the draws, the trigger (the game's mid-scene depth copy, 078B0000: every effect that works from the finished
//             opaque picture), the end-of-scene copy (the volumetric light's and the sun shafts' composites), the present;
//   WHAT      the call that runs it (the text as it stands in scene.cpp, or, for a step inside the AO's frame, in ao.cpp) and the PassTiming label of its GPU time;
//   MAKES     the shared data it produces (data:: below: the G-buffer's planes, the screen products of screen_frame.h, the light frame, the shadow atlas, the car structures ...);
//   READS     what it needs to have been made before it runs (`reads`: hard; `soft_reads`: reads it where it is there and so must still come after its maker), and what it takes from the LAST
//             frame (`reads_previous`: the forward headlight's atlas, the car paint's history image);
//   AFTER     orderings that are not data (`deps`, each with its reason): the light pass adds AFTER the occlusion's apply (nothing darkens it), the capture is made AFTER the lights, the reflections
//             AFTER the capture (a reflection is not a reflection of a reflection) ...;
//   WANTS     what the effect asks for while it is on: the G-buffer's planes, the shared screen products, the light manager; and the frame's start, where they are declared and read.
//
// THE FRAME, as scene.cpp runs it (LU-9 recorded the order the code had; GB-I3 moved one call: the reflections' wants are declared at the frame's start, lag 0, see kBeginSsr):
//   start      local lights declare | SSR declares | G-buffer reads its wants | screen products read theirs | AO declares | car reflection declares | light manager opens the frame's lights
//   draws      the main view's draws write the sidecar (the G-buffer's window) | the lit draws' forward headlight (lights of this frame, the maps of the LAST frame)
//   trigger    G-buffer publish | car sun map | car ray tracing structures | headlight shadow replay | light manager takes this frame's maps |
//              THE AO'S FRAME: depth chain, normals, colour_pre capture, ray traced occlusion, visibility, contact march, APPLY, screen publish (lin_depth, normal, colour_pre, Hi-Z) |
//              deferred local lights (additive, after the apply) | sun shafts | volumetric light (inject, integrate) | car ray tracing check |
//              colour_post capture (after the apply and the lights) | SSR (additive, after the capture: the LAST pass that adds to the colour here) | car history + ray traced reflections | debug views
//   end of scene   the volumetric composite, the sun shafts' composite
//   present    world classification | screen products trade their capture with the history | light manager checks and uploads | the rig makes the next frame's lights
// WHERE THE REFLECTIONS STAND (GB-I3): after the capture they read (a reflection is not a reflection of a reflection) and after the light pass (they show the lights), before the car history (which holds
// a vehicle's pixels only; SSR adds nothing to a vehicle's pixel) and the debug views. Their wants of the planes and the screen products are declared first thing in the frame (lag 0). Their GPU time is
// the labels of the SSR row ('ssr trace', 'ssr temporal', 'ssr apply', closed by 'after ssr') and, while they are the products' only consumer, the owner's 'screen hiz' and 'screen colour post'
// (namespace cost below: the arms a cost run sets, and which labels are the effect's own).
// Where it differs from LIGHT-UNIFY 5.3's picture: the AO still builds the depth chain, the normals and the colour chain itself (SS-2 moves them into the screen owner; the screen frame, the Hi-Z
// included, is published at the END of the AO's frame, after its apply, from ScreenShared::Publish); the sun shafts and the volume's inject / integrate record BEFORE the capture and SSR (the
// design listed them after SSR; they add nothing to the colour at the trigger, their composites are at the end-of-scene copy, so the order between them is free of data and the table keeps the code's).
//
// THE CHECKS
//   1. Scene notes each trigger and end-of-scene pass it records (`Recorder::Note`, the lines `order_.Note(frame_order::Step::k...)` in Scene::OnResolve, each right after its call), and Scene::OnPresent
//      ends the frame: the frame's notes must be in table order, no pass twice, every hard read made by an earlier pass that ran. An inversion is a bug in the code order: it asserts in a debug
//      build (NDEBUG off) and is logged once either way ("[gpu] frame order: ..."), and the first frame of each set of passes that ran is logged as a census line (what ran, in order).
//   2. frame_order_test.cpp holds the table to itself (every read has an earlier maker, every dependency an earlier step, the wants are what the consumers ask for and are made by somebody, the lag of each
//      want), holds scene.cpp, ao.cpp and the consumers' sources to the table (the calls stand in the table's order, each noted pass has its Note line, the labels exist, the Want calls exist), runs the
//      recorder over the full frame, over frames with passes missing and over frames with every kind of mistake, and moves every pass of the table before what it depends on to check the table says so.
//
// HOW TO ADD A PASS: add its Step in the place it must run, its Row (reads, makes, deps, wants), the `order_.Note` line after its call in scene.cpp, and, if it makes something new, a data:: bit. The test
// fails until the table, scene.cpp and the pass agree. HOW TO MOVE ONE: move its enumerator and row together and its call in scene.cpp; the dependencies say what it may not cross.
//
// Pure: no Plume, no renderer. Threading: the Recorder is for the render thread (Scene).
// ===========================================================================

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "gbuffer_types.h"
#include "screen_frame.h"

namespace mcla::gpu::frame_order {

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// the shared data
// ---------------------------------------------------------------------------------------------------------------------------------------------------
namespace data {
// The screen products of the published ScreenFrame (screen_frame.h Product): the same bits, so that a wants mask compares directly.
inline constexpr uint32_t kLinDepth = 1u << 0;
inline constexpr uint32_t kLinDepthFull = 1u << 1;
inline constexpr uint32_t kHizMin = 1u << 2;
inline constexpr uint32_t kNormal = 1u << 3;
inline constexpr uint32_t kColourPre = 1u << 4;
inline constexpr uint32_t kColourPost = 1u << 5;
inline constexpr uint32_t kHistory = 1u << 6;
// the rest
inline constexpr uint32_t kPlanes = 1u << 7;         // the G-buffer's planes (RT1 normal, RT2 albedo and lit weight, RT3 material) PUBLISHED at the trigger (gbuffer.h)
inline constexpr uint32_t kPlanesDrawn = 1u << 8;    // the sidecar as the main view's draws wrote it, until the publish closes the window
inline constexpr uint32_t kCarSunMap = 1u << 9;      // the player car's sun shadow map (car_shadow_map.h): the contact march reads it
inline constexpr uint32_t kCarStructures = 1u << 10; // the car ray tracing structures (car_rt_scene.h): the bottom level ones and this frame's top level
inline constexpr uint32_t kLights = 1u << 11;        // the frame's lights: the light manager's LightFrame and the rig's set (light_manager.h, headlight_rig.h)
inline constexpr uint32_t kShadowAtlas = 1u << 12;   // the shadow replay's maps recorded THIS frame in the atlas (local_shadow.h, shadow_atlas.h)
inline constexpr uint32_t kLightMaps = 1u << 13;     // the light manager's frame holds this frame's maps (its shadow views)
inline constexpr uint32_t kAoDepth = 1u << 14;       // the AO's own depth chain images (the owner of lin_depth until SS-2)
inline constexpr uint32_t kAoNormal = 1u << 15;      // the AO's normal image
inline constexpr uint32_t kColourChain = 1u << 16;   // the bounce light's colour chain (what colour_pre publishes)
inline constexpr uint32_t kRtOcclusion = 1u << 17;   // the car ray traced occlusion / sun term image (car_rt_ao.h)
inline constexpr uint32_t kAoTerm = 1u << 18;        // the occlusion's / the bounce light's accumulated result, read by the apply
inline constexpr uint32_t kContactTerm = 1u << 19;   // the contact shadows' visibility, read by the apply
inline constexpr uint32_t kVolume = 1u << 20;        // the volumetric light's integrated froxels (volumetrics.h)
inline constexpr uint32_t kShafts = 1u << 21;        // the sun shafts' light image (shafts.h)
inline constexpr uint32_t kCarHistory = 1u << 22;    // the car self-reflection's history image and the ray traced reflection of its texels: read by the NEXT frame's car paint draws
inline constexpr uint32_t kAll = (1u << 23) - 1;
constexpr const char* NameOf(uint32_t bit) {
  switch (bit) {
    case kLinDepth: return "lin_depth";
    case kLinDepthFull: return "lin_depth_full";
    case kHizMin: return "hiz_min";
    case kNormal: return "normal";
    case kColourPre: return "colour_pre";
    case kColourPost: return "colour_post";
    case kHistory: return "history";
    case kPlanes: return "G-buffer planes";
    case kPlanesDrawn: return "sidecar drawn";
    case kCarSunMap: return "car sun map";
    case kCarStructures: return "car RT structures";
    case kLights: return "frame lights";
    case kShadowAtlas: return "shadow atlas";
    case kLightMaps: return "light frame maps";
    case kAoDepth: return "AO depth chain";
    case kAoNormal: return "AO normals";
    case kColourChain: return "colour chain";
    case kRtOcclusion: return "RT occlusion";
    case kAoTerm: return "AO term";
    case kContactTerm: return "contact term";
    case kVolume: return "volume";
    case kShafts: return "shafts";
    case kCarHistory: return "car history";
    default: break;
  }
  return "?";
}
}  // namespace data
static_assert(data::kLinDepth == screen::ProductBit(screen::kLinDepth) && data::kLinDepthFull == screen::ProductBit(screen::kLinDepthFull) && data::kHizMin == screen::ProductBit(screen::kHizMin) &&
                  data::kNormal == screen::ProductBit(screen::kNormal) && data::kColourPre == screen::ProductBit(screen::kColourPre) &&
                  data::kColourPost == screen::ProductBit(screen::kColourPost) && data::kHistory == screen::ProductBit(screen::kHistory),
              "the screen products of the data bits are screen_frame.h's Product bits");
inline constexpr uint32_t kScreenData = screen::kAllProducts;  // the bits above that are screen products

// The three kinds of want a frame-start step can read (and whose declaring steps the lag is measured from).
namespace want {
inline constexpr uint32_t kPlanes = 1u << 0;   // GBuffer::Want
inline constexpr uint32_t kScreen = 1u << 1;   // ScreenShared::Want
inline constexpr uint32_t kLights = 1u << 2;   // LightManager::Want
}  // namespace want

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// the steps, in the order the frame runs them (Step(i) is row i of kRows)
// ---------------------------------------------------------------------------------------------------------------------------------------------------
enum class Phase : uint8_t { kBegin, kDraws, kTrigger, kEndOfScene, kPresent, kCount };
constexpr const char* PhaseName(Phase phase) {
  switch (phase) {
    case Phase::kBegin: return "frame start";
    case Phase::kDraws: return "draws";
    case Phase::kTrigger: return "trigger";
    case Phase::kEndOfScene: return "end of scene";
    case Phase::kPresent: return "present";
    case Phase::kCount: break;
  }
  return "?";
}

enum class Step : uint8_t {
  // frame start (Scene::BeginFrame)
  kBeginLocalLights,
  kBeginSsr,
  kBeginGBuffer,
  kBeginScreen,
  kBeginAo,
  kBeginCarRefl,
  kBeginLights,
  // the draws
  kDrawSidecar,
  kDrawHeadlight,
  // the trigger (Scene::OnResolve)
  kGBufferPublish,
  kCarSunMap,
  kCarStructures,
  kShadowReplay,
  kLightMaps,
  kAo,          // the AO's frame: the call, and the steps inside it
  kAoDepth,
  kAoNormals,
  kColourPre,
  kCarRtAo,
  kAoVisibility,
  kContact,
  kAoApply,
  kScreenPublish,
  kLocalLights,
  kShafts,
  kVolume,
  kCarRtCheck,
  kColourPost,
  kSsr,
  kCarHistory,
  kGBufferView,
  kScreenView,
  // the end-of-scene copy (the same function, an earlier text: BeforeResolve)
  kVolumeComposite,
  kShaftsComposite,
  // the present (Scene::OnPresent)
  kPresentWorld,
  kPresentScreen,
  kPresentLights,
  kPresentRig,
  kCount,
  kNone = 0xFF,
};
inline constexpr size_t kStepCount = size_t(Step::kCount);
static_assert(kStepCount <= 64, "the recorder keeps a frame's steps in one 64 bit mask");
constexpr size_t Index(Step step) { return size_t(step); }

// How a step is checked: kNoted = Scene notes it at run time (it is a call whose result says it recorded); kInner = a step inside a noted call (it ran when that call did: `umbrella`); kScanned = only
// the source checks hold it (the frame's start, the draws, the present, the debug views).
enum class Kind : uint8_t { kNoted, kInner, kScanned };

struct Dep {
  Step before = Step::kNone;  // the step that must come earlier in the table
  const char* why = "";
};

struct Row {
  Step step = Step::kNone;
  Phase phase = Phase::kTrigger;
  Kind kind = Kind::kScanned;
  Step umbrella = Step::kNone;       // kInner: the noted step whose call runs this one
  const char* name = "";             // plain words
  const char* file = "scene.cpp";    // the source file (in src/gpu) that holds `call`
  const char* call = "";             // the text of the call as it stands in that file, without the closing semicolon
  const char* label_file = "";       // the file holding the PassTiming label
  const char* label = "";            // the PassTiming label of the pass (its GPU time runs to the next mark)
  std::array<const char*, 3> more_labels{};  // the pass's other marks, in the order it writes them (a pass of several draws: the SSR's trace, temporal and apply); the pass's own time is the sum
  const char* closing_label = "";    // the mark written after the pass so that the time of what is recorded next is not the pass's ("after local lights"): not part of its own time
  uint32_t reads = 0;                // data:: bits that must exist when it runs (made by an earlier step that ran, or by the frame's start)
  uint32_t soft_reads = 0;           // data:: bits it reads where they are there: they must come from an earlier step
  uint32_t reads_previous = 0;       // data:: bits of the LAST frame it reads (no order inside the frame)
  uint32_t makes = 0;                // data:: bits it produces
  uint32_t wants_planes = 0;         // the G-buffer planes it asks for while it is on (gbuffer::PlaneBit)
  uint32_t wants_screen = 0;         // the screen products it asks for while it is on (data:: screen bits)
  bool wants_lights = false;         // it asks the light manager for the frame
  Step declares_for = Step::kNone;   // a frame-start step: the step whose wants it declares
  uint32_t reads_wants = 0;          // a frame-start step: the kinds of want it reads (want::)
  std::array<Dep, 3> deps{};
  const char* note = "";
};

namespace detail {
inline constexpr uint32_t kAllPlaneBits = gbuffer::kAllPlanes;
inline constexpr uint32_t kNormalPlane = gbuffer::PlaneBit(gbuffer::kPlaneNormal);
}  // namespace detail

// THE TABLE. Row i is Step(i); the phases do not go back; a kInner row follows its umbrella with only its own steps between.
inline constexpr std::array<Row, kStepCount> kRows = {{
    // ======================================================== the frame's start (Scene::BeginFrame) ====================================================
    {.step = Step::kBeginLocalLights, .phase = Phase::kBegin, .kind = Kind::kScanned, .name = "local lights declare their wants", .call = "local_lights_->BeginFrame()",
     .declares_for = Step::kLocalLights,
     .note = "FIRST: its wants of the planes, the screen products and the light manager are read by the owners below in this very frame (lag 0)"},
    {.step = Step::kBeginSsr, .phase = Phase::kBegin, .kind = Kind::kScanned, .name = "SSR declares its wants", .call = "ssr_->BeginFrame()", .declares_for = Step::kSsr,
     .note = "SECOND, before the G-buffer and the screen owner read (GB-I3 moved it above them): its wants of the planes, of the Hi-Z and of colour_post (ssr_rules.h WantedPlanes / WantedProducts: "
             "ONLY while REFLECTIONS ON SURFACES is on; nothing at off) are read in this very frame (lag 0), so the effect can run in the frame the setting changes, and the owners stop making them "
             "in the frame it is switched off. The call reads settings and sets wants only: nothing in the frame start between the old place and the new one depends on it"},
    {.step = Step::kBeginGBuffer, .phase = Phase::kBegin, .kind = Kind::kScanned, .name = "G-buffer reads the planes' wants, opens its window", .call = "gbuffer_->BeginFrame(frame_serial_)",
     .reads_wants = want::kPlanes},
    {.step = Step::kBeginScreen, .phase = Phase::kBegin, .kind = Kind::kScanned, .name = "screen products read their wants, make the frame's plan", .call = "screen_->BeginFrame(frame_serial_)",
     .reads_wants = want::kScreen},
    {.step = Step::kBeginAo, .phase = Phase::kBegin, .kind = Kind::kScanned, .name = "AO declares its wants", .call = "ao_->BeginFrame()", .declares_for = Step::kAo,
     .note = "after the screen owner's BeginFrame, which it needs: it asks the plan whether a consumer outside its family wants the depth chain"},
    {.step = Step::kBeginCarRefl, .phase = Phase::kBegin, .kind = Kind::kScanned, .name = "car self-reflection declares its wants", .call = "car_refl_->BeginFrame()", .declares_for = Step::kCarHistory},
    {.step = Step::kBeginLights, .phase = Phase::kBegin, .kind = Kind::kScanned, .name = "light manager opens the frame's lights", .call = "light_manager_->BeginLights(frame_serial_, slot)",
     .makes = data::kLights, .reads_wants = want::kLights,
     .note = "before the shadow replay's BeginFrame (the replay picks its cars from the frame's lights)"},
    // ======================================================== the draws =============================================================================
    {.step = Step::kDrawSidecar, .phase = Phase::kDraws, .kind = Kind::kScanned, .name = "main view's draws write the G-buffer's sidecar", .call = "gbuffer_->Attach(list, *colors[0], &sidecar)",
     .makes = data::kPlanesDrawn, .deps = {{{Step::kBeginGBuffer, "the plan of the frame's planes"}}},
     .note = "the window opens at the guest's clear of the main colour and closes at the publish (gbuffer_rules.h)"},
    {.step = Step::kDrawHeadlight, .phase = Phase::kDraws, .kind = Kind::kScanned, .name = "forward headlight stage of the lit draws", .call = "case sp::kFeatureHeadlight: {",
     .reads = data::kLights, .reads_previous = data::kShadowAtlas,
     .note = "the block is this frame's lights, the maps are the LAST frame's (HP3 in local_lights.h: a moving car's shadow edge differs by a frame's motion from the light pass's); runs before the "
             "AO's apply, so the occlusion darkens it (HP11) and the bounce light's colour chain sees it"},
    // ======================================================== the trigger (Scene::OnResolve) =======================================================
    {.step = Step::kGBufferPublish, .phase = Phase::kTrigger, .kind = Kind::kNoted, .name = "G-buffer publish", .call = "gbuffer_->OnResolve(record, summary, list)",
     .reads = data::kPlanesDrawn, .makes = data::kPlanes,
     .note = "closes the sidecar's window at the mid-scene depth copy and publishes the planes, before the effects that read them"},
    {.step = Step::kCarSunMap, .phase = Phase::kTrigger, .kind = Kind::kNoted, .name = "car sun shadow map", .call = "car_shadow_->OnResolve(record, summary, list)",
     .label_file = "car_shadow_map.cpp", .label = "car shadow map", .makes = data::kCarSunMap,
     .note = "BEFORE the AO: the contact march reads the map"},
    {.step = Step::kCarStructures, .phase = Phase::kTrigger, .kind = Kind::kNoted, .name = "car ray tracing structures", .call = "car_rt_->OnResolve(record, summary, list, slot_)",
     .label_file = "car_rt_scene.cpp", .label = "car rt build", .makes = data::kCarStructures,
     .note = "FIRST of everything that traces: the AO's ray traced term and the car reflection's trace both use this frame's top level"},
    {.step = Step::kShadowReplay, .phase = Phase::kTrigger, .kind = Kind::kNoted, .name = "headlight shadow replay", .call = "local_shadow_->OnResolve(record, summary, list, slot_)",
     .label_file = "local_shadow.cpp", .label = "headlight shadows", .soft_reads = data::kLights, .makes = data::kShadowAtlas,
     .note = "its own pass and target, independent of the others; the atlas is read by this frame's volume and light pass and by the next frame's lit draws"},
    {.step = Step::kLightMaps, .phase = Phase::kTrigger, .kind = Kind::kNoted, .name = "light manager takes this frame's maps", .call = "light_manager_->OnResolve(shadow_recorded)",
     .reads = data::kShadowAtlas, .makes = data::kLightMaps, .deps = {{{Step::kShadowReplay, "its maps are what it takes"}}},
     .note = "passive (light_manager.h): the block is checked against them; noted when the replay recorded"},
    {.step = Step::kAo, .phase = Phase::kTrigger, .kind = Kind::kNoted, .name = "the AO's frame (occlusion, bounce light, contact shadows, screen products)",
     .call = "ao_->OnResolve(record, summary, list, UpscalerRunning())", .label_file = "ao.cpp", .label = "ao depth",
     .wants_planes = detail::kNormalPlane, .wants_screen = data::kLinDepth | data::kNormal | data::kColourPre,
     .note = "runs a depth-only frame for a consumer outside its family when the occlusion and the contact shadows are off (screen_wants_); the steps below run inside it, in this order"},
    {.step = Step::kAoDepth, .phase = Phase::kTrigger, .kind = Kind::kInner, .umbrella = Step::kAo, .name = "depth chain (lin_depth's owner)", .file = "ao.cpp",
     .call = "DrawPass(list, depth_[0], depth_pipeline, push);", .label_file = "ao.cpp", .label = "ao depth", .makes = data::kAoDepth},
    {.step = Step::kAoNormals, .phase = Phase::kTrigger, .kind = Kind::kInner, .umbrella = Step::kAo, .name = "normals (G-buffer RT1 where valid, else the depth's)", .file = "ao.cpp",
     .call = "RecordNormals(list, plan, base, normals_);", .label_file = "ao.cpp", .label = "ao normals", .reads = data::kAoDepth, .soft_reads = data::kPlanes, .makes = data::kAoNormal},
    {.step = Step::kColourPre, .phase = Phase::kTrigger, .kind = Kind::kInner, .umbrella = Step::kAo, .name = "colour_pre capture (capped, only while the bounce light is on)", .file = "ao.cpp",
     .call = "screen_->CapturePre([&] { RecordColourChain(list, plan, base, *gi_colour, gi_colour_slot); });", .label_file = "ao.cpp", .label = "ssgi colour chain", .makes = data::kColourChain,
     .note = "the main colour BEFORE the apply: the bounce light must see the colour the occlusion has not multiplied yet"},
    {.step = Step::kCarRtAo, .phase = Phase::kTrigger, .kind = Kind::kInner, .umbrella = Step::kAo, .name = "ray traced occlusion and sun term of the cars", .file = "ao.cpp",
     .call = "rt = rt_ao_->Record(list, in);", .label_file = "car_rt_ao.cpp", .label = "ao rt", .reads = data::kAoDepth, .soft_reads = data::kCarStructures, .makes = data::kRtOcclusion},
    {.step = Step::kAoVisibility, .phase = Phase::kTrigger, .kind = Kind::kInner, .umbrella = Step::kAo, .name = "visibility, bounce, denoise, accumulation", .file = "ao.cpp",
     .call = "pass_timing_->Mark(list, plan.gi.on ? \"ssgi visibility\" : \"ao visibility\");", .label_file = "ao.cpp", .label = "ao visibility", .reads = data::kAoDepth,
     .soft_reads = data::kAoNormal | data::kColourChain | data::kRtOcclusion, .makes = data::kAoTerm},
    {.step = Step::kContact, .phase = Phase::kTrigger, .kind = Kind::kInner, .umbrella = Step::kAo, .name = "contact shadows' march", .file = "ao.cpp",
     .call = "contact_slot = RecordContact(list, plan, base, contact_mask);", .label_file = "ao.cpp", .label = "ao contact", .reads = data::kAoDepth,
     .soft_reads = data::kAoNormal | data::kCarSunMap | data::kRtOcclusion, .makes = data::kContactTerm},
    {.step = Step::kAoApply, .phase = Phase::kTrigger, .kind = Kind::kInner, .umbrella = Step::kAo, .name = "apply onto the main colour (multiplies)", .file = "ao.cpp",
     .call = "applied = RecordApply(list, summary, plan, push);", .label_file = "ao.cpp", .label = "ao apply", .reads = data::kAoDepth,
     .soft_reads = data::kAoTerm | data::kContactTerm | data::kAoNormal | data::kCarSunMap,
     .deps = {{{Step::kColourPre, "the bounce light's colour is captured BEFORE the apply multiplies the occlusion into it"}}}},
    {.step = Step::kScreenPublish, .phase = Phase::kTrigger, .kind = Kind::kInner, .umbrella = Step::kAo, .name = "screen frame published, Hi-Z made", .file = "ao.cpp",
     .call = "PublishScreen(list, summary, plan, base);", .label_file = "screen_shared.cpp", .label = "screen hiz", .reads = data::kAoDepth, .soft_reads = data::kAoNormal | data::kColourChain,
     .makes = data::kLinDepth | data::kLinDepthFull | data::kNormal | data::kColourPre | data::kHizMin,
     .note = "ScreenShared::Publish at the END of the AO's frame (after the apply): every reader of the ScreenFrame comes after the whole AO"},
    {.step = Step::kLocalLights, .phase = Phase::kTrigger, .kind = Kind::kNoted, .name = "deferred local lights (additive)", .call = "local_lights_->OnResolve(record, summary, list, gbuffer_published, slot_)",
     .label_file = "local_lights.cpp", .label = "local lights", .closing_label = "after local lights", .reads = data::kPlanes | data::kLinDepth | data::kLights,
     .soft_reads = data::kLightMaps | data::kShadowAtlas,
     .wants_planes = detail::kAllPlaneBits, .wants_screen = data::kLinDepth, .wants_lights = true,
     .deps = {{{Step::kAoApply, "added AFTER the occlusion's apply: nothing darkens the light (HP11), the bounce light does not see it (C10)"}}},
     .note = "the headlights read this frame's maps from the atlas through the one lookup; before the sun shafts, the volume and the game's own light volumes and glows"},
    {.step = Step::kShafts, .phase = Phase::kTrigger, .kind = Kind::kNoted, .name = "sun shafts' pass", .call = "shafts_->OnResolve(record, summary, list, UpscalerRunning())",
     .label_file = "shafts.cpp", .label = "shafts sky", .makes = data::kShafts,
     .note = "sky pass into an image of its own, no data dependency on the steps around it: in the play view its composite is at the end-of-scene copy (after SSR). The design (LIGHT-UNIFY 5.3) lists it after "
             "SSR; the code runs it before the capture, as it did, and GB-I3 keeps that: the pause and map worlds (and the debug switch, or a build with no copy at the end of the scene) composite the shafts "
             "right here, at the trigger, so the capture, the car history and, where it runs, SSR see them as the picture they are in. Moving the pass after SSR would drop them from the pause worlds' car history"},
    {.step = Step::kVolume, .phase = Phase::kTrigger, .kind = Kind::kNoted, .name = "volumetric light: inject and integrate", .call = "vol_->OnResolve(record, summary, list, slot_, UpscalerRunning())",
     .label_file = "volumetrics.cpp", .label = "vol inject", .soft_reads = data::kShadowAtlas | data::kLights, .makes = data::kVolume,
     .note = "compute passes into the froxels, nothing written to the main colour (its composite is at the end-of-scene copy): they read this frame's maps (the replay's last barrier names the compute stage). "
             "The depth is the main depth plane read directly: it does not use lin_depth and does not Want it (screen::Consumer::kVolumetrics is never declared; the design said it would read lin_depth)"},
    {.step = Step::kCarRtCheck, .phase = Phase::kTrigger, .kind = Kind::kNoted, .name = "car ray tracing check (debug)", .call = "car_rt_->OnAfterOcclusion(summary, list, slot_, ao_)",
     .label_file = "car_rt_scene.cpp", .label = "car rt check", .reads = data::kAoDepth | data::kCarStructures,
     .note = "reads the AO's linear depth image, so after the AO's frame"},
    {.step = Step::kColourPost, .phase = Phase::kTrigger, .kind = Kind::kNoted, .name = "colour_post capture (uncapped, with the history)", .call = "screen_->CapturePost(record, summary, list)",
     .label_file = "screen_shared.cpp", .label = "screen colour post", .makes = data::kColourPost | data::kHistory,
     .deps = {{{Step::kAoApply, "the colour as the occlusion left it"}, {Step::kLocalLights, "UNCAPPED and after the deferred lights: the reflections must see the lights"}}},
     .note = "made only while a consumer wants colour_post; before SSR adds to the colour (a reflection is not a reflection of a reflection)"},
    {.step = Step::kSsr, .phase = Phase::kTrigger, .kind = Kind::kNoted, .name = "screen-space reflections (additive)", .call = "ssr_->OnTrigger(summary, list)",
     .label_file = "ssr.cpp", .label = "ssr trace", .more_labels = {{"ssr temporal", "ssr apply"}}, .closing_label = "after ssr",
     .reads = data::kHizMin | data::kColourPost | data::kPlanes, .wants_planes = detail::kAllPlaneBits, .wants_screen = data::kHizMin | data::kColourPost,
     .deps = {{{Step::kColourPost, "reads the capture, which must be made before the effect adds to the colour"}, {Step::kLocalLights, "the reflection shows the lights"}}},
     .note = "the last pass that adds to the colour at the trigger: after the capture it reads and the light pass it shows, before the car history, whose vehicle-only image it does not touch (a vehicle's "
             "pixel takes none of the effect). GPU time: the labels 'ssr trace', 'ssr temporal', 'ssr apply' are its own (the chains sum the labels that start with 'ssr '), 'after ssr' closes the span and is "
             "not; the products it wants are timed under the owner's 'screen hiz' and 'screen colour post'. Its Hi-Z comes from the screen frame published inside the AO's frame"},
    {.step = Step::kCarHistory, .phase = Phase::kTrigger, .kind = Kind::kNoted, .name = "car self-reflection history and ray traced reflection", .call = "car_refl_->OnResolve(record, summary, list)",
     .label_file = "car_selfrefl.cpp", .label = "car history", .soft_reads = data::kColourPost | data::kCarStructures, .makes = data::kCarHistory, .wants_screen = data::kColourPost,
     .deps = {{{Step::kAoApply, "the history is the colour the occlusion left"}, {Step::kSsr, "kept after SSR as the design has it; above 1080 lines (colour_post is half size) the history reads the main colour, which holds "
                                                                                                "SSR's reflections by then, but the history keeps a vehicle's pixels only and SSR adds nothing to a vehicle's pixel (GB_FLAG_VEHICLE, the stub's stencil mark)"}}},
     .note = "the history image and the ray traced reflection of its texels (car_rt_refl.h, with this frame's top level) are read by the NEXT frame's car paint draws. "
             "CHECK (run): the cars' pixels with the reflections on and off at 4K are the same (a car's glass over a road pixel could keep the road's class: the sidecar's mask rule)"},
    {.step = Step::kGBufferView, .phase = Phase::kTrigger, .kind = Kind::kScanned, .name = "G-buffer debug view", .call = "gbuffer_->DrawView(summary, list)",
     .label_file = "gbuffer.cpp", .label = "gbuffer view", .reads = data::kPlanes, .deps = {{{Step::kCarHistory, "the view replaces what the effects above drew"}}},
     .note = "debug; drawn at every resolve while the planes are valid, so not noted as once-a-frame"},
    {.step = Step::kScreenView, .phase = Phase::kTrigger, .kind = Kind::kScanned, .name = "screen products' debug view", .call = "screen_->DrawView(summary, list)",
     .label_file = "screen_shared.cpp", .label = "screen view", .soft_reads = kScreenData, .deps = {{{Step::kGBufferView, "after the other views"}}}},
    // ======================================================== the end-of-scene copy (Scene::OnResolve, the BeforeResolve calls) =========================
    {.step = Step::kVolumeComposite, .phase = Phase::kEndOfScene, .kind = Kind::kNoted, .name = "volumetric composite into the colour", .call = "vol_->BeforeResolve(record, list)",
     .label_file = "volumetrics.cpp", .label = "vol apply", .soft_reads = data::kVolume,
     .note = "at the end-of-scene copy of the main colour, before that copy reads it; in the text of OnResolve it stands above the trigger's calls (the same function, a different resolve)"},
    {.step = Step::kShaftsComposite, .phase = Phase::kEndOfScene, .kind = Kind::kNoted, .name = "sun shafts' composite into the colour", .call = "shafts_->BeforeResolve(record, list)",
     .label_file = "shafts.cpp", .label = "shafts add", .soft_reads = data::kShafts, .deps = {{{Step::kVolumeComposite, "the shafts' light is added on top of the haze"}}}},
    // ======================================================== the present (Scene::OnPresent) =======================================================
    {.step = Step::kPresentWorld, .phase = Phase::kPresent, .kind = Kind::kScanned, .name = "the frame's world is classified", .call = "NoteMenuWorld();  // before the effects forget the frame",
     .note = "before the effects forget the frame (the camera history asks it)"},
    {.step = Step::kPresentScreen, .phase = Phase::kPresent, .kind = Kind::kScanned, .name = "screen products: the capture and its history trade places", .call = "screen_->OnPresent()",
     .reads = data::kColourPost, .deps = {{{Step::kPresentWorld, "it asks the camera history, which the classification reads"}}}},
    {.step = Step::kPresentLights, .phase = Phase::kPresent, .kind = Kind::kScanned, .name = "light manager checks and uploads the frame", .call = "light_manager_->OnPresent()",
     .reads = data::kLightMaps, .note = "BEFORE the rig makes the next frame's set"},
    {.step = Step::kPresentRig, .phase = Phase::kPresent, .kind = Kind::kScanned, .name = "the rig makes the next frame's lights", .call = "headlights_->OnPresent(frame_serial_)",
     .deps = {{{Step::kPresentLights, "the manager checks this frame's set before the rig replaces it"}}}},
}};

constexpr const Row& RowOf(Step step) { return kRows[size_t(step)]; }

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// THE COST ARMS of the reflections (GB-I3): what a cost run sets and where its GPU time is
// ---------------------------------------------------------------------------------------------------------------------------------------------------
// A cost run (the chains' arms: perf/scripts chain-gb-phase2b.ps1 'ssr-high', 'ssr-ultra', the frame's baseline) sets mcla_gpu_ssr to one of the words below and reads the GPU time of the labels. `auto` (the
// default) is the GPU class's cell, off everywhere until the owner has seen the pictures, so it is no arm: a run names the level. What each arm must show, in the pass timing and in the order census:
//   off     none of the SSR labels, no capture (no 'screen colour post', no 'screen hiz': nothing else asks for them at the defaults), no G-buffer planes (the sidecar needs a consumer), no held images:
//           the effect costs nothing, which is what the defaults' cost runs (P9) check
//   high    the SSR row's labels (its own time: 'ssr trace' + 'ssr temporal' + 'ssr apply'), plus the products that exist only for it: 'screen hiz' and 'screen colour post', plus the sidecar's cost in the
//           draws and the planes' memory; the census names the capture and the reflections after the light pass
//   ultra   the same labels, the trace with two rays and 64 steps (ssr_rules.h NumbersOf): the trace label costs more, the others about the same
// A chain that wants the effect's whole cost reads: the own labels + the product labels + the frame's difference to the off arm (the planes). frame_order_test.cpp holds the words to ssr_rules.h, the
// labels to the SSR row and to the sources.
namespace cost {
struct Arm {
  const char* word;  // the value of mcla_gpu_ssr
  bool runs;         // the capture and the SSR step are in the frame's census, the labels in the pass timing
};
inline constexpr const char* kReflectionsSetting = "mcla_gpu_ssr";
inline constexpr std::array<Arm, 3> kReflectionsArms = {{{"off", false}, {"high", true}, {"ultra", true}}};
// The labels of the products the owner makes only while a consumer wants them (SSR is the only consumer at the defaults of the other effects): the Hi-Z's pass and the capture's.
inline constexpr std::array<const char*, 2> kReflectionsProductLabels = {"screen hiz", "screen colour post"};
// The step whose row carries the effect's own labels (Row::label + Row::more_labels), and its closing label (Row::closing_label).
inline constexpr Step kReflectionsStep = Step::kSsr;
}  // namespace cost

// The table's own consistency, in functions over a row list so that a test can run them on a changed copy.
namespace rules {

// Position of `step` among `rows` (rows are in run order); n when it is not there.
inline size_t PositionIn(const Row* rows, size_t n, Step step) {
  for (size_t i = 0; i < n; ++i) {
    if (rows[i].step == step) return i;
  }
  return n;
}

// What a frame always has before the first trigger step: the data the rows outside the noted calls make (the frame's start, the draws, the present of the last frame).
constexpr uint32_t AlwaysThere(const Row* rows, size_t n) {
  uint32_t bits = 0;
  for (size_t i = 0; i < n; ++i) {
    if (rows[i].kind == Kind::kScanned) bits |= rows[i].makes;
  }
  return bits;
}

// Every problem the rows have, as text (empty: consistent). `identity`: also require row i to be Step(i) (the shipped table; a test's moved copy is not).
inline std::vector<std::string> Validate(const Row* rows, size_t n, bool identity) {
  std::vector<std::string> out;
  auto say = [&](size_t i, const std::string& text) { out.push_back(std::string(rows[i].name) + ": " + text); };
  for (size_t i = 0; i < n; ++i) {
    const Row& row = rows[i];
    if (identity && size_t(row.step) != i) say(i, "the row is not at its step's index");
    if (row.name[0] == '\0' || row.call[0] == '\0') say(i, "a row needs a name and a call");
    if (i > 0 && row.phase < rows[i - 1].phase) say(i, "the phases go back");
    if (row.kind == Kind::kInner) {
      const size_t at = PositionIn(rows, n, row.umbrella);
      if (at >= i) {
        say(i, "its umbrella call is not earlier");
      } else {
        if (rows[at].kind != Kind::kNoted) say(i, "its umbrella is not a noted call");
        if (rows[at].phase != row.phase) say(i, "its umbrella is in another phase");
        for (size_t j = at + 1; j < i; ++j) {
          if (rows[j].umbrella != row.umbrella) say(i, "something else stands between it and its umbrella");
        }
      }
    } else if (row.umbrella != Step::kNone) {
      say(i, "only an inner row has an umbrella");
    }
    // the data: every read has a maker earlier in the table (or is the frame's own: the rows outside the noted calls make it), previous-frame reads need a maker somewhere
    const uint32_t reads = row.reads | row.soft_reads;
    for (uint32_t bit = 1; bit != 0 && bit <= data::kAll; bit <<= 1) {
      if ((reads & bit) != 0) {
        bool earlier = false;
        for (size_t j = 0; j < i; ++j) earlier = earlier || (rows[j].makes & bit) != 0;
        if (!earlier) say(i, std::string("reads ") + data::NameOf(bit) + ", which no earlier step makes");
      }
      if ((row.reads_previous & bit) != 0) {
        bool anywhere = false;
        for (size_t j = 0; j < n; ++j) anywhere = anywhere || (rows[j].makes & bit) != 0;
        if (!anywhere) say(i, std::string("reads ") + data::NameOf(bit) + " of the last frame, which no step makes");
      }
    }
    if ((row.reads_previous & ~data::kAll) != 0 || (reads & ~data::kAll) != 0 || (row.makes & ~data::kAll) != 0) say(i, "a data bit that does not exist");
    for (const Dep& dep : row.deps) {
      if (dep.before == Step::kNone) continue;
      const size_t before = PositionIn(rows, n, dep.before);
      if (before >= i) say(i, std::string("must come after '") + (before < n ? rows[before].name : "a step that is not in the table") + "' and does not");
      if (dep.why[0] == '\0') say(i, "a dependency without its reason");
    }
    // the wants
    if ((row.wants_planes & ~gbuffer::kAllPlanes) != 0) say(i, "a plane that does not exist is wanted");
    if ((row.wants_screen & ~kScreenData) != 0) say(i, "a screen product that does not exist is wanted");
    if (row.declares_for != Step::kNone) {
      const size_t at = PositionIn(rows, n, row.declares_for);
      if (at >= n || (rows[at].wants_planes == 0 && rows[at].wants_screen == 0 && !rows[at].wants_lights)) say(i, "declares the wants of a step that wants nothing");
      if (rows[i].phase != Phase::kBegin) say(i, "only a frame-start step declares wants");
    }
  }
  // every want is made by somebody: the planes by the publish, the screen products by a step that makes them
  uint32_t made = 0;
  for (size_t i = 0; i < n; ++i) made |= rows[i].makes;
  bool planes_made = false;
  for (size_t i = 0; i < n; ++i) planes_made = planes_made || (rows[i].makes & data::kPlanes) != 0;
  for (size_t i = 0; i < n; ++i) {
    if ((rows[i].wants_screen & ~made) != 0) say(i, "wants a screen product that no step makes");
    if (rows[i].wants_planes != 0 && !planes_made) say(i, "wants planes that no step publishes");
    // a step that reads the screen products or the planes asks for them (its own want, or its umbrella's)
    const Row* asking = &rows[i];
    if (rows[i].kind == Kind::kInner) {
      const size_t at = PositionIn(rows, n, rows[i].umbrella);
      if (at < n) asking = &rows[at];
    }
    if (rows[i].kind != Kind::kInner && rows[i].kind != Kind::kScanned) {
      const uint32_t screen_reads = (rows[i].reads | rows[i].soft_reads) & kScreenData;
      if ((screen_reads & ~asking->wants_screen) != 0) say(i, "reads screen products it does not Want");
    }
    if (((rows[i].reads | rows[i].soft_reads) & data::kPlanes) != 0 && asking->wants_planes == 0 && rows[i].kind != Kind::kScanned) say(i, "reads the planes and does not Want them");
  }
  return out;
}

// The lag of a want, in frames: 0 when the step that declares it comes before the step that reads it at the frame's start, else 1 (the owner has read last frame's).
inline uint32_t WantLag(const Row* rows, size_t n, Step declarer, uint32_t kind) {
  size_t reader = n;
  for (size_t i = 0; i < n; ++i) {
    if (rows[i].phase == Phase::kBegin && (rows[i].reads_wants & kind) != 0) reader = i;
  }
  const size_t at = PositionIn(rows, n, declarer);
  if (at >= n || reader >= n) return 0xFFFFFFFFu;
  return at < reader ? 0u : 1u;
}
inline uint32_t WantLag(Step declarer, uint32_t kind) { return WantLag(kRows.data(), kRows.size(), declarer, kind); }

}  // namespace rules

// What a frame always has before its first noted step: the data the table's frame-start, draw and present rows make.
inline constexpr uint32_t kAlwaysThere = rules::AlwaysThere(kRows.data(), kRows.size());

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// the run-time record: Scene notes each noted step that ran; the frame's notes are checked at the present
// ---------------------------------------------------------------------------------------------------------------------------------------------------
class Recorder {
 public:
  enum class Problem : uint8_t {
    kNotNoted,    // a step that is not a noted call
    kTwice,       // a step noted again in the same frame (a second trigger)
    kOutOfOrder,  // a step noted after one that comes later in the table
    kReadMissing, // a hard read that no earlier step made (and that the frame's start does not make)
    kCount,
  };
  struct Finding {
    Problem problem = Problem::kNotNoted;
    Step step = Step::kNone;
    Step other = Step::kNone;  // kOutOfOrder: the later step already noted
    uint32_t bits = 0;         // kReadMissing: the data
  };
  struct Report {
    std::string census;    // the first frame of each set of steps that ran: "frame N: a > b > c" (empty otherwise)
    std::string problems;  // the findings not reported before (empty otherwise)
    bool any_findings = false;
  };
  static constexpr size_t kMaxFindings = 8;
  static constexpr size_t kMaxSets = 24;
  static constexpr size_t kMaxReported = 48;

  // `assert_on_inversion`: a debug build (NDEBUG off) asserts where a pass is noted before one the table puts after it. A test that makes the mistake on purpose turns it off.
  explicit Recorder(bool assert_on_inversion = true) : assert_on_inversion_(assert_on_inversion) { Reset(); }

  // A step of the frame ran (a recorded pass). `ran` false notes nothing (the call returned that it recorded nothing).
  void Note(Step step, bool ran = true) {
    if (!ran) return;
    if (size_t(step) >= kStepCount || RowOf(step).kind != Kind::kNoted) {
      Find({Problem::kNotNoted, step, Step::kNone, 0});
      return;
    }
    const uint64_t bit = uint64_t(1) << size_t(step);
    if ((ran_ & bit) != 0) {
      repeated_ = true;
      Find({Problem::kTwice, step, Step::kNone, 0});
      return;
    }
    if (last_ != Step::kNone && size_t(step) < size_t(last_) && !repeated_) {
      Find({Problem::kOutOfOrder, step, last_, 0});
      assert(!assert_on_inversion_ && "frame_order: a pass ran before one the table says it must follow (see frame_order.h)");
    }
    // the hard reads of the step and of the steps inside its call, in order, against what is there
    uint32_t there = made_;
    const size_t first = size_t(step);
    for (size_t i = first; i < kStepCount; ++i) {
      const Row& row = kRows[i];
      if (i != first && !(row.kind == Kind::kInner && row.umbrella == step)) break;
      const uint32_t missing = row.reads & ~there;
      if (missing != 0) Find({Problem::kReadMissing, row.step, Step::kNone, missing});
      there |= row.makes;
    }
    made_ = there;
    ran_ |= bit;
    if (last_ == Step::kNone || size_t(step) > size_t(last_)) last_ = step;
    order_.push_back(step);
  }

  // The guest frame is over: the report, and the record starts again.
  Report End(uint64_t frame) {
    Report report;
    if (ran_ != 0) {
      bool seen = false;
      for (uint64_t set : sets_) seen = seen || set == ran_;
      if (!seen && sets_.size() < kMaxSets) {
        sets_.push_back(ran_);
        report.census = "frame " + std::to_string(frame) + ": " + Describe(order_);
      }
    }
    report.any_findings = finding_count_ != 0;
    for (size_t i = 0; i < finding_count_; ++i) {
      const Finding& f = findings_[i];
      const uint32_t key = uint32_t(f.problem) | (uint32_t(f.step) << 4) | (uint32_t(f.other) << 12) | (f.bits << 20);
      bool reported = false;
      for (uint32_t k : reported_) reported = reported || k == key;
      if (reported || reported_.size() >= kMaxReported) continue;
      reported_.push_back(key);
      if (!report.problems.empty()) report.problems += "; ";
      report.problems += "frame " + std::to_string(frame) + ": " + Describe(f);
    }
    ++frames_;
    if (report.any_findings) ++frames_with_findings_;
    Reset();
    return report;
  }

  bool asserts_on_inversion() const { return assert_on_inversion_; }
  uint64_t frames() const { return frames_; }
  uint64_t frames_with_findings() const { return frames_with_findings_; }
  // The steps that have been noted in the frame so far, in the order they were noted.
  const std::vector<Step>& order() const { return order_; }
  size_t finding_count() const { return finding_count_; }
  const Finding& finding(size_t i) const { return findings_[i]; }

  static std::string Describe(const std::vector<Step>& order) {
    std::string text;
    for (size_t i = 0; i < order.size(); ++i) {
      if (i != 0) text += " > ";
      text += size_t(order[i]) < kStepCount ? RowOf(order[i]).name : "?";
    }
    return text;
  }
  static std::string Describe(const Finding& f) {
    const std::string step = size_t(f.step) < kStepCount ? RowOf(f.step).name : "?";
    switch (f.problem) {
      case Problem::kNotNoted: return "'" + step + "' is not a step the Scene notes";
      case Problem::kTwice: return "'" + step + "' ran twice in one frame (a second trigger?)";
      case Problem::kOutOfOrder: return "'" + step + "' ran after '" + (size_t(f.other) < kStepCount ? RowOf(f.other).name : "?") + "', which the table puts after it";
      case Problem::kReadMissing: {
        std::string bits;
        for (uint32_t bit = 1; bit != 0 && bit <= data::kAll; bit <<= 1) {
          if ((f.bits & bit) != 0) bits += std::string(bits.empty() ? "" : ", ") + data::NameOf(bit);
        }
        return "'" + step + "' ran without " + bits + ", which no earlier step of the frame made";
      }
      case Problem::kCount: break;
    }
    return "?";
  }

 private:
  void Reset() {
    ran_ = 0;
    last_ = Step::kNone;
    made_ = kAlwaysThere;
    repeated_ = false;
    order_.clear();
    finding_count_ = 0;
  }
  void Find(const Finding& finding) {
    if (finding_count_ < kMaxFindings) findings_[finding_count_++] = finding;
  }

  bool assert_on_inversion_ = true;
  uint64_t ran_ = 0;
  Step last_ = Step::kNone;
  uint32_t made_ = 0;
  bool repeated_ = false;
  std::vector<Step> order_;
  std::array<Finding, kMaxFindings> findings_{};
  size_t finding_count_ = 0;
  std::vector<uint64_t> sets_;
  std::vector<uint32_t> reported_;
  uint64_t frames_ = 0;
  uint64_t frames_with_findings_ = 0;
};

}  // namespace mcla::gpu::frame_order
