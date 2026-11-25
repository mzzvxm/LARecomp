#ifndef REXGLUE_HAS_XEO3_TARGET
#include "tire_camber.h"
#include "wheel_matrices.h"

using namespace wheel_matrices;

namespace {

constexpr uint32_t kReflCamber      = 2032;  // f32, +0 front, +4 rear
// mcVehicle car matrix; row1 is up (sub_8235A628 uses it as the ground plane
// normal). Only its direction matters here.
constexpr uint32_t kVehicleUp       = 144 + 16;
// mcCarModel per-wheel pointers sub_82354230 tests before rebuilding a wheel.
// A wheel it skips keeps last frame's matrices, so rotating it would pile up.
constexpr uint32_t kModelWheelPtr   = 4192;

Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

// Rodrigues rotation of v about the unit axis k.
Vec3 Rotate(const Vec3& v, const Vec3& k, float c, float s) {
    const Vec3 kv = Cross(k, v);
    const float d = Dot(k, v) * (1.0f - c);
    return {v.x * c + kv.x * s + k.x * d, v.y * c + kv.y * s + k.y * d,
            v.z * c + kv.z * s + k.z * d};
}

void WriteVec3(uint8_t* base, uint32_t ea, const Vec3& v) {
    WriteBEFloat(base, ea + 0, v.x);
    WriteBEFloat(base, ea + 4, v.y);
    WriteBEFloat(base, ea + 8, v.z);
}

// Leans each wheel about the car's front-to-rear axis, pivoting on the tyre
// centre, so the wheel keeps spinning about its (now tilted) axle. Four-wheel
// cars only: wheels 0,1 are the front pair and 2,3 the rear pair.
void ApplyCamber(uint8_t* base, uint32_t model, uint32_t vehicle,
                 const float (&degrees)[2], uint32_t targets) {
    Vec3 pivot[kNumWheels];
    for (int i = 0; i < kNumWheels; ++i) {
        if (!IsPlausibleGuestPtr(ReadBE32(base, model + kModelWheelPtr + 4u * i)))
            return;
        pivot[i] = ReadVec3(base, model + kBlocks[0].base +
                                      kBlocks[0].stride * uint32_t(i) + kMatrixRow3);
        if (!Finite(pivot[i])) return;
    }

    Vec3 fwd{pivot[0].x + pivot[1].x - pivot[2].x - pivot[3].x,
             pivot[0].y + pivot[1].y - pivot[2].y - pivot[3].y,
             pivot[0].z + pivot[1].z - pivot[2].z - pivot[3].z};
    const float len = std::sqrt(Dot(fwd, fwd));
    if (!(len > 1e-3f)) return;
    fwd = {fwd.x / len, fwd.y / len, fwd.z / len};

    const Vec3 up = ReadVec3(base, vehicle + kVehicleUp);
    if (!Finite(up)) return;

    Vec3 centre{0.0f, 0.0f, 0.0f};
    for (const Vec3& p : pivot) {
        centre.x += p.x * 0.25f;
        centre.y += p.y * 0.25f;
        centre.z += p.z * 0.25f;
    }

    for (int i = 0; i < kNumWheels; ++i) {
        const float deg = degrees[i < 2 ? 0 : 1];
        if (deg == 0.0f) continue;

        // Whether a positive turn moves the top of this wheel outward. That
        // gives the sign without knowing which side the wheel is on.
        const Vec3 outward{pivot[i].x - centre.x, pivot[i].y - centre.y,
                           pivot[i].z - centre.z};
        const float side = Dot(Cross(fwd, up), outward) >= 0.0f ? 1.0f : -1.0f;
        const float angle = deg * side * 0.017453292f;
        const float c = std::cos(angle);
        const float s = std::sin(angle);

        for (int b = 0; b < kNumBlocks; ++b) {
            if (!(targets & (1u << b))) continue;
            const uint32_t m =
                model + kBlocks[b].base + kBlocks[b].stride * uint32_t(i);

            for (uint32_t row = 0; row < 3; ++row) {
                const Vec3 v = ReadVec3(base, m + row * 16);
                if (Finite(v)) WriteVec3(base, m + row * 16, Rotate(v, fwd, c, s));
            }

            const Vec3 pos = ReadVec3(base, m + kMatrixRow3);
            if (!Finite(pos)) continue;
            const Vec3 rel = Rotate({pos.x - pivot[i].x, pos.y - pivot[i].y,
                                     pos.z - pivot[i].z}, fwd, c, s);
            WriteVec3(base, m + kMatrixRow3,
                      {pivot[i].x + rel.x, pivot[i].y + rel.y, pivot[i].z + rel.z});
        }
    }
}

}  // namespace

void ApplyTireCamber(uint8_t* base, uint32_t model, uint32_t vehicle,
                     uint32_t refl, uint32_t targets) {
    const float degrees[2] = {
        CamberDegrees(ReadBEFloat(base, refl + kReflCamber + 0), 0),
        CamberDegrees(ReadBEFloat(base, refl + kReflCamber + 4), 1)};
    if (degrees[0] == 0.0f && degrees[1] == 0.0f) return;  // stock car
    ApplyCamber(base, model, vehicle, degrees, targets);
}

#endif  // REXGLUE_HAS_XEO3_TARGET
