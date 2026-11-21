#pragma once
// The Discord presence switch (larecomp_discord_rpc) as a live setting: what each side of it decides, with no SDK, no thread and no Discord
// in it, so rpc_toggle_rules_test.cpp can check it on its own (LIVE-SETTINGS.md section 9, row L4-1).
//
// How the setting applies:
//   * The game's hooks tell discord_rpc.cpp their state at every change (LARECOMP_Discord_SetStateText). The text is kept in a PresenceMemory
//     whether or not the presence is on, so a switch turned on in the middle of a race shows the race at once, not at the next change.
//   * The settings' change callback only sets a flag (it runs under the settings' lock, where the worker must not be started or the
//     registry read again). The per-frame poll (RpcOnRaceTick, game thread) takes the flag: on starts the worker if it does not run
//     (DecideToggle) or sends the kept text again if it does; off needs nothing, because the worker shows a presence only while the
//     switch is on (ActivityShown) and clears it within a quarter of a second.
//   * The worker thread is never stopped by the switch: stopping joins it, which waits on Discord's pipe. It ends at exit, as before.

#include <cstdint>
#include <string>
#include <string_view>

namespace larecomp::discord_rpc {

// What the poll does once the switch changed. wanted: the setting now; started: the worker thread runs.
enum class ToggleAction : uint8_t {
  kNothing,  // off: the worker clears the presence by itself (ActivityShown); nothing to start or stop
  kStart,    // on, no worker yet (the game started with it off): start it with the kept text
  kRefresh,  // on, the worker runs (it was on before, or was turned off and on): send the kept text again
};

constexpr ToggleAction DecideToggle(bool wanted, bool started) {
  if (!wanted) return ToggleAction::kNothing;
  return started ? ToggleAction::kRefresh : ToggleAction::kStart;
}

// Whether the worker shows a presence: the presence switch and the activity switch (larecomp_discord_activity, debug) are both on.
constexpr bool ActivityShown(bool rpc_on, bool activity_on) { return rpc_on && activity_on; }

// The game's last presence text. Note() says whether the text is new, so a hook that calls often does not send the same words again.
class PresenceMemory {
 public:
  // True when the state, the details or the text differ from the kept ones (the first call is always new). The text is kept either way.
  bool Note(uint32_t state, std::string_view details, std::string_view text) {
    if (seen_ && state_ == state && details_ == details && text_ == text) return false;
    seen_ = true;
    state_ = state;
    details_.assign(details);
    text_.assign(text);
    return true;
  }
  bool seen() const { return seen_; }
  uint32_t state() const { return state_; }
  const std::string& details() const { return details_; }
  const std::string& text() const { return text_; }

 private:
  bool seen_ = false;
  uint32_t state_ = 0;
  std::string details_;
  std::string text_;
};

}  // namespace larecomp::discord_rpc
