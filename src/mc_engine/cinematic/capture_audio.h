#pragma once
//
// A take's sound (trailer toolkit M2c). While a take runs, the game's audio output is replaced by one that advances the mixer exactly
// as far as the video has advanced and writes what it mixed to a WAV; when the take ends the real device gets its mixer back. The
// maths and the books are in capture_audio_core.h (with its test); capture_audio.cpp is the driver and this interface.
//
//   the driver     NOT in the game's audio path until a take that records sound starts: audio_output.h makes the real output (the SDL
//                  device, or the silent one when none opens) alone, exactly as the game without the toolkit has it. Begin then asks the
//                  audio system (DriverHost, below) to put the driver in front of the first render client, and the SDK's audio worker
//                  talks to it from then on; outside a take it hands every frame of the mixer to the real output, untouched. A game that
//                  runs no take, or runs one with mcla_capture_audio off, never has the driver: no wrapper, no log line.
//   a take         capture mode (cinematic/capture_mode.cpp) calls Begin when the take starts, StepBegin / StepEnd at the frame gate
//                  of every update, End when the take ends. The game thread is held at the gate while the frame is read back, so the
//                  mixer runs while the picture is read back and the sound the update asked for is the sound that is mixed.
//   the WAV        where the take folder says: Begin takes the path. 32 bit float, 48 kHz, the stereo fold the real output plays, or
//                  the six channels of the game's 5.1 (mcla_capture_audio_surround); exactly SamplesThrough(frames, fps) samples, so it
//                  is as long as the video to the sample. Next to it, the CSV (the take folder's audio.csv): a row per video frame.
//
// Nothing here does anything unless a take that records sound asks, and a take that has no audio system of ours or has mcla_capture_audio
// off goes on without sound capture: the real output keeps playing and nothing is put in its way.
//

#include <cstddef>
#include <cstdint>
#include <string>

#include <rex/apu/audio_driver.h>
#include <rex/platform/sync.h>
#include <rex/kernel/heap.h>

namespace mc::cinematic::capture_audio {

// ---------------------------------------------------------------------------------------------------------------- the driver

// The audio system's side of the install (audio_output.h's OutputWithFallback implements it and registers itself with SetHost).
class DriverHost {
  public:
    virtual ~DriverHost() = default;
    // Put the capture driver in front of the real output of the first render client the game has, if it is not there yet: one swap of the
    // client's driver pointer under the SDK's own global critical region (the lock AudioSystem::SubmitFrame, UnregisterClient and the
    // worker's client lookup hold), so the audio worker sees either the real output or the wrapper, never half. Game thread. True: a
    // capture driver is in front of a client now. False: the game has no render client yet.
    virtual bool InstallCapture() = 0;
};
// The audio system registers itself when it is made and passes nullptr when it goes (a take in Begin and SetHost exclude each other).
void SetHost(DriverHost* host);

// The capture driver in front of `real` (owned by the result): called by the host's InstallCapture. `index`: the SDK's client.
rex::audio::AudioDriver* WrapDriver(rex::kernel::GuestAddressSpace* memory, rex::platform::Semaphore* frame_semaphore, rex::audio::AudioDriver* real,
                                    size_t index);
// Whether `driver` is a capture driver (what WrapDriver made).
bool IsCaptureDriver(const rex::audio::AudioDriver* driver);
// audio_output.h's DestroyDriver: the real output when `driver` is a capture driver (which is deleted), else `driver` itself.
rex::audio::AudioDriver* Unwrap(rex::audio::AudioDriver* driver);

// ---------------------------------------------------------------------------------------------------------------- the take (game thread)

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
    uint64_t mixer_frames = 0;      // released to the mixer
    uint32_t timeouts = 0;          // steps that gave up waiting for the mixer
    uint64_t silent_frames = 0;     // mixer frames put in as silence for those
    uint64_t late_dropped = 0;      // frames that arrived after their step gave up (thrown away)
    uint64_t padded_samples = 0;    // samples of silence added so that the file keeps its length
    uint32_t preroll_frames = 0;    // frames the mixer had made before the take, thrown away
    uint32_t credits = 0;           // the mixer's credits taken for the take, and given back at its end
    double wait_ms_mean = 0;        // the game thread's wait for the mixer, beyond the overlap with the readback
    double wait_ms_max = 0;
    bool wav_failed = false;
};

// A take starts (capture_mode.cpp BeginTake, before the take's first update): with mcla_capture_audio on, the capture driver is put in front
// of the audio output (when it is not there yet), the mixer is taken over and the WAV opened at `wav_path` (its folder is made). With it off
// nothing is installed or touched. `csv_path`: the per-frame CSV (the take folder's audio.csv); empty, it is `<wav name>.audio.csv` next to the
// WAV. `first_update`: the update of the take's first frame, for the CSV. False: no sound is captured for this take
// (no audio render client, mcla_capture_audio off, or the file cannot be made); the calls below then do nothing.
bool Begin(double fps, const std::string& wav_path, const std::string& csv_path, uint64_t first_update);
// The gate of an update (before the wait for the frame's readback): `video_frames` frames of the take exist. Releases the mixer frames
// that cover them (as many as the credits in hand allow) and returns at once: the mixer runs while the picture is read back.
void StepBegin(uint64_t video_frames);
// The gate, after the readback: waits for the mixer's frames, writes the video frames' samples to the WAV, one row to the CSV.
void StepEnd();
// The take ends (capture_mode.cpp EndTake, after the last StepEnd): finishes the WAV and the CSV and gives the mixer back to the real
// output. `video_frames`: the frames the take has. Logs the summary line.
Summary End(uint64_t video_frames);

// A take's audio is running.
bool Active();
// The last take's summary (what End returned).
const Summary& LastSummary();

}  // namespace mc::cinematic::capture_audio
