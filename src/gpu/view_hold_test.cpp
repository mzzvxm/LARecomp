#include "view_hold_rules.h"
#include "env_hold_link.h"

#include <cstdio>
#include <limits>
#include <numbers>

namespace {
using namespace mcla::gpu::view_hold;
using mcla::gpu::env_hold::Resolution;

int checks = 0;
int failures = 0;

void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    std::printf("FAIL: %s\n", message);
    ++failures;
  }
}

Vec3 Yaw(float degrees) {
  const float angle = degrees * std::numbers::pi_v<float> / 180.0f;
  return {std::sin(angle), 0.0f, std::cos(angle)};
}

void RefreshRules() {
  const Sample capture{.camera = {{0, 0, 0}, {0, 0, 1}, {0, 1, 0}},
                       .time_ms = 100, .brightness = 0.4f};
  const Limits limits;
  Sample now = capture;
  Check(Decide(limits, capture, now, false) == Decision::kMissing, "wait for both resolved products");
  now.time_ms = 60'000;
  Check(Decide(limits, capture, now, true) == Decision::kHold, "parked has no periodic redraw");
  now.camera.position[0] = 0.049f;
  now.camera.forward = Yaw(0.49f);
  Check(Decide(limits, capture, now, true) == Decision::kHold, "parked jitter stays inside the deadbands");
  now.camera.position[0] = 0.051f;
  Check(Decide(limits, capture, now, true) == Decision::kAge, "slow travel still refreshes");
  now.camera.position[0] = 0;
  now.camera.forward = Yaw(0.51f);
  Check(Decide(limits, capture, now, true) == Decision::kAge, "small sustained turns still refresh");
  now.camera.forward = capture.camera.forward;
  now.camera.up = {0.01f, 0.99995f, 0.0f};
  Check(Decide(limits, capture, now, true) == Decision::kAge, "slow camera roll still refreshes");

  now = capture;
  now.time_ms = 165;
  now.camera.position = {8.0f, 0, 0};
  now.camera.forward = Yaw(19.99f);
  Check(Decide(limits, capture, now, true) == Decision::kHold, "within all moving limits");
  now.time_ms = 166;
  Check(Decide(limits, capture, now, true) == Decision::kAge, "66 ms age floor");
  now.time_ms = 101;
  now.camera.position[0] = 8.01f;
  Check(Decide(limits, capture, now, true) == Decision::kTravel, "8 m travel forces a refresh");
  now.camera.position[0] = 0;
  now.camera.forward = Yaw(20.01f);
  Check(Decide(limits, capture, now, true) == Decision::kTurn, "20 degree yaw forces a refresh");
  now.camera.forward = capture.camera.forward;
  now.camera.up = {0.35f, 0.93675f, 0.0f};
  Check(Decide(limits, capture, now, true) == Decision::kTurn, "roll has the same limit");

  now = capture;
  now.cut = true;
  Check(Decide(limits, capture, now, true) == Decision::kCut, "cut at identical pose refreshes");
  now.cut = false;
  now.time_ms--;
  Check(Decide(limits, capture, now, true) == Decision::kCut, "clock regression refreshes");
  now = capture;
  for (uint32_t world : {1u, 2u, 4u, 8u, 16u}) {
    now.world = world;
    Check(Decide(limits, capture, now, true) == Decision::kWorld, "world or source changes refresh");
    Check(Decide(limits, now, capture, true) == Decision::kWorld, "returning to the previous world refreshes");
  }
  now = capture;
  ++now.clock_jumps;
  Check(Decide(limits, capture, now, true) == Decision::kLight, "hour jump");
  now = capture;
  now.weather = 2;
  Check(Decide(limits, capture, now, true) == Decision::kLight, "weather change");
  now = capture;
  now.brightness = 0.263f;
  Check(Decide(limits, capture, now, true) == Decision::kLight, "enter a darker tunnel");
  Check(Decide(limits, now, capture, true) == Decision::kLight, "leave a darker tunnel");
  now.brightness = 0.264f;
  Check(Decide(limits, capture, now, true) == Decision::kHold, "small brightness variation");
  now.brightness = -1;
  Check(Decide(limits, capture, now, true) == Decision::kHold, "missing lighting sample preserves the product");
  now.camera.position[0] = std::numeric_limits<float>::quiet_NaN();
  Check(Decide(limits, capture, now, true) == Decision::kMissing, "invalid current camera fails open");
  Check(Decide(limits, now, capture, true) == Decision::kMissing, "invalid captured camera fails open");
}

void MovingAndSettling() {
  Limits limits;
  Sample capture{.camera = {{0, 0, 0}, {0, 0, 1}, {0, 1, 0}}};
  uint32_t redraws = 0;
  for (uint64_t frame = 1; frame <= 60; ++frame) {
    Sample now = capture;
    now.time_ms = frame * 1000 / 60;
    now.camera.position[0] = 0.12f * float(frame);
    if (Decide(limits, capture, now, true) != Decision::kHold) {
      Check(now.time_ms - capture.time_ms <= 67, "motion refreshes on the first eligible frame");
      capture = now;
      ++redraws;
    }
  }
  Check(redraws == 15, "15 captures per second in steady motion at 60 FPS");
  Sample now = capture;
  now.camera.position[0] += 8.1f;
  Check(Decide(limits, capture, now, true) == Decision::kTravel, "crossing the travel threshold refreshes");
  capture = now;
  now.time_ms += 90;
  now.camera.position[0] -= 0.02f;
  Check(Decide(limits, capture, now, true) == Decision::kHold, "settling around the old threshold does not oscillate");
}
}  // namespace

int main() {
  RefreshRules();
  MovingAndSettling();
  Check(Resolution("game") == "off" && Resolution("off") == "off", "keep the guest resolution preference");
  for (const auto value : {"auto", "high", "ultra", "on"}) {
    Check(Resolution(value) == "on", "higher and Auto resolutions migrate to the fixed 2x mode");
  }
  std::printf("view hold: %d checks, %d failures\n", checks, failures);
  return failures != 0;
}
