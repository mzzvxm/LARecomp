#ifndef REXGLUE_HAS_XEO3_TARGET
//
// A take's sound (trailer toolkit M2c; feature-parity/CINEMATIC-PLAN.md, CINEMATIC-DESIGN.md section 2.4 item 7). The maths and the books
// are in capture_audio_core.h (with its test); this file is the driver, the take's calls and the CSV.
//
// HOW THE GAME'S AUDIO RUNS. The SDK's audio worker (rex/audio/audio_system.cpp) waits on a semaphore that holds credits, one for every
// 256-sample frame (5.33 ms at 48 kHz, six channels, planar, big endian float) the mixer may make. For each credit it runs the game's
// callback (sub_823F56D0, hooks/audio_mixer.cpp), which has the game's mixer thread mix a frame and ends in XAudioSubmitRenderDriverFrame,
// which is the driver's SubmitFrame. The real driver (SDL, or the silent one of audio_output.h) gives the credit back when it has played
// the frame, so the mixer runs at most queue-length frames ahead of the speakers (8 frames = 42.7 ms; audio_maxqframes, 4 to 64) and
// its output is not tied to the update that asked for a sound.
//
// WHAT THE DRIVER DOES. audio_output.h puts CaptureOutputDriver in front of the real output. Outside a take it passes every frame to the
// real output and nothing else changes. In a take (Arm .. Finish):
//   - Arm: from now on frames go to the take's queue, not to the real output. The credits are taken out of the semaphore (the mixer
//     cannot run any more) and so are the ones the real output still gives back as it plays out what it had queued, until every credit
//     of the SDK's queue is in hand (credits drained + frames the mixer made meanwhile = the queue length). What the mixer made ahead of
//     the take is thrown away. The real device keeps its stream open and plays silence.
//   - every video frame: the book says how many mixer frames cover the video's samples so far (800 a frame at 60 fps = 3.125 mixer
//     frames, the fraction carried exactly: capture_audio_core.h). StepBegin releases that many credits at the frame gate, before the wait for
//     the frame's readback, so the mixer runs while the picture is read back; StepEnd waits for them, folds them to stereo with the SDK's own
//     fold and gain (or keeps the six channels), and writes exactly the samples the video frame owns to the WAV. The game thread is
//     held at the gate for all of it, so the sounds an update asked for are the ones the mixer mixes next.
//   - a mixer frame that does not arrive within mcla_capture_audio_timeout_ms is given up on: a frame of silence takes its place, the
//     frame that comes late is thrown away, the WAV keeps its length; three such steps in a row stop the waiting for the rest of the take
//     (the file is padded with silence to the video's length).
//   - Finish: the WAV's header gets its sizes, the CSV is closed, the credits go back into the semaphore (all of them: the semaphore
//     is full again as at the start of the game) and the mixer refills the real output's queue by itself, as at start.
//
// The CSV, `<wav name>.audio.csv`: a row per video frame: frame, update, the mixer frames released, used, thrown away and silent this
// step, the samples written, the running total, what it should be (SamplesThrough), their difference (always 0: the file is cut to the
// video's length at every frame), the tail of mixed samples not yet written (under 256), the wait for the mixer, the loudest sample of
// the frame, the game's audio clock (0x8286CF34 / 0x8286CF38: it advances dt a frame, so it gains 1 s per take second when 48,000 samples
// are a game second), and the listener position the emitter system was given (below).
//
// The listener. The game loop calls sub_8272A130(emitter manager, position) once a frame with a pointer to three floats: the player car's
// matrix + 48, or the camera ring's current matrix + 240 (E7's reading of the generated code). It is the only place in the loop where
// a listener position reaches the audio code that was found [I]; whether the mixer's panning uses the same copy is not known (CHECK):
// the CSV has the pointer and the position per frame so that a run with a camera away from the car decides it.
//
// CHECK (the first run; nothing here has run):
//   1. That one client is the mixer (the log says which client index was made capture-capable; a second client is only passed through).
//   2. That the drain at Arm ends with "drained + arrived = queue length" (the [cap] audio: take armed line), not on its time-outs.
//   3. That a sound the update asked for is in the mixer frames of that very step (a guest thread other than the game's could handle sound
//      commands on its own clock): the horn's onset must sit at a constant offset from its press, and equal intervals between presses
//      must give equal intervals in the WAV to within one mixer frame (ports/scripts/wav_check.py).
//   4. The SDK's audio worker sleeps 500 ms after a wait that timed out (audio_system.cpp WorkerThreadMain): a take whose frames are more
//      than 0.5 s apart may wait up to that for the mixer per frame (the render scale 6 case). Not met at scale 1 to 3.
//   5. The game's audioFrameTimer (sub_82144888) reads the host clock: a sound that times itself with it runs in wall time, not take time.
//   6. That the mixer's callback never waits for something the game thread holds while it sits at the gate (a guest lock): the first sign
//      would be a step that gives up (the log's "the mixer gave no frame"), three in a row end the take's sound.
//

#include "capture_audio.h"

#include <rex/audio/conversion.h>
#include <rex/audio/downmix.h>
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "capture_audio_core.h"
#include "larecomp_log.h"

REXCVAR_DEFINE_BOOL(mcla_capture_audio, true, "MCLA/Cinematic",
    "TRAILER TOOLKIT capture mode: during a take the game's audio output is replaced by one that mixes exactly one video frame of sound "
    "per frame and writes it to <take>.wav; the real device is given back when the take ends. Off: the real output plays during the "
    "take, in real time, and no WAV is made. Read when a take starts.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();
REXCVAR_DEFINE_BOOL(mcla_capture_audio_surround, false, "MCLA/Cinematic",
    "TRAILER TOOLKIT capture mode: the take's WAV has the six channels of the game's 5.1 mix (fl fr fc lfe bl br) instead of the stereo "
    "fold the real output plays. Read when a take starts.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();
REXCVAR_DEFINE_INT32(mcla_capture_audio_timeout_ms, 2000, "MCLA/Cinematic",
    "TRAILER TOOLKIT capture mode: how long the game waits at its frame gate for the mixer's frames of one video frame before it goes on "
    "with a frame of silence for each that has not come (counted in the audio summary; three in a row stop the waiting for the take).")
    .range(50, 60000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();

// The listener hook (below): the emitter system's per-frame update.
REX_EXTERN(__imp__rex_sub_8272A130);

namespace mc::cinematic::capture_audio {

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kFrameChannels = 6;                              // the mixer's frame: 6 channels planar
constexpr uint32_t kFrameFloats = kMixerFrame * kFrameChannels;
constexpr uint32_t kAudioClockF = 0x8286CF34;                       // the audio engine's clock, float seconds (perf/TIMER-NOTES.md)
constexpr uint32_t kAudioClockMs = 0x8286CF38;                      // ... and whole milliseconds
constexpr uint32_t kTimeoutsToDead = 3;

uint32_t Swap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

uint8_t* Membase() {
    auto* rt = rex::Runtime::instance();
    return rt ? rt->virtual_membase() : nullptr;
}

float GuestFloat(const uint8_t* base, uint32_t ea) {
    uint32_t raw;
    std::memcpy(&raw, base + ea, 4);
    raw = Swap32(raw);
    float f;
    std::memcpy(&f, &raw, 4);
    return f;
}

// The SDK's queue length: audio_maxqframes clamped as audio_system.cpp does (0: not known).
uint32_t SdkQueueLength() {
    const std::string v = rex::cvar::GetFlagByName("audio_maxqframes");
    if (v.empty()) return 0;
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 10);
    if (end == v.c_str()) return 0;
    return static_cast<uint32_t>(std::clamp<long>(n, 4, 64));
}

// the listener the emitter system was given
std::atomic<uint32_t> g_lst_calls{0};
std::atomic<uint32_t> g_lst_ptr{0};
std::atomic<uint32_t> g_lst_bits[3] = {};  // float bits of x y z

struct Frame {
    float data[kFrameFloats];
};

class CaptureOutputDriver;
std::atomic<CaptureOutputDriver*> g_driver{nullptr};
Summary g_last;

class CaptureOutputDriver final : public rex::audio::AudioDriver {
  public:
    CaptureOutputDriver(rex::memory::Memory* memory, rex::thread::Semaphore* frame_semaphore, rex::audio::AudioDriver* real, size_t index)
        : AudioDriver(memory), sem_(frame_semaphore), real_(real) {
        CaptureOutputDriver* none = nullptr;
        if (g_driver.compare_exchange_strong(none, this, std::memory_order_acq_rel)) {
            primary_ = true;
            LARECOMP_APP_INFO("[cap] audio: client {} is the capture driver's: a take will take its mixer over (real output behind it)", index);
        } else {
            LARECOMP_APP_WARN("[cap] audio: client {} is a second render client of the game: its frames pass through, takes use the first", index);
        }
    }

    ~CaptureOutputDriver() override {
        if (primary_) {
            CaptureOutputDriver* me = this;
            g_driver.compare_exchange_strong(me, nullptr, std::memory_order_acq_rel);
        }
        Abort();
    }

    // The audio worker's thread (the game's mixer through XAudioSubmitRenderDriverFrame): a frame of the mixer.
    void SubmitFrame(uint32_t frame_ptr) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!capturing_) {
            if (announce_restore_) {
                announce_restore_ = false;
                LARECOMP_APP_INFO("[cap] audio: the mixer is back on the real output (first frame after the take)");
            }
            if (real_) real_->SubmitFrame(frame_ptr);
            return;
        }
        const float* in = memory_->TranslateVirtual<float*>(frame_ptr);
        queue_.emplace_back();
        std::memcpy(queue_.back().data, in, sizeof(Frame::data));
        ++arrived_total_;
        cv_.notify_all();
    }

    rex::audio::AudioDriver* TakeReal() {
        rex::audio::AudioDriver* r = real_;
        real_ = nullptr;
        return r;
    }

    bool Armed() const { return armed_; }

    // ---------------------------------------------------------------------------------------------------------------- the take

    bool Arm(double fps, const std::string& wav_path, uint64_t first_update) {
        if (armed_) {
            LARECOMP_APP_WARN("[cap] audio: a take's audio was still running: closed as it was");
            Abort();
        }
        if (!(fps > 0.0)) return false;
        channels_ = REXCVAR_GET(mcla_capture_audio_surround) ? 6 : 2;
        {
            std::error_code ec;
            const std::filesystem::path p(wav_path);
            if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
        }
        if (!wav_.Open(wav_path, channels_, kRate)) {
            LARECOMP_APP_ERROR("[cap] audio: cannot open {}: the take has no sound", wav_path);
            return false;
        }
        std::string csv_path = wav_path;
        if (csv_path.size() > 4 && csv_path.compare(csv_path.size() - 4, 4, ".wav") == 0) csv_path.resize(csv_path.size() - 4);
        csv_path += ".audio.csv";
        csv_ = std::fopen(csv_path.c_str(), "wb");
        if (csv_) {
            std::fprintf(csv_,
                         "#cin_capture_audio v1 fps=%g samples_per_frame=%.6f channels=%u first_update=%llu\n"
                         "frame,update,released,used,discarded,silent,samples,cum_samples,expected_samples,drift_samples,residual,wait_ms,"
                         "peak,audio_clock_s,audio_ms,lst_calls,lst_ptr,lst_x,lst_y,lst_z\n",
                         fps, static_cast<double>(kRate) / fps, channels_, static_cast<unsigned long long>(first_update));
        } else {
            LARECOMP_APP_WARN("[cap] audio: cannot open {}: the WAV is made without it", csv_path);
        }
        wav_path_ = wav_path;
        fps_ = fps;
        first_update_ = first_update;
        book_.Begin(fps);
        pcm_.clear();
        frames_ = 0;
        timeouts_ = 0;
        timeouts_in_a_row_ = 0;
        dead_ = false;
        wav_failed_logged_ = false;
        last_row_frames_ = 0;
        wait_sum_ms_ = 0;
        wait_max_ms_ = 0;
        steps_ = 0;
        late_dropped_ = 0;
        step_released_ = step_used_ = step_discarded_ = step_silent_ = 0;

        // From here the mixer's frames are the take's.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.clear();
            arrived_total_ = 0;
            capturing_ = true;
        }
        // Take every credit of the SDK's queue: those in the semaphore, and those the real output gives back as it plays out what it had
        // queued. Frames the mixer makes while we do it are the take's queue too (it took a credit before we did).
        const uint32_t queue_length = SdkQueueLength();
        uint64_t drained = 0;
        const auto begin = Clock::now();
        auto last_change = begin;
        uint64_t last_total = 0;
        for (;;) {
            while (rex::thread::Wait(sem_, false, std::chrono::milliseconds(0)) == rex::thread::WaitResult::kSuccess) ++drained;
            uint64_t arrived;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                arrived = arrived_total_;
            }
            const uint64_t total = drained + arrived;
            const auto now = Clock::now();
            if (total != last_total) {
                last_total = total;
                last_change = now;
            }
            if (queue_length != 0 && total >= queue_length) break;                                  // every credit is in hand
            if (now - last_change >= std::chrono::milliseconds(150)) break;                           // nothing came for a while
            if (now - begin >= std::chrono::milliseconds(1500)) break;                                // give up
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        uint64_t arrived;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            arrived = arrived_total_;
            preroll_ = static_cast<uint32_t>(queue_.size());
            queue_.clear();
        }
        held_ = drained + arrived;
        credits_ = static_cast<uint32_t>(held_);
        if (queue_length != 0 && held_ < queue_length) {
            // credits still out there (the real output has not played them yet, or the mixer is busy): the frames that come from them
            // show up in the take's queue as extra frames; Restore counts them as credits back
            LARECOMP_APP_WARN("[cap] audio: only {} of the {} credits came back in {:.0f} ms; the others come as extra mixer frames during the take",
                              held_, queue_length, std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
        }
        armed_ = true;
        LARECOMP_APP_INFO("[cap] audio: take armed at {:g} fps = {:.4f} samples per video frame = {:.5f} mixer frames; {} credits taken from the mixer "
                          "({} drained, {} frames it had mixed ahead thrown away), {} channels, {}",
                          fps, static_cast<double>(kRate) / fps, static_cast<double>(kRate) / fps / kMixerFrame, held_, drained, preroll_, channels_,
                          wav_path);
        return true;
    }

    // The gate, before the wait for the readback: `n` video frames exist.
    void StepBegin(uint64_t n) {
        if (!armed_) return;
        frames_ = std::max(frames_, n);
        step_released_ = step_used_ = step_discarded_ = step_silent_ = 0;
        if (dead_) return;
        Absorb(false);  // late frames of an earlier step: thrown away, their credits taken back
        step_released_ += ReleaseSome();
    }

    // The gate, after the readback.
    void StepEnd() {
        if (!armed_) return;
        const auto t0 = Clock::now();
        bool timed_out = false;
        if (!dead_) {
            const auto deadline = t0 + std::chrono::milliseconds(REXCVAR_GET(mcla_capture_audio_timeout_ms));
            for (int guard = 0; guard < 4096; ++guard) {
                bool met;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    met = cv_.wait_until(lock, deadline, [&] { return queue_.size() >= book_.QueueNeeded(); });
                }
                Absorb(!met);
                if (!met) {
                    timed_out = true;
                    break;
                }
                const uint64_t more = ReleaseSome();  // the rest of the video frame's mixer frames when the credits in hand were fewer
                step_released_ += more;
                if (more == 0) break;
            }
            if (timed_out) {
                ++timeouts_;
                ++timeouts_in_a_row_;
                LARECOMP_APP_WARN("[cap] audio: video frame {}: the mixer gave no frame in {} ms; silence in its place ({} step(s) in a row)", frames_ - 1,
                                  REXCVAR_GET(mcla_capture_audio_timeout_ms), timeouts_in_a_row_);
                if (timeouts_in_a_row_ >= kTimeoutsToDead && !dead_) {
                    dead_ = true;
                    LARECOMP_APP_ERROR("[cap] audio: {} steps in a row without the mixer: the rest of the take's WAV is silence", timeouts_in_a_row_);
                }
            } else {
                timeouts_in_a_row_ = 0;
            }
        }
        const double wait_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        wait_sum_ms_ += wait_ms;
        wait_max_ms_ = std::max(wait_max_ms_, wait_ms);
        ++steps_;

        // the file: exactly what the video frames so far own
        const uint64_t shortfall = book_.Shortfall(frames_);
        if (shortfall > 0) {
            book_.Pad(shortfall);
            pcm_.insert(pcm_.end(), static_cast<size_t>(shortfall) * channels_, 0.0f);
        }
        const uint64_t w = book_.Writable(frames_);
        float peak = 0.0f;
        const size_t floats = static_cast<size_t>(w) * channels_;
        for (size_t i = 0; i < floats && i < pcm_.size(); ++i) peak = std::max(peak, std::fabs(pcm_[i]));
        if (w > 0) {
            if (!wav_.Append(pcm_.data(), w) && !wav_failed_logged_) {
                wav_failed_logged_ = true;
                LARECOMP_APP_ERROR("[cap] audio: writing {} failed (disk full, or the WAV is at its size limit): the rest of the take's sound is lost", wav_path_);
            }
            pcm_.erase(pcm_.begin(), pcm_.begin() + std::min(floats, pcm_.size()));
        }
        book_.Wrote(w);
        if (frames_ > last_row_frames_) {  // one row for each video frame (a gate that saw no new frame has nothing to say)
            WriteRow(wait_ms, w, peak);
            last_row_frames_ = frames_;
        }
    }

    Summary Finish(uint64_t video_frames) {
        Summary s;
        if (!armed_) return s;
        if (video_frames > frames_) {  // frames the gate did not cover
            StepBegin(video_frames);
            StepEnd();
        }
        s.ran = true;
        s.wav = wav_path_;
        s.channels = channels_;
        s.fps = fps_;
        s.video_frames = video_frames;
        s.samples = book_.Written();
        s.expected = SamplesThrough(video_frames, fps_);
        s.mixer_frames = book_.TotalReleased();
        s.timeouts = timeouts_;
        s.silent_frames = book_.Silent();
        s.late_dropped = late_dropped_;
        s.padded_samples = book_.Padded();
        s.preroll_frames = preroll_;
        s.credits = credits_;
        s.wait_ms_mean = steps_ ? wait_sum_ms_ / static_cast<double>(steps_) : 0.0;
        s.wait_ms_max = wait_max_ms_;
        s.wav_failed = wav_.Failed();
        CloseFiles();
        s.wav_failed = s.wav_failed || wav_.Failed();
        s.ok = s.timeouts == 0 && s.samples == s.expected && !s.wav_failed && s.padded_samples == 0;
        const uint64_t credits_back = held_;
        Restore();
        armed_ = false;
        LARECOMP_APP_INFO("[cap] audio summary frames={} fps={:g} samples={} expected={} ({}) channels={} mixer_frames={} timeouts={} silent_mixer_frames={} "
                          "late_dropped={} padded_samples={} preroll_frames={} credits={}/{} wait_ms mean/max={:.2f}/{:.2f} wav={}",
                          s.video_frames, s.fps, s.samples, s.expected, s.samples == s.expected ? "exact" : "NOT EXACT", s.channels, s.mixer_frames, s.timeouts,
                          s.silent_frames, s.late_dropped, s.padded_samples, s.preroll_frames, credits_back, s.credits, s.wait_ms_mean, s.wait_ms_max, s.wav);
        if (!s.ok) LARECOMP_APP_ERROR("[cap] audio: the take's sound is not clean (see the lines above): {}", s.wav);
        return s;
    }

    // The take stopped without Finish (the game shut down): the files are closed as they are.
    void Abort() {
        if (!armed_) {
            CloseFiles();
            return;
        }
        CloseFiles();
        Restore();  // the credits in hand go back, so the mixer is not left without them
        armed_ = false;
    }

  private:
    // The queue's frames: late ones of a step that gave up are thrown away, this take's are folded onto the timeline. `give_up`: the wait ran
    // out, whatever is outstanding goes in as silence.
    void Absorb(bool give_up) {
        std::vector<Frame> used;
        Book::Pop p;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            p = book_.Consume(queue_.size());
            for (uint64_t i = 0; i < p.discard; ++i) queue_.pop_front();
            for (uint64_t i = 0; i < p.use; ++i) {
                used.push_back(queue_.front());
                queue_.pop_front();
            }
        }
        held_ += p.discard + p.use;  // each frame taken off the queue is a credit back
        late_dropped_ += p.discard;
        step_used_ += p.use;
        step_discarded_ += p.discard;
        for (const Frame& f : used) Convert(f);
        if (give_up && book_.Outstanding() > 0) {
            const uint64_t n = book_.GiveUp();
            step_silent_ += n;
            pcm_.insert(pcm_.end(), static_cast<size_t>(n) * kMixerFrame * channels_, 0.0f);
        }
    }

    // One mixer frame: folded to the WAV's channels as the real output would (the SDK's own fold and gain).
    void Convert(const Frame& f) {
        float out[kFrameFloats];
        if (channels_ == 2) {
            rex::audio::conversion::sequential_6_BE_to_interleaved_2_LE(out, f.data, kMixerFrame, rex::audio::GetStereoFold(), rex::audio::GetOutputGain());
        } else {
            rex::audio::conversion::sequential_6_BE_to_interleaved_6_LE(out, f.data, kMixerFrame, rex::audio::GetSurroundMix(), rex::audio::GetOutputGain());
        }
        pcm_.insert(pcm_.end(), out, out + static_cast<size_t>(kMixerFrame) * channels_);
    }

    // The credits the book wants released for the frames so far, as many as are in hand. Returns how many went.
    uint64_t ReleaseSome() {
        const uint64_t give = std::min(book_.Wanted(frames_), held_);
        const uint64_t done = ReleaseCredits(give);
        if (done > 0) {
            book_.Released(done);
            held_ -= done;
        }
        return done;
    }

    // `n` credits into the semaphore. One call; when that would exceed what the semaphore holds (credits that came from the real output after
    // the drain), one at a time until it is full. Returns how many went in.
    uint64_t ReleaseCredits(uint64_t n) {
        if (n == 0) return 0;
        if (sem_->Release(static_cast<int>(n), nullptr)) return n;
        uint64_t done = 0;
        while (done < n && sem_->Release(1, nullptr)) ++done;
        return done;
    }

    void WriteRow(double wait_ms, uint64_t samples, float peak) {
        if (!csv_) return;
        float clock_s = 0.0f;
        uint32_t clock_ms = 0;
        if (const uint8_t* base = Membase()) {
            clock_s = GuestFloat(base, kAudioClockF);
            std::memcpy(&clock_ms, base + kAudioClockMs, 4);
            clock_ms = Swap32(clock_ms);
        }
        float lx, ly, lz;
        const uint32_t bx = g_lst_bits[0].load(), by = g_lst_bits[1].load(), bz = g_lst_bits[2].load();
        std::memcpy(&lx, &bx, 4);
        std::memcpy(&ly, &by, 4);
        std::memcpy(&lz, &bz, 4);
        const uint64_t frame = frames_ > 0 ? frames_ - 1 : 0;
        const uint64_t cum = book_.Written();
        const uint64_t expected = book_.Target(frames_);
        std::fprintf(csv_, "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%lld,%llu,%.3f,%.6f,%.6f,%u,%u,0x%08X,%.6f,%.6f,%.6f\n",
                     static_cast<unsigned long long>(frame), static_cast<unsigned long long>(first_update_ + frame),
                     static_cast<unsigned long long>(step_released_), static_cast<unsigned long long>(step_used_),
                     static_cast<unsigned long long>(step_discarded_), static_cast<unsigned long long>(step_silent_),
                     static_cast<unsigned long long>(samples), static_cast<unsigned long long>(cum), static_cast<unsigned long long>(expected),
                     static_cast<long long>(cum) - static_cast<long long>(expected), static_cast<unsigned long long>(book_.Available()), wait_ms, peak,
                     clock_s, clock_ms, g_lst_calls.load(), g_lst_ptr.load(), lx, ly, lz);
    }

    void CloseFiles() {
        wav_.Close();
        if (csv_) {
            std::fclose(csv_);
            csv_ = nullptr;
        }
    }

    // The take is over: frames go to the real output again and it gets its mixer back (every credit in hand goes into the semaphore).
    void Restore() {
        uint64_t extra;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            capturing_ = false;
            announce_restore_ = true;
            extra = queue_.size();  // frames nobody asked for (credits from the real output that came after the drain): their credits are ours too
            queue_.clear();
        }
        held_ += extra;
        const uint64_t back = ReleaseCredits(held_);
        if (back < held_) {
            LARECOMP_APP_WARN("[cap] audio: gave {} of {} credits back to the mixer (the semaphore was full: the real output had returned the rest)", back, held_);
        }
        held_ = 0;
    }

    rex::thread::Semaphore* sem_;
    rex::audio::AudioDriver* real_;
    bool primary_ = false;

    // the mixer's side (the audio worker's thread writes, the game thread reads)
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Frame> queue_;
    bool capturing_ = false;
    bool announce_restore_ = false;  // log the first frame that goes to the real output again
    uint64_t arrived_total_ = 0;

    // the take (game thread)
    bool armed_ = false;
    bool dead_ = false;
    bool wav_failed_logged_ = false;
    Book book_;
    WavFile wav_;
    std::FILE* csv_ = nullptr;
    std::string wav_path_;
    std::vector<float> pcm_;  // the timeline's samples not yet written, interleaved
    uint32_t channels_ = 2;
    double fps_ = 60.0;
    uint64_t first_update_ = 0;
    uint64_t frames_ = 0;     // video frames the take has so far
    uint64_t last_row_frames_ = 0;  // ... and how many the CSV has a row for
    uint64_t held_ = 0;       // credits in hand
    uint32_t credits_ = 0, preroll_ = 0;
    uint64_t late_dropped_ = 0, steps_ = 0;
    uint32_t timeouts_ = 0, timeouts_in_a_row_ = 0;
    double wait_sum_ms_ = 0, wait_max_ms_ = 0;
    uint64_t step_released_ = 0, step_used_ = 0, step_discarded_ = 0, step_silent_ = 0;
};

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

rex::audio::AudioDriver* WrapDriver(rex::memory::Memory* memory, rex::thread::Semaphore* frame_semaphore, rex::audio::AudioDriver* real, size_t index) {
    return new CaptureOutputDriver(memory, frame_semaphore, real, index);
}

rex::audio::AudioDriver* Unwrap(rex::audio::AudioDriver* driver) {
    auto* ours = dynamic_cast<CaptureOutputDriver*>(driver);
    if (!ours) return driver;
    rex::audio::AudioDriver* real = ours->TakeReal();
    delete ours;
    return real;
}

bool Begin(double fps, const std::string& wav_path, uint64_t first_update) {
    g_last = Summary{};
    if (!REXCVAR_GET(mcla_capture_audio)) {
        LARECOMP_APP_INFO("[cap] audio: mcla_capture_audio is off: the real output plays during the take and no WAV is made");
        return false;
    }
    CaptureOutputDriver* d = g_driver.load(std::memory_order_acquire);
    if (!d) {
        LARECOMP_APP_WARN("[cap] audio: no capture driver (the game's audio system is not the one audio_output.h makes, or it has no render client yet): the take has no sound");
        return false;
    }
    return d->Arm(fps, wav_path, first_update);
}

void StepBegin(uint64_t video_frames) {
    if (CaptureOutputDriver* d = g_driver.load(std::memory_order_acquire)) d->StepBegin(video_frames);
}

void StepEnd() {
    if (CaptureOutputDriver* d = g_driver.load(std::memory_order_acquire)) d->StepEnd();
}

Summary End(uint64_t video_frames) {
    CaptureOutputDriver* d = g_driver.load(std::memory_order_acquire);
    if (!d || !d->Armed()) return Summary{};
    g_last = d->Finish(video_frames);
    return g_last;
}

bool Active() {
    CaptureOutputDriver* d = g_driver.load(std::memory_order_acquire);
    return d && d->Armed();
}

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
