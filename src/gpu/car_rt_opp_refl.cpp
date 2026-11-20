// MCLA GPU renderer -- ray traced reflections on the opponents' cars. See car_rt_opp_refl.h.

#include "car_rt_opp_refl.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <utility>
#include <vector>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "car_rt_scene.h"
#include "descriptors.h"
#include "device.h"
#include "gpu_class.h"
#include "host_precache.h"
#include "host_shaders.h"
#include "pass_timing.h"

// car_rt_instances.cpp: the number of other cars (the Low class has none unless asked for by hand).

namespace mcla::gpu {

namespace opp = car_rt_opp_refl;

namespace {

constexpr const char* kPrefix = "[car rt refl]";
constexpr const char* kPipelineName = "car rt refl opp";
constexpr uint32_t kGroup = 8;  // the shader's thread group is 8 x 8
constexpr uint32_t kWindowPasses = 300;

uint64_t PipelineKey() { return HostPrecache::Key(HostPrecache::kCarRtRefl, 2); }

}  // namespace

CarRtOppRefl::~CarRtOppRefl() = default;

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

void CarRtOppRefl::Initialize(Device& device, HostPasses& passes, BindlessHeap& heap, PassTiming& pass_timing, CarRtScene& scene,
                              const plume::RenderDescriptorSetBuilder& output_builder, bool list_at_start) {
  device_ = &device;
  passes_ = &passes;
  heap_ = &heap;
  pass_timing_ = &pass_timing;
  ready_ = false;
  listed_ = false;
  pipeline_failed_ = false;

  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  const uint32_t texture_set = builder.addDescriptorSet(heap.texture_set_builder());  // space 0: Texture2D<float4>[] (the history)
  const uint32_t output_set = builder.addDescriptorSet(output_builder);                // space 1: u0 the image, u1 the aux image
  const uint32_t scene_set = builder.addDescriptorSet(scene.SetDesc());                // space 2: the car's structures (CAR_RT_SPACE)
  builder.addPushConstant(0, kPushSpace, sizeof(Push), plume::RenderShaderStageFlag::COMPUTE);
  builder.end();
  if (texture_set != kTextureSet || output_set != kOutputSet || scene_set != kSceneSet) {
    REXLOG_ERROR("{} opponents: the layout's sets are not in the order the shader declares them", kPrefix);
    return;
  }
  layout_ = device.CreatePipelineLayout(builder, "car rt refl opp layout");
  if (!layout_) {
    return;
  }
  const HostShaderBlob blob = CarRtReflOppShader();
  shader_ = passes.CreateShader(blob.data, blob.size, blob.name);
  if (!shader_) {
    layout_.reset();
    return;
  }
  ready_ = true;
  if (list_at_start) {
    listed_ = passes.precache().Add(PipelineKey(), kPipelineName, [this] { return MakePipeline(); });
    passes.precache().Note("ray traced reflections of the opponents' paint: on where the car self-reflection is ray traced", listed_ ? 1u : 0u);
  }
}

void CarRtOppRefl::Shutdown() {
  // The host builder's function names this object's layout and shader: CarRtRefl::Shutdown stopped it already (passes_->precache().Stop()).
  pipeline_.reset();
  shader_.reset();
  layout_.reset();
  ready_ = false;
  device_ = nullptr;
  passes_ = nullptr;
  heap_ = nullptr;
  pass_timing_ = nullptr;
}

std::unique_ptr<plume::RenderPipeline> CarRtOppRefl::MakePipeline() {
  return device_->CreateComputePipeline(plume::RenderComputePipelineDesc(layout_.get(), shader_.get(), kGroup, kGroup, 1), kPipelineName);
}

// The pipeline when it is made, else null: the frame goes on without the pass (never a wait, never a compile on the render thread).
plume::RenderPipeline* CarRtOppRefl::Pipeline() {
  if (pipeline_) {
    return pipeline_.get();
  }
  if (pipeline_failed_) {
    return nullptr;
  }
  HostPrecache& precache = passes_->precache();
  if (!listed_) {
    // Off at the start, wanted now: made on a worker of their own.
    std::vector<HostPrecache::Item> fresh;
    fresh.push_back({PipelineKey(), kPipelineName, [this] { return MakePipeline(); }});
    listed_ = true;
    precache.AddLive("ray traced reflections of the opponents' paint", std::move(fresh));
    REXLOG_INFO("{} opponents: the pipeline was not listed at the start: it is made in the background, and the pass runs from the frame it is done", kPrefix);
    return nullptr;
  }
  if (!precache.Ready(PipelineKey())) {
    return nullptr;
  }
  pipeline_ = precache.Acquire(PipelineKey(), kPipelineName, [this] { return MakePipeline(); });
  if (!pipeline_) {
    pipeline_failed_ = true;
    REXLOG_ERROR("{} opponents: the pass's pipeline could not be made: the opponents' paint takes the screen-space version this session", kPrefix);
  }
  return pipeline_.get();
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

bool CarRtOppRefl::Record(const Frame& f) {
  if (!ready_ || !f.list || !f.view || !f.base_push || !f.image || !f.aux || !f.output_set || !f.image->texture || !f.aux->texture) {
    return false;
  }
  if (f.base_push_bytes != kBasePushBytes) {
    if (!size_logged_) {
      size_logged_ = true;
      REXLOG_ERROR("{} opponents: the player's push block is {} bytes, not the {} this pass copies (CarRtRefl::Push changed): the pass is off", kPrefix, f.base_push_bytes,
                   kBasePushBytes);
    }
    return false;
  }
  if (!larecomp::settings::mcla_gpu_car_rt_opp_refl.Get()) {
    ++window_.off;
    return false;
  }
  const CarRtView& view = *f.view;
  if (!view.valid || !view.set) {
    return false;
  }
  if (view.other_paint_instances == 0) {
    ++window_.no_paint;  // no other car's paint in the list: nothing to find (traffic, cars not drawn this frame, no other car)
    return false;
  }
  const uint64_t budget = opp::TexelBudget(gpu_class::Current(), int(larecomp::settings::mcla_gpu_car_rt_cars.Get()), int(larecomp::settings::mcla_gpu_car_rt_opp_refl_texels.Get()));
  if (budget == 0) {
    ++window_.off;
    return false;
  }
  plume::RenderPipeline* pipeline = Pipeline();
  if (!pipeline) {
    ++window_.not_ready;
    return false;
  }

  // where to look: the screen rectangle of each other car's box
  std::vector<opp::TexelRect> rects;
  rects.reserve(view.box_count);
  for (uint32_t i = 0; i < view.box_count && view.boxes; ++i) {
    if (!view.boxes[i].paint) {
      continue;  // a car with no draw of the paint's shader this frame (traffic, a car placed from memory): no pixel of it to find
    }
    rects.push_back(opp::BoxRect(view.boxes[i], f.tan_x, f.tan_y, double(f.render_width), double(f.render_height), double(std::max<uint32_t>(f.divisor, 1)),
                                 int32_t(f.image_width), int32_t(f.image_height)));
  }
  const opp::Plan plan = opp::PlanDispatches(rects, budget, uint32_t(std::clamp<int32_t>(larecomp::settings::mcla_gpu_car_rt_opp_refl_rays.Get(), 0, 4)));
  if (plan.dispatches.empty()) {
    ++window_.no_rect;  // the boxes are off the picture
    return false;
  }

  Push push = {};
  std::memcpy(&push, f.base_push, kBasePushBytes);
  push.slots[2] = opp::kPrimaryParts;
  push.opp[2] = view.player_row;
  push.opp[3] = plan.rays;

  plume::RenderCommandList* list = f.list;
  plume::RenderTextureBarrier barriers[2] = {plume::RenderTextureBarrier(f.image->texture.get(), plume::RenderTextureLayout::GENERAL),
                                             plume::RenderTextureBarrier(f.aux->texture.get(), plume::RenderTextureLayout::GENERAL)};
  pass_timing_->Mark(list, "car rt refl opp");
  list->setComputePipelineLayout(layout_.get());
  list->setComputeDescriptorSet(heap_->texture_set(), kTextureSet);
  list->setComputeDescriptorSet(f.output_set, kOutputSet);
  list->setComputeDescriptorSet(view.set, kSceneSet);
  list->setPipeline(pipeline);
  for (size_t i = 0; i < plan.dispatches.size(); ++i) {
    const opp::TexelRect& r = plan.dispatches[i];
    // The player's dispatch wrote the images (and each of ours may touch the texels the group grid of another reaches over): a barrier before every dispatch. Same layout:
    // a UAV barrier on D3D12, a memory barrier on Vulkan.
    list->barriers(plume::RenderBarrierStage::COMPUTE, nullptr, 0, barriers, 2);
    push.opp[0] = uint32_t(r.x0);
    push.opp[1] = uint32_t(r.y0);
    list->setComputePushConstants(0, &push, 0, sizeof(push));
    list->dispatch(opp::GroupsX(r, kGroup), opp::GroupsY(r, kGroup), 1);
  }

  // the totals and the log
  ++passes_run_;
  dispatches_run_ += plan.dispatches.size();
  Window& w = window_;
  if (w.passes == 0) {
    w.first_frame = f.frame;
  }
  ++w.passes;
  w.dispatches += plan.dispatches.size();
  w.cars += plan.cars;
  w.admitted += plan.admitted;
  w.skipped += plan.skipped;
  w.counted += plan.texels;
  w.covered += plan.covered;
  w.most_covered = std::max<uint64_t>(w.most_covered, plan.covered);
  ++w.frames_at_rays[plan.rays >= opp::kRaysFull ? 0 : plan.rays >= opp::kRaysReduced ? 1 : 2];
  if (!first_logged_) {
    first_logged_ = true;
    REXLOG_INFO("{} opponents: the first pass, frame {}: {} ({} paint instances of other cars in the top level, the player's record at row {}); budget {} texels; the history image {}x{} for "
                "a {}x{} picture (divisor {}); the pass is timed as 'car rt refl opp'",
                kPrefix, f.frame, opp::Describe(plan), view.other_paint_instances, view.player_row, budget, f.image_width, f.image_height, f.render_width, f.render_height, f.divisor);
  }
  if (w.passes >= kWindowPasses) {
    const double n = double(w.passes);
    REXLOG_INFO("{} opponents, frames {}..{}: {} passes ({} dispatches, {:.1f} cars admitted of {:.1f} with a rectangle, {:.2f} over the budget), rectangles counted {:.0f} texels, covered {:.0f} "
                "(most {}); frames at 4 / 2 / 1 rays: {} / {} / {}; frames not run: no paint instance {}, boxes off the picture {}, pipeline not ready {}, off or no budget {}; budget {} texels",
                kPrefix, w.first_frame, f.frame, w.passes, w.dispatches, double(w.admitted) / n, double(w.cars) / n, double(w.skipped) / n, double(w.counted) / n,
                double(w.covered) / n, w.most_covered, w.frames_at_rays[0], w.frames_at_rays[1], w.frames_at_rays[2], w.no_paint, w.no_rect, w.not_ready, w.off, budget);
    window_ = Window{};
  }
  return true;
}

std::string CarRtOppRefl::CensusText() {
  if (!ready_ || passes_run_ == 0) {
    return {};
  }
  const uint64_t interval = passes_run_ - census_passes_;
  census_passes_ = passes_run_;
  return std::format("; opponents: {} passes since the start ({} in the interval, {} dispatches)", passes_run_, interval, dispatches_run_);
}

}  // namespace mcla::gpu
