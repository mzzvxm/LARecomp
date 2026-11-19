#pragma once
//
// A take's sound (trailer toolkit M2c), the pure half: how many samples a number of video frames owns, how many of the mixer's
// 256-sample frames must be released to cover them, the books of what was released, what came back and what was written, the WAV
// file, and the take's session (Session: the order of the calls against a MixerPort). It knows nothing of the game, the SDK or the
// log, so capture_audio_test.cpp and capture_audio_session_test.cpp build it on their own. capture_audio.cpp is the glue (the real
// MixerPort on the SDK's audio system, the cvars, the CSV).
//
// THE TIMELINE. The take's WAV is one sample every 1/48000 s from the take's first frame. Video frame k (0-based) owns the samples
//
//     [ SamplesThrough(k), SamplesThrough(k + 1) )          SamplesThrough(n) = round(n * 48000 / fps)
//
// so after n frames the file holds exactly SamplesThrough(n) samples: 48000 / fps per frame, 800 at 60 fps, 2000 at 24, and for a
// rate that does not divide 48000 (59.94: 800.8) the fraction is not a running sum that could drift but a rounding of n times the
// exact ratio, so after any number of frames the file is within half a sample of the video's length (60 s at 60 fps is 2,880,000
// samples, ten minutes 28,800,000). The game's second is the take's second: capture mode gives the engine timer exactly 1/fps in
// the game's own ticks (capture_core.h), so one game second is 48000 samples.
//
// THE MIXER. The game's mixer makes one 256-sample frame (5.33 ms) for every ticket the take hands it (on rexglue 0.11 the game's audio
// callback, which the SDK's audio worker runs for every credit, waits at a gate for a ticket: capture_audio.cpp). To cover
// SamplesThrough(n) samples it needs ceil(SamplesThrough(n) / 256) frames in all: the frame that holds the end of a video
// frame is mixed with the game's state as of that video frame, and its tail (under 256 samples) is written as the start of the
// next video frame. So a sound the game starts in update k is in the first mixer frame mixed after update k's commands: the audio
// never leads its picture, and trails it by under one mixer frame (5.33 ms). Book keeps the counts; Session does the waiting.
//
// A frame that does not arrive in time is given up on (GiveUp): the timeline gets a frame of silence for it and the next frame
// that arrives is thrown away (skip), so one stall never moves the sound against the picture, and the file keeps its exact length.
//

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace mc::cinematic::capture_audio {

inline constexpr uint32_t kRate = 48000;       // the game's mixer output rate
inline constexpr uint32_t kMixerFrame = 256;   // samples per channel in one frame of the mixer (5.333 ms)

// ---------------------------------------------------------------------------------------------------------------- the timeline

// The samples the first `video_frames` frames own at `fps` (> 0): round(video_frames * 48000 / fps), computed from the product
// itself, never as a sum of rounded steps. 0 for a rate that is not a positive number.
uint64_t SamplesThrough(uint64_t video_frames, double fps);
// Mixer frames needed to hold `samples`: ceil(samples / 256).
uint64_t MixerFramesCovering(uint64_t samples);

// The books of one take's audio. The glue calls them in this order every video frame, on the game thread:
//     Wanted(n)  -> hand that many tickets (Released), wait for them: Consume(queued) says how many frames of the queue to throw
//     away and how many are this take's; the frames that did not arrive: GiveUp(); then Writable(n) samples go to the file (Wrote).
class Book {
  public:
    void Begin(double fps);
    double Fps() const { return fps_; }
    // The samples the file holds after `video_frames` frames.
    uint64_t Target(uint64_t video_frames) const { return SamplesThrough(video_frames, fps_); }

    // ---- release
    // Mixer frames still to release so that the timeline can reach Target(video_frames) (0 when it can already).
    uint64_t Wanted(uint64_t video_frames) const;
    void Released(uint64_t n) { released_ += n; }
    uint64_t TotalReleased() const { return released_; }

    // ---- arrival
    // Released and not yet on the timeline.
    uint64_t Outstanding() const { return released_ - counted_; }
    // Frames the queue has to hold for every released frame to be in it: the late ones to throw away and the outstanding ones.
    uint64_t QueueNeeded() const { return skip_ + Outstanding(); }
    struct Pop {
        uint64_t discard = 0;  // the first frames of the queue: late ones of a step that gave up (thrown away)
        uint64_t use = 0;      // the next frames: this take's, in order (now on the timeline)
    };
    // The queue holds `queued` frames: the split, booked. The oldest frames are the oldest credits, so the late ones come first.
    Pop Consume(uint64_t queued);
    // The wait ran out: every outstanding frame goes onto the timeline as silence and the same number of frames that arrive later
    // are thrown away. Returns how many frames of silence.
    uint64_t GiveUp();
    uint64_t Skipping() const { return skip_; }
    uint64_t Silent() const { return silent_; }

    // ---- the file
    // Mixer frames on the timeline (real or silent), and the samples they hold that the file does not yet.
    uint64_t Counted() const { return counted_; }
    uint64_t Available() const { return counted_ * kMixerFrame + padded_ - written_; }
    uint64_t Written() const { return written_; }
    // How many samples to write now for `video_frames` frames: what the timeline has of the target, not more than the target.
    uint64_t Writable(uint64_t video_frames) const;
    void Wrote(uint64_t n) { written_ += n; }
    // Samples of the target the timeline lacks even after everything that can arrive has (a mixer that stopped answering).
    uint64_t Shortfall(uint64_t video_frames) const;
    // The file gets `n` samples of silence for them (keeps the length exact); counted apart.
    void Pad(uint64_t n) {
        padded_ += n;
        pad_total_ += n;
    }
    uint64_t Padded() const { return pad_total_; }

  private:
    double fps_ = 60.0;
    uint64_t released_ = 0;  // credits given to the mixer
    uint64_t counted_ = 0;   // mixer frames on the timeline
    uint64_t skip_ = 0;      // late frames still to throw away
    uint64_t silent_ = 0;    // frames of silence put on the timeline for frames given up
    uint64_t written_ = 0;   // samples in the file
    uint64_t padded_ = 0;    // samples of silence that count as timeline (Pad), taken off Available as they are written
    uint64_t pad_total_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------- the WAV file

// RIFF / WAVE / fmt / fact / data for 32 bit float PCM with `sample_frames` samples per channel in the data chunk (the sizes). One or
// two channels: a plain 16 byte fmt chunk; more: WAVE_FORMAT_EXTENSIBLE with the speaker mask of the game's 5.1 order (fl fr fc lfe bl
// br). The size depends only on `channels`.
size_t WavHeaderSize(uint32_t channels);
std::vector<uint8_t> WavHeader(uint32_t channels, uint32_t rate, uint64_t sample_frames);
// The largest data chunk a WAV can describe (its size is 32 bits).
inline constexpr uint64_t kWavMaxDataBytes = 0xFFFFFFFFull - 128;

// A WAV file written as the take goes: the header first with zero sizes, the samples after it, the real sizes at Close.
class WavFile {
  public:
    WavFile() = default;
    WavFile(const WavFile&) = delete;
    WavFile& operator=(const WavFile&) = delete;
    ~WavFile() { Close(); }

    // False: the file could not be made (the parent folder is not made here).
    bool Open(const std::string& path, uint32_t channels, uint32_t rate);
    bool IsOpen() const { return file_ != nullptr; }
    // Appends `sample_frames` frames of interleaved floats. False: the write failed or the file is at the size a WAV can hold
    // (the rest of the take's samples are dropped and failed() says so).
    bool Append(const float* interleaved, uint64_t sample_frames);
    // The sizes, the file closed. False when any write failed.
    bool Close();
    uint64_t Frames() const { return frames_; }
    uint32_t Channels() const { return channels_; }
    bool Failed() const { return failed_; }

  private:
    std::FILE* file_ = nullptr;
    uint32_t channels_ = 2;
    uint32_t rate_ = kRate;
    uint64_t frames_ = 0;
    bool failed_ = false;
};

// ---------------------------------------------------------------------------------------------------------------- the take's session

// What a take's audio did, for the log and for the take folder's take.json.
struct Summary {
    bool ran = false;               // a take had audio (Begin worked)
    bool ok = false;                // exactly the video's samples, no timeout, nothing given up
    std::string wav;
    uint32_t channels = 2;
    double fps = 0;
    uint64_t video_frames = 0;
    uint64_t samples = 0;           // in the file
    uint64_t expected = 0;          // round(frames * 48000 / fps)
    uint64_t mixer_frames = 0;      // tickets handed to the mixer
    uint32_t timeouts = 0;          // steps that gave up waiting for the mixer
    uint64_t silent_frames = 0;     // mixer frames put in as silence for those
    uint64_t late_dropped = 0;      // frames that arrived after their step gave up (thrown away)
    uint64_t padded_samples = 0;    // samples of silence added so that the file keeps its length
    uint32_t preroll_frames = 0;    // frames the mixer had made before the take, thrown away
    double wait_ms_mean = 0;        // the game thread's wait for the mixer, beyond the overlap with the readback
    double wait_ms_max = 0;
    bool wav_failed = false;
    bool dead = false;              // the mixer went away (three steps in a row without a frame, or the device was lost)
};

// What a take needs from the game's mixer. capture_audio.cpp makes the real one (the SDK's stream queue, the tap in front of the device and the
// gate in the audio worker's callback), capture_audio_session_test.cpp a fake that runs a mixer thread.
class MixerPort {
  public:
    virtual ~MixerPort() = default;
    // The take takes the mixer over: whatever played the queue out stops, the mixer's callbacks wait for a ticket each, the ones already
    // running are waited for, and the frames queued before the take are thrown away. Returns how many were thrown away, or -1: cannot (no
    // open output device: the SDK's own clock then plays the queue out and cannot be held back).
    virtual int64_t Arm(uint32_t channels) = 0;
    // The take is over: the mixer runs free again and the output plays the queue out as before.
    virtual void Disarm() = 0;
    // Frames the stream queue holds now.
    virtual uint64_t Queued() = 0;
    // Takes the oldest frame off the queue: `channels` (2 or 6) interleaved floats per sample, kMixerFrame samples, folded and gained as the
    // real output would play them. False: it cannot (the device is gone); nothing is written.
    virtual bool Pull(float* out, uint32_t channels) = 0;
    // The mixer may mix `n` more frames.
    virtual void Hand(uint64_t n) = 0;
    // Waits for the mixer to have done something, at most `ms` (it may return sooner or for nothing).
    virtual void Wait(uint32_t ms) = 0;
    // How many frames the mixer may have out at once (handed and not yet pulled): the queue's length less one, so that it never has to wait
    // for a credit.
    virtual uint64_t Cap() = 0;
};

// What the CSV of a take gets for a video frame (a step whose gate saw a new frame).
struct StepRow {
    uint64_t frame = 0;           // 0-based video frame
    uint64_t released = 0;        // tickets handed this step
    uint64_t used = 0;            // mixer frames that came and went onto the timeline
    uint64_t discarded = 0;       // late frames of an earlier step thrown away
    uint64_t silent = 0;          // frames of silence put in for frames that did not come
    uint64_t samples = 0;         // written to the file now
    uint64_t cum = 0;             // in the file
    uint64_t expected = 0;        // what the video owns
    int64_t drift = 0;            // cum - expected (0: the file is cut to the video's length at every frame)
    uint64_t residual = 0;        // mixed samples not yet written (under one mixer frame)
    double wait_ms = 0;
    float peak = 0;               // the loudest sample written
};

// One take's audio, in the order capture mode calls it (game thread): Begin; for every frame gate StepBegin(n) then, after the frame's readback,
// StepEnd(); Finish. It owns the books, the WAV and the arithmetic; the port does the mixer's side.
class Session {
  public:
    using Logger = std::function<void(int level, const std::string& line)>;  // level 0 info, 1 warning, 2 error
    struct Config {
        double fps = 60.0;
        uint32_t channels = 2;      // 2, or 6 for the game's 5.1
        int timeout_ms = 2000;      // the wait for one step's frames
        std::string wav_path;
        Logger log;                                 // may be empty
        std::function<void(const StepRow&)> row;    // called at the end of every StepEnd that closed a video frame; may be empty
    };

    // False: nothing was armed or written (the port cannot take the mixer, or the WAV cannot be made).
    bool Begin(MixerPort& port, const Config& config);
    bool Armed() const { return armed_; }
    // The gate of an update, before the wait for the frame's readback: `video_frames` frames of the take exist. Hands the mixer tickets and returns.
    void StepBegin(uint64_t video_frames);
    // The gate, after the readback: waits for the mixer's frames, writes the video frames' samples to the WAV.
    void StepEnd();
    // The take ends: closes the WAV and gives the mixer back. The summary (ran = false when no take had audio).
    Summary Finish(uint64_t video_frames);
    // The frames the preroll threw away at Begin.
    uint32_t Preroll() const { return preroll_; }

  private:
    uint64_t HandSome();
    void PullQueued(uint64_t queued);
    void Say(int level, const std::string& line) const {
        if (cfg_.log) cfg_.log(level, line);
    }

    MixerPort* port_ = nullptr;
    Config cfg_;
    bool armed_ = false;
    bool dead_ = false;
    bool wav_failed_logged_ = false;
    Book book_;
    WavFile wav_;
    std::vector<float> pcm_;       // the timeline's samples not yet written, interleaved
    std::vector<float> scratch_;   // one mixer frame
    uint64_t frames_ = 0;          // video frames the take has so far
    uint64_t last_row_frames_ = 0;
    uint32_t preroll_ = 0;
    uint64_t late_dropped_ = 0;
    uint64_t steps_ = 0;
    uint32_t timeouts_ = 0;
    uint32_t timeouts_in_a_row_ = 0;
    double wait_sum_ms_ = 0;
    double wait_max_ms_ = 0;
    StepRow step_;                 // the counters of the step in hand
};

}  // namespace mc::cinematic::capture_audio
