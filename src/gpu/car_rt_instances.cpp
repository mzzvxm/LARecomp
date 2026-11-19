// MCLA GPU renderer -- the per-frame instance list of the car ray tracing scene. See car_rt_instances.h.

#include "car_rt_instances.h"

#include <algorithm>
#include <format>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "gpu_class.h"

REXCVAR_DEFINE_INT32(
    mcla_gpu_car_rt_cars, -1, "MCLA/GPU",
    "Debug: how many OTHER cars (opponents, traffic, parked cars) the ray traced car effects see besides your own: your car's paint and chrome "
    "reflect a car driving alongside. -1 (the default): the graphics card's class decides (the nearest 4 within 14 m of your car on the High class, 2 within 10 m on "
    "the Mid class, none on the Low class); 0 turns the other cars off (your car alone, as it was); 1 to 8 asks for that many. Each car is built "
    "once into ray tracing structures (8 to 10 MiB at full detail) and a new car appears in parts over a few frames. Live.")
    .debug_only()
    .range(-1, 8)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(
    mcla_gpu_car_rt_cars_memory, true, "MCLA/GPU",
    "Debug (the fix's own switch): the ray traced car effects also see the cars the camera does not (the car beside yours, out of the picture): a car that was "
    "drawn while it was near is remembered, and while it is not drawn it is placed again from the game's own list of cars. The same class limits apply "
    "(nearest cars, by distance to your car). off: only the cars drawn this frame. The reflection shows a remembered car in its paint where the model data gave it "
    "(mcla_gpu_car_rt_cars_model), else in a fixed grey in shade (its colour is not known while the camera does not see it). Live.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(
    mcla_gpu_car_rt_cars_model, 1, "MCLA/GPU",
    "Debug (the fix's own switch): the ray traced car effects see the cars the camera does not draw built from the cars' OWN MODEL DATA in the game's memory (the part "
    "table, each part's vertex and index buffers, the part matrices, the paint), not only from the draws a car once made. 0: off. 1 (the default): a car built from its "
    "model data only where no car placed from memory (mcla_gpu_car_rt_cars_memory) stands, and the paint colour of the car for the cars placed from memory. 2: the model "
    "data first: a car built from it wherever it is, a car placed from memory beside it is dropped. The same class limits apply (nearest cars, by distance to your car). Live.")
    .debug_only()
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(
    mcla_gpu_car_rt_cars_traffic, -1, "MCLA/GPU",
    "Debug (the fix's own switch): how many TRAFFIC and PARKED cars the ray traced car effects see built from their own model data (mcla_gpu_car_rt_cars_model), besides the racers: the cars "
    "of the game's ambient list (the renderer's traffic client) that the camera does not draw this frame, ranked by their distance to your car within the class's radius. -1 (the "
    "default): the class's count (4 on the High class, 2 on the Mid class); 0 leaves the traffic out; 1 to 8 asks for that many. A traffic car's model is a fraction of a racer's "
    "(one low detail drawable), so the count is cheap. A traffic car is placed only after its bone matrices have been checked against the draws of a traffic car the camera sees. Live.")
    .debug_only()
    .range(-1, 8)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(
    mcla_gpu_car_rt_cars_wheels, true, "MCLA/GPU",
    "Debug (the fix's own switch): a racer the camera does not draw is built WITH its four wheels (the tyre and the rim from the game's wheel manager, placed by the vehicle's own wheel "
    "matrices, steer and spin included), not the body and the parts alone. The wheels are placed only after their placement has been checked against the draws of the cars the "
    "camera sees. off: no wheels. Live.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_car_rt_cars_match, 0.0, "MCLA/GPU",
    "Debug: how near, in metres, the centre of a car the camera sees must be to a car of the game's own list to be that car, for the cars the camera does not see "
    "(mcla_gpu_car_rt_cars_memory). 0, the default: 3 m. A fast car's draws lag its record, so a larger number matches it; cars side by side are 3.5 m apart. Live.")
    .debug_only()
    .range(0.0, 8.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_car_rt_cars_radius, 0.0, "MCLA/GPU",
    "Debug: how far from your car, in metres, another car may be to be seen by the ray traced car effects (0, the default: the graphics card's "
    "class decides, 10 to 14 m). The reflection reaches 4 m from your car's paint, so a car more than about 10 m away cannot show in it. Live.")
    .debug_only()
    .range(0.0, 60.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu {

namespace rr = car_rt_rules;
namespace ci = car_rt_instances;

namespace {

constexpr const char* kPrefix = "[car rt]";
// Frames between two window lines of the log.
constexpr uint64_t kWindowFrames = 300;

}  // namespace

ci::Limits CarRtInstances::CurrentLimits() const {
  return ci::LimitsFor(gpu_class::Current(), int(REXCVAR_GET(mcla_gpu_car_rt_cars)), double(REXCVAR_GET(mcla_gpu_car_rt_cars_radius)));
}

bool CarRtInstances::RememberedOn() const {
  return REXCVAR_GET(mcla_gpu_car_rt_cars_memory);
}

car_model_rt::Mode CarRtInstances::ModelMode() const {
  return car_model_rt::Mode(std::clamp<int>(int(REXCVAR_GET(mcla_gpu_car_rt_cars_model)), 0, 2));
}

uint32_t CarRtInstances::TrafficCars() const {
  return car_model_parts::TrafficCarsFor(CurrentLimits(), int(REXCVAR_GET(mcla_gpu_car_rt_cars_traffic)));
}

bool CarRtInstances::WheelsOn() const {
  return REXCVAR_GET(mcla_gpu_car_rt_cars_wheels);
}

double CarRtInstances::MatchMetres() const {
  return double(REXCVAR_GET(mcla_gpu_car_rt_cars_match));
}

const ci::List& CarRtInstances::Build(const car_frame::Frame& frame, const ci::Limits& limits, bool flip_z, bool glass_rule, const FindMesh& find,
                                      const std::vector<ci::OtherCar>* extra_cars, const std::vector<ci::OtherDraw>* extra_draws,
                                      const std::vector<car_model_rt::CarInfo>* paints) {
  list_.Clear();
  drawn_painted_ = 0;
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
    if (ref.has_bounds) {
      ci::GrowBox(&list_.player_box, item.transform, ref);  // (a mesh without a box would add its origin alone: left out)
    }
  }
  // The other cars: the nearest whole cars that fit the instance budget and the class's count of cars. The cars the camera does not see this frame
  // (placed from memory) are ranked with the drawn ones by the distance to the player, not by the screen.
  const bool remembered = extra_cars && extra_draws && !extra_cars->empty();
  const bool painted = paints && !paints->empty() && !frame.other_cars.empty();
  if (limits.On() && (!frame.other_cars.empty() || remembered)) {
    if (remembered || painted) {
      merged_cars_.assign(frame.other_cars.begin(), frame.other_cars.end());
      // the cars the camera draws take the paint of the walked car at their place (their copies here: the frame's own list is not changed)
      for (ci::OtherCar& car : merged_cars_) {
        float rgb[3];
        if (painted && !car.has_paint && !car.remembered && car_model_rt::PaintForDrawn(car, *paints, rgb)) {
          car.has_paint = true;
          for (int c = 0; c < 3; ++c) {
            car.paint[c] = rgb[c];
          }
          ++drawn_painted_;
        }
      }
      merged_draws_.assign(frame.other_draws.begin(), frame.other_draws.end());
      if (remembered) {
        merged_cars_.insert(merged_cars_.end(), extra_cars->begin(), extra_cars->end());
        merged_draws_.insert(merged_draws_.end(), extra_draws->begin(), extra_draws->end());
      }
      ci::AppendOthers(&list_, merged_draws_, merged_cars_, frame.view, flip_z, limits.instances, rr::kMaxInstances, find, limits.cars);
    } else {
      ci::AppendOthers(&list_, frame.other_draws, frame.other_cars, frame.view, flip_z, limits.instances, rr::kMaxInstances, find, limits.cars);
    }
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
  w.chrome_sum += s.other_chrome;
  w.cars_sum += s.cars;
  w.missing_sum += s.missing_mesh;
  w.bad_sum += s.bad_matrix;
  w.cut_sum += s.cars_cut;
  w.remembered_cars_sum += s.remembered_cars;
  w.remembered_instances_sum += s.remembered_instances;
  w.frames_with_remembered += s.remembered_cars != 0;
  w.remembered_max = std::max(w.remembered_max, s.remembered_cars);
  w.player_max = std::max(w.player_max, s.player_instances);
  w.other_max = std::max(w.other_max, s.other_instances);
  w.chrome_max = std::max(w.chrome_max, s.other_chrome);
  w.cars_max = std::max(w.cars_max, s.cars);
  w.distinct_max = std::max(w.distinct_max, s.distinct_meshes);
  w.shared_max = std::max(w.shared_max, s.shared_meshes);

  if (s.cars != 0 && !first_cars_logged_) {
    first_cars_logged_ = true;
    REXLOG_INFO("{} frame {}: the first other cars are in the top level: {} cars, {} instances beside the player's {} (of the others' {} are draws of the chrome's "
                "shader: their tailpipes, badges and trim, traced for the chrome's occlusion), {} distinct meshes in all of which {} are "
                "named by more than one instance (one structure each, shared); limits: {}",
                kPrefix, frame, s.cars, s.other_instances, s.player_instances, s.other_chrome, s.distinct_meshes, s.shared_meshes, ci::Describe(limits));
  }
  if (s.remembered_cars != 0 && !first_remembered_logged_) {
    first_remembered_logged_ = true;
    REXLOG_INFO("{} frame {}: the first cars the camera does not see are in the top level: {} of the {} other cars, with {} instances, placed from memory at their vehicles' "
                "matrices (the game's own list of cars, the draws of an earlier frame); {} in all drawn this frame",
                kPrefix, frame, s.remembered_cars, s.cars, s.remembered_instances, s.cars - s.remembered_cars);
  }
  if (frame < w.first_frame + kWindowFrames) {
    return;
  }
  const double n = double(std::max<uint32_t>(w.frames, 1));
  if (limits.On() || w.frames_with_cars != 0) {
    REXLOG_INFO("{} cars, frames {}..{}: {} with a top level, {} of them with other cars; instances a frame: player {:.0f} (at most {}), others {:.0f} (at most {}; chrome draws {:.1f}, at most {}), "
                "other cars {:.1f} (at most {}); the most distinct meshes {} of which {} shared by several instances; left out per frame: {:.1f} draws "
                "with no structure yet, {:.2f} with a bad matrix, {:.2f} cars over the instance budget; structures built in the window: {}; other cars' "
                "meshes read from guest memory: {} ({} triangles), draws held back by the read budget {} and by the memory share {}; cars placed from memory "
                "(not drawn: car_rt_memory_rules.h): in {} frames, {:.2f} a frame (at most {}), {:.1f} instances a frame; limits: {}",
                kPrefix, w.first_frame, frame, w.frames, w.frames_with_cars, double(w.player_sum) / n, w.player_max, double(w.other_sum) / n, w.other_max,
                double(w.chrome_sum) / n, w.chrome_max, double(w.cars_sum) / n, w.cars_max, w.distinct_max, w.shared_max, double(w.missing_sum) / n, double(w.bad_sum) / n, double(w.cut_sum) / n,
                blas_built_total - w.built_at_start, window_work_.meshes_read, window_work_.triangles_read, window_work_.over_stage_budget,
                window_work_.over_memory, w.frames_with_remembered, double(w.remembered_cars_sum) / n, w.remembered_max, double(w.remembered_instances_sum) / n,
                ci::Describe(limits));
  }
  window_ = Window{};
  window_.first_frame = frame;
  window_.built_at_start = blas_built_total;
  window_work_ = Work{};
}

}  // namespace mcla::gpu
