#include "lzx_encode.h"

#include <algorithm>

namespace mc::modloader {

namespace {

constexpr size_t kChunkOutput = 32768;
constexpr uint32_t kBlockTypeStored = 3;

// Packs bits MSB first into 16-bit little-endian words.
class BitWriter {
 public:
    void Put(uint32_t value, int bits) {
        for (int i = bits - 1; i >= 0; --i) {
            buffer_ = (buffer_ << 1) | ((value >> i) & 1u);
            if (++count_ == 16) {
                out_.push_back(static_cast<uint8_t>(buffer_ & 0xFF));
                out_.push_back(static_cast<uint8_t>((buffer_ >> 8) & 0xFF));
                buffer_ = 0;
                count_ = 0;
            }
        }
    }

    void AlignToWord() {
        while (count_ != 0) Put(0, 1);
    }

    const std::vector<uint8_t>& bytes() const { return out_; }

 private:
    std::vector<uint8_t> out_;
    uint32_t buffer_ = 0;
    int count_ = 0;
};

void PushLE32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 24));
}

}  // namespace

std::vector<uint8_t> LzxEncodeStored(const uint8_t* data, size_t size) {
    std::vector<uint8_t> stream;
    stream.reserve(size + size / 1024 + 64);

    for (size_t offset = 0; offset < size; offset += kChunkOutput) {
        const size_t block_size = std::min(kChunkOutput, size - offset);

        BitWriter header;
        if (offset == 0) header.Put(0, 1);  // Intel E8 translation, first chunk only
        header.Put(kBlockTypeStored, 3);
        header.Put(static_cast<uint32_t>(block_size), 24);
        header.AlignToWord();

        std::vector<uint8_t> chunk = header.bytes();
        chunk.reserve(chunk.size() + 12 + block_size + 1);
        PushLE32(chunk, 1);  // R0
        PushLE32(chunk, 1);  // R1
        PushLE32(chunk, 1);  // R2
        chunk.insert(chunk.end(), data + offset, data + offset + block_size);
        if (chunk.size() & 1) chunk.push_back(0);

        // Chunk length is a 16-bit field; a stored 32 KB block plus its header
        // is comfortably inside it.
        stream.push_back(static_cast<uint8_t>(chunk.size() >> 8));
        stream.push_back(static_cast<uint8_t>(chunk.size() & 0xFF));
        stream.insert(stream.end(), chunk.begin(), chunk.end());
    }

    return stream;
}

}  // namespace mc::modloader
