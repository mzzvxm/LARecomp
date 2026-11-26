// MCLA GPU renderer -- ray traced reflections on the opponents' cars. See car_rt_opp_refl.h.

#include "car_rt_opp_refl.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <utility>
#include <vector>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "car_rt_parked_paint_rules.h"
#include "car_rt_hit_rules.h"
#include "car_rt_paint_tiles.h"
#include "car_rt_scene.h"
#include "car_rt_traffic_rules.h"
#include "descriptors.h"
#include "device.h"
#include "gpu_class.h"
#include "rt_tier.h"
#include "host_precache.h"
#include "host_shaders.h"
#include "pass_timing.h"

// car_rt_instances.cpp: the number of other cars (the Low class has none unless asked for by hand).

namespace mcla::gpu {

namespace opp = car_rt_opp_refl;
namespace traffic = car_rt_traffic;

namespace {

constexpr const char* kPrefix = "[car rt refl]";
constexpr const char* kPipelineName = "car rt refl opp";
constexpr uint32_t kGroup = 8;  // the shader's thread group is 8 x 8
constexpr uint32_t kWindowPasses = 300;

uint64_t PipelineKey() { return HostPrecache::Key(HostPrecache::kCarRtRefl, 2); }

// The class's traffic level (the ray tracing tier's, rt_tier.h: a weak card of the High class is Mid, none is Low: the GPU class would give a weak card the traffic share of a strong one) with the
// debug override.
traffic::Level TrafficLevel(gpu_class::GpuClass budget_class) {
  return traffic::LevelWith(traffic::LevelFor(budget_class), int(larecomp::settings::mcla_gpu_car_rt_traffic_refl.Get()));
}

// The screen rectangle of each other car's box with a paint instance (BoxRect, in image texels), and which box each rectangle is of: the pass's own rectangles and the parked cars' marks (RT-PARKED-PAINT)
// are made from the same ones, so the cars marked are the cars traced.
void CarRects(const CarRtView& view, double tan_x, double tan_y, uint32_t render_width, uint32_t render_height, uint32_t divisor, uint32_t image_width, uint32_t image_height,
              std::vector<traffic::CarRect>* rects, std::vector<uint32_t>* rect_box, bool one_ray) {
  if (!rects || !rect_box) {
    return;
  }
  rects->clear();
  rect_box->clear();
  rects->reserve(view.box_count);
  for (uint32_t i = 0; i < view.box_count && view.boxes; ++i) {
    if (!view.boxes[i].paint) {
      continue;  // a car with no draw of a paint shader this frame (a car placed from memory): no pixel of it to find
    }
    const auto& box = view.boxes[i];
    if (one_ray && box.parked && car_rt_refl::CompleteBox(box) && car_rt_refl::CompleteBox(view.player_box) &&
        !car_rt_hit::PaintBoxInReach(view.player_box.min, view.player_box.max, box.min, box.max)) {
      continue;
    }
    traffic::CarRect car;
    car.rect = opp::BoxRect(view.boxes[i], tan_x, tan_y, double(render_width), double(render_height), double(std::max<uint32_t>(divisor, 1)), int32_t(image_width), int32_t(image_height));
    car.traffic = view.boxes[i].traffic;
    car.near_depth = traffic::NearDepth(view.boxes[i]);
    rects->push_back(car);
    rect_box->push_back(i);
  }
}

}  // namespace

CarRtOppRefl::~CarRtOppRefl() = default;

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

void CarRtOppRefl::Initialize(Device& device, HostPasses& passes, BindlessHeap& heap, PassTiming& pass_timing, CarRtScene& scene,
                              const plume::RenderDescriptorSetBuilder& output_builder, const plume::RenderDescriptorSetBuilder& city_builder, bool list_at_start) {
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
  const uint32_t city_set = builder.addDescriptorSet(city_builder);                    // space 3: the city's rows and counters (city_refl.h)
  builder.addPushConstant(0, kPushSpace, sizeof(Push), plume::RenderShaderStageFlag::COMPUTE);
  builder.end();
  if (texture_set != kTextureSet || output_set != kOutputSet || scene_set != kSceneSet || city_set != kCitySet) {
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
  if (!view.valid || !view.set || !f.city || !f.city->set) {
    return false;
  }
  // The traffic cars' paint (RT-TRAFFIC-REFL) is paint to find where the class rule is on; with it off a frame with only traffic paint has nothing to do.
  // The class is the ray tracing tier's (rt_tier.h: a weak card of the High class is Mid, none is Low), as the pass's own budget below and the structures' counts are: the GPU class would give a
  // weak card the traffic share of a strong one.
  Push push = {};
  std::memcpy(&push, f.base_push, kBasePushBytes);
  push.coverage[0] = f.tile_slot + 1u;
  const bool one_ray = (push.slots[3] & car_rt_refl::kSingleRayFlag) != 0u;
  const auto budget_class = one_ray ? gpu_class::GpuClass::kHigh : gpu_class::CurrentRtClass();
  const traffic::Level traffic_level = TrafficLevel(budget_class);
  const uint32_t traffic_paint = std::min(view.traffic_paint_instances, view.other_paint_instances);
  if (view.other_paint_instances - traffic_paint == 0 && (!traffic_level.on || traffic_paint == 0)) {
    ++window_.no_paint;  // no other car's paint in the list: nothing to find (cars not drawn this frame, no other car, traffic paint with the traffic off)
    return false;
  }
  // CITY-RT M2 R2: with the city in the reflections the budget is the smaller of the class's and the level's cell's (city_refl_rules.h OppBudget), and a texel casts at most the cell's rays (OppRays).
  const city_refl::Cell& cell = f.city->cell;
  const uint64_t budget = city_refl::OppBudget(
      opp::TexelBudget(budget_class, int(larecomp::settings::mcla_gpu_car_rt_cars.Get()), int(larecomp::settings::mcla_gpu_car_rt_opp_refl_texels.Get())), f.city->on ? cell : city_refl::Cell{});
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
  std::vector<traffic::CarRect> rects;
  std::vector<uint32_t> rect_box;
  CarRects(view, f.tan_x, f.tan_y, f.render_width, f.render_height, f.divisor, f.image_width, f.image_height, &rects, &rect_box, one_ray);
  const traffic::TrafficPlan traffic_plan =
      traffic::PlanDispatches(rects, budget, traffic_level, uint32_t(std::clamp<int32_t>(larecomp::settings::mcla_gpu_car_rt_opp_refl_rays.Get(), 0, 4)));
  const opp::Plan& plan = traffic_plan.plan;
  if (plan.dispatches.empty()) {
    ++window_.no_rect;  // the boxes are off the picture
    return false;
  }

  push.slots[2] = opp::kPrimaryParts;
  push.opp[2] = view.player_row;
  push.opp[3] = city_refl::OppRays(plan.rays, f.city->on ? cell : city_refl::Cell{});

  plume::RenderCommandList* list = f.list;
  plume::RenderTextureBarrier barriers[2] = {plume::RenderTextureBarrier(f.image->texture.get(), plume::RenderTextureLayout::GENERAL),
                                             plume::RenderTextureBarrier(f.aux->texture.get(), plume::RenderTextureLayout::GENERAL)};
  pass_timing_->Mark(list, "car rt refl opp");
  list->setComputePipelineLayout(layout_.get());
  list->setComputeDescriptorSet(heap_->texture_set(), kTextureSet);
  list->setComputeDescriptorSet(f.output_set, kOutputSet);
  list->setComputeDescriptorSet(view.set, kSceneSet);
  list->setComputeDescriptorSet(f.city->set, kCitySet);
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
  w.traffic_cars += traffic_plan.traffic_cars;
  w.traffic_admitted += traffic_plan.traffic_admitted;
  w.traffic_far += traffic_plan.traffic_far;
  w.traffic_over += traffic_plan.traffic_over;
  w.traffic_texels += traffic_plan.traffic_texels;
  w.covered += plan.covered;
  w.most_covered = std::max<uint64_t>(w.most_covered, plan.covered);
  ++w.frames_at_rays[push.opp[3] >= opp::kRaysFull ? 0 : push.opp[3] >= opp::kRaysReduced ? 1 : 2];
  if (!first_logged_) {
    first_logged_ = true;
    REXLOG_INFO("{} opponents: the first pass, frame {}: {} ({} paint instances of other cars in the top level, {} of them the traffic paint's; the player's record at row {}); budget {} texels; "
                "traffic: {} ({}); the history image {}x{} for a {}x{} picture (divisor {}); the pass is timed as 'car rt refl opp'",
                kPrefix, f.frame, opp::Describe(plan), view.other_paint_instances, view.traffic_paint_instances, view.player_row, budget, traffic_level.on ? "on" : "off",
                traffic::Describe(traffic_plan), f.image_width, f.image_height, f.render_width, f.render_height, f.divisor);
  }
  if (w.passes >= kWindowPasses) {
    const double n = double(w.passes);
    REXLOG_INFO("{} opponents, frames {}..{}: {} passes ({} dispatches, {:.1f} cars admitted of {:.1f} with a rectangle, {:.2f} over the budget), rectangles counted {:.0f} texels, covered {:.0f} "
                "(most {}); frames at 4 / 2 / 1 rays: {} / {} / {}; frames not run: no paint instance {}, boxes off the picture {}, pipeline not ready {}, off or no budget {}; budget {} texels; "
                "traffic cars a pass (RT-TRAFFIC-REFL): {:.2f} with a rectangle, {:.2f} admitted ({:.0f} texels), {:.2f} too far or small, {:.2f} over the share",
                kPrefix, w.first_frame, f.frame, w.passes, w.dispatches, double(w.admitted) / n, double(w.cars) / n, double(w.skipped) / n, double(w.counted) / n,
                double(w.covered) / n, w.most_covered, w.frames_at_rays[0], w.frames_at_rays[1], w.frames_at_rays[2], w.no_paint, w.no_rect, w.not_ready, w.off, budget,
                double(w.traffic_cars) / n, double(w.traffic_admitted) / n, double(w.traffic_texels) / n, double(w.traffic_far) / n, double(w.traffic_over) / n);
    window_ = Window{};
  }
  return true;
}

// RT-PARKED-PAINT: the marks the history pass draws before this pass runs, one for every parked car the pass will trace (the same boxes, the same rectangles, the same admission as Record's: the
// class's traffic level and the opponents' budget; the budget here is the class's alone, where Record's may be lowered by the city's cell, so a mark may be made for a car the pass then leaves
// out: the admission is monotone in the budget, never the other way). Nothing while the switch is off, the pass has no pipeline yet, no traffic level, or no parked car has a rectangle in the picture.
std::vector<car_rt_parked_paint::Mark> CarRtOppRefl::ParkedMarks(const MarkInput& in) {
  std::vector<car_rt_parked_paint::Mark> marks;
  if (!ready_ || !pipeline_ || !in.view || !in.view->valid || !in.view->boxes || in.view->traffic_paint_instances == 0 ||
      !larecomp::settings::mcla_gpu_car_rt_opp_refl.Get() || !larecomp::settings::mcla_gpu_car_rt_parked_paint.Get()) {
    return marks;
  }
  const bool one_ray = in.one_ray;
  const auto budget_class = one_ray ? gpu_class::GpuClass::kHigh : gpu_class::CurrentRtClass();
  const traffic::Level level = TrafficLevel(budget_class);
  const uint64_t budget =
      opp::TexelBudget(budget_class, int(larecomp::settings::mcla_gpu_car_rt_cars.Get()), int(larecomp::settings::mcla_gpu_car_rt_opp_refl_texels.Get()));
  if (!level.on || budget == 0) {
    return marks;
  }
  std::vector<traffic::CarRect> rects;
  std::vector<uint32_t> rect_box;
  CarRects(*in.view, in.tan_x, in.tan_y, in.render_width, in.render_height, in.divisor, in.image_width, in.image_height, &rects, &rect_box, one_ray);
  const traffic::TrafficPlan plan = traffic::PlanDispatches(rects, budget, level, uint32_t(std::clamp<int32_t>(larecomp::settings::mcla_gpu_car_rt_opp_refl_rays.Get(), 0, 4)));
  marks = car_rt_parked_paint::PlanMarks(in.view->boxes, in.view->box_count, rects, rect_box, plan);
  return marks;
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
