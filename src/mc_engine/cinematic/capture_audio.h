#pragma once
//
// A take's sound (trailer toolkit M2c). While a take runs, the game's mixer is advanced exactly as far as the video has advanced and what it
// mixed is written to a WAV; when the take ends the real device gets its mixer back. The maths, the books and the order of the calls are in
// capture_audio_core.h (Session, with its tests); capture_audio.cpp is the mixer port on the SDK's audio system and this interface.
//
//   the tap        rexglue 0.11: the SDK's audio system mixes the game's stream (IAudioSystem) and the device pulls the mix through an
//                  AudioFill. LARecomp's output driver (music/audio_output.h) gives the device a fill of its own made by a MixTap
//                  (music/audio_tap.h); a take holds the tap: the device gets silence and the take pulls the SDK's mix itself, one
//                  256-sample period at a time (the SDK's own fold and gain). Needs an open output device (see Begin).
//   the gate       the game's audio callback (sub_823F56D0, run by the SDK's audio worker for every credit) is wrapped in a MixerGate
//                  (hooks/audio_mixer.cpp). While a take runs it waits for a ticket before it mixes a frame, and the take hands tickets
//                  only for the video frames so far: so a sound an update asked for is in a frame mixed after that update.
//   a take         capture mode (cinematic/capture_mode.cpp) calls Begin when the take starts, StepBegin / StepEnd at the frame gate
//                  of every update, End when the take ends. The game thread is held at the gate while the frame is read back, so the
//                  mixer runs while the picture is read back and the sound the update asked for is the sound that is mixed.
//   the WAV        where the take folder says: Begin takes the path. 32 bit float, 48 kHz, the stereo fold the real output plays, or
//                  the six channels of the game's 5.1 (mcla_capture_audio_surround); exactly SamplesThrough(frames, fps) samples, so it
//                  is as long as the video to the sample. Next to it, the CSV (the take folder's audio.csv): a row per video frame.
//
// Nothing here does anything unless a take that records sound asks, and a take with mcla_capture_audio off, or with no output device
// open, goes on without sound capture: the real output keeps playing and nothing is held.
//

#include <cstddef>
#include <cstdint>
#include <string>

#include "capture_audio_core.h"  // Summary

namespace mc::cinematic::capture_audio {

// ---------------------------------------------------------------------------------------------------------------- the take (game thread)

// A take starts (capture_mode.cpp BeginTake, before the take's first update): with mcla_capture_audio on, the output device's feed is taken
// over (the device gets silence), the mixer's callbacks are gated, the frames queued before the take are thrown away and the WAV is opened at
// `wav_path` (its folder is made). With it off nothing is touched. `csv_path`: the per-frame CSV (the take folder's audio.csv); empty, it is
// `<wav name>.audio.csv` next to the WAV. `first_update`: the update of the take's first frame, for the CSV. False: no sound is captured for
// this take (mcla_capture_audio off, no audio system, no output device open, or the file cannot be made); the calls below then do nothing.
bool Begin(double fps, const std::string& wav_path, const std::string& csv_path, uint64_t first_update);
// The gate of an update (before the wait for the frame's readback): `video_frames` frames of the take exist. Hands the mixer the tickets
// that cover them and returns at once: the mixer runs while the picture is read back.
void StepBegin(uint64_t video_frames);
// The gate, after the readback: waits for the mixer's frames, writes the video frames' samples to the WAV, one row to the CSV.
void StepEnd();
// The take ends (capture_mode.cpp EndTake, after the last StepEnd): finishes the WAV and the CSV and gives the mixer back to the real
// output. `video_frames`: the frames the take has. Logs the summary line.
Summary End(uint64_t video_frames);
// The game is closing (any thread): the gate opens and the device gets its mix back, so that nothing waits for a take that will not go on.
// The files are closed by End as usual, if it is still called.
void Abort();
// The process is ending (a flush hook of the exit, larecomp_app.cpp; any thread): a take that is still running has its WAV closed with the real
// sizes of what was written (the take's own End never comes, and nothing closes the file once the exit is immediate: exit_fast.h; without this
// the WAV keeps the zero sizes of its first header). Does nothing when no WAV is open, and never touches the mixer.
void CloseForExit();

// A take's audio is running.
bool Active();
// The last take's summary (what End returned).
const Summary& LastSummary();

// ---------------------------------------------------------------------------------------------------------------- the mixer's side

// Around the game's audio callback, on the SDK's audio worker (hooks/audio_mixer.cpp). Outside a take it does nothing but count the call. While
// a take runs, the constructor waits for a ticket; the callback then mixes one frame, and the destructor says it is done.
class MixerGate {
  public:
    MixerGate();
    ~MixerGate();
    MixerGate(const MixerGate&) = delete;
    MixerGate& operator=(const MixerGate&) = delete;
};

}  // namespace mc::cinematic::capture_audio
