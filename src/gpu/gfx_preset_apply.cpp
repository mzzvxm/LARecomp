// MCLA GPU renderer -- the graphics presets at run time. See gfx_preset_apply.h; the table and the rules are gfx_preset.h.
//
// Nothing here has been compiled or run (the header says what each piece is for; every CHECK: is a point only a build and a run can settle).

#include "gfx_preset_apply.h"

#include <mutex>
#include <string>
#include <unordered_set>

#include "gpu_class.h"
#include "rt_tier.h"
#include "mc_engine/core/config_save.h"
#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

namespace mcla::gpu::gfx_preset {

namespace {

namespace cv = larecomp::cvar;

// One lock for the whole state: Start, the preset's change callback (any thread that writes the setting) and the pause menu's hook all take it. Recursive because a write this file makes
// can call a function of it back on the same thread (the preset's own callback, which the guard below answers first).
std::recursive_mutex g_mutex;
bool g_started = false;
Preset g_held = Preset::kCustom;          // the named preset whose values are held over the rows now (CUSTOM: none)
std::unordered_set<std::string> g_rows;   // the rows held
bool g_had_own = false;                   // some row had a value of the player's own (the file's, or set this session) when the preset's values went over it

// Set while this file writes settings: the preset's own callback and the menu hook ignore what they see.
thread_local int t_writing = 0;
struct Writing {
  Writing() { ++t_writing; }
  ~Writing() { --t_writing; }
  Writing(const Writing&) = delete;
  Writing& operator=(const Writing&) = delete;
};

bool SetByCommandLineOrEnvironment(const std::string& name) {
  const cv::Source source = cv::GetFlagSource(name);
  return source == cv::Source::kCommandLine || source == cv::Source::kEnvironment;
}

// The named preset's values go over the rows. A row that is held already (the player moved from one preset to another) just takes the new value; a row the command line or the environment
// sets is not touched.
void HoldNamedLocked(Preset p) {
  const rex::SettingsBatch batch;  // the packages that watch these rows are called once, with the final values
  const Writing writing;
  int held = 0;
  int left = 0;
  for (int i = 0; i < kRowCount; ++i) {
    const Row& row = kRows[i];
    if (row.page == Page::kSupport) continue;
    const std::string name(row.cvar);
    const bool already = g_rows.count(name) != 0;
    if (!already) {
      if (SetByCommandLineOrEnvironment(name)) {
        ++left;
        continue;
      }
      const cv::Source source = cv::GetFlagSource(name);
      if (source == cv::Source::kConfig || source == cv::Source::kRuntime) g_had_own = true;
    }
    if (larecomp::SetCvarForScene(name, CellAt(i, p))) {
      g_rows.insert(name);
      ++held;
    }
  }
  g_held = p;
  REXLOG_INFO("[preset] {} held over {} rows ({} left to the command line or the environment)", Id(p), held, left);
}

// The held values are let go: put back as they were (the player's own, or the defaults), or kept as the player's own.
void ReleaseLocked(bool restore) {
  const rex::SettingsBatch batch;
  const Writing writing;
  for (const std::string& name : g_rows) {
    if (restore) {
      larecomp::RestoreCvarForScene(name);
    } else {
      larecomp::ReleaseCvarFromScene(name);
    }
  }
  REXLOG_INFO("[preset] {} rows {}", g_rows.size(), restore ? "put back as the player had them" : "kept as the player's own");
  g_rows.clear();
  g_held = Preset::kCustom;
  g_had_own = false;
}

// CUSTOM's base: a row the player's file does not name takes the value of the preset the rows came from, at LARecomp's own default layer (the file, the environment and the command line all
// beat it). `none` (a larecomp.toml written before presets) leaves every row at its own default, which is what it was.
void ApplyBaseLocked() {
  Preset from = Preset::kCustom;
  if (!Parse(cv::GetFlagByName(kFromSetting), &from) || !IsNamed(from)) return;
  const rex::SettingsBatch batch;
  const Writing writing;
  int based = 0;
  for (int i = 0; i < kRowCount; ++i) {
    const Row& row = kRows[i];
    if (row.page == Page::kSupport) continue;
    const std::string name(row.cvar);
    if (cv::GetFlagSource(name) != cv::Source::kDefault || cv::HasNonDefaultValue(name)) continue;
    if (larecomp::settings::SetDefault(name, CellAt(i, from))) ++based;
  }
  REXLOG_INFO("[preset] custom: {} rows without a value of their own take {}'s", based, Id(from));
}

Preset PresetOfSetting() {
  Preset p = Preset::kCustom;
  Parse(cv::GetFlagByName(kPresetSetting), &p);
  return p;
}

// The setting changed: the pause row, a value written to the file while the game runs, the console.
void OnPresetChanged(std::string_view value) {
  if (t_writing > 0) return;
  Preset picked = Preset::kCustom;
  if (!Parse(value, &picked)) return;
  const std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (!g_started) return;
  switch (OnPicked(g_held, picked, g_had_own)) {
    case Action::kNone:
      break;
    case Action::kOverlay:
      HoldNamedLocked(picked);
      break;
    case Action::kRestore:
      ReleaseLocked(true);
      ApplyBaseLocked();
      break;
    case Action::kFreeze: {
      const Preset was = g_held;
      ReleaseLocked(false);
      const Writing writing;
      cv::SetFlagByName(kFromSetting, Id(was));
      break;
    }
  }
}

}  // namespace

void Start() {
  const std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (g_started) return;
  g_started = true;
  // A change made while the game runs applies at once. The setting exists from RegisterAll on (OnConfigure); this runs in OnInit.
  cv::RegisterChangeCallback("mcla_gpu_preset", [](std::string_view, std::string_view value) { OnPresetChanged(value); });  // (a literal: tools/check_live_readers.py looks for it)
  const Preset p = PresetOfSetting();
  REXLOG_INFO("[preset] {} (recommended for this GPU: {})", Id(p), Id(Recommended()));
  if (IsNamed(p)) {
    HoldNamedLocked(p);
  } else {
    ApplyBaseLocked();
  }
}

Preset Current() { return PresetOfSetting(); }

Preset Recommended() {
  namespace gc = gpu_class;
  if (!gc::Known()) return Preset::kVanilla;
  return gc::RecommendedPreset(gc::Current(), gc::CurrentRtTier());
}

bool NoteRowWrite(std::string_view cvar) {
  if (t_writing > 0 || !IsCovered(cvar)) return false;
  const std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (!g_started || !IsNamed(g_held)) return false;
  const Preset was = g_held;
  ReleaseLocked(false);
  const Writing writing;
  cv::SetFlagByName(kFromSetting, Id(was));
  cv::SetFlagByName(kPresetSetting, Id(Preset::kCustom));
  REXLOG_INFO("[preset] {} changed by the player: {} becomes custom, and the rows keep the values it gave them", cvar, Id(was));
  return true;
}

}  // namespace mcla::gpu::gfx_preset
