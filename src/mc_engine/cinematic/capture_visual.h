#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "capture_luma.h"

namespace mc::cinematic::capture::visual {

enum Buffer : uint8_t { kFinal, kHdr, kAo, kRefl, kGi, kClass, kMotion, kReflHit, kCount };
constexpr uint32_t Bit(Buffer buffer) { return 1u << buffer; }

struct Stream {
    const char* name;
    const char* encoding;
    const char* space;
    uint32_t channels;
    uint32_t channel_bytes;
    uint32_t max_source_bytes;
};
inline constexpr std::array<Stream, kCount> kStreams = {{
    {"final", "u8", "display-srgb", 3, 1, 4},
    {"hdr", "f16le", "linear-scene", 3, 2, 8},
    {"ao", "f16le", "linear-scene", 1, 2, 8},
    {"refl", "f16le", "linear-scene", 3, 2, 8},
    {"gi", "f16le", "linear-scene", 3, 2, 8},
    {"class", "u8", "class-id", 1, 1, 1},
    {"motion", "f16le", "motion-px", 2, 2, 4},
    {"reflhit", "u8", "class-id", 2, 1, 4},
}};

bool ParseBuffers(std::string_view text, uint32_t& mask);
inline bool ParseRects(std::string_view text, LumaCrop& bounds) {
    if (text.empty()) return false;
    uint32_t x = UINT32_MAX, y = UINT32_MAX, right = 0, bottom = 0;
    for (;;) {
        const size_t separator = text.find(';');
        LumaCrop crop;
        if (!ParseLumaCrop(text.substr(0, separator), crop) ||
            !crop.Fits(UINT32_MAX, UINT32_MAX)) return false;
        x = std::min(x, crop.x);
        y = std::min(y, crop.y);
        right = std::max(right, crop.x + crop.width);
        bottom = std::max(bottom, crop.y + crop.height);
        if (separator == text.npos) break;
        text.remove_prefix(separator + 1);
    }
    bounds = {x, y, right - x, bottom - y};
    return true;
}
LumaCrop ScaleCrop(LumaCrop crop, uint32_t from_w, uint32_t from_h,
                   uint32_t to_w, uint32_t to_h);
constexpr bool Selected(uint64_t update, uint64_t first, uint32_t every) {
    return update >= first && every && (update - first) % every == 0;
}
constexpr uint64_t Align(uint64_t size, uint32_t alignment) {
    return (size + alignment - 1) / alignment * alignment;
}

struct Plane {
    LumaCrop crop;
    uint32_t width = 0, height = 0;  // uncropped source
    uint32_t row_pitch = 0, pixel_bytes = 0, channel_offset = 0;
    uint64_t offset = 0;            // from the start of the packed readback
    bool swap_rb = false;
    bool alternate = false;       // an alternate renderer target with different signal semantics
};
struct Frame {
    uint64_t index = 0;
    std::array<Plane, kCount> planes{};
};
Plane Place(LumaCrop crop, uint32_t width, uint32_t height, uint32_t pixel_bytes,
            uint32_t channel_offset, bool swap_rb, uint64_t& end);

class Writer {
public:
    void Reset(const std::filesystem::path& take);
    bool Write(const Frame& frame, std::span<const uint8_t> data, std::string& why);
    bool Finish(std::string& why);

private:
    struct Written {
        Plane plane;
        std::vector<uint64_t> frames;
    };
    std::filesystem::path root_;
    std::array<Written, kCount> streams_;
    std::vector<uint8_t> row_;
};

}  // namespace mc::cinematic::capture::visual
