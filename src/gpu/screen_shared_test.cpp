// Check of the shared screen products' frozen interface (screen_frame.h), the host's pure rules (screen_rules.h), the camera history (screen_history.h) and the Hi-Z's shader functions
// (hlsl/screen_hiz.hlsli). A program of its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_screen_shared_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_screen_shared_test [<src/gpu dir>] [--print]
//
// It checks: the interface's values pinned as they are frozen (the products, the consumers, the sizes of the arrays, the camera, the frame), the Want registry, the Hi-Z chain's level
// count and sizes at the sizes the renderer runs at and at odd ones, the memory it comes to, the plan (what is made, when the AO's trigger runs for it, what a debug view needs), the frame
// maker and the generation's rule (a ping-pong pair of depth images is not a new set), the vehicle mark and the source bits of the AO's depth (the twin of what ao_gbuffer.h keeps in the
// low bits), the min chain against a brute force over each texel's footprint (odd sizes, marks, the sky) and against the AO's mean levels (a mean is not a Hi-Z); then it compiles
// hlsl/screen_hiz.hlsli ITSELF as C++ (through the shim of the HLSL types below) and compares its two functions with the twin on random inputs. Given the src/gpu folder it also reads the
// sources: the shaders' registration, the push blocks' layout, the settings, the hooks in the AO, the Scene and the Renderer and the order they are called in.
//
// SS-1 adds: the colour captures' sizes, levels and memory, the plan's rule for colour_post and the history, the frame with both and the generation that a trade of the capture with its history
// does not move, the exposure bias, the history's usability rule, the camera history (Previous, Before, Current, LensAgrees, the pair for the products' camera), the capture's arithmetic against the
// car history pass's (a twin of the two loops, bit for bit on random samples, and the two shader texts compared where the twin cannot reach), and the sources: the order of the hooks (the anchor's
// camera before the effects', the capture after the occlusion and before the car history, the present after the world's classification), what moved out of the car self-reflection and the AO, and
// that nothing is recorded without a want.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "ao_gbuffer.h"
#include "ao_size.h"
#include "screen_frame.h"
#include "screen_history.h"
#include "screen_rules.h"
#include "setting_source.h"

// ---------------------------------------------------------------------------
// the .hlsli as C++: a shim of what the file uses, in a namespace of its own so that the standard library's min ... are not in the way
// ---------------------------------------------------------------------------
namespace hlsl_shim {
typedef unsigned int uint;
struct float4 {
  float x = 0, y = 0, z = 0, w = 0;
  float4() = default;
  float4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
};
inline float min(float a, float b) { return a < b ? a : b; }
inline uint asuint(float f) {
  uint u;
  std::memcpy(&u, &f, sizeof(u));
  return u;
}
inline float asfloat(uint u) {
  float f;
  std::memcpy(&f, &u, sizeof(f));
  return f;
}
#include "hlsl/screen_hiz.hlsli"
}  // namespace hlsl_shim

namespace sc = mcla::gpu::screen;
namespace ag = mcla::gpu::ao_gbuffer;

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
std::string Num(const char* fmt, double a, double b = 0.0, double c = 0.0, double d = 0.0) {
  char buf[400];
  std::snprintf(buf, sizeof(buf), fmt, a, b, c, d);
  return buf;
}

bool Slurp(const std::string& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream text;
  text << in.rdbuf();
  *out = text.str();
  return true;
}
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
size_t Count(const std::string& text, const std::string& needle) {
  size_t n = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
  return n;
}
// The index of the first `needle` at or after `from`, or npos.
size_t At(const std::string& text, const std::string& needle, size_t from = 0) { return text.find(needle, from); }

struct Lcg {
  uint32_t state = 20261120u;
  uint32_t Next() {
    state = state * 1664525u + 1013904223u;
    return state;
  }
  float Unit() { return float(Next() >> 8) / float(1u << 24); }
  float Range(float lo, float hi) { return lo + (hi - lo) * Unit(); }
};

uint32_t BitsOf(float v) { return ag::BitsOf(v); }
float FloatOf(uint32_t b) { return ag::FloatOf(b); }

// ---------------------------------------------------------------------------
// the interface, as frozen
// ---------------------------------------------------------------------------
void FrozenChecks() {
  // The products' numbers never change (a bit in ScreenFrame::valid and in every consumer's want).
  Check(sc::kLinDepth == 0 && sc::kLinDepthFull == 1 && sc::kHizMin == 2 && sc::kNormal == 3 && sc::kColourPre == 4 && sc::kColourPost == 5 && sc::kHistory == 6 && sc::kProductCount == 7,
        "the products keep their numbers (lin_depth 0, lin_depth_full 1, hiz_min 2, normal 3, colour_pre 4, colour_post 5, history 6)");
  Check(sc::kAllProducts == 0x7Fu && sc::ProductBit(sc::kHizMin) == 4u, "the products' bits");
  Check(sc::kNoSlot == 0xFFFFFFFFu, "kNoSlot is the bindless heap's invalid slot");
  Check(sc::kSky == 60000.0f && sc::kDepthMarkMask == 7u, "the sky's distance and the marks' mask");
  Check(sc::kDepthLevels == 3 && sc::kColourPreLevels == 3 && sc::kColourPostMaxLevels == 8 && sc::kHizMaxLevels == 8 && sc::kHizHalvings == 6, "the chains' sizes");
  // The consumers' numbers (an index into the registry).
  Check(int(sc::Consumer::kAmbientOcclusion) == 0 && int(sc::Consumer::kContactShadows) == 1 && int(sc::Consumer::kBounceLight) == 2 && int(sc::Consumer::kCarRtAo) == 3 &&
            int(sc::Consumer::kCarRtCheck) == 4 && int(sc::Consumer::kReflections) == 5 && int(sc::Consumer::kCarHistory) == 6 && int(sc::Consumer::kCarRtRefl) == 7 &&
            int(sc::Consumer::kVolumetrics) == 8 && int(sc::Consumer::kLocalLights) == 9 && int(sc::Consumer::kDebugView) == 10 && int(sc::Consumer::kRtBounce) == 11 && int(sc::Consumer::kCount) == 12,
        "the consumers keep their numbers");
  Check(int(sc::View::kOff) == 0 && int(sc::View::kLinDepth) == 1 && int(sc::View::kHizMin) == 2 && int(sc::View::kHizCheck) == 3 && int(sc::View::kNormal) == 4 &&
            int(sc::View::kVehicle) == 5 && int(sc::View::kColourPost) == 6 && int(sc::View::kHistory) == 7 && int(sc::View::kCount) == 8,
        "the debug views keep their numbers (SS-1 appended colour_post 6 and the history 7)");
  Check(sc::kCameraValid == 1u && sc::kCameraHasPrevious == 2u && sc::kFramePause == 1u && sc::kFrameHalfTraced == 2u, "the camera's and the frame's flags");
  // The layouts: the camera is a lens, four rows, two planes, an exposure bias, flags and three row-major matrices; the frame holds what the table of the header says.
  Check(sizeof(sc::ScreenCamera) == (2 + 4 + 2 + 1 + 1 + 48) * 4, Num("ScreenCamera is 58 floats (%.0f bytes)", double(sizeof(sc::ScreenCamera))));
  Check(sizeof(sc::ScreenFrame) == (4 + 2 + 2 + 2 + 2 + 3 + 1 + 8 + 1 + 3 + 8 + 2) * 4 + sizeof(sc::ScreenCamera), Num("ScreenFrame's size (%.0f bytes)", double(sizeof(sc::ScreenFrame))));
  Check(offsetof(sc::ScreenFrame, generation) == 0 && offsetof(sc::ScreenFrame, valid) == 8, "ScreenFrame starts with the generation, the serial, the valid mask");
  // Default values: nothing valid, every slot absent, the camera's exposure bias 1.
  const sc::ScreenFrame empty;
  bool all_absent = empty.valid == 0 && empty.normal_slot == sc::kNoSlot && empty.lin_depth_full_slot == sc::kNoSlot && empty.history_colour_slot == sc::kNoSlot &&
                    empty.history_depth_slot == sc::kNoSlot && empty.hiz_levels == 0 && empty.colour_post_levels == 0;
  for (uint32_t s : empty.lin_depth_slot) all_absent = all_absent && s == sc::kNoSlot;
  for (uint32_t s : empty.hiz_min_slot) all_absent = all_absent && s == sc::kNoSlot;
  for (uint32_t s : empty.colour_pre_slot) all_absent = all_absent && s == sc::kNoSlot;
  for (uint32_t s : empty.colour_post_slot) all_absent = all_absent && s == sc::kNoSlot;
  Check(all_absent, "an empty frame has no product and every slot is kNoSlot");
  Check(!empty.Has(sc::kLinDepth) && empty.camera.exposure_bias == 1.0f && empty.camera.flags == 0, "an empty frame has no camera");
  // The names (the log lines).
  Check(std::string(sc::ProductName(sc::kHizMin)) == "hiz_min" && std::string(sc::ProductName(sc::kLinDepth)) == "lin_depth" && std::string(sc::ProductName(sc::kProductCount)) == "?",
        "the product names");
  Check(std::string(sc::ConsumerName(sc::Consumer::kReflections)) == "reflections" && std::string(sc::ConsumerName(sc::Consumer::kCount)) == "?", "the consumer names");
}

void WantChecks() {
  sc::Wants wants;
  Check(wants.Union() == 0 && wants.Of(sc::Consumer::kReflections) == 0, "no consumer wants anything at the start");
  wants.Set(sc::Consumer::kReflections, sc::ProductBit(sc::kHizMin) | sc::ProductBit(sc::kLinDepth));
  wants.Set(sc::Consumer::kAmbientOcclusion, sc::ProductBit(sc::kLinDepth));
  Check(wants.Of(sc::Consumer::kReflections) == 5u && wants.Of(sc::Consumer::kAmbientOcclusion) == 1u && wants.Union() == 5u, "a want is kept per consumer and the union is their sum");
  wants.Set(sc::Consumer::kReflections, 0);
  Check(wants.Of(sc::Consumer::kReflections) == 0 && wants.Union() == 1u, "0 withdraws a want");
  wants.Set(sc::Consumer::kCarRtAo, 0xFFFFFFFFu);
  Check(wants.Of(sc::Consumer::kCarRtAo) == sc::kAllProducts, "bits above the products are dropped");
  wants.Set(sc::Consumer::kCount, sc::kAllProducts);
  Check(wants.Of(sc::Consumer::kCount) == 0, "a consumer that does not exist is ignored");
}

// ---------------------------------------------------------------------------
// the Hi-Z chain: sizes and memory
// ---------------------------------------------------------------------------
void SizeChecks() {
  Check(sc::HizLevelCount(1920, 1080) == 7 && sc::HizLevelCount(3840, 2160) == 7 && sc::HizLevelCount(960, 540) == 7 && sc::HizLevelCount(640, 360) == 7,
        "7 levels (down to 1/64) at 1080p, 4K, and their half sizes");
  Check(sc::HizLevelCount(64, 64) == 7 && sc::HizLevelCount(32, 32) == 6 && sc::HizLevelCount(2, 2) == 2 && sc::HizLevelCount(1, 1) == 1 && sc::HizLevelCount(0, 0) == 1,
        "the smaller side limits the levels: 64 gives 7, 32 gives 6, a texel gives 1");
  Check(sc::HizLevelCount(4096, 4) == 3 && sc::HizLevelCount(4, 4096) == 3, "a thin image has the levels of its smaller side");
  Check(sc::HizLevelCount(1920, 1080) <= sc::kHizMaxLevels, "the chain fits the frame's slot array");
  // Sizes halve with the odd edge rounded up, as the AO's coarser levels (ao_size.h SizesOf).
  for (uint32_t extent : {1u, 2u, 3u, 5u, 63u, 64u, 65u, 540u, 719u, 720u, 1080u, 1081u, 2160u, 3840u}) {
    const uint32_t r[2] = {extent, extent};
    mcla::gpu::ao_size::Choice full;
    const mcla::gpu::ao_size::Sizes s = mcla::gpu::ao_size::SizesOf(r, full);
    Check(sc::HizLevelSize(extent, 0) == std::max(extent, 1u) && sc::HizLevelSize(extent, 1) == s.level0[0] && sc::HizLevelSize(extent, 2) == s.level1[0],
          Num("level sizes of %.0f follow the AO's coarser levels (1: %.0f, 2: %.0f)", double(extent), double(s.level0[0]), double(s.level1[0])));
  }
  Check(sc::HizLevelSize(1080, 6) == 17 && sc::HizLevelSize(1920, 6) == 30 && sc::HizLevelSize(1, 5) == 1 && sc::HizLevelSize(0, 3) == 1, "the last level of 1920x1080 is 30x17; sizes never go below 1");
  // The memory: 1080p traced is the 11 MiB of the plan (LIGHT-UNIFY.md 5.4: hiz_min about 11 MiB), the half size of a 4K or 1080p picture at the AO's high a quarter of it.
  const double mib = double(sc::HizBytes(1920, 1080)) / (1024.0 * 1024.0);
  Check(mib > 10.0 && mib < 11.2, Num("the Hi-Z at 1920x1080 is %.2f MiB (the plan says about 11)", mib));
  Check(sc::HizBytes(960, 540) * 3 < sc::HizBytes(1920, 1080) && sc::HizBytes(960, 540) * 5 > sc::HizBytes(1920, 1080), "half size is about a quarter");
  Check(sc::HizBytes(1, 1) == 4, "a texel is 4 bytes");
  if (g_print) std::printf("hiz bytes: 1080p %.2f MiB, 540p %.2f MiB, 4K %.2f MiB\n", mib, double(sc::HizBytes(960, 540)) / 1048576.0, double(sc::HizBytes(3840, 2160)) / 1048576.0);
}

// ---------------------------------------------------------------------------
// the plan
// ---------------------------------------------------------------------------
void PlanChecks() {
  using sc::Consumer;
  using sc::ProductBit;
  sc::Wants wants;
  sc::Plan plan = sc::PlanFrame(wants, sc::View::kOff, 0);
  Check(plan.wanted == 0 && plan.make == 0 && !plan.NeedsDepthChain(), "with no consumer and no view nothing is wanted, nothing is made and the AO's trigger does not run for it");
  // The AO's own family never starts a frame of its own: its passes run anyway.
  wants.Set(Consumer::kAmbientOcclusion, ProductBit(sc::kLinDepth));
  wants.Set(Consumer::kContactShadows, ProductBit(sc::kLinDepth));
  wants.Set(Consumer::kBounceLight, ProductBit(sc::kLinDepth) | ProductBit(sc::kColourPre));
  plan = sc::PlanFrame(wants, sc::View::kOff, 0);
  Check(plan.wanted == (ProductBit(sc::kLinDepth) | ProductBit(sc::kColourPre)) && plan.make == 0 && !plan.NeedsDepthChain(),
        "the occlusion, the contact shadows and the bounce light want products but make no frame of their own and no Hi-Z");
  // SSR wants the Hi-Z: it is made here, and the trigger runs for it.
  wants.Set(Consumer::kReflections, ProductBit(sc::kLinDepth) | ProductBit(sc::kHizMin) | ProductBit(sc::kNormal));
  plan = sc::PlanFrame(wants, sc::View::kOff, 0);
  Check((plan.make & ProductBit(sc::kHizMin)) != 0 && plan.make == ProductBit(sc::kHizMin) && plan.NeedsDepthChain(), "a reflections consumer's want of the depth, the Hi-Z and the normal makes the Hi-Z and asks for the depth chain");
  // The same want of lin_depth alone (a consumer outside the family) asks for the depth chain but makes nothing here.
  sc::Wants only_depth;
  only_depth.Set(Consumer::kLocalLights, ProductBit(sc::kLinDepth));
  plan = sc::PlanFrame(only_depth, sc::View::kOff, 0);
  Check(plan.make == 0 && plan.NeedsDepthChain(), "a consumer outside the family that wants the depth alone asks for the chain and makes no Hi-Z");
  // A want of normal alone cannot start a frame (a frame for the screen products has no normal pass).
  sc::Wants only_normal;
  only_normal.Set(Consumer::kReflections, ProductBit(sc::kNormal));
  Check(!sc::PlanFrame(only_normal, sc::View::kOff, 0).NeedsDepthChain(), "a want of the normal alone does not run the depth chain");
  // The debug view is a consumer of its own and nobody can set it through Want.
  sc::Wants none;
  none.Set(Consumer::kDebugView, sc::kAllProducts);
  plan = sc::PlanFrame(none, sc::View::kOff, 0);
  Check(plan.wanted == 0 && !plan.NeedsDepthChain(), "Want(kDebugView) is ignored: the setting makes the debug view's wants");
  for (int v = 1; v < int(sc::View::kCount); ++v) {
    plan = sc::PlanFrame(none, sc::View(v), 3);
    const uint32_t needs = sc::ViewNeeds(sc::View(v));
    Check(needs != 0 && (plan.wanted & needs) == needs && plan.view == sc::View(v) && plan.view_level == 3, Num("view %.0f's products are wanted while it is on", double(v)));
  }
  Check(sc::PlanFrame(none, sc::View::kHizMin, 0).make == ProductBit(sc::kHizMin) && sc::PlanFrame(none, sc::View::kHizCheck, 0).make == ProductBit(sc::kHizMin) &&
            sc::PlanFrame(none, sc::View::kLinDepth, 0).make == 0,
        "the Hi-Z views make the Hi-Z, the depth views do not");
  Check(sc::PlanFrame(none, sc::View::kNormal, 0).wanted == ProductBit(sc::kNormal) && !sc::PlanFrame(none, sc::View::kNormal, 0).NeedsDepthChain(),
        "the normal view wants the normal and cannot start a frame (it shows black where the frame has none)");
  Check(sc::PlanFrame(none, sc::View::kHizMin, 0).NeedsDepthChain() && sc::PlanFrame(none, sc::View::kVehicle, 0).NeedsDepthChain(), "the Hi-Z and the vehicle views run the depth chain");
  // The settings' conversion.
  Check(sc::ViewFromSetting(0) == sc::View::kOff && sc::ViewFromSetting(1) == sc::View::kLinDepth && sc::ViewFromSetting(5) == sc::View::kVehicle && sc::ViewFromSetting(6) == sc::View::kColourPost &&
            sc::ViewFromSetting(7) == sc::View::kHistory && sc::ViewFromSetting(8) == sc::View::kOff && sc::ViewFromSetting(-1) == sc::View::kOff,
        "a setting outside the views is off");
  Check(sc::LevelFromSetting(-3) == 0 && sc::LevelFromSetting(4) == 4 && sc::LevelFromSetting(99) == sc::kHizMaxLevels - 1, "the level setting is clamped to the chain's slots");
}

// ---------------------------------------------------------------------------
// the frame and the generation
// ---------------------------------------------------------------------------
sc::FrameInputs TypicalInputs() {
  sc::FrameInputs in;
  in.serial = 777;
  in.traced[0] = 960;
  in.traced[1] = 540;
  in.render[0] = 1920;
  in.render[1] = 1080;
  in.host_samples = 4;
  in.sample0 = 1;
  in.flags = sc::kFrameHalfTraced;
  in.depth[0] = 100;
  in.depth[1] = 101;
  in.depth[2] = 102;
  in.owner_generation = 5;
  return in;
}

void FrameChecks() {
  sc::FrameInputs in = TypicalInputs();
  sc::ScreenFrame f = sc::MakeFrame(in, 9);
  Check(f.generation == 9 && f.frame_serial == 777 && f.traced[0] == 960 && f.render[1] == 1080 && f.host_samples == 4 && f.sample0 == 1 && f.flags == sc::kFrameHalfTraced, "the frame carries the numbers it was given");
  Check(f.valid == sc::ProductBit(sc::kLinDepth) && f.Has(sc::kLinDepth) && !f.Has(sc::kHizMin) && f.lin_depth_slot[0] == 100 && f.lin_depth_slot[2] == 102 && f.normal_slot == sc::kNoSlot,
        "only the depth chain is valid when only it was given");
  Check(f.hiz_levels == 0 && f.hiz_min_slot[0] == sc::kNoSlot, "no Hi-Z, no levels");
  // The coarser levels are optional: the occlusion off leaves level 0 alone.
  in.depth[1] = in.depth[2] = sc::kNoSlot;
  f = sc::MakeFrame(in, 1);
  Check(f.Has(sc::kLinDepth) && f.lin_depth_slot[1] == sc::kNoSlot, "a frame with level 0 alone is still valid (a screen-products frame has no coarser levels)");
  // No level 0, no lin_depth, whatever else there is.
  in = TypicalInputs();
  in.depth[0] = sc::kNoSlot;
  in.normal = 33;
  f = sc::MakeFrame(in, 1);
  Check(!f.Has(sc::kLinDepth) && f.Has(sc::kNormal) && f.normal_slot == 33, "products are valid one by one");
  // The full size depth, the normal, the colour chain.
  in = TypicalInputs();
  in.depth_full = 40;
  in.normal = 41;
  in.colour_pre[0] = 50;
  in.colour_pre[1] = 51;
  in.colour_pre[2] = 52;
  f = sc::MakeFrame(in, 1);
  Check(f.valid == (sc::ProductBit(sc::kLinDepth) | sc::ProductBit(sc::kLinDepthFull) | sc::ProductBit(sc::kNormal) | sc::ProductBit(sc::kColourPre)) && f.lin_depth_full_slot == 40 &&
            f.normal_slot == 41 && f.colour_pre_slot[2] == 52,
        "the adopted products are published with their slots");
  Check(!f.Has(sc::kColourPost) && !f.Has(sc::kHistory) && f.history_colour_slot == sc::kNoSlot, "colour_post and the history are not in a frame whose inputs have none");
  // The Hi-Z: valid with every level, never with a gap or too many levels.
  in = TypicalInputs();
  in.hiz_levels = 7;
  for (uint32_t i = 0; i < 7; ++i) in.hiz[i] = 200 + i;
  f = sc::MakeFrame(in, 1);
  Check(f.Has(sc::kHizMin) && f.hiz_levels == 7 && f.hiz_min_slot[0] == 200 && f.hiz_min_slot[6] == 206 && f.hiz_min_slot[7] == sc::kNoSlot, "a whole chain is valid");
  in.hiz[3] = sc::kNoSlot;
  f = sc::MakeFrame(in, 1);
  Check(!f.Has(sc::kHizMin) && f.hiz_levels == 0 && f.hiz_min_slot[0] == sc::kNoSlot, "a chain with a missing level is not published");
  in.hiz[3] = 203;
  in.hiz_levels = sc::kHizMaxLevels + 1;
  Check(!sc::MakeFrame(in, 1).Has(sc::kHizMin), "more levels than the slot array is not published");
  in.hiz_levels = 0;
  Check(!sc::MakeFrame(in, 1).Has(sc::kHizMin), "no levels, no Hi-Z");
  // The camera is copied whole.
  in = TypicalInputs();
  in.camera.tan_half_fov[0] = 0.5f;
  in.camera.lin[3] = 2.0f;
  in.camera.flags = sc::kCameraValid;
  in.camera.view_projection[5] = 3.0f;
  f = sc::MakeFrame(in, 1);
  Check(f.camera.tan_half_fov[0] == 0.5f && f.camera.lin[3] == 2.0f && f.camera.flags == sc::kCameraValid && f.camera.view_projection[5] == 3.0f, "the camera is the owner's");
}

void GenerationChecks() {
  sc::Generation gen;
  sc::FrameInputs in = TypicalInputs();
  sc::ScreenFrame f = sc::MakeFrame(in, 0);
  const uint32_t first = gen.Of(sc::SignatureOf(in, f));
  Check(first == 1, "the first published frame has generation 1 (0 means none)");
  // The same set of images, a new serial and slots that swap (the AO's depth ping-pong with the bounce light on): the same generation.
  in.serial = 778;
  std::swap(in.depth[0], in.depth[1]);
  f = sc::MakeFrame(in, 0);
  Check(gen.Of(sc::SignatureOf(in, f)) == first, "a new frame with the depth images traded is the same set: the generation stays");
  // A new size, new samples, a product appearing or going, a pause view, the owner's images made again: a new generation each.
  const auto next = [&](const char* what, auto change) {
    sc::FrameInputs changed = in;
    change(changed);
    const sc::ScreenFrame fc = sc::MakeFrame(changed, 0);
    const uint32_t before = gen.Of(sc::SignatureOf(in, sc::MakeFrame(in, 0)));
    const uint32_t after = gen.Of(sc::SignatureOf(changed, fc));
    Check(after != before, std::string("the generation moves when ") + what);
    // and back: the old set is a new generation again (a set that came back is not the one that left: the caches were dropped)
    const uint32_t back = gen.Of(sc::SignatureOf(in, sc::MakeFrame(in, 0)));
    Check(back != after, std::string("... and again when it goes back (") + what + ")");
  };
  next("the traced size changes", [](sc::FrameInputs& c) { c.traced[0] = 1920; c.traced[1] = 1080; });
  next("the picture's size changes", [](sc::FrameInputs& c) { c.render[0] = 2560; c.render[1] = 1440; });
  next("the host samples change", [](sc::FrameInputs& c) { c.host_samples = 8; });
  next("the pause view starts", [](sc::FrameInputs& c) { c.flags |= sc::kFramePause; });
  next("the normal image appears", [](sc::FrameInputs& c) { c.normal = 60; });
  next("the full size depth appears", [](sc::FrameInputs& c) { c.depth_full = 61; });
  next("the bounce light's colour chain appears", [](sc::FrameInputs& c) { c.colour_pre[0] = 62; });
  next("the Hi-Z appears", [](sc::FrameInputs& c) { c.hiz_levels = 2; c.hiz[0] = 70; c.hiz[1] = 71; });
  next("the owner's images are made again", [](sc::FrameInputs& c) { c.owner_generation += 1; });
  next("the Hi-Z's images are made again", [](sc::FrameInputs& c) { c.hiz_generation += 1; });
}


// ---------------------------------------------------------------------------
// SS-1: the colour captures, the history, the exposure
// ---------------------------------------------------------------------------
void ColourChecks() {
  using sc::Consumer;
  using sc::ProductBit;
  // The size: the AO's HEIGHT rule alone (ao_size.h kHalfAboveLines), the picture's up to 1080 lines and half of it (rounded up) above.
  Check(sc::kColourPostHalfAboveLines == mcla::gpu::ao_size::kHalfAboveLines, "colour_post's height rule is the AO's threshold");
  Check(!sc::ColourPostIsHalf(720) && !sc::ColourPostIsHalf(1080) && sc::ColourPostIsHalf(1081) && sc::ColourPostIsHalf(1440) && sc::ColourPostIsHalf(2160), "half size from above 1080 lines");
  Check(sc::ColourPostEdge(1920, false) == 1920 && sc::ColourPostEdge(3840, true) == 1920 && sc::ColourPostEdge(2161, true) == 1081 && sc::ColourPostEdge(1, true) == 1, "the edge: whole, or half rounded up");
  for (uint32_t extent : {1u, 2u, 3u, 5u, 719u, 1081u, 2160u, 3841u}) {
    Check(sc::ColourPostEdge(extent, true) == mcla::gpu::ao_size::Half(extent), Num("half of %.0f is the AO's Half", double(extent)));
  }
  // A trigger frame whose depth has no colour partner: quiet while it is short (the game's first frames), an error when it lasts, never an error for a frame that has its colour.
  Check(sc::JudgeColourPartner(true, 0) == sc::ColourPartner::kFound && sc::JudgeColourPartner(true, 100000) == sc::ColourPartner::kFound, "a colour target that exists is found, however long the run");
  Check(sc::JudgeColourPartner(false, 1) == sc::ColourPartner::kEarly && sc::JudgeColourPartner(false, sc::kColourMissingGraceFrames) == sc::ColourPartner::kEarly, "a missing colour target is early for the grace's frames");
  Check(sc::JudgeColourPartner(false, sc::kColourMissingGraceFrames + 1) == sc::ColourPartner::kMissingTooLong && sc::JudgeColourPartner(false, 1u << 30) == sc::ColourPartner::kMissingTooLong,
        "a missing colour target that lasts past the grace is an error");
  Check(sc::kColourMissingGraceFrames == 300, "the grace is 300 trigger frames (a few seconds: the game's first frames take about a second)");
  // The levels: one without the mips, up to four halvings with them, never past the smaller side.
  Check(sc::ColourPostLevelCount(1920, 1080, false) == 1 && sc::ColourPostLevelCount(1920, 1080, true) == 1 + sc::kColourPostMipHalvings && sc::ColourPostLevelCount(960, 540, true) == 5,
        "the chain is a single image without the mips and five levels with them at 1080p and its half");
  Check(sc::ColourPostLevelCount(8, 8, true) == 4 && sc::ColourPostLevelCount(2, 2, true) == 2 && sc::ColourPostLevelCount(1, 1, true) == 1 && sc::ColourPostLevelCount(4096, 2, true) == 2,
        "the smaller side limits the mips");
  Check(1 + sc::kColourPostMipHalvings <= sc::kColourPostMaxLevels, "the chain fits the frame's slot array");
  // The memory: the plan's figure (5.4) is about 21 MiB at 1080 lines with the chain; without it the level alone.
  const double with_mips = double(sc::ColourPostBytes(1920, 1080, 5)) / (1024.0 * 1024.0);
  const double single = double(sc::ColourPostBytes(1920, 1080, 1)) / (1024.0 * 1024.0);
  Check(with_mips > 18.0 && with_mips < 22.0 && single > 15.5 && single < 16.0, Num("colour_post at 1920x1080 is %.1f MiB with five levels and %.1f MiB alone (the plan: about 21 with its chain)", with_mips, single));
  Check(sc::ColourPostBytes(1, 1, 1) == 8 && sc::ColourPostBytes(4, 4, 3) == (16 + 4 + 1) * 8, "RGBA16F is 8 bytes a texel and the levels add");
  // Whether the chain has its mips: only the reflections' own want of colour_post, or a view of a level above 0.
  sc::Wants wants;
  Check(!sc::ColourPostWithMips(wants, sc::View::kOff, 0), "no consumer, no mips");
  wants.Set(Consumer::kCarHistory, ProductBit(sc::kColourPost));
  wants.Set(Consumer::kBounceLight, ProductBit(sc::kColourPre));
  Check(!sc::ColourPostWithMips(wants, sc::View::kOff, 0), "the car history's want makes no mips");
  wants.Set(Consumer::kReflections, ProductBit(sc::kColourPost));
  Check(sc::ColourPostWithMips(wants, sc::View::kOff, 0), "the reflections' want makes the mips");
  {
    // CITY-RT M2: the ray traced reflections of the car paint read the city hits' colour at the mip of the hit's footprint: their want makes the mips too
    sc::Wants rt;
    rt.Set(Consumer::kCarRtRefl, ProductBit(sc::kColourPost) | ProductBit(sc::kLinDepth));
    Check(sc::ColourPostWithMips(rt, sc::View::kOff, 0), "the car ray traced reflections' want of colour_post makes the mips");
    rt.Set(Consumer::kCarRtRefl, ProductBit(sc::kLinDepth));
    Check(!sc::ColourPostWithMips(rt, sc::View::kOff, 0), "... a want of lin_depth alone makes none");
  }
  sc::Wants none;
  Check(sc::ColourPostWithMips(none, sc::View::kColourPost, 2) && !sc::ColourPostWithMips(none, sc::View::kColourPost, 0) && !sc::ColourPostWithMips(none, sc::View::kHistory, 3),
        "a view of colour_post above level 0 makes the mips, level 0 and the history view do not");

  // The exposure bias: 2^bias (the main target's bias 4 is 16), 1 where no draw has told it, clamped like the car history's ExpBiasGain.
  Check(sc::ExposureOf(4, true) == 16.0f && sc::ExposureOf(0, true) == 1.0f && sc::ExposureOf(-3, true) == 0.125f && sc::ExposureOf(4, false) == 1.0f && sc::ExposureOf(99, true) == 16777216.0f &&
            sc::ExposureOf(-99, true) == 1.0f / 16777216.0f,
        "the exposure is 2^bias (1 until told), clamped to 24 octaves");
  for (int bias = -24; bias <= 24; ++bias) {
    Check(sc::ExposureOf(bias, true) == std::ldexp(1.0f, bias), Num("the exposure of %.0f is the exact power of two", double(bias)));
  }

  // The plan: colour_post is made when a consumer wants it, for the history, or for its view; the car history's want counts (the source setting 2) but starts no depth frame.
  sc::Plan plan = sc::PlanFrame(none, sc::View::kOff, 0);
  Check(plan.make == 0, "nothing is made for nobody");
  sc::Wants car;
  car.Set(Consumer::kCarHistory, ProductBit(sc::kColourPost));
  plan = sc::PlanFrame(car, sc::View::kOff, 0);
  Check(plan.make == ProductBit(sc::kColourPost) && !plan.NeedsDepthChain() && plan.external == ProductBit(sc::kColourPost),
        "the car history's want makes colour_post, asks for no depth chain, and counts as outside the AO's family");
  sc::Wants ssr;
  ssr.Set(Consumer::kReflections, ProductBit(sc::kLinDepth) | ProductBit(sc::kHizMin) | ProductBit(sc::kColourPost) | ProductBit(sc::kHistory));
  plan = sc::PlanFrame(ssr, sc::View::kOff, 0);
  Check(plan.make == (ProductBit(sc::kHizMin) | ProductBit(sc::kColourPost) | ProductBit(sc::kHistory)) && plan.NeedsDepthChain(), "SSR's want makes the Hi-Z, colour_post and the history");
  sc::Wants only_history;
  only_history.Set(Consumer::kReflections, ProductBit(sc::kHistory));
  plan = sc::PlanFrame(only_history, sc::View::kOff, 0);
  Check((plan.make & ProductBit(sc::kColourPost)) != 0 && (plan.make & ProductBit(sc::kHistory)) != 0 && (plan.wanted & ProductBit(sc::kColourPost)) != 0,
        "the history is the last frame's colour_post: wanting it makes it");
  Check(sc::PlanFrame(none, sc::View::kColourPost, 1).make == ProductBit(sc::kColourPost) && sc::PlanFrame(none, sc::View::kHistory, 0).make == (ProductBit(sc::kColourPost) | ProductBit(sc::kHistory)) &&
            !sc::PlanFrame(none, sc::View::kColourPost, 0).NeedsDepthChain() && !sc::PlanFrame(none, sc::View::kHistory, 0).NeedsDepthChain(),
        "the colour views make what they show and run no depth frame");
  Check(sc::ViewNeeds(sc::View::kColourPost) == ProductBit(sc::kColourPost) && sc::ViewNeeds(sc::View::kHistory) == ProductBit(sc::kHistory), "the colour views' needs");
  Check(sc::kMadeHere == (ProductBit(sc::kHizMin) | ProductBit(sc::kColourPost) | ProductBit(sc::kHistory)), "the owner makes the Hi-Z, colour_post and the history, the AO's products are adopted");

  // The history's rule.
  sc::CaptureKey last;
  last.valid = true;
  last.frame = 41;
  last.pause = false;
  last.size[0] = 1920;
  last.size[1] = 1080;
  sc::CaptureKey now = last;
  now.frame = 42;
  Check(sc::HistoryUsable(last, now), "the capture of the frame before, same view, same size is the history");
  sc::CaptureKey bad = now;
  bad.frame = 43;
  Check(!sc::HistoryUsable(last, bad), "a frame skipped: no history");
  bad = now;
  bad.frame = 41;
  Check(!sc::HistoryUsable(last, bad), "the same frame again: no history");
  bad = now;
  bad.pause = true;
  Check(!sc::HistoryUsable(last, bad), "another kind of view: no history");
  bad = now;
  bad.size[1] = 540;
  Check(!sc::HistoryUsable(last, bad), "another size: no history");
  sc::CaptureKey stale = last;
  stale.valid = false;
  Check(!sc::HistoryUsable(stale, now) && !sc::HistoryUsable(last, sc::CaptureKey{}), "an invalid capture is no history on either side");

  // The frame with colour_post, its mips and the history.
  sc::FrameInputs in = TypicalInputs();
  in.colour_post_levels = 3;
  in.colour_post[0] = 300;
  in.colour_post[1] = 301;
  in.colour_post[2] = 302;
  in.colour_post_size[0] = 1920;
  in.colour_post_size[1] = 1080;
  sc::ScreenFrame f = sc::MakeFrame(in, 1);
  Check(f.Has(sc::kColourPost) && !f.Has(sc::kHistory) && f.colour_post_levels == 3 && f.colour_post_slot[0] == 300 && f.colour_post_slot[2] == 302 && f.colour_post_slot[3] == sc::kNoSlot &&
            f.history_colour_slot == sc::kNoSlot,
        "colour_post is valid with its levels and no history");
  in.history_colour = 310;
  f = sc::MakeFrame(in, 1);
  Check(f.Has(sc::kColourPost) && f.Has(sc::kHistory) && f.history_colour_slot == 310 && f.history_depth_slot == sc::kNoSlot, "the history is valid with a colour slot (its depth is SS-2's)");
  in.history_depth = 311;
  Check(sc::MakeFrame(in, 1).history_depth_slot == 311, "... and carries a depth slot when there is one");
  in.colour_post[1] = sc::kNoSlot;
  f = sc::MakeFrame(in, 1);
  Check(!f.Has(sc::kColourPost) && !f.Has(sc::kHistory) && f.colour_post_levels == 0, "a chain with a missing level is not published, and the history with it is not");
  in.colour_post[1] = 301;
  in.colour_post_levels = sc::kColourPostMaxLevels + 1;
  Check(!sc::MakeFrame(in, 1).Has(sc::kColourPost), "more levels than the slot array is not published");
  in = TypicalInputs();
  in.history_colour = 310;
  Check(!sc::MakeFrame(in, 1).Has(sc::kHistory), "a history slot without this frame's colour_post is nothing");

  // The generation across the owner's steps: the frame is made in two phases (the AO's publication, then the capture) and sealed once, so what the sealed generation counts is the final set. A
  // trade of the capture with its history (the ping-pong: the slots swap places every frame) is not a new set, a new size or a remade image is.
  sc::Generation gen;
  sc::FrameInputs a = TypicalInputs();
  a.colour_post_levels = 1;
  a.colour_post[0] = 400;
  a.colour_post_size[0] = 1920;
  a.colour_post_size[1] = 1080;
  a.history_colour = 401;
  const uint32_t g1 = gen.Of(sc::SignatureOf(a, sc::MakeFrame(a, 0)));
  a.serial += 1;
  std::swap(a.colour_post[0], a.history_colour);
  Check(gen.Of(sc::SignatureOf(a, sc::MakeFrame(a, 0))) == g1, "the capture and its history trading places is the same set of images");
  sc::FrameInputs b = a;
  b.colour_generation += 1;
  Check(gen.Of(sc::SignatureOf(b, sc::MakeFrame(b, 0))) != g1, "a remade colour image is a new set");
  sc::FrameInputs c = b;
  const uint32_t g3 = gen.Of(sc::SignatureOf(c, sc::MakeFrame(c, 0)));
  c.colour_post_size[0] = 960;
  c.colour_post_size[1] = 540;
  Check(gen.Of(sc::SignatureOf(c, sc::MakeFrame(c, 0))) != g3, "a new colour_post size is a new set");
  c = b;
  c.history_colour = sc::kNoSlot;
  Check(gen.Of(sc::SignatureOf(c, sc::MakeFrame(c, 0))) != g3, "the history going is a new set");
  c = b;
  c.colour_post_levels = 3;
  c.colour_post[1] = 421;
  c.colour_post[2] = 422;
  const uint32_t g4 = gen.Of(sc::SignatureOf(c, sc::MakeFrame(c, 0)));
  Check(g4 != g3, "the mips appearing is a new set");
}

// ---------------------------------------------------------------------------
// SS-1: the camera history
// ---------------------------------------------------------------------------
mcla::gpu::temporal::Camera MakeCamera(double fov_y, double aspect, double near_plane, double far_plane, double x) {
  mcla::gpu::temporal::Camera camera;
  camera.fov_y = fov_y;
  camera.aspect = aspect;
  camera.near_plane = near_plane;
  camera.far_plane = far_plane;
  camera.position[0] = x;
  for (int i = 0; i < 4; ++i) {
    camera.view_projection.m[i][i] = 1.0 + x * 0.001 + i;  // a recognisable matrix, a different one for each camera
  }
  return camera;
}

void CameraHistoryChecks() {
  using mcla::gpu::temporal::Camera;
  sc::CameraHistory h;
  const Camera a = MakeCamera(0.8, 16.0 / 9.0, 0.2, 5000.0, 1.0);
  const Camera b = MakeCamera(0.8, 16.0 / 9.0, 0.2, 5000.0, 2.0);
  const Camera c = MakeCamera(0.8, 16.0 / 9.0, 0.2, 5000.0, 3.0);
  Check(!h.has_camera() && h.Previous(1, false) == nullptr && h.Current(0) == nullptr && h.Before(1, false) == nullptr && !h.LensAgrees(0) && h.LastPauseCamera() == nullptr, "no history at the start");
  // The play view, three frames in a row: the trigger of a frame finds the frame before's camera, and the pair at the third.
  h.OnAnchor(10, false, &a);
  Check(h.has_camera() && h.Current(10) != nullptr && h.Current(10)->position[0] == 1.0 && h.Current(9) == nullptr && h.Current(11) == nullptr, "the anchor's own camera is the current one of its frame only");
  Check(h.Previous(11, false) != nullptr && h.Previous(11, false)->position[0] == 1.0 && h.Before(11, false) == nullptr, "the next frame's trigger finds it as the previous camera, with no camera before it");
  Check(h.Previous(10, false) == nullptr && h.Previous(12, false) == nullptr && h.Previous(11, true) == nullptr, "... only for the frame after, and only in the same kind of view");
  Check(!h.LensAgrees(10), "the first anchor has no lens to agree with");
  h.OnAnchor(11, false, &b);
  Check(h.LensAgrees(11) && h.Previous(12, false)->position[0] == 2.0 && h.Before(12, false) != nullptr && h.Before(12, false)->position[0] == 1.0, "the second: the lens agrees, and the camera before the previous is the first");
  Check(!h.LensAgrees(10) && !h.LensAgrees(12), "the lens answer belongs to its frame");
  // A lens that moved more than 5% on any of the four numbers does not agree (the pause view's zoom, a sweep); the test is against the frame before's lens.
  const Camera wide = MakeCamera(0.8 * 1.06, 16.0 / 9.0, 0.2, 5000.0, 4.0);
  h.OnAnchor(12, false, &wide);
  Check(!h.LensAgrees(12) && h.Previous(13, false) != nullptr && h.Before(13, false) != nullptr, "a field of view 6% wider does not agree, but the cameras are still the previous and the one before");
  const Camera near_far = MakeCamera(0.8 * 1.06, 16.0 / 9.0, 0.2 * 1.04, 5000.0 * 0.96, 5.0);
  h.OnAnchor(13, false, &near_far);
  Check(h.LensAgrees(13), "4% on the near and the far planes is within the tolerance");
  const Camera aspect = MakeCamera(0.8 * 1.06, 16.0 / 9.0 * 1.06, 0.2 * 1.04, 5000.0 * 0.96, 6.0);
  h.OnAnchor(14, false, &aspect);
  Check(!h.LensAgrees(14), "an aspect 6% wider does not agree");
  const Camera aspect_again = MakeCamera(0.8 * 1.06, 16.0 / 9.0 * 1.06, 0.2 * 1.04, 5000.0 * 0.96, 7.0);
  h.OnAnchor(15, false, &aspect_again);
  Check(h.LensAgrees(15), "the same lens as the frame before agrees (the tolerance is against the frame before's, not the first's)");
  const Camera far_far = MakeCamera(0.8 * 1.06, 16.0 / 9.0 * 1.06, 0.2 * 1.04, 5000.0 * 0.96 * 1.07, 8.0);
  h.OnAnchor(16, false, &far_far);
  Check(!h.LensAgrees(16), "a far plane 7% further does not agree");
  // The tolerance is against the previous: the same numbers as the rule of the car self-reflection (relative to the previous frame's lens).
  Check(sc::kLensTolerance == 0.05 && sc::LensesAgree(a, a) && !sc::LensesAgree(MakeCamera(0.8 * 1.0501, 16.0 / 9.0, 0.2, 5000.0, 0), a) && sc::LensesAgree(MakeCamera(0.8 * 1.0499, 16.0 / 9.0, 0.2, 5000.0, 0), a),
        "the lens rule is 5% of the previous frame's number, on each of the four");
  // A gap: a frame that gave no anchor (a skipped frame, a menu) leaves no previous camera and no pair.
  h.OnAnchor(20, false, &a);
  h.OnAnchor(22, false, &b);
  Check(h.Previous(23, false) != nullptr && h.Before(23, false) == nullptr && !h.LensAgrees(22), "a frame skipped: the next one still has the camera before it, but no pair and no lens test");
  // Another kind of view: never mixed.
  h.OnAnchor(30, false, &a);
  h.OnAnchor(31, true, &b);
  Check(h.Previous(32, true) != nullptr && h.Previous(32, false) == nullptr && h.Before(32, true) == nullptr && !h.LensAgrees(31), "a pause view after a play view starts its own history");
  Check(h.LastPauseCamera() != nullptr && h.LastPauseCamera()->position[0] == 2.0, "the frame's world is classified by the pause camera");
  h.OnAnchor(32, true, &c);
  Check(h.LensAgrees(32) && h.Before(33, true) != nullptr && h.Before(33, true)->position[0] == 2.0, "... and then has its pair");
  h.OnAnchor(33, false, &a);
  Check(h.LastPauseCamera() == nullptr, "the last camera of the play kind is not a pause camera");
  // No camera: the history starts again.
  h.OnAnchor(40, false, &a);
  h.OnAnchor(41, false, nullptr);
  Check(!h.has_camera() && h.Previous(42, false) == nullptr && h.Current(41) == nullptr && !h.LensAgrees(41), "an anchor with no camera clears the history");
  h.OnAnchor(42, false, &b);
  Check(h.Previous(43, false) != nullptr && h.Before(43, false) == nullptr && !h.LensAgrees(42), "... and it starts over");
  // A second anchor in the same frame (never seen in the game) is no continuation: no lens agreement, no pair.
  h.OnAnchor(50, false, &a);
  h.OnAnchor(50, false, &b);
  Check(!h.LensAgrees(50) && h.Before(51, false) == nullptr, "two anchors in one frame agree on nothing");
  h.Reset();
  Check(!h.has_camera() && h.Previous(51, false) == nullptr, "Reset forgets everything");

  // The products' camera pair: only when the camera the products hold is the previous one.
  sc::CameraHistory pairs;
  pairs.OnAnchor(1, false, &a);
  pairs.OnAnchor(2, false, &b);
  sc::ScreenCamera camera;
  camera.flags = sc::kCameraValid;
  for (int r = 0; r < 4; ++r) {
    for (int col = 0; col < 4; ++col) camera.view_projection[r * 4 + col] = float(b.view_projection.m[r][col]);
  }
  Check(sc::FillPreviousViewProjection(pairs.Previous(3, false), pairs.Before(3, false), &camera) && (camera.flags & sc::kCameraHasPrevious) != 0 && camera.prev_view_projection[0] == float(a.view_projection.m[0][0]) &&
            camera.prev_view_projection[15] == float(a.view_projection.m[3][3]),
        "the pair is filled with the camera before the previous");
  sc::ScreenCamera other = camera;
  other.flags = sc::kCameraValid;
  other.view_projection[0] += 1.0f;
  for (float& v : other.prev_view_projection) v = -1.0f;
  Check(!sc::FillPreviousViewProjection(pairs.Previous(3, false), pairs.Before(3, false), &other) && (other.flags & sc::kCameraHasPrevious) == 0 && other.prev_view_projection[0] == -1.0f,
        "a camera that is not the previous one gets no pair");
  sc::ScreenCamera none_valid;
  Check(!sc::FillPreviousViewProjection(pairs.Previous(3, false), pairs.Before(3, false), &none_valid), "a camera that is not valid gets no pair");
  Check(!sc::FillPreviousViewProjection(nullptr, pairs.Before(3, false), &camera) && !sc::FillPreviousViewProjection(pairs.Previous(3, false), nullptr, &camera), "no previous, or nothing before it: no pair");
}

// ---------------------------------------------------------------------------
// SS-1: the capture's arithmetic against the car history pass's
// ---------------------------------------------------------------------------
// The two HLSL loops as C++ (the same operations in the same order on float): hlsl/car_history_ps.hlsl's multisampled colour and hlsl/screen_colour_ps.hlsl's level 0 at the picture's size. The
// car history pass makes its colour from the samples directly; with the source setting the capture makes it, the pass reads it, and the image must be the same bytes.
struct Rgb {
  float r, g, b;
};
float ClampColour(float v) {
  // HeldChannel in both shaders: a NaN (decided by the bits, not by fmax, which Vulkan leaves open for a NaN) is 0, else min(max(v, 0), 60000)
  if ((std::bit_cast<uint32_t>(v) & 0x7FFFFFFFu) > 0x7F800000u) return 0.0f;
  const float m = (v > 0.0f) ? v : 0.0f;
  return m < 60000.0f ? m : 60000.0f;
}
Rgb CarHistoryColour(const std::vector<Rgb>& samples) {
  Rgb sum = {0, 0, 0};
  for (const Rgb& s : samples) {
    sum.r += s.r;
    sum.g += s.g;
    sum.b += s.b;
  }
  const float n = float(samples.size());
  return {ClampColour(sum.r / n), ClampColour(sum.g / n), ClampColour(sum.b / n)};
}
// The capture at ratio 1 (the picture's size): the pixel's mean added to a zero total and divided by 1.
Rgb CaptureColour(const std::vector<Rgb>& samples, int ratio) {
  Rgb total = {0, 0, 0};
  for (int j = 0; j < ratio; ++j) {
    for (int i = 0; i < ratio; ++i) {
      Rgb sum = {0, 0, 0};
      for (const Rgb& s : samples) {
        sum.r += s.r;
        sum.g += s.g;
        sum.b += s.b;
      }
      const float n = float(samples.size());
      total.r += sum.r / n;
      total.g += sum.g / n;
      total.b += sum.b / n;
    }
  }
  const float d = float(ratio * ratio);
  return {ClampColour(total.r / d), ClampColour(total.g / d), ClampColour(total.b / d)};
}
bool SameBits(const Rgb& a, const Rgb& b) { return BitsOf(a.r) == BitsOf(b.r) && BitsOf(a.g) == BitsOf(b.g) && BitsOf(a.b) == BitsOf(b.b); }

void CaptureArithmeticChecks() {
  Lcg rng;
  bool same = true;
  bool same_nan = true;
  for (int i = 0; i < 20000; ++i) {
    const int n = 1 << (i % 4);  // 1, 2, 4 or 8 host samples
    std::vector<Rgb> samples;
    for (int k = 0; k < n; ++k) {
      // the 7e3 target's range: unsigned, up to about 32 times the bias, a few large (a lamp's glow), some zeros
      const auto value = [&]() { return rng.Unit() < 0.15f ? 0.0f : (rng.Unit() < 0.1f ? rng.Range(100.0f, 900.0f) : rng.Range(0.0f, 4.0f)); };
      samples.push_back({value(), value(), value()});
    }
    same = same && SameBits(CarHistoryColour(samples), CaptureColour(samples, 1));
    if (i % 97 == 0) {
      samples[0].g = std::numeric_limits<float>::quiet_NaN();  // a NaN sample: the mean is NaN, held to 0 by both, never a NaN in the image
      const Rgb a = CarHistoryColour(samples);
      const Rgb b = CaptureColour(samples, 1);
      same_nan = same_nan && SameBits(a, b) && a.g == 0.0f && !std::isnan(a.r) && !std::isnan(b.g);
    }
  }
  Check(same, "the capture at the picture's size is the car history pass's colour bit for bit (1, 2, 4 and 8 host samples, 20000 random pixels)");
  Check(same_nan, "... a NaN sample is held to black by both and never reaches the image");
  // At half size the texel is the mean of four pixels' means: a different filter, not the car history's (which is why the pass reads the capture only at the picture's size).
  std::vector<Rgb> flat = {{1, 2, 3}, {1, 2, 3}};
  Check(SameBits(CaptureColour(flat, 2), {1.0f, 2.0f, 3.0f}), "four equal pixels at half size give their colour");
  Check(CaptureColour({{70000.0f, 0.0f, 0.0f}}, 1).r == 60000.0f && CaptureColour({{70000.0f, 0.0f, 0.0f}}, 1).g == 0.0f, "the colour is held to what a half float keeps");
}

// ---------------------------------------------------------------------------
// the depth's low bits and the Hi-Z's two operations
// ---------------------------------------------------------------------------
void MarkChecks() {
  // The AO keeps the vehicle mark in bit 0 and, with the G-buffer's normals at half size, the source of the pixel in bits 1 and 2 (ao_gbuffer.h): the Hi-Z clears all three.
  Lcg rng;
  bool all = true;
  bool never_up = true;
  uint32_t worst_ulps = 0;
  for (int i = 0; i < 20000; ++i) {
    const float depth = rng.Range(0.05f, 900.0f);
    const bool vehicle = (rng.Next() & 1u) != 0;
    const uint32_t source = rng.Next() & 3u;
    const float marked = ag::SetSource(ag::SetVehicleBit(depth, vehicle), source);
    const float stripped = sc::StripMarks(marked);
    all = all && (BitsOf(stripped) & 7u) == 0 && (BitsOf(stripped) == (BitsOf(marked) & ~7u));
    never_up = never_up && stripped <= marked;
    worst_ulps = std::max(worst_ulps, BitsOf(marked) - BitsOf(stripped));
  }
  Check(all, "StripMarks clears exactly the three low bits (the vehicle mark and the half size source)");
  Check(never_up && worst_ulps <= 7, Num("a stripped depth is never above the marked one and at most 7 ulps below (worst: %.0f)", double(worst_ulps)));
  Check(sc::StripMarks(sc::kSky) == sc::kSky, "the sky's distance has no mark bits to lose");
  Check(sc::kDepthMarkMask == (ag::BitsOf(ag::SetSource(ag::FloatOf(0), 3)) | ag::BitsOf(ag::SetVehicleBit(ag::FloatOf(0), true))), "the mask is the union of the AO's mark bits");
}

// A depth image: width x height floats.
struct Image {
  uint32_t w = 0;
  uint32_t h = 0;
  std::vector<float> v;
  float At(int32_t x, int32_t y) const { return v[size_t(y) * w + size_t(x)]; }
};

Image RandomDepth(Lcg& rng, uint32_t w, uint32_t h, bool marks) {
  Image img;
  img.w = w;
  img.h = h;
  img.v.resize(size_t(w) * h);
  for (float& d : img.v) {
    const uint32_t pick = rng.Next() % 16;
    d = pick == 0 ? sc::kSky : (pick < 4 ? rng.Range(0.3f, 4.0f) : rng.Range(4.0f, 400.0f));
    if (marks) {
      d = ag::SetSource(ag::SetVehicleBit(d, (rng.Next() & 3u) == 0), rng.Next() & 3u);
    }
  }
  return img;
}

// The chain as the shader makes it: level 0 the stripped depth, each level above the min of the four texels below (clamped at the edge), through the twin's functions.
std::vector<Image> ChainByTwin(const Image& level0) {
  std::vector<Image> chain;
  const uint32_t levels = sc::HizLevelCount(level0.w, level0.h);
  for (uint32_t level = 0; level < levels; ++level) {
    Image out;
    out.w = sc::HizLevelSize(level0.w, level);
    out.h = sc::HizLevelSize(level0.h, level);
    out.v.resize(size_t(out.w) * out.h);
    for (uint32_t y = 0; y < out.h; ++y) {
      for (uint32_t x = 0; x < out.w; ++x) {
        float value;
        if (level == 0) {
          value = sc::StripMarks(level0.At(int32_t(x), int32_t(y)));
        } else {
          const Image& below = chain[level - 1];
          const sc::Footprint fp = sc::FootprintOf(int32_t(x), int32_t(y), int32_t(below.w) - 1, int32_t(below.h) - 1);
          value = sc::Min4(below.At(fp.x[0], fp.y[0]), below.At(fp.x[1], fp.y[1]), below.At(fp.x[2], fp.y[2]), below.At(fp.x[3], fp.y[3]));
        }
        out.v[size_t(y) * out.w + x] = value;
      }
    }
    chain.push_back(std::move(out));
  }
  return chain;
}

// The definition of the chain: texel (x, y) of level L is the min of the stripped level 0 texels it covers, [x << L, min((x + 1) << L, w)) x [y << L, ...).
float BruteForce(const Image& level0, uint32_t level, uint32_t x, uint32_t y) {
  const uint32_t x0 = x << level, x1 = std::min((x + 1) << level, level0.w);
  const uint32_t y0 = y << level, y1 = std::min((y + 1) << level, level0.h);
  float best = std::numeric_limits<float>::max();
  for (uint32_t yy = y0; yy < y1; ++yy) {
    for (uint32_t xx = x0; xx < x1; ++xx) {
      best = std::min(best, sc::StripMarks(level0.At(int32_t(xx), int32_t(yy))));
    }
  }
  return best;
}

void ChainChecks() {
  Lcg rng;
  const uint32_t sizes[][2] = {{1, 1}, {2, 2}, {3, 2}, {7, 5}, {1, 9}, {9, 1}, {33, 17}, {64, 64}, {100, 37}, {65, 129}, {320, 180}, {641, 361}};
  for (const auto& size : sizes) {
    const Image level0 = RandomDepth(rng, size[0], size[1], true);
    const std::vector<Image> chain = ChainByTwin(level0);
    bool same = chain.size() == sc::HizLevelCount(size[0], size[1]);
    uint32_t wrong_at = 0;
    for (uint32_t level = 0; level < chain.size() && same; ++level) {
      same = chain[level].w == sc::HizLevelSize(size[0], level) && chain[level].h == sc::HizLevelSize(size[1], level);
      for (uint32_t y = 0; y < chain[level].h && same; ++y) {
        for (uint32_t x = 0; x < chain[level].w; ++x) {
          if (chain[level].At(int32_t(x), int32_t(y)) != BruteForce(level0, level, x, y)) {
            same = false;
            wrong_at = level;
            break;
          }
        }
      }
    }
    Check(same, Num("the chain of a %.0fx%.0f depth image is the min over each texel's footprint at every level (first wrong level %.0f)", double(size[0]), double(size[1]), double(wrong_at)));
  }
  // One at the traced size of a 1080p picture at the AO's high (960x540: 7 levels) and one at the AO's ultra (1920x1080): the same, and the coarsest texel is the min of the whole block.
  for (const auto& size : {std::array<uint32_t, 2>{960, 540}, std::array<uint32_t, 2>{1920, 1080}}) {
    const Image level0 = RandomDepth(rng, size[0], size[1], true);
    const std::vector<Image> chain = ChainByTwin(level0);
    bool ok = chain.size() == 7;
    float global = std::numeric_limits<float>::max();
    for (const float d : level0.v) global = std::min(global, sc::StripMarks(d));
    float coarse = std::numeric_limits<float>::max();
    for (const float d : chain.back().v) coarse = std::min(coarse, d);
    ok = ok && coarse == global;
    // Every texel of every level is at or below every pixel it covers (the property a ray's early-out needs).
    for (uint32_t level = 1; level < chain.size() && ok; ++level) {
      for (uint32_t y = 0; y < level0.h && ok; y += 7) {
        for (uint32_t x = 0; x < level0.w; x += 5) {
          const uint32_t cx = std::min(x >> level, chain[level].w - 1), cy = std::min(y >> level, chain[level].h - 1);
          if (chain[level].At(int32_t(cx), int32_t(cy)) > sc::StripMarks(level0.At(int32_t(x), int32_t(y)))) {
            ok = false;
            break;
          }
        }
      }
    }
    Check(ok, Num("the chain at %.0fx%.0f: 7 levels, the coarsest min is the image's min, no texel is above a pixel it covers", double(size[0]), double(size[1])));
  }
}

// ao_mip_ps.hlsl's mean (a weighted mean that favours the farthest of the four), to show why the AO's coarser levels cannot be the Hi-Z.
float AoMeanOf(const float d[4], float range) {
  const float far_d = std::max(std::max(d[0], d[1]), std::max(d[2], d[3]));
  float sum = 0, weights = 0;
  for (int k = 0; k < 4; ++k) {
    const float w = std::clamp(1.0f - (far_d - d[k]) / range, 0.0f, 1.0f);
    sum += w * d[k];
    weights += w;
  }
  return std::min(sum / std::max(weights, 1e-6f), sc::kSky);
}

void MeanIsNotAMinChecks() {
  // A thin pole 2 m from the camera in front of a wall at 50 m: the 2x2 block holds three wall texels and one pole texel. The min is the pole, the AO's level is the wall.
  const float block[4] = {50.0f, 50.0f, 50.0f, 2.0f};
  const float range = 0.75f * 1.5f * 0.615f;  // the main pass's radius and falloff, as the mip pass computes it (hlsl/ao_mip_ps.hlsl)
  const float mean = AoMeanOf(block, range);
  const float min = sc::Min4(block[0], block[1], block[2], block[3]);
  Check(min == 2.0f && mean > 40.0f, Num("a pole in front of a wall: the Hi-Z level says %.1f m, the AO's mean level %.1f m (a ray would pass through the pole)", double(min), double(mean)));
  // And the mean can be above every surface a ray could meet when the weights favour the far one: the min never is.
  Lcg rng;
  bool min_always_below = true;
  bool mean_sometimes_above = false;
  for (int i = 0; i < 5000; ++i) {
    float d[4];
    for (float& x : d) x = rng.Range(0.5f, 120.0f);
    const float m = AoMeanOf(d, range);
    const float lo = sc::Min4(d[0], d[1], d[2], d[3]);
    min_always_below = min_always_below && lo <= d[0] && lo <= d[1] && lo <= d[2] && lo <= d[3];
    mean_sometimes_above = mean_sometimes_above || m > lo + 1.0f;
  }
  Check(min_always_below && mean_sometimes_above, "the min is below all four texels, the AO's mean is not");
}

// ---------------------------------------------------------------------------
// the .hlsli compiled as C++
// ---------------------------------------------------------------------------
void ShaderChecks() {
  Check(hlsl_shim::kScreenDepthMarkMask == sc::kDepthMarkMask, "the .hlsli's mark mask is the interface's");
  Lcg rng;
  bool strip_same = true;
  bool min_same = true;
  for (int i = 0; i < 50000; ++i) {
    // any float with a finite value, marks included
    float f;
    do {
      f = FloatOf(rng.Next());
    } while (!std::isfinite(f));
    strip_same = strip_same && BitsOf(hlsl_shim::ScreenHizStrip(f)) == BitsOf(sc::StripMarks(f));
    float v[4];
    for (float& x : v) {
      do {
        x = FloatOf(rng.Next());
      } while (!std::isfinite(x));
    }
    const float a = hlsl_shim::ScreenHizMin4(hlsl_shim::float4(v[0], v[1], v[2], v[3]));
    min_same = min_same && BitsOf(a) == BitsOf(sc::Min4(v[0], v[1], v[2], v[3]));
  }
  Check(strip_same, "ScreenHizStrip equals the twin's StripMarks on random floats");
  Check(min_same, "ScreenHizMin4 equals the twin's Min4 on random floats");
}

// ---------------------------------------------------------------------------
// the sources (given the src/gpu folder)
// ---------------------------------------------------------------------------
// Whether `later` is found in `text` after `earlier` (the order of two calls).
bool Before(const std::string& text, const std::string& earlier, const std::string& later) {
  const size_t a = At(text, earlier);
  const size_t b = a == std::string::npos ? a : At(text, later, a + earlier.size());
  return a != std::string::npos && b != std::string::npos;
}

void SourceChecks(const std::string& gpu) {
  std::string shared_h, shared_cpp, extract_cpp, hiz_ps, hiz_h, view_ps, ao_cpp, ao_h, scene_cpp, scene_h, renderer, cmake, shaders_h, precache, ao_common, manifest, meta, frame_h;
  if (!Slurp(gpu + "/screen_shared.h", &shared_h) || !Slurp(gpu + "/screen_shared.cpp", &shared_cpp) || !Slurp(gpu + "/screen_extract.cpp", &extract_cpp) || !Slurp(gpu + "/hlsl/screen_hiz_ps.hlsl", &hiz_ps) ||
      !Slurp(gpu + "/hlsl/screen_hiz.hlsli", &hiz_h) || !Slurp(gpu + "/hlsl/screen_view_ps.hlsl", &view_ps) || !Slurp(gpu + "/ao.cpp", &ao_cpp) || !Slurp(gpu + "/ao.h", &ao_h) ||
      !Slurp(gpu + "/scene.cpp", &scene_cpp) || !Slurp(gpu + "/scene.h", &scene_h) || !Slurp(gpu + "/renderer.cpp", &renderer) || !Slurp(gpu + "/CMakeLists.txt", &cmake) ||
      !Slurp(gpu + "/host_shaders.h", &shaders_h) || !Slurp(gpu + "/host_precache.h", &precache) || !Slurp(gpu + "/hlsl/ao_common.hlsli", &ao_common) ||
      !Slurp(gpu + "/../settings/generated/manifest.json", &manifest) || !Slurp(gpu + "/../settings/generated/settings_meta.inc", &meta) || !Slurp(gpu + "/screen_frame.h", &frame_h)) {
    Check(false, "the files of src/gpu could not be read (" + gpu + ")");
    return;
  }
  // the frozen header names itself and stays free of Plume and of the renderer
  Check(Has(frame_h, "THE FROZEN INTERFACE") && !Has(frame_h, "plume_render_interface.h") && !Has(frame_h, "host_passes.h"), "screen_frame.h says it is frozen and includes nothing of the renderer");
  Check(Has(shared_h, "#include \"screen_frame.h\"") && Has(shared_h, "#include \"screen_rules.h\""), "screen_shared.h is built on the frozen header and the rules");

  // the constants the shaders and the header share
  const size_t mask_at = At(hiz_h, "kScreenDepthMarkMask = ");
  Check(mask_at != std::string::npos && std::atoi(hiz_h.c_str() + mask_at + 23) == int(sc::kDepthMarkMask), "hlsl/screen_hiz.hlsli's mark mask is the interface's kDepthMarkMask");
  const size_t sky_at = At(ao_common, "kAoSky = ");
  Check(sky_at != std::string::npos && std::atof(ao_common.c_str() + sky_at + 9) == double(sc::kSky), "the interface's kSky is the AO's kAoSky");
  Check(Has(ao_common, "AoSetVehicleBit") && Has(ao_common, "asuint(depth) & ~1u"), "the AO still keeps its vehicle mark in bit 0 (the Hi-Z's mask counts on it)");

  // the push blocks: the structs of the shaders against the C++'s
  const size_t hp = At(hiz_ps, "struct HizPush {");
  const size_t hp_end = hp == std::string::npos ? hp : At(hiz_ps, "};", hp);
  Check(hp != std::string::npos && Count(hiz_ps.substr(hp, hp_end - hp), "uint4") == 2 && Has(shared_h, "static_assert(sizeof(HizPush) == 32)"), "the Hi-Z pass's push is two uint4 (32 bytes) on both sides");
  const size_t vp = At(view_ps, "struct ScreenViewPush {");
  const size_t vp_end = vp == std::string::npos ? vp : At(view_ps, "};", vp);
  Check(vp != std::string::npos && Count(view_ps.substr(vp, vp_end - vp), "uint4") == 3 && Has(shared_h, "static_assert(sizeof(ViewPush) == 48)"), "the debug view's push is three uint4 (48 bytes) on both sides");
  Check(Count(hiz_ps, "g_push.slots.y == 0u") == 1 && Has(shared_cpp, "push.slots[1] = level == 0 ? 0u : 1u;"), "the Hi-Z pass's mode word: 0 for level 0, 1 above it");
  for (int v = 1; v < int(sc::View::kCount); ++v) {
    Check(Has(view_ps, "view == " + std::to_string(v) + "u"), Num("the view shader handles view %.0f", double(v)));
  }
  Check(!Has(view_ps, "view == " + std::to_string(int(sc::View::kCount)) + "u"), "the view shader knows no view the header does not");

  // the registration of the shaders, the source file and the test
  Check(Has(cmake, "larecomp_gpu_host_shader(screen_hiz_ps ps_6_0)") && Has(cmake, "larecomp_gpu_host_shader(screen_view_ps ps_6_0)") && Has(cmake, "    screen_shared.cpp\n"),
        "CMake builds the two shaders and screen_shared.cpp");
  Check(Has(cmake, "add_executable(larecomp_screen_shared_test EXCLUDE_FROM_ALL screen_shared_test.cpp)"), "CMake has this test's target");
  Check(Count(shaders_h, "screen_hiz_ps.hlsl.") == 2 && Count(shaders_h, "screen_view_ps.hlsl.") == 2 && Has(shaders_h, "ScreenHizPixelShader()") && Has(shaders_h, "ScreenViewPixelShader()"),
        "host_shaders.h includes both blobs for both backends and hands them out");
  Check(Has(precache, "kScreenShared = 23"), "host_precache.h has the owner id the Hi-Z's pipeline is keyed by");
  Check(Has(shared_cpp, "HostPrecache::Key(HostPrecache::kScreenShared, 0)") && Has(shared_cpp, "AddLive(\"screen products\""), "the Hi-Z's pipeline is made in the background");

  // the settings
  for (const char* name : {"mcla_gpu_screen_view", "mcla_gpu_screen_view_level"}) {
    const setting_source::Info setting = setting_source::Find(gpu, shared_cpp, name);
    Check(setting.found && setting_source::DefaultIs(setting, 0.0) && setting.debug_only && setting.live && setting.has_range && setting.range_lo == 0.0,
          std::string("the setting ") + name + " exists: default 0, debug-only, live, from 0");
    if (setting.found) {
      const double hi = std::string(name) == "mcla_gpu_screen_view" ? double(int(sc::View::kCount) - 1) : double(sc::kHizMaxLevels - 1);
      Check(setting.range_hi == hi, Num("... and its range ends at %.0f", hi));
    }
    Check(Has(manifest, std::string("\"") + name + "\"") && Has(meta, std::string("{\"") + name + "\""), std::string("... and the manifest and the meta list have ") + name);
    Check(Has(shared_cpp, std::string("larecomp::settings::") + name + ".Get()"), std::string("screen_shared.cpp reads ") + name);
  }

  // the hooks, and the order of the calls
  Check(Has(ao_cpp, "#include \"screen_shared.h\"") && Has(ao_cpp, "screen_->Publish(list, adopted)") && Has(ao_cpp, "PublishScreen(list, summary, plan, base);"), "the AO publishes its frame at the end of Run");
  Check(Before(ao_cpp, "ran_this_frame_ = true;\n  PublishScreen(", "return true;\n}\n\nvoid AmbientOcclusion::PublishScreen"), "... right after it says the frame ran, as the last step of Run");
  Check(Has(ao_cpp, "screen_->Want(screen::Consumer::kAmbientOcclusion") && Has(ao_cpp, "screen_->Want(screen::Consumer::kContactShadows") && Has(ao_cpp, "screen_->Want(screen::Consumer::kBounceLight"),
        "the AO declares the three consumers' wants");
  Check(Has(ao_cpp, "screen_wants_ = ready_ && screen_->NeedsDepthChain() && EnsureScreenDepthPipelines();") && Has(ao_h, "|| screen_wants_ || world_light_wanted_;"), "the AO runs for the screen products only with its depth pipelines made");
  Check(Has(ao_cpp, "plan.screen = screen_wants_ && !plan.occlusion && !contact;") && Has(ao_cpp, "if (!plan.probe && !plan.screen) {"), "a frame for the screen products is the census's probe frame without the readback");
  Check(Has(ao_cpp, "if (mode_ == Mode::kOff && !contact_working && !screen_wants_) {"), "the AO does not give its images back while the screen products use them");
  Check(Has(scene_cpp, "screen_->BeginFrame(frame_serial_)") && Before(scene_cpp, "screen_->BeginFrame(frame_serial_)", "ao_->BeginFrame();"), "the Scene starts the screen frame before the AO's, which declares its wants after it");
  Check(Before(scene_cpp, "gbuffer_->DrawView(summary, list);", "screen_->DrawView(summary, list);"), "the Scene draws the screen view after the G-buffer's");
  Check(Has(scene_h, "SetScreenShared(ScreenShared* screen)"), "the Scene takes the owner");
  Check(Has(renderer, "screen_.Initialize(device_, heap_, passes_, edram_, pass_timing_)") && Has(renderer, "ao_.SetScreenShared(&screen_)") && Has(renderer, "scene_.SetScreenShared(&screen_)"), "the Renderer makes the owner and gives it to the AO and the Scene");
  Check(Count(renderer, "ao_.Shutdown();\n    screen_.Shutdown();") + Count(renderer, "ao_.Shutdown();\n  screen_.Shutdown();") == 2, "both of the Renderer's shutdowns stop the owner right after the AO");
  Check(Has(renderer, "screen_.HeldBytes()"), "the frame stats line has the owner's memory");
  // nothing is recorded without a want: the Hi-Z only under the plan's make, the view only under its setting
  Check(Has(shared_cpp, "if ((plan_.make & screen::ProductBit(screen::kHizMin)) != 0) {\n    if (hiz_made_ != 0 && ") && Has(shared_cpp, "in.hiz_levels = RecordHiz(list, *depth0, in.hiz);") &&
            Has(extract_cpp, "if (!hiz_fused_ || (plan_.make & screen::ProductBit(screen::kHizMin)) == 0) {\n    return 0;"),
        "the Hi-Z's passes are recorded only while it is wanted (Publish's own, and SS-3's fused ones in the depth step)");
  Check(Has(shared_cpp, "if (plan_.view == screen::View::kOff || !list || !summary.source) {"), "the view draws nothing while it is off");

  // BEHAVIOUR IDENTICAL WITHOUT A WANT, by construction: the only code of the owner that records a command is RecordHiz (under the plan's make) and DrawView (under the view); the
  // frame's publication and the AO's hand-over copy numbers and nothing else. (The run plan's frozen views check it on a GPU.)
  const auto body = [](const std::string& text, const std::string& from, const std::string& to) {
    const size_t a = At(text, from);
    const size_t b = a == std::string::npos ? a : At(text, to, a + from.size());
    return a == std::string::npos || b == std::string::npos ? std::string() : text.substr(a, b - a);
  };
  const std::string publish = body(shared_cpp, "void ScreenShared::Publish(", "const HostImage* ScreenShared::Image(");
  Check(!publish.empty() && !Has(publish, "list->") && !Has(publish, "DrawInto(") && !Has(publish, "Mark(") && !Has(publish, "EnsureImage(") && Count(publish, "RecordHiz(list") == 1,
        "ScreenShared::Publish records nothing itself: its one call that can is RecordHiz, under the plan");
  const std::string begin = body(shared_cpp, "void ScreenShared::BeginFrame(", "void ScreenShared::LogWants()");
  Check(!begin.empty() && !Has(begin, "list") && !Has(begin, "DrawInto(") && !Has(begin, "EnsureImage("), "ScreenShared::BeginFrame has no command list and makes no image");
  const std::string hand_over = body(ao_cpp, "void AmbientOcclusion::PublishScreen(", "bool AmbientOcclusion::RecordApply(");
  Check(!hand_over.empty() && !Has(hand_over, "list->") && !Has(hand_over, "DrawPass(") && !Has(hand_over, "Mark(") && !Has(hand_over, "EnsureImage(") && Count(hand_over, "screen_->Publish(list, adopted)") == 1,
        "the AO's hand-over records nothing itself and makes no image");
  const std::string record_hiz = body(shared_cpp, "uint32_t ScreenShared::RecordHiz(", "void ScreenShared::ReleaseHiz()");
  Check(!record_hiz.empty() && Has(record_hiz, "HizPipeline()") && Has(record_hiz, "if (!pipeline || !list"), "RecordHiz returns before anything is made while the pipeline is not ready");
}


// ---------------------------------------------------------------------------
// SS-1's sources (given the src/gpu folder)
// ---------------------------------------------------------------------------
void SourceChecksSs1(const std::string& gpu) {
  std::string shared_h, shared_cpp, colour_ps, car_ps, view_ps, car_cpp, car_h, ao_cpp, ao_h, scene_cpp, renderer, cmake, shaders_h, history_h, rt_h, rt_cpp;
  if (!Slurp(gpu + "/screen_shared.h", &shared_h) || !Slurp(gpu + "/screen_shared.cpp", &shared_cpp) || !Slurp(gpu + "/hlsl/screen_colour_ps.hlsl", &colour_ps) ||
      !Slurp(gpu + "/hlsl/car_history_ps.hlsl", &car_ps) || !Slurp(gpu + "/hlsl/screen_view_ps.hlsl", &view_ps) || !Slurp(gpu + "/car_selfrefl.cpp", &car_cpp) || !Slurp(gpu + "/car_selfrefl.h", &car_h) ||
      !Slurp(gpu + "/ao.cpp", &ao_cpp) || !Slurp(gpu + "/ao.h", &ao_h) || !Slurp(gpu + "/scene.cpp", &scene_cpp) || !Slurp(gpu + "/renderer.cpp", &renderer) ||
      !Slurp(gpu + "/CMakeLists.txt", &cmake) || !Slurp(gpu + "/host_shaders.h", &shaders_h) || !Slurp(gpu + "/screen_history.h", &history_h) || !Slurp(gpu + "/car_rt_refl.h", &rt_h) ||
      !Slurp(gpu + "/car_rt_refl.cpp", &rt_cpp)) {
    Check(false, "SS-1's files of src/gpu could not be read (" + gpu + ")");
    return;
  }
  const auto body = [](const std::string& text, const std::string& from, const std::string& to) {
    const size_t a = At(text, from);
    const size_t b = a == std::string::npos ? a : At(text, to, a + from.size());
    return a == std::string::npos || b == std::string::npos ? std::string() : text.substr(a, b - a);
  };

  // the shaders' registration
  Check(Has(cmake, "larecomp_gpu_host_shader(screen_colour_ps ps_6_0 DEFINES SCREEN_COLOUR_PASS=0 SCREEN_COLOUR_MS=0)") &&
            Has(cmake, "larecomp_gpu_host_shader(screen_colour_ms_ps ps_6_0 SOURCE screen_colour_ps DEFINES SCREEN_COLOUR_PASS=0 SCREEN_COLOUR_MS=1)") &&
            Has(cmake, "larecomp_gpu_host_shader(screen_colour_mip_ps ps_6_0 SOURCE screen_colour_ps DEFINES SCREEN_COLOUR_PASS=1)") &&
            Has(cmake, "larecomp_gpu_host_shader(car_history_ms_shared_ps ps_6_0 SOURCE car_history_ps DEFINES CAR_HISTORY_MS=2)"),
        "CMake builds the colour capture's three shaders and the car history's third variant");
  Check(Count(shaders_h, "screen_colour_ps.hlsl.") == 2 && Count(shaders_h, "screen_colour_ms_ps.hlsl.") == 2 && Count(shaders_h, "screen_colour_mip_ps.hlsl.") == 2 && Count(shaders_h, "car_history_ms_shared_ps.hlsl.") == 2 &&
            Has(shaders_h, "ScreenColourPixelShader(uint32_t pass)") && Has(shaders_h, "MCLA_GPU_HOST_SHADER_BLOB(car_history_ms_shared_ps)"),
        "host_shaders.h includes the four new blobs for both backends and hands them out");

  // the push block of the capture, both sides
  const size_t cp = At(colour_ps, "struct ScreenColourPush {");
  const size_t cp_end = cp == std::string::npos ? cp : At(colour_ps, "};", cp);
  Check(cp != std::string::npos && Count(colour_ps.substr(cp, cp_end - cp), "uint4") == 2 && Has(shared_h, "static_assert(sizeof(ColourPush) == 32)"), "the capture's push is two uint4 (32 bytes) on both sides");
  Check(Has(shared_cpp, "push.slots[2] = half ? 2u : 1u;") && Has(colour_ps, "const int ratio = int(max(g_push.slots.z, 1u));"), "the ratio of the source to the image rides in slots.z (1: the picture's size, 2: half)");

  // THE ARITHMETIC: the capture at the picture's size is the car history pass's own colour, operation for operation (the twin above is the C++ of these lines)
  Check(Has(colour_ps, "static const float kMaxColour = 60000.0f;") && Has(car_ps, "static const float kMaxColour = 60000.0f;"), "both clamp to 60000");
  Check(Has(colour_ps, "[loop] for (uint s = 0u; s < samples; ++s) {\n        sum += g_textures_ms[g_push.slots.x].Load(p, s).rgb;\n      }") &&
            Has(car_ps, "[loop] for (uint s = 0u; s < samples; ++s) {\n    sum += g_textures_ms[g_push.slots.y].Load(p, s).rgb;\n  }"),
        "both add the host samples one by one in order");
  Check(Has(colour_ps, "total += sum / float(samples);") && Has(car_ps, "const float3 colour = sum / float(samples);"), "both divide the sum by the sample count as a float");
  const std::string held = "float HeldChannel(float v) {\n  return ((asuint(v) & 0x7FFFFFFFu) <= 0x7F800000u) ? min(max(v, 0.0f), kMaxColour) : 0.0f;\n}\n"
                           "float3 HeldColour(float3 colour) {\n  return float3(HeldChannel(colour.x), HeldChannel(colour.y), HeldChannel(colour.z));\n}\n";
  Check(Has(colour_ps, "const float3 colour = total / float(ratio * ratio);") && Has(colour_ps, "return float4(HeldColour(colour), 1.0f);") &&
            Has(car_ps, "return float4(HeldColour(colour), LinearDistance(stored));") && Has(colour_ps, held) && Has(car_ps, held),
        "both clamp with the same HeldColour (a NaN black by the bits, else min(max(c, 0), kMaxColour)), the capture after dividing by 1 at the picture's size");
  Check(Has(colour_ps, "const uint samples = max(g_push.slots.y, 1u);") && Has(car_ps, "const uint samples = max(g_push.slots.w, 1u);"), "both take at least one sample");
  // the car history's variants: 0 and 1 keep their text, 2 reads one load
  Check(Has(car_ps, "#if CAR_HISTORY_MS == 2") && Has(car_ps, "const float3 colour = g_textures[g_push.slots.y].Load(int3(p, 0)).rgb;\n#elif CAR_HISTORY_MS") && Count(car_ps, "#if CAR_HISTORY_MS\n") == 1,
        "the car history shader's third variant reads the colour from one single sample image, the first two keep their code");
  Check(Has(car_ps, "id = StencilOf(g_utextures_ms[g_push.slots.z].Load(p, g_push.slots2.x)) >> 2;"), "... and the stencil and depth planes are still the multisampled ones in it");

  // the view shader knows the two new views
  Check(Has(view_ps, "view == 6u || view == 7u") && Has(view_ps, "g_textures[g_push.slots.y].Load(int3(clamp(int2(uv * float2(size)), int2(0, 0), size - 1), 0)).rgb"), "the view shader shows colour_post and the history");

  // THE OWNER: the order of the Scene's calls
  Check(Before(scene_cpp, "screen_->OnAnchor(banks);", "ao_->OnAnchor(banks, anchor_cut_);") && Before(scene_cpp, "screen_->OnAnchor(banks);", "car_refl_->OnAnchor(banks);"),
        "the owner reads the anchor's camera before the AO and the car self-reflection ask for it");
  Check(Has(scene_cpp, "if (screen_ && screen_->tracking()) {\n    // The kind of view of the frame") && Has(scene_cpp, "screen_->OnMainDraw(draw.match, d.state.color[0].exp_bias);") && Has(scene_cpp, "if (screen_ && screen_->tracking()) {\n      screen_->OnAnchor(banks);"),
        "the main view's draws and the anchor reach the owner only while it tracks");
  Check(Before(scene_cpp, "car_rt_->OnAfterOcclusion(summary, list, slot_, ao_);", "screen_->CapturePost(record, summary, list);") && Before(scene_cpp, "screen_->CapturePost(record, summary, list);", "car_refl_->OnResolve(record, summary, list);") &&
            Before(scene_cpp, "ao_->OnResolve(record, summary, list, UpscalerRunning());", "screen_->CapturePost(record, summary, list);") && Before(scene_cpp, "car_refl_->OnResolve(record, summary, list);", "screen_->DrawView(summary, list);"),
        "the capture comes after the occlusion's apply and the volumetric passes, before the car history, and the debug view after both");
  Check(Has(scene_cpp, "if (screen_ && screen_->capture_wanted()) {"), "the Scene calls the capture only while one is planned");
  Check(Has(scene_cpp, "screen_->OnComposite();") && Has(scene_cpp, "screen_->OnPresent();") && Before(scene_cpp, "NoteMenuWorld();  // before the effects forget the frame", "screen_->OnPresent();"),
        "the composite and the present reach the owner, the present after the world's classification (which asks the camera history)");
  Check(Has(renderer, "car_refl_.SetScreenShared(&screen_);"), "the Renderer gives the owner to the car self-reflection");

  // WHAT MOVED OUT OF THE CAR SELF-REFLECTION: the camera's reading and the previous frame's bookkeeping, and the trigger's rule
  Check(!Has(car_cpp, "ExtractCamera") && !Has(car_cpp, "BankFloat") && !Has(car_cpp, "kLensTolerance") && !Has(car_cpp, "Relative(") && !Has(car_cpp, "previous_frame_") && !Has(car_cpp, "have_previous_") &&
            !Has(car_cpp, "lens_used_") && !Has(car_h, "have_previous_") && !Has(car_h, "previous_frame_") && !Has(car_h, "temporal::Camera previous_;"),
        "the car self-reflection no longer reads the anchor's camera, keeps the previous frame's or tests the lens itself");
  Check(Has(car_cpp, "ScreenShared::IsTrigger(record, summary, anchor_seen_, composite_seen_, trigger_seen_)") && Has(car_cpp, "screen_->Cameras().Previous(screen_->frame_count(), pause)") &&
            Has(car_cpp, "screen_->Cameras().Current(shared_frame)") && Has(car_cpp, "screen_->Cameras().LensAgrees(shared_frame)") && Has(car_cpp, "screen_->Cameras().LastPauseCamera()"),
        "it asks the owner for the trigger's rule, the previous camera, this frame's, whether the lens agrees, and the world's pause camera");
  Check(Has(car_cpp, "screen_->WantCamera(screen::Consumer::kCarHistory, mode != Mode::kOff);") && Has(car_cpp, "screen_->Want(screen::Consumer::kCarHistory, mode != Mode::kOff && source_ == 2 ? screen::ProductBit(screen::kColourPost) : 0u);"),
        "it wants the camera while it is on, and colour_post only in the source setting 2");
  // the frame's numbers are the owner's count (every consumer counts the same presents)
  const std::string on_anchor = body(car_cpp, "void CarSelfRefl::OnAnchor(", "// ---------------------------------------------------------------------------\n// the history: the game's mid-scene depth copy");
  Check(!on_anchor.empty() && Has(on_anchor, "if (play_view_ == pause_view_) {\n    history_valid_ = false;\n    return;") && Has(on_anchor, "if (!camera) {\n    history_valid_ = false;\n    return;") &&
            Has(on_anchor, "history_valid_ = lens_agrees;") && Has(on_anchor, "history_camera_ = *camera;") && Has(on_anchor, "history_valid_ = false;  // none made this frame"),
        "the anchor's rules are the old ones: no kind or no camera: no history; the history is valid when this frame's lens agrees; none made this frame: not valid");

  // THE SOURCE: the colour is the capture's only at the picture's size and only with the setting's allowance; else the pass is the old one
  Check(Has(car_cpp, "const HostImage* shared = SharedColour(render_w, render_h);") && Has(car_cpp, "Pipeline(ms ? (shared ? kPassMsShared : kPassMs) : kPassPlain)") && Has(car_cpp, "push.slots[1] = shared ? shared->slot : colour_slot;"),
        "the history pass is the old pass with the colour slot unchanged unless the frame has a capture of the picture's size");
  const std::string shared_colour = body(car_cpp, "const HostImage* CarSelfRefl::SharedColour(", "// ---------------------------------------------------------------------------\n// the draws of the paint");
  Check(!shared_colour.empty() && Has(shared_colour, "source_ == 1 || !screen_ || !screen_->Frame().Has(screen::kColourPost)") && Has(shared_colour, "image->width != render_width || image->height != render_height"),
        "SharedColour: not with the source setting 1, not without a capture in the frame, not at another size");
  Check(Has(car_h, "enum Pass : uint32_t { kPassPlain, kPassMs, kPassMsShared, kPassCount };") && Has(car_h, "void SetScreenShared(ScreenShared* screen)") && Has(car_cpp, "PassName(pass)"),
        "the car self-reflection has the third pass and the owner's setter");
  const setting_source::Info source = setting_source::Find(gpu, car_cpp, "mcla_gpu_car_selfrefl_source");
  Check(source.found && setting_source::DefaultIs(source, 0.0) && source.debug_only && source.live && source.has_range && source.range_lo == 0.0 && source.range_hi == 2.0,
        "the setting mcla_gpu_car_selfrefl_source exists: default 0, debug-only, live, 0 to 2");
  Check(Has(car_cpp, "larecomp::settings::mcla_gpu_car_selfrefl_source.Get()"), "the car self-reflection reads it");
  // CarRtRefl: untouched by this step (rt-hit-shading changes its pass and push; the history info it is given carries the lens from the owner's camera through CarSelfRefl)
  Check(Has(car_cpp, "info.tan_x = 1.0 / std::max(std::fabs(p.m[0][0]), 1e-9);") && Has(car_cpp, "const temporal::Mat4& p = trigger_camera_.projection;"), "the ray traced pass is handed the lens of the camera history's previous frame, as before");
  Check(!Has(rt_h, "screen_shared") && !Has(rt_cpp, "screen_shared") && !Has(rt_h, "ScreenShared"), "car_rt_refl.* did not change for SS-1");

  // WHAT MOVED OUT OF THE AO: the colour chain's images, and the call that records it
  Check(!Has(ao_cpp, "colour_[") && !Has(ao_h, "HostImage colour_[3]") && Has(ao_h, "HostImage& Colour(uint32_t level) const;"), "the AO keeps no colour chain images of its own");
  Check(Has(ao_cpp, "screen_->CapturePre([&] { RecordColourChain(list, plan, base, *gi_colour, gi_colour_slot); });") && Before(ao_cpp, "screen_->CapturePre(", "CarRtAo::Result rt;"),
        "the AO records the colour chain inside the owner's CapturePre, at the place it always was (before the ray traced pass)");
  Check(Has(ao_cpp, "screen_ && screen_->EnsureColourPre(colour_sizes, kGiFormat)") && Has(ao_cpp, "screen_->ReleaseColourPre();") && Has(ao_cpp, "return screen_->ColourPre(level);"),
        "the AO's images of the chain are the owner's (made, kept and given back there)");
  const std::string ensure_pre = body(shared_cpp, "bool ScreenShared::EnsureColourPre(", "void ScreenShared::ReleaseColourPre()");
  Check(!ensure_pre.empty() && Has(ensure_pre, "kPreNames[level]") && Has(shared_cpp, "{\"ssgi colour\", \"ssgi colour 1/2\", \"ssgi colour 1/4\"}"), "the images keep the AO's names");
  Check(!Has(shared_h, "colour_pre[screen::kColourPreLevels] = {}") && Has(shared_h, "void CapturePre(const std::function<void()>& record);"), "the AO no longer hands colour_pre over, it records it through the owner");

  // NOTHING IS RECORDED WITHOUT A WANT, by construction
  const std::string capture = body(shared_cpp, "bool ScreenShared::CapturePost(", "// ---------------------------------------------------------------------------\n// the debug view");
  Check(!capture.empty() && Has(capture, "if (!list || !capture_wanted() || !IsTrigger(record, summary, anchor_seen_, composite_seen_, trigger_seen_)) {\n    return false;\n  }") &&
            Count(capture, "DrawInto(") == 2 && At(capture, "DrawInto(") > At(capture, "if (!ColourPipelinesReady()) {"),
        "CapturePost leaves at once unless a capture is planned at the trigger, and draws only after its pipelines are made");
  Check(Has(shared_h, "bool capture_wanted() const { return !post_done_ && (plan_.make & screen::ProductBit(screen::kColourPost)) != 0; }"), "a capture is planned only by the plan's make");
  Check(Has(capture, "++colour_missing_run_;") && Has(capture, "screen::JudgeColourPartner(false, colour_missing_run_) == screen::ColourPartner::kEarly") && Has(capture, "colour_missing_run_ = 0;") &&
            At(capture, "screen::JudgeColourPartner(") < At(capture, "REXLOG_ERROR(\"{} the main view's colour target"),
        "CapturePost counts the trigger frames whose depth has no colour partner and keeps the error for a colour that is missing too long (or differs)");
  const std::string on_present = body(shared_cpp, "void ScreenShared::OnPresent() {", "bool ScreenShared::IsTrigger(");
  Check(!on_present.empty() && !Has(on_present, "list") && !Has(on_present, "DrawInto(") && Has(on_present, "std::swap(post_[0], history_);") && Has(on_present, "++frame_count_;"),
        "the present records nothing: it trades the capture with its history and counts the frame");
  const std::string on_anchor_owner = body(shared_cpp, "void ScreenShared::OnAnchor(", "void ScreenShared::OnPresent() {");
  Check(!on_anchor_owner.empty() && !Has(on_anchor_owner, "list") && !Has(on_anchor_owner, "DrawInto(") && Has(on_anchor_owner, "cameras_.OnAnchor(frame_count_, pause, &camera);") && Count(on_anchor_owner, "cameras_.OnAnchor(") == 4,
        "the owner's anchor records nothing: it passes the camera, or none, to the history");
  Check(Has(shared_cpp, "in.camera.exposure_bias = screen::ExposureOf(exp_bias_, exposure_known_);") && Has(shared_cpp, "screen::FillPreviousViewProjection(cameras_.Previous(frame_count_, adopted.pause_view), cameras_.Before(frame_count_, adopted.pause_view), &in.camera);"),
        "the published camera has the exposure and, when it is the pair of the history's, the previous view-projection");
  Check(Has(shared_cpp, "if ((plan_.make & screen::ProductBit(screen::kColourPost)) == 0) {\n    Seal();") && Has(shared_cpp, "if (!sealed_ && frame_.valid != 0) {\n    Seal();"), "the frame is sealed once: by the AO's publication when no capture is planned, by the capture, or at the frame's end");
  Check(Has(shared_cpp, "std::function") || Has(shared_h, "std::function"), "the owner's CapturePre takes the AO's recorder");
  // the frame's memory counts the owner's new images
  Check(Has(shared_cpp, "for (const HostImage& image : colour_pre_) {\n    add(image);") && Has(shared_cpp, "for (const HostImage& image : post_) {\n    add(image);") && Has(shared_cpp, "add(history_);"), "HeldBytes counts the colour chain, colour_post and the history");
  // the history frame counts and the trigger's constants
  Check(Has(shared_cpp, "constexpr uint32_t kTriggerDestination = 0x078B0000;") && Has(car_h, "static constexpr uint32_t kTriggerDestination = 0x078B0000;"), "the trigger's destination is the same address in the owner and in the car self-reflection");
  // the history of the lens rule lives in one place
  Check(Has(history_h, "inline constexpr double kLensTolerance = 0.05;") && Has(history_h, "RelativeDifference(a.fov_y, b.fov_y) < kLensTolerance"), "the 5% lens rule is the camera history's");
  // the view settings
  const setting_source::Info view = setting_source::Find(gpu, shared_cpp, "mcla_gpu_screen_view");
  Check(view.found && view.has_range && view.range_hi == 7.0, "the view setting goes to 7 (colour_post, the history)");
}

}  // namespace

int main(int argc, char** argv) {
  std::string gpu;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--print") {
      g_print = true;
    } else {
      gpu = arg;
    }
  }
  FrozenChecks();
  WantChecks();
  SizeChecks();
  PlanChecks();
  FrameChecks();
  GenerationChecks();
  ColourChecks();
  CameraHistoryChecks();
  CaptureArithmeticChecks();
  MarkChecks();
  ChainChecks();
  MeanIsNotAMinChecks();
  ShaderChecks();
  if (!gpu.empty()) {
    SourceChecks(gpu);
    SourceChecksSs1(gpu);
  } else {
    std::printf("(no src/gpu folder given: the source checks were left out)\n");
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
