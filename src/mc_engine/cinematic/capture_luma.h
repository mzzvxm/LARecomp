#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "capture_core.h"

namespace mc::cinematic::capture {

inline constexpr char kLumaName[] = "video.luma";
inline constexpr std::string_view kLumaMagic = "RTLUMA1\n";
inline constexpr uint32_t kLumaKeyInterval = 60;

struct LumaCrop {
    uint32_t x = 0, y = 0, width = 0, height = 0;

    bool Fits(uint32_t frame_width, uint32_t frame_height) const {
        return width && height && x <= frame_width && y <= frame_height &&
               width <= frame_width - x && height <= frame_height - y;
    }

    size_t Pixels() const { return size_t(width) * height; }
};

// "full" leaves the size for the first readback to fill in.
inline bool ParseLumaCrop(std::string_view text, LumaCrop& crop) {
    if (text == "full") {
        crop = {};
        return true;
    }
    if (text.empty()) return false;
    LumaCrop parsed;
    for (uint32_t* field : {&parsed.x, &parsed.y, &parsed.width, &parsed.height}) {
        const size_t comma = text.find(',');
        const char* end = text.data() + (comma == text.npos ? text.size() : comma);
        const auto result = std::from_chars(text.data(), end, *field);
        if (result.ec != std::errc{} || result.ptr != end || (comma == text.npos) != (field == &parsed.height)) return false;
        if (comma != text.npos) text.remove_prefix(comma + 1);
    }
    if (!parsed.width || !parsed.height) return false;
    crop = parsed;
    return true;
}

inline std::array<uint8_t, 4> LumaWord(uint32_t value) {
    return {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
}

inline bool LumaPreview(uint64_t frame) { return frame == 0 || frame == 120 || frame == 239; }

inline bool LumaLastFrame(const TakeSceneClock& clock, uint64_t update, uint64_t first, double end, double duration) {
    return update >= first && clock.PastEnd(update - first + 1, end, duration);
}

inline uint8_t CaptureLuma(uint8_t r, uint8_t g, uint8_t b) {
    // Pillow's RGB to L conversion, including its fixed-point rounding.
    return uint8_t((19595u * r + 38470u * g + 7471u * b + 32768u) >> 16);
}

template <bool Bgra, bool Key>
inline void PredictLuma(const uint8_t* rgba, uint32_t frame_width, LumaCrop crop, uint8_t* previous, uint8_t* delta) {
    for (uint32_t y = 0; y < crop.height; ++y) {
        const uint8_t* row = rgba + (size_t(y + crop.y) * frame_width + crop.x) * 4;
        uint8_t left = 0;
        for (uint32_t x = 0; x < crop.width; ++x) {
            const uint8_t luma = CaptureLuma(row[4 * x + (Bgra ? 2 : 0)], row[4 * x + 1], row[4 * x + (Bgra ? 0 : 2)]);
            *delta++ = uint8_t(luma - (Key ? left : *previous));
            *previous++ = luma;
            left = luma;
        }
    }
}

}  // namespace mc::cinematic::capture
