#pragma once

// The per-wheel matrix sets of mcCarModel, shared by tire_offset.cpp and
// tire_camber.cpp. See tire_offset.cpp for the reverse engineering notes.

#include <cmath>
#include <cstdint>
#include <cstring>

namespace wheel_matrices {

constexpr uint32_t kMatrixRow0 = 0;
constexpr uint32_t kMatrixRow3 = 48;

struct WheelBlock {
    uint32_t base;
    uint32_t stride;
};

// Every per-wheel matrix set sub_82354230 writes, in the order of the bitmask.
constexpr WheelBlock kBlocks[] = {
    {1088, 64},   // tyre — confirmed through sub_8235A628
    {1344, 64},
    {1600, 64},
    {1856, 128},
    {1920, 128},
};
constexpr int kNumBlocks = int(sizeof(kBlocks) / sizeof(kBlocks[0]));
constexpr int kNumWheels = 4;

inline uint32_t ReadBE32(const uint8_t* base, uint32_t ea) {
    const uint8_t* p = base + ea;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

inline float ReadBEFloat(const uint8_t* base, uint32_t ea) {
    const uint32_t bits = ReadBE32(base, ea);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

inline void WriteBEFloat(uint8_t* base, uint32_t ea, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    uint8_t* p = base + ea;
    p[0] = uint8_t(bits >> 24);
    p[1] = uint8_t(bits >> 16);
    p[2] = uint8_t(bits >> 8);
    p[3] = uint8_t(bits);
}

inline bool IsPlausibleGuestPtr(uint32_t ea) {
    return ea >= 0x82000000u && ea < 0xC0000000u;
}

struct Vec3 {
    float x, y, z;
};

inline Vec3 ReadVec3(const uint8_t* base, uint32_t ea) {
    return {ReadBEFloat(base, ea), ReadBEFloat(base, ea + 4),
            ReadBEFloat(base, ea + 8)};
}

inline bool Finite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

inline float Dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

}  // namespace wheel_matrices
