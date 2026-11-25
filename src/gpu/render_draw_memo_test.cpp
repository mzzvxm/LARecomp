// Differential checks of the draw memos without a device or shader pack.
// Run it: cmake --build <dir> --target larecomp_render_draw_memo_test, then larecomp_render_draw_memo_test (exit 0 = passed).

#include <array>
#include <cstdio>
#include <vector>

#include "draw_target_memo.h"
#include "pipelines.h"
#include "temporal_identities.h"

using namespace mcla::gpu;

namespace {

int checks = 0;
int failures = 0;

void Check(bool okay, const char* what) {
  ++checks;
  if (!okay) {
    ++failures;
    std::printf("FAIL: %s\n", what);
  }
}

constexpr uint32_t Size(uint32_t width, uint32_t height) {
  return ((width - 1) << 18) | ((height - 1) << 3);
}

// In-memory EDRAM twin. The memo and its uncached reference both use the
// production DrawScale and DrawDepthKey with these target answers.
struct Targets {
  uint64_t serial = 1;
  bool open = true;
  uint32_t host_level = 2;
  Scale output = Scale::Whole(2);
  Scale world{25};
  bool created = false;
  uint64_t change_serial() const { return serial; }
  bool scene_phase() const { return open; }
  bool SceneApart() const { return world != output; }

  static bool IsSceneKey(const EdramKey& key) {
    return temporal::MainColorSamples(key) != 0 ||
           (temporal::MainDepthSamples(key) != 0 && key.reserved == 0);
  }
  uint8_t DepthLayer(const EdramKey& depth, const EdramKey* color0, uint32_t size) const {
    if (depth.depth && depth.base_tiles == 0 && depth.pitch == 320 && depth.samples == 2 &&
        depth.format == uint8_t(xe::DepthRenderTargetFormat::kD24FS8) &&
        size != 0 && size != Size(256, 256)) {
      return side_views::kForeignUseLayer;
    }
    const uint32_t samples = temporal::MainDepthSamples(depth);
    if (SceneApart() && (samples == 1 || samples == 4)) {
      return (color0 ? temporal::MainColorSamples(*color0) != 0 : open) ? 0 : 1;
    }
    return 0;
  }
  bool HostSamplesFollowLevel(const EdramKey& key, uint32_t size) const {
    return key.samples > 1 && size == Size(1280, 720);
  }
  uint32_t HostSamplesOf(const EdramKey& key, uint32_t size) const {
    return HostSamplesFollowLevel(key, size) ? host_level : key.samples;
  }
  Scale ScaleOf(const EdramKey& key, uint32_t size) const {
    if (created && IsSceneKey(key)) {
      return Scale{30};
    }
    if (size == Size(1280, 720)) {
      return IsSceneKey(key) ? world : output;
    }
    return {};
  }
};

struct TargetCase {
  Targets edram;
  XenosState state;
  uint32_t colors = 1;
  bool depth = true;
  uint32_t size = Size(1280, 720);

  TargetCase() {
    state.surface_pitch = temporal::kMainPitch;
    state.msaa = xe::MSAASamples::k4X;
    state.depth_base_tiles = temporal::kMainDepthBase;
    state.depth_format = xe::DepthRenderTargetFormat::kD24FS8;
    state.color[0].base_tiles = temporal::kMainColorBase;
    state.color[0].format = xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT;
  }
  DrawTargetMemo::Result Full() {
    return {DrawScale(edram, state, colors, depth, size),
            depth ? DrawDepthKey(edram, state, colors, size) : EdramKey{}};
  }
};

void TestTargets() {
  const auto get = [](DrawTargetMemo& memo, TargetCase& c, bool want_hit, const char* why) {
    bool hit = false;
    const auto& answer = memo.Get(c.edram, c.state, c.colors, c.depth, c.size, hit);
    Check(hit == want_hit, why);
    Check(answer == c.Full(), "target memo agrees with a full target lookup");
  };
  const auto change = [&](TargetCase before, auto modify, const char* why) {
    DrawTargetMemo memo;
    get(memo, before, false, "first target lookup");
    get(memo, before, true, "identical target lookup");
    modify(before);
    get(memo, before, false, why);
    get(memo, before, true, "updated target lookup");
  };
  const TargetCase main;
  change(main, [](auto& c) { c.state.surface_pitch = 640; }, "pitch");
  change(main, [](auto& c) { c.state.msaa = xe::MSAASamples::k2X; }, "guest MSAA");
  change(main, [](auto& c) { ++c.state.color[0].base_tiles; }, "colour base");
  change(main, [](auto& c) { c.state.color[0].format = xe::ColorRenderTargetFormat::k_8_8_8_8; },
         "colour format");
  change(main, [](auto& c) { ++c.state.depth_base_tiles; }, "depth base");
  change(main, [](auto& c) { c.state.depth_format = xe::DepthRenderTargetFormat::kD24S8; },
         "guest depth format");
  change(main, [](auto& c) { ++c.colors; }, "colour count");
  TargetCase zero_depth_fields;
  zero_depth_fields.state.depth_format = xe::DepthRenderTargetFormat::kD24S8;
  change(zero_depth_fields, [](auto& c) { c.depth = false; }, "depth presence");
  change(main, [](auto& c) { c.size = Size(640, 360); }, "declared size with identical registers");
  change(main, [](auto& c) { c.edram.created = true; ++c.edram.serial; }, "target creation serial");
  change(main, [](auto& c) { c.edram.host_level = 1; ++c.edram.serial; }, "MSAA level serial");
  change(main, [](auto& c) { c.edram.world = c.edram.output; ++c.edram.serial; }, "scene scale serial");

  TargetCase depth_only;
  depth_only.colors = 0;
  change(depth_only, [](auto& c) { c.edram.open = false; }, "depth-only scene and UI phases");
  change(depth_only, [](auto& c) { c.depth = false; }, "no first target");

  TargetCase foreign_depth;
  foreign_depth.colors = 0;
  foreign_depth.state.surface_pitch = 320;
  foreign_depth.state.msaa = xe::MSAASamples::k2X;
  foreign_depth.size = Size(256, 256);
  change(foreign_depth, [](auto& c) { c.size = Size(320, 180); }, "side-view depth use");
  DrawTargetMemo memo;
  TargetCase unrelated;
  get(memo, unrelated, false, "initial signature");
  unrelated.state.viewport[0] = 123;
  unrelated.state.scissor_br = 17;
  unrelated.state.color[1].base_tiles = 19;
  get(memo, unrelated, true, "unrelated register and secondary target changes");
}

struct LayoutAnswer {
  uint64_t vs;
  std::vector<uint8_t> declaration;
  std::vector<uint8_t> strides;
  bool operator==(const LayoutAnswer&) const = default;
};

void TestLayouts() {
  TwoWayDrawMemo<InputLayoutMemoKey, LayoutAnswer> memo;
  std::array<uint8_t, 2 * guest::kDeclElementBytes> decl{};
  std::array<uint8_t, guest::kStreamCount> strides{};
  auto get = [&](uint64_t vs, size_t length, bool expect_hit, bool two_way = true) {
    const InputLayoutMemoView key{vs, {decl.data(), length}, strides};
    const LayoutAnswer full{vs, {decl.begin(), decl.begin() + length},
                            {strides.begin(), strides.end()}};
    const auto* found = memo.Find(key);
    if (!found && two_way) {
      found = memo.FindOther(key);
    }
    Check((found != nullptr) == expect_hit, "layout memo hit");
    if (found) {
      Check(*found == full, "layout memo agrees with the full layout input");
    } else {
      memo.Put(key, full, two_way);
    }
  };
  get(1, decl.size(), false);
  get(2, decl.size(), false);  // VS
  get(1, decl.size(), true);
  get(2, decl.size(), true);
  decl.back() = 1;
  get(2, decl.size(), false);  // bytes and ownership
  get(2, guest::kDeclElementBytes, false);  // size
  strides.back() = 4;
  get(2, guest::kDeclElementBytes, false);  // stride
  get(2, guest::kDeclElementBytes, true);
  strides.back() = 0;
  get(2, guest::kDeclElementBytes, true);
  memo.Clear();
  get(1, 0, false, false);
  get(2, 0, false, false);
  get(1, 0, false, false);
  get(1, 0, true, false);
  get(2, 0, false);
}

struct PipelineCase {
  XenosState state;
  PipelineInputs inputs;
  plume::RenderPrimitiveTopology topology{};
  bool polygonal = false;
  plume::RenderFormat formats[kColorTargetCount]{};
  plume::RenderFormat depth_format{};
  uint32_t wanted = 0;

  PipelineBuildSignature Signature() const {
    return PipelineBuildSignature::From(state, inputs, topology, polygonal, formats, depth_format, wanted);
  }
};

void TestPipelineKeys() {
  ShaderStore::Entry vs, ps;
  InputLayout layout;
  const PipelineCase base;
  auto change = [&](auto modify, const char* why) {
    TwoWayDrawMemo<PipelineBuildSignature, unsigned> memo;
    memo.Put(base.Signature(), 1);
    PipelineCase other = base;
    modify(other);
    const auto signature = other.Signature();
    Check(!memo.Find(signature) && !memo.FindOther(signature), why);
    memo.Put(signature, 2);
    const unsigned* a = memo.FindOther(base.Signature());
    Check(a && *a == 1, "alternating draw gets its original pipeline key result");
    const unsigned* b = memo.FindOther(signature);
    Check(b && *b == 2, "alternating draw gets the second pipeline key result");
  };
  change([&](auto& c) { c.inputs.vs = &vs; }, "pipeline VS");
  change([&](auto& c) { c.inputs.ps = &ps; }, "pipeline PS");
  change([&](auto& c) { c.inputs.input_layout = &layout; }, "pipeline layout");
  change([](auto& c) { c.wanted = 1; }, "live feature mask");
  change([](auto& c) { c.topology = plume::RenderPrimitiveTopology(1); }, "host topology");
  change([](auto& c) { c.polygonal = true; }, "guest polygonal primitive");
  change([](auto& c) { c.depth_format = plume::RenderFormat(1); }, "host depth format");
  change([](auto& c) { c.state.depth_bound = true; }, "depth bound");
  change([](auto& c) { c.state.msaa = xe::MSAASamples::k4X; }, "pipeline guest MSAA");
  change([](auto& c) { c.state.depth_format = xe::DepthRenderTargetFormat::kD24FS8; }, "pipeline guest depth format");
  for (uint32_t i = 0; i < kColorTargetCount; ++i) {
    change([i](auto& c) { c.formats[i] = plume::RenderFormat(1); }, "host colour format");
    change([i](auto& c) { c.state.color[i].blend.value = 1; }, "colour blend");
    change([i](auto& c) { c.state.color[i].bound = true; }, "colour bound");
    change([i](auto& c) { c.state.color[i].write_mask = 1; }, "colour write mask");
    change([i](auto& c) { c.state.poly_offset[i] = 1.0f; }, "polygon bias word");
  }
  change([](auto& c) { c.state.mode.edram_mode = xe::EDRAMMode::kColorDepth; }, "EDRAM mode");
  change([](auto& c) { c.state.depth_control.value = 1; }, "depth control");
  change([](auto& c) { c.state.stencil.value = 1; }, "stencil");
  change([](auto& c) { c.state.stencil_bf.value = 1; }, "back-face stencil");
  change([](auto& c) { c.state.color_control.value = 1; }, "colour control");
  change([](auto& c) { c.state.sc_mode.value = 1; }, "raster control");
  change([](auto& c) { c.state.clip.clip_disable = 1; }, "depth clip");
  change([](auto& c) { c.state.alpha_ref = 1.0f; }, "alpha reference");
  PipelineCase unrelated = base;
  unrelated.state.viewport[0] = 12;
  unrelated.state.scissor_br = 12;
  unrelated.inputs.msaa_guest_samples = 4;  // a Pipelines::Get input
  Check(base.Signature() == unrelated.Signature(), "unrelated viewport, scissor and level tracking");
}

void TestReadyPipelines() {
  TwoWayDrawMemo<PipelineKey, int> memo;
  PipelineKey base;
  PipelineKey variant = base;
  variant.ps_variant = 4;
  PipelineKey multisampled = variant;
  multisampled.sample_count = 8;
  memo.Put(base, 1);
  memo.Put(variant, 2);
  Check(!memo.Find(multisampled) && !memo.FindOther(multisampled), "host sample count is in final key");
  const int* older = memo.FindOther(base);
  Check(older && *older == 1, "different variant resolves separately and an older hit promotes");
  memo.Put(multisampled, 3);
  Check(!memo.Find(variant) && !memo.FindOther(variant), "least recently used final key is evicted");
  memo.Clear();
  Check(!memo.Find(multisampled) && !memo.FindOther(base), "shutdown invalidates both ready ways");
}

}  // namespace

int main() {
  TestTargets();
  TestLayouts();
  TestPipelineKeys();
  TestReadyPipelines();
  std::printf("render_draw_memo_test: %d checks, %d failed\n", checks, failures);
  return failures != 0;
}
