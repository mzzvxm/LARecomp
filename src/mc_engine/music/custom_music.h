// Drops an MP3 in a folder and it is on the radio.
//
//   <exe>/music/*.mp3
//   <Documents>/Rockstar Games/LARecomp/User Music/*.mp3
//
// At startup each track gets a real game object, a real cue and a real streaming
// wave bank, exactly the way the shipped songs are put together -- the chain is
// in project_mcla_music_chain. The three files that carry it
//
//   audio/x360/config/game.dat      the radio entry plus MUSIC_0_MANAGER
//   audio/x360/config/sounds.dat    the cue and its two wave leaves
//   audio/x360/sfx/music/<bank>     the audio
//
// are rebuilt from the shipped pair and handed to the modloader, which packs
// them into xarchive_mods.rpf. Nothing is injected at runtime and no original
// file is touched.
//
// The banks are cached under <exe>/models/.custommusic, keyed by each source
// file's path, size and write time, so a boot that changes nothing rebuilds
// nothing.
//
// Names come from the file name read as "Artist - Title", the same convention
// the shipped songs follow. A manifest.txt beside the files overrides that:
//
//   genre = ROCK                       applies to every file in this folder
//
//   [Some Band - A Song.mp3]
//   genre   = HIPHOP
//   display = Some Band - A Song       what the radio shows
//   name    = HIPHOP_SOMEBAND_ASONG    the radio entry, rarely worth setting
//
// tools/addmusic.py does the same job offline and still reads manifest.json.
#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace mc::music {

struct GeneratedFile {
    std::string archive_path;            // e.g. audio/x360/sfx/music/artist_title
    std::filesystem::path source;        // where it was cached
};

// Scans the music folders and rebuilds whatever changed. Returns the files the
// mod archive should carry; empty when there is no music or nothing worked.
std::vector<GeneratedFile> Build(const std::filesystem::path& exe_dir,
                                 const std::filesystem::path& game_root);

// String-table key -> what the radio shows, valid after Build. A song entry's
// name is a KEY, and a key the shipped table does not carry renders as "!!Key!!".
const std::vector<std::pair<std::string, std::string>>& Titles();

// True once Build has put this file on the radio natively. The host MP3 player
// scans the same two folders, so without this every track would be listed twice
// -- once as a real game object and once injected into the manager's array.
bool Claimed(const std::filesystem::path& audio);

}  // namespace mc::music
