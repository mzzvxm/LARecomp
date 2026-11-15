//
// The pure half of a take's sound (trailer toolkit M2c). See capture_audio_core.h.
//

#include "capture_audio_core.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mc::cinematic::capture_audio {

// ---------------------------------------------------------------------------------------------------------------- the timeline

uint64_t SamplesThrough(uint64_t video_frames, double fps) {
    if (!(fps > 0.0)) return 0;  // also NaN
    // long double: n * 48000 reaches 1e11 for a day of video, and the quotient has to be right to well under a sample
    const long double exact = static_cast<long double>(video_frames) * static_cast<long double>(kRate) / static_cast<long double>(fps);
    return static_cast<uint64_t>(std::floor(exact + 0.5L));
}

uint64_t MixerFramesCovering(uint64_t samples) {
    return (samples + kMixerFrame - 1) / kMixerFrame;
}

// ---------------------------------------------------------------------------------------------------------------- the books

void Book::Begin(double fps) {
    *this = Book{};
    fps_ = fps > 0.0 ? fps : 60.0;
}

uint64_t Book::Wanted(uint64_t video_frames) const {
    const uint64_t need = MixerFramesCovering(Target(video_frames));
    // what the timeline holds or is owed already: counted frames, and the released ones on their way
    return need > released_ ? need - released_ : 0;
}

Book::Pop Book::Consume(uint64_t queued) {
    Pop p;
    p.discard = std::min(skip_, queued);
    skip_ -= p.discard;
    queued -= p.discard;
    p.use = std::min(queued, Outstanding());
    counted_ += p.use;
    return p;
}

uint64_t Book::GiveUp() {
    const uint64_t missing = Outstanding();
    counted_ += missing;
    silent_ += missing;
    skip_ += missing;
    return missing;
}

uint64_t Book::Writable(uint64_t video_frames) const {
    const uint64_t target = Target(video_frames);
    if (target <= written_) return 0;
    return std::min(target - written_, Available());
}

uint64_t Book::Shortfall(uint64_t video_frames) const {
    const uint64_t target = Target(video_frames);
    const uint64_t have = written_ + Available();
    return target > have ? target - have : 0;
}

// ---------------------------------------------------------------------------------------------------------------- the WAV file

namespace {

void Put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
void Put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
}
void PutTag(std::vector<uint8_t>& v, const char* tag) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(tag[i]));
}

// The speaker mask of the game's order: front left, front right, front centre, LFE, back left, back right (SPEAKER_* bits 0..5).
uint32_t ChannelMask(uint32_t channels) {
    switch (channels) {
        case 3: return 0x7;
        case 4: return 0x33;   // fl fr bl br
        case 5: return 0x37;
        case 6: return 0x3F;
        default: return 0;
    }
}

constexpr uint16_t kFormatFloat = 3;
constexpr uint16_t kFormatExtensible = 0xFFFE;

}  // namespace

size_t WavHeaderSize(uint32_t channels) {
    // RIFF chunk header (12) + fmt chunk (8 + 16 or 8 + 40) + fact chunk (8 + 4) + data chunk header (8)
    return channels <= 2 ? 12 + 24 + 12 + 8 : 12 + 48 + 12 + 8;
}

std::vector<uint8_t> WavHeader(uint32_t channels, uint32_t rate, uint64_t sample_frames) {
    channels = std::max<uint32_t>(channels, 1);
    const uint64_t data64 = sample_frames * channels * 4ull;
    const uint32_t data_bytes = static_cast<uint32_t>(std::min<uint64_t>(data64, 0xFFFFFFFFull));
    const size_t header = WavHeaderSize(channels);
    const uint64_t riff64 = (header - 8) + data64;
    std::vector<uint8_t> v;
    v.reserve(header);
    PutTag(v, "RIFF");
    Put32(v, static_cast<uint32_t>(std::min<uint64_t>(riff64, 0xFFFFFFFFull)));
    PutTag(v, "WAVE");
    PutTag(v, "fmt ");
    const bool extensible = channels > 2;
    Put32(v, extensible ? 40 : 16);
    Put16(v, extensible ? kFormatExtensible : kFormatFloat);
    Put16(v, static_cast<uint16_t>(channels));
    Put32(v, rate);
    Put32(v, rate * channels * 4u);
    Put16(v, static_cast<uint16_t>(channels * 4));
    Put16(v, 32);
    if (extensible) {
        Put16(v, 22);   // cbSize
        Put16(v, 32);   // valid bits
        Put32(v, ChannelMask(channels));
        // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT {00000003-0000-0010-8000-00AA00389B71}
        Put32(v, 0x00000003);
        Put16(v, 0x0000);
        Put16(v, 0x0010);
        const uint8_t tail[8] = {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        v.insert(v.end(), tail, tail + 8);
    }
    PutTag(v, "fact");
    Put32(v, 4);
    Put32(v, static_cast<uint32_t>(std::min<uint64_t>(sample_frames, 0xFFFFFFFFull)));
    PutTag(v, "data");
    Put32(v, data_bytes);
    return v;
}

bool WavFile::Open(const std::string& path, uint32_t channels, uint32_t rate) {
    Close();
    channels_ = std::max<uint32_t>(channels, 1);
    rate_ = rate;
    frames_ = 0;
    failed_ = false;
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) {
        failed_ = true;
        return false;
    }
    const std::vector<uint8_t> h = WavHeader(channels_, rate_, 0);
    if (std::fwrite(h.data(), 1, h.size(), file_) != h.size()) {
        failed_ = true;
        std::fclose(file_);
        file_ = nullptr;
        return false;
    }
    return true;
}

bool WavFile::Append(const float* interleaved, uint64_t sample_frames) {
    if (!file_ || failed_) return false;
    if (sample_frames == 0) return true;
    const uint64_t bytes = sample_frames * channels_ * 4ull;
    if ((frames_ * channels_ * 4ull) + bytes > kWavMaxDataBytes) {
        failed_ = true;  // a WAV cannot describe more; RF64 is not written
        return false;
    }
    if (std::fwrite(interleaved, sizeof(float), static_cast<size_t>(sample_frames * channels_), file_) != sample_frames * channels_) {
        failed_ = true;
        return false;
    }
    frames_ += sample_frames;
    return true;
}

bool WavFile::Close() {
    if (!file_) return !failed_;
    bool ok = !failed_;
    const std::vector<uint8_t> h = WavHeader(channels_, rate_, frames_);
    if (std::fflush(file_) != 0 || std::fseek(file_, 0, SEEK_SET) != 0 || std::fwrite(h.data(), 1, h.size(), file_) != h.size()) ok = false;
    if (std::fclose(file_) != 0) ok = false;
    file_ = nullptr;
    if (!ok) failed_ = true;
    return ok;
}

}  // namespace mc::cinematic::capture_audio
