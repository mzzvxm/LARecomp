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

// The slot budget a bank has to fit its header in, and what that works out to.
constexpr uint32_t kMaxHeaderSize = 0x6800;

struct BankInfo {
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    uint32_t packets = 0;       // per wave
    uint32_t samples = 0;       // per wave
    uint32_t header_end = 0;    // what has to fit under kMaxHeaderSize
    uint32_t block_size = 0;
};

// Builds the bank for one MP3. `left_hash`/`right_hash` are the wave name
// hashes the sounds.dat leaves point at. Fails, with a reason, when the file is
// not MPEG-1 Layer III or when it is too long for the slot's header budget.
bool BuildMp3Bank(const std::vector<uint8_t>& mp3, uint32_t left_hash, uint32_t right_hash,
                  std::vector<uint8_t>& out, BankInfo& info, std::string& error);

// Seconds of audio that still fit under the header budget for a given bitrate,
// for the "your track is too long" message.
uint32_t Mp3SecondsThatFit(uint32_t bytes_per_second);

}  // namespace mc::music
