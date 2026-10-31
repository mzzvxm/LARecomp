// The game's own audio mixer thread: a host priority for it.
//
// How the game's audio reaches the speakers (read from the recompiled code): the SDK's audio
// worker runs the game's callback once per 5.33 ms frame. The callback hands the frame to a
// guest thread that mixes it (about 0.7 ms of CPU on a fast PC) and waits for the answer. That
// thread is created by sub_823ECC60 with ExCreateThread, start address 0x823EC8C8, and its body
// is rex_sub_823EC8C8. The game asks for priority +15 for it (KeSetBasePriorityThread), which
// the SDK ignores (ignore_thread_priorities, default true), so the thread runs at the host's
// normal priority like the render, GPU and job threads. When those take every core the mixer
// waits to run, the audio worker waits for it, and the output runs dry.
//
// audio_mixer_priority gives the thread a host priority when it starts: the override of its body
// below runs on it. The thread is found by its start address, not by its handle (the handle
// depends on how many kernel objects the game made before it).
//   0 normal (the game's own behaviour), 1 above normal, 2 highest,
//   3 highest and registered with the Windows multimedia scheduler ("Pro Audio"),
//  -1 automatic: 3 on a PC with 5 or fewer logical processors, else 0.
// Takes effect at the next start. Whatever the setting, one line in larecomp.log says that the
// thread started and what was done to it. Windows only: elsewhere the thread is left as it is.

#if defined(_WIN32) && !defined(REXGLUE_HAS_XEO3_TARGET)
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>

#include <windows.h>
#include <avrt.h>

#include "hooks.h"
#include "hooks_internal.h"
#include "larecomp_log.h"

REXCVAR_DEFINE_INT32(audio_mixer_priority, -1, "MCLA/Audio",
    "Host priority of the game's own audio mixer thread (the guest thread that mixes each 5.33 ms "
    "frame for the audio worker; the game's request for a higher priority is ignored): 0 = "
    "normal, 1 = above normal, 2 = highest, 3 = highest and registered with the Windows "
    "multimedia scheduler (\"Pro Audio\"), -1 = automatic: 3 on a PC with 5 or fewer logical "
    "processors, 0 otherwise. Takes effect at the next start.")
    .range(-1, 3)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REX_EXTERN(__imp__rex_sub_823EC8C8);  // the mixer thread's body

namespace {

constexpr DWORD kSmallPcLogicalProcessors = 5;

// The level to apply, from the setting and the PC.
int MixerLevel(DWORD logical) {
    const int setting = REXCVAR_GET(audio_mixer_priority);
    if (setting >= 0) {
        return setting;
    }
    return logical <= kSmallPcLogicalProcessors ? 3 : 0;
}

}  // namespace

// Runs on the mixer thread, once, as it starts.
extern "C" REX_FUNC(rex_sub_823EC8C8) {
    const DWORD logical = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    const int level = MixerLevel(logical);
    if (level == 0) {
        LARECOMP_APP_INFO("[AudioMixer] mixer thread started (entry 823EC8C8), priority left "
                          "alone (audio_mixer_priority {}, {} logical processors)",
                          REXCVAR_GET(audio_mixer_priority), logical);
    } else {
        HANDLE self = GetCurrentThread();
        const BOOL set = SetThreadPriority(
            self, level >= 2 ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_ABOVE_NORMAL);
        const DWORD set_error = set ? 0 : GetLastError();
        // The multimedia scheduler registration lasts until the thread ends; the mixer runs for
        // the whole session, so it is never reverted.
        DWORD task = 0;
        HANDLE mmcss = level >= 3 ? AvSetMmThreadCharacteristicsW(L"Pro Audio", &task) : nullptr;
        const DWORD mmcss_error = (level >= 3 && !mmcss) ? GetLastError() : 0;
        LARECOMP_APP_INFO("[AudioMixer] mixer thread started (entry 823EC8C8), level {} "
                          "(audio_mixer_priority {}, {} logical processors): SetThreadPriority "
                          "{} (error {}), now {}; multimedia scheduler {} (error {})",
                          level, REXCVAR_GET(audio_mixer_priority), logical,
                          set ? "ok" : "failed", set_error, GetThreadPriority(self),
                          level >= 3 ? (mmcss ? "ok" : "failed") : "not asked", mmcss_error);
    }
    __imp__rex_sub_823EC8C8(ctx, base);
}
#endif  // _WIN32 && !REXGLUE_HAS_XEO3_TARGET
