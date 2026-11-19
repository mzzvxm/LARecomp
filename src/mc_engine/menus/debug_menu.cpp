#include "debug_menu.h"

#include <algorithm>
#include <atomic>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "mc_engine/core/config_save.h"

namespace {

// Set once by InitDebugMenu, and DebugMenuUnlocked() reads g_unlocked, not the setting. The setting itself cannot change
// any more by then either: it is kStartupOnly, which the SDK closes just before OnInit (the 0.10 validator that refused a
// write after InitDebugMenu is that freeze now: settings/larecomp_app_settings.h). The settings panel shows it read-only.
std::atomic<bool> g_latched{false};
std::atomic<bool> g_unlocked{false};
std::atomic<const char*> g_source{""};

}  // namespace

namespace larecomp {

namespace {

// The SDK settings that stand behind DEBUG rows, so they are not LARecomp's to tag in their own definition: the Xenia
// renderer's vsync (the XENIA VSYNC row on the debug RENDERING page). The other two of the 0.10 list are gone: FIDELITY FX
// (present_effect and its FSR and CAS settings) left with the Xenia presenter's effects, and GRAPHICS API is LARecomp's own
// setting now, tagged kDebugOnly where it is declared. The SDK built its settings without the trait, so the mark is made
// here (larecomp::cvar::MarkDebugOnly) instead of poking the registry's entries as 0.10 did.
constexpr const char* kSdkDebugCvars[] = {
    "gpu.vsync",
};

}  // namespace

bool DebugMenuUnlocked() {
  if (g_latched.load()) {
    return g_unlocked.load();
  }
  return larecomp::settings::debug_menu.Get();
}

std::string_view DebugMenuUnlockSource() {
  return g_source.load();
}

const char* CvarSourceName(larecomp::cvar::Source source) {
  switch (source) {
    case larecomp::cvar::Source::kConfig:
      return "larecomp.toml";
    case larecomp::cvar::Source::kEnvironment:
      return "environment";
    case larecomp::cvar::Source::kCommandLine:
      return "command line";
    case larecomp::cvar::Source::kRuntime:
      return "this session";
    case larecomp::cvar::Source::kDefault:
      break;
  }
  return "default";
}

void MarkSdkDebugCvars() {
  for (const char* name : kSdkDebugCvars) {
    larecomp::cvar::MarkDebugOnly(name);
  }
}

std::vector<DebugCvarChange> NonDefaultDebugCvars() {
  std::vector<DebugCvarChange> changes;
  for (const std::string& name : larecomp::cvar::ListFlags()) {
    const larecomp::cvar::FlagEntry* info = larecomp::cvar::GetFlagInfo(name);
    if (!info || !info->is_debug_only || info->type == larecomp::cvar::FlagType::Command ||
        name == "debug_menu") {
      continue;
    }
    // Source kDefault with another value means code wrote the storage directly: not a setting
    // anybody chose.
    if (info->source == larecomp::cvar::Source::kDefault) {
      continue;
    }
    std::string value = info->getter();
    if (value == info->default_value) {
      continue;
    }
    if (info->source == larecomp::cvar::Source::kRuntime && IsAtStartupValue(name)) {
      continue;
    }
    changes.push_back({name, std::move(value), info->source});
  }
  std::sort(changes.begin(), changes.end(),
            [](const DebugCvarChange& a, const DebugCvarChange& b) { return a.name < b.name; });
  return changes;
}

// The call is in LarecompApp::OnInit, after the SDK has read larecomp.toml, the environment and the command line (App::Init
// runs OnInit last) and before anything draws a menu. CHECK: not built or run on 0.11 yet.
void InitDebugMenu() {
  if (g_latched.load()) {
    return;
  }
  MarkSdkDebugCvars();

  const bool unlocked = larecomp::settings::debug_menu.Get();
  const char* source = unlocked ? CvarSourceName(larecomp::cvar::GetFlagSource("debug_menu")) : "";
  g_source.store(source);
  g_unlocked.store(unlocked);
  g_latched.store(true);

  if (unlocked) {
    REXLOG_INFO("[debug] debug menu unlocked ({})", source);
  } else {
    REXLOG_INFO("[debug] debug menu locked");
  }

  // What the player (or a tester) changed outside the normal menus, locked or not: the cheapest
  // way to see in a bug report what is not at its default.
  const std::vector<DebugCvarChange> changes = NonDefaultDebugCvars();
  if (!changes.empty()) {
    REXLOG_INFO("[settings] {} debug setting(s) are not at their default:", changes.size());
    for (const DebugCvarChange& change : changes) {
      REXLOG_INFO("[settings] {} = {} ({})", change.name, change.value,
                  CvarSourceName(change.source));
    }
  }
}

namespace testing {

void ResetDebugMenuForTesting() {
  g_latched.store(false);
  g_unlocked.store(false);
  g_source.store("");
}

}  // namespace testing

}  // namespace larecomp
