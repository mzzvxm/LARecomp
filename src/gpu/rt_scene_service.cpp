// MCLA GPU renderer -- the ray tracing scene service. See rt_scene_service.h.

#include "rt_scene_service.h"

#include <cmath>
#include <format>
#include <string>

#include <rex/logging/log_category.h>

#include "car_dump_rules.h"
#include "car_rt_scene.h"
#include "city_hit_scene.h"
#include "city_rt_scene.h"
#include "live_text.h"
#include "settings/larecomp_settings.h"
#include "temporal_camera.h"

namespace mcla::gpu {

namespace {
constexpr const char* kPrefix = "[rt scene]";

// mcla_gpu_city_hit_data (debug): off, high, ultra.
LiveText& DebugHitSetting() {
  static LiveText text("mcla_gpu_city_hit_data");
  return text;
}
}  // namespace

void RtSceneService::Initialize(CarRtScene* cars, CityRtScene* city, CityHitScene* hit) {
  cars_ = cars;
  city_ = city;
  hit_ = hit;
  board_ = rt_scene::WantBoard{};
  camera_.Reset();
  counts_ = Counts{};
}

void RtSceneService::Shutdown() {
  cars_ = nullptr;
  city_ = nullptr;
  hit_ = nullptr;
}

void RtSceneService::Want(rt_scene::Consumer who, const rt_scene::Needs& needs) {
  board_.Want(who, needs);
  if (needs.cars && cars_ != nullptr) {
    cars_->RequestCollect();  // the car frame collects the player's draws only while somebody asks (not Request(): that one would add the want to the car effects' own)
  }
}

void RtSceneService::NoteAnchor(const ConstantBanks::Banks& banks, bool cut) {
  const uint8_t* ps = banks.ps.data();
  const float width = car_dump::BankFloat(ps, 62, 0);
  const float height = car_dump::BankFloat(ps, 62, 1);
  if (!(width >= 16.0f && width <= 16384.0f && height >= 16.0f && height <= 16384.0f)) {
    return;
  }
  temporal::Camera camera;
  if (temporal::ExtractCamera(ps, uint32_t(width + 0.5f), uint32_t(height + 0.5f), board_.frame(), &camera) != temporal::Reject::kNone) {
    return;  // a frame whose anchor gave no camera: the pose of the frame before stands (the predictor refuses one that is too old)
  }
  ++anchors_;
  cut_anchors_ += cut ? 1 : 0;
  camera_.Note(board_.frame(), camera.view_inverse, cut);
}

rt_scene::TlasPlan RtSceneService::PlanTlas(bool car_frame_valid) {
  rt_scene::CameraPrediction prediction;
  const bool camera_ok = camera_.Predict(board_.frame(), &prediction);
  const rt_scene::TlasPlan plan = rt_scene::PlanTlas(board_.Now(), car_frame_valid, camera_ok);
  LogPlan(plan, car_frame_valid);
  if (!plan.build) {
    if (!board_.Now().any()) {
      ++counts_.nothing;
    } else if (!car_frame_valid && board_.Now().city != city_rt::Level::kOff && !camera_ok) {
      ++counts_.no_camera;
    }
    return plan;
  }
  if (plan.view == rt_scene::ViewSource::kCamera) {
    ++counts_.city_only_tops;
    counts_.held_pose += prediction.extrapolated ? 0 : 1;
  } else {
    ++counts_.car_tops;
  }
  return plan;
}

bool RtSceneService::PredictedView(temporal::Mat4* view, bool* flip_z) {
  rt_scene::CameraPrediction prediction;
  if (!camera_.Predict(board_.frame(), &prediction) || !view || !flip_z) {
    return false;
  }
  *view = prediction.view;
  *flip_z = prediction.flip_z;
  return true;
}

void RtSceneService::EndFrame() {
  // The debug want: the structures and the hit data with no effect reading them (mcla_gpu_city_hit_data), to test the pool and the city-only top level on their own.
  const city_rt::Level debug = city_rt::LevelOfCell(DebugHitSetting().Get());
  if (debug != city_rt::Level::kOff) {
    rt_scene::Needs needs;
    needs.city = debug;
    needs.hit = debug;
    board_.Want(rt_scene::Consumer::kCheck, needs);
  }
  board_.EndFrame();
  // The merged wants of the frame that just ended (and the one before): the level of the structures and of the hit data for the frame that follows. The hit data first, so that the
  // city scene's budget sees whether the pool is there (CityHitScene::Adjust).
  const rt_scene::Needs needs = board_.Now();
  if (hit_ != nullptr) {
    hit_->BeginFrame(needs.hit);
  }
  if (city_ != nullptr) {
    city_->BeginFrame(needs.city, needs.city_radius);
  }
  if (board_.ReleaseDue()) {
    REXLOG_INFO("{} nothing has asked for the ray tracing scene for {} frames: the structures and the pool are given back by their own idle counts", kPrefix, rt_scene::WantBoard::kIdleReleaseFrames);
  }
}

void RtSceneService::LogPlan(const rt_scene::TlasPlan& plan, bool car_frame_valid) {
  // One line when the reason changes (the first frame, the car leaving the picture, the camera lost), not one a frame.
  const int why = int(plan.view) * 16 + (plan.build ? 1 : 0) + (car_frame_valid ? 2 : 0) + (plan.cars ? 4 : 0) + (plan.city ? 8 : 0);
  if (why == logged_why_ || (log_frame_ != 0 && board_.frame() < log_frame_ + 120)) {
    return;
  }
  logged_why_ = why;
  log_frame_ = board_.frame();
  REXLOG_INFO("{} frame {}: {} (the top level {}{}{}, the player's car {})", kPrefix, board_.frame(), plan.why, plan.build ? "is built" : "is not built", plan.cars ? " with the cars" : "",
              plan.city ? " with the city" : "", car_frame_valid ? "was found" : "was not found");
}

std::string RtSceneService::CensusText() const {
  std::string wants;
  for (size_t i = 0; i < rt_scene::kConsumerCount; ++i) {
    const uint64_t n = board_.frames_wanted(rt_scene::Consumer(i));
    if (n != 0) {
      wants += std::format("{}{} {}", wants.empty() ? "" : ", ", rt_scene::ConsumerName(rt_scene::Consumer(i)), n);
    }
  }
  if (wants.empty() && counts_.city_only_tops == 0) {
    return {};
  }
  return std::format("; ray tracing scene: wanted by {}; {} top levels with the car's camera, {} city-only ({} on a held pose), {} without a camera", wants.empty() ? "nobody" : wants, counts_.car_tops,
                     counts_.city_only_tops, counts_.held_pose, counts_.no_camera);
}

}  // namespace mcla::gpu
