#pragma once
// ===========================================================================
// Thread names that Linux keeps
// ===========================================================================
// rex::platform::SetCurrentThreadName ends in pthread_setname_np on Linux, which refuses a name of 16 bytes or more (with its NUL):
// it returns ERANGE and the thread keeps the name of the program ("larecomp"). "larecomp gpu render" (19) and "larecomp texture decode"
// (23) were both refused, so every thread of ours showed up as "larecomp" in top -H, ps -L, perf and the crash report's
// `Thread: tNNN "..."` line, which is the line a Steam Deck tester's log is read by.
//
// ThreadNameFor gives the name to hand to SetCurrentThreadName: unchanged where the OS has no such limit (Windows keeps its long names,
// and the tools that read them: ports\scripts\etl_stalls.py looks for "larecomp gpu render"); on Linux a name that is too long loses its
// leading "larecomp " first ("gpu render", "texture decode") and is cut to the limit if it is still too long.
//
// Pure (no OS calls): thread_name_test.cpp has a check that needs no game.
// ===========================================================================

#include <cstddef>
#include <string>
#include <string_view>

namespace larecomp::os {

// The longest name pthread_setname_np takes, without its NUL.
inline constexpr size_t kLinuxThreadNameMax = 15;

inline std::string ThreadNameFor(std::string_view name, bool linux_limit) {
  if (!linux_limit || name.size() <= kLinuxThreadNameMax) {
    return std::string(name);
  }
  constexpr std::string_view kPrefix = "larecomp ";
  if (name.size() > kPrefix.size() && name.substr(0, kPrefix.size()) == kPrefix) {
    name.remove_prefix(kPrefix.size());
  }
  if (name.size() > kLinuxThreadNameMax) {
    // A cut must not fall inside a UTF-8 sequence (the names are ASCII today; the rule is for the next one): if the first byte that goes is a
    // continuation byte, the character it belongs to goes with it.
    size_t cut = kLinuxThreadNameMax;
    while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) {
      --cut;
    }
    name = name.substr(0, cut);
  }
  return std::string(name);
}

// The limit of the OS this is compiled for.
#if defined(__linux__)
inline constexpr bool kThreadNameLimited = true;
#else
inline constexpr bool kThreadNameLimited = false;
#endif

inline std::string ThreadNameFor(std::string_view name) { return ThreadNameFor(name, kThreadNameLimited); }

}  // namespace larecomp::os
