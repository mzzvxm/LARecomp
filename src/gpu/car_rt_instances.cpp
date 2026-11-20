// MCLA GPU renderer -- the per-frame instance list of the car ray tracing scene. See car_rt_instances.h.

#include "car_rt_instances.h"

#include <algorithm>
#include <format>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "gpu_class.h"

namespace mcla::gpu {

namespace rr = car_rt_rules;
namespace ci = car_rt_instances;

namespace {

constexpr const char* kPrefix = "[car rt]";
// Frames between two window lines of the log.
constexpr uint64_t kWindowFrames = 300;

}  // namespace

ci::Limits CarRtInstances::CurrentLimits() const {
  return ci::LimitsFor(gpu_class::Current(), int(larecomp::settings::mcla_gpu_car_rt_cars.Get()), double(larecomp::settings::mcla_gpu_car_rt_cars_radius.Get()));
}

const ci::List& CarRtInstances::Build(const car_frame::Frame& frame, const ci::Limits& limits, bool flip_z, bool glass_rule, const FindMesh& find) {
  list_.Clear();
  if (list_.items.capacity() < rr::kMaxInstances) {
    list_.items.reserve(rr::kMaxInstances);
    list_.rows.reserve(rr::kMaxInstances + ci::kMaxCars);
  }
  // The player's car: the body draws whose mesh has a structure (the driver is not in the scene), in the draws' order.
  for (uint32_t i = 0; i < frame.draws.size() && list_.items.size() < rr::kMaxInstances; ++i) {
    const car_frame::Draw& draw = frame.draws[i];
    if (draw.part != car_frame::Part::kBody || draw.mesh_key == 0) {
      continue;
    }
    ci::MeshRef ref;
    if (!find(draw.mesh_key, &ref)) {
      ++list_.stats.missing_mesh;
      continue;
    }
    ci::Item item;
    item.mesh_key = draw.mesh_key;
    if (!rr::PackInstanceTransform(draw.model_to_world, frame.view, flip_z, item.transform)) {
      ++list_.stats.bad_matrix;
      continue;
    }
    item.mask = rr::InstanceMask(draw.vs == rr::kTyreVs, draw.after_world, glass_rule);
    list_.items.push_back(item);
    list_.rows.push_back(rr::MakeRow(ref.first_index, ref.first_vertex, item.mask, draw.after_world, i));
    ++list_.stats.player_instances;
  }
  // The other cars: the nearest whole cars that fit the instance budget.
  if (limits.On() && !frame.other_cars.empty()) {
    ci::AppendOthers(&list_, frame.other_draws, frame.other_cars, frame.view, flip_z, limits.instances, rr::kMaxInstances, find);
  }
  ci::CountSharing(&list_);
  return list_;
}

void CarRtInstances::NoteFrame(uint64_t frame, uint64_t blas_built_total, const ci::Limits& limits) {
  const ci::ListStats& s = list_.stats;
  if (!started_) {
    started_ = true;
    window_.first_frame = frame;
    window_.built_at_start = blas_built_total;
  }
  Window& w = window_;
  ++w.frames;
  w.frames_with_cars += s.cars != 0;
  w.player_sum += s.player_instances;
  w.other_sum += s.other_instances;
  w.cars_sum += s.cars;
  w.missing_sum += s.missing_mesh;
  w.bad_sum += s.bad_matrix;
  w.cut_sum += s.cars_cut;
  w.player_max = std::max(w.player_max, s.player_instances);
  w.other_max = std::max(w.other_max, s.other_instances);
  w.cars_max = std::max(w.cars_max, s.cars);
  w.distinct_max = std::max(w.distinct_max, s.distinct_meshes);
  w.shared_max = std::max(w.shared_max, s.shared_meshes);

  if (s.cars != 0 && !first_cars_logged_) {
    first_cars_logged_ = true;
    REXLOG_INFO("{} frame {}: the first other cars are in the top level: {} cars, {} instances beside the player's {}, {} distinct meshes in all of which {} are "
                "named by more than one instance (one structure each, shared); limits: {}",
                kPrefix, frame, s.cars, s.other_instances, s.player_instances, s.distinct_meshes, s.shared_meshes, ci::Describe(limits));
  }
  if (frame < w.first_frame + kWindowFrames) {
    return;
  }
  const double n = double(std::max<uint32_t>(w.frames, 1));
  if (limits.On() || w.frames_with_cars != 0) {
    REXLOG_INFO("{} cars, frames {}..{}: {} with a top level, {} of them with other cars; instances a frame: player {:.0f} (at most {}), others {:.0f} (at most {}), "
                "other cars {:.1f} (at most {}); the most distinct meshes {} of which {} shared by several instances; left out per frame: {:.1f} draws "
                "with no structure yet, {:.2f} with a bad matrix, {:.2f} cars over the instance budget; structures built in the window: {}; other cars' "
                "meshes read from guest memory: {} ({} triangles), draws held back by the read budget {} and by the memory share {}; limits: {}",
                kPrefix, w.first_frame, frame, w.frames, w.frames_with_cars, double(w.player_sum) / n, w.player_max, double(w.other_sum) / n, w.other_max,
                double(w.cars_sum) / n, w.cars_max, w.distinct_max, w.shared_max, double(w.missing_sum) / n, double(w.bad_sum) / n, double(w.cut_sum) / n,
                blas_built_total - w.built_at_start, window_work_.meshes_read, window_work_.triangles_read, window_work_.over_stage_budget,
                window_work_.over_memory, ci::Describe(limits));
  }
  window_ = Window{};
  window_.first_frame = frame;
  window_.built_at_start = blas_built_total;
  window_work_ = Work{};
}

}  // namespace mcla::gpu
