#pragma once
// ===========================================================================
// The city pack's start-up rules: what the app decides and says (CITY-RT M1)
// ===========================================================================
// The ray traced city shadows need the city pack (src/citypack), built on the player's PC from their own game files. bootprogress/larecomp_city_pack.h runs it; this header
// holds the decisions and the words that need no game, no renderer and no threads, so that city_pack_rules_test.cpp checks them on their own.
//
// WHEN THE PACK IS BUILT (the decision, 2026-11-20):
//   1. larecomp.exe --build_city_pack, the launcher's setup step (after --build_shader_packs): the whole build on every core, a few seconds on a fast PC, so the first start has
//      nothing to do. The launcher runs it where the PC's graphics card can ray trace (the pack is 250 MB of disk and the game does not read it otherwise).
//   2. The game itself, in the BACKGROUND, when the pack is missing or stale AND the renderer wants it: ray traced shadows resolved on (the setting is auto on a High class card
//      that can trace, high / ultra by hand), the contact shadows on (their consumer) and the device able to trace. Never at start-up and never behind the boot popup: the
//      game does not need the pack to start, and most PCs never need it at all. The build runs on a thread of its own with a quarter of the PC's threads as workers (at least one,
//      at most four), in background mode (a lower CPU, disk and memory priority than the game's own threads), so it never takes a core or the disk the game wants: about 17 seconds of one core in all (1.7 s on 12 threads; a slow PC takes a minute or two). The shadows come
//      on by themselves when it is done (the renderer asks again every 2 s while it runs). A failure is logged once and not tried again until the game is started again.
//   3. Never when no setting wants it: a PC that cannot trace, or a player who set RT SHADOWS to OFF, never pays the disk or the cores.
//
// THE COMMAND LINE'S OUTPUT (larecomp.exe --build_city_pack, what a launcher reads on stdout; every line starts with "@citypack|", fields are separated by '|', the last is free
// text; the same shape as --build_shader_packs):
//   @citypack|folder|<where the pack goes>                   once, first
//   @citypack|phases|<name>:<weight>:<title>[;...]            before the first phase of --build_city_pack
//   @citypack|phase|<steps, 0 when unknown>|<name>           a phase begins ("far level of detail", "city sectors")
//   @citypack|step|<done>|<steps>|<item>                     progress in it, at most five a second
//   @citypack|log|<info|warn|error>|<text>                   the build's own log lines
//   @citypack|result|<status>|<text>                         once, last: ok, up to date, cancelled, failed, no archive, not needed
// The exit code is 0 for ok / up to date / not needed, 1 otherwise. Ending the process is the cancel and is safe at any moment (the pack is written under a temporary name and
// the stamp comes last).
//
// THE HIT DATA (CITY-HIT-ACTION, 2026-11-21): the two files beside the city pack that the effects reading the city's surfaces need, the hit file (mcla_city_hit.v1.pack) and the texture file
// (mcla_city_tex.v1.pack), built by the same library as citypack_tool's build-hit / check-hit / check-tex (src/citypack/hit_data.h): the release ships larecomp.exe, not the tool, so the
// player's PC builds them with what ships. THREE ACTIONS (SDK actions like --build_city_pack; each takes the folder from the game's own settings, no argument):
//
//   larecomp.exe --build_city_hit               the textures first, then the hit file over the city pack in <shader pack folder>/city, both on every core but one (the setting
//                                               shader_pack_threads, 0 = auto), stamp written LAST. Needs the city pack (--build_city_pack first, or on the same command line, where
//                                               the pack comes first); without one it says "no pack". Nothing is built when the stamp is current: "up to date" in a moment.
//                                               --build_city_hit_force=true builds anyway.
//   larecomp.exe --check_city_hit               STAT ONLY (a few file stats and the stamp, milliseconds): "up to date" (exit 0) or "not current" with the reason (exit 1). With
//                                               --check_city_hit_deep=true the files are READ BACK whole instead (seconds, not milliseconds): the city pack, the hit
//                                               file against it and every picture of the texture file; "up to date" when all are whole, "failed" with the counts when not.
//   (both)                                      --build_city_hit --check_city_hit: the build, then the check when the build succeeded (add --check_city_hit_deep=true for the full one).
//
// The lines are the city pack's own (the prefix, the line kinds and the field order above), so one parser reads all three; the launcher keys its progress bar on the PHASE NAMES the
// builders print (a phase with 0 steps is "unknown"):
//   build:   "city textures" (one step a dictionary, 441), "city texture files" (one a picture), "far level of detail" (1), "city hit sectors" (one a sector, 439)
//   check:   "checking the city pack", "checking the city hit file", "checking the city textures" (deep only; one step each)
// plus the log lines of the builders ("@citypack|log|info|the city hit data is built: ..."). One more status joins the set, for the two new answers (the old ones are unchanged):
//   @citypack|result|no pack|<text>        the hit data goes over the city pack and there is none (or it is damaged); exit 1
//   @citypack|result|not current|<why>     --check_city_hit (stat only) found the hit data missing or built from other files; exit 1
// and the results the old statuses give here: "ok" (built: text "built the hit data: 439 sectors, 85,422 runs, ..., in 12.3 s"), "up to date" (a build that found its stamp current, a stat
// check that does, a deep check that read everything back whole), "failed" (a build that failed, a deep check that found problems or could not open a file), "cancelled", "no archive".
// Several actions on one command line print their lines one after the other, each action ending with its OWN result line; the exit code is the first one that was not 0 (the rest do not run).
// The launcher's usual way is one process a step: --build_city_pack, then --build_city_hit, then --check_city_hit --check_city_hit_deep=true. The logs are logs/city_hit_build.txt and
// logs/city_hit_check.txt (the pack's is logs/city_pack_build.txt).
//
// THE GAME'S OWN BUILD of the hit data (like the pack's, item 2 above): in the BACKGROUND at the first start when it is missing or stale, when something wants it: the renderer asks (the
// hit scene's file probe, gpu/city_hit_scene.h) only while a consumer of the ray tracing scene service wants the city's hit data at a level above OFF: that is "the RT tier is not none"
// (a PC that cannot trace, or whose ray traced effects are all off, never asks; the tier, when it lands, resolves each effect to off on a PC with none), and the city pack is current (the hit data goes over it: the pack's own background build comes first and the hit scene
// is only given its base when the pack is open). The same thread budget and background mode as the pack's (BackgroundWorkers), one build a session, a failure is final until the next start.
// ===========================================================================

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "gpu/city_pack_state.h"

namespace larecomp::citypackbuild {

namespace city_rt = mcla::gpu::city_rt;

// ---------------------------------------------------------------------------
// the state the renderer is told
// ---------------------------------------------------------------------------

// What the app knows about the pack right now (stat only: the stamp check, the game archive, this session's build).
struct Facts {
  bool source_exists = false;   // <game>/xarchive_cache.rpf is there
  bool pack_exists = false;     // the pack file or its stamp is there
  bool current = false;         // the stamp matches the game files now (citypack::CheckCityPack)
  bool building = false;        // a background build of this session is running
  bool failed = false;          // a build of this session failed
};

// The order matters: a current pack is current whatever else is true; a build that runs is "being built" (the renderer waits and asks again); a failure of this session is final
// (no retry until the next start); no archive means nothing can be built; else the pack is missing (a first start) or stale (built from other files or another builder).
constexpr city_rt::PackState StateFor(const Facts& facts) {
  if (facts.current) return city_rt::PackState::kCurrent;
  if (facts.building) return city_rt::PackState::kBuilding;
  if (facts.failed) return city_rt::PackState::kFailed;
  if (!facts.source_exists) return city_rt::PackState::kNoSource;
  return facts.pack_exists ? city_rt::PackState::kStale : city_rt::PackState::kMissing;
}

// Whether a request from the renderer starts a background build: only for a pack that is missing or stale, once (a build that runs or failed is never started again in a session).
constexpr bool ShouldStartBackgroundBuild(city_rt::PackState state, bool started_before) { return city_rt::WantsBuild(state) && !started_before; }

// ---------------------------------------------------------------------------
// how hard the background build works
// ---------------------------------------------------------------------------

// Workers of the in-game build: a quarter of the PC's hardware threads, at least one, at most four (the game's own threads keep the rest; 3 on a 12 thread CPU is 6 seconds of work,
// 1 on a 4 thread CPU is 20 to 30). The command line's build takes the builder's own default (every thread but one).
constexpr uint32_t BackgroundWorkers(uint32_t hardware_threads) { return std::clamp<uint32_t>(hardware_threads / 4, 1, 4); }

// ---------------------------------------------------------------------------
// the words
// ---------------------------------------------------------------------------

// "22,236,322"
inline std::string GroupThousands(uint64_t value) {
  std::string digits = std::to_string(value);
  std::string out;
  const size_t n = digits.size();
  for (size_t i = 0; i < n; ++i) {
    out += digits[i];
    const size_t left = n - 1 - i;
    if (left != 0 && left % 3 == 0) out += ',';
  }
  return out;
}

// "248 MiB", "0.4 MiB"
inline std::string MebiBytes(uint64_t bytes) {
  const double mib = double(bytes) / (1024.0 * 1024.0);
  char text[32];
  std::snprintf(text, sizeof(text), mib >= 10.0 ? "%.0f MiB" : "%.1f MiB", mib);
  return text;
}

// The log line of a finished build, the same words in the game's log and on the command line: "built 439 sectors (22,236,322 detail and 382,485 far triangles), 248 MiB, in 3.4 s".
inline std::string BuiltText(uint32_t sectors, uint64_t hi_triangles, uint64_t lod_triangles, uint64_t pack_bytes, double seconds) {
  char time[32];
  std::snprintf(time, sizeof(time), "%.1f s", seconds);
  return "built " + std::to_string(sectors) + " sectors (" + GroupThousands(hi_triangles) + " detail and " + GroupThousands(lod_triangles) + " far triangles), " + MebiBytes(pack_bytes) +
         ", in " + time;
}

// ---------------------------------------------------------------------------
// the command line's lines
// ---------------------------------------------------------------------------

enum class Status : uint8_t { kOk, kUpToDate, kNotNeeded, kCancelled, kFailed, kNoArchive, kNoPack, kNotCurrent };

constexpr std::string_view StatusName(Status status) {
  switch (status) {
    case Status::kOk:
      return "ok";
    case Status::kUpToDate:
      return "up to date";
    case Status::kNotNeeded:
      return "not needed";
    case Status::kCancelled:
      return "cancelled";
    case Status::kNoArchive:
      return "no archive";
    case Status::kNoPack:
      return "no pack";
    case Status::kNotCurrent:
      return "not current";
    default:
      return "failed";
  }
}

// 0 when the pack is in place (or is not wanted at all), 1 when it is not.
constexpr int ExitCodeFor(Status status) {
  return status == Status::kOk || status == Status::kUpToDate || status == Status::kNotNeeded ? 0 : 1;
}

// One line of free text: no line breaks, no field separator, not too long (the log file has the whole of it).
inline std::string Field(std::string text, size_t max = 600) {
  for (char& c : text) {
    if (c == '\r' || c == '\n' || c == '\t' || c == '|') c = ' ';
  }
  if (text.size() > max) text = text.substr(0, max - 3) + "...";
  return text;
}

inline std::string LineFolder(const std::string& folder) { return "@citypack|folder|" + Field(folder, 1000); }
inline std::string LinePhase(int steps, const std::string& name) { return "@citypack|phase|" + std::to_string(steps) + "|" + Field(name); }
inline std::string LineStep(int done, int steps, const std::string& item) { return "@citypack|step|" + std::to_string(done) + "|" + std::to_string(steps) + "|" + Field(item); }
inline std::string LineLog(int level, const std::string& text) {  // level: 0 info, 1 warning, 2 error
  return std::string("@citypack|log|") + (level >= 2 ? "error" : level == 1 ? "warn" : "info") + "|" + Field(text);
}
inline std::string LineResult(Status status, const std::string& text) { return "@citypack|result|" + std::string(StatusName(status)) + "|" + Field(text); }

// ---------------------------------------------------------------------------
// the command line's actions and the order they run in (CITY-HIT-ACTION)
// ---------------------------------------------------------------------------

// Which action options the command line has (main.cpp reads them from the arguments; the SDK keeps them as actions, settings/cli_rules.h).
struct ActionFlags {
  bool shader_packs = false;     // --build_shader_packs
  bool precache = false;         // --precache_pipelines
  bool city_pack = false;        // --build_city_pack
  bool city_hit = false;         // --build_city_hit
  bool check_city_hit = false;   // --check_city_hit
};

enum class ActionKind : uint8_t { kNone, kBuildShaderPacks, kPrecachePipelines, kBuildCityPack, kBuildCityHit, kCheckCityHit };

// What runs: the FIRST action (the one that sets the program's mode: no window and no runtime when there is one), then, when it succeeded and in this order, the city pack, the hit data
// and the check of the hit data of those that were given too. The first is the earliest of: shader packs, pipeline pre-cache, city pack, hit data, check (the order the launcher's setup
// does them in, which is also the order the files depend on each other). Two of the first two (--build_shader_packs with --precache_pipelines) run the first only, as before.
struct ActionPlan {
  ActionKind first = ActionKind::kNone;
  bool then_city_pack = false;
  bool then_city_hit = false;
  bool then_check_city_hit = false;
  bool any() const { return first != ActionKind::kNone; }
};

constexpr ActionPlan PlanActions(const ActionFlags& flags) {
  ActionPlan plan;
  if (flags.shader_packs) {
    plan.first = ActionKind::kBuildShaderPacks;
  } else if (flags.precache) {
    plan.first = ActionKind::kPrecachePipelines;
  } else if (flags.city_pack) {
    plan.first = ActionKind::kBuildCityPack;
  } else if (flags.city_hit) {
    plan.first = ActionKind::kBuildCityHit;
  } else if (flags.check_city_hit) {
    plan.first = ActionKind::kCheckCityHit;
  }
  plan.then_city_pack = flags.city_pack && plan.first != ActionKind::kBuildCityPack;
  plan.then_city_hit = flags.city_hit && plan.first != ActionKind::kBuildCityHit;
  plan.then_check_city_hit = flags.check_city_hit && plan.first != ActionKind::kCheckCityHit;
  return plan;
}

// ---------------------------------------------------------------------------
// the hit data's words
// ---------------------------------------------------------------------------

// "built the hit data: 439 sectors, 85,422 runs, 6,805 materials, 8,342 pictures; hit file 110 MiB, texture file 138 MiB, in 12.3 s" (a hit file built without textures names none).
inline std::string HitBuiltText(uint32_t sectors, uint64_t runs, uint32_t materials, uint32_t pictures, uint64_t hit_bytes, uint64_t texture_bytes, double seconds) {
  char time[32];
  std::snprintf(time, sizeof(time), "%.1f s", seconds);
  std::string text = "built the hit data: " + std::to_string(sectors) + " sectors, " + GroupThousands(runs) + " runs, " + GroupThousands(materials) + " materials, " + GroupThousands(pictures) +
                     " pictures; hit file " + MebiBytes(hit_bytes);
  if (texture_bytes != 0) text += ", texture file " + MebiBytes(texture_bytes);
  return text + ", in " + time;
}

// The stat check's good answer and the build's "nothing to do".
inline std::string HitCurrentText(const std::string& folder) {
  return "The city hit data in " + folder + " is up to date (its stamp matches the game files and the city pack, and the texture file is the size it says).";
}

// What a deep check found: the counts of every part that was read (a part that was not asked for has 0), `pictures` the texture table's size, `textures_skipped` for a hit file with none.
inline std::string DeepCheckText(uint32_t pack_problems, uint32_t hit_problems, uint32_t texture_problems, uint32_t pictures, bool textures_skipped, double seconds) {
  char time[32];
  std::snprintf(time, sizeof(time), "%.1f s", seconds);
  if (pack_problems == 0 && hit_problems == 0 && texture_problems == 0) {
    return std::string("The city data is whole: the city pack, the hit file") + (textures_skipped ? " (it has no textures)" : " and " + GroupThousands(pictures) + " pictures of the texture file") +
           " read back with no problems, in " + time + ".";
  }
  return "The city data has problems: " + std::to_string(pack_problems) + " in the city pack, " + std::to_string(hit_problems) + " in the hit file, " + std::to_string(texture_problems) +
         " in the texture file (read back in " + time + "). Build it again with build_city_hit_force=true (the pack with build_city_pack_force=true).";
}

}  // namespace larecomp::citypackbuild
