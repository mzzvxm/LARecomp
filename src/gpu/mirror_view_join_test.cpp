// Check of the JOIN of the shared rear view's three halves (perf/MIRROR-VIEW.md section 12): MV1 the host view (mirror_view_rules.h), MV2 the guest (hooks/rear_view_shared.h) and MV3 the glass finder's
// flag (mirror_glass_need.h), through the one link header (mirror_view_link.h). The halves have tests of their own (mirror_view_rules_test, rear_view_shared_test, mirror_glass_rules_test); this one
// pins what only the join decides:
//   1. ONE SWITCH: mcla_gpu_mirror_view drives both halves. With it off the guest sends no marker, asks for nothing, skips nothing and answers every hook as the game did; with it on, markers
//      and the host's phase tracker stay balanced however the switch flips in the middle of a frame.
//   2. THE REQUEST, once a frame: the host publishes redirect (and the side mirrors' need, MV3's flag with its hold) every frame; the guest's side-only phase and its skip of the camera step's clear
//      exist only while the request says redirect; the stamp follows the guest's one stamp flag.
//   3. THE RACERS join the side mirrors' view (the main session's decision of 2026-11-22, MIRROR-VIEW.md 11.5).
//   4. THE FILES: one link header, no second definition, one publisher of the request, one switch (the setting the guest read before, rearview_shared_view, is gone), the wiring lines.
// A program of its own: it needs no game, no ReXGlue and no renderer, and exits 0 when every check holds.
//
//   cmake --build <dir> --target larecomp_mirror_view_join_test, then run src/gpu/larecomp_mirror_view_join_test [<src dir>] (exit 0 = passed).
//   by hand (from the repo root): clang++ -std=c++23 -I src src/gpu/mirror_view_join_test.cpp -o mirror_view_join_test.exe

#include <cstdint>
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
  void BeginFrame(int view_at_frame, bool is_blocked, bool glass_need) {
    enabled = view_at_frame != 0;
    blocked = is_blocked;
    mv::RequestFacts f;
    f.enabled = enabled;
    f.blocked = blocked;
    f.side_need = glass_need;
    mv::PublishRequest(mv::RequestFor(f));
    phase.FrameStart();
    armed = false;
    ++age;
  }
  // A marker out of the composite record. `drew`: the phase had draws (a real phase), not the empty pair of a held frame (MirrorView::EndPhase: the picture before stays and the stamp is armed from it).
  bool OnMarker(uint32_t marker, bool* redirected, bool drew = true) {
    if (marker == mv::kMarkerBegin) {
      mv::BeginFacts b;
      b.enabled = enabled;
      b.rect_valid = rect_valid;
      b.plan_valid = true;
      b.blocked = blocked;
      const bool r = phase.Begin(b) == mv::PhaseState::kRedirect;
      if (redirected && r) *redirected = true;
      return r;
    }
    if (phase.End()) {
      if (drew) {
        whole = true;
        age = 0;
      }
      armed = mv::StampDue(whole, age, rect_valid, mv::StampWanted());
    }
    return true;
  }
  // The first copy of the main colour after the end marker: is the stamp drawn?
  bool StampNow() const { return armed; }
};

// NoteMirrorPhase (mirror_view.cpp): a begin only while the setting is on, an end only for a begin that went.
struct Notes {
  bool open = false;
  // returns the marker sent, 0 for none
  uint32_t Note(bool begin, int view_now) {
    if (begin) {
      if (view_now == 0) return 0;
      open = true;
      return mv::kMarkerBegin;
    }
    if (open) {
      open = false;
      return mv::kMarkerEnd;
    }
    return 0;
  }
};

// One whole frame: the host's BeginFrame, then the guest's tick, the cadence, the phase's begin and end, the end-of-scene stamp. `flip_to` >= 0 turns the switch to that value between the guest tracker's
// read and NoteMirrorPhase's (the worst place for it).
Frame RunFrame(Host& host, Notes& notes, rs::MarkerTracker& tracker, const World& w, bool glass_need, int flip_to = -1) {
  Frame out;
  // host: the render thread's frame start. The side mirrors' need is MV3's flag, read through the real atomic.
  mg::PublishNeed(mg::NeedState{glass_need, uint8_t(glass_need ? 1 : 0), uint16_t(glass_need ? 240 : 0)});
  host.BeginFrame(w.view_at_frame, w.blocked, mg::SideNeed());

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
  if (out.phase_asked && active) {
    out.camera_clear_skipped = rs::SkipCameraClear(shared, out.request);
  }
  const uint32_t begin_marker = tracker.OnBegin(shared, out.phase_asked, active);
  int view_for_notes = w.view;
  if (flip_to >= 0) view_for_notes = flip_to;
  if (begin_marker != 0) {
    const uint32_t sent = notes.Note(true, view_for_notes);
    if (sent != 0) {
      out.markers.push_back(sent);
      host.OnMarker(sent, &redirected_now);
    }
  }
  const uint32_t end_marker = tracker.OnEnd();
  if (end_marker != 0) {
    const uint32_t sent = notes.Note(false, view_for_notes);
    if (sent != 0) {
      out.markers.push_back(sent);
      host.OnMarker(sent, nullptr);
    }
  }
  if (tracker.OnHeldFrame(shared, active && w.held)) {  // a held frame: the empty phase at the place the phase would have ended
    for (bool begin : {true, false}) {
      const uint32_t sent = notes.Note(begin, view_for_notes);
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
  // Every combination with the switch off: nothing is sent, nothing asked, nothing skipped, every hook answers as the game did (rear_view_shared.h's rules with shared off).
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
    Check(f.markers.empty(), "no marker with the switch off" + tag);
    Check(!f.host_redirected && !host.whole && !f.stamp_armed, "the host redirects nothing and stamps nothing" + tag);
    Check(!f.request.redirect && !f.request.side_need, "the host's request says no redirect and no need (the finder's flag is not passed on)" + tag);
    Check(!f.mode.side_need && !f.side_only, "no side-only phase" + tag);
    Check(!f.camera_clear_skipped, "the camera step's clear is the game's" + tag);
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
        Check(f.markers[i] == (i % 2 == 0 ? mv::kMarkerBegin : mv::kMarkerEnd), "markers alternate BEGIN / END" + tag);
      }
      Check(host.phase.state == mv::PhaseState::kIdle && host.phase.unbalanced == 0, "the host's tracker ends the frame closed and balanced" + tag);
      const bool ran = !f.markers.empty() && !w.held;
      // the camera clear is skipped exactly when the phase ran under a redirect
      Check(f.camera_clear_skipped == (f.phase_asked && w.consumer_active && !w.blocked && (f.mode.hud || f.mode.side_need) && f.visible), "the camera step's clear is skipped only under a redirect" + tag);
      if (f.camera_clear_skipped) {
        Check(f.host_redirected, "a skipped camera clear always meets a host that redirects the phase" + tag);
      }
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
  // The switch flips between the guest tracker's read and NoteMirrorPhase's (and the host reads it at the frame's start): the host's tracker never sees an unbalanced phase.
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
    const int flip = (next() % 3) == 0 ? int(next() % 2) * 2 : -1;  // 0 (off) or 2 (on) between the two reads
    const Frame f = RunFrame(host, notes, tracker, w, glass, flip);
    for (uint32_t m : f.markers) (m == mv::kMarkerBegin ? begins : ends)++;
    if (f.markers.size() % 2 != 0) balanced = false;
    if (host.phase.state != mv::PhaseState::kIdle || notes.open) open_at_end = true;
    redirected += f.host_redirected ? 1 : 0;
    if (w.view == 0 && flip < 0) {
      if (!f.markers.empty()) balanced = false;  // a marker with the switch off and no flip
    }
  }
  Check(balanced, "random frames with the switch flipping mid-frame: the markers always come in BEGIN / END pairs, none with the switch off");
  Check(!open_at_end, "and no phase or note is left open at a frame's end");
  Check(host.phase.unbalanced == 0, "the host's tracker saw no begin without an end and no end without a begin");
  Check(begins == ends && begins > 1000 && redirected > 500, "the run really exercised the join (" + std::to_string(begins) + " pairs, " + std::to_string(redirected) + " redirected)");
}

// ---------------------------------------------------------------------------
// 2. the request, once a frame, and MV3's flag with its hold
// ---------------------------------------------------------------------------

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
    Check(Has(body, "larecomp::settings::mcla_gpu_mirror_view.Get() != 0") && Has(body, "mcla::gpu::Enabled()") && !Has(body, "rearview_shared"), "the guest half is on exactly while the host's switch is on, with the Plume renderer");
  }
  {
    const size_t at = rv.find("static void SendMirrorMarker(uint32_t marker) {");
    const size_t end = rv.find("\n}\n", at);
    const std::string body = at == std::string::npos ? std::string() : rv.substr(at, end - at);
    Check(Has(body, "mcla::gpu::NoteMirrorPhase(marker == vlink::kMarkerBegin)") && !Has(body, "OnGuestSceneComposited"), "the guest sends its markers through the host's entry point (which tests the switch itself)");
  }
  {
    const size_t at = host.find("void NoteMirrorPhase(bool begin) {");
    const size_t end = host.find("\n}\n", at);
    const std::string body = at == std::string::npos ? std::string() : host.substr(at, end - at);
    Check(Has(body, "mcla_gpu_mirror_view.Get() == 0") && Has(body, "g_open.exchange(false") && Has(body, "mirror_view::kMarkerBegin") && Has(body, "mirror_view::kMarkerEnd"),
          "NoteMirrorPhase tests the switch and sends an end only for a begin it sent");
  }
  Check(Has(gpu_h, "void NoteMirrorPhase(bool begin);"), "gpu.h declares NoteMirrorPhase");

  // ---- the request, once a frame, from one place, before anything can return
  {
    const size_t at = host.find("void MirrorView::BeginFrame() {");
    const size_t end = host.find("\n}\n", at);
    const std::string body = at == std::string::npos ? std::string() : host.substr(at, end - at);
    const size_t publish = body.find("mirror_view::PublishRequest(mirror_view::RequestFor(request));");
    Check(publish != std::string::npos && (body.find("return") == std::string::npos || body.find("return") > publish), "MirrorView::BeginFrame publishes the request, and no early return comes before it");
    Check(Has(body, "request.enabled = enabled();") && Has(body, "request.blocked = Blocked();") && Has(body, "request.side_need = mirror_glass::SideNeed();"),
          "from the switch, the blocked state and MV3's flag (mirror_glass::SideNeed())");
    Check(body.find("mode_ = larecomp::settings::mcla_gpu_mirror_view.Get();") < publish, "after the switch is read for the frame");
  }
  {
    const size_t at = host.find("void MirrorView::Shutdown() {");
    const size_t end = host.find("\n}\n", at);
    const std::string body = at == std::string::npos ? std::string() : host.substr(at, end - at);
    Check(Has(body, "mirror_view::WithdrawRequest();"), "MirrorView::Shutdown withdraws it (G9)");
  }
  Check(Has(host, "facts.blocked = Blocked();") && Has(host_h, "bool Blocked() const { return frame_ < blocked_until_ || shaders_failed_; }"), "the phase and the request ask the same Blocked()");
  Check(Has(host, "#include \"mirror_glass_need.h\"") && Has(rules, "inline Request RequestFor(const RequestFacts& f)"), "the request's rule is the pure RequestFor, the finder's flag is the atomic header");
  Check(Count(scene, "mirror_view_->BeginFrame();") == 1, "Scene::BeginFrame calls MirrorView::BeginFrame once");
  // the guest reads the need only through the request
  {
    const size_t at = rv.find("static shared::Mode CurrentMode() {");
    const size_t end = rv.find("\n}\n", at);
    const std::string body = at == std::string::npos ? std::string() : rv.substr(at, end - at);
    Check(Has(body, "shared::EffectiveSideNeed(m.shared, vlink::CurrentRequest())") && !Has(rv, "mirror_glass::") && !Has(rv, "mirror_glass_need.h"), "the guest's need is the request's (MV3's flag is copied in by the host, the hooks do not include it)");
  }
  // the stamp's one channel
  Check(Has(rv, "vlink::SetStampWanted(hud_stamp);") && Has(rv, "vlink::SetStampWanted(true);") && !Has(link, "hud_stamp"), "the stamp's flag has one channel: SetStampWanted (TickRearView sets it, the facts block does not repeat it)");
  // the camera step's clear and the hooks' wiring
  Check(Has(rv, "return shared::SkipCameraClear(SharedViewOn(), vlink::CurrentRequest());"), "the camera step's clear is skipped under the request's redirect");
  Check(Has(rv, "SendMirrorMarker(g_markers.OnBegin(SharedViewOn(), g_phase_requested, base && MirrorConsumerActive(base)));") && Has(rv, "SendMirrorMarker(g_markers.OnEnd());"), "the markers: BEGIN at the begin step, END at the phase's end");
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
  TestRequestEveryFrame();
  TestSideOnlyEndToEnd();
  TestRacersJoin();
  TestFiles(src);
  ResetLink();
  std::printf("mirror_view_join_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
