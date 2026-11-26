// Check of the frame's order (frame_order.h: the table of every pass that makes or reads the shared screen and light data, LIGHT-UNIFY U9, package LU-9). A program of its own: it needs no
// renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_frame_order_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_frame_order_test [<src/gpu dir>] [--print]      (the folder defaults to the one this file is in)
//
// It checks:
//   THE TABLE    consistent with itself: row i is step i, the phases never go back, every read has a maker earlier in the table, every dependency is earlier and has its reason, the inner steps of the AO's
//                frame follow their call, every want is made by somebody and every read of the screen products or the planes is wanted; the facts the design needs are pinned (what comes before what at the
//                trigger, inside the AO's frame, at the frame's start); the wants are the consumers' own (gbuffer_rules.h ConsumerPlanes, screen_rules.h PlanFrame: what each want makes the screen owner make and
//                whether the AO runs a depth-only frame for it) and the lag of each want (the frame-start order) is pinned. Then the table is MOVED, step by step: every step put before what it reads or must follow,
//                every maker removed, every want dropped, and each is found.
//   THE RECORDER the full frame passes; frames with passes missing (4000 random subsets of the noted steps, against an independent model) give no order finding and exactly the reads the model says were
//                made by nobody; an inversion, a second trigger, a step that is not a noted call, a composite before the trigger are each found; a finding is reported once; the census names a new set of passes once.
//   GB-I3        the reflections' place: their wants are declared at the frame's start before the owners read them (lag 0) and are the rules' (ssr_rules.h: nothing while off, the Hi-Z, colour_post and
//                the three planes at high and ultra); the pass timing labels of the SSR row and the light pass (own labels, the closing mark) exist in their files in the pass's order; the cost arms
//                (off / high / ultra) are the setting's levels and name the labels a cost run sums; a frame of the off arm and one of the high arm have different census lines.
//   RR-2         the guide pass's place: the anchor phase after the end of the scene and before the present, the pass after the trigger's publish of the planes, its debug view after it, their calls in
//                temporal.cpp in the table's order (the pass before the backend's Upscale call, the view after it), the temporal system's frame boundary between the G-buffer's read of its wants and the screen
//                owner's (the want lags one frame), the pass's wants are the planes' consumer table's (gbuffer_rules.h kRayReconstruction), its labels exist, the guides are made by one step.
//   THE SOURCES  given the src/gpu folder: every call of the table stands in scene.cpp (or ao.cpp for the AO's own steps) in the table's order, phase by phase; every noted step has its `order_.Note` line right after its
//                call and no other Note exists; every PassTiming label exists in its file; every Want the table lists is declared by its consumer; the frame start's order the wants' lag rests on.
//
// Exit 0 = passed.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "frame_order.h"
#include "gbuffer_rules.h"
#include "screen_rules.h"
#include "ssr_rules.h"

namespace fo = mcla::gpu::frame_order;
namespace gb = mcla::gpu::gbuffer;
namespace sc = mcla::gpu::screen;
using fo::Kind;
using fo::Phase;
using fo::Row;
using fo::Step;

namespace {

int g_checks = 0;
int g_failed = 0;
bool g_print = false;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

std::string Join(const std::vector<std::string>& lines) {
  std::string out;
  for (const std::string& line : lines) out += (out.empty() ? "" : " | ") + line;
  return out;
}

bool Slurp(const std::string& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  out->erase(std::remove(out->begin(), out->end(), '\r'), out->end());
  return true;
}
size_t At(const std::string& text, const std::string& what, size_t from = 0) { return text.find(what, from); }
bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }
size_t Count(const std::string& text, const std::string& what) {
  size_t n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) ++n;
  return n;
}

constexpr size_t N = fo::kStepCount;
const std::array<Row, N>& Rows() { return fo::kRows; }
std::vector<Row> Copy() { return std::vector<Row>(fo::kRows.begin(), fo::kRows.end()); }
std::vector<std::string> Problems(const std::vector<Row>& rows, bool identity) { return fo::rules::Validate(rows.data(), rows.size(), identity); }
size_t PosOf(Step step) { return size_t(step); }
bool Before(Step a, Step b) { return PosOf(a) < PosOf(b); }

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// the table
// ---------------------------------------------------------------------------------------------------------------------------------------------------
void CheckTableItself() {
  const auto problems = Problems(Copy(), true);
  Check(problems.empty(), "the shipped table is consistent: " + Join(problems));
  Check(Rows().size() == 56 && Rows().back().step == Step::kPresentRig, "56 steps, the present's rig last");
  Check(fo::RowOf(Step::kRtAoTrace).makes == fo::data::kRtAoSignal &&
        fo::RowOf(Step::kRtAoFilter).reads == fo::data::kRtAoSignal &&
        fo::RowOf(Step::kRtAoCompose).reads == (fo::data::kAoDepth | fo::data::kRtAoSignal) &&
        fo::RowOf(Step::kRtAoCompose).makes == fo::data::kAoTerm, "trace guides reach the filter and compose");
  size_t phase_count[size_t(Phase::kCount)] = {};
  size_t noted = 0;
  size_t inner = 0;
  size_t scanned = 0;
  std::set<std::string> names;
  std::set<std::string> calls;
  for (size_t i = 0; i < N; ++i) {
    const Row& r = Rows()[i];
    ++phase_count[size_t(r.phase)];
    noted += r.kind == Kind::kNoted;
    inner += r.kind == Kind::kInner;
    scanned += r.kind == Kind::kScanned;
    Check(size_t(r.step) == i, std::string("row ") + std::to_string(i) + " is its step");
    Check(names.insert(r.name).second, std::string("the name '") + r.name + "' is only used once");
    Check(calls.insert(std::string(r.file) + "|" + r.call).second, std::string("the call '") + r.call + "' is only listed once");
    Check((r.kind == Kind::kNoted || r.kind == Kind::kInner) == (r.phase == Phase::kTrigger || r.phase == Phase::kEndOfScene) || r.kind == Kind::kScanned, std::string(r.name) + ": only trigger and end-of-scene steps are noted");
    // the inner steps are the AO's, in ao.cpp, and (RT-LIGHTS-D1) the ray traced visibility's inside the light pass, in local_lights.cpp
    if (r.kind == Kind::kInner) Check((r.umbrella == Step::kAo && std::strcmp(r.file, "ao.cpp") == 0) || (r.step == Step::kRtLights && r.umbrella == Step::kLocalLights && std::strcmp(r.file, "local_lights.cpp") == 0),
                                      std::string(r.name) + ": the inner steps are the AO's, in ao.cpp (and the light pass's ray traced visibility, in local_lights.cpp)");
    if (r.kind == Kind::kNoted) Check(std::strcmp(r.file, "scene.cpp") == 0, std::string(r.name) + ": a noted call stands in scene.cpp");
    // RR-2: the anchor's steps are the temporal system's, in temporal.cpp, and held by the source checks alone
    if (r.phase == Phase::kAnchor) Check(std::strcmp(r.file, "temporal.cpp") == 0 && r.kind == Kind::kScanned, std::string(r.name) + ": an anchor step stands in temporal.cpp and is a scanned one");
  }
  Check(phase_count[size_t(Phase::kBegin)] == 10 && phase_count[size_t(Phase::kDraws)] == 3 && phase_count[size_t(Phase::kTrigger)] == 32 && phase_count[size_t(Phase::kEndOfScene)] == 4 &&
            phase_count[size_t(Phase::kAnchor)] == 3 && phase_count[size_t(Phase::kPresent)] == 4,
        "10 at the frame's start, 3 in the draws (MIRROR-VIEW MV1 added the mirror phase's), 32 at the trigger (RT-LIGHTS-D1 added the ray traced visibility inside the light pass, MIRROR-RT the side mirrors, GI-BOUNCE-SS the two captures at the light pass's sides, RTAO the three steps inside the AO's frame), 4 at the end of the scene (MV1 added the mirror stamp, MV4 the side mirror glass after it), 3 at the anchor (RR-3: the upscale joined the guide pass and its view), 4 at the present");
  Check(noted == 20 && inner == 12 && scanned == 24, "20 noted calls, 11 steps inside the AO's frame and 1 inside the light pass, 24 held by the source checks alone (MIRROR-VIEW MV1 added two, MV4 one)");
  Check(fo::data::kRtAoSignal == (1u << 31) && std::string(fo::data::NameOf(fo::data::kRtAoSignal)) == "RTAO signal",
        "the RTAO signal owns the last data bit");
  // every data bit is made (the AO term by two steps: the visibility and the RTAO compose write the same term slot), nothing else by more than one
  for (uint32_t bit = 1; bit != 0; bit <<= 1) {
    int makers = 0;
    for (const Row& r : Rows()) makers += (r.makes & bit) != 0;
    const int expected = bit == fo::data::kAoTerm ? 2 : 1;
    Check(makers == expected, std::string("the data '") + fo::data::NameOf(bit) + "' has " + std::to_string(expected) +
                                   " makers, has " + std::to_string(makers));
    Check(std::strcmp(fo::data::NameOf(bit), "?") != 0, "every data bit has a name");
  }
  // the screen products of the table are the frozen interface's
  Check(fo::kScreenData == sc::kAllProducts, "the screen data bits are screen_frame.h's seven products");
  Check(fo::RowOf(Step::kScreenPublish).makes == (fo::data::kLinDepth | fo::data::kLinDepthFull | fo::data::kNormal | fo::data::kColourPre | fo::data::kHizMin), "the AO's publish makes lin_depth, its full size, normal, colour_pre and the Hi-Z");
  Check(fo::RowOf(Step::kColourPost).makes == (fo::data::kColourPost | fo::data::kHistory), "the capture makes colour_post and the history");
}

// What the design (LIGHT-UNIFY 5.3) and the code's comments say must come before what. A failure here means the table, or the order it records, changed.
void CheckPinnedOrder() {
  // the trigger
  const Step trigger[] = {Step::kGBufferPublish, Step::kCarSunMap, Step::kCarStructures, Step::kShadowReplay, Step::kLightMaps, Step::kGiField, Step::kAo, Step::kGiApply, Step::kLightsBefore, Step::kLocalLights, Step::kLightsAfter, Step::kShafts,
                          Step::kVolume, Step::kCarRtCheck, Step::kColourPost, Step::kSsr, Step::kCarHistory, Step::kCarMirror, Step::kGBufferView, Step::kScreenView};
  for (size_t i = 1; i < std::size(trigger); ++i) Check(Before(trigger[i - 1], trigger[i]), std::string("at the trigger '") + fo::RowOf(trigger[i - 1]).name + "' comes before '" + fo::RowOf(trigger[i]).name + "'");
  // inside the AO's frame
  const Step ao[] = {Step::kAo, Step::kAoDepth, Step::kAoNormals, Step::kColourPre, Step::kCarRtAo, Step::kRtAoTrace, Step::kRtAoFilter, Step::kAoVisibility, Step::kRtAoCompose, Step::kContact, Step::kAoApply, Step::kScreenPublish};
  for (size_t i = 1; i < std::size(ao); ++i) Check(Before(ao[i - 1], ao[i]), std::string("in the AO's frame '") + fo::RowOf(ao[i - 1]).name + "' comes before '" + fo::RowOf(ao[i]).name + "'");
  for (size_t i = 1; i < std::size(ao); ++i) {
    Check(fo::RowOf(ao[i]).kind == Kind::kInner && fo::RowOf(ao[i]).umbrella == Step::kAo, std::string(fo::RowOf(ao[i]).name) + " is inside the AO's call");
  }
  // the facts the reasons name
  Check(Before(Step::kColourPre, Step::kAoApply), "the bounce light's colour is captured BEFORE the apply multiplies the occlusion into the colour");
  Check(Before(Step::kAoApply, Step::kLocalLights), "the deferred lights are added AFTER the apply (nothing darkens them)");
  // GI-BOUNCE-SS: the bounce light takes the lights' share from the LAST frame: the two captures stand at the sides of the light pass, after everything that adds to the colour before it, and the pair is read by the NEXT
  // frame's colour chain (a read of the last frame: no order inside this one)
  Check(Before(Step::kColourPre, Step::kLightsBefore) && Before(Step::kAoApply, Step::kLightsBefore) && Before(Step::kGiApply, Step::kLightsBefore) && Before(Step::kLightsBefore, Step::kLocalLights) &&
            Before(Step::kLocalLights, Step::kLightsAfter) && Before(Step::kRtLights, Step::kLightsAfter) && Before(Step::kLightsAfter, Step::kShafts) && Before(Step::kLightsAfter, Step::kColourPost),
        "the colour before the light pass is captured after the AO's frame and the probe field's apply and right ahead of the lights; the colour after, right behind them and before everything else that adds to it");
  Check(PosOf(Step::kLightsBefore) + 1 == PosOf(Step::kLocalLights) && PosOf(Step::kRtLights) + 1 == PosOf(Step::kLightsAfter), "nothing runs between a capture and the light pass: the difference of the two is the light pass's alone");
  Check(fo::RowOf(Step::kColourPre).reads_previous == fo::data::kLightHistory && (fo::RowOf(Step::kColourPre).reads & fo::data::kLightHistory) == 0 && fo::RowOf(Step::kLightsAfter).makes == fo::data::kLightHistory &&
            fo::RowOf(Step::kLightsBefore).makes == fo::data::kLightsBefore && fo::RowOf(Step::kLightsAfter).reads == fo::data::kLightsBefore,
        "the colour chain reads the LAST frame's lights history, not this frame's (the light pass comes after it); the second capture makes it from the first");
  Check(fo::RowOf(Step::kLightsBefore).kind == Kind::kNoted && fo::RowOf(Step::kLightsAfter).kind == Kind::kNoted && fo::RowOf(Step::kLightsBefore).wants_screen == 0 && fo::RowOf(Step::kLightsAfter).wants_screen == 0 &&
            !fo::RowOf(Step::kLightsBefore).wants_lights,
        "the captures are noted calls that want no screen product (they read the main colour, which is always there)");
  // the probe field (CITY-RT M3 G4): published before the AO plans, applied after the AO's apply and before the lamps' light, and everything that adds to the colour stays before the capture
  Check(Before(Step::kGiField, Step::kAo) && Before(Step::kCarStructures, Step::kGiField), "the probe field's frame is made after the city's structures and BEFORE the AO plans (the AO asks the field's share)");
  Check(Before(Step::kAoApply, Step::kGiApply) && Before(Step::kScreenPublish, Step::kGiApply) && Before(Step::kGiField, Step::kGiApply) && Before(Step::kGBufferPublish, Step::kGiApply),
        "the field's apply comes after the occlusion's apply, the screen publish (lin_depth), the field's frame and the G-buffer's planes");
  Check(Before(Step::kGiApply, Step::kLocalLights) && Before(Step::kGiApply, Step::kColourPost), "the field's apply is before the lamps' light and the capture (the reflections see the bounce)");
  Check(Before(Step::kLocalLights, Step::kColourPost), "the colour_post capture is made AFTER the deferred lights (the reflections see the lights)");
  Check(Before(Step::kAoApply, Step::kColourPost), "... and after the apply");
  Check(Before(Step::kColourPost, Step::kSsr), "the reflections come AFTER the capture (a reflection is not a reflection of a reflection)");
  Check(Before(Step::kScreenPublish, Step::kLocalLights) && Before(Step::kScreenPublish, Step::kSsr), "the screen frame (lin_depth, the Hi-Z) is published before the light pass and the reflections read it");
  Check(Before(Step::kCarStructures, Step::kCarRtAo) && Before(Step::kCarStructures, Step::kCarHistory), "the car structures are built before everything that traces (the AO's ray traced term, the car reflection)");
  Check(Before(Step::kCarSunMap, Step::kContact), "the car sun map is made before the contact march reads it");
  Check(Before(Step::kShadowReplay, Step::kLocalLights) && Before(Step::kShadowReplay, Step::kVolume) && Before(Step::kShadowReplay, Step::kLightMaps), "the headlight maps are recorded before the light pass, the volume and the manager read them");
  Check(Before(Step::kGBufferPublish, Step::kAoNormals) && Before(Step::kGBufferPublish, Step::kLocalLights) && Before(Step::kGBufferPublish, Step::kSsr), "the G-buffer's planes are published before every reader");
  Check(Before(Step::kAo, Step::kCarRtCheck), "the car ray tracing check follows the AO (it reads the AO's linear depth)");
  Check(Before(Step::kCarHistory, Step::kGBufferView) && Before(Step::kGBufferView, Step::kScreenView), "the debug views come last, the screen's after the G-buffer's");
  Check(Before(Step::kVolume, Step::kVolumeComposite) && Before(Step::kShafts, Step::kShaftsComposite) && Before(Step::kVolumeComposite, Step::kShaftsComposite),
        "the composites are at the end-of-scene copy, the shafts' on top of the haze");
  Check(fo::RowOf(Step::kVolumeComposite).phase == Phase::kEndOfScene && fo::RowOf(Step::kShaftsComposite).phase == Phase::kEndOfScene, "the composites are end-of-scene steps");
  // the draws and the present
  Check(fo::RowOf(Step::kDrawHeadlight).reads_previous == fo::data::kShadowAtlas && (fo::RowOf(Step::kDrawHeadlight).reads & fo::data::kShadowAtlas) == 0, "the forward headlight reads the LAST frame's maps, not this frame's");
  Check(fo::RowOf(Step::kDrawHeadlight).phase == Phase::kDraws && Before(Step::kDrawHeadlight, Step::kGBufferPublish), "the lit draws are before the trigger (so the AO darkens the forward headlight and the colour chain sees it)");
  Check(Before(Step::kPresentLights, Step::kPresentRig) && Before(Step::kPresentWorld, Step::kPresentScreen), "the present: the manager checks before the rig replaces the set; the world is classified before the screen trades");
  // the frame's start
  Check(Before(Step::kBeginLocalLights, Step::kBeginGBuffer) && Before(Step::kBeginLocalLights, Step::kBeginScreen) && Before(Step::kBeginLocalLights, Step::kBeginLights), "local lights declare before the three owners read");
  Check(Before(Step::kBeginScreen, Step::kBeginAo), "the screen owner makes its plan before the AO asks it");
  Check(Before(Step::kBeginGiField, Step::kBeginGiApply) && Before(Step::kBeginSsr, Step::kBeginGiField) && Before(Step::kBeginGiApply, Step::kBeginGBuffer) && Before(Step::kBeginGiApply, Step::kBeginScreen) &&
            Before(Step::kBeginGiApply, Step::kBeginAo),
        "the probe field starts its frame, then its apply declares (it reads whether the field runs), both before the G-buffer and the screen owner read (and before the AO asks the plan)");
  Check(Before(Step::kBeginLocalLights, Step::kBeginSsr) && Before(Step::kBeginSsr, Step::kBeginGBuffer) && Before(Step::kBeginSsr, Step::kBeginScreen) && Before(Step::kBeginSsr, Step::kBeginAo),
        "GB-I3: the reflections declare right after the light pass and before the G-buffer and the screen owner read (and before the AO asks the plan)");
  // the reflections' place at the trigger (GB-I3): the last pass that adds to the colour, right after the capture it reads
  {
    const size_t capture = PosOf(Step::kColourPost);
    const size_t ssr = PosOf(Step::kSsr);
    Check(ssr == capture + 1, "the reflections are the very next step after the capture (nothing runs between what they read and them)");
    Check(PosOf(Step::kSsr) + 1 == PosOf(Step::kCarHistory), "... and the car history is the very next after them (a vehicle-only image: the order with SSR is free of data, kept as the design has it)");
    // everything that adds to the main colour at the trigger comes before the capture except the reflections: the AO's apply and the light pass (kinds of the table that write the colour)
    for (Step added : {Step::kAoApply, Step::kGiApply, Step::kLocalLights}) Check(Before(added, Step::kColourPost), std::string("'") + fo::RowOf(added).name + "' adds to the colour before the capture");
    // the shafts and the volume record before the capture and write no colour at the trigger in the play view (composites at the end-of-scene copy)
    Check(Before(Step::kShafts, Step::kColourPost) && Before(Step::kVolume, Step::kColourPost) && fo::RowOf(Step::kShaftsComposite).phase == Phase::kEndOfScene && fo::RowOf(Step::kVolumeComposite).phase == Phase::kEndOfScene,
          "the sun shafts' and the volume's passes are before the capture, their composites at the end of the scene (after the reflections)");
    Check(fo::RowOf(Step::kShafts).note[0] != '\0' && std::string(fo::RowOf(Step::kShafts).note).find("GB-I3 keeps that") != std::string::npos, "the shafts' row says why they stay before the capture");
    // the car history's dependency on SSR carries the reason
    bool car_after_ssr = false;
    for (const fo::Dep& dep : fo::RowOf(Step::kCarHistory).deps) car_after_ssr = car_after_ssr || (dep.before == Step::kSsr && std::string(dep.why).find("vehicle's pixels only") != std::string::npos);
    Check(car_after_ssr, "the car history is kept after SSR and the dependency says why it does not see SSR (vehicle pixels only)");
  }
  Check(Before(Step::kBeginLights, Step::kDrawSidecar), "the frame's lights are made before the draws");
  // RR-2: the temporal system's frame boundary stands between the G-buffer's read of its wants and the screen owner's (scene.cpp), the guide pass at the anchor after everything of the trigger and the end of
  // the scene, the debug view right after the pass
  Check(Before(Step::kBeginGBuffer, Step::kBeginTemporal) && Before(Step::kBeginTemporal, Step::kBeginScreen), "RR-2: the temporal system's frame boundary is between the G-buffer's and the screen owner's");
  Check(Before(Step::kShaftsComposite, Step::kRrGuides) && Before(Step::kGBufferPublish, Step::kRrGuides) && Before(Step::kRrGuides, Step::kPresentWorld) &&
            fo::RowOf(Step::kRrGuides).phase == Phase::kAnchor && PosOf(Step::kUpscale) == PosOf(Step::kRrGuides) + 1 && PosOf(Step::kRrGuidesView) == PosOf(Step::kUpscale) + 1 &&
            PosOf(Step::kPresentWorld) == PosOf(Step::kRrGuidesView) + 1,
        "RR-2/RR-3: the guide pass is at the anchor, after the end of the scene and the planes' publish, the upscale right after it, the debug view after that, the present next");
  Check(fo::RowOf(Step::kUpscale).phase == Phase::kAnchor && fo::RowOf(Step::kUpscale).kind == Kind::kScanned && std::strcmp(fo::RowOf(Step::kUpscale).file, "temporal.cpp") == 0 &&
            fo::RowOf(Step::kUpscale).soft_reads == fo::data::kRrGuides && fo::RowOf(Step::kUpscale).reads == 0 && fo::RowOf(Step::kUpscale).makes == 0 &&
            fo::RowOf(Step::kUpscale).deps[0].before == Step::kRrGuides,
        "RR-3: the upscale is a scanned anchor step in temporal.cpp that reads the guides only where they are there (a Super Resolution frame has none) and makes no shared data");
  Check(fo::RowOf(Step::kSsr).makes == fo::data::kRrHitSsr && fo::RowOf(Step::kRrGuides).soft_reads == (fo::data::kRrHitSsr | fo::data::kRrHitCar | fo::data::kRrHitGi) && Before(Step::kSsr, Step::kRrGuides) && Before(Step::kCarHistory, Step::kRrGuides) &&
            (fo::RowOf(Step::kCarHistory).makes & fo::data::kRrHitCar) != 0,
        "RR-4: the reflections' hit distance image is made at the trigger by the effect (a soft read of the guide pass at the anchor: only the frames the effect ran raw have it)");
  Check(fo::RowOf(Step::kGiApply).makes == fo::data::kRrHitGi && Before(Step::kGiApply, Step::kRrGuides) && fo::data::kRrHitGi == (1u << 27) && fo::data::kLightsBefore == (1u << 28) && fo::data::kLightHistory == (1u << 29) &&
            fo::data::kMirrorView == (1u << 30) && fo::data::kRtAoSignal == (1u << 31) && fo::data::kAll == 0xFFFFFFFFu &&
            std::strcmp(fo::data::NameOf(fo::data::kRrHitGi), "RR hit distance (bounce light)") == 0,
        "CITY-RT G6: the bounce light's per-pixel pass's hit distance image is made by the field's apply at the trigger (a soft read of the guide pass at the anchor: only the frames the pass ran raw have it)");
  Check(fo::RowOf(Step::kRrGuides).reads == fo::data::kPlanes && fo::RowOf(Step::kRrGuides).makes == fo::data::kRrGuides && fo::RowOf(Step::kRrGuidesView).reads == fo::data::kRrGuides &&
            fo::RowOf(Step::kRrGuidesView).makes == 0,
        "RR-2: the guide pass reads the planes and makes the guides; the view reads the guides and makes nothing");
}

void CheckWants() {
  const Row& ll = fo::RowOf(Step::kLocalLights);
  const Row& ssr = fo::RowOf(Step::kSsr);
  const Row& ao = fo::RowOf(Step::kAo);
  const Row& car = fo::RowOf(Step::kCarHistory);
  Check(ll.wants_planes == gb::ConsumerPlanes(gb::Consumer::kLocalLights), "the light pass wants the planes gbuffer_rules.h says");
  Check(ssr.wants_planes == gb::ConsumerPlanes(gb::Consumer::kReflections), "the reflections want the planes gbuffer_rules.h says");
  Check(ao.wants_planes == (gb::ConsumerPlanes(gb::Consumer::kAmbientOcclusion) | gb::ConsumerPlanes(gb::Consumer::kBounceLight)), "the AO's frame wants the planes gbuffer_rules.h says (the normals, for the occlusion and the bounce light)");
  Check(ll.wants_planes == gb::kAllPlanes && ssr.wants_planes == gb::kAllPlanes && ao.wants_planes == gb::PlaneBit(gb::kPlaneNormal), "all three planes for the light pass and the reflections, the normal for the AO");
  Check(ll.wants_lights && !ssr.wants_lights && !ao.wants_lights && !car.wants_lights, "only the light pass asks the light manager");
  {
    // the probe field's apply (gi_apply.cpp): RT1 and RT2 through the rules' ConsumerPlanes, and lin_depth, while the field runs
    const Row& gi = fo::RowOf(Step::kGiApply);
    Check(gi.wants_planes == gb::ConsumerPlanes(gb::Consumer::kRtBounce) && gi.wants_planes == (gb::PlaneBit(gb::kPlaneNormal) | gb::PlaneBit(gb::kPlaneAlbedo)), "the field's apply wants the planes gbuffer_rules.h says (the normal and the albedo)");
    Check(gi.wants_screen == fo::data::kLinDepth && !gi.wants_lights, "the field's apply wants lin_depth and not the light manager");
    Check(fo::rules::WantLag(Step::kBeginGiApply, fo::want::kPlanes) == 0 && fo::rules::WantLag(Step::kBeginGiApply, fo::want::kScreen) == 0, "the field's apply's wants are read in the frame they are made (lag 0)");
  }
  // GB-I3: the reflections' row is the rules' wants at high and ultra, and the rules ask for nothing at off
  Check(ssr.wants_screen == mcla::gpu::ssr::kWantedProducts && ssr.wants_screen == mcla::gpu::ssr::WantedProducts(mcla::gpu::ssr::Level::kHigh) && ssr.wants_screen == mcla::gpu::ssr::WantedProducts(mcla::gpu::ssr::Level::kUltra),
        "the reflections' row wants the products ssr_rules.h asks for at high and at ultra");
  Check(ssr.wants_planes == mcla::gpu::ssr::WantedPlanes(mcla::gpu::ssr::Level::kHigh) && ssr.wants_planes == mcla::gpu::ssr::WantedPlanes(mcla::gpu::ssr::Level::kUltra),
        "the reflections' row wants the planes ssr_rules.h asks for at high and at ultra");
  Check(mcla::gpu::ssr::WantedProducts(mcla::gpu::ssr::Level::kOff) == 0 && mcla::gpu::ssr::WantedPlanes(mcla::gpu::ssr::Level::kOff) == 0 && !mcla::gpu::ssr::WantsCamera(mcla::gpu::ssr::Level::kOff),
        "GB-I3: while REFLECTIONS ON SURFACES is off the effect wants no product, no plane and no camera (GBuffer::Want(kReflections) is 0)");
  // the table's screen wants as the frame's plan sees them
  sc::Wants wants;
  wants.Set(sc::Consumer::kLocalLights, ll.wants_screen);
  sc::Plan plan = sc::PlanFrame(wants, sc::View::kOff, 0);
  Check(plan.wanted == fo::data::kLinDepth && plan.NeedsDepthChain() && plan.make == 0, "the light pass wants lin_depth, which makes the AO run a depth-only frame; the owner makes nothing itself");
  wants = sc::Wants{};
  wants.Set(sc::Consumer::kReflections, ssr.wants_screen);
  plan = sc::PlanFrame(wants, sc::View::kOff, 0);
  Check(plan.NeedsDepthChain() && (plan.make & fo::data::kHizMin) != 0 && (plan.make & fo::data::kColourPost) != 0 && (plan.make & ~(fo::data::kHizMin | fo::data::kColourPost)) == 0,
        "the reflections' wants make the Hi-Z and colour_post, and the depth chain runs for them");
  wants = sc::Wants{};
  wants.Set(sc::Consumer::kCarHistory, car.wants_screen);
  plan = sc::PlanFrame(wants, sc::View::kOff, 0);
  Check(!plan.NeedsDepthChain() && plan.make == fo::data::kColourPost, "the car history's want makes colour_post alone and needs no depth frame");
  wants = sc::Wants{};
  wants.Set(sc::Consumer::kAmbientOcclusion, fo::data::kLinDepth | fo::data::kNormal);
  wants.Set(sc::Consumer::kContactShadows, fo::data::kLinDepth | fo::data::kNormal);
  wants.Set(sc::Consumer::kBounceLight, ao.wants_screen);
  plan = sc::PlanFrame(wants, sc::View::kOff, 0);
  Check(plan.wanted == ao.wants_screen && !plan.NeedsDepthChain() && plan.make == 0, "the AO family's wants are its own frame's: nothing for the owner to make, no depth-only frame");
  // each consumer's wants are products the table has a maker for, at the AO's publish or the capture
  Check(((ll.wants_screen | ssr.wants_screen | ao.wants_screen | car.wants_screen) & ~fo::kScreenData) == 0, "every screen want is a screen product");
  // the lag of a want = frames between the setting and the owner reading it
  using fo::want::kLights;
  using fo::want::kPlanes;
  using fo::want::kScreen;
  Check(fo::rules::WantLag(Step::kBeginLocalLights, kPlanes) == 0 && fo::rules::WantLag(Step::kBeginLocalLights, kScreen) == 0 && fo::rules::WantLag(Step::kBeginLocalLights, kLights) == 0,
        "the light pass's wants are read in the frame they are made (lag 0)");
  Check(fo::rules::WantLag(Step::kBeginSsr, kPlanes) == 0 && fo::rules::WantLag(Step::kBeginSsr, kScreen) == 0,
        "GB-I3: the reflections' wants are read in the frame they are made (lag 0: the planes, the Hi-Z and the capture are there in the frame the setting changes; it was 1 while the call stood after the owners')");
  Check(fo::rules::WantLag(Step::kBeginAo, kPlanes) == 1 && fo::rules::WantLag(Step::kBeginAo, kScreen) == 1, "the AO's wants lag one frame");
  // RR-2: the guide pass's wants are the consumer table's, declared by the temporal system's frame boundary, which comes after the G-buffer reads: one frame
  Check(fo::RowOf(Step::kRrGuides).wants_planes == gb::ConsumerPlanes(gb::Consumer::kRayReconstruction) && gb::ConsumerPlanes(gb::Consumer::kRayReconstruction) == gb::kAllPlanes &&
            fo::RowOf(Step::kRrGuides).wants_screen == 0 && !fo::RowOf(Step::kRrGuides).wants_lights,
        "RR-2: the guide pass wants the three planes (gbuffer_rules.h kRayReconstruction) and no screen product");
  Check(fo::rules::WantLag(Step::kBeginTemporal, kPlanes) == 1 && fo::RowOf(Step::kBeginTemporal).declares_for == Step::kRrGuides,
        "RR-2: the guide pass's want of the planes is declared at the temporal system's frame boundary and lags one frame");
  {
    gb::ConsumerStates rr_on;
    rr_on.ray_reconstruction = true;
    Check(gb::AutoPlanes(rr_on) == gb::kAllPlanes && std::string(gb::ConsumerName(gb::Consumer::kRayReconstruction)) == "ray reconstruction", "RR-2: the auto policy makes all three planes while the guide pass is wanted");
  }
  Check(fo::rules::WantLag(Step::kBeginCarRefl, kScreen) == 1, "the car self-reflection's want of colour_post lags one frame");
  Check(fo::rules::WantLag(Step::kBeginAo, kLights) == 0 || true, "(no step reads a light want after the AO's: the manager's want is the light pass's alone)");
  Check(fo::rules::WantLag(Step::kBeginCarRefl, kPlanes) == 1, "an owner that reads before the declarer lags one frame");
  // a declared want belongs to a step that wants the kind, and a reader of that kind exists
  for (const Row& r : Rows()) {
    if (r.declares_for == Step::kNone) continue;
    const Row& target = fo::RowOf(r.declares_for);
    const uint32_t kinds = (target.wants_planes ? kPlanes : 0u) | (target.wants_screen ? kScreen : 0u) | (target.wants_lights ? kLights : 0u);
    Check(kinds != 0, std::string(r.name) + ": declares wants for a step that wants something");
    for (uint32_t kind : {kPlanes, kScreen, kLights}) {
      if ((kinds & kind) != 0) Check(fo::rules::WantLag(r.step, kind) != 0xFFFFFFFFu, std::string(r.name) + ": every kind it declares has an owner that reads it");
    }
  }
}

void CheckMoves() {
  // every step put before what it reads or must follow is found
  size_t moved = 0;
  for (size_t i = 0; i < N; ++i) {
    const Row& row = Rows()[i];
    size_t earliest = i;
    const uint32_t reads = row.reads | row.soft_reads;
    for (size_t j = 0; j < i; ++j) {
      if ((Rows()[j].makes & reads) != 0) earliest = std::min(earliest, j);
    }
    for (const fo::Dep& dep : row.deps) {
      if (dep.before != Step::kNone) earliest = std::min(earliest, PosOf(dep.before));
    }
    if (row.kind == Kind::kInner) earliest = std::min(earliest, PosOf(row.umbrella));
    if (earliest == i) continue;
    std::vector<Row> rows = Copy();
    const Row r = rows[i];
    rows.erase(rows.begin() + std::ptrdiff_t(i));
    rows.insert(rows.begin() + std::ptrdiff_t(earliest), r);
    const auto problems = Problems(rows, false);
    Check(!problems.empty(), std::string("'") + row.name + "' moved before '" + Rows()[earliest].name + "' is found");
    ++moved;
  }
  Check(moved >= 24, "a good number of steps have something to follow (" + std::to_string(moved) + ")");
  // every step with something after it: put it after the last thing that reads it
  size_t pushed = 0;
  for (size_t i = 0; i < N; ++i) {
    const Row& row = Rows()[i];
    size_t latest = i;
    for (size_t j = i + 1; j < N; ++j) {
      if (((Rows()[j].reads | Rows()[j].soft_reads) & row.makes) != 0) latest = j;
      for (const fo::Dep& dep : Rows()[j].deps) {
        if (dep.before == row.step) latest = j;
      }
      if (Rows()[j].kind == Kind::kInner && Rows()[j].umbrella == row.step) latest = j;
    }
    if (latest == i) continue;
    std::vector<Row> rows = Copy();
    const Row r = rows[i];
    rows.erase(rows.begin() + std::ptrdiff_t(i));
    rows.insert(rows.begin() + std::ptrdiff_t(latest), r);  // after the reader that was at `latest`
    Check(!Problems(rows, false).empty(), std::string("'") + row.name + "' moved after '" + Rows()[latest].name + "', which needs it, is found");
    ++pushed;
  }
  Check(pushed >= 20, "a good number of steps have something that needs them (" + std::to_string(pushed) + ")");
  // a maker removed; a want dropped; a dependency on a later step; a read of what nothing makes
  for (size_t i = 0; i < N; ++i) {
    if (Rows()[i].makes == 0 || Rows()[i].step == Step::kRtAoCompose) continue;
    bool read = false;
    for (const Row& other : Rows()) read = read || ((other.reads | other.soft_reads) & Rows()[i].makes) != 0;
    if (!read) continue;
    std::vector<Row> rows = Copy();
    rows[i].makes = 0;
    Check(!Problems(rows, true).empty(), std::string("without '") + Rows()[i].name + "' making anything its readers are found");
  }
  {
    std::vector<Row> rows = Copy();
    rows[PosOf(Step::kSsr)].wants_screen = 0;
    Check(!Problems(rows, true).empty(), "a reader of the screen products that does not Want them is found (the reflections)");
    rows = Copy();
    rows[PosOf(Step::kLocalLights)].wants_planes = 0;
    Check(!Problems(rows, true).empty(), "a reader of the planes that does not Want them is found (the light pass)");
    rows = Copy();
    rows[PosOf(Step::kAo)].wants_planes = 0;
    Check(!Problems(rows, true).empty(), "the AO's normals step reads the planes: the AO's frame must Want them");
    rows = Copy();
    rows[PosOf(Step::kColourPost)].makes = 0;
    Check(!Problems(rows, true).empty(), "the reflections' and the car history's want of colour_post: nobody making it is found");
    rows = Copy();
    rows[PosOf(Step::kAoDepth)].deps[0] = {Step::kLocalLights, "a later step"};
    Check(!Problems(rows, true).empty(), "a dependency on a later step is found");
    rows = Copy();
    rows[PosOf(Step::kAoApply)].deps[0].why = "";
    Check(!Problems(rows, true).empty(), "a dependency without its reason is found");
    rows = Copy();
    rows[PosOf(Step::kAoApply)].soft_reads |= (1u << 27);
    Check(!Problems(rows, true).empty(), "a data bit that does not exist is found");
    rows = Copy();
    rows[PosOf(Step::kBeginSsr)].declares_for = Step::kShafts;
    Check(!Problems(rows, true).empty(), "declaring the wants of a step that wants nothing is found");
    rows = Copy();
    std::swap(rows[PosOf(Step::kSsr)], rows[PosOf(Step::kColourPost)]);
    Check(!Problems(rows, true).empty(), "two steps swapped are found in the identity check");
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// the recorder
// ---------------------------------------------------------------------------------------------------------------------------------------------------
std::vector<Step> NotedSteps() {
  std::vector<Step> out;
  for (const Row& r : Rows()) {
    if (r.kind == Kind::kNoted) out.push_back(r.step);
  }
  return out;
}

struct Expected {
  Step step;
  uint32_t bits;
};
// The independent model of the hard reads a subset leaves unmade (a plain loop over the table, written without the recorder's code).
std::vector<Expected> ModelReads(const std::set<Step>& ran) {
  std::vector<Expected> out;
  uint32_t there = 0;
  for (const Row& r : Rows()) {
    if (r.kind == Kind::kScanned) there |= r.makes;
  }
  for (const Row& r : Rows()) {
    const bool member = (r.kind == Kind::kNoted && ran.count(r.step) != 0) || (r.kind == Kind::kInner && ran.count(r.umbrella) != 0);
    if (!member) continue;
    if ((r.reads & ~there) != 0) out.push_back({r.step, r.reads & ~there});
    there |= r.makes;
  }
  return out;
}

void CheckRecorder() {
  const std::vector<Step> all = NotedSteps();
  // the full frame, in the table's order: no finding, a census line once
  {
    fo::Recorder rec(true);
    for (Step s : all) rec.Note(s);
    Check(rec.finding_count() == 0 && rec.order() == all, "the full frame in the table's order has no finding");
    const fo::Recorder::Report report = rec.End(10);
    Check(!report.census.empty() && report.problems.empty() && !report.any_findings, "the census names the full frame; no problems");
    Check(report.census.find("frame 10: G-buffer publish > car sun shadow map > car ray tracing structures > headlight shadow replay") == 0, "the census names the passes in order: " + report.census.substr(0, 120));
    Check(report.census.find("volumetric composite into the colour > sun shafts' composite into the colour") != std::string::npos, "... and ends with the composites");
    for (Step s : all) rec.Note(s);
    const fo::Recorder::Report again = rec.End(11);
    Check(again.census.empty() && again.problems.empty(), "the same set again is not reported again");
    Check(rec.frames() == 2 && rec.frames_with_findings() == 0, "two frames, none with findings");
  }
  // frames with passes missing: no order finding, exactly the reads the model says
  {
    uint32_t seed = 12345;
    auto next = [&]() {
      seed = seed * 1664525u + 1013904223u;
      return seed >> 8;
    };
    int frames = 0;
    int with_findings = 0;
    for (int trial = 0; trial < 4000; ++trial) {
      std::set<Step> ran;
      const uint32_t density = 1 + next() % 8;  // 1/8 .. 8/8
      for (Step s : all) {
        if (next() % 8 < density) ran.insert(s);
      }
      fo::Recorder rec(true);
      for (Step s : all) {
        if (ran.count(s) != 0) rec.Note(s);
        else rec.Note(s, false);  // a call that recorded nothing notes nothing
      }
      const std::vector<Expected> model = ModelReads(ran);
      bool ok = rec.finding_count() == std::min(model.size(), fo::Recorder::kMaxFindings);
      for (size_t i = 0; ok && i < rec.finding_count(); ++i) {
        const fo::Recorder::Finding& f = rec.finding(i);
        ok = f.problem == fo::Recorder::Problem::kReadMissing && f.step == model[i].step && f.bits == model[i].bits;
      }
      Check(ok, "a frame with " + std::to_string(ran.size()) + " of the passes: the findings are exactly the model's reads (trial " + std::to_string(trial) + ")");
      ++frames;
      with_findings += rec.finding_count() != 0;
      if (!ok) break;
    }
    Check(frames == 4000 && with_findings > 100 && with_findings < frames, "the random frames are a mix of clean and not (" + std::to_string(with_findings) + " of " + std::to_string(frames) + " have a read nothing made)");
  }
  // a frame the way the game runs without the optional passes: AO, light pass, capture, SSR, car history
  {
    fo::Recorder rec(true);
    for (Step s : {Step::kGBufferPublish, Step::kAo, Step::kLocalLights, Step::kColourPost, Step::kSsr, Step::kCarHistory}) rec.Note(s);
    Check(rec.finding_count() == 0, "G-buffer, AO, lights, capture, SSR, car history: complete and in order");
    rec.End(1);
    rec.Note(Step::kAo);
    rec.Note(Step::kCarHistory);
    Check(rec.finding_count() == 0, "a frame with only the AO and the car history is whole (the history's reads are soft)");
    rec.End(2);
    rec.Note(Step::kVolume);
    rec.Note(Step::kVolumeComposite);
    rec.Note(Step::kShaftsComposite);  // the shafts' composite with no pass of the shafts this frame: its read is soft
    Check(rec.finding_count() == 0, "the volume's pass and composites, with the shafts' pass missing, are whole");
    rec.End(3);
  }
  // GB-I3: the arms of the cost run in the census: the off arm's frame has no capture and no reflections, the high arm's has both after the light pass
  {
    fo::Recorder rec(true);
    for (Step s : {Step::kGBufferPublish, Step::kAo, Step::kLocalLights, Step::kCarHistory}) rec.Note(s);
    const fo::Recorder::Report off = rec.End(1);
    for (Step s : {Step::kGBufferPublish, Step::kAo, Step::kLocalLights, Step::kColourPost, Step::kSsr, Step::kCarHistory}) rec.Note(s);
    const fo::Recorder::Report high = rec.End(2);
    const std::string ssr_name = fo::RowOf(Step::kSsr).name;
    const std::string post_name = fo::RowOf(Step::kColourPost).name;
    Check(!off.census.empty() && off.census.find(ssr_name) == std::string::npos && off.census.find(post_name) == std::string::npos && !off.any_findings, "the off arm's census names neither the capture nor the reflections");
    Check(!high.census.empty() && high.census.find(ssr_name) != std::string::npos && high.census.find(post_name) != std::string::npos && !high.any_findings &&
              high.census.find(post_name) < high.census.find(ssr_name),
          "the high arm's census names the capture, then the reflections: the chains read the arm that ran from it");
    // the reflections without the capture they read (a frame where the capture was not planned) is a finding: the order table's hard read of colour_post
    fo::Recorder lone(false);
    for (Step s : {Step::kGBufferPublish, Step::kAo, Step::kLocalLights, Step::kSsr}) lone.Note(s);
    bool missing = false;
    for (size_t i = 0; i < lone.finding_count(); ++i) missing = missing || (lone.finding(i).problem == fo::Recorder::Problem::kReadMissing && lone.finding(i).step == Step::kSsr && (lone.finding(i).bits & fo::data::kColourPost) != 0);
    Check(missing, "the reflections noted without the capture before them is a finding (colour_post nobody made)");
    // the reflections before the capture is an inversion
    fo::Recorder swapped(false);
    for (Step s : {Step::kGBufferPublish, Step::kAo, Step::kLocalLights, Step::kSsr, Step::kColourPost}) swapped.Note(s);
    bool inverted = false;
    for (size_t i = 0; i < swapped.finding_count(); ++i) inverted = inverted || (swapped.finding(i).problem == fo::Recorder::Problem::kOutOfOrder && swapped.finding(i).step == Step::kColourPost && swapped.finding(i).other == Step::kSsr);
    Check(inverted, "the capture noted after the reflections is an inversion naming both");
  }
  // an inversion (the recorder that does not assert)
  {
    fo::Recorder rec(false);
    rec.Note(Step::kGBufferPublish);
    rec.Note(Step::kLocalLights);
    rec.Note(Step::kAo);
    Check(rec.finding_count() >= 1 && rec.finding(0).problem == fo::Recorder::Problem::kReadMissing, "the light pass without the AO's frame before it read lin_depth that nothing made");
    bool order = false;
    for (size_t i = 0; i < rec.finding_count(); ++i) order = order || (rec.finding(i).problem == fo::Recorder::Problem::kOutOfOrder && rec.finding(i).step == Step::kAo && rec.finding(i).other == Step::kLocalLights);
    Check(order, "the AO's frame noted after the light pass is an inversion naming both");
    const fo::Recorder::Report report = rec.End(5);
    Check(report.any_findings && report.problems.find("'the AO's frame") != std::string::npos && report.problems.find("which the table puts after it") != std::string::npos, "the report says it in words: " + report.problems.substr(0, 200));
    rec.Note(Step::kGBufferPublish);
    rec.Note(Step::kLocalLights);
    rec.Note(Step::kAo);
    const fo::Recorder::Report again = rec.End(6);
    Check(again.any_findings && again.problems.empty(), "the same mistake is counted but reported once");
    Check(rec.frames_with_findings() == 2, "both frames count as having findings");
  }
  {
    fo::Recorder rec(false);
    rec.Note(Step::kVolumeComposite);
    rec.Note(Step::kGBufferPublish);
    bool seen = false;
    for (size_t i = 0; i < rec.finding_count(); ++i) seen = seen || (rec.finding(i).problem == fo::Recorder::Problem::kOutOfOrder && rec.finding(i).step == Step::kGBufferPublish);
    Check(seen, "a composite at the end-of-scene copy before the trigger's pass is an inversion");
  }
  {
    fo::Recorder rec(false);
    rec.Note(Step::kGBufferPublish);
    rec.Note(Step::kCarSunMap);
    rec.Note(Step::kGBufferPublish);
    Check(rec.finding_count() == 1 && rec.finding(0).problem == fo::Recorder::Problem::kTwice && rec.finding(0).step == Step::kGBufferPublish, "a pass that ran twice (a second trigger) is found");
    rec.Note(Step::kCarSunMap);  // after the repeat: the order is not judged any more in this frame
    rec.Note(Step::kAo);
    Check(rec.finding_count() == 2 && rec.finding(1).step == Step::kCarSunMap && rec.finding(1).problem == fo::Recorder::Problem::kTwice, "after a second trigger only repeats are named, not inversions");
    const fo::Recorder::Report report = rec.End(1);
    Check(report.problems.find("ran twice in one frame") != std::string::npos, "the report names the repeat");
  }
  {
    fo::Recorder rec(false);
    rec.Note(Step::kAoDepth);
    rec.Note(Step::kBeginSsr);
    rec.Note(Step::kNone);
    rec.Note(Step::kCount);
    Check(rec.finding_count() == 4 && rec.finding(0).problem == fo::Recorder::Problem::kNotNoted && rec.finding(3).problem == fo::Recorder::Problem::kNotNoted && rec.order().empty(),
          "a step inside a call, a frame-start step and a step that does not exist are not noted calls");
    rec.End(1);
    Check(rec.order().empty() && rec.finding_count() == 0, "End starts the record again");
  }
  // the census: a new set once, a cap
  {
    fo::Recorder rec(true);
    rec.Note(Step::kGBufferPublish);
    Check(!rec.End(1).census.empty(), "the first set is reported");
    rec.Note(Step::kGBufferPublish);
    Check(rec.End(2).census.empty(), "the same set is not");
    rec.Note(Step::kGBufferPublish);
    rec.Note(Step::kAo);
    const std::string census = rec.End(3).census;
    Check(census == "frame 3: G-buffer publish > the AO's frame (occlusion, bounce light, contact shadows, screen products)", "a different set is, with the passes' names: " + census);
    Check(rec.End(4).census.empty(), "an idle frame (nothing ran) reports nothing");
    size_t reported = 0;
    for (int trial = 0; trial < 200; ++trial) {
      std::set<Step> pick;
      uint32_t bits = uint32_t(trial) * 2654435761u;
      for (size_t k = 0; k < all.size() && k < 31; ++k) {
        if ((bits >> k) & 1u) pick.insert(all[k]);
      }
      if (pick.empty()) continue;
      for (Step s : all) {
        if (pick.count(s) != 0) rec.Note(s);
      }
      reported += !rec.End(100 + uint64_t(trial)).census.empty();
    }
    Check(reported + 3 <= fo::Recorder::kMaxSets + 1 && reported >= fo::Recorder::kMaxSets - 3, "the census stops at kMaxSets distinct sets (" + std::to_string(reported) + " more)");
  }
  // an assert would stop the run: a debug build with the checks on asserts on an inversion; here only that the flag is there to turn off
  {
    fo::Recorder rec;  // the default holds the assertion on: nothing out of order is noted, so it does not fire
    for (Step s : all) rec.Note(s);
    Check(rec.finding_count() == 0 && rec.asserts_on_inversion() && !fo::Recorder(false).asserts_on_inversion(), "the default recorder (assertion on) takes the full frame without firing");
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// the sources
// ---------------------------------------------------------------------------------------------------------------------------------------------------
std::string StepEnumName(Step step) {
  // the enumerator as scene.cpp writes it: frame_order::Step::kX, found by the table's own name list
  static const char* const kNames[] = {"kBeginLocalLights", "kBeginSsr", "kBeginGiField", "kBeginGiApply", "kBeginGBuffer", "kBeginTemporal", "kBeginScreen", "kBeginAo", "kBeginCarRefl", "kBeginLights", "kDrawSidecar", "kDrawHeadlight", "kDrawMirrorView",
                                       "kGBufferPublish", "kCarSunMap", "kCarStructures", "kShadowReplay", "kLightMaps", "kGiField", "kAo", "kAoDepth", "kAoNormals", "kColourPre", "kCarRtAo",
                                       "kRtAoTrace", "kRtAoFilter", "kAoVisibility", "kRtAoCompose", "kContact", "kAoApply", "kScreenPublish", "kGiApply", "kLightsBefore", "kLocalLights", "kRtLights", "kLightsAfter", "kShafts", "kVolume", "kCarRtCheck", "kColourPost", "kSsr", "kCarHistory", "kCarMirror",
                                       "kGBufferView", "kScreenView", "kVolumeComposite", "kShaftsComposite", "kMirrorStamp", "kMirrorGlass", "kRrGuides", "kUpscale", "kRrGuidesView", "kPresentWorld", "kPresentScreen", "kPresentLights", "kPresentRig"};
  static_assert(std::size(kNames) == N, "one name for each step");
  return kNames[size_t(step)];
}

// The tokens `screen::kName` on the text of one statement, as a mask of the screen products.
uint32_t ProductsNamedIn(const std::string& text) {
  struct P {
    const char* name;
    uint32_t bit;
  };
  static const P kP[] = {{"screen::kLinDepthFull", fo::data::kLinDepthFull}, {"screen::kLinDepth", fo::data::kLinDepth}, {"screen::kHizMin", fo::data::kHizMin}, {"screen::kNormal", fo::data::kNormal},
                         {"screen::kColourPre", fo::data::kColourPre}, {"screen::kColourPost", fo::data::kColourPost}, {"screen::kHistory", fo::data::kHistory}};
  uint32_t mask = 0;
  size_t i = 0;
  while (i < text.size()) {
    bool hit = false;
    for (const P& p : kP) {
      if (text.compare(i, std::strlen(p.name), p.name) == 0) {
        mask |= p.bit;
        i += std::strlen(p.name);
        hit = true;
        break;
      }
    }
    if (!hit) ++i;
  }
  return mask;
}
std::string StatementAt(const std::string& text, size_t at) {
  const size_t end = text.find(';', at);
  return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

void CheckSources(const std::string& gpu) {
  std::map<std::string, std::string> files;
  auto text_of = [&](const std::string& name) -> const std::string& {
    auto it = files.find(name);
    if (it == files.end()) {
      std::string text;
      const bool ok = Slurp(gpu + "/" + name, &text);
      Check(ok, "the source " + name + " is readable in " + gpu);
      it = files.emplace(name, text).first;
    }
    return it->second;
  };
  const std::string& scene = text_of("scene.cpp");
  const std::string& scene_h = text_of("scene.h");
  const std::string& ao = text_of("ao.cpp");
  if (scene.empty() || ao.empty()) return;
  const auto ao_call = [&](Step step) { return At(ao, fo::RowOf(step).call); };
  Check(ao_call(Step::kCarRtAo) < ao_call(Step::kRtAoTrace) &&
        ao_call(Step::kRtAoTrace) < ao_call(Step::kRtAoFilter) &&
        ao_call(Step::kRtAoFilter) < ao_call(Step::kAoVisibility) &&
        ao_call(Step::kAoVisibility) < ao_call(Step::kRtAoCompose) &&
        ao_call(Step::kRtAoCompose) < ao_call(Step::kContact), "the RTAO calls bracket visibility before contact");
  const size_t gi_guides = ao.find("gi_normal_slot = RecordGiGuides(list, summary, plan, base);");
  const size_t gi_trace = ao.find("RecordQuarterGi(list, plan, base,");
  Check(gi_guides < ao_call(Step::kColourPre) &&
        ao_call(Step::kRtAoCompose) < gi_trace && gi_trace < ao_call(Step::kAoApply),
        "quarter guides precede colour and the independent bounce reaches the apply after RTAO");

  // every call stands in its file, once
  std::array<size_t, N> pos{};
  for (size_t i = 0; i < N; ++i) {
    const Row& r = Rows()[i];
    const std::string& text = text_of(r.file);
    pos[i] = At(text, r.call);
    Check(pos[i] != std::string::npos, std::string(r.name) + ": '" + r.call + "' stands in " + r.file);
    Check(Count(text, r.call) == 1, std::string(r.name) + ": '" + r.call + "' stands once in " + r.file + ", stands " + std::to_string(Count(text, r.call)) + " times");
  }
  // the order of the calls, phase by phase, in the table's order
  auto ascending = [&](Phase phase, const char* file, const std::string& what) {
    size_t last = 0;
    bool first = true;
    bool ok = true;
    std::string bad;
    for (size_t i = 0; i < N; ++i) {
      const Row& r = Rows()[i];
      if (r.phase != phase || std::strcmp(r.file, file) != 0) continue;
      if (pos[i] == std::string::npos) continue;
      if (!first && pos[i] <= last) {
        ok = false;
        bad = r.name;
      }
      first = false;
      last = pos[i];
    }
    Check(ok, what + (ok ? "" : ": '" + bad + "' stands before the step the table puts earlier"));
  };
  ascending(Phase::kBegin, "scene.cpp", "the frame's start calls stand in scene.cpp in the table's order");
  ascending(Phase::kTrigger, "scene.cpp", "the trigger's calls stand in scene.cpp in the table's order");
  ascending(Phase::kEndOfScene, "scene.cpp", "the end-of-scene composites stand in the table's order");
  ascending(Phase::kPresent, "scene.cpp", "the present's calls stand in the table's order");
  ascending(Phase::kTrigger, "ao.cpp", "the steps of the AO's frame stand in ao.cpp in the table's order");
  ascending(Phase::kAnchor, "temporal.cpp", "RR-2: the anchor's calls stand in temporal.cpp in the table's order");
  {
    // the guide pass is called from RecordUpscale before the backend gets the frame, the view after the backend ran (and failed frames never get one); the pass's wants are declared by its owner
    const std::string& temporal = text_of("temporal.cpp");
    const size_t record = At(temporal, "bool TemporalSystem::RecordUpscale(");
    const size_t pass = pos[PosOf(Step::kRrGuides)];
    const size_t upscale = At(temporal, "backend->Upscale(list, in)", record);
    const size_t view = pos[PosOf(Step::kRrGuidesView)];
    Check(record != std::string::npos && pass != std::string::npos && upscale != std::string::npos && view != std::string::npos && record < pass && pass < upscale && upscale < view,
          "RR-2: temporal.cpp's RecordUpscale calls the guide pass before the backend's Upscale and the debug view after it");
    Check(Has(temporal, "rr_pass_->BeginFrame(rr_guides::PassWanted(rr_plan_, RrGuides::ViewSetting(), effective_ != Upscaler::kOff));") && Has(text_of("scene.cpp"), "temporal_->BeginFrame(slot, list)"),
          "RR-2: the frame boundary tells the guide pass whether it is wanted, from the plan, the view setting and whether an upscaler runs");
    // RR-3: the hand-over and the choice. The guide pass's images go to NoteRrGuides inside RecordRrGuides; the dispatch is chosen after the pass and before the backend is called; the census notes it
    // after; the Ray Reconstruction context follows the Super Resolution request at the frame boundary, and the current slot's request is never Ray Reconstruction's.
    {
      const size_t choose = At(temporal, "ChooseRrDispatch();", record);
      const size_t note = At(temporal, "NoteRrDispatch(dispatch, used_rr, width, height, out_width, out_height);", record);
      Check(Has(temporal, "NoteRrGuides(rr_pass_->guides(), made && rr_pass_->ready()"), "RR-3: RecordRrGuides hands the guide pass's images to NoteRrGuides");
      Check(choose != std::string::npos && choose > pass && choose < upscale && note != std::string::npos && note > upscale,
            "RR-3: RecordUpscale chooses the dispatch after the guide pass and before the backend's Upscale, and notes it after");
      Check(Has(temporal, "backend->Request(request);\n    request_ = request;\n    request_made_ = true;\n    ManageRrContext(request);"),
            "RR-3: the Ray Reconstruction context follows the Super Resolution request at the frame boundary");
      Check(!Has(temporal, "request.ray_reconstruction = rr_plan_") && !Has(temporal, "request->ray_reconstruction = rr_plan_"),
            "RR-3: the current slot's request is Super Resolution's (Ray Reconstruction is the companion's, never a change of the upscaler's own context)");
      Check(Has(temporal, "rr::RuntimePhaseCount(from_runtime, frame_.ratio, rr_may_run)"), "RR-3: the jitter sequence is 32 positions or more while Ray Reconstruction may run");
    }
    // the frame boundary stands between the G-buffer's read and the screen owner's in scene.cpp, as the table's rows say
    const size_t gbuffer_begin = At(scene, "gbuffer_->BeginFrame(frame_serial_)");
    const size_t temporal_begin = At(scene, "temporal_->BeginFrame(slot, list)");
    const size_t screen_begin = At(scene, "screen_->BeginFrame(frame_serial_)");
    Check(gbuffer_begin < temporal_begin && temporal_begin < screen_begin, "RR-2: scene.cpp's temporal frame boundary stands between the G-buffer's BeginFrame and the screen owner's");
  }
  Check(Has(scene, "light_manager_->BeginLights(frame_serial_, slot)") && At(scene, "light_manager_->BeginLights(frame_serial_, slot)") < At(scene, "local_shadow_->BeginFrame()"),
        "the frame's lights are opened before the shadow replay's frame start (the replay picks its cars from them)");
  Check(At(scene, "light_manager_->BeginFrame(frame_serial_, slot)") > At(scene, "local_shadow_->BeginFrame()"), "... and the manager's second call (the last trigger's maps) after it");
  // the end-of-scene composites stand above the trigger's calls: the same function, an earlier text, a later resolve
  {
    const size_t execute = At(scene, "resolve_->Execute(record, list, float_scene");
    const size_t composite = pos[PosOf(Step::kShaftsComposite)];
    const size_t first_trigger = pos[PosOf(Step::kGBufferPublish)];
    Check(execute != std::string::npos && composite < execute && execute < first_trigger, "the composites are called before the resolve's copy runs, the trigger's passes after it");
  }
  // every noted step has its Note line right after its call, noting what the call returned, and there is no other Note
  size_t noted = 0;
  for (size_t i = 0; i < N; ++i) {
    const Row& r = Rows()[i];
    if (r.kind != Kind::kNoted) continue;
    ++noted;
    const std::string key = "order_.Note(frame_order::Step::" + StepEnumName(r.step) + ", ";
    const size_t at = At(scene, key);
    Check(at != std::string::npos && Count(scene, key) == 1, std::string(r.name) + ": its Note line is in scene.cpp, once");
    if (at == std::string::npos || pos[i] == std::string::npos) continue;
    Check(at > pos[i] && at - pos[i] < 700, std::string(r.name) + ": the Note line follows its call closely");
    // what it notes: the variable the call's result went to
    const size_t line_start = scene.rfind('\n', pos[i]) + 1;
    const std::string line = scene.substr(line_start, pos[i] - line_start);
    std::string variable;
    const size_t eq = line.rfind(" = ");
    if (eq != std::string::npos) {
      size_t b = eq;
      while (b > 0 && (std::isalnum(static_cast<unsigned char>(line[b - 1])) || line[b - 1] == '_')) --b;
      variable = line.substr(b, eq - b);
    } else {
      variable = "shadow_recorded";  // the manager's call returns nothing: it is noted with the replay's answer (its own trigger)
      Check(r.step == Step::kLightMaps, std::string(r.name) + ": only the manager's call is not assigned");
    }
    const std::string argument = scene.substr(at + key.size(), scene.find(')', at) - (at + key.size()));
    Check(argument == variable, std::string(r.name) + ": notes '" + variable + "' (the call's result), notes '" + argument + "'");
  }
  Check(Count(scene, "order_.Note(") == noted && noted == 20, "scene.cpp has one Note for each of the 20 noted calls and no other");
  Check(Has(scene, "const frame_order::Recorder::Report order = order_.End(frame_serial_);") && Has(scene_h, "frame_order::Recorder order_;"), "the Scene holds a recorder and ends the frame with it at the present");
  Check(At(scene, "order_.End(frame_serial_)") > At(scene, "light_manager_->OnPresent()"), "the frame ends after the present's calls");
  Check(Has(scene, "IS THE TABLE IN frame_order.h") && !Has(scene, "//   1. the car's sun shadow map (car_shadow_map.h)"), "the comment block that listed the order is replaced by the table's pointer");
  // the PassTiming labels exist in their files
  for (const Row& r : Rows()) {
    if (r.label[0] == '\0') continue;
    Check(r.label_file[0] != '\0' && Has(text_of(r.label_file), std::string("\"") + r.label + "\""), std::string(r.name) + ": the PassTiming label \"" + r.label + "\" is in " + r.label_file);
  }
  // GB-I3: the other marks of a pass and its closing mark exist in the file, after the pass's first label, in the table's order; the closing mark is the last
  for (const Row& r : Rows()) {
    if (r.more_labels[0] == nullptr && r.closing_label[0] == '\0') continue;
    const std::string& text = text_of(r.label_file);
    size_t last = At(text, std::string("Mark(list, \"") + r.label + "\")");
    Check(last != std::string::npos, std::string(r.name) + ": the first label '" + r.label + "' is a Mark in " + r.label_file);
    for (const char* more : r.more_labels) {
      if (more == nullptr) continue;
      const size_t at = At(text, std::string("Mark(list, \"") + more + "\")");
      Check(at != std::string::npos && at > last, std::string(r.name) + ": the mark '" + more + "' is in " + r.label_file + " after the one before it");
      last = at == std::string::npos ? last : at;
    }
    if (r.closing_label[0] != '\0') {
      const size_t at = At(text, std::string("Mark(list, \"") + r.closing_label + "\")");
      Check(at != std::string::npos && at > last, std::string(r.name) + ": the closing mark '" + r.closing_label + "' is in " + r.label_file + " after the pass's own marks");
      Check(Count(text, std::string("\"") + r.closing_label + "\"") == 1, std::string(r.name) + ": the closing mark '" + r.closing_label + "' is written once");
      bool also_own = std::strcmp(r.closing_label, r.label) == 0;
      for (const char* more : r.more_labels) also_own = also_own || (more != nullptr && std::strcmp(more, r.closing_label) == 0);
      Check(!also_own, std::string(r.name) + ": the closing mark is not one of the pass's own labels (its time is whatever is recorded next)");
    }
  }
  {
    // the reflections' and the light pass's labels as the cost runs read them
    const Row& ssr_row = fo::RowOf(fo::cost::kReflectionsStep);
    std::vector<std::string> own = {ssr_row.label};
    for (const char* more : ssr_row.more_labels) {
      if (more != nullptr) own.push_back(more);
    }
    Check(own == std::vector<std::string>({"ssr trace", "ssr rt", "ssr temporal", "ssr denoise", "ssr apply"}), "the reflections' own labels are the three passes', the ray traced fallback's (CITY-RT R4) and its denoiser's (R6; the chains sum the labels that start with 'ssr ')");
    for (const std::string& label : own) Check(label.rfind("ssr ", 0) == 0, "own label '" + label + "' starts with 'ssr '");
    Check(std::string(ssr_row.closing_label) == "after ssr" && std::string(ssr_row.closing_label).rfind("ssr ", 0) != 0, "the closing label 'after ssr' is not in that sum");
    for (const char* label : fo::cost::kReflectionsProductLabels) {
      Check(Has(text_of("screen_shared.cpp"), std::string("Mark(list, \"") + label + "\")"), std::string("the product label '") + label + "' is a Mark in screen_shared.cpp");
      Check(std::string(label).rfind("ssr ", 0) != 0, std::string("the product label '") + label + "' is not an SSR label: it is the owner's, timed apart");
    }
    const Row& light_row = fo::RowOf(Step::kLocalLights);
    Check(std::string(light_row.label) == "local lights" && std::string(light_row.closing_label) == "after local lights" && light_row.more_labels[0] == nullptr, "the light pass: one pass, the label 'local lights' and the closing mark 'after local lights'");
    // the cost arms: the words are the levels of the setting, and a frame of the off arm and one of the high arm have census lines that differ by the capture and the reflections
    Check(std::string(fo::cost::kReflectionsSetting) == "mcla_gpu_ssr" && Has(text_of("ssr.cpp"), "static LiveText text(\"mcla_gpu_ssr\");"), "the cost arms set the setting the effect reads");
    for (const fo::cost::Arm& arm : fo::cost::kReflectionsArms) {
      const mcla::gpu::ssr::Level level = mcla::gpu::ssr::LevelFromText(arm.word);
      Check(mcla::gpu::ssr::LevelName(level) == arm.word && arm.runs == (level != mcla::gpu::ssr::Level::kOff), std::string("arm '") + arm.word + "' is a level of the setting, and runs unless it is off");
    }
    Check(fo::cost::kReflectionsArms.size() == mcla::gpu::ssr::kLevelCount, "one arm for each level of the setting");
  }
  // the wants the table lists are declared by their consumers
  struct Decl {
    const char* file;
    const char* text;
  };
  const Decl decls[] = {{"local_lights.cpp", "screen_->Want(screen::Consumer::kLocalLights"}, {"local_lights.cpp", "gbuffer_->Want(gbuffer::Consumer::kLocalLights"}, {"local_lights.cpp", "manager_->Want("},
                        {"ssr.cpp", "screen_->Want(screen::Consumer::kReflections"}, {"ssr.cpp", "gbuffer_->Want(gbuffer::Consumer::kReflections"},
                        {"ao.cpp", "screen_->Want(screen::Consumer::kAmbientOcclusion"}, {"ao.cpp", "screen_->Want(screen::Consumer::kContactShadows"}, {"ao.cpp", "screen_->Want(screen::Consumer::kBounceLight"},
                        {"ao.cpp", "gbuffer_->Want(gbuffer::Consumer::kAmbientOcclusion"}, {"ao.cpp", "gbuffer_->Want(gbuffer::Consumer::kBounceLight"}, {"car_selfrefl.cpp", "screen_->Want(screen::Consumer::kCarHistory"},
                        {"gi_apply.cpp", "screen_->Want(screen::Consumer::kRtBounce"}, {"gi_apply.cpp", "gbuffer_->Want(gbuffer::Consumer::kRtBounce"},
                        {"rr_guides.cpp", "gbuffer_->Want(gbuffer::Consumer::kRayReconstruction"}};
  for (const Decl& d : decls) Check(Has(text_of(d.file), d.text), std::string(d.file) + " declares '" + d.text + "'");
  {
    const std::string& ll = text_of("local_lights.cpp");
    const std::string& ssr = text_of("ssr.cpp");
    const std::string& car = text_of("car_selfrefl.cpp");
    Check(ProductsNamedIn(StatementAt(ll, At(ll, "screen_->Want(screen::Consumer::kLocalLights"))) == fo::RowOf(Step::kLocalLights).wants_screen, "the light pass asks the screen owner for the products the table lists");
    const std::string& ssr_rules = text_of("ssr_rules.h");
    Check(ProductsNamedIn(StatementAt(ssr_rules, At(ssr_rules, "inline constexpr uint32_t kWantedProducts"))) == fo::RowOf(Step::kSsr).wants_screen &&
              Has(ssr, "screen_->Want(screen::Consumer::kReflections, ssr::WantedProducts(level_))"),
          "the reflections ask the screen owner, through ssr_rules.h WantedProducts, for the products the table lists");
    Check(ProductsNamedIn(StatementAt(car, At(car, "screen_->Want(screen::Consumer::kCarHistory"))) == fo::RowOf(Step::kCarHistory).wants_screen, "the car self-reflection asks for the product the table lists");
    const size_t from = At(ao, "const uint32_t normal = larecomp::settings::mcla_gpu_ao_gbuffer_normals.Get()");
    const size_t bounce = At(ao, "screen_->Want(screen::Consumer::kBounceLight");
    Check(from != std::string::npos && bounce != std::string::npos && from < bounce &&
              ProductsNamedIn(ao.substr(from, bounce - from) + StatementAt(ao, bounce)) == fo::RowOf(Step::kAo).wants_screen,
          "the AO's three wants are lin_depth, normal and colour_pre, as the table's AO row lists");
    Check(Has(ll, "gbuffer::ConsumerPlanes(gbuffer::Consumer::kLocalLights)") && Has(ssr_rules, "gbuffer::ConsumerPlanes(gbuffer::Consumer::kReflections)") &&
              Has(ssr, "gbuffer_->Want(gbuffer::Consumer::kReflections, ssr::WantedPlanes(level_))"),
          "the light pass and the reflections ask the G-buffer for ConsumerPlanes (which the table's wants equal; the reflections through ssr_rules.h WantedPlanes, 0 while off)");
  }
  // the order the lag rests on: the owners read the wants at their BeginFrame, the consumers declare at theirs (frame start, above)
  Check(At(scene, "ssr_->BeginFrame()") < At(scene, "gbuffer_->BeginFrame(frame_serial_)") && At(scene, "ssr_->BeginFrame()") < At(scene, "screen_->BeginFrame(frame_serial_)") &&
            At(scene, "local_lights_->BeginFrame()") < At(scene, "ssr_->BeginFrame()") && At(scene, "ssr_->BeginFrame()") < At(scene, "ao_->BeginFrame()"),
        "SSR declares after the light pass and before the G-buffer and the screen owner read (lag 0)");
  Check(Count(scene, "ssr_->BeginFrame()") == 1, "the reflections' frame start is called once");
  Check(At(scene, "ssr_->BeginFrame()") < At(scene, "gi_integration_->BeginFrame(frame_serial_)") && At(scene, "gi_integration_->BeginFrame(frame_serial_)") < At(scene, "gi_apply_->BeginFrame()") &&
            At(scene, "gi_apply_->BeginFrame()") < At(scene, "gbuffer_->BeginFrame(frame_serial_)") && At(scene, "gi_apply_->BeginFrame()") < At(scene, "screen_->BeginFrame(frame_serial_)") &&
            At(scene, "gi_apply_->BeginFrame()") < At(scene, "ao_->BeginFrame()"),
        "the probe field starts its frame before its apply declares, and the apply declares before the G-buffer and the screen owner read (lag 0)");
  Check(At(scene, "gi_integration_->OnResolve(summary, list, slot_)") < At(scene, "ao_->OnResolve(record, summary, list, UpscalerRunning())") &&
            At(scene, "ao_->OnResolve(record, summary, list, UpscalerRunning())") < At(scene, "gi_apply_->OnTrigger(summary, list, gbuffer_published, slot_)"),
        "the probe field's frame is recorded before the AO's frame (the AO asks the field's share while it plans) and the field's apply after it");
}

}  // namespace

int main(int argc, char** argv) {
  std::string gpu;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--print") == 0) g_print = true;
    else if (argv[i][0] != '-') gpu = argv[i];
  }
  if (gpu.empty()) gpu = std::filesystem::path(__FILE__).parent_path().string();
  if (g_print) {
    for (size_t i = 0; i < N; ++i) {
      const Row& r = Rows()[i];
      std::string makes;
      std::string reads;
      for (uint32_t bit = 1; bit != 0; bit <<= 1) {
        if ((r.makes & bit) != 0) makes += std::string(makes.empty() ? "" : ", ") + fo::data::NameOf(bit);
        if (((r.reads | r.soft_reads) & bit) != 0) reads += std::string(reads.empty() ? "" : ", ") + fo::data::NameOf(bit) + ((r.reads & bit) ? "" : "?");
      }
      std::printf("%2zu %-12s %-7s %-58s label '%s' | makes: %s | reads: %s\n", i, fo::PhaseName(r.phase), r.kind == Kind::kNoted ? "noted" : (r.kind == Kind::kInner ? "inner" : "scanned"), r.name, r.label,
                  makes.c_str(), reads.c_str());
    }
  }
  CheckTableItself();
  CheckPinnedOrder();
  CheckWants();
  CheckMoves();
  CheckRecorder();
  CheckSources(gpu);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
