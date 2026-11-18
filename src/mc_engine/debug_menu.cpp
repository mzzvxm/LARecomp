#include "debug_menu.h"

#include <algorithm>
#include <atomic>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "config_save.h"

namespace {

// Set once by InitDebugMenu. After that the cvar can no longer change the answer: the validator
// below refuses a write, and DebugMenuUnlocked() reads g_unlocked, not the cvar. (kInitOnly alone
// does not do it: the SDK only enforces it after rex::cvar::FinalizeInit(), and nothing in the
// SDK's app start or in LARecomp calls that. The settings panel does show it as read-only.)
std::atomic<bool> g_latched{false};
std::atomic<bool> g_unlocked{false};
std::atomic<const char*> g_source{""};

}  // namespace

namespace larecomp {

namespace {

// The SDK cvars that stand behind DEBUG rows, so they are not LARecomp's to tag in their own
// definition. FIDELITY FX is the SDK presenter's effect and its FSR and CAS settings (the Xenia
// path only); GRAPHICS API is the SDK's backend choice, a restart-only troubleshooting switch. A
// name this SDK does not have is skipped.
constexpr const char* kSdkDebugCvars[] = {
    "present_effect",
    "present_fsr_quality_mode",
    "present_cas_additional_sharpness",
    "present_fsr_sharpness_reduction",
    "graphics_backend",
    "vsync",  // the Xenia renderer's vsync: XENIA VSYNC on the debug RENDERING page
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
  // GetRegistry hands out the entries themselves, and GetFlagInfo only a const view of them.
  // Called while the program starts, before anything else walks the registry.
  for (larecomp::cvar::FlagEntry& entry : rex::cvar::GetRegistry()) {
    for (const char* name : kSdkDebugCvars) {
      if (entry.name == name) {
        entry.is_debug_only = true;
        break;
      }
    }
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

// CHECK: the call is in LarecompApp::OnPostInitLogging, after larecomp.toml is read and before
// anything draws a menu. Unit-tested against the stock SDK's registry only (native/sdk-stock), not
// run in the game, and not built as part of the full target.
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
