#pragma once
//
// A take's sound (trailer toolkit M2c), the pure half: how many samples a number of video frames owns, how many of the mixer's
// 256-sample frames must be released to cover them, the books of what was released, what came back and what was written, and the
// WAV file. It knows nothing of the game, the SDK or the log, so capture_audio_test.cpp builds it on its own. capture_audio.cpp is
// the glue (the driver in front of the real output, the cvars, the CSV).
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
// THE MIXER. The game's mixer makes one 256-sample frame (5.33 ms) for every credit the SDK's audio worker finds on a semaphore.
// To cover SamplesThrough(n) samples it needs ceil(SamplesThrough(n) / 256) frames in all: the frame that holds the end of a video
// frame is mixed with the game's state as of that video frame, and its tail (under 256 samples) is written as the start of the
// next video frame. So a sound the game starts in update k is in the first mixer frame mixed after update k's commands: the audio
// never leads its picture, and trails it by under one mixer frame (5.33 ms). Book keeps the counts; the glue does the waiting.
//
// A frame that does not arrive in time is given up on (GiveUp): the timeline gets a frame of silence for it and the next frame
// that arrives is thrown away (skip), so one stall never moves the sound against the picture, and the file keeps its exact length.
//

#include <cstdint>
#include <cstdio>
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
//     Wanted(n)  -> release that many credits (Released), wait for them: Consume(queued) says how many frames of the queue to throw
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

}  // namespace mc::cinematic::capture_audio
