// Check of the guest frame's once-only state (guest_frame.h; LIGHT-STACK-REVIEW F2). A program of its own: no renderer, no ReXGlue, no GPU; exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_guest_frame_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_guest_frame_test [<src/gpu dir>]
//
// Standalone: clang++ -std=c++20 -I <repo>/src -I <repo>/src/gpu guest_frame_test.cpp
//
// Scene::BeginFrame runs for every list the renderer opens. The screen space reflections and the shared screen products made their per guest frame state in BeginFrame, so a second list opened
// after the frame's trigger (or after the frame was published) dropped it: the reflections' `ran` mark (OnPresent then broke the history and counted the frame twice) and the products'
// capture / history trade. The twins below are the two owners' frames written from ssr.cpp and screen_shared.cpp; the sources are pinned to them.

#include <cstdint>
#include <cstdio>
#include <string>

#include "guest_frame.h"

namespace gf = mcla::gpu::guest_frame;

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

std::string ReadFile(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    return std::string();
  }
  std::string s;
  char buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    s.append(buf, n);
  }
  std::fclose(f);
  return s;
}

std::string DirOf(const char* file) {
  const std::string s(file);
  const size_t at = s.find_last_of("/\\");
  return at == std::string::npos ? std::string(".") : s.substr(0, at);
}

bool Has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

// The text from `from` to the next `to` (a function's body when `to` is the next function's head).
std::string Between(const std::string& text, const char* from, const char* to) {
  const size_t a = text.find(from);
  if (a == std::string::npos) {
    return std::string();
  }
  const size_t b = text.find(to, a + 1);
  return text.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

// ---------------------------------------------------------------------------
// the gate
// ---------------------------------------------------------------------------

void TestGate() {
  gf::Gate gate;
  Check(gate.Begin() && !gate.worked() && gate.repeats() == 0, "a fresh gate starts the frame");
  // lists that open before any work (the empty list of an idle present, then the frame's own) all start afresh: an idle list must not make the real frame a repeat
  Check(gate.Begin() && gate.Begin() && gate.repeats() == 0, "lists before the work each start the frame afresh and are not counted");
  gate.Worked();
  Check(!gate.Begin() && gate.repeats() == 1 && !gate.Begin() && gate.repeats() == 2, "a list opened after the work keeps the state and is counted");
  gate.Present();
  Check(gate.Begin() && !gate.worked() && gate.repeats() == 2, "the present ends the guest frame: the next list starts afresh, the count stays");
  gate.Present();  // a present of a frame without work
  Check(gate.Begin(), "a present of a frame without work changes nothing");
  Check(!gf::LogDue(0) && gf::LogDue(1) && !gf::LogDue(2) && !gf::LogDue(9) && gf::LogDue(10) && !gf::LogDue(11) && !gf::LogDue(20) && gf::LogDue(100) && !gf::LogDue(101) && gf::LogDue(1000) &&
            !gf::LogDue(1100),
        "the repeats are logged at 1, 10, 100, 1000, ...");
}

// ---------------------------------------------------------------------------
// the screen space reflections' frame (ssr.cpp BeginFrame, OnTrigger, OnPresent)
// ---------------------------------------------------------------------------

struct SsrFrame {
  bool gated = true;           // false: what BeginFrame did before the fix
  gf::Gate guest;
  bool trigger_done = false;
  bool ran = false;
  bool history_valid = false;  // last_key_.valid
  uint64_t frames = 0;         // the noise's frame number
  void BeginFrame() {
    if (!gated || guest.Begin()) {
      trigger_done = false;
      ran = false;
      ++frames;
    }
  }
  void Trigger() {
    if (trigger_done) {
      return;
    }
    trigger_done = true;
    guest.Worked();
    ran = true;
    history_valid = true;
  }
  void Present() {
    if (!ran) {
      history_valid = false;  // a frame without the effect breaks the history
    }
    ran = false;
    guest.Present();
  }
};

void TestSsrFrame() {
  // the normal frame: one list, a trigger, the present: the history stays, each frame counts once
  SsrFrame normal;
  normal.BeginFrame();
  normal.Trigger();
  normal.Present();
  normal.BeginFrame();
  normal.Trigger();
  normal.Present();
  Check(normal.history_valid && normal.frames == 2, "one list a guest frame: the history stays and two frames count two");
  // the review's frame: the trigger in list 1, a second list before the present
  SsrFrame lost;
  lost.gated = false;
  lost.BeginFrame();
  lost.Trigger();
  lost.BeginFrame();  // a second list: `ran` is cleared, and no second trigger comes
  lost.Present();
  Check(!lost.history_valid && lost.frames == 2, "without the gate the second list clears `ran`, the present breaks the history and the frame counts twice (the bug)");
  SsrFrame gated;
  gated.BeginFrame();
  gated.Trigger();
  gated.BeginFrame();  // a second list
  gated.Trigger();     // the same guest frame: it does not run again
  gated.Present();
  Check(gated.history_valid && gated.frames == 1 && gated.guest.repeats() == 1, "with the gate the second list keeps the trigger's mark: the history stays, the frame counts once, the repeat is counted");
  // lists before the trigger, as an idle present's: the frame starts afresh and runs
  SsrFrame idle;
  idle.BeginFrame();  // the empty list of an idle present (no OnPresent follows it)
  idle.BeginFrame();  // the frame's own list
  idle.Trigger();
  idle.Present();
  Check(idle.history_valid && idle.frames == 2 && idle.guest.repeats() == 0, "an empty list before the frame's own does not make the frame a repeat");
  // the next frame after a frame with a repeat runs its own trigger
  gated.BeginFrame();
  gated.Trigger();
  gated.Present();
  Check(gated.history_valid && gated.frames == 2 && gated.guest.repeats() == 1, "the frame after a repeated one runs its own trigger");
}

// ---------------------------------------------------------------------------
// the shared screen products' frame (screen_shared.cpp BeginFrame, Publish, OnPresent)
// ---------------------------------------------------------------------------

struct ProductsFrame {
  bool gated = true;
  gf::Gate guest;
  bool published = false;
  bool post_done = false;
  uint64_t serial = 0;
  bool traded = false;  // OnPresent's history trade (`post_done_ && keep && post_[0].texture`)
  void BeginFrame(uint64_t scene_serial) {
    if (gated && !guest.Begin()) {
      return;
    }
    serial = scene_serial;
    published = false;
    post_done = false;
  }
  void PublishAndCapture() {
    published = true;
    guest.Worked();
    post_done = true;
  }
  void Present() {
    traded = post_done;
    guest.Present();
  }
};

void TestProductsFrame() {
  ProductsFrame normal;
  normal.BeginFrame(10);
  normal.PublishAndCapture();
  normal.Present();
  Check(normal.traded && normal.serial == 10, "one list: the capture is traded into the history at the present");
  ProductsFrame without;
  without.gated = false;
  without.BeginFrame(10);
  without.PublishAndCapture();
  without.BeginFrame(11);  // a second list
  without.Present();
  Check(!without.traded && !without.published, "without the gate a second list forgets the published frame and the capture: the history trade is skipped (the bug)");
  ProductsFrame gated;
  gated.BeginFrame(10);
  gated.PublishAndCapture();
  gated.BeginFrame(11);  // a second list
  gated.Present();
  Check(gated.traded && gated.published == true && gated.serial == 10 && gated.guest.repeats() == 1,
        "with the gate the second list keeps the frame (its serial too: the frame belongs to the first list) and the trade happens");
  gated.BeginFrame(12);
  Check(!gated.published && !gated.post_done && gated.serial == 12, "the next guest frame starts afresh with its own serial");
  // an idle present's empty list, then the frame
  ProductsFrame idle;
  idle.BeginFrame(20);  // the empty list
  idle.BeginFrame(21);  // the frame's own
  Check(idle.serial == 21 && idle.guest.repeats() == 0, "an empty list before the frame's own does not keep its serial");
}

// ---------------------------------------------------------------------------
// the sources
// ---------------------------------------------------------------------------

void TestSources(const std::string& dir) {
  const std::string ssr = ReadFile(dir + "/ssr.cpp");
  const std::string ssr_h = ReadFile(dir + "/ssr.h");
  const std::string shared = ReadFile(dir + "/screen_shared.cpp");
  const std::string shared_h = ReadFile(dir + "/screen_shared.h");
  if (ssr.empty() || ssr_h.empty() || shared.empty() || shared_h.empty()) {
    Check(false, "the sources could not be read (run the test with the src/gpu folder as its argument)");
    return;
  }
  const std::string ssr_begin = Between(ssr, "void ScreenReflections::BeginFrame() {", "\nvoid ScreenReflections::OnMainDraw(");
  Check(Has(ssr_begin, "if (guest_.Begin()) {\n    trigger_done_ = false;\n    ran_ = false;\n    wet_votes_.Reset();\n    ++frames_;\n  } else if (guest_frame::LogDue(guest_.repeats())) {") &&
            Has(ssr_begin, "BeginFrame ran twice in one guest frame") && ssr_begin.find("guest_.Begin()") < ssr_begin.find("Setting().Get()"),
        "ssr.cpp: BeginFrame resets the trigger, the ran mark, the wet votes and the frame count only for the first list of a guest frame, and logs the repeats (before it reads the settings)");
  Check(Has(ssr, "trigger_done_ = true;  // once a frame, whatever comes of it\n  guest_.Worked();") &&
            Has(Between(ssr, "void ScreenReflections::OnPresent() {", "\n}  // namespace"), "  ran_ = false;\n  guest_.Present();") && Has(ssr_h, "guest_frame::Gate guest_;") && Has(ssr_h, "#include \"guest_frame.h\""),
        "ssr.cpp: the trigger marks the frame worked, OnPresent ends the guest frame; ssr.h holds the gate");
  const std::string shared_begin = Between(shared, "void ScreenShared::BeginFrame(", "void ScreenShared::LogWants()");
  Check(Has(shared_begin, "if (!guest_.Begin()) {") && Has(shared_begin, "BeginFrame ran twice in one guest frame") && Has(shared_begin, "    return;\n  }\n  frame_serial_ = frame_serial;") &&
            shared_begin.find("guest_.Begin()") < shared_begin.find("published_ = false;"),
        "screen_shared.cpp: BeginFrame keeps the published frame, the capture and the history trade of a guest frame whose first list published it, and logs the repeats");
  Check(Has(shared, "  published_ = true;\n  guest_.Worked();") && Has(Between(shared, "void ScreenShared::OnPresent() {", "bool ScreenShared::IsTrigger("), "  trigger_seen_ = false;\n  guest_.Present();") &&
            Has(shared_h, "guest_frame::Gate guest_;") && Has(shared_h, "#include \"guest_frame.h\""),
        "screen_shared.cpp: Publish marks the frame worked, OnPresent ends the guest frame; screen_shared.h holds the gate");
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir = DirOf(__FILE__);
  if (argc > 1) {
    dir = argv[1];
  }
  TestGate();
  TestSsrFrame();
  TestProductsFrame();
  TestSources(dir);
  std::printf("guest_frame_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
