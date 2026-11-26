// Check of the shared screen products' EXTRACTION (screen_extract.h, SS-2): the depth chain, the normal image and the bounce light's colour chain are recorded by ScreenShared from the AO's frame
// block, and the pictures must be the old ones, to the bit. A program of its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_screen_extract_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_screen_extract_test [<src/gpu dir>] [--print]
//
// WHAT IT PROVES. The passes are the same shaders (the test reads the registration), into images of the same sizes and formats, recorded in the same order with the same labels, so what could
// differ is the 128 byte push block each pass is given. Below, the OLD recording is run: the statements of ao.cpp's old path (mcla_gpu_screen_extract false) are copied here as macros (comments
// out), compiled and EXECUTED against a mock of what they touch (the frame's plan, the images' slots, DrawPass, the pass timing), and the same statements are required to stand in ao.cpp's text,
// whitespace and comments aside (so the copy cannot drift from the code that the A/B runs); the pushes and the order of the draws they produce are compared byte for byte with what screen_extract.h's
// functions make from the same frame, over a grid of frames: every size the renderer runs at (720p to 4K and odd ones), the half size rule's three answers, the contact shadows at full size beside a
// half size trace, one sample or all of the colour target's samples, the normal plane on and off, a random frame block (every field of the 128 bytes random, so that a field the new code leaves
// alone and the old code changed, or the other way round, shows). Then the sources: screen_extract.cpp records the same passes in the same order with the same labels, the pass numbers are the
// registration's, ao.cpp calls the owner at the places it recorded these passes from, and the owner's push block is the AO's layout. Last, the pass list and the bandwidth model, printed for
// 1080p and 4K (--print): what a frame records with the products built once against each effect making its own.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "ao_size.h"
#include "screen_extract.h"
#include "ssgi_light.h"

namespace ex = mcla::gpu::screen::extract;
using mcla::gpu::AoPush;
namespace ssgi_light = mcla::gpu::ssgi_light;

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

bool Slurp(const std::string& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

struct Rng {
  uint64_t s = 0x9E3779B97F4A7C15ull;
  uint32_t Next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return uint32_t(s >> 11);
  }
};

// every byte random, so that no field of the block is accidentally equal
AoPush RandomBlock(Rng& rng) {
  AoPush push;
  uint32_t* words = reinterpret_cast<uint32_t*>(&push);
  for (size_t i = 0; i < sizeof(AoPush) / 4; ++i) {
    words[i] = rng.Next() * 2654435761u;
  }
  return push;
}

bool SameBytes(const AoPush& a, const AoPush& b) { return std::memcmp(&a, &b, sizeof(AoPush)) == 0; }

// The text of the old recording as it stands in ao.cpp, comments and whitespace aside.
std::string Squash(const std::string& text) {
  std::string out;
  size_t i = 0;
  while (i < text.size()) {
    if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/') {
      while (i < text.size() && text[i] != '\n') ++i;
      continue;
    }
    if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == '"') {  // the stringified macro escapes its quotes
      out += '"';
      i += 2;
      continue;
    }
    if (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r') {
      ++i;
      continue;
    }
    out += text[i++];
  }
  return out;
}

// ---------------------------------------------------------------------------
// the old recording, executed: the mock of what the statements touch
// ---------------------------------------------------------------------------

struct Image {
  uint32_t slot = 0;
  bool texture = true;
};
struct Plan {
  uint32_t ao[2] = {};
  uint32_t level[2][2] = {};
  uint32_t render[2] = {};
  bool split = false;
  bool one_sample = false;
  struct Gi {
    float cap = 2.0f;
    uint32_t cabin_id = 0;
  } gi;
  bool SplitContact() const { return split; }
  bool OneSampleColour() const { return one_sample; }
};
struct NormalPlan {
  uint32_t plane_slot = 0;
  uint32_t plane_samples = 1;
  uint32_t plane_sample0 = 0;
  uint32_t plane_size[2] = {};
  bool depth_source_bits = false;
  struct Rows {
    float x[4] = {}, y[4] = {}, z[4] = {};
  } rows;
};
struct Summary {
  uint32_t stencil_slot = 0;
};
struct Target {
  uint32_t host_samples = 1;
};
struct PassTiming {
  std::vector<std::string>* log = nullptr;
  void Mark(void*, const char* label) { log->push_back(std::string("mark ") + label); }
};
constexpr uint32_t kColourOneSampleFlag = 1u << 21;  // ao.cpp's own constant

// What a recording did: marks and draws in order, a draw = the image it wrote and the block it pushed.
struct Draw {
  int image;  // 0 depth0, 1 depth1, 2 depth2, 3 depth_full, 4 normal, 5 normal_full, 6 colour0, 7 colour1, 8 colour2
  AoPush push;
};
struct Trace {
  std::vector<std::string> marks;  // "mark label" and "draw <image>" interleaved, the order of the calls
  std::vector<Draw> draws;
};

struct Env {
  Plan plan;
  NormalPlan normals;
  Summary summary;
  Target colour;
  AoPush base;
  uint32_t colour_slot = 0;
  bool levels = false;
  Image img[9];
  Trace trace;
  PassTiming pass_timing_storage;
  PassTiming* pass_timing_ = &pass_timing_storage;
  void* list = nullptr;
  int depth_pipeline = 1, mip_pipeline = 2, pipeline = 3;
  Env() { pass_timing_storage.log = &trace.marks; }
  Image& Depth(int level) { return img[level]; }
  Image& DepthFull() { return img[3]; }
  Image& NormalImage() { return img[4]; }
  Image& NormalFullImage() { return img[5]; }
  Image& Colour(int level) { return img[6 + level]; }
  int Pipeline(int variant) { return variant; }
  void DrawPass(void*, Image& image, int, const AoPush& push) {
    const int which = int(&image - img);
    trace.marks.push_back("draw " + std::to_string(which));
    trace.draws.push_back({which, push});
  }
};

// ---- the old depth pass's statements, as ao.cpp's Run has them in its old path (comments out) ----
#define LEGACY_DEPTH                                                                          \
  pass_timing_->Mark(list, "ao depth");                                                      \
  push = depth_base;                                                      \
  push.sizes[0] = float(plan.ao[0]);                                                         \
  push.sizes[1] = float(plan.ao[1]);                                                         \
  push.sizes[2] = float(plan.render[0]);                                                     \
  push.sizes[3] = float(plan.render[1]);                                                     \
  push.slots[2] = summary.stencil_slot;                                                      \
  push.slots2[3] = normals_.depth_source_bits ? 1u : 0u;                                     \
  DrawPass(list, Depth(0), depth_pipeline, push);                                            \
  if (levels) {                                                                              \
    push.slots[2] = Depth(0).slot;                                                           \
    push.sizes[0] = float(plan.level[0][0]);                                                 \
    push.sizes[1] = float(plan.level[0][1]);                                                 \
    push.sizes[2] = float(plan.ao[0]);                                                       \
    push.sizes[3] = float(plan.ao[1]);                                                       \
    DrawPass(list, Depth(1), mip_pipeline, push);                                            \
    push.slots[2] = Depth(1).slot;                                                           \
    push.sizes[0] = float(plan.level[1][0]);                                                 \
    push.sizes[1] = float(plan.level[1][1]);                                                 \
    push.sizes[2] = float(plan.level[0][0]);                                                 \
    push.sizes[3] = float(plan.level[0][1]);                                                 \
    DrawPass(list, Depth(2), mip_pipeline, push);                                            \
  }                                                                                          \
  if (plan.SplitContact()) {                                                                 \
    pass_timing_->Mark(list, "ao contact depth");                                            \
    push = base;                                                                             \
    push.slots2[2] &= ~(1u << 16);                                                           \
    push.slots[2] = summary.stencil_slot;                                                    \
    push.sizes[0] = push.sizes[2] = float(plan.render[0]);                                   \
    push.sizes[1] = push.sizes[3] = float(plan.render[1]);                                   \
    DrawPass(list, DepthFull(), depth_pipeline, push);                                       \
  }

// ---- the old normal pass: the body of AmbientOcclusion::RecordNormals after the pipeline lookup ----
#define LEGACY_NORMALS                                                                        \
  pass_timing_->Mark(list, "ao normals");                                                    \
  Push push = base;                                                                          \
  push.slots[0] = normals.plane_slot;                                                        \
  push.slots[1] = normals.plane_sample0;                                                     \
  push.slots[2] = Depth(0).slot;                                                             \
  push.sizes[0] = float(plan.ao[0]);                                                         \
  push.sizes[1] = float(plan.ao[1]);                                                         \
  push.sizes[2] = float(normals.plane_size[0]);                                              \
  push.sizes[3] = float(normals.plane_size[1]);                                              \
  for (int k = 0; k < 4; ++k) {                                                              \
    push.lin[k] = normals.rows.x[k];                                                         \
    push.shape[k] = normals.rows.y[k];                                                       \
    push.light[k] = normals.rows.z[k];                                                       \
  }                                                                                          \
  DrawPass(list, NormalImage(), pipeline, push);                                             \
  if (plan.SplitContact() && NormalFullImage().texture) {                                    \
    pass_timing_->Mark(list, "ao contact normals");                                          \
    push.slots[2] = DepthFull().slot;                                                        \
    push.sizes[0] = float(plan.render[0]);                                                   \
    push.sizes[1] = float(plan.render[1]);                                                   \
    push.slots2[2] &= ~(1u << 16);                                                           \
    DrawPass(list, NormalFullImage(), pipeline, push);                                       \
  }

namespace interior_camera = mcla::gpu::interior_camera;

// ---- the old colour chain: the body of AmbientOcclusion::RecordColourChain after the transition ----
#define LEGACY_COLOUR                                                                         \
  pass_timing_->Mark(list, "ssgi colour chain");                                             \
  const bool ms = colour.host_samples > 1;                                                   \
  Push push = base;                                                                          \
  push.slots2[2] &= ~0xFFu;                                                                  \
  push.slots[0] = colour_slot;                                                               \
  push.slots[1] = colour.host_samples;                                                       \
  push.sizes[0] = float(plan.ao[0]);                                                         \
  push.sizes[1] = float(plan.ao[1]);                                                         \
  push.sizes[2] = float(plan.render[0]);                                                     \
  push.sizes[3] = float(plan.render[1]);                                                     \
  push.shape[0] = ssgi_light::RadianceCap(plan.gi.cap, base.light[3], base.extra[0]);        \
  push.shape[1] = ssgi_light::SourceStart(base.light[3], base.extra[0]);                     \
  if (ms && plan.OneSampleColour()) {                                                        \
    push.slots2[2] |= kColourOneSampleFlag;                                                  \
    push.slots2[0] = base.slots[1];                                                          \
  }                                                                                          \
  if (plan.gi.cabin_id != 0u) {                                                               \
    push.slots2[2] |= interior_camera::kSsgiMatchColour;                                      \
    push.slots2[0] = base.slots[1];                                                          \
    push.slots2[1] = Depth(0).slot;                                                          \
  }                                                                                          \
  DrawPass(list, Colour(0), Pipeline(ms ? kVColorMs : kVColor), push);                       \
  plume_RenderPipeline_mip = Pipeline(kVColorMip);                                           \
  push = base;                                                                               \
  push.slots2[2] &= ~0xFFu;                                                                  \
  push.slots[2] = Colour(0).slot;                                                            \
  push.sizes[0] = float(plan.level[0][0]);                                                   \
  push.sizes[1] = float(plan.level[0][1]);                                                   \
  push.sizes[2] = float(plan.ao[0]);                                                         \
  push.sizes[3] = float(plan.ao[1]);                                                         \
  DrawPass(list, Colour(1), mip, push);                                                      \
  push.slots[2] = Colour(1).slot;                                                            \
  push.sizes[0] = float(plan.level[1][0]);                                                   \
  push.sizes[1] = float(plan.level[1][1]);                                                   \
  push.sizes[2] = float(plan.level[0][0]);                                                   \
  push.sizes[3] = float(plan.level[0][1]);                                                   \
  DrawPass(list, Colour(2), mip, push);

#define STR_(...) #__VA_ARGS__
#define STR(...) STR_(__VA_ARGS__)

constexpr int kVColor = 11, kVColorMs = 12, kVColorMip = 13;

Trace RunLegacyDepth(Env& e) {
  e.trace = Trace{};
  auto& plan = e.plan;
  auto& summary = e.summary;
  auto& base = e.base;
  auto& list = e.list;
  auto& pass_timing_ = e.pass_timing_;
  auto& normals_ = e.normals;
  const bool levels = e.levels;
  const int depth_pipeline = e.depth_pipeline, mip_pipeline = e.mip_pipeline;
  using Push = AoPush;
  const Push depth_base = base;
  Push push{};
#define Depth(i) e.Depth(i)
#define DepthFull() e.DepthFull()
#define DrawPass(l, image, pipe, p) e.DrawPass(l, image, pipe, p)
  LEGACY_DEPTH
#undef Depth
#undef DepthFull
#undef DrawPass
  return e.trace;
}

Trace RunLegacyNormals(Env& e) {
  e.trace = Trace{};
  auto& plan = e.plan;
  auto& base = e.base;
  auto& list = e.list;
  auto& pass_timing_ = e.pass_timing_;
  auto& normals = e.normals;
  const int pipeline = e.pipeline;
  using Push = AoPush;
#define Depth(i) e.Depth(i)
#define DepthFull() e.DepthFull()
#define NormalImage() e.NormalImage()
#define NormalFullImage() e.NormalFullImage()
#define DrawPass(l, image, pipe, p) e.DrawPass(l, image, pipe, p)
  LEGACY_NORMALS
#undef Depth
#undef DepthFull
#undef NormalImage
#undef NormalFullImage
#undef DrawPass
  return e.trace;
}

Trace RunLegacyColour(Env& e) {
  e.trace = Trace{};
  auto& plan = e.plan;
  auto& base = e.base;
  auto& list = e.list;
  auto& pass_timing_ = e.pass_timing_;
  auto& colour = e.colour;
  const uint32_t colour_slot = e.colour_slot;
  using Push = AoPush;
  int plume_RenderPipeline_mip = 0;
  const int mip = 13;
#define Colour(i) e.Colour(i)
#define Depth(i) e.Depth(i)
#define Pipeline(v) e.Pipeline(v)
#define DrawPass(l, image, pipe, p) e.DrawPass(l, image, pipe, p)
  LEGACY_COLOUR
#undef Colour
#undef Depth
#undef Pipeline
#undef DrawPass
  (void)plume_RenderPipeline_mip;
  return e.trace;
}

// ---------------------------------------------------------------------------
// the new recording's pushes, in the order screen_extract.cpp draws them
// ---------------------------------------------------------------------------

ex::Frame FrameOf(const Env& e) {
  ex::Frame f;
  for (int a = 0; a < 2; ++a) {
    f.traced[a] = e.plan.ao[a];
    f.level[0][a] = e.plan.level[0][a];
    f.level[1][a] = e.plan.level[1][a];
    f.render[a] = e.plan.render[a];
  }
  f.levels = e.levels;
  f.full = e.plan.split;
  return f;
}

Trace NewDepth(const Env& e) {
  Trace t;
  const ex::Frame f = FrameOf(e);
  t.marks.push_back("mark ao depth");
  const AoPush level0 = ex::DepthPush(e.base, f, e.summary.stencil_slot, e.normals.depth_source_bits);
  t.draws.push_back({0, level0});
  t.marks.push_back("draw 0");
  if (f.levels) {
    t.draws.push_back({1, ex::DepthMipPush(level0, f, 1, e.img[0].slot)});
    t.marks.push_back("draw 1");
    t.draws.push_back({2, ex::DepthMipPush(level0, f, 2, e.img[1].slot)});
    t.marks.push_back("draw 2");
  }
  if (f.full) {
    t.marks.push_back("mark ao contact depth");
    t.draws.push_back({3, ex::DepthFullPush(e.base, f, e.summary.stencil_slot)});
    t.marks.push_back("draw 3");
  }
  return t;
}

ex::NormalRecipe RecipeOf(const NormalPlan& n) {
  ex::NormalRecipe r;
  r.plane_slot = n.plane_slot;
  r.plane_samples = n.plane_samples;
  r.plane_sample0 = n.plane_sample0;
  r.plane_size[0] = n.plane_size[0];
  r.plane_size[1] = n.plane_size[1];
  for (int k = 0; k < 4; ++k) {
    r.rows_x[k] = n.rows.x[k];
    r.rows_y[k] = n.rows.y[k];
    r.rows_z[k] = n.rows.z[k];
  }
  return r;
}

Trace NewNormals(const Env& e) {
  Trace t;
  const ex::Frame f = FrameOf(e);
  t.marks.push_back("mark ao normals");
  const AoPush traced = ex::NormalPush(e.base, f, RecipeOf(e.normals), e.img[0].slot);
  t.draws.push_back({4, traced});
  t.marks.push_back("draw 4");
  if (f.full && e.img[5].texture) {
    t.marks.push_back("mark ao contact normals");
    t.draws.push_back({5, ex::NormalFullPush(traced, f, e.img[3].slot)});
    t.marks.push_back("draw 5");
  }
  return t;
}

ex::ColourRecipe ColourRecipeOf(const Env& e) {
  // ao.cpp ColourRecipeFor, as written there
  ex::ColourRecipe r;
  r.colour_slot = e.colour_slot;
  r.host_samples = e.colour.host_samples;
  r.cap = ssgi_light::RadianceCap(e.plan.gi.cap, e.base.light[3], e.base.extra[0]);
  r.source_start = ssgi_light::SourceStart(e.base.light[3], e.base.extra[0]);
  r.one_sample = e.colour.host_samples > 1 && e.plan.OneSampleColour();
  r.sample = e.base.slots[1];
  r.match_depth = e.plan.gi.cabin_id != 0u;
  r.matched_depth_slot = e.img[0].slot;
  return r;
}

Trace NewColour(const Env& e) {
  Trace t;
  const ex::Frame f = FrameOf(e);
  t.marks.push_back("mark ssgi colour chain");
  t.draws.push_back({6, ex::ColourLevel0Push(e.base, f, ColourRecipeOf(e))});
  t.marks.push_back("draw 6");
  t.draws.push_back({7, ex::ColourMipPush(e.base, f, 1, e.img[6].slot)});
  t.marks.push_back("draw 7");
  t.draws.push_back({8, ex::ColourMipPush(e.base, f, 2, e.img[7].slot)});
  t.marks.push_back("draw 8");
  return t;
}

bool SameTrace(const Trace& a, const Trace& b, std::string* why) {
  if (a.marks != b.marks) {
    *why = "the marks and draws differ in order or label";
    return false;
  }
  if (a.draws.size() != b.draws.size()) {
    *why = "a different number of draws";
    return false;
  }
  for (size_t i = 0; i < a.draws.size(); ++i) {
    if (a.draws[i].image != b.draws[i].image) {
      *why = "a draw wrote another image";
      return false;
    }
    if (!SameBytes(a.draws[i].push, b.draws[i].push)) {
      const uint32_t* x = reinterpret_cast<const uint32_t*>(&a.draws[i].push);
      const uint32_t* y = reinterpret_cast<const uint32_t*>(&b.draws[i].push);
      for (size_t w = 0; w < 32; ++w) {
        if (x[w] != y[w]) {
          *why = "draw " + std::to_string(i) + " pushes another block: word " + std::to_string(w) + " old " + std::to_string(x[w]) + " new " + std::to_string(y[w]);
          break;
        }
      }
      return false;
    }
  }
  return true;
}

// The frames: every size the renderer runs at, the three answers of the half size rule, the contact shadows' split, the colour target's samples.
struct Case {
  uint32_t render[2];
  bool occlusion_ultra, contact, contact_ultra, bounce;
};

void IdentityChecks() {
  const uint32_t sizes[][2] = {{1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {1921, 1081}, {2560, 1440}, {3440, 1440}, {3840, 2160}, {5120, 2880}, {1920, 1104}, {640, 360}};
  Rng rng;
  int frames = 0, bad_depth = 0, bad_normals = 0, bad_colour = 0;
  std::string why_depth, why_normals, why_colour;
  int half_frames = 0, split_frames = 0, one_sample_frames = 0, ms_frames = 0, normal_full_frames = 0;
  for (const auto& size : sizes) {
    for (int mask = 0; mask < 32; ++mask) {
      mcla::gpu::ao_size::Inputs in;
      in.render_height = size[1];
      in.occlusion = true;
      in.occlusion_ultra = (mask & 1) != 0;
      in.contact = (mask & 2) != 0;
      in.contact_ultra = (mask & 4) != 0;
      in.bounce = (mask & 8) != 0;
      in.bounce_ultra = false;
      in.half_above_ao = (mask & 16) == 0;
      in.half_above_bounce = true;
      const mcla::gpu::ao_size::Choice choice = mcla::gpu::ao_size::Decide(in);
      const uint32_t render[2] = {size[0], size[1]};
      const mcla::gpu::ao_size::Sizes sz = mcla::gpu::ao_size::SizesOf(render, choice);
      for (int variant = 0; variant < 8; ++variant) {
        Env e;
        for (int a = 0; a < 2; ++a) {
          e.plan.ao[a] = sz.ao[a];
          e.plan.level[0][a] = sz.level0[a];
          e.plan.level[1][a] = sz.level1[a];
          e.plan.render[a] = render[a];
        }
        e.plan.split = in.contact && choice.contact_full;
        e.plan.one_sample = in.bounce && choice.by_height && in.half_above_bounce;
        e.plan.gi.cap = 0.5f + float(rng.Next() % 100) * 0.05f;
        e.plan.gi.cabin_id = in.bounce && (variant & 2) != 0 ? 9u : 0u;
        e.levels = (variant & 1) != 0;
        e.base = RandomBlock(rng);
        e.summary.stencil_slot = rng.Next() & 0x7FFF;
        e.normals.depth_source_bits = (variant & 2) != 0 && choice.low;
        e.normals.plane_slot = rng.Next() & 0x7FFF;
        e.normals.plane_samples = (variant & 4) != 0 ? 4u : 1u;
        e.normals.plane_sample0 = rng.Next() & 3;
        e.normals.plane_size[0] = render[0] + 16 * (rng.Next() & 1);
        e.normals.plane_size[1] = render[1] + 24 * (rng.Next() & 1);
        for (int k = 0; k < 4; ++k) {
          e.normals.rows.x[k] = float(int(rng.Next() % 2001) - 1000) * 0.001f;
          e.normals.rows.y[k] = float(int(rng.Next() % 2001) - 1000) * 0.001f;
          e.normals.rows.z[k] = float(int(rng.Next() % 2001) - 1000) * 0.001f;
        }
        e.colour.host_samples = (variant & 4) != 0 ? 4u : 1u;
        e.colour_slot = rng.Next() & 0x7FFF;
        for (int i = 0; i < 9; ++i) {
          e.img[i].slot = 100 + rng.Next() % 20000;
          e.img[i].texture = true;
        }
        e.img[5].texture = (variant & 2) == 0 || !e.plan.split ? true : (rng.Next() & 1) != 0;
        ++frames;
        half_frames += choice.low ? 1 : 0;
        split_frames += e.plan.split ? 1 : 0;
        one_sample_frames += (e.plan.one_sample && e.colour.host_samples > 1) ? 1 : 0;
        ms_frames += e.colour.host_samples > 1 ? 1 : 0;
        std::string why;
        if (!SameTrace(RunLegacyDepth(e), NewDepth(e), &why)) {
          ++bad_depth;
          why_depth = why;
        }
        {
          Trace old_normals = RunLegacyNormals(e);
          normal_full_frames += (old_normals.draws.size() == 2) ? 1 : 0;
          if (!SameTrace(old_normals, NewNormals(e), &why)) {
            ++bad_normals;
            why_normals = why;
          }
        }
        if (!SameTrace(RunLegacyColour(e), NewColour(e), &why)) {
          ++bad_colour;
          why_colour = why;
        }
      }
    }
  }
  Check(bad_depth == 0, "the depth chain's pushes and draws equal the old recording's in every frame: " + why_depth);
  Check(bad_normals == 0, "the normal passes' pushes and draws equal the old recording's in every frame: " + why_normals);
  Check(bad_colour == 0, "the colour chain's pushes and draws equal the old recording's in every frame: " + why_colour);
  Check(frames == 11 * 32 * 8, "the grid has every frame");
  Check(half_frames > 100 && split_frames > 20 && one_sample_frames > 10 && ms_frames > 100 && normal_full_frames > 20,
        "the grid has half size frames, the contact shadows' split, one-sample colour frames, multisampled colour and full size normals: " + std::to_string(half_frames) + " " + std::to_string(split_frames) + " " +
            std::to_string(one_sample_frames) + " " + std::to_string(ms_frames) + " " + std::to_string(normal_full_frames));
  if (g_print) std::printf("identity grid: %d frames (%d half size, %d with the contact split, %d one-sample colour, %d multisampled colour, %d with full size normals)\n", frames, half_frames, split_frames,
                           one_sample_frames, ms_frames, normal_full_frames);
}

// What the new functions change of the frame block, per pass: only the fields the shaders read and the old code set. (A field neither set would be a field the old code left to the AO's block.)
void FieldChecks() {
  Rng rng;
  Env e;
  for (int a = 0; a < 2; ++a) {
    e.plan.ao[a] = 960 + a * 540;
    e.plan.level[0][a] = 480 + a;
    e.plan.level[1][a] = 240 + a;
    e.plan.render[a] = 1920 + a * 1080;
  }
  e.base = RandomBlock(rng);
  const ex::Frame f = FrameOf(e);
  const auto changed = [&](const AoPush& p) {
    std::string names;
    const uint32_t* x = reinterpret_cast<const uint32_t*>(&e.base);
    const uint32_t* y = reinterpret_cast<const uint32_t*>(&p);
    const char* field[] = {"slots", "slots", "slots", "slots", "slots2", "slots2", "slots2", "slots2", "sizes", "sizes", "sizes", "sizes", "lens", "lens", "lens", "lens",
                           "lin",   "lin",   "lin",   "lin",   "shape",  "shape",  "shape",  "shape",  "light", "light", "light", "light", "extra", "extra", "extra", "extra"};
    std::string last;
    for (int w = 0; w < 32; ++w) {
      if (x[w] != y[w]) {
        if (last != field[w]) names += std::string(names.empty() ? "" : " ") + field[w];
        last = field[w];
      }
    }
    return names;
  };
  const AoPush l0 = ex::DepthPush(e.base, f, 77, true);
  Check(changed(l0) == "slots slots2 sizes", "the level 0 depth pass changes only the slots, the flags and the sizes of the block: " + changed(l0));
  Check(changed(ex::DepthMipPush(l0, f, 1, 5)) == "slots slots2 sizes", "a coarser depth level keeps the block of level 0 apart from the slot and the sizes");
  const ex::NormalRecipe n = RecipeOf(e.normals);
  Check(changed(ex::NormalPush(e.base, f, n, 9)).find("lin") != std::string::npos, "the normal pass puts the camera's rows in lin, shape and light");
  Check(ex::DepthMipPush(l0, f, 1, 5).slots2[3] == l0.slots2[3], "the coarser levels carry the depth pass's slots2.w (the old path kept one block through the three passes)");
}

// ---------------------------------------------------------------------------
// the start's list (SS-PRECACHE): what the owner's pipelines are listed for with the boot precache
// ---------------------------------------------------------------------------

void StartKindsChecks() {
  using ex::Bit;
  using ex::Kind;
  Check(ex::StartKinds(false, false, false) == 0, "nothing on at the start: no chain pipeline is listed");
  Check(ex::StartKinds(true, false, false) == (Bit(Kind::kDepth) | Bit(Kind::kDepthMs)), "the depth pass is listed for both sample counts (the MSAA level changes while the game runs)");
  Check(ex::StartKinds(true, true, false) == (Bit(Kind::kDepth) | Bit(Kind::kDepthMs) | Bit(Kind::kMip)), "the occlusion's start adds the coarser level pass");
  Check(ex::StartKinds(true, true, true) == (Bit(Kind::kDepth) | Bit(Kind::kDepthMs) | Bit(Kind::kMip) | Bit(Kind::kColour) | Bit(Kind::kColourMs) | Bit(Kind::kColourMip)),
        "the bounce light's start adds the colour chain's three");
  Check(ex::StartKinds(false, false, true) == (Bit(Kind::kColour) | Bit(Kind::kColourMs) | Bit(Kind::kColourMip)), "the colour chain is listed on its own flag");
  // what the AO's own table listed before SS-2 is what the owner lists now: six passes, every one a base kind with an AO pass number, none of the normal passes (never in that list) or the fused ones
  // (an addition of SS-3, listed by ChainReady the first frame the Hi-Z is wanted)
  const uint32_t all = ex::StartKinds(true, true, true);
  int count = 0;
  bool base_only = (all & ex::kFusedKinds) == 0 && (all & (Bit(Kind::kNormal) | Bit(Kind::kNormalMs))) == 0;
  for (uint32_t i = 0; i < uint32_t(Kind::kCount); ++i) {
    if ((all & (1u << i)) == 0) continue;
    ++count;
    base_only = base_only && ex::PassOf(Kind(i)) != ex::kNoAoPass && !ex::IsFused(Kind(i));
  }
  Check(count == 6 && base_only, "the start lists the six passes the AO's table listed (depth, depth ms, level, colour three), no normal pass and no fused one");
  // a start list is a superset of what a frame asks for: the kinds of the depth chain (with its levels) and of the colour chain are inside it for the plain and the multisampled sample counts
  bool covered = true;
  for (const bool ms : {false, true}) {
    covered = covered && (ex::KindsFor(true, true, false, false, ms, false) & ~all) == 0 && (ex::KindsFor(false, false, false, true, ms, false) & ~all) == 0;
  }
  Check(covered, "every depth or colour chain a frame can ask for, plain or multisampled, is in the start's list (the first frame finds them made)");
}

// ---------------------------------------------------------------------------
// the sources
// ---------------------------------------------------------------------------

size_t At(const std::string& text, const std::string& needle, size_t from = 0) { return text.find(needle, from); }
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

// The part of `text` from `start` up to `stop` (stop not included), empty when either is missing.
std::string Between(const std::string& text, const std::string& start, const std::string& stop) {
  const size_t a = At(text, start);
  if (a == std::string::npos) return {};
  const size_t b = At(text, stop, a + start.size());
  if (b == std::string::npos) return {};
  return text.substr(a, b - a);
}

void SourceChecks(const std::string& gpu) {
  std::string ao_cpp, ao_h, ext_cpp, ext_h, shared_h, shared_cpp, shaders_h, cmake, manifest;
  if (!Slurp(gpu + "/ao.cpp", &ao_cpp) || !Slurp(gpu + "/ao.h", &ao_h) || !Slurp(gpu + "/screen_extract.cpp", &ext_cpp) || !Slurp(gpu + "/screen_extract.h", &ext_h) ||
      !Slurp(gpu + "/screen_shared.h", &shared_h) || !Slurp(gpu + "/screen_shared.cpp", &shared_cpp) || !Slurp(gpu + "/host_shaders.h", &shaders_h) || !Slurp(gpu + "/CMakeLists.txt", &cmake) ||
      !Slurp(gpu + "/../settings/generated/manifest.json", &manifest)) {
    Check(false, "the sources could not be read from " + gpu);
    return;
  }
  // ---- the old recording's statements are in ao.cpp, as the macros above have them ----
  const std::string depth_region = Between(ao_cpp, "// The old path (mcla_gpu_screen_extract false): this class records the same passes into the same images, with its own pipelines.", "  if (normals_.on) {\n    if (extract) {");
  const std::string normals_region = Between(ao_cpp, "  pass_timing_->Mark(list, \"ao normals\");\n  Push push = base;", "bool AmbientOcclusion::Run(");
  const std::string colour_region = Between(ao_cpp, "  pass_timing_->Mark(list, \"ssgi colour chain\");\n  const bool ms = colour.host_samples > 1;", "// ---------------------------------------------------------------------------\n// the census's depth statistic");
  Check(!depth_region.empty() && !normals_region.empty() && !colour_region.empty(), "the three regions of the old recording are found in ao.cpp");
  const std::string want_depth = Squash(STR(LEGACY_DEPTH));
  const std::string want_normals = Squash(STR(LEGACY_NORMALS));
  const std::string want_colour = Squash(STR(LEGACY_COLOUR));
  // the macros keep one thing that ao.cpp spells another way: the mip pipeline lookup of the colour chain (a stand-in for the mock), and the mock's DrawPass(list, Depth(0), ...) calls are the same text
  Check(Has(Squash(depth_region), want_depth), "ao.cpp's old depth recording is, statement for statement, the one the twin ran");
  Check(Has(Squash(normals_region), want_normals), "ao.cpp's old normal recording is, statement for statement, the one the twin ran");
  {
    std::string want = want_colour;
    const std::string stand_in = Squash("plume_RenderPipeline_mip = Pipeline(kVColorMip);");
    const size_t at = At(want, stand_in);
    Check(at != std::string::npos, "the colour chain's twin has its mip pipeline stand-in");
    if (at != std::string::npos) {
      want.replace(at, stand_in.size(), Squash("plume::RenderPipeline* mip = Pipeline(kVColorMip);"));
    }
    Check(Has(Squash(colour_region), want), "ao.cpp's old colour chain recording is, statement for statement, the one the twin ran");
  }
  // ---- the owner records the same passes in the same order with the same labels ----
  const std::string rec_depth = Between(ext_cpp, "void ScreenShared::RecordDepth(", "void ScreenShared::RecordNormals(");
  const std::string rec_normals = Between(ext_cpp, "void ScreenShared::RecordNormals(", "void ScreenShared::RecordColourPre(");
  const std::string rec_colour = Between(ext_cpp, "void ScreenShared::RecordColourPre(", "}  // namespace mcla::gpu");
  const auto order = [](const std::string& text, const std::vector<std::string>& parts) {
    size_t at = 0;
    for (const std::string& part : parts) {
      const size_t found = text.find(part, at);
      if (found == std::string::npos) return false;
      at = found + part.size();
    }
    return true;
  };
  Check(order(rec_depth, {"Mark(list, \"ao depth\")", "screen::extract::DepthPush(base, frame, stencil_slot, source_bits)", "DrawWide(list, depth_[0], depth, level0)", "if (frame.levels)",
                          "DrawWide(list, depth_[1], mip,", "DepthMipPush(level0, frame, 1, depth_[0].slot)", "DrawWide(list, depth_[2], mip,", "DepthMipPush(level0, frame, 2, depth_[1].slot)",
                          "if (frame.full)", "Mark(list, \"ao contact depth\")", "DrawWide(list, depth_full_, depth,", "DepthFullPush(base, frame, stencil_slot)"}),
        "RecordDepth draws level 0, the coarser levels and the full size depth in the old order with the old labels");
  Check(order(rec_normals, {"Mark(list, \"ao normals\")", "NormalPush(base, frame, recipe, depth_[0].slot)", "DrawWide(list, normal_, pipeline, push)", "if (frame.full && normal_full_.texture)",
                            "Mark(list, \"ao contact normals\")", "DrawWide(list, normal_full_, pipeline,", "NormalFullPush(push, frame, depth_full_.slot)"}),
        "RecordNormals draws the traced and the full size normals in the old order with the old labels");
  Check(order(rec_colour, {"Transition(list, colour, plume::RenderTextureLayout::SHADER_READ)", "Mark(list, \"ssgi colour chain\")", "DrawWide(list, colour_pre_[0], level0,", "ColourLevel0Push(base, frame, recipe)",
                           "DrawWide(list, colour_pre_[1], mip,", "ColourMipPush(base, frame, 1, colour_pre_[0].slot)", "DrawWide(list, colour_pre_[2], mip,", "ColourMipPush(base, frame, 2, colour_pre_[1].slot)",
                           "pre_made_ = true"}),
        "RecordColourPre transitions the colour target, draws level 0 and the two coarser levels in the old order and counts the capture made");
  // ---- the same shaders, formats and the same layout ----
  const auto pass_of = [&](const char* kind) {
    const std::string needle = std::string("case Kind::") + kind + ": return ";
    const size_t at = ext_h.find(needle);
    return at == std::string::npos ? -1 : std::atoi(ext_h.c_str() + at + needle.size());
  };
  Check(pass_of("kDepth") == 0 && pass_of("kDepthMs") == 1 && pass_of("kMip") == 2 && pass_of("kColour") == 11 && pass_of("kColourMs") == 12 && pass_of("kColourMip") == 13 && pass_of("kNormal") == 27 &&
            pass_of("kNormalMs") == 28,
        "the owner's pass numbers are AmbientOcclusionPixelShader's");
  Check(Has(shaders_h, "case 0: return MCLA_GPU_HOST_SHADER_BLOB(ao_depth_ps);") && Has(shaders_h, "case 1: return MCLA_GPU_HOST_SHADER_BLOB(ao_depth_ms_ps);") &&
            Has(shaders_h, "case 2: return MCLA_GPU_HOST_SHADER_BLOB(ao_mip_ps);") && Has(shaders_h, "case 11: return MCLA_GPU_HOST_SHADER_BLOB(ao_color_ps);") &&
            Has(shaders_h, "case 12: return MCLA_GPU_HOST_SHADER_BLOB(ao_color_ms_ps);") && Has(shaders_h, "case 13: return MCLA_GPU_HOST_SHADER_BLOB(ao_color_mip_ps);") &&
            Has(shaders_h, "case 27: return MCLA_GPU_HOST_SHADER_BLOB(ao_normal_ps);") && Has(shaders_h, "case 28: return MCLA_GPU_HOST_SHADER_BLOB(ao_normal_ms_ps);"),
        "the passes' numbers are the shaders the AO's own table makes of them");
  // formats: the owner's against the AO's MakePipeline
  Check(Has(ao_cpp, "case kVDepth: return make(kPassDepth, plume::RenderFormat::R32_FLOAT, copy, 1);") && Has(ao_cpp, "case kVMip: return make(kPassMip, plume::RenderFormat::R16_FLOAT, copy, 1);") &&
            Has(ao_cpp, "case kVNormal: return make(kPassNormal, plume::RenderFormat::R16G16_UNORM, copy, 1);") && Has(ao_cpp, "case kVColor: return make(kPassColor, kGiFormat, copy, 1);") &&
            Has(ext_cpp, "case Kind::kDepthMs: return plume::RenderFormat::R32_FLOAT;") && Has(ext_cpp, "case Kind::kMip: return plume::RenderFormat::R16_FLOAT;") &&
            Has(ext_cpp, "case Kind::kNormalMs: return plume::RenderFormat::R16G16_UNORM;") && Has(ext_cpp, "case Kind::kColourMip: return plume::RenderFormat::R16G16B16A16_FLOAT;") &&
            Has(ao_cpp, "constexpr plume::RenderFormat kGiFormat = plume::RenderFormat::R16G16B16A16_FLOAT;") &&
            Has(ext_cpp, "plume::RenderBlendDesc::Copy(), 1, kKindNames[k]"),
        "the owner's pipelines write the formats the AO's did, with a copy blend and one sample");
  Check(Has(Squash(ext_cpp), Squash("builder.addPushConstant(0, 4, sizeof(AoPush), plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::PIXEL);")) &&
            Has(Squash(ao_cpp), Squash("builder.addPushConstant(0, 4, sizeof(Push), plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::PIXEL);")) &&
            Has(ao_h, "using Push = AoPush;"),
        "the owner's layout is the AO's: the host's sets and a 128 byte push block for both stages");
  // the images: the names and sizes are the AO's
  Check(Has(ext_cpp, "\"ao depth\"") && Has(ext_cpp, "\"ao depth 1/2\"") && Has(ext_cpp, "\"ao depth 1/4\"") && Has(ext_cpp, "\"ao contact depth\"") && Has(ext_cpp, "\"ao normals\"") &&
            Has(ext_cpp, "\"ao contact normals\"") && Has(ext_cpp, "\"ssgi depth history\""),
        "the owner keeps the images under the names the AO gave them");
  // ---- ao.cpp calls the owner where it recorded these passes ----
  Check(order(ao_cpp, {"if (extract) {", "screen_->RecordDepth(list, chain, depth_base, summary.stencil_slot, normals_.depth_source_bits, ms);", "} else {", "pass_timing_->Mark(list, \"ao depth\");",
                       "screen_->RecordNormals(list, chain, depth_base, NormalRecipeFor(normals_));", "RecordNormals(list, plan, depth_base, normals_);",
                       "screen_->RecordColourPre(list, GiFrame(plan), base, *gi_colour, ColourRecipeFor(plan, base, *gi_colour, gi_colour_slot, gi_normal_slot));", "screen_->CapturePre([&] { RecordColourChain(", "CarRtAo::Result rt;"}),
        "ao.cpp calls the owner for the depth chain, the normals and the colour chain at the places it recorded them from, the old recording beside each");
  Check(Has(ao_cpp, "const bool extract = Extracting();") && Has(ao_cpp, "bool AmbientOcclusion::Extracting() const {\n  return screen_ && screen_->extract_on();\n}") &&
            Has(shared_h, "bool extract_on() const { return extract_ && wide_layout_ != nullptr; }") &&
            Has(shared_cpp, "extract_ = larecomp::settings::mcla_gpu_screen_extract.Get();"),
        "the switch is read by the owner once a frame and the AO asks it (an owner without its layout and shaders leaves the old path)");
  Check(Has(manifest, "\"mcla_gpu_screen_extract\": {\n  \"type\": \"bool\",\n  \"default\": \"true\",") && Has(manifest, "\"mcla_gpu_screen_extract\"") && Has(shared_cpp, "mcla_gpu_screen_extract"),
        "the A/B switch is a debug-only bool, on by default");
  Check(Has(ao_cpp, "screen_->EndDepthChain(plan.SharedGi());") && Has(ao_cpp, "linear_depth_image_ = screen_->FrameDepth0();"),
        "the shared trace swaps the owner's depth history; the quarter trace keeps its own guide pair");
  Check(Has(cmake, "    screen_extract.cpp\n") && Has(cmake, "add_executable(larecomp_screen_extract_test EXCLUDE_FROM_ALL screen_extract_test.cpp)"), "the owner's second file and this test are in the build");
  // the AO no longer holds the images
  Check(!Has(ao_h, "HostImage depth_[3]") && !Has(ao_h, "HostImage depth_full_;") && !Has(ao_h, "HostImage depth_prev_;") && !Has(ao_h, "HostImage normal_;") && !Has(ao_h, "HostImage normal_full_;"),
        "the AO holds none of the depth chain's, the history's or the normal images");
  // ---- SS-PRECACHE: the owner's chain pipelines are in the boot precache, and a flip of the A/B makes the other side's in the background ----
  std::string renderer_cpp;
  Check(Slurp(gpu + "/renderer.cpp", &renderer_cpp), "renderer.cpp can be read");
  const std::string list_at_start = Between(ext_cpp, "uint32_t ScreenShared::ListChainAtStart(uint32_t kinds) {", "bool ScreenShared::ChainReady(uint32_t kinds) {");
  Check(!list_at_start.empty() && Has(list_at_start, "precache.Add(KeyOf(kind), kKindNames[i], [this, kind] { return MakeChainPipeline(kind); })") && Has(list_at_start, "chain_listed_ |= bit;") &&
            !Has(list_at_start, "AddLive") && Has(list_at_start, "if (!passes_ || !wide_layout_) {\n    return 0;"),
        "ListChainAtStart lists the pipelines with the boot precache (Add, not the late AddLive) under the keys ChainReady and ChainPipeline look for, and only for an owner with its layout");
  Check(Has(shared_h, "uint32_t ListChainAtStart(uint32_t kinds);") && Has(ext_h, "constexpr uint32_t StartKinds(bool depth, bool mip, bool colour) {"), "the owner's method and the rule it lists by are declared");
  Check(order(renderer_cpp, {"screen_.Initialize(device_, heap_, passes_, edram_, pass_timing_);", "screen_.ListChainAtStart(ao_.ChainKindsAtStart());", "passes_.precache().Start();"}),
        "the renderer lists the owner's chain pipelines after the owner's Initialize (its layout and shaders) and before the precache starts");
  Check(order(renderer_cpp, {"ao_.Initialize(device_, frames_, heap_, passes_, edram_, pass_timing_, resolve_);", "screen_.ListChainAtStart(ao_.ChainKindsAtStart());"}),
        "... and after the AO's Initialize, which collected the start's choices");
  const std::string list_precache = Between(ao_cpp, "void AmbientOcclusion::ListPrecache(", "precache.Note(host_plan::Describe(host_plan::Pass::kOcclusion, occlusion, cls), for_occlusion);");
  Check(!list_precache.empty() && Has(list_precache, "const bool owner_records_chain = larecomp::settings::mcla_gpu_screen_extract.Get();") && Has(list_precache, "start_chain_kinds_ = 0;") &&
            Has(list_precache, "if (owner_records_chain) {\n      start_chain_kinds_ |= ChainKindsFor(needs);\n    }") && Has(list_precache, "for (const uint32_t variant : VariantsFor(needs)) {"),
        "ListPrecache collects the owner's kinds for every group it lists (occlusion, contact shadows, bounce light) while the owner records the chain");
  Check(Has(ao_cpp, "return screen::extract::StartKinds(needs.depth, needs.mip, needs.bounce && gi_ready_);") && Has(ao_cpp, "if (needs.depth && own_chain) {\n    variants.push_back(kVDepth);\n    variants.push_back(kVDepthMs);\n  }") &&
            Has(ao_cpp, "if (needs.mip && own_chain) {\n    variants.push_back(kVMip);\n  }") && Has(ao_cpp, "if (!own_chain && (variant == kVColor || variant == kVColorMs || variant == kVColorMip)) {"),
        "the owner's kinds follow the same conditions as the old path's own variants (depth, level, and the colour chain with the bounce light's shaders): one side lists, the other does not");
  Check(Has(ao_cpp, "chain_side_ = larecomp::settings::mcla_gpu_screen_extract.Get();  // the side whose chain pipelines the start lists") && Has(ao_h, "uint32_t ChainKindsAtStart() const { return start_chain_kinds_; }"),
        "the AO remembers the side the start listed and hands its kinds to the renderer");
  const std::string begin_frame = Between(ao_cpp, "void AmbientOcclusion::BeginFrame(gbuffer::PauseFxView pause_fx) {", "mode_ = mode;");
  Check(order(begin_frame, {"const bool chain_side = larecomp::settings::mcla_gpu_screen_extract.Get();", "if (chain_side != chain_side_) {", "chain_side_ = chain_side;", "occlusion_built_ = false;",
                            "contact_built_ = false;", "bounce_built_ = false;", "HoldUntilBuilt(&mode, &contact, &gi_mode);"}),
        "a flip of mcla_gpu_screen_extract asks HoldUntilBuilt again (before it runs), which lists the other side's pipelines on a worker and holds the effects off until they are made, instead of Pipeline() making them on the render thread");
}

// ---------------------------------------------------------------------------
// the pass list and the bandwidth model
// ---------------------------------------------------------------------------

struct Config {
  const char* name;
  uint32_t w, h;
  bool ultra;  // the occlusion's, the contact shadows' and the bounce light's own level: ultra traces at the picture's size (until 1080 lines), high at half of it
  bool occlusion, bounce, contact, reflections, normals;
};

struct Totals {
  uint32_t passes = 0;
  uint64_t write = 0, read = 0;
  void Add(const ex::cost::Pass& pass) {
    ++passes;
    write += pass.write_bytes;
    read += pass.read_bytes;
  }
};

// The passes the owner records once for the frame (the AO's, the bounce light's, the contact shadows' and the reflections' inputs): the depth chain (the coarser levels for the occlusion), the normals
// (the plane's: one image beside the picture's size one for the contact shadows' split), the colour chain for the bounce light, the Hi-Z for the reflections.
Totals SharedFrame(const Config& c, std::vector<std::string>* list) {
  Totals t;
  mcla::gpu::ao_size::Inputs in;
  in.render_height = c.h;
  in.occlusion = c.occlusion;
  in.occlusion_ultra = c.ultra;
  in.contact = c.contact;
  in.contact_ultra = c.contact && c.ultra;
  in.bounce = c.bounce;
  in.bounce_ultra = c.bounce && c.ultra;
  const mcla::gpu::ao_size::Choice choice = mcla::gpu::ao_size::Decide(in);
  const uint32_t render[2] = {c.w, c.h};
  const mcla::gpu::ao_size::Sizes sz = mcla::gpu::ao_size::SizesOf(render, choice);
  const bool split = c.contact && choice.contact_full;
  const auto add = [&](const ex::cost::Pass& pass) {
    t.Add(pass);
    if (list) list->push_back(std::string(pass.what));
  };
  add(ex::cost::DepthLevel0(sz.ao[0], sz.ao[1], choice.low));
  if (c.occlusion) {
    add(ex::cost::DepthMip(sz.level0[0], sz.level0[1], ex::cost::kBytesR32, "depth level 1"));
    add(ex::cost::DepthMip(sz.level1[0], sz.level1[1], ex::cost::kBytesR16, "depth level 2"));
  }
  if (split) add(ex::cost::DepthLevel0(c.w, c.h, false));
  if (c.normals) {
    add(ex::cost::Normal(sz.ao[0], sz.ao[1], "normals"));
    if (split) add(ex::cost::Normal(c.w, c.h, "normals, full size"));
  }
  if (c.bounce) {
    const bool one = choice.by_height;
    add(ex::cost::ColourLevel0(sz.ao[0], sz.ao[1], one ? 1u : 4u));
    add(ex::cost::ColourMip(sz.level0[0], sz.level0[1], "colour chain level 1"));
    add(ex::cost::ColourMip(sz.level1[0], sz.level1[1], "colour chain level 2"));
  }
  if (c.reflections) {
    const uint32_t levels = mcla::gpu::screen::HizLevelCount(sz.ao[0], sz.ao[1]);
    for (uint32_t level = 0; level < levels; ++level) {
      add(ex::cost::HizLevel(mcla::gpu::screen::HizLevelSize(sz.ao[0], level), mcla::gpu::screen::HizLevelSize(sz.ao[1], level), level == 0, "hiz level"));
    }
  }
  return t;
}

// What the same frame records when each effect makes the inputs it reads for itself: the occlusion its depth chain and normals; the bounce light its own depth chain, normals and colour chain; the contact
// shadows their own depth (the full size one beside a half size trace) and normals; the reflections their own depth and the Hi-Z over it. (A model of the arrangement before the owner; the stack never ran it:
// SS-0 and the AO's own sharing came first. It is the cost of not having one owner.)
Totals PerEffect(const Config& c) {
  Totals t;
  mcla::gpu::ao_size::Inputs in;
  in.render_height = c.h;
  in.occlusion = c.occlusion;
  in.occlusion_ultra = c.ultra;
  in.contact = c.contact;
  in.contact_ultra = c.contact && c.ultra;
  in.bounce = c.bounce;
  in.bounce_ultra = c.bounce && c.ultra;
  const mcla::gpu::ao_size::Choice choice = mcla::gpu::ao_size::Decide(in);
  const uint32_t render[2] = {c.w, c.h};
  const mcla::gpu::ao_size::Sizes sz = mcla::gpu::ao_size::SizesOf(render, choice);
  const bool split = c.contact && choice.contact_full;
  const auto chain = [&](bool coarse) {
    t.Add(ex::cost::DepthLevel0(sz.ao[0], sz.ao[1], choice.low));
    if (coarse) {
      t.Add(ex::cost::DepthMip(sz.level0[0], sz.level0[1], ex::cost::kBytesR32, "depth level 1"));
      t.Add(ex::cost::DepthMip(sz.level1[0], sz.level1[1], ex::cost::kBytesR16, "depth level 2"));
    }
    if (c.normals) t.Add(ex::cost::Normal(sz.ao[0], sz.ao[1], "normals"));
  };
  if (c.occlusion) chain(true);
  if (c.bounce) {
    chain(true);
    const bool one = choice.by_height;
    t.Add(ex::cost::ColourLevel0(sz.ao[0], sz.ao[1], one ? 1u : 4u));
    t.Add(ex::cost::ColourMip(sz.level0[0], sz.level0[1], "colour chain level 1"));
    t.Add(ex::cost::ColourMip(sz.level1[0], sz.level1[1], "colour chain level 2"));
  }
  if (c.contact) {
    if (split) {
      t.Add(ex::cost::DepthLevel0(c.w, c.h, false));
      if (c.normals) t.Add(ex::cost::Normal(c.w, c.h, "normals, full size"));
    } else {
      chain(false);
    }
  }
  if (c.reflections) {
    chain(false);
    const uint32_t levels = mcla::gpu::screen::HizLevelCount(sz.ao[0], sz.ao[1]);
    for (uint32_t level = 0; level < levels; ++level) {
      t.Add(ex::cost::HizLevel(mcla::gpu::screen::HizLevelSize(sz.ao[0], level), mcla::gpu::screen::HizLevelSize(sz.ao[1], level), level == 0, "hiz level"));
    }
  }
  return t;
}

double Mib(uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0); }

void CostChecks() {
  const Config configs[] = {
      {"1080p, occlusion ultra", 1920, 1080, true, true, false, false, false, false},
      {"1080p, occlusion + contact + bounce light + reflections, ultra, G-buffer normals", 1920, 1080, true, true, true, true, true, true},
      {"1080p, occlusion + reflections, ultra", 1920, 1080, true, true, false, false, true, false},
      {"1080p, occlusion + contact + bounce light + reflections, high, G-buffer normals", 1920, 1080, false, true, true, true, true, true},
      {"4K, occlusion ultra", 3840, 2160, true, true, false, false, false, false},
      {"4K, occlusion + contact + bounce light + reflections, ultra, G-buffer normals", 3840, 2160, true, true, true, true, true, true},
      {"4K, occlusion + reflections, ultra", 3840, 2160, true, true, false, false, true, false},
      {"4K, occlusion + contact + bounce light + reflections, high, G-buffer normals", 3840, 2160, false, true, true, true, true, true},
  };
  for (const Config& c : configs) {
    std::vector<std::string> list;
    const Totals shared = SharedFrame(c, &list);
    const Totals each = PerEffect(c);
    if (g_print) {
      std::printf("%s\n  owner, once a frame : %2u passes, writes %7.1f MiB, reads %7.1f MiB\n", c.name, shared.passes, Mib(shared.write), Mib(shared.read));
      std::printf("  per effect          : %2u passes, writes %7.1f MiB, reads %7.1f MiB\n", each.passes, Mib(each.write), Mib(each.read));
      std::printf("  saved by the owner  : %2d passes, writes %7.1f MiB, reads %7.1f MiB per frame\n", int(each.passes) - int(shared.passes), Mib(each.write - shared.write), Mib(each.read - shared.read));
    }
    Check(shared.passes <= each.passes && shared.write <= each.write && shared.read <= each.read, std::string("the owner never costs more than each effect making its own: ") + c.name);
    const int effects = int(c.occlusion) + int(c.bounce) + int(c.contact) + int(c.reflections);
    if (effects >= 3) {
      Check(shared.passes < each.passes, std::string("with three effects on the owner records fewer passes: ") + c.name);
    }
    if (effects == 1 && !c.reflections) {
      Check(shared.passes == each.passes, std::string("one effect alone records the same passes either way: ") + c.name);
    }
  }
  // The move itself saves nothing: the old path and the new one record the same passes, that is the identity above (the same draws in the same order).
  Check(ex::cost::Texels(1920, 1080) == 2073600ull, "the model's texel count");
  // The passes of a frame at the default settings: the occlusion alone is the depth chain, the coarser levels and the apply's inputs (4 passes with normals, 3 without).
  std::vector<std::string> list;
  const Totals alone = SharedFrame(configs[0], &list);
  Check(alone.passes == 3 && list[0] == "depth level 0" && list[1] == "depth level 1" && list[2] == "depth level 2", "the occlusion alone records the depth chain: level 0 and two coarser levels");
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
  IdentityChecks();
  FieldChecks();
  StartKindsChecks();
  CostChecks();
  if (!gpu.empty()) {
    SourceChecks(gpu);
  } else {
    std::printf("(no src/gpu folder given: the source checks were left out)\n");
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
