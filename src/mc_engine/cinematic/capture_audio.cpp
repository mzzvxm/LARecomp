#ifndef REXGLUE_HAS_XEO3_TARGET
//
// A take's sound (trailer toolkit M2c; feature-parity/CINEMATIC-PLAN.md, CINEMATIC-DESIGN.md section 2.4 item 7). The maths and the books
// are in capture_audio_core.h (with its test).
//
// NOT PORTED TO rexglue 0.11 YET. The 0.10 implementation (about 450 lines, never run: its own CHECK list was the first run's agenda)
// is `git show 792f2fe6:src/mc_engine/cinematic/capture_audio.cpp`. It was a CaptureOutputDriver in front of the real output of the
// game's first render client: it took the credits of the SDK's audio queue out of the SDK's semaphore (so the mixer could not run),
// released exactly the credits that cover the video frames so far at each frame gate, and folded the frames the mixer submitted with the
// SDK's own conversion. 0.11 has no such seam: the audio system mixes from a pull (IAudioDriver's AudioFill, called by the device
// or by a timer), and the credits are the APU's own semaphore, released when a frame is consumed. The audio glue (mc_engine/audio_output.h)
// now sits there as an IAudioDriver, so the port is possible. THE DESIGN for whoever does it:
//   - a take stops the device pulling: the glue driver's fill hands the real device silence while the take runs and keeps the SDK's
//     AudioFill for itself (the stream queues then fill to apu.max_queued_frames and the guest mixer stalls: the same "mixer
//     cannot run" state as taking the credits);
//   - the mixer frames covering the video frames so far are then produced on demand: the game's audio callback (sub_823F56D0, already
//     overridden in hooks/audio_mixer.cpp, run by the "Audio Worker" per credit) waits at a gate for a ticket, and StepBegin hands out
//     as many tickets as the book's Wanted() says (the old ReleaseSome); each ticket is one frame mixed AFTER the update asked for its
//     sound, which is the exactness the old credits gave;
//   - StepEnd waits until that many frames are queued (IAudioSystem::GetStats().queued_frames, with the timeout the old code had) and
//     pulls them by calling the AudioFill itself, for 2 or 6 channels: the SDK has folded and gained them already, so Convert() and
//     the rex/audio/conversion.h and downmix.h helpers (not public in 0.11) are not needed;
//   - Arm drains the frames queued before the take the same way (calls the fill, discards), End lets the device pull again.
// Until then Begin() says so once and returns false, which the take already handles (capture_mode.cpp: no sound, the take goes on).
//

#include "capture_audio.h"

#include "settings/larecomp_settings.h"
#include <rex/ppc/recompiled_code.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "capture_audio_core.h"
#include "larecomp_log.h"


// The listener hook (below): the emitter system's per-frame update.
REX_EXTERN(__imp__rex_sub_8272A130);

namespace mc::cinematic::capture_audio {

namespace {

uint32_t Swap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

// the listener the emitter system was given
std::atomic<uint32_t> g_lst_calls{0};
std::atomic<uint32_t> g_lst_ptr{0};
std::atomic<uint32_t> g_lst_bits[3] = {};  // float bits of x y z

Summary g_last;

}  // namespace

// ---------------------------------------------------------------------------------------------------------------- the listener

// Called by the hook below with the position the emitter system was given.
void NoteListener(uint32_t pos, const uint8_t* base) {
    if (pos < 0x10000u || !base) return;
    g_lst_ptr.store(pos, std::memory_order_relaxed);
    for (uint32_t i = 0; i < 3; ++i) {
        uint32_t raw;
        std::memcpy(&raw, base + pos + 4 * i, 4);
        g_lst_bits[i].store(Swap32(raw), std::memory_order_relaxed);
    }
    g_lst_calls.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------------------------- the interface

bool Begin(double, const std::string&, const std::string&, uint64_t) {
    g_last = Summary{};
    if (!larecomp::settings::mcla_capture_audio.Get()) {
        LARECOMP_APP_INFO("[cap] audio: mcla_capture_audio is off: the real output plays during the take and no WAV is made");
        return false;
    }
    static std::atomic<bool> said{false};
    if (!said.exchange(true)) {
        LARECOMP_APP_WARN("[cap] audio: the take's sound is not ported to the rexglue 0.11 audio system yet (see capture_audio.cpp): the take has no sound");
    }
    return false;
}

void StepBegin(uint64_t) {}

void StepEnd() {}

Summary End(uint64_t) { return Summary{}; }

bool Active() { return false; }

const Summary& LastSummary() {
    return g_last;
}

}  // namespace mc::cinematic::capture_audio

// The emitter system's per-frame update, called by the game loop with a pointer to the position it listens at. It only notes the position.
extern "C" REX_FUNC(rex_sub_8272A130) {
    mc::cinematic::capture_audio::NoteListener(ctx.r4.u32, base);
    __imp__rex_sub_8272A130(ctx, base);
}

#endif  // !REXGLUE_HAS_XEO3_TARGET
