#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace larecomp::settings {

class DebugSettings final : public rex::SettingOwner {
 public:
  DebugSettings() : SettingOwner("larecomp") {}

  REX_SETTING(bool, lzx_stats, false, "Measure pgStreamer LZX decompression (XMemDecompressStream): per-2s window stats "
    "appended to <exe>/lzx_stats.txt. For diagnosing streaming stutter (South Central).", rex::kDebugOnly);

  REX_SETTING(bool, mcla_gfx_probe, false, "Milestone 0: passive D3D9-Xenon probe. Renders nothing, changes nothing. "
                    "Writes mcla_gfx_probe_*.csv next to the executable.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(i32, mcla_gfx_probe_frames, 900, "Milestone 0: number of guest frames to capture before the probe dumps its "
                     "report and goes idle (0 = never stop).", rex::kDebugOnly, rex::Range{0, 100000});

  REX_SETTING(std::string, mcla_test_crash, "", "Crash test for the Linux crash logger: MODE or MODE:SECONDS (5 seconds by default), a few "
    "seconds after the game has started. segv writes to address 0x10 on a worker thread; guest "
    "reads the guest's null page (the runtime logs 'Unhandled guest access violation' first, as for "
    "a real guest crash); stack runs a worker thread out of stack (it has a signal stack); abort "
    "calls abort(); terminate throws an exception nobody catches; illegal runs an illegal "
    "instruction; fpe divides by zero. The log must end with '!!! CRASH DETECTED !!!' (or '!!! "
    "ABORT DETECTED !!!') and a stack trace, and the game dies with the signal's status. Only works "
    "with the debug menu unlocked. Linux only.", rex::kRestart, rex::kDebugOnly);  // was inside a #if in the .cpp: check

};

inline DebugSettings g_debug;

inline auto& lzx_stats = g_debug.lzx_stats;
inline auto& mcla_gfx_probe = g_debug.mcla_gfx_probe;
inline auto& mcla_gfx_probe_frames = g_debug.mcla_gfx_probe_frames;
inline auto& mcla_test_crash = g_debug.mcla_test_crash;

}  // namespace larecomp::settings
