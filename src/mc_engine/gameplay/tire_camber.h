#pragma once

#include <cmath>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────
// Tire camber (per-car wheel lean). Visual only; the garage row that edits the
// value lives in pause_menu.cpp.
// ─────────────────────────────────────────────────────────────────────────

// Camber0/1: two floats in degrees (front, rear) in the car's own config.
// Every stock .xcc carries -2.5 / -1.0 and no consumer of them was found in
// retail, so exactly those values mean "straight wheel"; anything else is the
// angle itself, negative leaning the top of the wheel inward.
constexpr float kStockCamber[2] = {-2.5f, -1.0f};

inline float CamberDegrees(float stored, int axle) {
    if (!std::isfinite(stored) || std::fabs(stored - kStockCamber[axle]) < 0.001f)
        return 0.0f;
    return stored < -45.0f ? -45.0f : (stored > 45.0f ? 45.0f : stored);
}

// Leans the wheels of one mcCarModel by its car's camber. Called from the wheel
// matrix hook in tire_offset.cpp. `refl` is CustomData + 0x40 and `targets` the
// mask of matrix blocks to rotate.
void ApplyTireCamber(uint8_t* base, uint32_t model, uint32_t vehicle,
                     uint32_t refl, uint32_t targets);
