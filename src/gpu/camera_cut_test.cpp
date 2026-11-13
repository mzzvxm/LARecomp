// Check of the camera cut rule (temporal_camera.h, temporal_cut.cpp). A program of its own: it needs
// no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_camera_cut_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_camera_cut_test [camera trace csv ...]
//
// The sequences are of two kinds, and each says which:
//   RECORDED  numbers copied out of the game's own logs: the drive's camera trace (the pacing trace's
//             cam_x..cam_bz columns, ports/run/cm2-base-60 and cam-125), the lens lines of the
//             opening fly-in and the cut lines of the pause camera, the cutscene and the race exit
//             (ports/run/um-race-dlss-before). Where the log has only the frames that changed
//             (the fly-in's lens lines skip frames) the rows are used as consecutive frames, which
//             makes each step larger than it was: a harder test.
//   MADE UP   sequences the logs do not have (the garage turntable, the map, a camera that updates
//             every second frame, a roll): built to the scale the notes in temporal_camera.h give.
// The earlier rule is written out again below (OldTestCut) so each check can say what it did
// differently; it is the rule of the main branch before this one.
//
// With camera trace csv files as arguments (columns frame, cam_x, cam_y, cam_z and optionally
// cam_bx, cam_by, cam_bz: the pacing trace of ports/run/cm2-* and cam-*) the program also prints,
// for each file, how many cuts the old and the new rule find in it and the first few of the new.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "temporal_camera.h"

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

constexpr double kPi = 3.14159265358979323846;
double Rad(double degrees) { return degrees * kPi / 180.0; }

using V3 = std::array<double, 3>;

V3 Sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double Dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 Cross(const V3& a, const V3& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
V3 Unit(const V3& a) {
  const double n = std::sqrt(Dot(a, a));
  return n > 1e-12 ? V3{a[0] / n, a[1] / n, a[2] / n} : a;
}

// A camera from a position, a forward vector and an up hint. The game's axes are the rows of the
// view inverse: right, up and BACK, a right-handed set (the camera looks along minus the third
// row): right = up x back, up = back x right. `axes` false makes the camera as the occlusion's
// hand-made one is: position and forward only.
Camera MakeCamera(const V3& position, const V3& forward, const V3& up_hint, double fov_degrees,
                  double near_plane, bool axes = true) {
  Camera c;
  const V3 f = Unit(forward);
  for (int i = 0; i < 3; ++i) {
    c.position[i] = position[i];
    c.forward[i] = f[i];
  }
  if (axes) {
    const V3 back{-f[0], -f[1], -f[2]};
    const V3 right = Unit(Cross(up_hint, back));
    const V3 up = Cross(back, right);
    for (int i = 0; i < 3; ++i) {
      c.right[i] = right[i];
      c.up[i] = up[i];
    }
  }
  c.fov_y = Rad(fov_degrees);
  c.aspect = 16.0 / 9.0;
  c.near_plane = near_plane;
  c.far_plane = 9000.0;
  return c;
}

// The rule of the main branch before this one (temporal_camera.cpp TestCut as it was): a step of
// more than 25 units that the step before does not predict (allowed 25 plus half the step before),
// a forward vector turn of more than 30 degrees, or a field of view change of more than 5%.
struct OldCut {
  bool fires = false;
  bool by_distance = false;
  bool by_turn = false;
  bool by_fov = false;
};
OldCut OldTestCut(const Camera& now, const Camera& before, const double* previous_step) {
  double dist2 = 0.0, deviation2 = 0.0, previous2 = 0.0, dot = 0.0, n_now = 0.0, n_before = 0.0;
  for (int i = 0; i < 3; ++i) {
    const double d = now.position[i] - before.position[i];
    const double predicted = previous_step ? previous_step[i] : 0.0;
    dist2 += d * d;
    deviation2 += (d - predicted) * (d - predicted);
    previous2 += predicted * predicted;
    dot += now.forward[i] * before.forward[i];
    n_now += now.forward[i] * now.forward[i];
    n_before += before.forward[i] * before.forward[i];
  }
  OldCut cut;
  const double distance = std::sqrt(dist2);
  const double deviation = std::sqrt(deviation2);
  const double tolerance = 25.0 + 0.5 * std::sqrt(previous2);
  const double cosine = dot / std::max(1e-30, std::sqrt(n_now * n_before));
  const double fov = std::fabs(now.fov_y - before.fov_y) / std::max(1e-30, before.fov_y);
  cut.by_distance = distance > 25.0 && deviation > tolerance;
  cut.by_turn = cosine < 0.866;
  cut.by_fov = fov > 0.05;
  cut.fires = cut.by_distance || cut.by_turn || cut.by_fov;
  return cut;
}

struct Outcome {
  std::vector<size_t> now_cuts;  // indexes of the frames the new rule cut at
  std::vector<size_t> old_cuts;  // ... the old rule (kept as the temporal system kept its step)
  std::vector<Cut> now_detail;   // the new rule's result for each frame (index 0 unused)
};

// The sequence through both rules the way the temporal system runs them: the step is remembered
// after each frame; the new rule forgets it at a cut (CutMotion::Accept), the old kept it.
Outcome Run(const std::vector<Camera>& cams) {
  Outcome out;
  out.now_detail.resize(cams.size());
  CutMotion motion;
  double old_step[3] = {};
  bool old_known = false;
  for (size_t i = 1; i < cams.size(); ++i) {
    const Cut cut = TestCut(cams[i], cams[i - 1], motion);
    out.now_detail[i] = cut;
    motion.Accept(cut);
    if (cut.fires) {
      out.now_cuts.push_back(i);
    }
    const OldCut old = OldTestCut(cams[i], cams[i - 1], old_known ? old_step : nullptr);
    if (old.fires) {
      out.old_cuts.push_back(i);
    }
    for (int k = 0; k < 3; ++k) {
      old_step[k] = cams[i].position[k] - cams[i - 1].position[k];
    }
    old_known = true;
  }
  return out;
}

bool Has(const std::vector<size_t>& cuts, size_t index) {
  return std::find(cuts.begin(), cuts.end(), index) != cuts.end();
}

std::string List(const std::vector<size_t>& cuts) {
  std::string s;
  for (size_t c : cuts) {
    s += (s.empty() ? "" : ",") + std::to_string(c);
  }
  return s.empty() ? "none" : s;
}

void Report(const char* name, const Outcome& o) {
  std::printf("  %-34s old rule cuts at [%s]   new rule cuts at [%s]\n", name, List(o.old_cuts).c_str(),
              List(o.now_cuts).c_str());
}

// ----------------------------------------------------------------------------- RECORDED sequences

// RECORDED: the drive's camera around the placement of the car (ports/run/cm2-base-60/camera.csv,
// frames 1726-1757): position and the camera's third row (the back). Five frames at rest, then the
// car is placed 1,600 m away (the map warp) and the camera settles: steps of 4.6, 3.0, 1.8 m.
struct DriveRow {
  double x, y, z, bx, by, bz;
};
constexpr DriveRow kDriveTeleport[] = {
    {823.7375, 19.5748, -815.9604, 0.47466, 0.06995, -0.87739},
    {823.7375, 19.5748, -815.9604, 0.47466, 0.06995, -0.87739},
    {823.7375, 19.5748, -815.9604, 0.47466, 0.06995, -0.87739},
    {823.7375, 19.5748, -815.9604, 0.47466, 0.06995, -0.87739},
    {823.7375, 19.5748, -815.9604, 0.47466, 0.06995, -0.87739},
    {-770.0851, 20.7430, -646.9406, -0.21973, 0.07466, -0.97270},  // frame 1731: the placement
    {-774.1570, 20.7175, -644.7501, -0.69363, 0.07192, -0.71673},
    {-775.9179, 20.6680, -642.3464, -0.89767, 0.06676, -0.43559},
    {-776.5494, 20.6385, -640.6559, -0.96929, 0.06409, -0.23744},
    {-776.7926, 20.5924, -639.6154, -0.99162, 0.05915, -0.11486},
    {-776.8636, 20.5851, -638.8686, -0.99783, 0.05888, -0.02946},
    {-776.8675, 20.5957, -638.3903, -0.99785, 0.06072, 0.02477},
    {-776.8550, 20.6164, -638.0785, -0.99619, 0.06356, 0.05979},
    {-776.8396, 20.6462, -637.8701, -0.99428, 0.06730, 0.08297},
    {-776.8265, 20.6703, -637.7524, -0.99287, 0.07046, 0.09617},
    {-776.8163, 20.6926, -637.6841, -0.99188, 0.07339, 0.10389},
    {-776.8079, 20.7132, -637.6442, -0.99118, 0.07608, 0.10847},
    {-776.8002, 20.7320, -637.6208, -0.99069, 0.07853, 0.11119},
    {-776.7930, 20.7491, -637.6073, -0.99033, 0.08074, 0.11281},
    {-776.7858, 20.7646, -637.5997, -0.99006, 0.08274, 0.11375},
    {-776.7787, 20.7788, -637.5956, -0.98984, 0.08453, 0.11433},
    {-776.7719, 20.7917, -637.5936, -0.98966, 0.08613, 0.11465},
    {-776.7652, 20.8037, -637.5927, -0.98952, 0.08757, 0.11486},
    {-776.7590, 20.8148, -637.5924, -0.98939, 0.08886, 0.11497},
    {-776.7532, 20.8251, -637.5923, -0.98927, 0.09001, 0.11505},
    {-776.7481, 20.8348, -637.5925, -0.98917, 0.09105, 0.11510},
    {-776.7436, 20.8439, -637.5928, -0.98908, 0.09198, 0.11513},
    {-776.7393, 20.8553, -637.5931, -0.98897, 0.09310, 0.11516},
    {-776.7358, 20.8658, -637.5932, -0.98888, 0.09412, 0.11518},
    {-776.7327, 20.8756, -637.5933, -0.98879, 0.09501, 0.11520},
    {-776.7299, 20.8846, -637.5934, -0.98871, 0.09581, 0.11521},
    {-776.7277, 20.8925, -637.5936, -0.98864, 0.09649, 0.11522},
};
constexpr size_t kPlacement = 5;  // the row of frame 1731

// RECORDED: the same drive's camera 20 frames around a frame-time hitch (ports/run/cam-125/cam.csv,
// frames 8880-8899, the 153 ms step at frame 8889: the camera's step is 2.9 m off the one before).
// The orientation is not in that file: it looks along the last row of the placement table.
constexpr V3 kHitch[] = {
    {1018.199, 30.935, -1216.262}, {1018.406, 30.901, -1215.837}, {1018.616, 30.865, -1215.405},
    {1018.811, 30.835, -1215.003}, {1019.009, 30.802, -1214.592}, {1019.211, 30.768, -1214.172},
    {1019.409, 30.736, -1213.760}, {1019.614, 30.703, -1213.333}, {1019.810, 30.674, -1212.922},
    {1021.088, 30.330, -1210.252}, {1021.140, 30.347, -1210.140}, {1021.347, 30.408, -1209.711},
    {1021.551, 30.391, -1209.269}, {1021.739, 30.368, -1208.857}, {1021.931, 30.343, -1208.435},
    {1022.128, 30.318, -1208.001}, {1022.315, 30.299, -1207.589}, {1022.501, 30.280, -1207.178},
    {1022.691, 30.260, -1206.756}, {1022.881, 30.241, -1206.332},
};

// RECORDED: the end of the opening fly-in at 30 frames a second (ports/run/cm2-base-30/camera.csv,
// frames 706-722): the camera drops straight down towards the car at about 2 to 3 m per ms. A 73 ms
// stall at frame 713 makes the step of frame 714 (140 m after 73) twice the one before, and the steps
// after it follow the lengths of the frames that came with it (14, 33, 52 m, then 48, 43, 40).
constexpr DriveRow kFlyInStall[] = {
    {846.1309, 1305.6475, -858.8271, 0.00000, 1.00000, 0.00111},
    {847.4923, 1196.2817, -859.9532, 0.00000, 1.00000, 0.00111},
    {847.3958, 1092.7217, -859.7728, 0.00014, 1.00000, 0.00088},
    {847.0939, 995.6582, -859.2139, 0.00061, 1.00000, 0.00008},
    {846.6017, 905.1777, -858.3026, 0.00146, 1.00000, -0.00137},
    {845.9316, 820.9150, -857.0618, 0.00273, 0.99999, -0.00356},
    {845.0966, 742.6406, -855.5157, 0.00444, 0.99997, -0.00655},
    {844.1066, 669.9111, -853.6829, 0.00660, 0.99992, -0.01038},
    {841.4764, 529.9062, -848.8128, 0.01195, 0.99973, -0.01994},  // frame 714: the long step
    {841.1458, 515.7305, -848.2007, 0.01433, 0.99960, -0.02432},
    {840.3298, 483.0449, -846.6899, 0.01616, 0.99949, -0.02764},
    {838.8744, 431.5039, -843.9952, 0.01955, 0.99924, -0.03386},
    {837.2943, 383.2803, -841.0696, 0.02365, 0.99886, -0.04145},
    {835.6584, 339.9580, -838.0408, 0.02793, 0.99839, -0.04942},
    {834.0551, 300.3457, -835.0721, 0.03232, 0.99782, -0.05757},
    {832.6141, 264.5117, -832.4042, 0.03697, 0.99713, -0.06602},
    {831.3177, 232.0371, -830.0038, 0.04208, 0.99628, -0.07523},
};

// RECORDED: the same place at 60 frames a second (ports/run/cm2-base-60/camera.csv, frames 1298-1311):
// a 64 ms stall before frame 1305 and the step of frame 1306 is 129 m after 36 (3.6 times), then 15, 27,
// 28, 28, 27 m.
constexpr DriveRow kFlyInStall60[] = {
    {847.0953, 986.4595, -859.1221, 0.00070, 1.00000, -0.00007},
    {846.8616, 940.5000, -858.6902, 0.00112, 1.00000, -0.00078},
    {846.5809, 896.0415, -858.1716, 0.00164, 1.00000, -0.00167},
    {846.2552, 853.1411, -857.5697, 0.00227, 0.99999, -0.00275},
    {845.8851, 811.6606, -856.8856, 0.00301, 0.99999, -0.00405},
    {845.4749, 771.8730, -856.1277, 0.00387, 0.99998, -0.00555},
    {845.0108, 732.4258, -855.2700, 0.00487, 0.99996, -0.00730},
    {844.5305, 696.2471, -854.3825, 0.00595, 0.99994, -0.00921},
    {842.3160, 566.9531, -850.2903, 0.01027, 0.99981, -0.01687},  // frame 1306: the long step
    {841.9998, 551.9072, -849.7060, 0.01223, 0.99972, -0.02047},
    {841.4027, 525.1621, -848.6025, 0.01360, 0.99964, -0.02295},
    {840.7219, 496.9893, -847.3445, 0.01532, 0.99954, -0.02607},
    {839.9968, 469.3115, -846.0045, 0.01720, 0.99942, -0.02951},
    {839.2387, 442.5850, -844.6035, 0.01919, 0.99927, -0.03315},
};

// RECORDED: the opening fly-in's lens lines (ports/run/um-race-dlss-before/larecomp.log, frames
// 1865-1907 of "[gpu] temporal: lens in frame"): the camera rises from 9 to 1,313 m, tipping from
// level to straight down, the field of view narrowing from 41 to 31 degrees, near plane 0.5 and, at
// the last two rows, 201 and 313. The log has only the frames whose lens moved by 1%, so the rows
// are 1 to 3 frames apart and are used here as consecutive frames: each step is up to three times
// what the camera really took.
struct FlyRow {
  int frame;
  double fov;
  double near_plane;
  V3 position;
  V3 forward;
};
constexpr FlyRow kFlyIn[] = {
    {1865, 41.000, 0.500, {2531.00, 9.00, 141.80}, {-0.996, 0.088, 0.010}},
    {1873, 40.518, 0.500, {2530.13, 11.74, 141.79}, {-0.997, 0.081, 0.008}},
    {1876, 40.096, 0.500, {2528.26, 17.63, 141.77}, {-0.998, 0.066, 0.004}},
    {1879, 39.553, 0.500, {2524.33, 29.98, 141.73}, {-0.999, 0.034, -0.005}},
    {1881, 39.128, 0.500, {2520.03, 43.50, 141.68}, {-1.000, -0.001, -0.015}},
    {1883, 38.657, 0.500, {2513.95, 62.64, 141.62}, {-0.998, -0.051, -0.029}},
    {1885, 38.142, 0.500, {2505.66, 88.73, 141.53}, {-0.992, -0.118, -0.048}},
    {1887, 37.589, 0.500, {2494.70, 123.21, 141.42}, {-0.976, -0.207, -0.072}},
    {1889, 37.000, 0.500, {2480.57, 167.64, 141.27}, {-0.943, -0.318, -0.101}},
    {1891, 36.381, 0.500, {2462.76, 223.69, 141.08}, {-0.882, -0.452, -0.131}},
    {1893, 35.736, 0.500, {2440.69, 293.10, 140.84}, {-0.781, -0.604, -0.158}},
    {1895, 35.071, 0.500, {2413.80, 377.69, 140.56}, {-0.631, -0.757, -0.169}},
    {1897, 34.389, 0.500, {2381.48, 479.37, 140.21}, {-0.465, -0.871, -0.162}},
    {1898, 34.044, 0.500, {2363.09, 537.21, 140.02}, {-0.383, -0.911, -0.152}},
    {1899, 33.697, 0.500, {2343.12, 600.07, 139.81}, {-0.305, -0.942, -0.138}},
    {1900, 33.349, 0.500, {2321.47, 668.17, 139.58}, {-0.233, -0.965, -0.120}},
    {1901, 33.000, 0.500, {2298.07, 741.78, 139.33}, {-0.169, -0.981, -0.099}},
    {1902, 32.651, 0.500, {2272.84, 821.15, 139.06}, {-0.114, -0.991, -0.076}},
    {1903, 32.303, 0.500, {2245.70, 906.53, 138.78}, {-0.070, -0.996, -0.053}},
    {1904, 31.956, 0.500, {2216.57, 998.18, 138.47}, {-0.037, -0.999, -0.032}},
    {1905, 31.611, 0.500, {2185.36, 1096.35, 138.14}, {-0.015, -1.000, -0.015}},
    {1906, 31.268, 201.300, {2152.00, 1201.30, 137.78}, {-0.003, -1.000, -0.004}},
    {1907, 30.929, 313.279, {2116.40, 1313.28, 137.40}, {-0.001, -1.000, -0.001}},
};

// RECORDED: the pause camera's zoom as the pause menu opens (the same log, frames 3034-3039): the
// lens is 31.99 degrees for the first three frames, 34.81 (+8.8%) for the next two and 36.92 (+6.1%
// from 34.80) after: it steps every second frame. The camera moves 0.6 and 0.4 m and turns 2.4 and
// 1.8 degrees (the logged turn cosines 0.9991 and 0.9995).
struct PauseRow {
  double fov;
  V3 position;
  double yaw_degrees;  // the logged turns as a cumulative yaw about the vertical
};
constexpr PauseRow kPauseZoom[] = {
    {31.99, {823.8, 19.4, -814.8}, 0.0},  {31.99, {823.8, 19.4, -814.8}, 0.0},
    {31.99, {823.8, 19.4, -814.8}, 0.0},  {34.81, {823.7, 19.3, -814.3}, 2.4},
    {34.81, {823.7, 19.3, -814.3}, 2.4},  {36.92, {823.7, 19.2, -813.9}, 4.2},
    {36.92, {823.7, 19.2, -813.9}, 4.2},
};

// RECORDED: the lens after a race (frames 6668-6672, the camera at 109.7, 13.5, -574.2 and moving
// 0.1 to 0.2 m): 43.52 degrees, then steps of -10.3, -8.9, -7.3 and -5.9% (39.04, 35.56, 32.95,
// 31.02); the log stops after four lines (its cap), the ease went on to about 25 degrees.
constexpr double kRaceExitFov[] = {43.52, 43.52, 43.52, 39.04, 35.56, 32.95, 31.02,
                                   29.5,  28.2,  27.2,  26.4,  25.8,  25.4,  25.2, 25.0};

// RECORDED: the four shot changes of the race intro's cutscene (the same log, frames 4055, 4173,
// 4235, 4292, camera at -641.9, 20.0, -637.4 FOV 21.87 and nearby): the camera moved 9.5, 9.4, 13.8
// and 13.1 m while the forward vectors' cosines were 0.6837, -0.9929, -0.5651 and -0.3932 (the
// last also changed the field of view from 21.87 to 25.0). Before each the camera stood still.
struct CutsceneRow {
  double moved;
  double cosine;
  double fov_before;
  double fov_after;
};
constexpr CutsceneRow kCutscene[] = {
    {9.5, 0.6837, 21.87, 21.87},
    {9.4, -0.9929, 21.87, 21.87},
    {13.8, -0.5651, 21.87, 21.87},
    {13.1, -0.3932, 21.87, 25.00},
};

// ----------------------------------------------------------------------------- the checks

void CheckDriveTeleport() {
  std::vector<Camera> cams;
  for (const DriveRow& r : kDriveTeleport) {
    cams.push_back(MakeCamera({r.x, r.y, r.z}, {-r.bx, -r.by, -r.bz}, {0, 1, 0}, 25.0, 0.2));
  }
  const Outcome o = Run(cams);
  Report("drive placement (1731)", o);
  Check(!Has(o.now_cuts, 1) && !Has(o.now_cuts, 2) && !Has(o.now_cuts, 3) && !Has(o.now_cuts, 4),
        "drive: no cut while the camera stands still before the placement");
  Check(Has(o.now_cuts, kPlacement), "drive: the placement (1,600 m) is a cut");
  Check(Has(o.old_cuts, kPlacement), "drive: the old rule cut the placement too");
  // The camera then swings 41, 31, 20 and 12 degrees (the boom finding the car's heading) taking
  // steps of 4.6, 3.0 and 1.8 m: a swing that slows smoothly. The old rule cut the 31 degree frame
  // too (the cosine of the forward vectors 0.855, under its 0.866); the new rule judges it against
  // the 41 degrees the frame before took, so only the placement is a cut. Every frame after is quiet
  // in both.
  Check(Has(o.old_cuts, kPlacement + 1), "drive: the old rule cut the 31 degree swing after the placement as well");
  Check(!Has(o.now_cuts, kPlacement + 1), "drive: the new rule does not: a swing that slows is not a second cut");
  for (size_t i = kPlacement + 2; i < cams.size(); ++i) {
    Check(!Has(o.now_cuts, i) && !Has(o.old_cuts, i), "drive: no cut while the camera settles, row " + std::to_string(i));
  }
  Check(o.now_cuts.size() == 1 && o.old_cuts.size() == 2, "drive: one cut by the new rule, two by the old, in the whole sequence");
  // The first cut is a jump (and a turn); its reason names them.
  const Cut& first = o.now_detail[kPlacement];
  Check(first.by_distance && first.by_turn && !first.by_fov, "drive: the placement is a jump and a turn, not a lens change");
  const std::string reason = CutReason(first);
  Check(reason.rfind("jump: moved 1602.8", 0) == 0, "drive: the reason names the jump: " + reason);
  Check(reason.find("turn:") != std::string::npos && reason.find("lens:") == std::string::npos,
        "drive: the reason names the turn and not the lens");
}

void CheckDriveHitch() {
  std::vector<Camera> cams;
  for (const V3& p : kHitch) {
    cams.push_back(MakeCamera(p, {-0.99, 0.09, 0.11}, {0, 1, 0}, 25.0, 0.2));
  }
  const Outcome o = Run(cams);
  Report("drive frame-time hitch", o);
  Check(o.now_cuts.empty() && o.old_cuts.empty(), "hitch: a 2.9 m step off the last one is not a cut");
  double worst = 0.0;
  for (size_t i = 1; i < cams.size(); ++i) {
    worst = std::max(worst, o.now_detail[i].deviation);
  }
  Check(worst > 2.0 && worst < 3.5, "hitch: the recorded deviation is about 2.9 m (it is " + std::to_string(worst) + ")");
  Check(o.now_detail[1].floor > 3.0 * worst, "hitch: the floor in play is more than three times the worst deviation recorded in the drive");
}

void CheckFlyIn() {
  // Forward only, as the occlusion's camera is, and with axes: the up vector parallel-transported
  // along the flight, so the roll stays continuous while the camera tips from level to straight down.
  std::vector<Camera> plain;
  std::vector<Camera> with_axes;
  V3 up{0, 1, 0};
  for (const FlyRow& r : kFlyIn) {
    plain.push_back(MakeCamera(r.position, r.forward, {0, 1, 0}, r.fov, r.near_plane, false));
    const V3 f = Unit(r.forward);
    const V3 back{-f[0], -f[1], -f[2]};
    up = Unit(Sub(up, {back[0] * Dot(up, back), back[1] * Dot(up, back), back[2] * Dot(up, back)}));
    with_axes.push_back(MakeCamera(r.position, r.forward, up, r.fov, r.near_plane, true));
  }
  const Outcome a = Run(plain);
  const Outcome b = Run(with_axes);
  Report("fly-in, forward vectors only", a);
  Report("fly-in, with the full axes", b);
  Check(a.now_cuts.empty(), "fly-in: no cut (forward vectors only)");
  Check(b.now_cuts.empty(), "fly-in: no cut (full axes; rows used as consecutive frames)");
  Check(a.old_cuts.empty(), "fly-in: the old rule did not cut it either (the drive's behaviour is kept)");
  // Where the margin is thinnest: the step against what it is allowed.
  double worst_jump = 0.0;
  double worst_turn = 0.0;
  for (size_t i = 1; i < with_axes.size(); ++i) {
    const Cut& c = b.now_detail[i];
    worst_jump = std::max(worst_jump, c.deviation / c.tolerance);
    worst_turn = std::max(worst_turn, c.turn_deviation / c.turn_tolerance);
  }
  std::printf("  fly-in margins: jump deviation at most %.2f of what is allowed, turn %.2f\n", worst_jump, worst_turn);
  Check(worst_jump < 0.5 && worst_turn < 0.6, "fly-in: at least a factor of two of margin on the jump and the turn");
  // The same flight taken every second row (a harder test: steps twice as long, growing faster).
  std::vector<Camera> thin;
  for (size_t i = 0; i < with_axes.size(); i += 2) {
    thin.push_back(with_axes[i]);
  }
  const Outcome t = Run(thin);
  Report("fly-in, every second row", t);
  Check(t.now_cuts.empty(), "fly-in: no cut with every second row either");
}

void CheckFlyInStall() {
  std::vector<Camera> slow;
  for (const DriveRow& r : kFlyInStall) {
    slow.push_back(MakeCamera({r.x, r.y, r.z}, {-r.bx, -r.by, -r.bz}, {0, 0, -1}, 25.0, 0.5));
  }
  const Outcome o = Run(slow);
  Report("fly-in end, 73 ms stall (30 fps)", o);
  // (Row 1 is the first step of the excerpt, from a camera with no steps before it: a cut for both
  // rules, an artefact of starting in the middle of the flight.) The long step of frame 714 (row 8)
  // is 1.8 times the one before: a speed-relative tolerance takes it, the old rule cut it; and the
  // steps of 14, 33 and 52 m after it, which follow the lengths of the frames that came with the
  // stall, are not cuts in either.
  Check(Has(o.old_cuts, 8), "fly-in stall at 30 fps: the old rule cut the long step");
  Check(o.now_cuts.size() == 1 && o.now_cuts[0] == 1, "fly-in stall at 30 fps: the new rule cuts nothing after the excerpt's first step: " + List(o.now_cuts));

  std::vector<Camera> fast;
  for (const DriveRow& r : kFlyInStall60) {
    fast.push_back(MakeCamera({r.x, r.y, r.z}, {-r.bx, -r.by, -r.bz}, {0, 0, -1}, 25.0, 0.5));
  }
  const Outcome p = Run(fast);
  Report("fly-in end, 64 ms stall (60 fps)", p);
  // Here the stall step is 3.6 times the one before (129 m after 36): beyond what a step may differ
  // by (the floor and the step before): a jump, in both rules. The 15 m step after it is the camera
  // on a short frame: shorter than the 129 before it and within what the 36 before that allows, so
  // not another jump.
  Check(Has(p.now_cuts, 8) && Has(p.old_cuts, 8), "fly-in stall at 60 fps: the 3.6 times longer step is a jump for both rules");
  const Cut& after = p.now_detail[9];
  Check(after.distance > after.floor && !after.by_distance, "fly-in stall at 60 fps: the 15 m step after it is not a jump");
  Check(p.now_cuts.size() == 2 && p.now_cuts[0] == 1 && p.now_cuts[1] == 8, "fly-in stall at 60 fps: no other cut: " + List(p.now_cuts));
}

void CheckPauseZoom() {
  std::vector<Camera> cams;
  for (const PauseRow& r : kPauseZoom) {
    const double yaw = Rad(r.yaw_degrees);
    cams.push_back(MakeCamera(r.position, {std::sin(yaw), -0.05, -std::cos(yaw)}, {0, 1, 0}, r.fov, 0.2));
  }
  const Outcome o = Run(cams);
  Report("pause camera zoom (opening)", o);
  Check(o.old_cuts.size() == 2, "pause zoom: the old rule cut the 8.8% and the 6.1% steps");
  Check(o.now_cuts.empty(), "pause zoom: the new rule does not cut a lens ease");
}

void CheckRaceExitZoom() {
  std::vector<Camera> cams;
  double z = -574.2;
  for (double fov : kRaceExitFov) {
    cams.push_back(MakeCamera({109.7, 13.5, z}, {0.2, -0.05, -1.0}, {0, 1, 0}, fov, 0.2));
    z -= 0.15;
  }
  const Outcome o = Run(cams);
  Report("camera leaving a race (lens ease)", o);
  Check(o.old_cuts.size() == 4, "race exit: the old rule cut four frames in a row (10.3, 8.9, 7.3, 5.9%)");
  Check(o.now_cuts.empty(), "race exit: the new rule cuts none");
}

void CheckCutsceneShots() {
  // Each recorded shot change, built from the logged distance and cosine, from a camera that stood
  // still; the position moves along x and the forward vector turns in the horizontal plane.
  for (const CutsceneRow& r : kCutscene) {
    const Camera before = MakeCamera({-641.9, 20.0, -637.4}, {0, 0, -1}, {0, 1, 0}, r.fov_before, 0.2);
    const double angle = std::acos(r.cosine);
    const Camera now = MakeCamera({-641.9 + r.moved, 20.0, -637.4}, {std::sin(angle), 0, -std::cos(angle)}, {0, 1, 0}, r.fov_after, 0.2);
    CutMotion motion;
    const Cut cut = TestCut(now, before, motion);
    const OldCut old = OldTestCut(now, before, nullptr);
    std::printf("  cutscene shot (moved %.1f, cosine %.4f): old %s, new %s\n", r.moved, r.cosine,
                old.fires ? "cuts" : "misses", cut.fires ? "cuts" : "misses");
    Check(old.fires, "cutscene: the old rule cut the recorded shot change");
    Check(cut.fires && cut.by_turn, "cutscene: the new rule cuts it, by the turn: " + CutReason(cut));
  }
  // The floor of a turn at this lens (0.6 of the 37.8 degrees of horizontal view) is 22.7 degrees,
  // below the old 30: a 24 degree shot change cuts now.
  const Camera before = MakeCamera({0, 20, 0}, {0, 0, -1}, {0, 1, 0}, 21.87, 0.2);
  const Camera now = MakeCamera({0, 20, 0}, {std::sin(Rad(24.0)), 0, -std::cos(Rad(24.0))}, {0, 1, 0}, 21.87, 0.2);
  CutMotion motion;
  Check(TestCut(now, before, motion).fires && !OldTestCut(now, before, nullptr).fires,
        "cutscene: a 24 degree turn at a 21.87 degree lens is a cut now (it was not)");
  // And the same turn at a wide lens is not: 60 degrees of lens leave a 30 degree floor.
  const Camera wide_before = MakeCamera({0, 20, 0}, {0, 0, -1}, {0, 1, 0}, 60.0, 0.2);
  const Camera wide_now = MakeCamera({0, 20, 0}, {std::sin(Rad(24.0)), 0, -std::cos(Rad(24.0))}, {0, 1, 0}, 60.0, 0.2);
  Check(!TestCut(wide_now, wide_before, motion).fires, "cutscene: a 24 degree turn at a 60 degree lens is not");
}

void CheckGarageTurntable() {
  // MADE UP: the camera orbits a car at the origin, radius 6 m, height 1.5 m, 36 degrees a second
  // at 60 frames a second, looking at it; then cuts to the other side of the car (100 degrees
  // round), then to a pose 4 m away turned 20 degrees.
  const auto pose = [](double azimuth_degrees, double radius) {
    const double a = Rad(azimuth_degrees);
    const V3 position{radius * std::sin(a), 1.5, radius * std::cos(a)};
    const V3 toward{-position[0], -position[1] + 0.8, -position[2]};
    return MakeCamera(position, toward, {0, 1, 0}, 30.0, 0.2);
  };
  std::vector<Camera> cams;
  for (int i = 0; i < 120; ++i) {
    cams.push_back(pose(0.6 * i, 6.0));
  }
  Outcome o = Run(cams);
  Report("garage turntable, orbiting", o);
  Check(o.now_cuts.empty() && o.old_cuts.empty(), "turntable: an orbit is not a cut");
  cams.push_back(pose(0.6 * 119 + 100.0, 6.0));  // the cut to the other side
  o = Run(cams);
  Check(Has(o.now_cuts, cams.size() - 1) && o.now_cuts.size() == 1, "turntable: a cut to the other side of the car is one cut");
  // A cut to a pose 4 m away turned 20 degrees stays under every floor (9 m at this lens, 26 degrees
  // turn): the rule cannot see a cut that small from the poses alone. Written down so a change to the
  // floors knows it is moving this.
  cams.pop_back();
  const Camera& last_orbit = cams.back();
  const double c20 = std::cos(Rad(20.0));
  const double s20 = std::sin(Rad(20.0));
  const V3 turned{last_orbit.forward[0] * c20 + last_orbit.forward[2] * s20, last_orbit.forward[1],
                  -last_orbit.forward[0] * s20 + last_orbit.forward[2] * c20};
  cams.push_back(MakeCamera({last_orbit.position[0] + 4.0, last_orbit.position[1], last_orbit.position[2]},
                            turned, {0, 1, 0}, 30.0, 0.2));
  o = Run(cams);
  Check(o.now_cuts.empty(), "turntable: a 4 m, 20 degree shot change is below every floor (known limit)");
}

void CheckMap() {
  // MADE UP at the scale the logs give for the map: the camera at 8,996 m looking down, field of
  // view 25 degrees, near plane 7,995 (PAUSE-VIEW-DESIGN.md: the map's near plane is about 7,995).
  // A pan that starts from rest and grows to 300 m a frame, a zoom (the camera drops 8,996 to
  // 3,000 m in steps up to 150 m), then a map warp.
  const auto camera = [](double x, double y, double z) {
    return MakeCamera({x, y, z}, {0.0, -1.0, -0.02}, {0, 0, -1}, 25.0, 0.89 * y);
  };
  std::vector<Camera> cams;
  double x = -325.7;
  double speed = 0.0;
  for (int i = 0; i < 40; ++i) {  // accelerate 15 m a frame per frame to 300, hold, stop
    speed = i < 20 ? std::min(300.0, speed + 15.0) : std::max(0.0, speed - 30.0);
    x += speed;
    cams.push_back(camera(x, 8996.0, 111.5));
  }
  const std::vector<Camera> pan_only(cams.begin(), cams.end());
  double y = 8996.0;
  double drop = 0.0;
  for (int i = 0; i < 60 && y > 3000.0; ++i) {
    drop = std::min(150.0, drop + 10.0);
    y -= drop;
    cams.push_back(camera(x, y, 111.5));
  }
  Outcome o = Run(cams);
  Report("map pan and zoom", o);
  Check(!o.old_cuts.empty(), "map: the old rule cut the pan or the zoom (it is " + List(o.old_cuts) + ")");
  Check(o.now_cuts.empty(), "map: the new rule does not cut a pan or a zoom");
  // The floor at the map's near plane.
  Check(o.now_detail[1].floor > 800.0 && o.now_detail[1].floor < 1000.0,
        "map: the floor of a jump is about 900 m (it is " + std::to_string(o.now_detail[1].floor) + ")");
  // From the end of the pan (standing still at the full height): a warp 1,600 m away is a cut; a
  // step of 600 m (a quarter of the frame at that height is about 900 m) is a pan.
  std::vector<Camera> warp = pan_only;
  warp.push_back(camera(x + 1600.0, 8996.0, 111.5));
  o = Run(warp);
  Check(Has(o.now_cuts, warp.size() - 1), "map: a warp of 1,600 m is a cut");
  std::vector<Camera> step = pan_only;
  step.push_back(camera(x + 600.0, 8996.0, 111.5));
  o = Run(step);
  Check(o.now_cuts.empty(), "map: a step of 600 m is a pan");
  // The same 600 m in play (near plane 0.2) is a teleport.
  const Camera play_a = MakeCamera({0, 20, 0}, {0, 0, -1}, {0, 1, 0}, 25.0, 0.2);
  const Camera play_b = MakeCamera({600, 20, 0}, {0, 0, -1}, {0, 1, 0}, 25.0, 0.2);
  CutMotion none;
  Check(TestCut(play_b, play_a, none).fires, "map: 600 m in play is a cut");
}

void CheckEverySecondFrame() {
  // MADE UP: a camera that takes its step every second frame (0, d, 0, d, ...: the pause camera's
  // lens does, the cutscene camera may). Judged against the last step only, every second frame is
  // a surprise; judged against the last two steps, only the onset is.
  const auto camera = [](double x) { return MakeCamera({x, 20.0, 0.0}, {0, 0, -1}, {0, 1, 0}, 25.0, 0.2); };
  std::vector<Camera> cams;
  double x = 0.0;
  cams.push_back(camera(x));
  cams.push_back(camera(x));
  for (int i = 0; i < 20; ++i) {
    x += 30.0;  // 30 m is above the 9 m floor and 3 times it
    cams.push_back(camera(x));
    cams.push_back(camera(x));
  }
  const Outcome o = Run(cams);
  Report("every second frame, 30 m", o);
  Check(o.now_cuts.size() == 1 && o.now_cuts[0] == 2, "every second frame: only the onset (a 30 m step from rest) is a cut");
  // The single-step memory the occlusion has (the old signature) cuts again and again.
  int repeats = 0;
  for (size_t i = 1; i < cams.size(); ++i) {
    double step[3];
    for (int k = 0; k < 3; ++k) {
      step[k] = cams[i - 1].position[k] - (i >= 2 ? cams[i - 2].position[k] : cams[i - 1].position[k]);
    }
    repeats += TestCut(cams[i], cams[i - 1], step).fires;
  }
  std::printf("  every second frame with the one-step signature: %d cuts\n", repeats);
  Check(repeats > 5, "every second frame: the one-step signature cuts at every second step (why the tracker keeps two)");
}

void CheckRoll() {
  // MADE UP: the same place and the same look, the camera rolled by 90 degrees (a photo mode, the
  // free camera's roll keys): the forward vectors agree, the rotation does not.
  const Camera before = MakeCamera({0, 20, 0}, {0, 0, -1}, {0, 1, 0}, 30.0, 0.2);
  const Camera now = MakeCamera({0, 20, 0}, {0, 0, -1}, {1, 0, 0}, 30.0, 0.2);
  CutMotion motion;
  const Cut cut = TestCut(now, before, motion);
  Check(!OldTestCut(now, before, nullptr).fires, "roll: the old rule did not see a 90 degree roll");
  Check(cut.by_turn && std::fabs(cut.turn - Rad(90.0)) < 1e-6, "roll: the new rule cuts it, as a turn of 90 degrees: " + CutReason(cut));
  Check(std::fabs(cut.cosine - 1.0) < 1e-9, "roll: the forward cosine is still 1");
  // A 5 degree roll (a chase camera banking) is nothing.
  const Camera banked = MakeCamera({0, 20, 0}, {0, 0, -1}, {std::sin(Rad(5.0)), std::cos(Rad(5.0)), 0}, 30.0, 0.2);
  Check(!TestCut(banked, before, motion).fires, "roll: 5 degrees of banking is not a cut");
  // A camera without axes (the occlusion's) falls back to the forward vectors and sees no roll.
  Camera bare_before = before;
  Camera bare_now = now;
  for (int i = 0; i < 3; ++i) {
    bare_before.right[i] = bare_before.up[i] = bare_now.right[i] = bare_now.up[i] = 0.0;
  }
  Check(!TestCut(bare_now, bare_before, motion).fires, "roll: with no axes the rule judges the forward vectors alone");
}

void CheckLens() {
  const Camera before = MakeCamera({0, 20, 0}, {0, 0, -1}, {0, 1, 0}, 25.0, 0.2);
  CutMotion motion;
  const auto change = [&](double to) {
    return TestCut(MakeCamera({0, 20, 0}, {0, 0, -1}, {0, 1, 0}, to, 0.2), before, motion);
  };
  Check(change(50.0).by_fov, "lens: 25 to 50 degrees (a shot with another lens) is a cut");
  Check(change(25.0 * 1.16).by_fov, "lens: +16% is a cut");
  Check(!change(25.0 * 1.14).fires, "lens: +14% is not");
  Check(!change(25.0 * 0.9).fires, "lens: -10% is not");
  // A change right after the changes before it follows them: 20, 20, 20% steps is a fast zoom.
  CutMotion zoom;
  double fov = 25.0;
  Camera previous = before;
  int fired = 0;
  for (int i = 0; i < 5; ++i) {
    fov *= 1.2;
    const Camera now = MakeCamera({0, 20, 0}, {0, 0, -1}, {0, 1, 0}, fov, 0.2);
    const Cut cut = TestCut(now, previous, zoom);
    zoom.Accept(cut);
    fired += cut.fires;
    previous = now;
  }
  Check(fired == 1, "lens: a steady zoom of 20% a frame is cut at its onset only (it fired " + std::to_string(fired) + ")");
}

void CheckFloors() {
  const Camera drive = MakeCamera({0, 20, 0}, {0, 0, -1}, {0, 1, 0}, 25.0, 0.2);
  CutMotion none;
  const Cut at_drive = TestCut(drive, drive, none);
  Check(at_drive.floor > 8.5 && at_drive.floor < 9.5, "floor: about 9 m at the drive's lens (" + std::to_string(at_drive.floor) + ")");
  Check(at_drive.turn_floor > Rad(25.0) && at_drive.turn_floor < Rad(27.0), "floor: a turn floor of about 26 degrees at 25 degrees of lens");
  const Camera map = MakeCamera({0, 8996, 0}, {0, -1, 0}, {0, 0, -1}, 25.0, 7995.0);
  Check(TestCut(map, map, none).floor > 850.0 && TestCut(map, map, none).floor < 950.0, "floor: about 900 m at the map's near plane");
  const Camera wide = MakeCamera({0, 20, 0}, {0, 0, -1}, {0, 1, 0}, 60.0, 0.2);
  Check(TestCut(wide, wide, none).floor > at_drive.floor, "floor: a wider lens has a larger one");
  Check(TestCut(wide, wide, none).turn_floor <= Rad(30.0) + 1e-12, "floor: a turn floor is never above the old 30 degrees");
  const Camera narrow = MakeCamera({0, 20, 0}, {0, 0, -1}, {0, 1, 0}, 5.0, 0.2);
  Check(TestCut(narrow, narrow, none).turn_floor >= Rad(15.0) - 1e-12, "floor: ... nor below 15");
  // The occlusion's camera has no lens data of its own: the floors are those of the chase camera.
  Camera bare;
  bare.position[2] = 0.0;
  bare.forward[2] = -1.0;
  const Cut at_bare = TestCut(bare, bare, none);
  Check(std::fabs(at_bare.floor - at_drive.floor) < 1.0 && !at_bare.fires, "floor: a camera without a lens gets the chase camera's floors and no cut");
}

void CheckStateAndOverloads() {
  // Two cuts in a row are both cuts: the first one's step is not a motion to repeat.
  const auto camera = [](double x) { return MakeCamera({x, 20.0, 0.0}, {0, 0, -1}, {0, 1, 0}, 25.0, 0.2); };
  std::vector<Camera> cams{camera(0.0), camera(0.0), camera(1600.0), camera(5600.0), camera(5600.0)};
  const Outcome o = Run(cams);
  Check(Has(o.now_cuts, 2) && Has(o.now_cuts, 3) && o.now_cuts.size() == 2, "state: two different teleports in a row are two cuts");
  // The same 1,600 m step twice is a camera moving 1,600 m a frame: a cut at its onset, then a pan.
  std::vector<Camera> repeated{camera(0.0), camera(0.0), camera(1600.0), camera(3200.0), camera(4800.0)};
  const Outcome rep_out = Run(repeated);
  Check(rep_out.now_cuts.size() == 1 && rep_out.now_cuts[0] == 2, "state: the same step again is not a second cut");
  // A camera that stands still after a teleport and then takes a 20 m step from rest is cut again
  // once the teleport is two frames back; a 20 m step in the frame right after it is the camera
  // settling (the drive's takes 4.6 m there), shorter than the teleport, and is not.
  std::vector<Camera> after{camera(0.0), camera(0.0), camera(1600.0), camera(1600.0), camera(1600.0), camera(1600.0), camera(1620.0)};
  const Outcome aft_out = Run(after);
  Check(Has(aft_out.now_cuts, 2) && Has(aft_out.now_cuts, 6) && aft_out.now_cuts.size() == 2,
        "state: a 20 m step from rest two frames after a teleport is a cut");
  std::vector<Camera> at_once{camera(0.0), camera(0.0), camera(1600.0), camera(1620.0)};
  Check(Run(at_once).now_cuts.size() == 1, "state: a 20 m step right after a teleport is the camera settling, not a cut");

  // A steady fast camera (60 m a frame, the fly-in's scale) is never a cut, after the first step.
  std::vector<Camera> fast;
  for (int i = 0; i < 30; ++i) {
    fast.push_back(camera(60.0 * i));
  }
  const Outcome f = Run(fast);
  Check(f.now_cuts.size() == 1 && f.now_cuts[0] == 1, "state: a steady 60 m a frame is cut at its first step only");

  // Accept keeps the last two steps, newest first, and forgets at a cut.
  CutMotion m;
  Cut a;
  a.step[0] = 1.0;
  a.turn = 0.1;
  a.fov_change = 0.01;
  m.Accept(a);
  Check(m.steps == 1 && m.step[0][0] == 1.0, "accept: one step known");
  Cut b;
  b.step[0] = 2.0;
  b.turn = 0.2;
  b.fov_change = 0.02;
  m.Accept(b);
  Check(m.steps == 2 && m.step[0][0] == 2.0 && m.step[1][0] == 1.0 && m.turn[1] == 0.1 && m.fov[0] == 0.02, "accept: two steps, newest first");
  Cut c;
  c.step[0] = 3.0;
  m.Accept(c);
  Check(m.steps == 2 && m.step[0][0] == 3.0 && m.step[1][0] == 2.0, "accept: never more than two");
  Cut cut;
  cut.fires = true;
  m.Accept(cut);
  Check(m.steps == 2 && m.cut[0] && !m.cut[1] && m.step[0][0] == 0.0, "accept: a cut is remembered, flagged");
  m.Clear();
  Check(m.steps == 0 && !m.cut[0] && m.turn[0] == 0.0, "accept: Clear forgets all");

  // The one-step overload is the tracker with one step and no memory of the turn and the lens.
  const double step[3] = {50.0, 0.0, 0.0};
  CutMotion one;
  one.steps = 1;
  one.step[0][0] = 50.0;
  const Camera p = camera(0.0);
  const Camera q = camera(50.0);
  const Cut x = TestCut(q, p, step);
  const Cut y = TestCut(q, p, one);
  Check(x.fires == y.fires && x.deviation == y.deviation && x.tolerance == y.tolerance && x.floor == y.floor,
        "overload: the one-step signature agrees with the tracker holding one step");
  Check(!TestCut(q, p, step).fires && TestCut(q, p, static_cast<const double*>(nullptr)).fires,
        "overload: 50 m after a 50 m step is a step; from rest it is a cut");
}

void CheckReasons() {
  const Camera a = MakeCamera({0, 20, 0}, {0, 0, -1}, {0, 1, 0}, 25.0, 0.2);
  const Camera jump = MakeCamera({500, 20, 0}, {0, 0, -1}, {0, 1, 0}, 25.0, 0.2);
  const Camera turned = MakeCamera({0, 20, 0}, {1, 0, 0}, {0, 1, 0}, 25.0, 0.2);
  const Camera lens = MakeCamera({0, 20, 0}, {0, 0, -1}, {0, 1, 0}, 40.0, 0.2);
  CutMotion none;
  const std::string j = CutReason(TestCut(jump, a, none));
  const std::string t = CutReason(TestCut(turned, a, none));
  const std::string l = CutReason(TestCut(lens, a, none));
  Check(j.rfind("jump:", 0) == 0 && j.find("turn") == std::string::npos && j.find("lens") == std::string::npos, "reason: " + j);
  Check(t.rfind("turn:", 0) == 0 && t.find("90.0 deg") != std::string::npos, "reason: " + t);
  Check(l.rfind("lens:", 0) == 0 && l.find("60.0%") != std::string::npos, "reason: " + l);
  const Camera all = MakeCamera({500, 20, 0}, {1, 0, 0}, {0, 1, 0}, 40.0, 0.2);
  const std::string both = CutReason(TestCut(all, a, none));
  Check(both.find("jump:") != std::string::npos && both.find("; turn:") != std::string::npos && both.find("; lens:") != std::string::npos,
        "reason: all three clauses in one line: " + both);
  Check(CutReason(TestCut(a, a, none)) == "no clause fired", "reason: nothing fired, nothing claimed");
}

// ----------------------------------------------------------------------------- a trace from the game

// Reads a pacing-trace csv (columns by name) and runs both rules over it.
void RunTrace(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    std::printf("trace %s: cannot open it\n", path.c_str());
    return;
  }
  std::string line;
  std::getline(in, line);
  std::vector<std::string> names;
  {
    std::stringstream header(line);
    std::string cell;
    while (std::getline(header, cell, ',')) {
      while (!cell.empty() && (cell.back() == '\r' || cell.back() == ' ')) {
        cell.pop_back();
      }
      names.push_back(cell);
    }
  }
  const auto column = [&](const char* name) {
    for (size_t i = 0; i < names.size(); ++i) {
      if (names[i] == name) {
        return int(i);
      }
    }
    return -1;
  };
  const int cf = column("frame"), cx = column("cam_x"), cy = column("cam_y"), cz = column("cam_z");
  const int bx = column("cam_bx"), by = column("cam_by"), bz = column("cam_bz");
  if (cx < 0 || cy < 0 || cz < 0) {
    std::printf("trace %s: no cam_x, cam_y, cam_z columns\n", path.c_str());
    return;
  }
  std::vector<Camera> cams;
  std::vector<std::string> frames;
  while (std::getline(in, line)) {
    std::vector<double> v;
    std::vector<std::string> raw;
    std::stringstream row(line);
    std::string cell;
    while (std::getline(row, cell, ',')) {
      raw.push_back(cell);
      v.push_back(std::atof(cell.c_str()));
    }
    if (v.size() <= size_t(std::max({cx, cy, cz}))) {
      continue;
    }
    V3 forward{0, 0, -1};
    if (bx >= 0 && by >= 0 && bz >= 0 && v.size() > size_t(std::max({bx, by, bz}))) {
      forward = {-v[bx], -v[by], -v[bz]};
    }
    cams.push_back(MakeCamera({v[cx], v[cy], v[cz]}, forward, {0, 1, 0}, 25.0, 0.2, false));
    frames.push_back(cf >= 0 && size_t(cf) < raw.size() ? raw[cf] : std::to_string(cams.size()));
  }
  const Outcome o = Run(cams);
  std::printf("trace %s: %zu rows (frame, position%s), old rule %zu cuts, new rule %zu cuts\n", path.c_str(),
              cams.size(), bx >= 0 ? ", back vector" : " only", o.old_cuts.size(), o.now_cuts.size());
  size_t shown = 0;
  for (size_t i : o.now_cuts) {
    if (shown++ == 8) {
      break;
    }
    std::printf("    frame %s: %s\n", frames[i].c_str(), CutReason(o.now_detail[i]).c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  CheckFloors();
  CheckDriveTeleport();
  CheckDriveHitch();
  CheckFlyIn();
  CheckFlyInStall();
  CheckPauseZoom();
  CheckRaceExitZoom();
  CheckCutsceneShots();
  CheckGarageTurntable();
  CheckMap();
  CheckEverySecondFrame();
  CheckRoll();
  CheckLens();
  CheckStateAndOverloads();
  CheckReasons();
  for (int i = 1; i < argc; ++i) {
    RunTrace(argv[i]);
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
