// Check of the JOIN of the shared rear view's three halves (perf/MIRROR-VIEW.md section 12): MV1 the host view (mirror_view_rules.h), MV2 the guest (hooks/rear_view_shared.h) and MV3 the glass finder's
// flag (mirror_glass_need.h), through the one link header (mirror_view_link.h). The halves have tests of their own (mirror_view_rules_test, rear_view_shared_test, mirror_glass_rules_test); this one
// pins what only the join decides:
//   1. ONE SWITCH: mcla_gpu_mirror_view drives both halves. With it off the guest sends no marker, asks for nothing, skips nothing and answers every hook as the game did; with it on, markers
//      and the host's phase tracker stay balanced however the switch flips in the middle of a frame.
//   2. THE REQUEST, once a frame: the host publishes redirect (and the side mirrors' need, MV3's flag with its hold) every frame; the guest's side-only phase and its skip of the camera step's clear
//      exist only while the request says redirect; the stamp follows the guest's one stamp flag.
//   3. THE RACERS join the side mirrors' view (the main session's decision of 2026-11-22, MIRROR-VIEW.md 11.5).
//   4. THE FILES: one link header, no second definition, one publisher of the request, one switch (the setting the guest read before, rearview_shared_view, is gone), the wiring lines.
//   5. JOIN2 (section 14): where the ray traced side mirrors win the request carries no need and no field (no side-only phase nobody reads); the side mirrors' wider field makes the round trip
//      host request -> guest camera -> CameraFacts -> host plan -> the stamp's crop, and the HUD mirror alone is bit for bit what it was.
// A program of its own: it needs no game, no ReXGlue and no renderer, and exits 0 when every check holds.
//
//   cmake --build <dir> --target larecomp_mirror_view_join_test, then run src/gpu/larecomp_mirror_view_join_test [<src dir>] (exit 0 = passed).
//   by hand (from the repo root): clang++ -std=c++23 -I src src/gpu/mirror_view_join_test.cpp -o mirror_view_join_test.exe

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "mc_engine/hooks/rear_view_shared.h"
#include "mirror_glass_need.h"
#include "mirror_view_link.h"
#include "mirror_view_rules.h"

namespace mv = mcla::gpu::mirror_view;
namespace mg = mcla::gpu::mirror_glass;
namespace rs = larecomp::rearview_shared;
namespace fs = std::filesystem;

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
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
size_t Count(const std::string& text, const std::string& part) {
  size_t n = 0;
  for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) ++n;
  return n;
}

// ---------------------------------------------------------------------------
// a two-half model: the guest's rules and the host's rules, wired the way the hooks and MirrorView wire them
// ---------------------------------------------------------------------------

// The state one frame of the join depends on.
struct World {
  int view = 0;              // mcla_gpu_mirror_view as the guest and NoteMirrorPhase read it live (0 off, 1 to 4 on)
  int view_at_frame = 0;     // ... as MirrorView::BeginFrame read it at the frame's start (mode_)
  bool hud = false;          // the player's rearview setting
  bool blocked = false;      // the host cannot make the view (shaders, images): MirrorView::Blocked()
  bool game_visible = true;  // the game's own visibility answers
  bool form_visible = true;  // the HUD form's visible bit
  bool consumer_active = true;
  bool held = false;         // the cadence holds this frame
};

struct Frame {
  // what the guest decided
  rs::Mode mode;
  mv::Request request;       // the request the guest read
  bool phase_asked = false;  // the guest keeps the mirror's bit this frame (TickRearView's want, minus a held frame)
  bool visible = false;      // the answer of the two visibility hooks
  bool side_only = false;
  bool camera_clear_skipped = false;
  bool racers = false;
  // what crossed the link
  std::vector<uint32_t> markers;  // in order, as the host received them
  // what the host did
  bool host_redirected = false;
  bool stamp_armed = false;
};

// What MirrorView::BeginFrame reads besides the switch, the blocked state and the finder's flag (JOIN2).
struct HostFacts {
  bool rt_side_mirrors = false;  // CarMirrorRt::active() as of the last frame's start (Scene passes it in)
  double retail_hfov_deg = 0.0;  // CameraFacts::retail_hfov_deg
  double stamp_aspect = 0.0;     // the HUD rectangle's width over height
  mv::Class cls = mv::Class::kMid;
};

// A host with the state it keeps between frames: the phase tracker, whether a picture is whole and how old it is. It receives the markers the way Renderer -> Scene -> MirrorView does.
struct Host {
  mv::PhaseTracker phase;
  bool whole = false;
  uint32_t age = 0;
  bool rect_valid = true;
  bool enabled = false;
  bool blocked = false;
  bool armed = false;  // MirrorView::stamp_armed_: set by an end marker, cleared at the frame's start

  // Scene::BeginFrame -> MirrorView::BeginFrame
  void BeginFrame(int view_at_frame, bool is_blocked, bool glass_need, const HostFacts& x = HostFacts{}) {
    enabled = view_at_frame != 0;
    blocked = is_blocked;
    mv::RequestFacts f;
    f.enabled = enabled;
    f.blocked = blocked;
    f.side_need = glass_need;
    f.rt_side_mirrors = x.rt_side_mirrors;
    f.hud_stamp = mv::StampWanted();
    f.cls = x.cls;
    f.retail_hfov_deg = x.retail_hfov_deg;
    f.stamp_aspect = x.stamp_aspect;
    mv::PublishRequest(mv::RequestFor(f));
    phase.FrameStart();
    armed = false;
    ++age;
  }
  // A marker out of the composite record. `drew`: the phase had draws (a real phase), not the empty pair of a held frame (MirrorView::EndPhase: the picture before stays and the stamp is armed from it).
  bool OnMarker(uint32_t marker, bool* redirected, bool drew = true) {
    if (mv::IsBeginMarker(marker)) {
      mv::BeginFacts b;
      b.enabled = enabled;
      b.rect_valid = rect_valid;
      b.plan_valid = true;
      b.blocked = blocked;
      b.hud_stamp = marker == mv::kMarkerBegin;
      phase_hud_stamp = b.hud_stamp;
      const bool r = phase.Begin(b) == mv::PhaseState::kRedirect;
      if (redirected && r) *redirected = true;
      return r;
    }
    if (phase.End()) {
      if (drew) {
        whole = true;
        age = 0;
      }
      armed = mv::StampDue(whole, age, rect_valid, phase_hud_stamp);
    }
    return true;
  }
  // The first copy of the main colour after the end marker: is the stamp drawn?
  bool StampNow() const { return armed; }
  bool phase_hud_stamp = false;
};

// NoteMirrorPhase (mirror_view.cpp): the begin carries the hook's HUD decision, and the end closes a begin that went.
struct Notes {
  bool open = false;
  // returns the marker sent, 0 for none
  uint32_t Note(bool begin, bool hud_stamp) {
    if (begin) {
      open = true;
      return hud_stamp ? mv::kMarkerBegin : mv::kMarkerBeginNoStamp;
    }
    if (open) {
      open = false;
      return mv::kMarkerEnd;
    }
    return 0;
  }
};

// One whole frame. `flip_to` >= 0 changes the setting after the guest has scheduled the phase.
Frame RunFrame(Host& host, Notes& notes, rs::MarkerTracker& tracker, const World& w, bool glass_need, int flip_to = -1, const HostFacts& hf = HostFacts{}) {
  Frame out;
  // host: the render thread's frame start. The side mirrors' need is MV3's flag, read through the real atomic.
  mg::PublishNeed(mg::NeedState{glass_need, uint8_t(glass_need ? 1 : 0), uint16_t(glass_need ? 240 : 0)});
  host.BeginFrame(w.view_at_frame, w.blocked, mg::SideNeed(), hf);

  // guest: TickRearView
  const bool shared = w.view != 0;
  out.request = mv::CurrentRequest();
  out.mode.shared = shared;
  out.mode.hud = w.hud;
  out.mode.side_need = rs::EffectiveSideNeed(shared, out.request);
  const bool want = rs::PhaseWanted(out.mode);
  out.side_only = want && rs::SideOnly(out.mode);
  if (shared) {
    mv::SetStampWanted(want ? rs::HudStamp(out.mode, w.form_visible) : false);
  } else {
    mv::SetStampWanted(true);
  }
  out.racers = rs::CarsInView(out.mode, false);
  // the visibility hooks: the phase is requested and the consumer active only when the answer is "visible"
  out.visible = rs::PhaseVisible(out.mode, w.game_visible);
  const bool bit = want && out.visible;
  out.phase_asked = bit && !w.held;
  const bool active = bit && w.consumer_active;  // (a held frame needs the game's bit, the guest's want and an active consumer: the cadence step returns early otherwise)

  // the phase loop: the camera step's clear (colour pass begin), the begin step, the end step
  bool redirected_now = false;
  const bool hud_stamp = rs::HudStamp(out.mode, w.form_visible);
  const bool view_at_begin = (flip_to >= 0 ? flip_to : w.view) != 0;
  if (out.phase_asked && active) {
    out.camera_clear_skipped = !hud_stamp || rs::SkipCameraClear(view_at_begin, out.request);
  }
  const uint32_t begin_marker = tracker.OnBegin(view_at_begin || !hud_stamp, out.phase_asked, active);
  if (begin_marker != 0) {
    const uint32_t sent = notes.Note(true, hud_stamp);
    if (sent != 0) {
      out.markers.push_back(sent);
      host.OnMarker(sent, &redirected_now);
    }
  }
  const uint32_t end_marker = tracker.OnEnd();
  if (end_marker != 0) {
    const uint32_t sent = notes.Note(false, false);
    if (sent != 0) {
      out.markers.push_back(sent);
      host.OnMarker(sent, nullptr);
    }
  }
  if (tracker.OnHeldFrame(view_at_begin, active && w.held)) {  // a held frame: the empty phase at the place the phase would have ended
    for (bool begin : {true, false}) {
      const uint32_t sent = notes.Note(begin, hud_stamp);
      if (sent != 0) {
        out.markers.push_back(sent);
        host.OnMarker(sent, begin ? &redirected_now : nullptr, false);
      }
    }
  }
  out.host_redirected = redirected_now;
  out.stamp_armed = host.StampNow();
  return out;
}

void ResetLink() {
  mv::WithdrawRequest();
  mv::SetStampWanted(true);
  mg::WithdrawNeed();
}

// ---------------------------------------------------------------------------
// 1. one switch
// ---------------------------------------------------------------------------

void TestSwitchOff() {
  // With the switch off the game's visible HUD mirror is left alone. A buffered hidden phase is still marked for suppression.
  int frames = 0;
  for (int bits = 0; bits < 128; ++bits) {
    World w;
    w.view = 0;
    w.view_at_frame = 0;
    w.hud = (bits & 1) != 0;
    w.blocked = (bits & 2) != 0;
    w.game_visible = (bits & 4) != 0;
    w.form_visible = (bits & 8) != 0;
    w.consumer_active = (bits & 16) != 0;
    w.held = (bits & 32) != 0;
    const bool glass = (bits & 64) != 0;
    ResetLink();
    Host host;
    Notes notes;
    rs::MarkerTracker tracker;
    const Frame f = RunFrame(host, notes, tracker, w, glass);
    ++frames;
    const std::string tag = " [off, hud " + std::to_string(w.hud) + " blocked " + std::to_string(w.blocked) + " game " + std::to_string(w.game_visible) + " form " + std::to_string(w.form_visible) +
                            " active " + std::to_string(w.consumer_active) + " held " + std::to_string(w.held) + " glass " + std::to_string(glass) + "]";
    const bool hidden_phase = f.phase_asked && w.consumer_active && !w.form_visible;
    Check(f.markers.size() == (hidden_phase ? 2 : 0), "only a buffered hidden phase is marked with the switch off" + tag);
    Check(!f.host_redirected && !host.whole && !f.stamp_armed, "the host redirects nothing and stamps nothing" + tag);
    Check(!f.request.redirect && !f.request.side_need, "the host's request says no redirect and no need (the finder's flag is not passed on)" + tag);
    Check(!f.mode.side_need && !f.side_only, "no side-only phase" + tag);
    Check(f.camera_clear_skipped == hidden_phase, "only a buffered hidden phase skips the camera step's clear with the switch off" + tag);
    Check(f.visible == (w.hud ? w.game_visible : false), "the visibility answers are the game's, or hidden with the HUD mirror off" + tag);
    Check(rs::PhaseWanted(f.mode) == w.hud && rs::FrameHidden(f.mode, w.form_visible) == !w.hud, "the phase and the frame follow the player's setting alone" + tag);
    Check(mv::StampWanted(), "the stamp flag is back at its default" + tag);
    Check(f.racers == false, "the racers follow the player's setting alone" + tag);
  }
  Check(frames == 128, "all 128 combinations ran");
}

void TestSwitchOn() {
  // Every combination with the switch on (the view at the class size, and the forced sizes).
  for (int view : {1, 3}) {
    for (int bits = 0; bits < 256; ++bits) {
      World w;
      w.view = view;
      w.view_at_frame = view;
      w.hud = (bits & 1) != 0;
      w.blocked = (bits & 2) != 0;
      w.game_visible = (bits & 4) != 0;
      w.form_visible = (bits & 8) != 0;
      w.consumer_active = (bits & 16) != 0;
      w.held = (bits & 32) != 0;
      const bool glass = (bits & 64) != 0;
      const bool prior_whole = (bits & 128) != 0;  // a picture from an earlier frame is held
      ResetLink();
      Host host;
      if (prior_whole) {
        host.whole = true;
        host.age = 1;
      }
      Notes notes;
      rs::MarkerTracker tracker;
      const Frame f = RunFrame(host, notes, tracker, w, glass);
      const std::string tag = " [view " + std::to_string(view) + " hud " + std::to_string(w.hud) + " blocked " + std::to_string(w.blocked) + " game " + std::to_string(w.game_visible) + " form " +
                              std::to_string(w.form_visible) + " active " + std::to_string(w.consumer_active) + " held " + std::to_string(w.held) + " glass " + std::to_string(glass) + " prior " +
                              std::to_string(prior_whole) + "]";
      // the request: redirect unless blocked, the need only with the redirect
      Check(f.request.redirect == !w.blocked, "the host redirects unless it is blocked" + tag);
      Check(f.request.side_need == (glass && !w.blocked), "the finder's need reaches the guest through the request, and only with a redirect" + tag);
      Check(f.mode.side_need == (glass && !w.blocked), "the guest's need is the request's" + tag);
      // the side-only phase exists only with a redirect and a need, and never with the HUD mirror on
      Check(f.side_only == (!w.hud && glass && !w.blocked), "the side-only phase: HUD off, a need, a redirect" + tag);
      // the markers are balanced, and a pair of them reaches the host only when the guest ran or held the phase
      Check(f.markers.size() % 2 == 0, "markers come in pairs" + tag);
      for (size_t i = 0; i < f.markers.size(); ++i) {
        const uint32_t begin = w.hud && w.form_visible ? mv::kMarkerBegin : mv::kMarkerBeginNoStamp;
        Check(f.markers[i] == (i % 2 == 0 ? begin : mv::kMarkerEnd), "markers carry the HUD decision and alternate BEGIN / END" + tag);
      }
      Check(host.phase.state == mv::PhaseState::kIdle && host.phase.unbalanced == 0, "the host's tracker ends the frame closed and balanced" + tag);
      const bool ran = !f.markers.empty() && !w.held;
      Check(f.camera_clear_skipped == (ran && (!w.blocked || !w.hud || !w.form_visible)), "the camera clear is skipped for a redirect or a hidden HUD" + tag);
      if (ran) {
        Check(f.host_redirected == !w.blocked, "the host redirects a marked phase unless it is blocked" + tag);
      }
      // the stamp: for the HUD mirror only, from the picture of this frame, or of the last whole one on a held frame
      if (f.side_only) {
        Check(!mv::StampWanted() && !f.stamp_armed, "a side-only phase says no stamp, and none is armed" + tag);
      }
      if (w.hud && w.form_visible && f.phase_asked && w.consumer_active && !w.blocked && f.visible) {
        Check(f.stamp_armed, "the HUD mirror's phase is stamped" + tag);
      }
      if (w.hud && !w.form_visible) {
        Check(!mv::StampWanted(), "the HUD form hidden: no stamp" + tag);
      }
      if (w.hud && w.form_visible && w.held && w.consumer_active && f.visible) {
        Check(f.markers.size() == 2, "a held frame sends the empty pair" + tag);
        Check(f.stamp_armed == (prior_whole && !w.blocked), "... and the kept picture is stamped when there is one" + tag);
      }
    }
  }
}

void TestFlipsInTheMiddle() {
  // The switch flips after the guest schedules the phase. The host's tracker never sees an unbalanced phase.
  uint32_t seed = 2026u;
  auto next = [&seed]() {
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 13) & 0xFFFFu;
  };
  ResetLink();
  Host host;
  Notes notes;
  rs::MarkerTracker tracker;
  int begins = 0, ends = 0, redirected = 0;
  bool balanced = true;
  bool open_at_end = false;
  for (int i = 0; i < 20000; ++i) {
    World w;
    w.view = int(next() % 3) == 0 ? 0 : 1 + int(next() % 4);
    w.view_at_frame = int(next() % 4) == 0 ? 0 : w.view;
    w.hud = (next() & 1) != 0;
    w.blocked = (next() % 5) == 0;
    w.game_visible = (next() % 4) != 0;
    w.form_visible = (next() % 4) != 0;
    w.consumer_active = (next() % 8) != 0;
    w.held = (next() % 4) == 0;
    const bool glass = (next() % 3) == 0;
    const int flip = (next() % 3) == 0 ? int(next() % 2) * 2 : -1;
    const Frame f = RunFrame(host, notes, tracker, w, glass, flip);
    for (uint32_t m : f.markers) (mv::IsBeginMarker(m) ? begins : ends)++;
    if (f.markers.size() % 2 != 0) balanced = false;
    if (host.phase.state != mv::PhaseState::kIdle || notes.open) open_at_end = true;
    redirected += f.host_redirected ? 1 : 0;
  }
  Check(balanced, "random frames with the switch flipping mid-frame: the markers always come in BEGIN / END pairs");
  Check(!open_at_end, "and no phase or note is left open at a frame's end");
  Check(host.phase.unbalanced == 0, "the host's tracker saw no begin without an end and no end without a begin");
  Check(begins == ends && begins > 1000 && redirected > 500, "the run really exercised the join (" + std::to_string(begins) + " pairs, " + std::to_string(redirected) + " redirected)");
}

// ---------------------------------------------------------------------------
// 2. the request, once a frame, and MV3's flag with its hold
// ---------------------------------------------------------------------------

void TestQueuedHudEligibility() {
  ResetLink();
  Host host;
  host.BeginFrame(1, true, false, HostFacts{});
  host.OnMarker(mv::kMarkerBeginNoStamp, nullptr);
  Check(host.phase.suppressing(), "an unavailable view discards the queued hidden-HUD phase even when the latest HUD flag is on");
  host.OnMarker(mv::kMarkerEnd, nullptr);
  Check(!host.StampNow() && !host.phase.suppressing(), "the suppressed phase ends without a stamp");

  host.BeginFrame(1, false, false, HostFacts{});
  Check(host.OnMarker(mv::kMarkerBeginNoStamp, nullptr), "a ready view redirects the same phase");
  host.OnMarker(mv::kMarkerEnd, nullptr);
  Check(!host.StampNow(), "a redirected hidden-HUD phase does not stamp even when the latest HUD flag is on");

  mv::SetStampWanted(false);
  host.BeginFrame(1, false, false, HostFacts{});
  host.OnMarker(mv::kMarkerBegin, nullptr);
  host.OnMarker(mv::kMarkerEnd, nullptr, false);
  Check(host.StampNow(), "a held visible-HUD phase stamps the held picture even when the latest HUD flag is off");
  ResetLink();
}

void TestRequestEveryFrame() {
  ResetLink();
  Host host;
  // The request is published in every host frame, and the guest's Current follows the switch from one frame to the next: a request is never kept.
  host.BeginFrame(1, false, false);
  Check(mv::CurrentRequest().redirect, "frame 1: the switch is on: redirect");
  host.BeginFrame(0, false, true);
  Check(!mv::CurrentRequest().redirect && !mv::CurrentRequest().side_need, "frame 2: the switch went off: no redirect, no need");
  host.BeginFrame(2, true, true);
  Check(!mv::CurrentRequest().redirect && !mv::CurrentRequest().side_need, "frame 3: blocked: no redirect, no need (the guest runs no side-only phase, keeps the game's camera clear)");
  host.BeginFrame(4, false, true);
  Check(mv::CurrentRequest().redirect && mv::CurrentRequest().side_need, "frame 4: unblocked, a need: both");
  mv::WithdrawRequest();
  Check(mv::CurrentRequest() == mv::Request{}, "the renderer's shutdown withdraws it");

  // MV3's flag with its hold: a mirror in view for 3 frames raises the guest's need at once, and it falls 20 frames after the last frame that had one.
  ResetLink();
  mg::NeedHold hold;
  Host h2;
  int need_frames = 0, last_need_frame = -1;
  for (int frame = 0; frame < 60; ++frame) {
    const mg::NeedState now{frame < 3, uint8_t(frame < 3 ? 1 : 0), uint16_t(frame < 3 ? 200 : 0)};
    mg::PublishNeed(hold.Update(now));  // MirrorGlass::OnPresent
    h2.BeginFrame(1, false, mg::SideNeed());  // the next frame's MirrorView::BeginFrame
    if (mv::CurrentRequest().side_need) {
      ++need_frames;
      last_need_frame = frame;
    }
  }
  Check(need_frames == 3 + int(mg::kNeedHoldFrames) && last_need_frame == 2 + int(mg::kNeedHoldFrames), "the request's need follows MV3's flag: 3 frames and the 20 frame hold, then gone");
  // MV3 off (the finder's setting off): its flag is withdrawn and the request carries no need
  ResetLink();
  mg::PublishNeed(mg::NeedState{true, 1, 200});
  mg::WithdrawNeed();
  Host h3;
  h3.BeginFrame(1, false, mg::SideNeed());
  Check(mv::CurrentRequest().redirect && !mv::CurrentRequest().side_need, "the finder off: a redirect with no need");
  ResetLink();
}

void TestSideOnlyEndToEnd() {
  // The HUD mirror off, the view on, the finder finds a pair: the phase runs for the side mirrors, bracketed, redirected, with no stamp. Then the pair goes: the phase stops after the hold.
  ResetLink();
  Host host;
  Notes notes;
  rs::MarkerTracker tracker;
  mg::NeedHold hold;
  World w;
  w.view = 1;
  w.view_at_frame = 1;
  w.hud = false;
  w.game_visible = false;  // the game's own HUD mirror is hidden
  int phases = 0, stamps = 0, skipped = 0;
  int first_phase = -1, last_phase = -1;
  for (int frame = 0; frame < 80; ++frame) {
    const bool need_now = frame >= 5 && frame < 15;
    // MirrorGlass::OnPresent: the finder's frame goes through the hold; the model has the request follow it in the same frame (the real chain is one to two frames later)
    const mg::NeedState held = hold.Update(mg::NeedState{need_now, uint8_t(need_now ? 1 : 0), uint16_t(need_now ? 240 : 0)});
    const Frame f = RunFrame(host, notes, tracker, w, held.need());
    if (f.markers.size() == 2 && f.host_redirected) {
      ++phases;
      if (first_phase < 0) first_phase = frame;
      last_phase = frame;
    }
    if (f.stamp_armed) ++stamps;
    if (f.camera_clear_skipped) ++skipped;
  }
  Check(first_phase == 5, "the phase starts in the frame the need is up: frame " + std::to_string(first_phase));
  Check(last_phase == 14 + int(mg::kNeedHoldFrames), "and runs on through the 20 frame hold: last phase at frame " + std::to_string(last_phase));
  Check(phases == 10 + int(mg::kNeedHoldFrames), "one marked, redirected phase in every frame of the need and of the hold: " + std::to_string(phases));
  Check(stamps == 0, "no stamp is ever drawn for a phase the side mirrors alone asked for");
  Check(skipped == phases, "the camera step's clear is skipped in every phase that ran, and in no other frame");
  ResetLink();
}

// ---------------------------------------------------------------------------
// 3. the racers
// ---------------------------------------------------------------------------

void TestRacersJoin() {
  // Decided 2026-11-22 (MIRROR-VIEW.md 11.5): the racers join the side mirrors' view: a side mirror that never shows the car alongside is not a mirror.
  const rs::Mode side_only{true, false, true};
  const rs::Mode both{true, true, true};
  const rs::Mode hud_only{true, true, false};
  const rs::Mode off{false, true, true};
  Check(rs::CarsInView(side_only, false), "side mirrors' view only, CARS IN REAR VIEW off: the racers are in");
  Check(rs::CarsInView(both, false), "one view, two readers: the racers are in the HUD stamp too");
  Check(!rs::CarsInView(hud_only, false) && rs::CarsInView(hud_only, true), "the HUD mirror alone: the player's setting");
  Check(!rs::CarsInView(off, false) && rs::CarsInView(off, true), "the switch off: the player's setting");
}

// ---------------------------------------------------------------------------
// 5. JOIN2: the ray traced side mirrors win; the field's round trip; the HUD mirror as it was
// ---------------------------------------------------------------------------

bool SameFrame(const Frame& a, const Frame& b) {
  return a.request == b.request && a.mode.shared == b.mode.shared && a.mode.hud == b.mode.hud && a.mode.side_need == b.mode.side_need && a.phase_asked == b.phase_asked && a.visible == b.visible &&
         a.side_only == b.side_only && a.camera_clear_skipped == b.camera_clear_skipped && a.racers == b.racers && a.markers == b.markers && a.host_redirected == b.host_redirected &&
         a.stamp_armed == b.stamp_armed;
}

void TestRayTracedMirrorsWin() {
  // Where CarMirrorRt is active the glass finder's need is nobody's: the request says no need (and no field), so the guest runs no side-only phase, adds no racers and changes nothing the HUD mirror's
  // own phase did. Every combination of the world: the frame with the finder up is the frame with the finder down.
  int compared = 0, controls = 0;
  for (int view : {1, 3}) {
    for (int bits = 0; bits < 128; ++bits) {
      World w;
      w.view = view;
      w.view_at_frame = view;
      w.hud = (bits & 1) != 0;
      w.blocked = (bits & 2) != 0;
      w.game_visible = (bits & 4) != 0;
      w.form_visible = (bits & 8) != 0;
      w.consumer_active = (bits & 16) != 0;
      w.held = (bits & 32) != 0;
      const bool prior_whole = (bits & 64) != 0;
      HostFacts rt;
      rt.rt_side_mirrors = true;
      rt.retail_hfov_deg = 100.0;
      rt.stamp_aspect = 229.0 / 44.0;
      Frame frames[2];
      for (int glass = 0; glass < 2; ++glass) {
        ResetLink();
        Host host;
        host.whole = prior_whole;
        host.age = prior_whole ? 1 : 0;
        Notes notes;
        rs::MarkerTracker tracker;
        frames[glass] = RunFrame(host, notes, tracker, w, glass != 0, -1, rt);
      }
      const std::string tag = " [view " + std::to_string(view) + " hud " + std::to_string(w.hud) + " blocked " + std::to_string(w.blocked) + " game " + std::to_string(w.game_visible) + " form " +
                              std::to_string(w.form_visible) + " active " + std::to_string(w.consumer_active) + " held " + std::to_string(w.held) + " prior " + std::to_string(prior_whole) + "]";
      Check(SameFrame(frames[0], frames[1]), "RT mirrors win: a frame with the finder's need up is the frame with it down, in everything the guest and the host do" + tag);
      const Frame& f = frames[1];
      Check(!f.request.side_need && !f.request.has_field() && f.request.redirect == !w.blocked, "the request: redirect as ever, no need, no field" + tag);
      Check(!f.mode.side_need && !f.side_only && !f.racers, "no side-only phase, no racers added" + tag);
      if (!w.hud) {
        Check(!f.phase_asked && f.markers.empty() && !f.host_redirected && !f.stamp_armed, "the HUD mirror off: the guest asks for no phase nobody reads, no marker, no redirect" + tag);
      }
      ++compared;
      // the control: the same frame with the RT mirrors off does ask for the side-only phase (the test is not vacuous)
      if (!w.hud && !w.blocked && w.consumer_active && !w.held) {
        ResetLink();
        Host host;
        Notes notes;
        rs::MarkerTracker tracker;
        const Frame c = RunFrame(host, notes, tracker, w, true, -1, HostFacts{});
        Check(c.request.side_need && c.side_only && c.phase_asked && c.markers.size() == 2 && c.host_redirected && !c.stamp_armed, "control: without the RT mirrors the side-only phase runs, bracketed and redirected" + tag);
        ++controls;
      }
    }
  }
  Check(compared == 256 && controls == 16, "all combinations ran, and the 16 that can are controls (" + std::to_string(compared) + ", controls " + std::to_string(controls) + ")");
  ResetLink();
}

void TestRayTracedMirrorsComeAndGo() {
  // The RT side mirrors are switched on and off live: the request follows at the next frame, the guest's side-only phase starts and stops with it.
  ResetLink();
  Host host;
  Notes notes;
  rs::MarkerTracker tracker;
  World w;
  w.view = 1;
  w.view_at_frame = 1;
  w.hud = false;
  w.game_visible = false;
  HostFacts rt;
  int phases_without = 0, phases_with = 0;
  for (int frame = 0; frame < 30; ++frame) {
    rt.rt_side_mirrors = frame >= 10 && frame < 20;
    const Frame f = RunFrame(host, notes, tracker, w, true, -1, rt);
    const bool ran = f.markers.size() == 2;
    (rt.rt_side_mirrors ? phases_with : phases_without) += ran ? 1 : 0;
    Check(ran == !rt.rt_side_mirrors, "frame " + std::to_string(frame) + ": the side-only phase runs exactly while the RT mirrors are off");
  }
  Check(phases_with == 0 && phases_without == 20, "20 phases with the RT mirrors off, none with them on");
  ResetLink();
}

// The camera the guest's hook reports for a request (Patch_RearViewCamera): what BuildCamera builds, and the facts it publishes (the 'from the host's request' bit included, MV-J2).
mv::CameraFacts GuestCamera(float game_fov_deg, float game_aspect, const mv::Request& req, double fov_scale = 1.0) {
  const rs::CameraBuild cam = rs::BuildCamera(game_fov_deg, game_aspect, 1500.0f, true, req, fov_scale, 0.0);
  mv::CameraFacts facts;
  facts.retail_hfov_deg = rs::HudFieldDeg(game_fov_deg, game_aspect, fov_scale);
  facts.used_hfov_deg = rs::HorizontalFovDeg(cam.fov_deg, cam.aspect);
  facts.used_vfov_deg = cam.fov_deg;
  facts.from_request = cam.applied;
  facts.seq = 1;
  mv::PublishCamera(facts);  // through the link, as the hook does: packed to the centidegree
  return mv::CurrentCamera();
}

// What the phase's begin marker carries for a camera (NoteMirrorPhase(true)): the field of that camera, through the composite record's second word.
mv::PhaseField TagOf(const mv::CameraFacts& cam) { return mv::UnpackPhaseField(mv::PackPhaseField(mv::PhaseFieldOf(cam))); }

mv::Rig RigOf(double hfov_deg, double vfov_deg) {
  mv::Rig r;
  r.tan_half_h = std::tan(hfov_deg * 0.5 * 3.14159265358979323846 / 180.0);
  r.tan_half_v = std::tan(vfov_deg * 0.5 * 3.14159265358979323846 / 180.0);
  return r;
}

void TestFieldRoundTrip() {
  const float game_aspect = 229.0f / 44.0f;
  const float game_fov = 26.0f;  // the design's guess for the retail field: about 100 across the 229x44 stamp (CHECK 11)
  // 1. the side mirrors alone: the field is asked for at once (no retail field is needed), the guest builds it, the begin marker tags the phase with it, the host sizes the view by it
  for (mv::Class c : {mv::Class::kLow, mv::Class::kMid, mv::Class::kHigh}) {
    ResetLink();
    mv::WithdrawCamera();
    Host host;
    HostFacts hf;
    hf.cls = c;
    hf.stamp_aspect = game_aspect;
    mv::SetStampWanted(false);  // the HUD form is hidden: the guest's flag
    host.BeginFrame(1, false, true, hf);
    const mv::Request asked = mv::CurrentRequest();
    Check(asked.redirect && asked.side_need && asked.has_field() && std::fabs(asked.hfov_deg - 69.0f) < 1e-3f && std::fabs(asked.vfov_deg - 44.0f) < 1e-3f, "side mirrors alone: 69 x 44 asked at once");
    const mv::CameraFacts cam = GuestCamera(game_fov, game_aspect, asked);
    const mv::PhaseField tag = TagOf(cam);
    Check(tag.requested && tag.known() && std::fabs(tag.hfov_deg - asked.hfov_deg) < 0.0151f && std::fabs(tag.vfov_deg - asked.vfov_deg) < 0.0151f, "the guest built it and the begin marker says so, with the field it asked for");
    const mv::Plan plan = mv::PlanForPhase(c, 229.0, 44.0, tag);
    const mv::Plan field = mv::PlanForField(c, double(asked.hfov_deg), double(asked.vfov_deg));
    Check(plan.width == field.width && plan.height == field.height && plan.hfov_deg > 60.0, "the view is sized by the field");
    const double camera_aspect = std::tan(double(cam.used_hfov_deg) * 0.5 * 3.14159265358979323846 / 180.0) / std::tan(double(cam.used_vfov_deg) * 0.5 * 3.14159265358979323846 / 180.0);
    Check(std::fabs(double(plan.width) / double(plan.height) - camera_aspect) / camera_aspect < 0.01, "and its pixels' aspect is the camera's, not the 229 by 44 rectangle's (" + std::to_string(plan.width) + "x" + std::to_string(plan.height) + ")");
  }
  // 2. the side mirrors with the HUD stamp: frame 1 has no retail field (nothing asked, the game's own serves both), frame 2 asks 100 x 44, the HUD's crop of it is the game's own picture
  for (mv::Class c : {mv::Class::kLow, mv::Class::kMid, mv::Class::kHigh}) {
    ResetLink();
    mv::WithdrawCamera();
    Host host;
    HostFacts hf;
    hf.cls = c;
    hf.stamp_aspect = game_aspect;
    mv::SetStampWanted(true);
    host.BeginFrame(1, false, true, hf);  // no camera built yet
    const mv::Request first = mv::CurrentRequest();
    Check(first.redirect && first.side_need && !first.has_field(), "frame 1, the HUD stamp on and no retail field yet: the need, no field");
    mv::CameraFacts cam = GuestCamera(game_fov, game_aspect, first);
    const mv::PhaseField tag1 = TagOf(cam);
    Check(tag1.known() && !tag1.requested && cam.retail_hfov_deg > 90.0f, "the guest built the game's own field and reported the retail one: the phase's tag is known, not requested");
    const mv::Plan plan1 = mv::PlanForPhase(c, 229.0, 44.0, tag1);
    const mv::Plan rect_plan = mv::PlanForRect(c, 229.0, 44.0);
    Check(plan1.width == rect_plan.width && plan1.height == rect_plan.height && plan1.hfov_deg == 0.0, "so the view is the rectangle's, as the HUD mirror always had");
    Check(mv::HudCrop(plan1, double(cam.retail_hfov_deg), double(game_aspect)).u1 == 1.0 && mv::HudCrop(plan1, double(cam.retail_hfov_deg), double(game_aspect)).v0 == 0.0, "and the stamp shows all of it");
    hf.retail_hfov_deg = double(cam.retail_hfov_deg);
    host.BeginFrame(1, false, true, hf);  // the next frame: the retail field is known
    const mv::Request second = mv::CurrentRequest();
    Check(second.has_field() && second.hfov_deg >= cam.retail_hfov_deg - 0.01f && std::fabs(second.vfov_deg - 44.0f) < 1e-3f, "frame 2: a field that holds the retail HUD field across and 44 up");
    // the guest has not built a camera with it yet: a phase of the old camera is still the rectangle's plan (no stretched picture), whatever the host has asked for since
    Check(mv::PlanForPhase(c, 229.0, 44.0, TagOf(cam)).hfov_deg == 0.0, "until the guest builds a camera with it, the phases' tags and plans stay the rectangle's");
    cam = GuestCamera(game_fov, game_aspect, second);
    const mv::PhaseField tag2 = TagOf(cam);
    Check(tag2.requested, "the guest builds it: the phase's tag says requested");
    const mv::Plan plan2 = mv::PlanForPhase(c, 229.0, 44.0, tag2);
    Check(plan2.hfov_deg > 90.0 && plan2.width == mv::PlanForField(c, double(second.hfov_deg), double(second.vfov_deg)).width, "the view is sized by the field");
    // the stamp: the crop of the wider view is the game's own picture: for directions inside the retail frustum, the picture coordinate in the crop equals the one the game's own camera gives
    const double retail = double(cam.retail_hfov_deg);
    const mv::Crop crop = mv::HudCrop(plan2, retail, double(game_aspect));
    const mv::Rig view_rig = RigOf(double(cam.used_hfov_deg), double(cam.used_vfov_deg));
    const mv::Rig hud_rig = RigOf(retail, mv::StampVerticalFov(retail, double(game_aspect)));
    bool exact = true;
    for (int i = -4; i <= 4; ++i) {
      for (int j = -4; j <= 4; ++j) {
        const mv::V3 dir = mv::Normalize(mv::V3{-1.0, 0.25 * i * hud_rig.tan_half_h, 0.25 * j * hud_rig.tan_half_v});
        const mv::Uv a = mv::ProjectDirection(view_rig, dir), b = mv::ProjectDirection(hud_rig, dir);
        const double lu = (a.u - crop.u0) / (crop.u1 - crop.u0), lv = (a.v - crop.v0) / (crop.v1 - crop.v0);
        exact = exact && a.ok && b.ok && std::fabs(lu - b.u) < 2e-3 && std::fabs(lv - b.v) < 2e-3;  // the link carries the field to a centidegree
      }
    }
    Check(exact, "the HUD stamp's crop of the wider view shows what the game's own camera showed (class " + std::to_string(int(c)) + ")");
    // the need goes: the request has no field, the guest goes back to the game's own camera, the plan is the rectangle's again
    host.BeginFrame(1, false, false, hf);
    const mv::Request gone = mv::CurrentRequest();
    Check(gone.redirect && !gone.side_need && !gone.has_field(), "the need gone: no field");
    cam = GuestCamera(game_fov, game_aspect, gone);
    const mv::Plan back = mv::PlanForPhase(c, 229.0, 44.0, TagOf(cam));
    Check(back.width == rect_plan.width && back.height == rect_plan.height && back.hfov_deg == 0.0, "the view is the rectangle's again");
  }
  // 3. the HUD mirror alone: nothing is ever asked, the camera is the game's own, the plan the rectangle's, the stamp whole: bit for bit what it was
  for (mv::Class c : {mv::Class::kLow, mv::Class::kMid, mv::Class::kHigh}) {
    ResetLink();
    mv::WithdrawCamera();
    Host host;
    HostFacts hf;
    hf.cls = c;
    hf.stamp_aspect = game_aspect;
    mv::SetStampWanted(true);
    for (int frame = 0; frame < 3; ++frame) {
      host.BeginFrame(1, false, false, hf);
      const mv::Request r = mv::CurrentRequest();
      Check(r.redirect && !r.side_need && !r.has_field() && r.every == 1, "HUD mirror alone: a redirect and nothing else");
      const mv::CameraFacts cam = GuestCamera(game_fov, game_aspect, r);
      hf.retail_hfov_deg = double(cam.retail_hfov_deg);
      const mv::PhaseField tag = TagOf(cam);
      Check(tag.known() && !tag.requested, "the game's own field, known, not requested");
      const mv::Plan plan = mv::PlanForPhase(c, 229.0, 44.0, tag);
      const mv::Plan rect_plan = mv::PlanForRect(c, 229.0, 44.0);
      Check(plan.width == rect_plan.width && plan.height == rect_plan.height && plan.hfov_deg == 0.0 && plan.vfov_deg == 0.0 && plan.every == rect_plan.every, "the view is PlanForRect's, bit for bit");
      const mv::Crop crop = mv::HudCrop(plan, double(cam.retail_hfov_deg), double(game_aspect));
      Check(crop.u0 == 0.0 && crop.v0 == 0.0 && crop.u1 == 1.0 && crop.v1 == 1.0, "the stamp shows the whole picture");
    }
  }
  // 4. every request the host can make is one the guest applies and tags (the handshake never stalls): the guest believes the field, the link carries it with the phase's begin marker, the host plans by it
  int asked = 0;
  bool all_applied = true, all_tagged = true, all_planned = true;
  for (mv::Class c : {mv::Class::kLow, mv::Class::kMid, mv::Class::kHigh}) {
    for (bool hud : {false, true}) {
      for (double retail = 20.0; retail <= 130.0; retail += 1.5) {
        for (double aspect : {5.2045, 4.0, 3.5, 6.5, 2.0}) {
          mv::RequestFacts f;
          f.enabled = true;
          f.side_need = true;
          f.hud_stamp = hud;
          f.cls = c;
          f.retail_hfov_deg = retail;
          f.stamp_aspect = aspect;
          const mv::Request r = mv::UnpackRequest(mv::Pack(mv::RequestFor(f)));  // the link
          if (!r.has_field()) continue;
          ++asked;
          const rs::Camera cam = rs::SharedCamera(26.0f, game_aspect, r);
          all_applied = all_applied && cam.applied;
          mv::CameraFacts facts;
          facts.used_hfov_deg = rs::HorizontalFovDeg(cam.fov_deg, cam.aspect);
          facts.used_vfov_deg = cam.fov_deg;
          facts.from_request = cam.applied;
          const mv::PhaseField tag = TagOf(mv::UnpackCamera(mv::Pack(facts)));
          all_tagged = all_tagged && tag.requested && std::fabs(tag.hfov_deg - r.hfov_deg) <= 0.0151f && std::fabs(tag.vfov_deg - r.vfov_deg) <= 0.0151f;
          const mv::Plan plan = mv::PlanForPhase(c, 229.0, 44.0, tag);
          const mv::Plan want = mv::PlanForField(c, double(r.hfov_deg), double(r.vfov_deg));
          all_planned = all_planned && plan.valid() && plan.width == want.width && std::abs(int(plan.height) - int(want.height)) <= 2;
        }
      }
    }
  }
  Check(asked > 300, "the sweep made many requests with a field (" + std::to_string(asked) + ")");
  Check(all_applied, "the guest applies every field the host asks for");
  Check(all_tagged, "and its begin marker names every one of them as the field asked for, after the camera facts' and the tag's round trips through the link");
  Check(all_planned, "and the host's plan by the tag is the plan by the request (the same width, the height to the pixel pair)");
  ResetLink();
  mv::WithdrawCamera();
}

// ---------------------------------------------------------------------------
// 3b. MV-J2: the guest runs ahead of the render thread; a phase's size, its stamp's crop and the overlay's lookup follow the field of ITS camera
// ---------------------------------------------------------------------------

// The old rule (MIRROR-VIEW.md 15.1: PlanForView / FieldApplied, removed with MV-J2), kept here as a model of what the tag replaced: the host's request of this host frame against the guest's LATEST
// camera facts. Where the two disagreed the plan fell back to the rectangle's. The test counts how often that was wrong for a guest `lag` frames ahead.
mv::Plan OldPlanForView(mv::Class c, double rect_w, double rect_h, const mv::Request& req, const mv::CameraFacts& cam) {
  const bool applied = req.has_field() && cam.valid() && std::fabs(double(req.hfov_deg) - double(cam.used_hfov_deg)) <= 0.05 && std::fabs(double(req.vfov_deg) - double(cam.used_vfov_deg)) <= 0.05;
  return applied ? mv::PlanForField(c, double(req.hfov_deg), double(req.vfov_deg)) : mv::PlanForRect(c, rect_w, rect_h);
}

double CameraAspect(const mv::CameraFacts& cam) {
  return std::tan(double(cam.used_hfov_deg) * 0.5 * 3.14159265358979323846 / 180.0) / std::tan(double(cam.used_vfov_deg) * 0.5 * 3.14159265358979323846 / 180.0);
}

// Frames of a session: the finder's need and the HUD stamp come and go (random runs of frames); the guest builds one camera and sends one tagged begin per frame, `lag` frames before the host handles that
// phase; the host's request of frame j is published at its start. For every phase the host handles, the picture's truth is the camera that phase was built with.
void TestFieldLag() {
  const float game_aspect = 229.0f / 44.0f;
  const float game_fov = 26.0f;
  struct Lcg {
    uint64_t s;
    uint32_t Next() {
      s = s * 6364136223846793005ull + 1442695040888963407ull;
      return uint32_t(s >> 33);
    }
  };
  int phases = 0, changes = 0;
  int new_wrong_plan = 0, new_wrong_field = 0, old_wrong_plan = 0, old_wrong_field = 0, old_wrong_at_lag0 = 0;
  for (mv::Class c : {mv::Class::kLow, mv::Class::kMid, mv::Class::kHigh}) {
    for (int lag = 0; lag <= 3; ++lag) {
      for (uint64_t seed = 1; seed <= 6; ++seed) {
        Lcg rng{seed * 7919u + uint64_t(lag) * 104729u + uint64_t(c) * 31u};
        const int frames = 400;
        // the world per guest frame: the finder's need (held in runs) and the HUD stamp flag (the guest's own)
        std::vector<bool> need(size_t(frames + 8)), hud(size_t(frames + 8));
        bool n = false, h = true;
        for (int i = 0; i < frames + 8; ++i) {
          if (rng.Next() % 23 == 0) n = !n;
          if (rng.Next() % 31 == 0) h = !h;
          need[size_t(i)] = n;
          hud[size_t(i)] = h;
        }
        // the host's request at each host frame j (RequestFor, as MirrorView::BeginFrame): it follows the need of j and the guest's flag as of when the host looks (the guest is `lag` frames ahead)
        std::vector<mv::Request> request(size_t(frames + 8));
        std::vector<mv::CameraFacts> camera(size_t(frames + 8));
        std::vector<uint32_t> tag(size_t(frames + 8));
        // the guest's camera build for guest frame i reads the request of the host frame the render thread has reached when the guest is at i: host frame i - lag
        // (the host processes guest frame i's phase at host frame i)
        double retail = 0.0;
        for (int j = 0; j < frames; ++j) {
          mv::SetStampWanted(hud[size_t(std::min(j + lag, frames + 7))]);
          mv::RequestFacts f;
          f.enabled = true;
          f.side_need = need[size_t(j)];
          f.hud_stamp = mv::StampWanted();
          f.cls = c;
          f.retail_hfov_deg = retail;
          f.stamp_aspect = double(game_aspect);
          request[size_t(j)] = mv::UnpackRequest(mv::Pack(mv::RequestFor(f)));
          // the guest's frame j + lag has not been built yet for the host at j, but guest frames up to j + lag already are: build them in order here, each from the request the host had published when the guest
          // reached it (host frame i - lag)
          const int i = j + lag;
          if (i < frames) {
            const mv::Request seen = request[size_t(std::max(0, i - lag))];
            const rs::CameraBuild b = rs::BuildCamera(game_fov, game_aspect, 1500.0f, true, seen, 1.0, 0.0);
            mv::CameraFacts facts;
            facts.retail_hfov_deg = rs::HudFieldDeg(game_fov, game_aspect, 1.0);
            facts.used_hfov_deg = rs::HorizontalFovDeg(b.fov_deg, b.aspect);
            facts.used_vfov_deg = b.fov_deg;
            facts.from_request = b.applied;
            facts.seq = uint32_t(i & 0xFF);
            mv::PublishCamera(facts);
            camera[size_t(i)] = mv::CurrentCamera();
            tag[size_t(i)] = mv::PackPhaseField(mv::PhaseFieldOf(camera[size_t(i)]));  // NoteMirrorPhase(true) at guest frame i
            retail = double(facts.retail_hfov_deg);
          }
          // the host handles the phase of guest frame j now (its begin marker, then its end): the camera facts that are published at this moment are guest frame j + lag's
          if (j < frames && camera[size_t(j)].valid()) {
            ++phases;
            const mv::CameraFacts truth = camera[size_t(j)];
            const mv::Plan by_tag = mv::PlanForPhase(c, 229.0, 44.0, mv::UnpackPhaseField(tag[size_t(j)]));
            const mv::Plan by_old = OldPlanForView(c, 229.0, 44.0, request[size_t(j)], mv::CurrentCamera());
            const double want = CameraAspect(truth);
            const bool new_ok = by_tag.valid() && std::fabs(double(by_tag.width) / double(by_tag.height) - want) / want < 0.02;
            const bool old_ok = by_old.valid() && std::fabs(double(by_old.width) / double(by_old.height) - want) / want < 0.02;
            new_wrong_plan += new_ok ? 0 : 1;
            old_wrong_plan += old_ok ? 0 : 1;
            if (lag == 0) old_wrong_at_lag0 += old_ok ? 0 : 1;
            // the overlay's lookup field: the picture's own, against the guest's latest facts
            const mv::PhaseField t = mv::UnpackPhaseField(tag[size_t(j)]);
            const bool new_field_ok = t.known() && std::fabs(t.hfov_deg - truth.used_hfov_deg) < 0.0151f && std::fabs(t.vfov_deg - truth.used_vfov_deg) < 0.0151f;
            const mv::CameraFacts latest = mv::CurrentCamera();
            const bool old_field_ok = std::fabs(latest.used_hfov_deg - truth.used_hfov_deg) < 0.0151f && std::fabs(latest.used_vfov_deg - truth.used_vfov_deg) < 0.0151f;
            new_wrong_field += new_field_ok ? 0 : 1;
            old_wrong_field += old_field_ok ? 0 : 1;
            if (j > 0 && camera[size_t(j - 1)].valid() && std::fabs(camera[size_t(j)].used_hfov_deg - camera[size_t(j - 1)].used_hfov_deg) > 0.5f) ++changes;
          }
        }
      }
    }
  }
  Check(phases > 20000 && changes > 100, "the lag sessions have many phases and many changes of the field (" + std::to_string(phases) + " phases, " + std::to_string(changes) + " changes)");
  Check(new_wrong_plan == 0, "by the tag, every phase's view has its own camera's aspect, for every class and a guest 0 to 3 frames ahead (" + std::to_string(new_wrong_plan) + " wrong)");
  Check(new_wrong_field == 0, "by the tag, every picture's lookup field is its own camera's (" + std::to_string(new_wrong_field) + " wrong)");
  Check(old_wrong_at_lag0 == 0, "the old rule is right with no lag (the model is faithful)");
  Check(old_wrong_plan > 100 && old_wrong_field > 100, "the old rule (the request against the latest camera facts) is wrong at every change when the guest is ahead: the lag is what this test catches (" + std::to_string(old_wrong_plan) +
                                                          " stretched plans, " + std::to_string(old_wrong_field) + " wrong lookup fields)");
  std::printf("lag model: %d phases, %d changes of the field; wrong by the tag: %d plans, %d fields; wrong by the old rule: %d plans, %d fields (%d with no lag)\n", phases, changes, new_wrong_plan,
              new_wrong_field, old_wrong_plan, old_wrong_field, old_wrong_at_lag0);
  ResetLink();
  mv::WithdrawCamera();
}

// ---------------------------------------------------------------------------
// 4. the files
// ---------------------------------------------------------------------------

bool IsSource(const fs::path& p) {
  const std::string e = p.extension().string();
  return e == ".h" || e == ".cpp" || e == ".inc" || e == ".hlsl" || e == ".hlsli";
}

void TestFiles(const std::string& src) {
  const std::string link = ReadFile(src + "/gpu/mirror_view_link.h");
  const std::string rules = ReadFile(src + "/gpu/mirror_view_rules.h");
  const std::string host = ReadFile(src + "/gpu/mirror_view.cpp");
  const std::string host_h = ReadFile(src + "/gpu/mirror_view.h");
  const std::string gpu_h = ReadFile(src + "/gpu/gpu.h");
  const std::string scene = ReadFile(src + "/gpu/scene.cpp");
  const std::string cmake = ReadFile(src + "/gpu/CMakeLists.txt");
  const std::string rv = ReadFile(src + "/mc_engine/hooks/rear_view.cpp");
  const std::string shared_h = ReadFile(src + "/mc_engine/hooks/rear_view_shared.h");
  const std::string menu = ReadFile(src + "/mc_engine/menus/debug_menu_pages.cpp");
  const std::string hud_settings = ReadFile(src + "/settings/generated/settings_hud.h");
  const std::string gpu_settings = ReadFile(src + "/settings/generated/settings_gpu.h");
  const std::string manifest = ReadFile(src + "/settings/generated/manifest.json");
  const std::string meta = ReadFile(src + "/settings/generated/settings_meta.inc");
  const std::string toml = ReadFile(src + "/../larecomp_config.toml");
  if (link.empty() || rv.empty() || host.empty() || cmake.empty()) {
    std::printf("NOTE: the source files were not found under %s: the file checks are left out\n", src.c_str());
    return;
  }

  // ---- one link header, nothing defined twice
  Check(!fs::exists(src + "/gpu/mirror_view_guest.h"), "the guest's own link header is gone (folded into mirror_view_link.h)");
  Check(Has(link, "#include <algorithm>") && Has(link, "#include <atomic>") && Has(link, "#include <cmath>") && Has(link, "#include <cstdint>") && !Has(link, "plume") && !Has(link, "\n#include \"") &&
            !Has(link, "rex/"),
        "mirror_view_link.h holds nothing of the renderer or the guest (the hooks include it)");
  Check(Count(link, "namespace mcla::gpu::mirror_view {") == 1 && !Has(link, "namespace mcla::gpu::mirror_view_guest"), "one namespace in it, mcla::gpu::mirror_view");
  Check(Has(shared_h, "#include \"gpu/mirror_view_link.h\"") && !Has(shared_h, "mirror_view_guest") && Has(rv, "#include \"gpu/mirror_view_link.h\"") && !Has(rv, "mirror_view_guest") &&
            Has(rules, "#include \"mirror_view_link.h\""),
        "the guest's rules, its hooks and the host's rules all include the one header");
  Check(Has(shared_h, "namespace vlink = mcla::gpu::mirror_view;") && Has(rv, "namespace vlink = mcla::gpu::mirror_view;"), "and use its namespace (vlink is an alias of it in both)");
  // Walk the tree: the definitions the link owns are made once, and nothing else makes a Request or a marker of its own.
  size_t marker_defs = 0, request_defs = 0, guest_facts_defs = 0, camera_facts_defs = 0, stamp_defs = 0, publish_calls = 0, guest_ns = 0, old_setting = 0, note_calls_outside = 0, scene_composited_markers = 0;
  size_t files = 0;
  std::string publisher_file;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec) || !IsSource(it->path())) continue;
    const std::string name = it->path().filename().string();
    if (name.ends_with("_test.cpp")) continue;
    const std::string text = ReadFile(it->path().string());
    ++files;
    marker_defs += Count(text, "kMarkerBegin =");
    stamp_defs += Count(text, "g_stamp_wanted{");
    if (name.starts_with("mirror_view") || name.starts_with("rear_view")) {  // other parts of the tree have structs of these names (a Request of the online code)
      request_defs += Count(text, "struct Request {");
      guest_facts_defs += Count(text, "struct GuestFacts {");
      camera_facts_defs += Count(text, "struct CameraFacts {");
    }
    guest_ns += Count(text, "namespace mcla::gpu::mirror_view_guest");
    old_setting += Count(text, "rearview_shared_view");
    const size_t p = Count(text, "PublishRequest(");
    if (p != 0 && name != "mirror_view_link.h") {
      publish_calls += p;
      publisher_file = name;
    }
    if (name.ends_with(".cpp") && name != "rear_view.cpp" && name != "mirror_view.cpp") note_calls_outside += Count(text, "NoteMirrorPhase(");
    if (name == "rear_view.cpp") scene_composited_markers += Count(text, "OnGuestSceneComposited(");
  }
  Check(files > 200, "the walk saw the source tree (" + std::to_string(files) + " files)");
  Check(marker_defs == 1 && stamp_defs == 1, "the markers and the stamp flag are defined once in the tree (no copy in the guest's header)");
  Check(request_defs == 1 && guest_facts_defs == 1 && camera_facts_defs == 1, "Request, GuestFacts and CameraFacts are defined once");
  Check(guest_ns == 0, "no namespace mirror_view_guest anywhere");
  Check(old_setting == 0, "the guest's own switch rearview_shared_view is gone from every source (the setting, the manifest, the menu, the hooks)");
  Check(!Has(manifest, "rearview_shared_view") && !Has(meta, "rearview_shared_view") && !Has(hud_settings, "rearview_shared_view") && !Has(menu, "rearview_shared_view") && !Has(toml, "rearview_shared_view"),
        "... nor from the generated settings, the debug menu or the TOML");
  Check(publish_calls == 1 && publisher_file == "mirror_view.cpp", "the request has ONE publisher, MirrorView::BeginFrame (found " + std::to_string(publish_calls) + " in " + publisher_file + ")");
  Check(note_calls_outside == 0, "NoteMirrorPhase is called from the one hook file only");
  Check(scene_composited_markers == 0, "the guest no longer sends the markers through OnGuestSceneComposited itself");

  // ---- one switch
  Check(Has(gpu_settings, "mcla_gpu_mirror_view, 0,") && Has(manifest, "\"mcla_gpu_mirror_view\"") && Has(meta, "{\"mcla_gpu_mirror_view\", \"MCLA/GPU\"}"), "mcla_gpu_mirror_view is the one switch: a setting, default 0");
  {
    const size_t at = gpu_settings.find("mcla_gpu_mirror_view, 0,");
    const size_t end = gpu_settings.find(");", at);
    const std::string def = at == std::string::npos ? std::string() : gpu_settings.substr(at, end - at);
    Check(Has(def, "rex::kDebugOnly") && !Has(def, "kRequiresRestart"), "... debug only and live");
  }
  {
    const size_t at = rv.find("static bool SharedViewOn() {");
    const size_t end = rv.find("\n}\n", at);
    const std::string body = at == std::string::npos ? std::string() : rv.substr(at, end - at);
    Check(Has(body, "mcla::gpu::MirrorViewMode() != 0") && Has(body, "mcla::gpu::Enabled()") && !Has(body, "rearview_shared") && !Has(body, "mcla_gpu_mirror_view.Get()"),
          "the guest half is on exactly while the host's answer (the SIDE MIRRORS row under the debug switch, gpu.h MirrorViewMode) is not 0, with the Plume renderer");
  }
  {
    const size_t at = rv.find("static void SendMirrorMarker(uint32_t marker, bool hud_stamp = false) {");
    const size_t end = rv.find("\n}\n", at);
    const std::string body = at == std::string::npos ? std::string() : rv.substr(at, end - at);
    Check(Has(body, "mcla::gpu::NoteMirrorPhase(marker == vlink::kMarkerBegin, hud_stamp)") && !Has(body, "OnGuestSceneComposited"), "the guest sends markers and HUD eligibility through the host entry point");
  }
  {
    const size_t at = host.find("void NoteMirrorPhase(bool begin, bool hud_stamp) {");
    const size_t end = host.find("\n}\n", at);
    const std::string body = at == std::string::npos ? std::string() : host.substr(at, end - at);
    Check(!Has(body, "MirrorViewMode()") && !Has(body, "mcla_gpu_mirror_view.Get()") && Has(body, "g_open.exchange(false") && Has(body, "mirror_view::kMarkerEnd"),
          "NoteMirrorPhase keeps the guest's decision and sends an end only for a begin it sent");
  }
  Check(Has(gpu_h, "void NoteMirrorPhase(bool begin, bool hud_stamp);") && Has(gpu_h, "int MirrorViewMode();"), "gpu.h declares NoteMirrorPhase and MirrorViewMode");

  // ---- the request, once a frame, from one place, before anything can return
  {
    const size_t at = host.find("void MirrorView::BeginFrame(bool rt_side_mirrors) {");
    const size_t end = host.find("\n}\n", at);
    const std::string body = at == std::string::npos ? std::string() : host.substr(at, end - at);
    const size_t publish = body.find("mirror_view::PublishRequest(mirror_view::RequestFor(request));");
    Check(publish != std::string::npos && (body.find("return") == std::string::npos || body.find("return") > publish), "MirrorView::BeginFrame publishes the request, and no early return comes before it");
    Check(Count(body, "PublishRequest(") == 1 && Has(body, "mirror_view::PublishRequest(mirror_view::RequestFor(request));") && !Has(body, "request_"), "the request is the pure rule's answer (RequestFor), published (MV-J2: the plan no longer follows it, so it is not kept)");
    Check(Has(body, "request.enabled = enabled();") && Has(body, "request.blocked = Blocked();") && Has(body, "request.side_need = mirror_glass::SideNeed();"),
          "from the switch, the blocked state and MV3's flag (mirror_glass::SideNeed())");
    Check(Has(body, "request.rt_side_mirrors = rt_side_mirrors;") && Has(body, "request.hud_stamp = mirror_view::StampWanted();") && Has(body, "request.cls = SizeClass();") &&
              Has(body, "request.retail_hfov_deg = double(mirror_view::CurrentCamera().retail_hfov_deg);") && Has(body, "request.stamp_aspect = hud.valid() ? double(hud.w) / double(hud.h) : 0.0;"),
          "JOIN2: and from the RT mirrors' flag, the guest's stamp flag, the class, the HUD's retail field and the rectangle's aspect");
    Check(body.find("mode_ = setting.mode;") != std::string::npos && body.find("mode_ = setting.mode;") < publish && body.find("mirror_setting::Current()") < body.find("mode_ = setting.mode;"),
          "after the row and the switch under it are read for the frame (mirror_setting.h Current)");
    Check(Has(body, "request.every = setting.every;") && body.find("request.every = setting.every;") < publish && body.find("request.every = setting.every;") < body.find("mirror_view::RequestFor(request)"),
          "MV6: the request carries the level's cadence, set before the pure rule answers");
  }
  {
    const size_t at = host.find("void MirrorView::Shutdown() {");
    const size_t end = host.find("\n}\n", at);
    const std::string body = at == std::string::npos ? std::string() : host.substr(at, end - at);
    Check(Has(body, "mirror_view::WithdrawRequest();"), "MirrorView::Shutdown withdraws it (G9)");
  }
  Check(Has(host, "facts.blocked = Blocked();") && Has(host_h, "bool Blocked() const { return frame_ < blocked_until_ || shaders_failed_; }"), "the phase and the request ask the same Blocked()");
  Check(Has(host, "#include \"mirror_glass_need.h\"") && Has(rules, "inline Request RequestFor(const RequestFacts& f)"), "the request's rule is the pure RequestFor, the finder's flag is the atomic header");
  Check(Count(scene, "mirror_view_->BeginFrame(car_mirror_ && car_mirror_->active());") == 1 && Count(scene, "mirror_view_->BeginFrame(") == 1,
        "Scene::BeginFrame calls MirrorView::BeginFrame once, with CarMirrorRt::active() (the ray traced side mirrors win)");
  Check(Has(host_h, "void BeginFrame(bool rt_side_mirrors);") && !Has(host_h, "mirror_view::Request request_;") && Has(host_h, "mirror_view::Plan picture_plan_;"), "JOIN2: the view keeps the plan of its picture (MV-J2: not the request it published)");
  // ---- MV-J2: the per-phase field tag
  Check(Has(host_h, "bool BeginPhase(const mirror_draws::Rect& rect, const mirror_view::PhaseField& field, bool hud_stamp);") &&
            Has(host, "plan_ = mirror_view::PlanForPhase(SizeClass(), double(rect.w), double(rect.h), field);") && !Has(host, "PlanForView(") && !Has(host, "PlanForRect(") && !Has(rules, "PlanForView(") && !Has(rules, "FieldApplied("),
        "MV-J2: a phase's size follows the field of its own camera (PlanForPhase, from the begin marker's tag), not the request against the latest camera build (PlanForView and FieldApplied are gone)");
  Check(Has(host, "picture_field_ = phase_field_;") && Has(host, "phase_field_ = field;") && Has(host, "picture_field_ = mirror_view::PhaseField{};") && Has(host_h, "mirror_view::PhaseField phase_field_;") && Has(host_h, "mirror_view::PhaseField picture_field_;") &&
            Has(host_h, "bool PictureField(float* hfov_deg, float* vfov_deg) const;") && Has(host, "if (!whole_ || !picture_field_.known()) {"),
        "MV-J2: the view keeps the field of the phase that drew the picture it holds, and says it only while the picture is whole");
  {
    const size_t at = host.find("void NoteMirrorPhase(bool begin, bool hud_stamp) {");
    const size_t end = host.find("\n}\n", at);
    const std::string body = at == std::string::npos ? std::string() : host.substr(at, end - at);
    Check(Has(body, "OnGuestSceneComposited(hud_stamp ? mirror_view::kMarkerBegin : mirror_view::kMarkerBeginNoStamp,") &&
              Has(body, "mirror_view::PackPhaseField(mirror_view::PhaseFieldOf(mirror_view::CurrentCamera())));") && Has(body, "OnGuestSceneComposited(mirror_view::kMarkerEnd);"),
          "the begin carries the phase camera's field and HUD eligibility, and the end carries nothing");
  }
  Check(Has(gpu_h, "void OnGuestSceneComposited(uint32_t bucket, uint32_t tag = 0);"), "MV-J2: gpu.h's composite entry takes the record's second word");
  {
    const std::string records = ReadFile(src + "/gpu/records.h");
    const std::string capture = ReadFile(src + "/gpu/guest_capture.cpp");
    const std::string renderer = ReadFile(src + "/gpu/renderer.cpp");
    const std::string trace = ReadFile(src + "/gpu/frame_trace.cpp");
    const size_t rec = records.find("struct CompositeRecord {");
    const std::string rec_body = rec == std::string::npos ? std::string() : records.substr(rec, records.find("};", rec) - rec);
    Check(Has(rec_body, "uint32_t bucket = 0;") && Has(rec_body, "uint32_t tag = 0;") && !Has(rec_body, "reserved"), "MV-J2: the composite record's second word is the tag");
    Check(Has(capture, "void GuestCapture::Composite(uint32_t bucket, uint32_t tag) {") && Has(capture, "  r->bucket = bucket;\n  r->tag = tag;\n  CountRecord(sizeof(CompositeRecord));"), "... the capture writes it");
    Check(Has(renderer, "void OnGuestSceneComposited(uint32_t bucket, uint32_t tag) {") && Has(renderer, "c->Composite(bucket, tag);") &&
              Has(renderer, "scene_.OnMirrorMarker(bucket, reinterpret_cast<const CompositeRecord*>(payload)->tag, list);"),
          "... the renderer passes it to the scene with the marker");
    Check(Has(trace, "r.bucket, r.tag, draws_);"), "... and the frame trace prints it");
  }
  Check(Has(scene, "if (mirror_view::IsBeginMarker(bucket)) {") && Has(scene, "mirror_view_->BeginPhase(mirror_draws::Current(), mirror_view::UnpackPhaseField(tag), bucket == mirror_view::kMarkerBegin)") &&
            Has(scene, "const bool field_known = mirror_view_->PictureField(&field_h, &field_v);") && Has(scene, "mirror_overlay_->NoteViewDrawn(car_frame_->Current(), frame_serial_, field_known, field_h, field_v);"),
        "MV-J2: the scene hands the begin's tag to the view and the picture's own field to the overlay");
  {
    // nothing sizes, crops or places a picture by the guest's LATEST camera build any more: the host reads it in three places (the guest's own tag at NoteMirrorPhase, the request's retail field, the stamp's
    // retail field), and the overlay and the scene not at all
    const std::string overlay = ReadFile(src + "/gpu/mirror_overlay.cpp");
    Check(Count(host, "CurrentCamera()") == 3 && !Has(overlay, "CurrentCamera()") && !Has(overlay, "ReadViewField") && !Has(scene, "CurrentCamera()"),
          "MV-J2: the latest camera facts are read by NoteMirrorPhase (the tag), the request's retail field and the stamp's retail field, and by nothing that sizes or places a picture");
  }
  Check(Has(rv, "facts.from_request = cam.applied;"), "MV-J2: the guest's camera hook says whether the host's request was applied");
  Check(Has(host, "mirror_view::HudCrop(picture_plan_, double(mirror_view::CurrentCamera().retail_hfov_deg), stamp_aspect)") && Has(host, "{float(crop.u0), float(crop.v0), float(crop.u1), float(crop.v1)}") &&
            !Has(host, "{0.0f, 0.0f, 1.0f, 1.0f}") && Has(host, "const double stamp_aspect = rect_.valid() ? double(rect_.w) / double(rect_.h) : 0.0;") && Has(host, "picture_plan_ = plan_;") && Has(host, "picture_plan_ = mirror_view::Plan{};"),
        "JOIN2: the stamp cuts the HUD's field out of the picture it draws from (the plan the picture was made with)");
  Check(Has(rv, "facts.retail_hfov_deg = shared::HudFieldDeg(fov_was, aspect_was, fov_scale);") && Has(shared_h, "inline float HudFieldDeg(float game_fov_deg, float game_aspect, double fov_scale)"),
        "JOIN2: the guest reports the HUD's own field (the game's, narrowed by the debug scale) beside the one it used");
  // the guest reads the need only through the request
  {
    const size_t at = rv.find("static shared::Mode CurrentMode() {");
    const size_t end = rv.find("\n}\n", at);
    const std::string body = at == std::string::npos ? std::string() : rv.substr(at, end - at);
    Check(Has(body, "shared::EffectiveSideNeed(m.shared, vlink::CurrentRequest())") && !Has(rv, "mirror_glass::") && !Has(rv, "mirror_glass_need.h"), "the guest's need is the request's (MV3's flag is copied in by the host, the hooks do not include it)");
  }
  Check(Has(rv, "vlink::SetStampWanted(hud_stamp);") && Has(rv, "vlink::SetStampWanted(true);"), "the request builder still reads the HUD stamp flag from TickRearView");
  Check(Has(host, "facts.hud_stamp = hud_stamp;") && Has(host, "phase_hud_stamp_ = hud_stamp;") &&
            Count(host, "rect_.valid(), phase_hud_stamp_)") == 2, "both fresh and held pictures use the phase marker for the HUD stamp");
  // the camera step's clear and the hooks' wiring
  Check(Has(rv, "if (mcla::gpu::Enabled() && !HudStampWanted()) return true;") &&
            Has(rv, "return shared::SkipCameraClear(SharedViewOn(), vlink::CurrentRequest());"), "the camera step skips the main clear for hidden and redirected mirrors");
  Check(Has(rv, "SendMirrorMarker(g_markers.OnBegin(SharedViewOn() || !hud_stamp, true, base && MirrorConsumerActive(base)), hud_stamp);") &&
            Has(rv, "SendMirrorMarker(g_markers.OnEnd());"), "the begin covers the clear and sky even when the client loop is masked");
  Check(Has(toml, "address = 0x822E6908\nname = \"Patch_RearViewPhaseEnd\"\nregisters = [\"r30\"]\nafter_instruction = false"), "the END hook stays after the sky callback and before the phase changes");
  const size_t draw = scene.find("void Scene::OnDraw(");
  const size_t clear = scene.find("void Scene::OnClear(");
  const std::string suppress = "if (mirror_view_ && mirror_view_->suppressing())";
  const size_t draw_guard = scene.find(suppress, draw);
  const size_t clear_guard = scene.find(suppress, clear);
  Check(scene.find("banks_.Apply(", draw) < draw_guard && draw_guard < scene.find("const Skip skip = Prepare(", draw),
        "suppressed draws consume constant deltas and skip draw preparation and recording");
  Check(clear_guard < scene.find("const RegisterWindow pre(", clear), "suppressed clears leave the main target alone");
  Check(Has(rv, "SetClientMirrorPhase(base, renderer, kCars, mirror_on && shared::CarsInView(mode, larecomp::settings::rearview_cars.Get()));") &&
            Has(rv, "if (!shared::CarsInView(CurrentMode(), larecomp::settings::rearview_cars.Get()) || !PlausibleGuestPtr(item)) return;"),
        "the racers join the side mirrors' view in both places that decide it (the membership and the car's phase hook)");
  Check(Has(shared_h, "inline bool CarsInView(const Mode& m, bool cars_setting) { return cars_setting || (m.shared && m.side_need); }"), "the one line of the decision: the setting, or the side mirrors' need");
  // build
  Check(Has(cmake, "add_executable(larecomp_mirror_view_join_test EXCLUDE_FROM_ALL mirror_view_join_test.cpp)"), "CMakeLists.txt builds this test");
}

}  // namespace

int main(int argc, char** argv) {
  std::string src = argc > 1 ? argv[1] : std::string();
  if (src.empty()) {
    const std::string self = __FILE__;
    const size_t slash = self.find_last_of("/\\");
    const std::string dir = slash == std::string::npos ? std::string(".") : self.substr(0, slash);  // .../src/gpu
    src = dir + "/..";
  }
  TestSwitchOff();
  TestSwitchOn();
  TestFlipsInTheMiddle();
  TestQueuedHudEligibility();
  TestRequestEveryFrame();
  TestSideOnlyEndToEnd();
  TestRacersJoin();
  TestRayTracedMirrorsWin();
  TestRayTracedMirrorsComeAndGo();
  TestFieldRoundTrip();
  TestFieldLag();
  TestFiles(src);
  ResetLink();
  std::printf("mirror_view_join_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
