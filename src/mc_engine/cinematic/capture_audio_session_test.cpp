// Check of a take's sound session (capture_audio_core.h: Session, against a MixerPort): the order of the calls capture mode makes, the
// tickets, the frames pulled off the queue, the WAV. The mixer is a fake with a thread of its own that makes one frame per ticket (its value
// says which one it is), has a queue of 8 like the SDK's and a stale queue from before the take. A program of its own: no game, no ReXGlue.
// It includes capture_audio_core.cpp itself, so one file builds it (tools/unit-test.ps1 or the CMake target larecomp_capture_audio_session_test).
//
// What it checks:
//   - the file holds exactly the video's samples after EVERY frame (SamplesThrough), at rates from 10 to 120 fps, stereo and 5.1;
//   - the sound in it is the mixer's, in order, the frames mixed after the take began (none of the stale queue), no frame twice or lost;
//   - the mixer never has more frames out than the cap, also when one video frame needs more mixer frames than the queue holds (15 fps);
//   - a mixer that stalls once: that step gives up (silence in its place), the late frame is thrown away, the sound after it is where it
//     would have been, the length stays exact; a mixer that stops for good: three steps and the rest is silence, the length stays exact;
//   - a port that cannot be armed, a WAV that cannot be made: Begin says no and leaves the mixer free.

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "capture_audio_core.cpp"

using namespace mc::cinematic::capture_audio;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAILED: %s\n", what.c_str());
    }
}

void CheckU(uint64_t got, uint64_t want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failed;
        std::printf("FAILED: %s: got %llu, want %llu\n", what.c_str(), static_cast<unsigned long long>(got), static_cast<unsigned long long>(want));
    }
}

// ---------------------------------------------------------------------------------------------------------------- the fake mixer

// One frame per ticket, made after `delay_ms(n)` ms (n = 1, 2, ... in the order made), pushed on a queue of `queue_max` (the thread waits for
// room, as the SDK's audio worker waits for a credit). Frame n's samples are n + 0.25 * channel; frames queued before the take are -1.
class FakeMixer final : public MixerPort {
  public:
    explicit FakeMixer(uint32_t stale = 8, uint32_t queue_max = 8) : queue_max_(queue_max) {
        for (uint32_t i = 0; i < stale; ++i) queue_.push_back(-1.0f);
        thread_ = std::thread([this] { Run(); });
    }
    ~FakeMixer() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

    std::function<int(uint64_t)> delay_ms = [](uint64_t) { return 0; };
    bool can_arm = true;
    int arms = 0, disarms = 0;
    uint64_t max_out = 0;   // the most frames out at once: handed and not yet pulled
    uint64_t made() {
        std::lock_guard<std::mutex> lock(mutex_);
        return made_;
    }

    int64_t Arm(uint32_t channels) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++arms;
        if (!can_arm) return -1;
        channels_ = channels;
        const int64_t n = static_cast<int64_t>(queue_.size());
        queue_.clear();
        armed_ = true;
        return n;
    }
    void Disarm() override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++disarms;
        armed_ = false;
    }
    uint64_t Queued() override {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }
    bool Pull(float* out, uint32_t channels) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty() || !armed_) return false;
        const float v = queue_.front();
        queue_.pop_front();
        ++pulled_;
        for (uint32_t s = 0; s < kMixerFrame; ++s)
            for (uint32_t c = 0; c < channels; ++c) out[s * channels + c] = v + 0.25f * static_cast<float>(c);
        cv_.notify_all();
        return true;
    }
    void Hand(uint64_t n) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tickets_ += n;
            handed_ += n;
            max_out = std::max(max_out, handed_ - pulled_);
        }
        cv_.notify_all();
    }
    void Wait(uint32_t ms) override {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(ms));
    }
    uint64_t Cap() override { return queue_max_ - 1; }

  private:
    void Run() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            cv_.wait(lock, [&] { return stop_ || (tickets_ > 0 && queue_.size() < queue_max_); });
            if (stop_) return;
            --tickets_;
            const uint64_t n = ++made_;
            const int d = delay_ms(n);
            if (d > 0) {  // the mixer is busy (the lock is free meanwhile); a stop ends it
                cv_.wait_for(lock, std::chrono::milliseconds(d), [&] { return stop_; });
                if (stop_) return;
            }
            queue_.push_back(static_cast<float>(n));
            cv_.notify_all();
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<float> queue_;
    uint32_t queue_max_;
    uint32_t channels_ = 2;
    bool armed_ = false;
    bool stop_ = false;
    uint64_t tickets_ = 0, made_ = 0, handed_ = 0, pulled_ = 0;
    std::thread thread_;
};

std::string TempWav(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

// The samples of a WAV the Session wrote (after its header), interleaved.
std::vector<float> ReadWav(const std::string& path, uint32_t channels) {
    std::vector<float> v;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return v;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    const long header = static_cast<long>(WavHeaderSize(channels));
    if (size > header) {
        v.resize(static_cast<size_t>(size - header) / sizeof(float));
        std::fseek(f, header, SEEK_SET);
        if (std::fread(v.data(), sizeof(float), v.size(), f) != v.size()) v.clear();
    }
    std::fclose(f);
    return v;
}

struct Run {
    Summary summary;
    uint64_t rows = 0;
    uint64_t bad_rows = 0;           // rows whose file was not exactly the video's length
    uint64_t bad_cut = 0;            // video frames after which the file was not exactly SamplesThrough
    std::vector<std::string> warnings;
    std::vector<float> wav;
};

// The calls capture mode makes: for each video frame n = 1..frames: StepBegin(n), (the readback), StepEnd(); then Finish(frames).
Run Take(FakeMixer& mixer, double fps, uint64_t frames, uint32_t channels, int timeout_ms, const char* name, uint64_t extra_quiet_ms = 0) {
    Run r;
    const std::string path = TempWav(name);
    std::filesystem::remove(path);
    Session session;
    Session::Config cfg;
    cfg.fps = fps;
    cfg.channels = channels;
    cfg.timeout_ms = timeout_ms;
    cfg.wav_path = path;
    cfg.log = [&](int level, const std::string& line) {
        if (level >= 1) r.warnings.push_back(line);
    };
    uint64_t last_frame = 0;
    cfg.row = [&](const StepRow& row) {
        ++r.rows;
        if (row.drift != 0) ++r.bad_rows;
        last_frame = row.frame;
    };
    if (!session.Begin(mixer, cfg)) return r;
    for (uint64_t n = 1; n <= frames; ++n) {
        session.StepBegin(n);
        session.StepEnd();
        if (last_frame + 1 != n) ++r.bad_cut;
    }
    if (extra_quiet_ms) std::this_thread::sleep_for(std::chrono::milliseconds(extra_quiet_ms));
    r.summary = session.Finish(frames);
    r.wav = ReadWav(path, channels);
    std::filesystem::remove(path);
    return r;
}

// Every sample of the file is the mixer's: frame i of the timeline holds the value i + 1 (or 0 where the take gave a frame up), channel c adds
// c / 4. Returns the number of timeline frames that are silent, and sets `bad` to the number of samples that are neither.
uint64_t CheckContent(const std::vector<float>& wav, uint32_t channels, uint64_t samples, uint64_t& bad) {
    bad = 0;
    uint64_t silent_frames = 0;
    std::vector<bool> silent((samples + kMixerFrame - 1) / kMixerFrame, false);
    for (uint64_t s = 0; s < samples; ++s) {
        const uint64_t frame = s / kMixerFrame;
        for (uint32_t c = 0; c < channels; ++c) {
            const float got = wav[s * channels + c];
            const float want = static_cast<float>(frame + 1) + 0.25f * static_cast<float>(c);
            if (got == want) continue;
            if (got == 0.0f) {
                silent[frame] = true;
                continue;
            }
            ++bad;
        }
    }
    for (bool b : silent) silent_frames += b ? 1 : 0;
    return silent_frames;
}

// ---------------------------------------------------------------------------------------------------------------- the checks

void TestExact() {
    struct Case {
        double fps;
        uint64_t frames;
        uint32_t channels;
    };
    const Case cases[] = {{60.0, 300, 2},  {59.94, 300, 2}, {30.0, 150, 2}, {24.0, 120, 2},  {120.0, 400, 2},
                          {15.0, 60, 2},   {10.0, 40, 2},   {60.0, 120, 6}, {23.976, 100, 6}};
    int i = 0;
    for (const Case& c : cases) {
        FakeMixer mixer;
        const std::string tag = std::to_string(c.fps) + " fps x " + std::to_string(c.frames) + ", " + std::to_string(c.channels) + " ch";
        const std::string name = "larecomp_cap_session_" + std::to_string(i++) + ".wav";
        Run r = Take(mixer, c.fps, c.frames, c.channels, 2000, name.c_str());
        const uint64_t want = SamplesThrough(c.frames, c.fps);
        Check(r.summary.ran && r.summary.ok, tag + ": the take's sound ran clean");
        CheckU(r.summary.samples, want, tag + ": the file holds exactly the video's samples");
        CheckU(r.summary.expected, want, tag + ": and that is what the summary expects");
        CheckU(r.rows, c.frames, tag + ": one row for each video frame");
        CheckU(r.bad_rows, 0, tag + ": the file was exact after every frame");
        CheckU(r.bad_cut, 0, tag + ": every step closed its video frame");
        CheckU(r.summary.preroll_frames, 8, tag + ": the 8 frames queued before the take were thrown away");
        CheckU(r.summary.mixer_frames, MixerFramesCovering(want), tag + ": only the mixer frames the video needs were asked for");
        CheckU(r.summary.timeouts, 0, tag + ": no timeout");
        CheckU(r.summary.late_dropped, 0, tag + ": nothing thrown away");
        CheckU(r.summary.padded_samples, 0, tag + ": nothing padded");
        Check(mixer.max_out <= mixer.Cap(), tag + ": the mixer never had more than the cap out (" + std::to_string(mixer.max_out) + ")");
        Check(r.warnings.empty(), tag + ": nothing warned");
        CheckU(r.wav.size(), want * c.channels, tag + ": the file read back has that many samples");
        if (r.wav.size() == want * c.channels) {
            uint64_t bad = 0;
            const uint64_t silent = CheckContent(r.wav, c.channels, want, bad);
            CheckU(bad, 0, tag + ": every sample is the mixer's, in order (no stale frame, none twice, none lost)");
            CheckU(silent, 0, tag + ": no silence");
        }
        Check(mixer.arms == 1 && mixer.disarms == 1, tag + ": armed once, given back once");
    }
}

// A mixer slower than the readback is waited for, not given up on (the frames are in the file, in order).
void TestSlowMixer() {
    FakeMixer mixer;
    mixer.delay_ms = [](uint64_t) { return 1; };  // 1 ms a frame
    Run r = Take(mixer, 60.0, 100, 2, 2000, "larecomp_cap_session_slow.wav");
    CheckU(r.summary.samples, SamplesThrough(100, 60.0), "slow mixer: exact length");
    Check(r.summary.ok && r.summary.timeouts == 0, "slow mixer: waited for, no timeout");
    Check(r.summary.wait_ms_max > 0.5, "slow mixer: the game thread did wait for it (" + std::to_string(r.summary.wait_ms_max) + " ms)");
    uint64_t bad = 0;
    CheckContent(r.wav, 2, SamplesThrough(100, 60.0), bad);
    CheckU(bad, 0, "slow mixer: the sound is the mixer's, in order");
}

// One stall: the mixer takes 70 ms over frame 41 with a 30 ms timeout.
void TestOneStall() {
    FakeMixer mixer;
    mixer.delay_ms = [](uint64_t n) { return n == 41 ? 70 : 0; };
    Run r = Take(mixer, 60.0, 120, 2, 30, "larecomp_cap_session_stall.wav", 150);
    const uint64_t want = SamplesThrough(120, 60.0);
    CheckU(r.summary.samples, want, "one stall: the file keeps its exact length");
    Check(r.summary.timeouts >= 1 && r.summary.timeouts < 3 && !r.summary.dead, "one stall: it timed out once or twice and the take's sound went on");
    Check(!r.summary.ok, "one stall: and the summary says the take is not clean");
    Check(r.summary.silent_frames >= 1, "one stall: silence went in for the frames that did not come");
    CheckU(r.summary.late_dropped, r.summary.silent_frames, "one stall: the same number of late frames were thrown away");
    CheckU(r.summary.padded_samples, 0, "one stall: nothing needed padding");
    CheckU(r.bad_rows, 0, "one stall: the file was exact after every frame");
    Check(r.wav.size() == want * 2, "one stall: the file read back has the video's samples");
    if (r.wav.size() == want * 2) {
        uint64_t bad = 0;
        const uint64_t silent = CheckContent(r.wav, 2, want, bad);
        CheckU(bad, 0, "one stall: every sample is the mixer's or silence");
        CheckU(silent, r.summary.silent_frames, "one stall: the silent frames are the ones given up on");
    }
    Check(!r.warnings.empty(), "one stall: it was warned about");
}

// A mixer that never answers again after frame 20: three steps and the take's sound is over; the file is still the video's length.
void TestDeadMixer() {
    FakeMixer mixer;
    mixer.delay_ms = [](uint64_t n) { return n > 20 ? 60000 : 0; };
    Run r = Take(mixer, 60.0, 40, 2, 20, "larecomp_cap_session_dead.wav");
    const uint64_t want = SamplesThrough(40, 60.0);
    CheckU(r.summary.samples, want, "dead mixer: the file keeps its exact length");
    Check(r.summary.dead && !r.summary.ok, "dead mixer: the summary says so");
    CheckU(r.summary.timeouts, 3, "dead mixer: three steps waited");
    Check(r.summary.padded_samples > 0, "dead mixer: the rest was padded with silence");
    CheckU(r.bad_rows, 0, "dead mixer: the file was exact after every frame");
    // the frames that did come are in the file, in order
    uint64_t bad = 0;
    if (r.wav.size() == want * 2) CheckContent(r.wav, 2, want, bad);
    CheckU(bad, 0, "dead mixer: what is in the file is the mixer's or silence");
}

void TestCannotArm() {
    FakeMixer mixer;
    mixer.can_arm = false;
    const std::string path = TempWav("larecomp_cap_session_noarm.wav");
    std::filesystem::remove(path);
    Session session;
    Session::Config cfg;
    cfg.wav_path = path;
    Check(!session.Begin(mixer, cfg), "no device: Begin says no");
    Check(!session.Armed(), "no device: nothing armed");
    Check(!std::filesystem::exists(path), "no device: no file made");
    session.StepBegin(1);
    session.StepEnd();
    Check(!session.Finish(1).ran, "no device: the calls after it do nothing");
    CheckU(mixer.made(), 0, "no device: no ticket was handed");
}

void TestCannotOpenWav() {
    FakeMixer mixer;
    // a path under an existing FILE cannot be made
    const std::string blocker = TempWav("larecomp_cap_session_blocker.txt");
    {
        std::FILE* f = std::fopen(blocker.c_str(), "wb");
        if (f) std::fclose(f);
    }
    Session session;
    Session::Config cfg;
    cfg.wav_path = blocker + "/take.wav";
    std::vector<std::string> errors;
    cfg.log = [&](int level, const std::string& line) {
        if (level >= 2) errors.push_back(line);
    };
    Check(!session.Begin(mixer, cfg), "no WAV: Begin says no");
    Check(mixer.arms == 1 && mixer.disarms == 1, "no WAV: the mixer that was armed is given back");
    Check(errors.size() == 1, "no WAV: the error was logged once");
    std::error_code ec;
    std::filesystem::remove(blocker, ec);
}

// Two takes one after the other on the same mixer: the second starts clean.
void TestTwoTakes() {
    FakeMixer mixer;
    Run a = Take(mixer, 60.0, 60, 2, 2000, "larecomp_cap_session_two_a.wav");
    Run b = Take(mixer, 30.0, 30, 2, 2000, "larecomp_cap_session_two_b.wav");
    Check(a.summary.ok && b.summary.ok, "two takes: both clean");
    CheckU(b.summary.samples, SamplesThrough(30, 30.0), "two takes: the second has its own exact length");
    uint64_t bad = 0;
    // the second take's frames carry on counting where the mixer was, so only the order and the length are checked
    CheckContent(a.wav, 2, a.summary.samples, bad);
    CheckU(bad, 0, "two takes: the first one's sound is the mixer's");
    CheckU(b.summary.preroll_frames, 0, "two takes: nothing was left queued by the first");
}

// The gate called for a frame it already had (the same count twice) does nothing new.
void TestRepeatedGate() {
    FakeMixer mixer;
    Session session;
    Session::Config cfg;
    cfg.fps = 60.0;
    cfg.wav_path = TempWav("larecomp_cap_session_repeat.wav");
    uint64_t rows = 0;
    cfg.row = [&](const StepRow&) { ++rows; };
    Check(session.Begin(mixer, cfg), "repeated gate: Begin");
    session.StepBegin(1);
    session.StepEnd();
    session.StepBegin(1);
    session.StepEnd();
    session.StepBegin(2);
    session.StepEnd();
    CheckU(rows, 2, "repeated gate: one row per video frame");
    const Summary s = session.Finish(2);
    CheckU(s.samples, SamplesThrough(2, 60.0), "repeated gate: exact length");
    std::error_code ec;
    std::filesystem::remove(cfg.wav_path, ec);
}

// Finish for frames the gate did not cover (the take ended on a frame no gate saw): they are written then.
void TestFinishCovers() {
    FakeMixer mixer;
    Session session;
    Session::Config cfg;
    cfg.fps = 60.0;
    cfg.wav_path = TempWav("larecomp_cap_session_finish.wav");
    Check(session.Begin(mixer, cfg), "finish: Begin");
    session.StepBegin(10);
    session.StepEnd();
    const Summary s = session.Finish(25);
    CheckU(s.samples, SamplesThrough(25, 60.0), "finish: the frames the gate never saw are in the file");
    Check(s.ok, "finish: clean");
    std::error_code ec;
    std::filesystem::remove(cfg.wav_path, ec);
}

}  // namespace

int main() {
    TestExact();
    TestSlowMixer();
    TestOneStall();
    TestDeadMixer();
    TestCannotArm();
    TestCannotOpenWav();
    TestTwoTakes();
    TestRepeatedGate();
    TestFinishCovers();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
