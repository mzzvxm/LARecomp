#pragma once
// ===========================================================================
// MCLA GPU renderer -- the city pack's state, as the app tells the renderer (CITY-RT M1)
// ===========================================================================
// The ray traced city shadows read the city pack (src/citypack: mcla_city.v1.pack and its stamp in the shader pack folder's `city` folder), which LARecomp builds on the
// player's PC from the player's own game files: by the launcher's setup (larecomp.exe --build_city_pack) or, when that did not happen, by the game itself in the
// background, at the moment the renderer first wants it (bootprogress/larecomp_city_pack.h). The renderer never reads the game files or the stamp: it asks the app (the
// hooks of gpu.h SetCityPackHooks) which of these the pack is in, and acts on the answer. This header holds that vocabulary and the rules that need no device, so that
// city_rt_rules_test.cpp checks them; it includes nothing.
//
//   kCurrent   a pack is in place and its stamp matches the game files now (stat only, a few file stats): open it.
//   kBuilding  the app is building it (a background thread): ask again soon.
//   kMissing   there is none (a first start, or the launcher's setup did not run it): the renderer asks the app to build it, once.
//   kStale     there is one, built from other inputs (the game's archive changed, a mod replaced a city file, another builder version): the same.
//   kNoSource  the game's archive is not there to build from: nothing to do, off this session.
//   kFailed    a build of this session failed: not tried again until the game is started again (the log says why).
//
// The renderer keeps the game's own shadows in every state but kCurrent: a pack that is missing or stale is never opened (a stale pack would shadow a city that is not the
// player's).
// ===========================================================================

#include <cstdint>
#include <string_view>

namespace mcla::gpu::city_rt {

enum class PackState : uint8_t { kCurrent, kBuilding, kMissing, kStale, kNoSource, kFailed };

constexpr std::string_view PackStateName(PackState state) {
  switch (state) {
    case PackState::kCurrent:
      return "current";
    case PackState::kBuilding:
      return "being built";
    case PackState::kMissing:
      return "missing";
    case PackState::kStale:
      return "out of date";
    case PackState::kNoSource:
      return "no game archive";
    default:
      return "failed";
  }
}

// Only a current pack is opened.
constexpr bool CanOpenPack(PackState state) { return state == PackState::kCurrent; }

// The states in which the renderer asks the app to build the pack (once per session: the app answers kBuilding at once, and kFailed after a failure).
constexpr bool WantsBuild(PackState state) { return state == PackState::kMissing || state == PackState::kStale; }

// Frames to wait before asking about the pack again after this answer (60 a second at the shipped frame rate; the count is in frames of the structures' own clock, CityRtScene::frame_).
inline constexpr uint32_t kProbeBuildingFrames = 120;   // a build runs: look every 2 s, so that the shadows come on soon after it ends
inline constexpr uint32_t kProbeIdleFrames = 3600;      // missing or stale with no build running, or the game's archive absent: a minute
inline constexpr uint32_t kProbeNeverFrames = 0x7FFFFFFFu;  // failed: not this session
constexpr uint32_t ProbeWaitFrames(PackState state, bool build_requested) {
  switch (state) {
    case PackState::kCurrent:
      return 0;
    case PackState::kBuilding:
      return kProbeBuildingFrames;
    case PackState::kMissing:
    case PackState::kStale:
      return build_requested ? kProbeBuildingFrames : kProbeIdleFrames;
    case PackState::kNoSource:
      return kProbeIdleFrames;
    default:
      return kProbeNeverFrames;
  }
}

// What the hit data's file probe does with the app's answer (CITY-HIT-ACTION, city_hit_scene.cpp OpenFile): the same vocabulary for the hit file and the texture file beside the pack. The hit scene
// asks only while a consumer wants the hit data (the level above OFF), after the base pack is open (it is current), so a state of "being built" is the app's own background build of the hit
// data, started by this very probe (once per session: `build_requested`).
struct FileProbeStep {
  bool open = false;           // the files are current: open them now
  bool request_build = false;  // ask the app to build them (once)
  uint32_t wait_frames = 0;    // frames until the probe is asked again (ProbeWaitFrames)
};
constexpr FileProbeStep DecideFileProbe(PackState state, bool build_requested) {
  FileProbeStep step;
  step.open = CanOpenPack(state);
  step.request_build = WantsBuild(state) && !build_requested;
  step.wait_frames = ProbeWaitFrames(state, build_requested || step.request_build);
  return step;
}

}  // namespace mcla::gpu::city_rt
