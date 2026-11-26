// Check of the menu worlds' rules (menu_world.h). A program of its own: it needs no renderer, no ReXGlue and no
// GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_menu_world_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_menu_world_test

#include <cstdint>
#include <cstdio>
#include <string>

#include "menu_world.h"
#include "temporal_camera.h"

using namespace mcla::gpu::menu_world;

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

Lens Camera(double near_plane, double forward_y) {
  Lens lens;
  lens.valid = true;
  lens.near_plane = near_plane;
  lens.forward_y = forward_y;
  return lens;
}

// The cameras of the recorded runs (ports/run/gc3-pausemap, ao5-pause-high, cen-pause): the pause menu's and the
// map's, near plane and the y of the looking direction.
void RecordedCameras() {
  Check(!TopDown(Camera(0.2, -0.070)), "the pause camera at the car (near 0.2, looking -0.070) is not the map");
  Check(!TopDown(Camera(5.0, -0.004)), "the pause camera of the other spot (near 5, looking -0.004) is not the map");
  Check(!TopDown(Camera(0.5, -0.134)), "the pause camera swing (near 0.5) is not the map");
  Check(TopDown(Camera(7994.9085, -1.000)), "the map's camera (near 7994.9, looking straight down) is the map");
  Check(TopDown(Camera(7347.4969, -1.000)), "the map's camera at 8347 m (near 7347) is the map");
  Check(TopDown(Camera(7224.4558, -1.000)), "the map's camera at 8224 m (near 7224) is the map");
}

void TopDownNeedsBoth() {
  Check(!TopDown(Lens{}), "no camera is not the map");
  Lens high_but_level = Camera(7000.0, -0.2);
  Check(!TopDown(high_but_level), "far above but looking along the horizon is not the map");
  Check(!TopDown(Camera(0.2, -1.0)), "looking straight down at the street is not the map (the near plane says)");
  Check(!TopDown(Camera(kMapNear, -1.0)), "a near plane at the limit is not the map (strictly above)");
  Check(TopDown(Camera(kMapNear + 1.0, -1.0)), "a near plane just over the limit is");
  Check(!TopDown(Camera(7000.0, kMapDown)), "a look at the limit is not the map (strictly under)");
  Check(TopDown(Camera(7000.0, kMapDown - 0.001)), "a look just under the limit is");
  Lens invalid = Camera(7995.0, -1.0);
  invalid.valid = false;
  Check(!TopDown(invalid), "a camera marked invalid is not the map");
}

void Classification() {
  const Lens none;
  const Lens pause = Camera(0.2, -0.07);
  const Lens map = Camera(7995.0, -1.0);
  Check(Classify(false, none, false) == World::kPlay, "2x samples: the game view");
  Check(Classify(false, map, true) == World::kPlay, "2x samples are the game view whatever the camera and the byte say");
  Check(Classify(true, pause, false) == World::kPause, "4x with the pause camera: the pause menu's world");
  Check(Classify(true, pause, true) == World::kGarage, "4x with the pause camera and the garage byte: the garage");
  Check(Classify(true, map, false) == World::kMap, "4x with the map camera: the map");
  Check(Classify(true, map, true) == World::kMap, "the map wins over a garage byte that was left on");
  Check(Classify(true, none, false) == World::kPause, "4x with no camera yet: the pause menu's world until it is known");
  Check(Classify(true, none, true) == World::kGarage, "4x with no camera yet in the garage: the garage");
}

void RulesTable() {
  for (const World world : {World::kPlay, World::kPause, World::kGarage}) {
    const Rules r = RulesFor(world);
    Check(r.occlusion && r.contact && r.bounce && r.shafts && r.car_refl, std::string("every effect runs in the ") + Name(world));
  }
  const Rules map = RulesFor(World::kMap);
  Check(map.occlusion && map.contact, "the map keeps the occlusion and the contact shadows as they were");
  Check(!map.bounce && !map.shafts, "the map has no bounce light and no shafts");
  Check(!map.car_refl, "the map has no car self-reflections (a car is a few pixels from 9 km up)");
  const Rules none = RulesFor(World::kNone);
  Check(!none.occlusion && !none.contact && !none.bounce && !none.shafts && !none.car_refl, "no 3D view runs nothing");
}

void ShaftsNeedSkyOutsideTheGameView() {
  Check(ShaftsRun(World::kPlay, false), "the game view runs the shafts with no sky draw told (the light stands in)");
  Check(ShaftsRun(World::kPlay, true), "the game view runs them with one");
  Check(!ShaftsRun(World::kPause, false), "the pause menu's world needs a sky draw");
  Check(ShaftsRun(World::kPause, true), "the pause menu's world with a sky draw runs them");
  Check(!ShaftsRun(World::kGarage, false), "the garage (no sky is drawn) has none");
  Check(ShaftsRun(World::kGarage, true), "the garage with a sky draw would (a window onto the street)");
  Check(!ShaftsRun(World::kMap, true), "the map never runs them");
  Check(!ShaftsRun(World::kNone, true), "no 3D view never");
  using mcla::gpu::gbuffer::PauseFxView;
  Check(!PauseFxRuns(World::kPause, PauseFxView::kNone), "the switch is required to admit the pause city to new effects");
  for (const auto view : {PauseFxView::kPause, PauseFxView::kPhoto}) {
    Check(PauseFxRuns(World::kPause, view) && ShaftsRun(World::kPause, false, view),
          "the admitted city runs shafts with the light direction when there is no sky draw");
    Check(!PauseFxRuns(World::kMap, view) && !ShaftsRun(World::kMap, true, view),
          "an overhead map never gets the new effects even if the movie lags");
    Check(!PauseFxRuns(World::kGarage, view) && !ShaftsRun(World::kGarage, false, view),
          "the garage keeps its sky rule even if the movie lags");
  }
}

void LensRule() {
  Check(!LensSteady(false, 0.0), "an unknown rate of change is not steady");
  Check(LensSteady(true, 0.0), "a lens that did not change is steady");
  Check(LensSteady(true, 0.05), "a change of 5% is still steady (the cut rule's limit)");
  Check(!LensSteady(true, 0.0501), "more than 5% is a swing");
  Check(kLensSteadyStep == mcla::gpu::temporal::kCutFov && kLensSteadyStep == 0.05, "the steady limit is the temporal system's cut rule for the lens");
  mcla::gpu::temporal::Camera before{};
  before.fov_y = 1.0;
  before.aspect = 1.6;
  before.near_plane = 0.5;
  before.far_plane = 1000.0;
  auto now = before;
  Check(LensSteady(true, LensStep(now, before)), "a frozen photo lens is steady");
  now.fov_y *= 1.02;
  Check(LensSteady(true, LensStep(now, before)), "a slow zoom can keep its last lens");
  for (auto member : {&decltype(now)::fov_y, &decltype(now)::aspect, &decltype(now)::near_plane, &decltype(now)::far_plane}) {
    now = before;
    now.*member *= 0.8;
    Check(!LensSteady(true, LensStep(now, before)), "an abrupt change to any lens parameter drops history");
  }
}

void FirstPointGeometry() {
  // The 1280x720 picture of the 1280x736 main target (kHeightAlignment 32): the whole-picture destination.
  Check(PauseFirstPointGeometry(0, 0, 1280, 720, 1280, 720, 1280, 736, 32), "the whole picture triggers");
  Check(PauseFirstPointGeometry(0, 512, 1280, 720, 1280, 720, 1280, 736, 32), "a tile that does not start at the origin triggers (any order)");
  Check(PauseFirstPointGeometry(256, 0, 512, 720, 1280, 720, 1280, 736, 32), "a tile of three, the middle one, triggers");
  Check(!PauseFirstPointGeometry(-1, 0, 1280, 720, 1280, 720, 1280, 736, 32), "a negative edge does not");
  Check(!PauseFirstPointGeometry(0, 0, 0, 720, 1280, 720, 1280, 736, 32), "an empty rectangle does not");
  Check(!PauseFirstPointGeometry(0, 0, 1281, 720, 1280, 720, 1280, 736, 32), "a rectangle leaving the picture does not");
  Check(!PauseFirstPointGeometry(0, 0, 1280, 721, 1280, 720, 1280, 736, 32), "one row too many does not");
  // A tile's own texture (256 wide, 720 tall): the picture is smaller than the target.
  Check(!PauseFirstPointGeometry(0, 0, 256, 720, 256, 720, 1280, 736, 32), "a tile's own texture is not the whole picture");
  Check(!PauseFirstPointGeometry(0, 0, 1280, 256, 1280, 256, 1280, 736, 32), "a destination of 256 rows is not the whole picture");
  Check(PauseFirstPointGeometry(0, 0, 1280, 705, 1280, 705, 1280, 736, 32), "a picture within the alignment of the target's height is");
  Check(!PauseFirstPointGeometry(0, 0, 1280, 704, 1280, 704, 1280, 736, 32), "one row under the alignment is not");
}

void LensFromCamera() {
  Check(!LensOf(nullptr).valid, "no camera, no lens");
  mcla::gpu::temporal::Camera camera;
  camera.near_plane = 7995.0;
  camera.forward[1] = -1.0;
  const Lens lens = LensOf(&camera);
  Check(lens.valid && lens.near_plane == 7995.0 && lens.forward_y == -1.0, "the lens takes the near plane and the y of the look");
  Check(TopDown(lens), "a camera of the map's numbers is the map through LensOf");
  camera.near_plane = 0.2;
  camera.forward[1] = -0.07;
  Check(!TopDown(LensOf(&camera)), "a camera of the pause's numbers is not");
}

void GarageHint() {
  Check(!GarageLighting(), "the hint starts off");
  SetGarageLighting(true);
  Check(GarageLighting(), "the hook sets it");
  SetGarageLighting(false);
  Check(!GarageLighting(), "and clears it");
}

void Text() {
  Check(std::string(Name(World::kPlay)) == "game view", "name of the game view");
  Check(std::string(Name(World::kGarage)) == "garage", "name of the garage");
  Check(std::string(Name(World::kMap)) == "map", "name of the map");
  Check(std::string(Name(World::kPause)).find("pause") != std::string::npos, "name of the pause world");
  Check(std::string(Name(World::kNone)).find("no 3D") != std::string::npos, "name of none");

  const auto all = [](const char* ao, const char* contact, const char* gi, const char* shafts) {
    return EffectsText(World::kPause, {{"ambient occlusion", ao}, {"contact shadows", contact},
                                       {"bounce light", gi}, {"sun shafts", shafts}});
  };
  Check(all("ultra", "ultra", "high", "default") ==
            "ambient occlusion ultra, contact shadows ultra, bounce light high, sun shafts default (where the sky is drawn)",
        "the pause world lists what is on, the shafts with their sky note: " + all("ultra", "ultra", "high", "default"));
  Check(all("off", "off", "off", "off") == "no effect is on", "nothing on says so");
  Check(all("ultra", "", "off", "off") == "ambient occlusion ultra", "an empty level and 'off' are off");

  const std::string play = EffectsText(World::kPlay, {{"ambient occlusion", "ultra"}, {"contact shadows", "off"},
                                                       {"bounce light", "ultra"}, {"sun shafts", "strong"}});
  Check(play == "ambient occlusion ultra, bounce light ultra, sun shafts strong",
        "the game view has no sky note on the shafts: " + play);

  const std::string map = EffectsText(World::kMap, {{"ambient occlusion", "ultra"}, {"contact shadows", "high"},
                                                     {"bounce light", "ultra"}, {"sun shafts", "default"}});
  Check(map == "ambient occlusion ultra, contact shadows high; left out here: bounce light ultra, sun shafts default",
        "the map lists what it left out: " + map);
  const std::string map_only_off = EffectsText(World::kMap, {{"ambient occlusion", "off"}, {"contact shadows", "off"},
                                                              {"bounce light", "off"}, {"sun shafts", "off"}});
  Check(map_only_off == "no effect is on", "nothing on in the map says so");
  const std::string map_left = EffectsText(World::kMap, {{"ambient occlusion", "off"}, {"contact shadows", "off"},
                                                          {"bounce light", "high"}, {"sun shafts", "off"}});
  Check(map_left == "no effect is on; left out here: bounce light high", "only the left-out ones on: " + map_left);

  // The car self-reflections are the fifth effect: they run in the game view, the pause world and the garage, and are left
  // out in the map.
  const std::string car_play = EffectsText(World::kPlay, {{"ambient occlusion", "ultra"}, {"contact shadows", "off"},
                                                           {"bounce light", "off"}, {"sun shafts", "off"},
                                                           {"car self-reflections", "on"}});
  Check(car_play == "ambient occlusion ultra, car self-reflections on", "the game view lists them: " + car_play);
  const std::string car_garage = EffectsText(World::kGarage, {{"ambient occlusion", "ultra"}, {"contact shadows", "ultra"},
                                                               {"bounce light", "off"}, {"sun shafts", "strong"},
                                                               {"car self-reflections", "on"}});
  Check(car_garage == "ambient occlusion ultra, contact shadows ultra, sun shafts strong (where the sky is drawn), car self-reflections on",
        "the garage lists them, with no sky note of their own: " + car_garage);
  const std::string car_map = EffectsText(World::kMap, {{"ambient occlusion", "ultra"}, {"contact shadows", "ultra"},
                                                         {"bounce light", "off"}, {"sun shafts", "off"},
                                                         {"car self-reflections", "on"}});
  Check(car_map == "ambient occlusion ultra, contact shadows ultra; left out here: car self-reflections on",
        "the map leaves them out: " + car_map);
  const std::string car_off = EffectsText(World::kPause, {{"ambient occlusion", "off"}, {"contact shadows", "off"},
                                                           {"bounce light", "off"}, {"sun shafts", "off"},
                                                           {"car self-reflections", "off"}});
  Check(car_off == "no effect is on", "all five off says so: " + car_off);
}

// The announcements: one a change of world, after the world has held a few frames.
void Announcing() {
  Announcer a;
  Check(!a.Observe(World::kNone), "a frame with no 3D view announces nothing");
  uint32_t lines = 0;
  for (int i = 0; i < 10; ++i) {
    lines += a.Observe(World::kPlay) ? 1u : 0u;
  }
  Check(lines == 1 && a.announced() == World::kPlay, "ten game view frames announce the game view once");

  // The pause menu opens: the first frames are a spell's, the world holds from the first of them.
  lines = 0;
  bool at_fourth = false;
  for (int i = 1; i <= 30; ++i) {
    const bool say = a.Observe(World::kPause);
    lines += say ? 1u : 0u;
    if (say) {
      at_fourth = (i == int(Announcer::kStableFrames));
    }
  }
  Check(lines == 1 && at_fourth, "the pause world is announced once, on its kStableFrames-th frame");

  // One frame of another world in the middle of a spell (a transition) does not announce it.
  lines = 0;
  lines += a.Observe(World::kMap) ? 1u : 0u;
  lines += a.Observe(World::kPause) ? 1u : 0u;
  lines += a.Observe(World::kPause) ? 1u : 0u;
  Check(lines == 0, "a one-frame blip of another world announces nothing, and coming back is not a change");
  for (int i = 0; i < 10; ++i) {
    lines += a.Observe(World::kMap) ? 1u : 0u;
  }
  Check(lines == 1 && a.announced() == World::kMap, "the map announced after it held");
  // A gap with no 3D view does not break the held world or announce it again.
  lines = 0;
  for (int i = 0; i < 5; ++i) {
    lines += a.Observe(World::kNone) ? 1u : 0u;
  }
  for (int i = 0; i < 5; ++i) {
    lines += a.Observe(World::kMap) ? 1u : 0u;
  }
  Check(lines == 0, "frames with no 3D view in between do not announce the same world again");
  // Garage, then back to the game view.
  lines = 0;
  for (int i = 0; i < 6; ++i) {
    lines += a.Observe(World::kGarage) ? 1u : 0u;
  }
  for (int i = 0; i < 6; ++i) {
    lines += a.Observe(World::kPlay) ? 1u : 0u;
  }
  Check(lines == 2 && a.announced() == World::kPlay, "garage and the game view after it: two lines");
  Check(a.announcements() == 5, "five announcements in all: " + std::to_string(a.announcements()));
}

void ShortStayAnnouncesNothing() {
  Announcer a;
  for (int i = 0; i < 20; ++i) {
    a.Observe(World::kPlay);
  }
  uint32_t lines = 0;
  // The pause menu opened and closed within three frames: nothing to say about it.
  for (int i = 0; i < 3; ++i) {
    lines += a.Observe(World::kPause) ? 1u : 0u;
  }
  for (int i = 0; i < 10; ++i) {
    lines += a.Observe(World::kPlay) ? 1u : 0u;
  }
  Check(lines == 0, "a pause spell shorter than the hold announces nothing, and neither does the return");
}

}  // namespace

int main() {
  RecordedCameras();
  TopDownNeedsBoth();
  Classification();
  RulesTable();
  ShaftsNeedSkyOutsideTheGameView();
  LensRule();
  LensFromCamera();
  FirstPointGeometry();
  GarageHint();
  Text();
  Announcing();
  ShortStayAnnouncesNothing();
  std::printf("menu_world_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
