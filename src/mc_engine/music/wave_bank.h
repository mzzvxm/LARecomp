// Writes the streaming wave bank the game serves at audio/x360/sfx/music/<name>.
//
// The format is documented in tools/wavebank.py, which round-trips all 108
// shipped banks. Two things are easy to get wrong and produce a bank that opens,
// streams, logs nothing and plays silence:
//
//   * the header's `blockSize` at +0x2C is not a block size, it is the offset of
//     chunk 0. Header plus chunk table must fit inside it, and chunk 0 must
//     start exactly there. Its ceiling is the wave slot's MaxHeaderSize from
//     config/waveslots.xml -- 0x6800 for the AMBIENCE_STREAM slots that carry
//     music -- which is what caps how long a track can be.
//   * `leadIn` in the chunk metadata makes the guest DROP the first packet of
//     the chunk. That exists for the XMA overlap; a passthrough packet decodes
//     standalone, so it has to stay 0.
//
// The payload is not XMA. There is no XMA2 encoder outside Microsoft's XDK, so
// the packets carry the source MP3's own frames behind an 'LPCM' magic and the
// runtime's XmaContext decodes them -- see rex::audio::XmaContext and
// project_mcla_passthrough_codec.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mc::music {

// What config/waveslots.xml ships as MaxHeaderSize for the AMBIENCE_STREAM
// slots that carry music, and what used to decide how long a track could be:
// the header costs about 8 bytes per 2036 bytes of MP3, so 26624 runs out
// around 5 minutes of 128 kbps and 2.5 of 320.
//
// Not to be confused with the slot's `Size` (288768 for those slots), which is
// this header budget plus the 2 x 0x20000 streaming window that follows it.
//
// It is not a limit of the engine, only a number in a file. The slot buffers
// are carved out of one pool sized by the Size values in that same xml
// (sub_82138AA0 sums them and allocates once), and MaxHeaderSize only caps how
// much of the bank file audWaveSlot::RequestLoad reads into the staging buffer
// (sub_82138410, `round2048(min(MaxHeaderSize, fileSize))`). A header past the
// cap is read short, which leaves the chunk table truncated and the track
// silent with nothing logged.
//
// So custom_music ships a patched waveslots.xml through the mod archive with a
// budget measured from the tracks actually present, and the only cost is guest
// heap on a pool that ships at 20 MB.
constexpr uint32_t kStockHeaderSize = 0x6800;

struct BankInfo {
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    uint32_t packets = 0;       // per wave
    uint32_t samples = 0;       // per wave
    uint32_t header_end = 0;    // what has to fit under the header budget
    uint32_t block_size = 0;
};

// Builds the bank for one MP3. `left_hash`/`right_hash` are the wave name
// hashes the sounds.dat leaves point at, and `header_budget` is the slot budget
// the caller is going to ship -- pass kStockHeaderSize to stay inside the
// shipped waveslots.xml. Fails, with a reason, when the file is not Layer III
// or when even the shipped budget cannot be raised far enough to hold it.
bool BuildMp3Bank(const std::vector<uint8_t>& mp3, uint32_t left_hash, uint32_t right_hash,
                  uint32_t header_budget, std::vector<uint8_t>& out, BankInfo& info,
                  std::string& error);

// Seconds of audio that fit under a given header budget at a given bitrate, for
// the "your track is too long" message.
uint32_t Mp3SecondsThatFit(uint32_t bytes_per_second, uint32_t header_budget);

}  // namespace mc::music
