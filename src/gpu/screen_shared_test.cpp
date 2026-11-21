// Check of the shared screen products' frozen interface (screen_frame.h), the host's pure rules (screen_rules.h) and the Hi-Z's shader functions (hlsl/screen_hiz.hlsli). A program
// of its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
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

#include <algorithm>
#include <array>
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
            int(sc::Consumer::kVolumetrics) == 8 && int(sc::Consumer::kLocalLights) == 9 && int(sc::Consumer::kDebugView) == 10 && int(sc::Consumer::kCount) == 11,
        "the consumers keep their numbers");
  Check(int(sc::View::kOff) == 0 && int(sc::View::kLinDepth) == 1 && int(sc::View::kHizMin) == 2 && int(sc::View::kHizCheck) == 3 && int(sc::View::kNormal) == 4 &&
            int(sc::View::kVehicle) == 5 && int(sc::View::kCount) == 6,
        "the debug views keep their numbers");
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
  Check((plan.make & ProductBit(sc::kHizMin)) != 0 && plan.make == sc::kMadeHere && plan.NeedsDepthChain(), "a reflections consumer's want makes the Hi-Z and asks for the depth chain");
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
  Check(sc::PlanFrame(none, sc::View::kHizMin, 0).make == sc::kMadeHere && sc::PlanFrame(none, sc::View::kHizCheck, 0).make == sc::kMadeHere && sc::PlanFrame(none, sc::View::kLinDepth, 0).make == 0,
        "the Hi-Z views make the Hi-Z, the depth views do not");
  Check(sc::PlanFrame(none, sc::View::kNormal, 0).wanted == ProductBit(sc::kNormal) && !sc::PlanFrame(none, sc::View::kNormal, 0).NeedsDepthChain(),
        "the normal view wants the normal and cannot start a frame (it shows black where the frame has none)");
  Check(sc::PlanFrame(none, sc::View::kHizMin, 0).NeedsDepthChain() && sc::PlanFrame(none, sc::View::kVehicle, 0).NeedsDepthChain(), "the Hi-Z and the vehicle views run the depth chain");
  // The settings' conversion.
  Check(sc::ViewFromSetting(0) == sc::View::kOff && sc::ViewFromSetting(1) == sc::View::kLinDepth && sc::ViewFromSetting(5) == sc::View::kVehicle && sc::ViewFromSetting(6) == sc::View::kOff &&
            sc::ViewFromSetting(-1) == sc::View::kOff,
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
  Check(!f.Has(sc::kColourPost) && !f.Has(sc::kHistory) && f.history_colour_slot == sc::kNoSlot, "colour_post and the history are not in this step's frames");
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
  std::string shared_h, shared_cpp, hiz_ps, hiz_h, view_ps, ao_cpp, ao_h, scene_cpp, scene_h, renderer, cmake, shaders_h, precache, ao_common, manifest, meta, frame_h;
  if (!Slurp(gpu + "/screen_shared.h", &shared_h) || !Slurp(gpu + "/screen_shared.cpp", &shared_cpp) || !Slurp(gpu + "/hlsl/screen_hiz_ps.hlsl", &hiz_ps) ||
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
  Check(Has(ao_cpp, "screen_wants_ = ready_ && screen_->NeedsDepthChain() && EnsureScreenDepthPipelines();") && Has(ao_h, "|| screen_wants_;"), "the AO runs for the screen products only with its depth pipelines made");
  Check(Has(ao_cpp, "plan.screen = screen_wants_ && !plan.occlusion && !contact;") && Has(ao_cpp, "if (!plan.probe && !plan.screen) {"), "a frame for the screen products is the census's probe frame without the readback");
  Check(Has(ao_cpp, "if (mode_ == Mode::kOff && !contact_working && !screen_wants_) {"), "the AO does not give its images back while the screen products use them");
  Check(Has(scene_cpp, "screen_->BeginFrame(frame_serial_)") && Before(scene_cpp, "screen_->BeginFrame(frame_serial_)", "ao_->BeginFrame();"), "the Scene starts the screen frame before the AO's, which declares its wants after it");
  Check(Before(scene_cpp, "gbuffer_->DrawView(summary, list);", "screen_->DrawView(summary, list);"), "the Scene draws the screen view after the G-buffer's");
  Check(Has(scene_h, "SetScreenShared(ScreenShared* screen)"), "the Scene takes the owner");
  Check(Has(renderer, "screen_.Initialize(passes_, edram_, pass_timing_)") && Has(renderer, "ao_.SetScreenShared(&screen_)") && Has(renderer, "scene_.SetScreenShared(&screen_)"), "the Renderer makes the owner and gives it to the AO and the Scene");
  Check(Count(renderer, "ao_.Shutdown();\n    screen_.Shutdown();") + Count(renderer, "ao_.Shutdown();\n  screen_.Shutdown();") == 2, "both of the Renderer's shutdowns stop the owner right after the AO");
  Check(Has(renderer, "screen_.HeldBytes()"), "the frame stats line has the owner's memory");
  // nothing is recorded without a want: the Hi-Z only under the plan's make, the view only under its setting
  Check(Has(shared_cpp, "if ((plan_.make & screen::ProductBit(screen::kHizMin)) != 0) {\n    in.hiz_levels = RecordHiz"), "the Hi-Z's passes are recorded only while it is wanted");
  Check(Has(shared_cpp, "if (plan_.view == screen::View::kOff || !list || !summary.source || !published_) {"), "the view draws nothing while it is off");

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
  MarkChecks();
  ChainChecks();
  MeanIsNotAMinChecks();
  ShaderChecks();
  if (!gpu.empty()) {
    SourceChecks(gpu);
  } else {
    std::printf("(no src/gpu folder given: the source checks were left out)\n");
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
