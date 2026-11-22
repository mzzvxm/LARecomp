#ifndef REXGLUE_HAS_XEO3_TARGET
//
// A take's sound (trailer toolkit M2c; feature-parity/CINEMATIC-PLAN.md, CINEMATIC-DESIGN.md section 2.4 item 7). The maths, the books, the
// WAV and the order of the calls are in capture_audio_core.h (Session, with its tests); this file is the mixer port on rexglue 0.11's audio
// system, the take's calls and the CSV. (The 0.10 version, a CaptureOutputDriver in front of the SDK's render client, is
// `git show 792f2fe6:src/mc_engine/cinematic/capture_audio.cpp`; the SDK seam it used is gone.)
//
// HOW THE GAME'S AUDIO RUNS ON 0.11 (read from the SDK's src/kernel/apu.cpp and src/apu/audio_system.cpp). The game registers one render client
// (XAudioRegisterRenderDriverClient); the SDK opens a stream for it and gives its semaphore 8 credits (apu.max_queued_frames). The "Audio
// Worker" thread waits on the semaphore and runs the game's callback (sub_823F56D0, hooks/audio_mixer.cpp) once per credit; the callback has the
// game's mixer thread mix one 256-sample frame (6 channels, 5.33 ms) and ends in XAudioSubmitRenderDriverFrame, which queues the frame on the
// stream. The mix (AudioSystem's mix_period) takes one frame off every stream's queue, folds and gains it, and releases the stream's credit.
// Who runs the mix: the output device, through the AudioFill it was opened with (when no device is open the SDK's timer thread does).
//
// THE TAP. The device's fill is made by mcla::audio::MixTap (music/audio_tap.h; audio_output.h passes the SDK's fill through it). A take Holds
// it: the device then gets silence and the SDK's mix runs only when the take pulls (MixTap::Pull), one 256-sample period of 2 or 6 channels at a
// time, the SDK's own fold and gain included (apu.gain, apu.mute, the surround weights: the WAV is what the speakers would have played).
//
// THE GATE. Holding the tap stops the credits coming back, so the mixer would stop at its queue limit, but it would still run 8 frames ahead of
// the take's state. So the callback is wrapped (MixerGate, called from hooks/audio_mixer.cpp): while a take runs it waits for a ticket before it
// mixes a frame. The take hands tickets only for the video frames so far (StepBegin), so every frame in the WAV was mixed after the update that
// asked for its sound, as the 0.10 take had it with credits.
//
// A TAKE (Session, capture_audio_core.cpp):
//   - Arm (below, Begin): hold the tap, close the gate, wait for the callbacks already running (they finish into the queue), pull one period at
//     the OTHER channel count (the SDK's mix keeps the unread rest of a period for the same count: the pull at the other count drops it, so
//     the take's first pull starts on a period boundary), then pull and throw away every frame queued before the take.
//   - every video frame: StepBegin hands the tickets the book wants (at most the queue length less one out at once, so the worker never waits on an
//     empty credit semaphore); StepEnd waits until they are queued, pulling them as they come (so a video frame that needs more mixer frames than the
//     queue holds, below 23 fps, goes through), and writes exactly the samples the video frame owns.
//   - a frame that does not arrive within mcla_capture_audio_timeout_ms is given up on: silence in its place, the frame that comes late is thrown
//     away; three such steps in a row end the waiting (the file is padded with silence to the video's length).
//   - Finish: the WAV gets its sizes, the gate opens (the parked callback runs, the queue refills with the game's state of then) and the device
//     gets the mix again.
// No output device open (no speakers, mcla_audio_output = null, a broken driver): the SDK mixes on its own timer and cannot be held back, so
// Begin says so and the take has no sound.
//
// The CSV, `<wav name>.audio.csv`: a row per video frame: frame, update, the tickets handed, used, thrown away and silent this step, the samples
// written, the running total, what it should be (SamplesThrough), their difference (always 0: the file is cut to the video's length at every frame),
// the tail of mixed samples not yet written (under 256), the wait for the mixer, the loudest sample of the frame, the game's audio clock (0x8286CF34 /
// 0x8286CF38: it advances dt a frame, so it gains 1 s per take second when 48,000 samples are a game second), and the listener position the
// emitter system was given (below).
//
// The listener. The game loop calls sub_8272A130(emitter manager, position) once a frame with a pointer to three floats: the player car's
// matrix + 48, or the camera ring's current matrix + 240 (E7's reading of the generated code). It is the only place in the loop where
// a listener position reaches the audio code that was found [I]; whether the mixer's panning uses the same copy is not known (CHECK):
// the CSV has the pointer and the position per frame so that a run with a camera away from the car decides it.
//
// CHECK (the first run; nothing here has run on 0.11, the books and the protocol are tested against a fake mixer in
//        capture_audio_session_test.cpp, the tap in audio_tap_test.cpp):
//   1. That a callback's frame is in the stream's queue when the callback returns (the game's callback ends in XAudioSubmitRenderDriverFrame,
//      read from the 0.10 notes, not re-read here). If it is not, StepEnd finds the queue empty and polls every 5 ms: the CSV's wait_ms shows
//      it, the take is still exact.
//   2. That the game has ONE stream (the log line "[cap] audio: take armed" says how many the SDK has open). With two the queue counter sums them
//      and the mix takes a frame off each: a frame of the second stream would count as the first's.
//   3. That the device's fill is silent and clean while held and the mix comes back without a pop at the end of the take (the real output has
//      its queue refilled by the callbacks the open gate lets through).
//   4. That a sound the update asked for is in the mixer frames of that very step (a guest thread other than the game's could handle sound
//      commands on its own clock): the horn's onset must sit at a constant offset from its press, and equal intervals between presses
//      must give equal intervals in the WAV to within one mixer frame (ports/scripts/wav_check.py).
//   5. The SDK's audio worker sleeps 500 ms after a wait that timed out (apu.cpp WorkerMain): the cap on the tickets keeps it from waiting on
//      an empty semaphore, so it should not meet this; a take whose frames are more than 0.5 s apart AND need more than 7 mixer frames each
//      (below 23 fps at a huge render scale) might.
//   6. The game's audioFrameTimer (sub_82144888) reads the host clock: a sound that times itself with it runs in wall time, not take time.
//   7. That the mixer's callback never waits for something the game thread holds while it sits at the gate (a guest lock): the first sign
//      would be a step that gives up (the log's "the mixer gave no frame"), three in a row end the take's sound.
//   8. The worker parked in the gate when the SDK pauses or stops it: nothing in the SDK or LARecomp calls TitleRuntime::SetAudioPaused (grep),
//      and the window's close calls Abort() (larecomp_app.cpp FinishWindowCloseWork), which opens the gate; a shutdown that does not come through
//      that function would see the SDK's 500 ms "Audio worker still running" warning, no more.
//   9. apu.mute / apu.mute_unfocused / apu.gain apply to the WAV (it is the SDK's mix): a take made with the window out of focus and
//      apu.mute_unfocused on is silent.
//

#include "capture_audio.h"
#include "settings/immortal.h"

#include "settings/larecomp_settings.h"
#include <rex/apu/audio_system.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/ppc/context.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "capture_audio_core.h"
#include "larecomp_guest.h"
#include "larecomp_log.h"
#include "mc_engine/music/audio_output.h"


// The listener hook (below): the emitter system's per-frame update.
REX_EXTERN(__imp__rex_sub_8272A130);

namespace mc::cinematic::capture_audio {

namespace {

constexpr uint32_t kAudioClockF = 0x8286CF34;   // the audio engine's clock, float seconds (perf/TIMER-NOTES.md)
constexpr uint32_t kAudioClockMs = 0x8286CF38;  // ... and whole milliseconds
constexpr uint32_t kMixerFrameChannels = 6;      // the largest frame a pull can be (the 5.1 fold)

uint32_t Swap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

float GuestFloat(const uint8_t* base, uint32_t ea) {
    uint32_t raw;
    std::memcpy(&raw, base + ea, 4);
    raw = Swap32(raw);
    float f;
    std::memcpy(&f, &raw, 4);
    return f;
}

// the listener the emitter system was given
std::atomic<uint32_t> g_lst_calls{0};
std::atomic<uint32_t> g_lst_ptr{0};
std::atomic<uint32_t> g_lst_bits[3] = {};  // float bits of x y z

// ---------------------------------------------------------------------------------------------------------------- the mixer port

// The mixer as the take sees it: the SDK's stream queue (IAudioSystem's counters), the tap in front of the device (pulls) and the gate in the
// audio worker's callback (tickets).
class RealMixer final : public MixerPort {
  public:
    // ---- the take (game thread)

    int64_t Arm(uint32_t channels) override {
        rex::apu::IAudioSystem* audio = mcla::audio::AudioSystem();
        if (!audio) {
            LARECOMP_APP_WARN("[cap] audio: the app has no audio system connected: the take has no sound");
            return -1;
        }
        const rex::apu::AudioStats before = audio->GetStats();
        if (before.open_streams == 0) {
            LARECOMP_APP_WARN("[cap] audio: the game has no audio stream open: the take has no sound");
            return -1;
        }
        if (!mcla::audio::Tap().Hold()) {
            LARECOMP_APP_WARN("[cap] audio: no output device is open (no speakers, mcla_audio_output = null or a driver that did not start): the SDK "
                              "mixes on its own clock and cannot be held back, so the take has no sound");
            return -1;
        }
        audio_ = audio;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            armed_ = true;  // from here a callback that starts waits for a ticket
            tickets_ = 0;
        }
        {
            // the callbacks that were running finish into the queue (about 0.7 ms each)
            std::unique_lock<std::mutex> lock(mutex_);
            if (!cv_.wait_for(lock, std::chrono::milliseconds(500), [&] { return running_ == 0; })) {
                LARECOMP_APP_WARN("[cap] audio: a mixer callback was still running 500 ms after the take took over (its frame is thrown away, or counts as the take's)");
            }
        }
        const uint64_t queued = audio->GetStats().queued_frames;
        std::vector<float> scratch(static_cast<size_t>(kMixerFrame) * kMixerFrameChannels, 0.0f);
        // The SDK's mix keeps the unread rest of a period (the device pulls in steps that are not a multiple of 256 samples) for the channel
        // count it was mixed for: a pull at the other count throws it away and mixes a fresh period, so the pulls of the take start on a period.
        const uint32_t other = channels == 2 ? kMixerFrameChannels : 2;
        mcla::audio::Tap().Pull(std::span<float>(scratch.data(), static_cast<size_t>(kMixerFrame) * other), other);
        for (uint64_t guard = 0; guard < 1000; ++guard) {  // the frames mixed before the take
            if (audio->GetStats().queued_frames == 0) break;
            if (!Pull(scratch.data(), channels)) break;
        }
        LARECOMP_APP_INFO("[cap] audio: take armed: {} stream(s) open, {} frame(s) were queued and thrown away, queue length {}, {} frames may be out at once",
                          before.open_streams, queued, audio->GetSettings().max_queued_frames.Get(), Cap());
        if (before.open_streams != 1) {
            LARECOMP_APP_WARN("[cap] audio: {} streams are open: the take counts the frames of all of them as the game's (see capture_audio.cpp CHECK 2)",
                              before.open_streams);
        }
        return static_cast<int64_t>(queued);
    }

    void Disarm() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            armed_ = false;
            tickets_ = 0;
        }
        cv_.notify_all();
        mcla::audio::Tap().Release();
    }

    uint64_t Queued() override { return audio_ ? audio_->GetStats().queued_frames : 0; }

    bool Pull(float* out, uint32_t channels) override {
        return mcla::audio::Tap().Pull(std::span<float>(out, static_cast<size_t>(kMixerFrame) * channels), channels);
    }

    void Hand(uint64_t n) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tickets_ += n;
        }
        cv_.notify_all();
    }

    void Wait(uint32_t ms) override {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(ms), [&] { return completed_ != seen_; });
        seen_ = completed_;
    }

    uint64_t Cap() override {
        const uint64_t length = audio_ ? audio_->GetSettings().max_queued_frames.Get() : 8;
        return length > 1 ? length - 1 : 1;
    }

    // ---- the audio worker (the game's callback)

    void Enter() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (armed_ && tickets_ == 0) cv_.wait(lock);
        if (armed_) --tickets_;
        ++running_;
    }

    void Leave() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --running_;
            ++completed_;
        }
        cv_.notify_all();
    }

  private:
    rex::apu::IAudioSystem* audio_ = nullptr;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool armed_ = false;       // a take runs: callbacks wait for a ticket
    uint64_t tickets_ = 0;     // handed, not yet taken by a callback
    int running_ = 0;          // callbacks between their gate and their end
    uint64_t completed_ = 0;   // callbacks finished
    uint64_t seen_ = 0;        // completed_ as Wait last saw it
};

RealMixer& Mixer() {
    static RealMixer mixer;
    return mixer;
}

// ---------------------------------------------------------------------------------------------------------------- the take

Session& g_session = larecomp::Leak<Session>();
Summary& g_last = larecomp::Leak<Summary>();
std::atomic<bool> g_active{false};
std::FILE* g_csv = nullptr;
uint64_t g_first_update = 0;

void CloseCsv() {
    if (g_csv) {
        std::fclose(g_csv);
        g_csv = nullptr;
    }
}

void LogLine(int level, const std::string& line) {
    if (level >= 2) {
        LARECOMP_APP_ERROR("[cap] audio: {}", line);
    } else if (level == 1) {
        LARECOMP_APP_WARN("[cap] audio: {}", line);
    } else {
        LARECOMP_APP_INFO("[cap] audio: {}", line);
    }
}

void WriteRow(const StepRow& r) {
    if (!g_csv) return;
    float clock_s = 0.0f;
    uint32_t clock_ms = 0;
    if (const uint8_t* base = larecomp::guest::Membase()) {
        clock_s = GuestFloat(base, kAudioClockF);
        std::memcpy(&clock_ms, base + kAudioClockMs, 4);
        clock_ms = Swap32(clock_ms);
    }
    float lx, ly, lz;
    const uint32_t bx = g_lst_bits[0].load(), by = g_lst_bits[1].load(), bz = g_lst_bits[2].load();
    std::memcpy(&lx, &bx, 4);
    std::memcpy(&ly, &by, 4);
    std::memcpy(&lz, &bz, 4);
    std::fprintf(g_csv, "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%lld,%llu,%.3f,%.6f,%.6f,%u,%u,0x%08X,%.6f,%.6f,%.6f\n",
                 static_cast<unsigned long long>(r.frame), static_cast<unsigned long long>(g_first_update + r.frame),
                 static_cast<unsigned long long>(r.released), static_cast<unsigned long long>(r.used), static_cast<unsigned long long>(r.discarded),
                 static_cast<unsigned long long>(r.silent), static_cast<unsigned long long>(r.samples), static_cast<unsigned long long>(r.cum),
                 static_cast<unsigned long long>(r.expected), static_cast<long long>(r.drift), static_cast<unsigned long long>(r.residual), r.wait_ms,
                 r.peak, clock_s, clock_ms, g_lst_calls.load(), g_lst_ptr.load(), lx, ly, lz);
}

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

// ---------------------------------------------------------------------------------------------------------------- the gate

MixerGate::MixerGate() { Mixer().Enter(); }

MixerGate::~MixerGate() { Mixer().Leave(); }

// ---------------------------------------------------------------------------------------------------------------- the interface

bool Begin(double fps, const std::string& wav_path, const std::string& csv_path_asked, uint64_t first_update) {
    g_last = Summary{};
    CloseCsv();
    if (!larecomp::settings::mcla_capture_audio.Get()) {
        LARECOMP_APP_INFO("[cap] audio: mcla_capture_audio is off: the real output plays during the take and no WAV is made");
        return false;
    }
    Session::Config cfg;
    cfg.fps = fps;
    cfg.channels = larecomp::settings::mcla_capture_audio_surround.Get() ? 6 : 2;
    cfg.timeout_ms = larecomp::settings::mcla_capture_audio_timeout_ms.Get();
    cfg.wav_path = wav_path;
    cfg.log = &LogLine;
    cfg.row = &WriteRow;
    g_first_update = first_update;
    if (!g_session.Begin(Mixer(), cfg)) {
        g_active.store(false, std::memory_order_release);
        return false;
    }
    g_active.store(true, std::memory_order_release);

    std::string csv_path = csv_path_asked;
    if (csv_path.empty()) {  // none asked for: next to the WAV, `<wav name>.audio.csv`
        csv_path = wav_path;
        if (csv_path.size() > 4 && csv_path.compare(csv_path.size() - 4, 4, ".wav") == 0) csv_path.resize(csv_path.size() - 4);
        csv_path += ".audio.csv";
    }
    g_csv = std::fopen(csv_path.c_str(), "wb");
    if (g_csv) {
        std::fprintf(g_csv,
                     "#cin_capture_audio v2 fps=%g samples_per_frame=%.6f channels=%u first_update=%llu\n"
                     "frame,update,released,used,discarded,silent,samples,cum_samples,expected_samples,drift_samples,residual,wait_ms,"
                     "peak,audio_clock_s,audio_ms,lst_calls,lst_ptr,lst_x,lst_y,lst_z\n",
                     fps, static_cast<double>(kRate) / fps, cfg.channels, static_cast<unsigned long long>(first_update));
    } else {
        LARECOMP_APP_WARN("[cap] audio: cannot open {}: the WAV is made without it", csv_path);
    }
    LARECOMP_APP_INFO("[cap] audio: take's sound at {:g} fps = {:.4f} samples per video frame = {:.5f} mixer frames, {} channels, {}", fps,
                      static_cast<double>(kRate) / fps, static_cast<double>(kRate) / fps / kMixerFrame, cfg.channels, wav_path);
    return true;
}

void StepBegin(uint64_t video_frames) { g_session.StepBegin(video_frames); }

void StepEnd() { g_session.StepEnd(); }

Summary End(uint64_t video_frames) {
    if (!g_session.Armed()) return Summary{};
    g_last = g_session.Finish(video_frames);
    g_active.store(false, std::memory_order_release);
    CloseCsv();
    const Summary& s = g_last;
    LARECOMP_APP_INFO("[cap] audio summary frames={} fps={:g} samples={} expected={} ({}) channels={} mixer_frames={} timeouts={} silent_mixer_frames={} "
                      "late_dropped={} padded_samples={} preroll_frames={} wait_ms mean/max={:.2f}/{:.2f} wav={}",
                      s.video_frames, s.fps, s.samples, s.expected, s.samples == s.expected ? "exact" : "NOT EXACT", s.channels, s.mixer_frames, s.timeouts,
                      s.silent_frames, s.late_dropped, s.padded_samples, s.preroll_frames, s.wait_ms_mean, s.wait_ms_max, s.wav);
    if (!s.ok) LARECOMP_APP_ERROR("[cap] audio: the take's sound is not clean (see the lines above): {}", s.wav);
    return g_last;
}

void Abort() {
    if (g_active.load(std::memory_order_acquire)) {
        LARECOMP_APP_WARN("[cap] audio: the game is closing during a take: the mixer is given back");
    }
    Mixer().Disarm();
}

void CloseForExit() {
    uint64_t frames = 0;
    if (!g_session.CloseWavForExit(&frames)) return;
    LARECOMP_APP_WARN("[cap] audio: the game ended during a take: its WAV was closed with the {} sample frames written so far (the take is incomplete)", frames);
}

bool Active() { return g_active.load(std::memory_order_acquire); }

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
