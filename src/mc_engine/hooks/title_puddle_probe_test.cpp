#include "title_puddle_probe.h"
#include "gpu/title_capture_rules.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace {
int checks = 0;
void Check(bool value) {
    ++checks;
    if (value) return;
    std::fprintf(stderr, "title_puddle_probe: check %d failed\n", checks);
    std::exit(1);
}
}  // namespace

int main() {
  using namespace mc::title_puddles;
  namespace capture = mcla::gpu::title_capture;
  using namespace std::chrono_literals;
  constexpr std::array arms{Arm::Off, Arm::Baseline, Arm::DryResponse, Arm::NoPlanar,
                            Arm::NoProjected, Arm::NoGlows, Arm::FixedExposure, Arm::NoBloom};
  constexpr std::array names{"", "baseline", "dry-response", "no-planar", "no-projected",
                             "no-glows", "fixed-exposure", "no-bloom"};
  Check(Parse("dry") == Arm::Off);
  for (size_t i = 0; i < arms.size(); ++i) {
    Check(Parse(names[i]) == arms[i]);
    double wetness = 1.17;
    RoadParams(arms[i], wetness);
    Check(wetness == (arms[i] == Arm::DryResponse ? double(0.095f) : 1.17));
    Check(SkipVehiclePoint(arms[i], 0) == (arms[i] == Arm::NoGlows));
    Check(SkipVehiclePoint(arms[i], 1) == (arms[i] == Arm::NoProjected));
    Check(SkipVehiclePoint(arms[i], 0xFFFFFFFFu) == (arms[i] == Arm::NoProjected));
  }

  const capture::Clock::time_point title(10s);
  Check(!capture::Due(0, {}, title));
  Check(!capture::Due(0, title, title - 1ms));
  Check(capture::Due(0, title, title));
  Check(capture::Due(0, title, title + 2s));
  Check(!capture::Due(30, title, title + 29'999ms));
  Check(capture::Due(30, title, title + 30s));
  Check(capture::Due(30, title, title + 31s));
  Check(ClassifyToneInput(true, false, 1, 1) == Input::Exposure);
  Check(ClassifyToneInput(true, false, 1, 2) == Input::Other);
  Check(ClassifyToneInput(false, true, 1, 1) == Input::Other);
  Check(ClassifyToneInput(false, true, 320, 180) == Input::Bloom);
  Check(ClassifyToneInput(true, false, 320, 180) == Input::Other);
  Check(ClassifyToneInput(false, true, 320, 179) == Input::Other);
  Check(ClassifyToneInput(false, true, 1280, 720) == Input::Other);

  constexpr auto label = "320 2x k_2_10_10_10_FLOAT@128";
  auto plan = capture::Parse(" road reflection , rt:128,320 2x k_2_10_10_10_FLOAT@128,scene ", "30, 0,30,12");
  Check(plan.has_value());
  Check(plan->seconds == (std::vector<uint32_t>{0, 12, 30}));
  Check(plan->targets.size() == 3);
  const auto& named = plan->targets[0];
  const auto& tiled = plan->targets[1];
  const auto& labelled = plan->targets[2];
  Check(named.Matches(128, label, "road reflection"));
  Check(!named.Matches(128, label, ""));
  Check(tiled.Matches(128, "any label", ""));
  Check(!tiled.Matches(129, label, "road reflection"));
  Check(labelled.Matches(128, label, ""));
  Check(!labelled.Matches(128, "320 1x k_2_10_10_10_FLOAT@128", ""));
  auto scene = capture::Parse("scene", "0");
  Check(scene && scene->targets.empty() && scene->seconds == (std::vector<uint32_t>{0}));
  auto tile_zero = capture::Parse("rt:0", "30");
  Check(tile_zero && tile_zero->targets[0].Matches(0, "", ""));
  Check(tile_zero && !tile_zero->targets[0].Matches(1, "", ""));
  for (auto invalid : {"", " ", "rt:", "rt:-1", "rt:abc", "rt:4294967296", "scene,", ",rt:0"}) {
    Check(!capture::Parse(invalid, "0"));
  }
  for (auto invalid : {"", " ", "+1", "-1", "6.5", "4294967296", "30,", "0, ,30", "30s"}) {
    Check(!capture::Parse("scene", invalid));
  }
  std::printf("title_puddle_probe: %d checks passed\n", checks);
}
