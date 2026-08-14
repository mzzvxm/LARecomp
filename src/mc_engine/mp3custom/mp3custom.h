#pragma once
//
// Custom MP3 radio for MCLA ("mp3custom").
//
// Drops the files found in
//
//     <exe>/music/*.mp3
//     %USERPROFILE%/Documents/Rockstar Games/LARecomp/User Music/*.mp3
//
// into mcMusicManager as real songs, so they show up in the radio menu (the
// ALL station and the Custom Playlists tab) and can be selected like any
// shipped track. Playback itself is done on the host -- see mp3_player.h for
// why the guest cue path cannot be used.
//

#include <cstdint>

// Called once from InitHooks(). Scans the music folders and brings the host
// player up; does nothing if the custom_music cvar is off or no files exist.
void InitCustomMusic();

// Per-frame, driven from Patch_DeltaTimePre() in hooks.cpp. Tracks the music
// volume slider and advances the station when a custom track runs out.
void TickCustomMusic();
