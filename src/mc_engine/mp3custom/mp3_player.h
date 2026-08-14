#pragma once
//
// Host-side MP3 playback for the custom radio (mp3custom).
//
// The guest never sees this audio. MCLA's own music path is a RAGE audio cue
// (mcMusicManager -> sub_821F4E78), which only knows about the banked streams
// in music.rpf; there is no way to hand it a file off the disk. So a custom
// track is played on the host: decoded with Media Foundation and pushed to
// XAudio2, while the midasm hook at 0x821EFB5C stops the guest from starting a
// cue of its own for that row.
//
// Windows-only for now. Everything degrades to "no custom tracks" elsewhere.
//

#include <cstdint>
#include <string>

namespace mp3custom {

// Brings up Media Foundation + XAudio2. Safe to call twice; returns false if
// either subsystem is unavailable, in which case every other call is a no-op.
bool PlayerInit();
void PlayerShutdown();

// Decodes `path` on a worker thread and starts playing it. Any track already
// playing is stopped first. Returns false only if the player is not up.
bool PlayerPlayFile(const std::wstring& path);

void PlayerStop();
void PlayerPause(bool paused);

// 0.0 .. 1.0, applied to the mastering voice.
void PlayerSetVolume(float volume);

// True between PlayerPlayFile() and either PlayerStop() or the end of the
// track. Decoding counts as playing, so a slow decode does not read as "ended".
bool PlayerIsPlaying();

// True once the submitted buffer has drained. Latches until the next
// PlayerPlayFile()/PlayerStop(), so the caller can poll it once per frame.
bool PlayerTrackEnded();

}  // namespace mp3custom
