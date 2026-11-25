// Check of the guest half of the shared rear view (rear_view_shared.h and gpu/mirror_view_link.h, the one link header): the three states of the mirror's phase, the markers' state machine, the mirror
// camera's field and aspect, the cadence, the redirect gate and the packing of the blocks the two halves share. A program of its own: it needs no game, no ReXGlue and no
// renderer, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_rear_view_shared_test     (CMakeLists.txt, next to larecomp_rear_view_cheap_test)
//   <build dir>/larecomp_rear_view_shared_test
//
// The compile by hand (from the folder of this file): clang++ -std=c++23 -I ../../ -I . rear_view_shared_test.cpp -o rear_view_shared_test.exe

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "gpu/mirror_view_link.h"
#include "rear_view_shared.h"

namespace vlink = mcla::gpu::mirror_view;
namespace rs = larecomp::rearview_shared;

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

void CheckNum(double got, double want, const std::string& what, double tol = 1e-4) {
  ++g_checks;
  if (!(std::fabs(got - want) <= tol)) {
    ++g_failed;
    std::printf("FAILED: %s: got %.9g, want %.9g\n", what.c_str(), got, want);
  }
}

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

// ---------------------------------------------------------------------------
// the link: markers, packing
// ---------------------------------------------------------------------------

void TestMarkers() {
  Check(vlink::kMarkerBegin == 0x7F000010u && vlink::kMarkerEnd == 0x7F000011u, "the marker values are the design's");
  Check(vlink::kMarkerBegin != vlink::kMarkerEnd, "the two markers differ");
  // They are messages of the side channel (top byte 0x7F, ultrawide.h IsUiMessage) and none of the values ultrawide.h uses (0x7F000001 .. 0x7F00000A).
  Check((vlink::kMarkerBegin & 0xFF000000u) == 0x7F000000u && (vlink::kMarkerEnd & 0xFF000000u) == 0x7F000000u, "both are in the side channel");
  for (uint32_t v = 0x7F000001u; v <= 0x7F00000Au; ++v) {
    Check(v != vlink::kMarkerBegin && v != vlink::kMarkerEnd, "a marker does not collide with ultrawide's 0x7F0000nn values");
  }
  Check(vlink::IsMarker(vlink::kMarkerBegin) && vlink::IsMarker(vlink::kMarkerEnd), "IsMarker says yes to both");
  Check(!vlink::IsMarker(0x10000000u) && !vlink::IsMarker(0x7F000001u) && !vlink::IsMarker(0) && !vlink::IsMarker(0x7F000012u), "IsMarker says no to a real bucket and to the other messages");
}

void TestCenti() {
  CheckNum(vlink::FromCenti(vlink::ToCenti(69.0f)), 69.0, "69 degrees survives");
  CheckNum(vlink::FromCenti(vlink::ToCenti(44.44f)), 44.44, "44.44 survives");
  Check(vlink::ToCenti(0.0f) == 0 && vlink::ToCenti(-5.0f) == 0 && vlink::ToCenti(kNaN) == 0 && vlink::ToCenti(kInf) == 0, "zero, negative, NaN and infinity are 0");
  Check(vlink::ToCenti(0.004f) == 0 && vlink::ToCenti(0.006f) == 1, "rounds to the nearest centidegree");
  Check(vlink::ToCenti(1.0e6f) == 65535u, "a huge field is held at the 16 bit limit");
}

void TestRequestPacking() {
  vlink::Request r;
  r.side_need = true;
  r.every = 2;
  r.hfov_deg = 69.0f;
  r.vfov_deg = 44.0f;
  const vlink::Request back = vlink::UnpackRequest(vlink::Pack(r));
  Check(back.side_need && back.every == 2, "side need and cadence survive");
  CheckNum(back.hfov_deg, 69.0, "hfov survives");
  CheckNum(back.vfov_deg, 44.0, "vfov survives");
  Check(back.has_field(), "a request with both fields has a field");

  vlink::Request none;
  Check(!none.has_field() && !none.redirect && !none.side_need, "a default request has no field, no redirect and no need");
  {
    // redirect and side_need are two bits that do not leak into each other
    vlink::Request a;
    a.redirect = true;
    const vlink::Request ab = vlink::UnpackRequest(vlink::Pack(a));
    Check(ab.redirect && !ab.side_need, "redirect alone survives");
    vlink::Request b;
    b.side_need = true;
    const vlink::Request bb = vlink::UnpackRequest(vlink::Pack(b));
    Check(!bb.redirect && bb.side_need, "need alone survives");
    a.side_need = true;
    a.every = 3;
    const vlink::Request cc = vlink::UnpackRequest(vlink::Pack(a));
    Check(cc.redirect && cc.side_need && cc.every == 3, "both together, with the cadence");
    Check(!vlink::UnpackRequest(0).redirect, "all zero redirects nothing");
  }
  Check(!none.has_field(), "a default request has no field");
  Check(!vlink::UnpackRequest(0).side_need && vlink::UnpackRequest(0).every == 1 && !vlink::UnpackRequest(0).has_field(), "all zero is: no need, every frame, no field (the withdrawn state)");
  vlink::Request half;
  half.hfov_deg = 80.0f;
  Check(!half.has_field(), "only a horizontal field is not a field");
  half.hfov_deg = 0.0f;
  half.vfov_deg = 30.0f;
  Check(!half.has_field(), "only a vertical field is not a field");

  vlink::Request many;
  many.every = 100;
  Check(vlink::UnpackRequest(vlink::Pack(many)).every == vlink::kMaxEvery, "a cadence above the range is held at the top");
  many.every = 0;
  Check(vlink::UnpackRequest(vlink::Pack(many)).every == 1, "a cadence of 0 is every frame");

  // the fields do not leak into each other, and the flag does not leak into the fields
  vlink::Request a;
  a.hfov_deg = 100.0f;
  a.vfov_deg = 26.0f;
  const vlink::Request ab = vlink::UnpackRequest(vlink::Pack(a));
  Check(!ab.side_need, "no need stays no need");
  CheckNum(ab.hfov_deg, 100.0, "hfov alone");
  CheckNum(ab.vfov_deg, 26.0, "vfov alone");
  vlink::Request b;
  b.side_need = true;
  const vlink::Request bb = vlink::UnpackRequest(vlink::Pack(b));
  Check(bb.side_need && !bb.has_field() && bb.hfov_deg == 0.0f && bb.vfov_deg == 0.0f, "need alone carries no field");

  vlink::WithdrawRequest();
  Check(!vlink::CurrentRequest().side_need && !vlink::CurrentRequest().has_field(), "Withdraw clears the request");
  vlink::PublishRequest(r);
  Check(vlink::CurrentRequest().side_need && vlink::CurrentRequest().has_field(), "Publish is read back by Current");
  vlink::WithdrawRequest();
  Check(!vlink::CurrentRequest().side_need, "and Withdraw clears it again");
}

void TestGuestPacking() {
  for (int mask = 0; mask < 4; ++mask) {
    vlink::GuestFacts g;
    g.phase_asked = (mask & 1) != 0;
    g.side_only = (mask & 2) != 0;
    g.rig = 1;
    const vlink::GuestFacts back = vlink::UnpackGuest(vlink::Pack(g));
    Check(back.phase_asked == g.phase_asked && back.side_only == g.side_only && back.rig == 1, "the two flags are independent: mask " + std::to_string(mask));
  }
  for (int rig : {-1, 0, 1, 2, 3, 4, 6, 100, 254}) {
    vlink::GuestFacts g;
    g.rig = rig;
    Check(vlink::UnpackGuest(vlink::Pack(g)).rig == rig, "rig " + std::to_string(rig) + " survives");
  }
  vlink::GuestFacts g;
  g.rig = -50;
  Check(vlink::UnpackGuest(vlink::Pack(g)).rig == -1, "a rig below -1 is unknown");
  g.rig = 5000;
  Check(vlink::UnpackGuest(vlink::Pack(g)).rig == 254, "a rig above the range is held at the top");
  Check(vlink::UnpackGuest(0).rig == -1 && !vlink::UnpackGuest(0).phase_asked && !vlink::UnpackGuest(0).side_only, "all zero is: rig unknown, nothing up");
  vlink::PublishGuest(vlink::GuestFacts{true, false, 1});
  Check(vlink::CurrentGuest().phase_asked && !vlink::CurrentGuest().side_only && vlink::CurrentGuest().rig == 1, "Publish is read back by Current");
  vlink::PublishGuest(vlink::GuestFacts{});
}

void TestCameraPacking() {
  vlink::CameraFacts c;
  c.retail_hfov_deg = 100.4f;
  c.used_hfov_deg = 69.0f;
  c.used_vfov_deg = 44.0f;
  c.seq = 7;
  const vlink::CameraFacts back = vlink::UnpackCamera(vlink::Pack(c));
  CheckNum(back.retail_hfov_deg, 100.4, "retail hfov survives");
  CheckNum(back.used_hfov_deg, 69.0, "used hfov survives");
  CheckNum(back.used_vfov_deg, 44.0, "used vfov survives");
  Check(back.seq == 7 && back.valid(), "seq survives and the facts are valid");
  c.seq = 256 + 3;
  Check(vlink::UnpackCamera(vlink::Pack(c)).seq == 3, "seq counts modulo 256");
  Check(!vlink::UnpackCamera(0).valid(), "all zero is not a camera");
  vlink::CameraFacts half;
  half.used_hfov_deg = 50.0f;
  Check(!half.valid(), "a camera needs both fields");
  vlink::PublishCamera(c);
  Check(vlink::CurrentCamera().valid(), "Publish is read back by Current");
  vlink::WithdrawCamera();
  Check(!vlink::CurrentCamera().valid(), "Withdraw clears it");
}

// The stamp's flag is the ONE channel of "the HUD mirror is up" (the guest's facts do not repeat it): true until the guest says otherwise.
void TestStampFlag() {
  Check(vlink::StampWanted(), "the stamp is wanted by default (the host alone: the HUD mirror is the only reason for a phase)");
  vlink::SetStampWanted(false);
  Check(!vlink::StampWanted(), "a phase only the side mirrors asked for: no stamp");
  vlink::SetStampWanted(true);
  Check(vlink::StampWanted(), "and back");
}

// ---------------------------------------------------------------------------
// the three states
// ---------------------------------------------------------------------------

void TestStates() {
  for (int bits = 0; bits < 8; ++bits) {
    rs::Mode m;
    m.shared = (bits & 1) != 0;
    m.hud = (bits & 2) != 0;
    m.side_need = (bits & 4) != 0;
    const std::string tag = " [shared " + std::to_string(m.shared) + " hud " + std::to_string(m.hud) + " need " + std::to_string(m.side_need) + "]";
    const bool forced = m.shared && m.side_need;
    Check(rs::PhaseWanted(m) == (m.hud || forced), "PhaseWanted" + tag);
    Check(rs::SideOnly(m) == (!m.hud && forced), "SideOnly" + tag);
    for (int game = 0; game < 2; ++game) {
      const bool want = forced ? true : (m.hud ? game != 0 : false);
      Check(rs::PhaseVisible(m, game != 0) == want, "PhaseVisible game=" + std::to_string(game) + tag);
    }
    for (int vis = 0; vis < 2; ++vis) {
      const bool hidden = !m.hud ? true : (m.shared && vis == 0);
      Check(rs::FrameHidden(m, vis != 0) == hidden, "FrameHidden form=" + std::to_string(vis) + tag);
      Check(rs::HudStamp(m, vis != 0) == (m.hud && vis != 0), "HudStamp form=" + std::to_string(vis) + tag);
    }
    for (int cars = 0; cars < 2; ++cars) {
      Check(rs::CarsInView(m, cars != 0) == (cars != 0 || forced), "CarsInView cars=" + std::to_string(cars) + tag);
    }
  }

  // The named cases of the design's table (MIRROR-VIEW.md 2.4), spelled out.
  {
    const rs::Mode on_no{true, true, false};  // HUD mirror on, side mirrors do not need it: the phase as today, the stamp
    Check(rs::PhaseWanted(on_no) && !rs::SideOnly(on_no) && rs::PhaseVisible(on_no, true) && !rs::FrameHidden(on_no, true) && rs::HudStamp(on_no, true), "row 1: HUD on, no need: phase, frame, stamp");
    const rs::Mode on_yes{true, true, true};  // both: one view
    Check(rs::PhaseWanted(on_yes) && !rs::SideOnly(on_yes) && rs::HudStamp(on_yes, true) && !rs::FrameHidden(on_yes, true), "row 2: HUD on, need: one view, the stamp, the frame");
    const rs::Mode off_yes{true, false, true};  // HUD off, need: the phase with the HUD form hidden
    Check(rs::PhaseWanted(off_yes) && rs::SideOnly(off_yes) && rs::PhaseVisible(off_yes, false) && rs::FrameHidden(off_yes, true) && !rs::HudStamp(off_yes, true), "row 3: HUD off, need: the phase runs, no frame, no stamp");
    const rs::Mode off_no{true, false, false};  // nothing
    Check(!rs::PhaseWanted(off_no) && !rs::PhaseVisible(off_no, true) && rs::FrameHidden(off_no, true) && !rs::HudStamp(off_no, true), "row 4: HUD off, no need: nothing, even if the game's own bit is set");
  }

  // With the guest half off the answers are exactly what they were before this work, whatever the host's request says.
  for (int hud = 0; hud < 2; ++hud) {
    for (int need = 0; need < 2; ++need) {
      const rs::Mode m{false, hud != 0, need != 0};
      Check(rs::PhaseWanted(m) == (hud != 0), "off: the phase is wanted when the HUD mirror is on");
      Check(!rs::SideOnly(m), "off: never the side mirrors' view only");
      Check(rs::PhaseVisible(m, true) == (hud != 0) && !rs::PhaseVisible(m, false), "off: the game's answer with the HUD mirror on, hidden with it off");
      Check(rs::FrameHidden(m, true) == (hud == 0) && rs::FrameHidden(m, false) == (hud == 0), "off: the frame is hidden exactly when the HUD mirror is off, whatever the form says");
      Check(rs::CarsInView(m, false) == false && rs::CarsInView(m, true) == true, "off: the racers follow the player's setting alone");
    }
  }
}

void TestRedirectGate() {
  for (int bits = 0; bits < 8; ++bits) {
    const bool shared = (bits & 1) != 0;
    vlink::Request req;
    req.redirect = (bits & 2) != 0;
    req.side_need = (bits & 4) != 0;
    const std::string tag = " [shared " + std::to_string(shared) + " redirect " + std::to_string(req.redirect) + " need " + std::to_string(req.side_need) + "]";
    Check(rs::EffectiveSideNeed(shared, req) == (shared && req.redirect && req.side_need), "EffectiveSideNeed" + tag);
    Check(rs::SkipCameraClear(shared, req) == (shared && req.redirect), "SkipCameraClear" + tag);
  }
  // the cases that matter, spelled out
  vlink::Request redirecting;
  redirecting.redirect = true;
  vlink::Request needing;
  needing.side_need = true;
  Check(!rs::EffectiveSideNeed(true, needing), "the host's need without a redirect does nothing (an unframed mirror must not reach the main picture)");
  vlink::Request both = redirecting;
  both.side_need = true;
  Check(rs::EffectiveSideNeed(true, both), "the host's need counts with a redirect");
  Check(!rs::EffectiveSideNeed(true, redirecting), "a redirect with no need is no need");
  Check(!rs::EffectiveSideNeed(false, both), "the guest half off: no need");
  Check(!rs::SkipCameraClear(true, vlink::Request{}) && rs::SkipCameraClear(true, redirecting) && !rs::SkipCameraClear(false, redirecting), "the camera step's clear is skipped only with the guest half on and a redirect");
}

// ---------------------------------------------------------------------------
// the markers
// ---------------------------------------------------------------------------

void TestMarkerTracker() {
  {
    rs::MarkerTracker t;
    Check(!t.open(), "starts closed");
    Check(t.OnBegin(true, true, true) == vlink::kMarkerBegin && t.open(), "a phase that runs sends BEGIN");
    Check(t.OnEnd() == vlink::kMarkerEnd && !t.open(), "its end sends END");
    Check(t.OnEnd() == 0, "a second end sends nothing");
    Check(t.begun() == 1 && t.ended() == 1 && t.closed_late() == 0 && t.nested() == 0, "counters: one begin, one end");
  }
  {
    rs::MarkerTracker t;
    Check(t.OnBegin(false, true, true) == 0 && !t.open(), "the guest half off: no BEGIN");
    Check(t.OnBegin(true, false, true) == 0 && !t.open(), "a phase nobody asked for (a held frame, the HUD mirror hidden): no BEGIN");
    Check(t.OnBegin(true, true, false) == 0 && !t.open(), "the consumer inactive (the begin step does nothing): no BEGIN");
    Check(t.not_run() == 3, "the three are counted as begin steps that did not run the phase");
    Check(t.OnEnd() == 0, "and no END without a BEGIN");
  }
  {
    rs::MarkerTracker t;
    t.OnBegin(true, true, true);
    Check(t.OnBegin(true, true, true) == 0 && t.nested() == 1, "a BEGIN inside an open bracket is ignored and counted");
    Check(t.OnEnd() == vlink::kMarkerEnd, "and one END closes it");
    Check(t.OnEnd() == 0, "there is only one");
  }
  {
    rs::MarkerTracker t;
    Check(t.OnFrameStart() == 0, "the frame's start with nothing open sends nothing");
    t.OnBegin(true, true, true);
    Check(t.OnFrameStart() == vlink::kMarkerEnd && !t.open() && t.closed_late() == 1, "a bracket that lost its END is closed at the next frame's start");
    Check(t.OnEnd() == 0, "and a late END then sends nothing");
    Check(t.OnBegin(true, true, true) == vlink::kMarkerBegin, "the next frame can begin again");
  }
  {
    rs::MarkerTracker t;
    t.OnBegin(true, true, true);
    Check(t.OnEnd() == vlink::kMarkerEnd, "the END does not look at the guest half's switch: a bracket opened with it on is closed with it off");
  }
  {
    // A held frame: the two back to back, only with the guest half on and nothing open.
    rs::MarkerTracker t;
    Check(t.OnHeldFrame(true, true) && t.held_pairs() == 1, "a held frame with the guest half on: send the pair");
    Check(!t.open(), "the pair is complete: nothing left open");
    Check(!t.OnHeldFrame(false, true), "the guest half off: no pair");
    Check(!t.OnHeldFrame(true, false), "a frame that was not held: no pair");
    t.OnBegin(true, true, true);
    Check(!t.OnHeldFrame(true, true) && t.held_pairs() == 1, "a bracket open: no pair on top of it");
    Check(t.OnEnd() == vlink::kMarkerEnd, "and its END is still sent");
    Check(t.OnHeldFrame(true, true) && t.held_pairs() == 2, "closed again: the next held frame sends its pair");
  }
  {
    // A long run of random events: what is sent always alternates BEGIN, END, BEGIN, END ... starting with BEGIN, and a frame start always leaves it closed.
    rs::MarkerTracker t;
    uint32_t seed = 12345u;
    auto next = [&seed]() {
      seed = seed * 1664525u + 1013904223u;
      return (seed >> 16) & 0xFFu;
    };
    uint32_t last = 0;
    bool alternates = true;
    bool closed_after_start = true;
    int begins = 0, ends = 0;
    for (int i = 0; i < 20000; ++i) {
      const uint32_t r = next();
      uint32_t sent = 0;
      if (r < 100) {
        sent = t.OnBegin((next() & 3) != 0, (next() & 3) != 0, (next() & 3) != 0);
      } else if (r < 190) {
        sent = t.OnEnd();
      } else {
        sent = t.OnFrameStart();
        if (t.open()) closed_after_start = false;
      }
      if (sent != 0) {
        if (sent == vlink::kMarkerBegin) ++begins;
        if (sent == vlink::kMarkerEnd) ++ends;
        const bool ok = (last == 0 && sent == vlink::kMarkerBegin) || (last == vlink::kMarkerBegin && sent == vlink::kMarkerEnd) || (last == vlink::kMarkerEnd && sent == vlink::kMarkerBegin);
        if (!ok) alternates = false;
        last = sent;
      }
      if (t.open() != (last == vlink::kMarkerBegin)) alternates = false;
    }
    Check(alternates, "random events: what is sent alternates BEGIN / END from a BEGIN, and open() says the same");
    Check(closed_after_start, "random events: a frame's start always leaves the bracket closed");
    Check(begins > 100 && ends > 100, "random events: the run really sent both");
    Check(begins - ends == (t.open() ? 1 : 0), "random events: every BEGIN but an open one has its END");
  }
}

// ---------------------------------------------------------------------------
// the camera
// ---------------------------------------------------------------------------

void TestHorizontalFov() {
  CheckNum(rs::HorizontalFovDeg(90.0f, 1.0f), 90.0, "90 up at 1:1 is 90 across");
  CheckNum(rs::HorizontalFovDeg(53.13010235f, 2.0f), 90.0, "a half height tangent of 0.5 at 2:1 gives a half width tangent of 1: 90 across", 1e-3);
  CheckNum(rs::HorizontalFovDeg(60.0f, 16.0f / 9.0f), 91.49, "60 up at 16:9 is about 91.5 across", 0.01);
  Check(rs::HorizontalFovDeg(0.0f, 1.0f) == 0.0f && rs::HorizontalFovDeg(-5.0f, 1.0f) == 0.0f && rs::HorizontalFovDeg(30.0f, 0.0f) == 0.0f, "no field, no aspect: 0");
  Check(rs::HorizontalFovDeg(kNaN, 1.0f) == 0.0f && rs::HorizontalFovDeg(30.0f, kNaN) == 0.0f && rs::HorizontalFovDeg(30.0f, kInf) == 0.0f && rs::HorizontalFovDeg(kInf, 1.0f) == 0.0f, "NaN and infinity: 0");
  Check(rs::HorizontalFovDeg(180.0f, 1.0f) == 0.0f && rs::HorizontalFovDeg(200.0f, 1.0f) == 0.0f, "a vertical field of 180 or more: 0");
  Check(rs::HorizontalFovDeg(26.0f, 229.0f / 44.0f) > 95.0f && rs::HorizontalFovDeg(26.0f, 229.0f / 44.0f) < 105.0f, "the design's stamp: 26 up across 229x44 is about 100 across");
}

void TestSharedCamera() {
  vlink::Request none;
  const rs::Camera game = rs::SharedCamera(25.0f, 5.2045f, none);
  Check(!game.applied && game.fov_deg == 25.0f && game.aspect == 5.2045f, "no request: the game's own field and aspect, unchanged");

  vlink::Request req;
  req.hfov_deg = 90.0f;
  req.vfov_deg = 53.13010235f;
  const rs::Camera c = rs::SharedCamera(25.0f, 5.2045f, req);
  Check(c.applied, "a request with a field is used");
  CheckNum(c.fov_deg, 53.1301, "f1 is the requested vertical field", 1e-3);
  CheckNum(c.aspect, 2.0, "f2 is the aspect that gives the requested field across", 1e-3);

  // The relation holds over a grid: the field across that the game's projection gives for (f1, f2) is the requested one.
  bool grid_ok = true;
  for (float h = 40.0f; h <= 110.0f; h += 10.0f) {
    for (float v = 20.0f; v <= 45.0f; v += 5.0f) {
      vlink::Request r;
      r.hfov_deg = h;
      r.vfov_deg = v;
      const rs::Camera cc = rs::SharedCamera(25.0f, 5.2f, r);
      if (!cc.applied || std::fabs(cc.fov_deg - v) > 1e-4f || std::fabs(rs::HorizontalFovDeg(cc.fov_deg, cc.aspect) - h) > 0.02f) grid_ok = false;
    }
  }
  Check(grid_ok, "over the design's range the camera's field across is the requested one");

  // the design's side mirrors' view: 69 x 44 degrees
  vlink::Request side;
  side.hfov_deg = 69.0f;
  side.vfov_deg = 44.0f;
  const rs::Camera sc = rs::SharedCamera(25.0f, 5.2f, side);
  CheckNum(rs::HorizontalFovDeg(sc.fov_deg, sc.aspect), 69.0, "69 x 44: 69 across", 0.01);
  CheckNum(sc.aspect, std::tan(34.5 * 3.14159265358979 / 180.0) / std::tan(22.0 * 3.14159265358979 / 180.0), "69 x 44: the aspect is the tangents' ratio", 1e-4);

  // what is not believed is not used
  for (float bad : {4.9f, 170.1f, 300.0f}) {
    vlink::Request r;
    r.hfov_deg = bad;
    r.vfov_deg = 30.0f;
    Check(!rs::SharedCamera(25.0f, 5.2f, r).applied, "a horizontal field of " + std::to_string(bad) + " is not used");
    r.hfov_deg = 80.0f;
    r.vfov_deg = bad;
    Check(!rs::SharedCamera(25.0f, 5.2f, r).applied, "a vertical field of " + std::to_string(bad) + " is not used");
  }
  vlink::Request wide;
  wide.hfov_deg = 170.0f;
  wide.vfov_deg = 5.0f;
  Check(!rs::SharedCamera(25.0f, 5.2f, wide).applied, "a field whose aspect is absurd (over 20:1) is not used");
  vlink::Request tall;
  tall.hfov_deg = 5.0f;
  tall.vfov_deg = 170.0f;
  Check(!rs::SharedCamera(25.0f, 5.2f, tall).applied, "nor one under 1:5");
  vlink::Request nan_req;
  nan_req.hfov_deg = kNaN;
  nan_req.vfov_deg = 30.0f;
  Check(!rs::SharedCamera(25.0f, 5.2f, nan_req).applied, "NaN is not used");
}

void TestBuildCamera() {
  const float fov = 25.0f, aspect = 5.2045f, far_m = 1500.0f;
  vlink::Request none;
  vlink::Request req;
  req.hfov_deg = 69.0f;
  req.vfov_deg = 44.0f;

  // Everything off: the game's own, bit for bit (the old behaviour).
  {
    const rs::CameraBuild b = rs::BuildCamera(fov, aspect, far_m, false, none, 1.0, 0.0);
    Check(b.fov_deg == fov && b.aspect == aspect && b.far_m == far_m && !b.applied, "all off: the game's own field, aspect and far plane, bit for bit");
  }
  // A request nobody withdrew changes nothing while the guest half is off.
  {
    const rs::CameraBuild b = rs::BuildCamera(fov, aspect, far_m, false, req, 1.0, 0.0);
    Check(b.fov_deg == fov && b.aspect == aspect && !b.applied, "guest half off: a request is not read");
  }
  // The cheap options as before (rear_view_cheap.h), with the guest half off.
  {
    const rs::CameraBuild b = rs::BuildCamera(fov, aspect, far_m, false, none, 0.5, 250.0);
    CheckNum(b.fov_deg, 12.5, "off: the field scale narrows the game's field");
    Check(b.aspect == aspect, "off: the aspect is left");
    CheckNum(b.far_m, 250.0, "off: the far plane is pulled in");
  }
  // Guest half on, a request: its field and aspect; the field scale does not touch it, the far plane still comes in.
  {
    const rs::CameraBuild b = rs::BuildCamera(fov, aspect, far_m, true, req, 0.5, 250.0);
    Check(b.applied, "on: the request is used");
    CheckNum(b.fov_deg, 44.0, "on: f1 is the requested vertical field, the scale does not narrow it");
    CheckNum(rs::HorizontalFovDeg(b.fov_deg, b.aspect), 69.0, "on: and the field across is the requested one", 0.01);
    CheckNum(b.far_m, 250.0, "on: the far plane is pulled in");
  }
  // Guest half on, no request: the game's own field (the retail look), the scale still narrows it.
  {
    const rs::CameraBuild b = rs::BuildCamera(fov, aspect, far_m, true, none, 1.0, 0.0);
    Check(!b.applied && b.fov_deg == fov && b.aspect == aspect && b.far_m == far_m, "on, no request: the game's own, bit for bit");
    const rs::CameraBuild s = rs::BuildCamera(fov, aspect, far_m, true, none, 0.5, 0.0);
    CheckNum(s.fov_deg, 12.5, "on, no request: the field scale narrows the game's own");
  }
  // A request that is not believed falls back to the game's own and the scale then applies.
  {
    vlink::Request bad;
    bad.hfov_deg = 400.0f;
    bad.vfov_deg = 44.0f;
    const rs::CameraBuild b = rs::BuildCamera(fov, aspect, far_m, true, bad, 0.5, 0.0);
    Check(!b.applied && std::fabs(b.fov_deg - 12.5f) < 1e-4f && b.aspect == aspect, "an unbelievable request: the game's own, narrowed by the scale");
  }
}

// ---------------------------------------------------------------------------
// the cadence
// ---------------------------------------------------------------------------

void TestEffectiveEvery() {
  vlink::Request r;
  r.every = 2;
  Check(rs::EffectiveEvery(1, r, false) == 1, "off: the setting alone (the host's cadence is not read)");
  Check(rs::EffectiveEvery(3, r, false) == 3, "off: the setting alone, 3");
  Check(rs::EffectiveEvery(1, r, true) == 2, "on: the host's cadence when the setting is 1");
  Check(rs::EffectiveEvery(4, r, true) == 4, "on: the larger of the two, the setting's");
  Check(rs::EffectiveEvery(2, r, true) == 2, "on: equal");
  Check(rs::EffectiveEvery(0, r, true) == 2 && rs::EffectiveEvery(-3, r, false) == 1, "a setting under 1 is 1");
  Check(rs::EffectiveEvery(99, r, true) == int(vlink::kMaxEvery) && rs::EffectiveEvery(99, r, false) == int(vlink::kMaxEvery), "never over the top of the range");
  vlink::Request big;
  big.every = 200;
  Check(rs::EffectiveEvery(1, big, true) == int(vlink::kMaxEvery), "a host cadence over the range is held at the top");
}

}  // namespace

int main() {
  TestMarkers();
  TestCenti();
  TestRequestPacking();
  TestGuestPacking();
  TestCameraPacking();
  TestStampFlag();
  TestStates();
  TestRedirectGate();
  TestMarkerTracker();
  TestHorizontalFov();
  TestSharedCamera();
  TestBuildCamera();
  TestEffectiveEvery();
  std::printf("rear_view_shared_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
