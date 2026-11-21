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
//      game does not need the pack to start, and most PCs never need it at all. One worker thread (a quarter of the PC's threads, at most four) at a lower priority than the
//      game's own threads, so it never takes a core the game wants: about 17 seconds of one core in all (1.7 s on 12 threads; a slow PC takes a minute or two). The shadows come
//      on by themselves when it is done (the renderer asks again every 2 s while it runs). A failure is logged once and not tried again until the game is started again.
//   3. Never when no setting wants it: a PC that cannot trace, or a player who set RAY TRACED SHADOWS to OFF, never pays the disk or the cores.
//
// THE COMMAND LINE'S OUTPUT (larecomp.exe --build_city_pack, what a launcher reads on stdout; every line starts with "@citypack|", fields are separated by '|', the last is free
// text; the same shape as --build_shader_packs):
//   @citypack|folder|<where the pack goes>                   once, first
//   @citypack|phase|<steps, 0 when unknown>|<name>           a phase begins ("far level of detail", "city sectors")
//   @citypack|step|<done>|<steps>|<item>                     progress in it, at most five a second
//   @citypack|log|<info|warn|error>|<text>                   the build's own log lines
//   @citypack|result|<status>|<text>                         once, last: ok, up to date, cancelled, failed, no archive, not needed
// The exit code is 0 for ok / up to date / not needed, 1 otherwise. Ending the process is the cancel and is safe at any moment (the pack is written under a temporary name and
// the stamp comes last).
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

enum class Status : uint8_t { kOk, kUpToDate, kNotNeeded, kCancelled, kFailed, kNoArchive };

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

}  // namespace larecomp::citypackbuild
