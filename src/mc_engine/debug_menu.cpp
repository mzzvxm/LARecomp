#include "debug_menu.h"

#include <algorithm>
#include <atomic>

#include <rex/cvar.h>
#include <rex/logging.h>

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

REXCVAR_DEFINE_BOOL(debug_menu, false, "MCLA/Debug",
    "Unlock the hidden DEBUG menu and the developer settings, for testing and for bug reports. "
    "Turn it on with --debug_menu on the command line, REX_DEBUG_MENU=1 in the environment, or "
    "debug_menu = true in larecomp.toml. It cannot be changed while the game runs, and the game "
    "never writes it to the file. Settings changed in the DEBUG menu are not saved either.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly)
    .validator([](std::string_view) { return !g_latched.load(); })
    .debug_only();

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
  return REXCVAR_GET(debug_menu);
}

std::string_view DebugMenuUnlockSource() {
  return g_source.load();
}

const char* CvarSourceName(rex::cvar::Source source) {
  switch (source) {
    case rex::cvar::Source::kConfig:
      return "larecomp.toml";
    case rex::cvar::Source::kEnvironment:
      return "environment";
    case rex::cvar::Source::kCommandLine:
      return "command line";
    case rex::cvar::Source::kRuntime:
      return "this session";
    case rex::cvar::Source::kDefault:
      break;
  }
  return "default";
}

void MarkSdkDebugCvars() {
  // GetRegistry hands out the entries themselves, and GetFlagInfo only a const view of them.
  // Called while the program starts, before anything else walks the registry.
  for (rex::cvar::FlagEntry& entry : rex::cvar::GetRegistry()) {
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
  for (const std::string& name : rex::cvar::ListFlags()) {
    const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(name);
    if (!info || !info->is_debug_only || info->type == rex::cvar::FlagType::Command ||
        name == "debug_menu") {
      continue;
    }
    // Source kDefault with another value means code wrote the storage directly: not a setting
    // anybody chose.
    if (info->source == rex::cvar::Source::kDefault) {
      continue;
    }
    std::string value = info->getter();
    if (value == info->default_value) {
      continue;
    }
    if (info->source == rex::cvar::Source::kRuntime && IsAtStartupValue(name)) {
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

  const bool unlocked = REXCVAR_GET(debug_menu);
  const char* source = unlocked ? CvarSourceName(rex::cvar::GetFlagSource("debug_menu")) : "";
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
