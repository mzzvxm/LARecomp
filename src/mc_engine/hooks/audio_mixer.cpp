// The game's own audio threads: a host priority for each.
//
// How the game's audio reaches the speakers (read from the recompiled code): the SDK's audio
// worker (a host thread named "Audio Worker") runs the game's callback once per 5.33 ms frame.
// The callback is sub_823F56D0 (sub_823F3718 passes it to XAudioRegisterRenderDriverClient); it
// loads the audio object and jumps to sub_823ECA40, which hands the frame to a guest thread that
// mixes it (about 0.7 ms of CPU on a fast PC) and waits for the answer. That thread is created
// by sub_823ECC60 with ExCreateThread, start address 0x823EC8C8, and its body is
// rex_sub_823EC8C8. The game asks for priority +15 for it (KeSetBasePriorityThread), which the
// SDK ignores (ignore_thread_priorities, default true), so the thread runs at the host's normal
// priority like the render, GPU and job threads, and so does the audio worker. When those take
// every core the mixer waits to run, or the worker does, and the output runs dry. The worker
// does almost nothing itself (it sets the mixer's event and waits) but it has to be on a CPU
// soon after the device frees a slot.
//
// audio_mixer_priority gives both threads a host priority when they start. The mixer is found
// by its start address: the override of its body below runs on it. The worker is found by the
// callback it runs: the SDK calls the game's callback on that thread only, so the first call of
// the override of sub_823F56D0 is on it. Neither is found by handle (the handle depends on how
// many kernel objects the game made before it).
//   0 normal (the game's own behaviour), 1 above normal, 2 highest,
//   3 highest and registered with the Windows multimedia scheduler ("Pro Audio"),
//  -1 automatic: 3 on a PC with 5 or fewer logical processors, else 0.
// Takes effect at the next start. Whatever the setting, one line in larecomp.log per thread
// says that it started and what was done to it. Windows only: elsewhere the threads are left
// as they are.

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
    "Host priority of the game's two audio threads: the guest thread that mixes each 5.33 ms "
    "frame (the game's request for a higher priority is ignored) and the SDK's audio worker "
    "that calls the game for each frame: 0 = normal, 1 = above normal, 2 = highest, 3 = highest "
    "and registered with the Windows multimedia scheduler (\"Pro Audio\"), -1 = automatic: 3 on "
    "a PC with 5 or fewer logical processors, 0 otherwise. Takes effect at the next start.")
    .range(-1, 3)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REX_EXTERN(__imp__rex_sub_823EC8C8);  // the mixer thread's body
REX_EXTERN(__imp__sub_823F56D0);      // the game's audio callback, run by the SDK's audio worker

namespace {

constexpr DWORD kSmallPcLogicalProcessors = 5;

// The level to apply, from the setting and the PC.
int AudioLevel(DWORD logical) {
    const int setting = REXCVAR_GET(audio_mixer_priority);
    if (setting >= 0) {
        return setting;
    }
    return logical <= kSmallPcLogicalProcessors ? 3 : 0;
}

// Applies the setting to the calling thread and logs one line about it; `who` starts the line.
void ApplyToThisThread(const char* who) {
    const DWORD logical = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    const int level = AudioLevel(logical);
    HANDLE self = GetCurrentThread();
    // The name the SDK gave the thread, if any: it shows that the right thread was found.
    char name[64] = "?";
    PWSTR description = nullptr;
    if (SUCCEEDED(GetThreadDescription(self, &description)) && description) {
        if (WideCharToMultiByte(CP_UTF8, 0, description, -1, name, int(sizeof(name)), nullptr,
                                nullptr) <= 0) {
            name[0] = '?';
            name[1] = '\0';
        }
        LocalFree(description);
    }
    if (level == 0) {
        LARECOMP_APP_INFO("{}, priority left alone (audio_mixer_priority {}, {} logical "
                          "processors); thread \"{}\"",
                          who, REXCVAR_GET(audio_mixer_priority), logical, name);
        return;
    }
    const BOOL set = SetThreadPriority(
        self, level >= 2 ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_ABOVE_NORMAL);
    const DWORD set_error = set ? 0 : GetLastError();
    // The multimedia scheduler registration lasts until the thread ends; both threads run for
    // the whole session, so it is never reverted.
    DWORD task = 0;
    HANDLE mmcss = level >= 3 ? AvSetMmThreadCharacteristicsW(L"Pro Audio", &task) : nullptr;
    const DWORD mmcss_error = (level >= 3 && !mmcss) ? GetLastError() : 0;
    LARECOMP_APP_INFO("{}, level {} (audio_mixer_priority {}, {} logical processors): "
                      "SetThreadPriority {} (error {}), now {}; multimedia scheduler {} (error "
                      "{}); thread \"{}\"",
                      who, level, REXCVAR_GET(audio_mixer_priority), logical,
                      set ? "ok" : "failed", set_error, GetThreadPriority(self),
                      level >= 3 ? (mmcss ? "ok" : "failed") : "not asked", mmcss_error, name);
}

}  // namespace

// Runs on the mixer thread, once, as it starts.
extern "C" REX_FUNC(rex_sub_823EC8C8) {
    ApplyToThisThread("[AudioMixer] mixer thread started (entry 823EC8C8)");
    __imp__rex_sub_823EC8C8(ctx, base);
}

// Runs on the SDK's audio worker once per frame; acts on the first call on each thread (the
// worker is one thread for the whole session, so that is once).
extern "C" REX_FUNC(sub_823F56D0) {
    static thread_local bool applied = false;
    if (!applied) {
        applied = true;
        ApplyToThisThread(
            "[AudioWorker] audio worker thread: first call of the callback (823F56D0)");
    }
    __imp__sub_823F56D0(ctx, base);
}
#endif  // _WIN32 && !REXGLUE_HAS_XEO3_TARGET
