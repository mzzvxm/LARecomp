// Check of the Discord presence switch's rules (rpc_toggle_rules.h): what the per-frame poll does after larecomp_discord_rpc changed, when the
// worker shows a presence, and that the game's text is kept while the presence is off. A program of its own: it needs no game, no ReXGlue and no
// Discord, and exits 0 when every check holds. It also reads the three source files that use the rules and checks the wiring that the rules
// cannot show (the change callback only sets a flag; the poll runs from the per-frame tick; the worker asks ActivityShown), so a later edit
// that undoes the live path fails here.
//
//   cmake --build <build dir> --target larecomp_rpc_toggle_test     (CMakeLists.txt)
//   <build dir>/larecomp_rpc_toggle_test <src dir>
//
// or, with no build directory:
//   clang++ -std=c++20 -I src src/discord_rpc/rpc_toggle_rules_test.cpp -o rpc_toggle_rules_test && ./rpc_toggle_rules_test src

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "discord_rpc/rpc_toggle_rules.h"

using namespace larecomp::discord_rpc;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

void Toggle() {
  static_assert(DecideToggle(false, false) == ToggleAction::kNothing);
  static_assert(DecideToggle(false, true) == ToggleAction::kNothing);
  static_assert(DecideToggle(true, false) == ToggleAction::kStart);
  static_assert(DecideToggle(true, true) == ToggleAction::kRefresh);
  Check(DecideToggle(false, true) == ToggleAction::kNothing, "turned off with the worker running: nothing to stop (the worker clears the presence)");
  Check(DecideToggle(true, false) == ToggleAction::kStart, "turned on in a game that started with it off: the worker starts");
  Check(DecideToggle(true, true) == ToggleAction::kRefresh, "turned on again after off: the kept text is sent again");
  static_assert(ActivityShown(true, true));
  static_assert(!ActivityShown(false, true));
  static_assert(!ActivityShown(true, false));
  static_assert(!ActivityShown(false, false));
}

void Memory() {
  PresenceMemory m;
  Check(!m.seen(), "a new memory has seen nothing");
  Check(m.Note(3, "Customizing", "In the garage"), "the first text is new");
  Check(m.seen() && m.state() == 3 && m.details() == "Customizing" && m.text() == "In the garage", "the text is kept");
  Check(!m.Note(3, "Customizing", "In the garage"), "the same text again is not new (a hook that calls often)");
  Check(m.Note(3, "Customizing", "At the shop"), "another text is new");
  Check(m.Note(5, "Customizing", "At the shop"), "another state with the same words is new");
  // The presence is off: the hooks go on telling, the memory keeps the last words, and turning it on shows them.
  m.Note(5, "Racing", "Race in progress");
  m.Note(5, "Racing", "Race in progress");
  m.Note(6, "Pause", "Pause menu");
  Check(m.details() == "Pause" && m.text() == "Pause menu" && m.state() == 6, "while off, the memory follows the game");
  Check(m.Note(6, "", ""), "empty words are text too");
  Check(m.details().empty() && m.text().empty(), "empty words are kept as empty");
}

std::string ReadSource(const std::string& dir, const std::string& relative) {
  std::ifstream in(dir + "/" + relative, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// The part of <text> from the first <from> to the first <to> after it (empty when either is missing).
std::string Between(const std::string& text, const std::string& from, const std::string& to) {
  const size_t a = text.find(from);
  if (a == std::string::npos) return {};
  const size_t b = text.find(to, a + from.size());
  return b == std::string::npos ? std::string() : text.substr(a, b - a);
}

void Wiring(const std::string& src) {
  const std::string rpc = ReadSource(src, "discord_rpc/discord_rpc.cpp");
  const std::string client = ReadSource(src, "discord_rpc/rpc_client.cpp");
  const std::string hooks = ReadSource(src, "discord_rpc/discord_hooks.cpp");
  const std::string header = ReadSource(src, "discord_rpc/discord_rpc.h");
  const std::string settings = ReadSource(src, "settings/generated/settings_discord.h");
  Check(!rpc.empty() && !client.empty() && !hooks.empty() && !header.empty() && !settings.empty(), "the five source files can be read (pass the src folder)");
  if (rpc.empty() || client.empty() || hooks.empty() || header.empty() || settings.empty()) return;

  const std::string callback = Between(rpc, "RegisterChangeCallback(\"larecomp_discord_rpc\"", "});");
  Check(!callback.empty(), "discord_rpc.cpp registers a change callback on larecomp_discord_rpc");
  Check(Has(callback, "g_toggle_pending.store(true") && !Has(callback, "Start(") && !Has(callback, "Stop(") && !Has(callback, ".Get()"),
        "the callback only sets the flag (it runs under the settings' lock: no thread start or stop, no setting read)");

  const std::string poll = Between(rpc, "void LARECOMP_Discord_Poll()", "\n}\n");
  Check(Has(poll, "g_toggle_pending.exchange(false") && Has(poll, "DecideToggle(") && Has(poll, "larecomp_discord_rpc.Get()"),
        "the poll takes the flag, reads the setting and asks DecideToggle");
  Check(!Has(poll, "Stop()") && !Has(poll, "Shutdown"), "turning the switch off never stops the worker");
  Check(Has(header, "void LARECOMP_Discord_Poll();"), "discord_rpc.h declares the poll");
  Check(Has(hooks, "LARECOMP_Discord_Poll();"), "the per-frame tick (RpcOnRaceTick) runs the poll");

  const std::string text_fn = Between(rpc, "void LARECOMP_Discord_SetStateText(", "\n}\n");
  const size_t note = text_fn.find("g_memory.Note(");
  const size_t started = text_fn.find("g_rpc_started");
  Check(note != std::string::npos && started != std::string::npos && note < started,
        "SetStateText keeps the text before it asks whether the worker runs (so a text told while off is not lost)");
  Check(!Has(text_fn, "larecomp_discord_rpc.Get()") || text_fn.find("larecomp_discord_rpc.Get()") > note,
        "SetStateText does not drop the text when the switch is off");

  Check(Has(client, "ActivityShown(") && !Has(client, "const bool activity_enabled = larecomp::settings::larecomp_discord_activity.Get();"),
        "the worker shows a presence only while both switches are on (ActivityShown)");
  Check(Has(settings, "larecomp_discord_rpc, true") && !Has(Between(settings, "larecomp_discord_rpc, true", ");"), "kRequiresRestart"),
        "larecomp_discord_rpc is not tagged restart-only");
}

}  // namespace

int main(int argc, char** argv) {
  Toggle();
  Memory();
  if (argc > 1) {
    Wiring(argv[1]);
  } else {
    std::printf("note: no src folder given, the wiring checks were skipped\n");
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
