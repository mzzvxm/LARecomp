#include "view_hold_rules.h"
#include "env_hold_link.h"
#include "side_view_ui_map.h"

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

void MapRefreshRules() {
  const MapSample capture{.car = {{0, 0, 0}, {0, 0, 1}, {0, 1, 0}},
                          .zoom = {1, 1, 1}, .owner = 1, .has_transform = true};
  constexpr int64_t deadline = 33'333'333;
  const auto check = [&](const MapSample& now, Decision want, const char* message) {
    Check(DecideMap(capture, now, deadline, true) == want, message);
  };
  Check(DecideMap(capture, capture, deadline, false) == Decision::kMissing,
        "missing or incompatible resolve replays the radar draws");
  MapSample now = capture;
  now.time_ns = deadline - 1;
  check(now, Decision::kHold, "parked holds until 30 Hz");
  ++now.time_ns;
  check(now, Decision::kAge, "parked redraws moving markers at 30 Hz");
  now.time_ns = 1;
  now.car.position[0] = 2.0f;
  now.car.forward = Yaw(2.99f);
  check(now, Decision::kHold, "small travel and steering stay held before the deadline");
  now.car.position[0] = 2.01f;
  check(now, Decision::kTravel, "fast travel forces an immediate update");
  now.car.position[0] = 0.0f;
  now.car.forward = Yaw(3.01f);
  check(now, Decision::kTurn, "heading forces an immediate update");
  now = capture;
  now.angles[1] = 0.053f;
  check(now, Decision::kTurn, "movie rotation can change on its own");
  now.angles[1] = 2.0f * std::numbers::pi_v<float> - 0.015f;
  check(now, Decision::kHold, "a heading wrapping around north stays held");
  now.origin[2] += 2.01f;
  check(now, Decision::kTravel, "the movie can pan while the car stands still");
  now = capture;
  now.zoom[2] += 0.00005f;
  check(now, Decision::kHold, "ignore zoom noise");
  now.zoom[2] += 0.001f;
  check(now, Decision::kWorld, "zoom refreshes immediately");
  now = capture;
  now.has_transform = false;
  check(now, Decision::kWorld, "the map's transform availability changed");
  now = capture;
  now.zoom[0] = std::numeric_limits<float>::quiet_NaN();
  check(now, Decision::kMissing, "invalid projection redraws the view");
  Check(DecideMap(now, capture, deadline, true) == Decision::kMissing,
        "a missing old projection redraws even if the new one is valid");
  now = capture;
  now.car.forward = {0, 1, 0};
  now.car.up = {0, 0, -1};
  check(now, Decision::kMissing, "no heading in the map plane");
  now = capture;
  now.owner++;
  check(now, Decision::kWorld, "a replacement movie cannot reuse the product");
  now = capture;
  now.world = 1;
  check(now, Decision::kWorld, "a pause or photo transition refreshes");
  now = capture;
  now.cut = true;
  check(now, Decision::kCut, "scripted cut refreshes");
  now = capture;
  now.clock_epoch++;
  check(now, Decision::kCut, "scene clock epoch change refreshes");
  now.time_ns = 10;
  Check(DecideMap(now, capture, deadline, true) == Decision::kCut, "clock regression refreshes");

  for (int64_t fps : {24, 30, 40, 60, 90, 120, 144}) {
    now = capture;
    MapSample last = now;
    int64_t due = MapDeadline(now, 0, Decision::kMissing);
    int captures = 0;
    for (int64_t frame = 1; frame <= fps; ++frame) {
      now.time_ns = frame * 1'000'000'000 / fps;
      const auto decision = DecideMap(last, now, due, true);
      if (decision != Decision::kHold) {
        last = now;
        due = MapDeadline(now, due, decision);
        ++captures;
      }
    }
    Check(captures == std::min<int64_t>(fps, 30), "30 Hz on even and uneven frame rates");
  }
  now.time_ns = 1'000'000'000;
  Check(MapDeadline(now, deadline, Decision::kAge) == now.time_ns + deadline,
        "no catch-up burst after a hitch");
  Check(MapDeadline(now, deadline, Decision::kTurn) == now.time_ns + deadline,
        "a forced draw restarts cadence");
}

void RadarIdentity() {
  using mcla::gpu::ui_map_views::RadarTarget;
  struct Key {
    uint16_t base_tiles = 168, pitch = 240;
    uint8_t depth = 0, format = 0, samples = 4, reserved = 0;
  };
  constexpr uint32_t size = (219u << 18) | (219u << 3);
  Check(RadarTarget(Key{}, size), "the 220x220 radar alone is eligible");
  Check(!RadarTarget(Key{}, 0), "a missing declaration fails open");
  Check(!RadarTarget(Key{}, size + 8), "foreign use of the radar key fails open");
  Check(!RadarTarget(Key{.base_tiles = 169}, size), "a different EDRAM tile fails open");
  Check(!RadarTarget(Key{.pitch = 238}, size), "a different framebuffer pitch fails open");
  Check(!RadarTarget(Key{.base_tiles = 0, .pitch = 760}, size), "expanded map stays live");
  Check(!RadarTarget(Key{.base_tiles = 224, .pitch = 160}, size), "impostor refresh stays live");
  Check(!RadarTarget(Key{.base_tiles = 224, .pitch = 280}, size), "256x256 refresh stays live");
  Check(!RadarTarget(Key{.depth = 1}, size), "depth cannot share the radar hold");
  Check(!RadarTarget(Key{.format = 1}, size), "a different color format fails open");
  Check(!RadarTarget(Key{.samples = 2}, size), "a different sample count fails open");
  Check(!RadarTarget(Key{.reserved = 1}, size), "a different EDRAM layer fails open");
}
}  // namespace

int main() {
  RefreshRules();
  MovingAndSettling();
  MapRefreshRules();
  RadarIdentity();
  Check(Resolution("game") == "off" && Resolution("off") == "off", "keep the guest resolution preference");
  for (const auto value : {"auto", "high", "ultra", "on"}) {
    Check(Resolution(value) == "on", "higher and Auto resolutions migrate to the fixed 2x mode");
  }
  std::printf("view hold: %d checks, %d failures\n", checks, failures);
  return failures != 0;
}
