#include "mc_engine/music/wave_bank.h"

#include <algorithm>
#include <cstring>

namespace mc::music {
namespace {

constexpr uint32_t kPacket = 2048;
constexpr uint32_t kChunkSize = 0x20000;
constexpr uint32_t kChunkHeader = 0x800;
constexpr uint32_t kChunkCapacity = (kChunkSize - 2 * kChunkHeader) / kPacket;  // 62
constexpr uint32_t kMaxStartPackets = 0x212;  // the guest rejects anything above

// Passthrough packet header, shared with rex::audio::XmaContext.
constexpr uint32_t kPcmMagic = 0x4C50434Du;  // 'LPCM'
constexpr uint8_t kCodecMp3 = 2;
constexpr uint32_t kMp3PacketHeader = 12;
constexpr uint32_t kMp3SamplesPerFrame = 1152;

void StoreBE16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

void StoreBE32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

void PutBE32(std::vector<uint8_t>& out, size_t at, uint32_t v) { StoreBE32(out.data() + at, v); }

void AppendBE32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

struct Wave {
    uint32_t hash = 0;
    std::vector<uint8_t> packets;
    std::vector<uint32_t> first_sample;
    uint32_t sample_count = 0;
    uint32_t rate = 44100;

    uint32_t count() const { return static_cast<uint32_t>(first_sample.size()); }

    // Inclusive last sample the wave covers once `packet` has been decoded.
    uint32_t last_sample(uint32_t packet) const {
        const uint32_t next =
            packet + 1 < count() ? first_sample[packet + 1] : sample_count;
        return next - 1;
    }
};

struct Mp3Frame {
    uint32_t offset = 0;
    uint32_t length = 0;
};

// MPEG-1 Layer III only; anything else is refused up front rather than written
// into a bank the runtime cannot decode.
uint32_t Mp3FrameLength(const uint8_t* p, size_t available, uint32_t& rate, uint32_t& channels) {
    static const uint32_t kBitrate[16] = {0,   32,  40,  48,  56,  64,  80,  96,
                                          112, 128, 160, 192, 224, 256, 320, 0};
    static const uint32_t kRate[4] = {44100, 48000, 32000, 0};
    if (available < 4 || p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return 0;
    const uint32_t version = (p[1] >> 3) & 3;
    const uint32_t layer = (p[1] >> 1) & 3;
    const uint32_t bitrate_index = p[2] >> 4;
    const uint32_t rate_index = (p[2] >> 2) & 3;
    const uint32_t padding = (p[2] >> 1) & 1;
    if (version != 3 || layer != 1 || bitrate_index == 0 || bitrate_index == 15 ||
        rate_index == 3) {
        return 0;
    }
    rate = kRate[rate_index];
    channels = ((p[3] >> 6) == 3) ? 1 : 2;
    return (144 * kBitrate[bitrate_index] * 1000 / kRate[rate_index]) + padding;
}

std::vector<Mp3Frame> ScanMp3Frames(const std::vector<uint8_t>& blob, uint32_t& rate,
                                    uint32_t& channels) {
    size_t at = 0;
    if (blob.size() > 10 && blob[0] == 'I' && blob[1] == 'D' && blob[2] == '3') {
        at = 10 + ((blob[6] & 0x7Fu) << 21 | (blob[7] & 0x7Fu) << 14 | (blob[8] & 0x7Fu) << 7 |
                   (blob[9] & 0x7Fu));
        if (blob[5] & 0x10) at += 10;
    }
    std::vector<Mp3Frame> frames;
    while (at + 4 <= blob.size()) {
        uint32_t frame_rate = 0, frame_channels = 0;
        const uint32_t length =
            Mp3FrameLength(blob.data() + at, blob.size() - at, frame_rate, frame_channels);
        if (!length) {
            ++at;
            continue;
        }
        if (at + length > blob.size()) break;
        if (!rate) {
            rate = frame_rate;
            channels = frame_channels;
        }
        frames.push_back(Mp3Frame{static_cast<uint32_t>(at), length});
        at += length;
    }
    return frames;
}

// One wave: the whole MP3 bitstream sliced into packet payloads. A frame is
// allowed to straddle a packet boundary -- the runtime keeps a byte buffer and
// drains whole frames out of it -- because at 320 kbps a frame is 1044 bytes and
// refusing to split would waste half of every packet.
Wave BuildMp3Wave(const std::vector<uint8_t>& blob, const std::vector<Mp3Frame>& frames,
                  uint32_t hash, uint32_t rate, uint8_t channel) {
    Wave wave;
    wave.hash = hash;
    wave.rate = rate;

    const uint32_t base = frames.front().offset;
    const uint32_t body = frames.back().offset + frames.back().length - base;
    const uint32_t per = kPacket - kMp3PacketHeader;

    size_t next_frame = 0;
    for (uint32_t start = 0; start < body; start += per) {
        const uint32_t piece = std::min(per, body - start);
        // The table only has to locate a chunk, so a packet is credited with the
        // frames that BEGIN inside it.
        uint32_t begins = 0;
        while (next_frame < frames.size() && frames[next_frame].offset - base < start + piece) {
            ++begins;
            ++next_frame;
        }
        if (!begins) begins = 1;

        const size_t at = wave.packets.size();
        wave.packets.resize(at + kPacket, 0);
        uint8_t* packet = wave.packets.data() + at;
        StoreBE32(packet, kPcmMagic);
        StoreBE16(packet + 4, static_cast<uint16_t>(begins * kMp3SamplesPerFrame));
        packet[6] = kCodecMp3;
        packet[7] = channel;
        StoreBE16(packet + 8, static_cast<uint16_t>(piece));
        std::memcpy(packet + kMp3PacketHeader, blob.data() + base + start, piece);

        wave.first_sample.push_back(wave.sample_count);
        wave.sample_count += begins * kMp3SamplesPerFrame;
    }
    return wave;
}

struct ChunkPlan {
    // Signed, because before a wave has been handed its first packet its "end"
    // is one sample BEFORE the start. As unsigned that wraps to 0xFFFFFFFF, the
    // picker reads the wave as the furthest ahead instead of the furthest
    // behind, and the next chunk indexes packet -1.
    int64_t first_sample = 0;
    uint32_t start[2] = {0, 0};
    uint32_t count[2] = {0, 0};
    int64_t end[2] = {0, 0};
};

// Packets go to whichever wave is furthest behind, 62 to a chunk; the next chunk
// starts at the earliest sample both waves have reached.
std::vector<ChunkPlan> PlanChunks(const Wave (&waves)[2]) {
    std::vector<ChunkPlan> plan;
    uint32_t start[2] = {0, 0};
    int64_t chunk_first = 0;

    while (true) {
        ChunkPlan entry;
        entry.first_sample = chunk_first;
        uint32_t cur[2] = {start[0], start[1]};
        int64_t end[2];
        for (int i = 0; i < 2; ++i) {
            entry.start[i] = start[i];
            entry.count[i] = 0;
            end[i] = static_cast<int64_t>(waves[i].first_sample[start[i]]) - 1;
        }

        for (uint32_t k = 0; k < kChunkCapacity; ++k) {
            int pick = -1;
            for (int i = 0; i < 2; ++i) {
                if (cur[i] >= waves[i].count()) continue;
                if (pick < 0 || end[i] < end[pick]) pick = i;
            }
            if (pick < 0) break;
            end[pick] = waves[pick].last_sample(cur[pick]);
            ++cur[pick];
            ++entry.count[pick];
        }

        entry.end[0] = end[0];
        entry.end[1] = end[1];
        plan.push_back(entry);

        if (cur[0] >= waves[0].count() && cur[1] >= waves[1].count()) return plan;

        chunk_first = std::min(end[0], end[1]) + 1;
        for (int i = 0; i < 2; ++i) {
            if (cur[i] == 0) {
                start[i] = 0;
                continue;
            }
            const uint32_t previous = cur[i] - 1;
            start[i] = static_cast<int64_t>(waves[i].last_sample(previous)) >= chunk_first
                           ? previous
                           : cur[i];
        }
    }
}

std::vector<uint8_t> BuildBank(const Wave (&waves)[2], BankInfo& info) {
    const std::vector<ChunkPlan> plan = PlanChunks(waves);

    std::vector<uint8_t> desc[2];
    for (int i = 0; i < 2; ++i) {
        desc[i].assign(0x38 + 4ull * waves[i].count(), 0);
        uint8_t* d = desc[i].data();
        StoreBE32(d + 0x08, waves[i].hash);
        StoreBE32(d + 0x0C, static_cast<uint32_t>(waves[i].packets.size()));
        StoreBE32(d + 0x10, waves[i].sample_count);
        StoreBE32(d + 0x14, 0xFFFFFFFFu);
        StoreBE32(d + 0x18, (waves[i].rate << 16) | 0xFF41u);
        StoreBE32(d + 0x24, 0x38);
        StoreBE32(d + 0x34, waves[i].count());
        for (uint32_t k = 0; k < waves[i].count(); ++k) {
            StoreBE32(d + 0x38 + 4 * k, waves[i].first_sample[k]);
        }
    }

    const uint32_t data_start =
        0x50 + static_cast<uint32_t>(desc[0].size() + desc[1].size());
    const uint32_t head_end = data_start + 8 * static_cast<uint32_t>(plan.size());
    // `blockSize` is the offset of chunk 0, so it has to clear the header and
    // the chunk table. Shipped banks are all multiples of 0x800.
    const uint32_t block_size = ((head_end + 0x7FF) / 0x800) * 0x800;

    info.packets = waves[0].count();
    info.samples = waves[0].sample_count;
    info.sample_rate = waves[0].rate;
    info.header_end = head_end;
    info.block_size = block_size;
    if (block_size > kMaxHeaderSize) return {};

    std::vector<uint8_t> out(0x50, 0);
    out.insert(out.end(), desc[0].begin(), desc[0].end());
    out.insert(out.end(), desc[1].begin(), desc[1].end());

    PutBE32(out, 0x04, data_start);
    PutBE32(out, 0x08, static_cast<uint32_t>(plan.size()));
    PutBE32(out, 0x0C, kChunkSize);
    PutBE32(out, 0x18, 0x30);
    PutBE32(out, 0x20, data_start);
    PutBE32(out, 0x24, 2);
    PutBE32(out, 0x2C, block_size);
    // The last wave-table entry overlaps the first descriptor block's two
    // leading zero dwords; both sides write zeros there.
    PutBE32(out, 0x38, waves[0].hash);
    PutBE32(out, 0x3C, static_cast<uint32_t>(desc[0].size()));
    PutBE32(out, 0x44, static_cast<uint32_t>(desc[0].size()));
    PutBE32(out, 0x48, waves[1].hash);
    PutBE32(out, 0x4C, static_cast<uint32_t>(desc[1].size()));

    for (const ChunkPlan& entry : plan) {
        AppendBE32(out, static_cast<uint32_t>(entry.first_sample));
        AppendBE32(out, waves[0].rate);
    }
    out.resize(block_size, 0);

    for (const ChunkPlan& entry : plan) {
        std::vector<uint8_t> chunk(kChunkSize, 0);
        StoreBE32(chunk.data() + 0x04, 0x18);
        StoreBE32(chunk.data() + 0x0C, 0x38);
        StoreBE32(chunk.data() + 0x14, 0x38);

        uint32_t at = 0;
        for (int i = 0; i < 2; ++i) {
            const Wave& wave = waves[i];
            const uint32_t first = wave.first_sample[entry.start[i]];
            const uint32_t span = static_cast<uint32_t>(entry.end[i] + 1 - first);
            // leadIn stays 0: a passthrough packet decodes standalone, so the
            // guest must not drop the first packet of the chunk.
            uint8_t* meta = chunk.data() + 0x18 + 16 * i;
            StoreBE32(meta, at);
            StoreBE32(meta + 4, entry.count[i]);
            StoreBE32(meta + 8, 0);
            StoreBE32(meta + 12, span);

            for (uint32_t k = 0; k < entry.count[i]; ++k) {
                const uint32_t packet = entry.start[i] + k;
                uint8_t* pair = chunk.data() + 0x38 + 8 * (at + k);
                StoreBE32(pair, wave.first_sample[packet]);
                StoreBE32(pair + 4, static_cast<uint32_t>(std::min<int64_t>(
                                        wave.last_sample(packet), entry.end[i])));
                std::memcpy(chunk.data() + kChunkHeader + kPacket * (at + k),
                            wave.packets.data() + kPacket * packet, kPacket);
            }
            at += entry.count[i];
        }
        if (at > kMaxStartPackets) return {};
        out.insert(out.end(), chunk.begin(), chunk.end());
    }
    return out;
}

}  // namespace

uint32_t Mp3SecondsThatFit(uint32_t bytes_per_second) {
    if (!bytes_per_second) return 0;
    // Header cost per packet: 4 bytes in each wave's firstSample table plus
    // 8 bytes of chunk table per 62 packets, and both waves carry the stream.
    const double per_packet = 2.0 * 4.0 + 8.0 / kChunkCapacity * 2.0;
    const double packets = (kMaxHeaderSize - 0xC0) / per_packet;
    return static_cast<uint32_t>(packets * (kPacket - kMp3PacketHeader) / bytes_per_second);
}

bool BuildMp3Bank(const std::vector<uint8_t>& mp3, uint32_t left_hash, uint32_t right_hash,
                  std::vector<uint8_t>& out, BankInfo& info, std::string& error) {
    uint32_t rate = 0, channels = 0;
    const std::vector<Mp3Frame> frames = ScanMp3Frames(mp3, rate, channels);
    if (frames.empty() || !rate) {
        error = "no MPEG-1 Layer III frames (only plain MP3 is supported)";
        return false;
    }
    if (rate != 32000 && rate != 44100 && rate != 48000) {
        error = "sample rate " + std::to_string(rate) + " is not one the XMA context can carry";
        return false;
    }
    info.channels = channels;

    // Both waves hold the same stereo bitstream; the packet names the channel,
    // because a bank wave is mono and an MP3 does not split without re-encoding.
    const Wave waves[2] = {
        BuildMp3Wave(mp3, frames, left_hash, rate, 0),
        BuildMp3Wave(mp3, frames, right_hash, rate, channels > 1 ? 1 : 0),
    };

    out = BuildBank(waves, info);
    if (out.empty()) {
        const uint32_t seconds = static_cast<uint32_t>(
            static_cast<uint64_t>(frames.size()) * kMp3SamplesPerFrame / rate);
        const uint32_t fits = Mp3SecondsThatFit(
            static_cast<uint32_t>((frames.back().offset + frames.back().length -
                                   frames.front().offset) /
                                  std::max<uint32_t>(1, seconds)));
        error = "track is " + std::to_string(seconds) + " s but only about " +
                std::to_string(fits) +
                " s of it fit in the wave slot's header budget at this bitrate";
        return false;
    }
    return true;
}

}  // namespace mc::music
