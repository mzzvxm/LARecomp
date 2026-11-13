// Check of the camera cut requests and the motion blur constants on a cut (cut_request.h). A program of its own: it needs no
// renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_cut_request_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_cut_request_test
//
// The compile by hand: clang++ -std=c++23 cut_request_test.cpp cut_request.cpp -o cut_request_test.exe

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "cut_request.h"

using namespace mcla::gpu::cut;

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

void CheckNum(double got, double want, const std::string& what, double tol = 1e-9) {
  ++g_checks;
  if (!(std::fabs(got - want) <= tol)) {
    ++g_failed;
    std::printf("FAILED: %s: got %.9g, want %.9g\n", what.c_str(), got, want);
  }
}

// A camera at `pos` looking along yaw (degrees, 0 = +Z, 90 = +X), level, with a roll.
CameraPose Cam(double x, double y, double z, double yaw_deg, double roll_deg = 0.0) {
  const double yaw = yaw_deg * 3.14159265358979323846 / 180.0;
  const double roll = roll_deg * 3.14159265358979323846 / 180.0;
  CameraPose p;
  p.position[0] = x;
  p.position[1] = y;
  p.position[2] = z;
  p.forward[0] = std::sin(yaw);
  p.forward[1] = 0.0;
  p.forward[2] = std::cos(yaw);
  const double right[3] = {std::cos(yaw), 0.0, -std::sin(yaw)};
  const double up0[3] = {0.0, 1.0, 0.0};
  for (int c = 0; c < 3; ++c) p.up[c] = up0[c] * std::cos(roll) - right[c] * std::sin(roll);
  return p;
}

void TestSameCamera() {
  const CameraPose a = Cam(100, 5, -50, 30);
  Check(SameCamera(a, a), "a camera is the same as itself");
  Check(SameCamera(a, Cam(100.05, 5, -50, 30)), "5 cm apart is the same frame (the game rounds to floats)");
  Check(!SameCamera(a, Cam(100.2, 5, -50, 30)), "20 cm apart is not");
  Check(SameCamera(a, Cam(100, 5, -50, 30.4)), "0.4 degrees of turn is the same");
  Check(!SameCamera(a, Cam(100, 5, -50, 32)), "2 degrees of turn is not");
  Check(!SameCamera(a, Cam(100, 5, -50, 30, 3.0)), "3 degrees of roll is not (up differs)");
  Check(!SameCamera(a, Cam(100, 5, -50, 70)), "a 40 degree turn is not");
  Check(!SameCamera(a, Cam(110, 5, -50, 30)), "a 10 m jump is not");
}

void TestRequests() {
  const CameraPose before = Cam(100, 5, -50, 30);
  // E5's defect: a cut that only turns. The frame before it stands where the cut frame stands, so a match by position would have
  // fired on it, one frame early.
  const CameraPose turned = Cam(100, 5, -50, 70);
  Requests r;
  Check(!r.Pending(), "nothing pending at first");
  r.Submit(3914, turned);
  Check(r.Pending(), "pending after a submit");
  Requests::Taken t = r.Take(before);
  Check(!t.hit, "the frame before a turn-only cut does not match (E5: it fired a frame early)");
  Check(r.Pending(), "... and the request waits");
  t = r.Take(turned);
  Check(t.hit && t.game_frame == 3914 && t.age == 2 && t.skipped == 0, "the cut frame matches: game frame 3914, the second anchor");
  Check(!r.Pending(), "taken once");
  Check(!r.Take(turned).hit, "the frame after (same camera again) does not match a request that is gone");

  // a jump
  r.Submit(500, Cam(120, 5, -50, 30));
  Check(!r.Take(before).hit, "the frame before a jump does not match");
  Check(r.Take(Cam(120, 5, -50, 30)).hit, "the frame at the new place does");

  // two requests, the frame of the older one was never seen: the newer one takes it with it
  Requests s;
  s.Submit(10, Cam(0, 0, 0, 0));
  s.Submit(11, Cam(500, 0, 0, 0));
  t = s.Take(Cam(500, 0, 0, 0));
  Check(t.hit && t.game_frame == 11 && t.skipped == 1, "the newer request matched; the older went with it");
  Check(!s.Pending(), "nothing left");
  Check(!s.Take(Cam(0, 0, 0, 0)).hit, "the older frame is over");
  Check(s.counts().submitted == 2 && s.counts().matched == 1 && s.counts().skipped == 1, "the counts");

  // two cuts in a row are both found, in order
  Requests u;
  u.Submit(20, Cam(0, 0, 0, 0));
  u.Submit(21, Cam(500, 0, 0, 0));
  Check(u.Take(Cam(0, 0, 0, 0)).game_frame == 20, "the older is found first");
  Check(u.Take(Cam(500, 0, 0, 0)).game_frame == 21, "then the newer");

  // never met: given up after kGiveUpAfter anchors, once, and reported
  Requests e;
  e.Submit(99, Cam(7, 7, 7, 7));
  uint32_t gave_up = 0;
  uint64_t gave_up_serial = 0;
  for (uint32_t i = 0; i < kGiveUpAfter + 5; ++i) {
    const Requests::Taken x = e.Take(before);
    if (x.expired) {
      gave_up += x.expired;
      gave_up_serial = x.expired_serial;
      Check(x.expired_game_frame == 99, "the dropped request names its game frame");
    }
    Check(!x.hit, "nothing matches");
  }
  Check(gave_up == 1 && gave_up_serial == 1, "dropped once, after waiting");
  Check(!e.Pending(), "and gone");
  Check(e.counts().expired == 1, "counted");

  // a flood is bounded
  Requests f;
  for (uint32_t i = 0; i < 40; ++i) f.Submit(i, Cam(double(i) * 10, 0, 0, 0));
  Check(f.counts().submitted == 40 && f.counts().expired == 24, "at most 16 wait: the oldest are dropped");
  Check(f.Take(Cam(390, 0, 0, 0)).game_frame == 39, "the newest is still there");
}

void TestThreads() {
  // The game thread submits while the render thread takes (and nothing matches): every request is counted once, and all of them
  // are dropped in the end, none lost and none counted twice.
  Requests r;
  constexpr uint32_t kN = 5000;
  std::atomic<bool> done{false};
  std::thread producer([&] {
    for (uint32_t i = 0; i < kN; ++i) {
      r.Submit(i, Cam(double(i) * 5.0, 0, 0, 0));
      if (i % 16 == 0) std::this_thread::yield();
    }
    done.store(true);
  });
  std::thread consumer([&] {
    while (!done.load()) {
      r.Take(Cam(-1.0e6, 0, 0, 0));
    }
  });
  producer.join();
  consumer.join();
  for (uint32_t i = 0; i < kGiveUpAfter + 20; ++i) r.Take(Cam(-1.0e6, 0, 0, 0));
  const Requests::Counts c = r.counts();
  Check(c.submitted == kN, "every submit was counted");
  Check(c.matched == 0 && c.skipped == 0, "nothing matched");
  Check(c.expired == kN, "every request was dropped exactly once (" + std::to_string(c.expired) + " of " + std::to_string(kN) + ")");
  Check(!r.Pending(), "nothing is left");
}

// ------------------------------------------------------------------------------------------------------------ the anchor's constants

void PutBe(uint8_t* bank, uint32_t reg, uint32_t comp, float v) {
  uint32_t u;
  std::memcpy(&u, &v, 4);
  uint8_t* p = bank + reg * 16 + comp * 4;
  p[0] = uint8_t(u >> 24);
  p[1] = uint8_t(u >> 16);
  p[2] = uint8_t(u >> 8);
  p[3] = uint8_t(u);
}

void TestCameraFromBank() {
  std::array<uint8_t, 256 * 16> bank{};
  // right, up, backward, position of a camera at (10, 20, 30) looking along +X, rolled a little
  const double yaw = 3.14159265358979323846 / 2;
  const float up[3] = {0.0f, 1.0f, 0.0f};
  const float back[3] = {-float(std::sin(yaw)), 0.0f, -float(std::cos(yaw))};
  for (uint32_t c = 0; c < 3; ++c) {
    PutBe(bank.data(), 13, c, up[c] * 2.0f);  // not unit: the reader normalises
    PutBe(bank.data(), 14, c, back[c]);
  }
  PutBe(bank.data(), 15, 0, 10.0f);
  PutBe(bank.data(), 15, 1, 20.0f);
  PutBe(bank.data(), 15, 2, 30.0f);
  CameraPose p;
  Check(CameraFromGuestBank(bank.data(), &p), "a camera is read from c12 to c15");
  CheckNum(p.position[0], 10.0, "position x");
  CheckNum(p.position[1], 20.0, "position y");
  CheckNum(p.position[2], 30.0, "position z");
  CheckNum(p.forward[0], 1.0, "forward is minus the backward row: +X", 1e-6);
  CheckNum(p.forward[2], 0.0, "forward z", 1e-6);
  CheckNum(p.up[1], 1.0, "up is unit", 1e-6);
  // the match of a hook camera against the bank's camera
  CameraPose hook = Cam(10, 20, 30, 90);
  Check(SameCamera(hook, p), "the hook's camera and the bank's agree");

  PutBe(bank.data(), 15, 1, std::nanf(""));
  Check(!CameraFromGuestBank(bank.data(), &p), "a NaN position is no camera");
  PutBe(bank.data(), 15, 1, 20.0f);
  for (uint32_t c = 0; c < 3; ++c) PutBe(bank.data(), 14, c, 0.0f);
  Check(!CameraFromGuestBank(bank.data(), &p), "no backward axis is no camera");
}

struct Mat {
  double m[4][4];
};

// A perspective-like view-projection (row vectors, y up), not any real camera: what matters is that it is a general matrix.
Mat SomeViewProjection() {
  Mat v = {{{1.46538, -0.292235, 0.633472, 0.633155},
            {0.31, 2.2, 0.114, 0.12},
            {-0.4, 0.05, 1.0005, 0.9},
            {12.5, -3.25, -4.99, 5.0}}};
  return v;
}

void TestNeutralize() {
  std::vector<float> bank(256 * 4, -777.0f);
  const Mat vp = SomeViewProjection();
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) bank[(8 + r) * 4 + c] = float(vp.m[r][c]);
  }
  bank[62 * 4 + 0] = 1280.0f;
  bank[62 * 4 + 1] = 720.0f;
  bank[62 * 4 + 2] = 1.0f / 1280.0f;
  bank[62 * 4 + 3] = 1.0f / 720.0f;
  // the game's own values of an old shot, to be overwritten, and the neighbours that must not be
  for (uint32_t k = 0; k < kBlurSlots; ++k) {
    for (uint32_t r = 0; r < 4; ++r) {
      for (uint32_t c = 0; c < 4; ++c) bank[(kBlurFirstRegister + 4 * k + r) * 4 + c] = 1e29f;
    }
  }
  bank[68 * 4] = 4242.0f;
  bank[165 * 4] = 1280.0f;  // c165: the frame size, the register after the slots
  Check(NeutralizeMotionBlur(bank.data()), "the bank is made neutral");
  // the census's own numbers: c69 row 0 = [1343.06 333.141 0.633472 0.633155] for this VP row at 1280x720
  CheckNum(bank[69 * 4 + 0], 1343.06, "c69 x: 640 (a + d) from the census", 0.01);
  CheckNum(bank[69 * 4 + 1], 333.141, "c69 y: 360 (d - b) from the census", 0.01);
  CheckNum(bank[69 * 4 + 2], 0.633472, "c69 z is the view-projection's", 1e-6);
  CheckNum(bank[69 * 4 + 3], 0.633155, "c69 w is the view-projection's", 1e-6);
  // every slot is the same matrix
  bool same = true;
  for (uint32_t k = 1; k < kBlurSlots; ++k) {
    for (uint32_t i = 0; i < 16; ++i) {
      if (bank[(kBlurFirstRegister + 4 * k) * 4 + i] != bank[kBlurFirstRegister * 4 + i]) same = false;
    }
  }
  Check(same, "all 24 slots hold the same matrix: no vehicle moves either");
  CheckNum(bank[68 * 4], 4242.0, "c68 is not touched");
  CheckNum(bank[165 * 4], 1280.0, "c165 is not touched");
  CheckNum(bank[8 * 4 + 0], vp.m[0][0], "the view-projection is not touched", 1e-6);

  // The pixel a world point lands on through the neutral matrix is the pixel the view-projection gives it: no streak.
  const double p[4] = {3.0, -1.5, 7.0, 1.0};
  double clip[4] = {};
  double pix[4] = {};
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      clip[c] += p[r] * vp.m[r][c];
      pix[c] += p[r] * double(bank[(kBlurFirstRegister + r) * 4 + c]);
    }
  }
  const double ndc_x = clip[0] / clip[3];
  const double ndc_y = clip[1] / clip[3];
  CheckNum(pix[0] / pix[3], (ndc_x * 0.5 + 0.5) * 1280.0, "the neutral matrix puts a point at its own pixel x (edge convention)", 1e-2);
  CheckNum(pix[1] / pix[3], (0.5 - ndc_y * 0.5) * 720.0, "... and its own pixel y (flipped)", 1e-2);

  // not usable: nothing is written
  std::vector<float> bad = bank;
  bad[62 * 4] = 0.0f;
  bad[69 * 4] = 5.0f;
  Check(!NeutralizeMotionBlur(bad.data()) && bad[69 * 4] == 5.0f, "a zero frame size: refused and nothing written");
  bad = bank;
  bad[9 * 4 + 1] = std::nanf("");
  bad[69 * 4] = 5.0f;
  Check(!NeutralizeMotionBlur(bad.data()) && bad[69 * 4] == 5.0f, "a NaN view-projection: refused and nothing written");
}

}  // namespace

int main() {
  TestSameCamera();
  TestRequests();
  TestThreads();
  TestCameraFromBank();
  TestNeutralize();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
