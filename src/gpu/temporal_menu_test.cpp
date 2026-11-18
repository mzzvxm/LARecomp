// Check of the temporal upscalers' two menu-world rules (temporal_menu.h): which vehicle motion feeds the vectors of the pause
// view, and when the jitter is held. A program of its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every
// check holds.
//
//   cmake --build <build dir> --target larecomp_temporal_menu_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_temporal_menu_test [<src/gpu dir>]
//
// With the source folder as its argument it also checks that temporal.cpp applies the two rules where they belong (the vehicle
// table is built only through UseGameVehicleMotion, the jitter goes through HeldJitter, the hold sees the frames that kept
// or restarted their history).

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "temporal_menu.h"
#include "settings/larecomp_settings.h"

using namespace mcla::gpu::temporal;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

void VehicleChecks() {
  Check(UseGameVehicleMotion(false, false), "the game view takes the game's vehicle motion");
  Check(UseGameVehicleMotion(false, true), "... with the debug switch too");
  Check(!UseGameVehicleMotion(true, false), "the pause view (menu, garage, map) does not by default: the vehicles take the camera's motion");
  Check(UseGameVehicleMotion(true, true), "the debug switch gives the game's slots back in the pause view");
}

void HoldChecks() {
  {
    JitterHold h;
    Check(!h.Held() && h.streak() == 0, "nothing is held at the start");
    bool changed = false;
    for (uint32_t i = 0; i < JitterHold::kHoldAfter - 1; ++i) {
      changed = h.Observe(true) || changed;
    }
    Check(!changed && !h.Held(), "frames that restarted the history one or two times in a row do not hold the jitter");
    Check(h.Observe(true) && h.Held(), "the third in a row holds it (and says the hold began)");
    Check(h.holds() == 1, "one hold began");
    Check(!h.Observe(true) && h.Held(), "a fourth does not change anything: still held, nothing to say");
    Check(h.streak() == JitterHold::kHoldAfter, "the streak stops at the limit (no overflow over a long run of restarts)");
    Check(h.Observe(false) && !h.Held() && h.streak() == 0, "the first frame that kept its history ends the hold at once (and says it)");
    Check(!h.Observe(false), "a frame that keeps its history says nothing");
  }
  {
    // one restart now and then (a camera cut) never holds anything, however often over a long run
    JitterHold h;
    bool ever_held = false;
    for (uint32_t i = 0; i < 100000; ++i) {
      h.Observe(i % 7 == 0 || i % 7 == 1);   // two in a row, then five that keep
      ever_held = ever_held || h.Held();
    }
    Check(!ever_held, "two restarts in a row, then good frames, over 100000 frames: the jitter is never held");
    Check(h.restarted_total() == 100000 / 7 * 2 + 2, "the restarts are counted (28572 of 100000)");
  }
  {
    // a world that restarts every frame holds from the third frame on and stays held; one good frame ends it, the next restarts hold it again after three
    JitterHold h;
    uint32_t held_frames = 0;
    for (uint32_t i = 0; i < 300; ++i) {
      h.Observe(true);
      held_frames += h.Held();
    }
    Check(held_frames == 300 - (JitterHold::kHoldAfter - 1), "a world that restarts the history every frame holds the jitter from its third frame on");
    h.Observe(false);
    Check(!h.Held(), "a good frame ends the hold");
    for (uint32_t i = 0; i < JitterHold::kHoldAfter - 1; ++i) {
      h.Observe(true);
    }
    Check(!h.Held(), "it takes kHoldAfter restarts in a row again, not fewer");
    h.Observe(true);
    Check(h.Held() && h.holds() == 2, "and then holds again (the second hold)");
    h.Clear();
    Check(!h.Held() && h.streak() == 0, "Clear drops the streak (a path change starts the run again)");
  }
  Check(HeldJitter(0.37f, false, true) == 0.37f, "a frame that is not held keeps its jitter");
  Check(HeldJitter(-0.21f, true, true) == 0.0f, "a held frame has none");
  Check(HeldJitter(0.37f, true, false) == 0.37f, "the debug switch off: held frames keep the jitter as before");
}

bool ReadFile(const std::string& path, std::string* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return true;
}

void SourceChecks(const std::string& dir) {
  std::string cpp;
  std::string header;
  if (!ReadFile(dir + "/temporal.cpp", &cpp) || !ReadFile(dir + "/temporal.h", &header)) {
    Check(false, "temporal.cpp or temporal.h could not be read from " + dir);
    return;
  }
  Check(cpp.find("UseGameVehicleMotion(fr.view == temporal::View::kPause,") != std::string::npos &&
            cpp.find("larecomp::settings::mcla_gpu_pause_vehicle_vectors.Get()") != std::string::npos,
        "temporal.cpp builds the vehicle table only where UseGameVehicleMotion allows it (the pause view, unless the debug cvar)");
  Check(cpp.find("BuildVehicleTable(") != std::string::npos &&
            cpp.find("if (pending_.previous && temporal::UseGameVehicleMotion(") != std::string::npos,
        "the table is built inside that condition");
  Check(cpp.find("temporal::HeldJitter(") != std::string::npos && cpp.find("jitter_hold_.Held()") != std::string::npos,
        "the jitter of an armed frame goes through HeldJitter with the hold's state");
  Check(cpp.find("jitter_hold_.Observe(") != std::string::npos, "EndFrame tells the hold how each accepted frame went");
  Check(cpp.find("jitter_hold_.Clear()") != std::string::npos, "a restart of the run clears the hold");
  Check(cpp.find("& ~(kResetFirst | kResetView)") != std::string::npos,
        "the first frame of a spell and a change of view are not restarts of their own (they are plain frames)");
  Check(header.find("temporal::JitterHold jitter_hold_;") != std::string::npos, "temporal.h holds the JitterHold");
  Check(cpp.find("mcla_gpu_pause_vehicle_vectors") != std::string::npos && cpp.find("mcla_gpu_temporal_jitter_hold") != std::string::npos,
        "both debug cvars are defined");
  Check(cpp.find("ProbePauseSlots(f.banks->ps.data(), fr.stencil_ids)") != std::string::npos &&
            cpp.find("the game's vehicle slots in the pause view") != std::string::npos,
        "the pause view's unused slots are counted and the summary says what they held");
}

}  // namespace

int main(int argc, char** argv) {
  VehicleChecks();
  HoldChecks();
  if (argc > 1) {
    SourceChecks(argv[1]);
  } else {
    std::printf("(no src/gpu folder given: temporal.cpp was not read)\n");
  }
  std::printf("temporal_menu_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
