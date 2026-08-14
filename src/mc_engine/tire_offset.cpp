#ifndef REXGLUE_HAS_XEO3_TARGET
#include "tire_offset.h"

#include "logging.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/runtime.h>

// ─────────────────────────────────────────────────────────────────────────
// Tire offset — the consumer for a field retail never reads.
//
// Reverse engineering (default.xex):
//
//   sub_82394388          customization reflection. TireOffset0 / TireOffset1
//                         are u8 at reflBase+2040/+2041 (reflBase = CustomData
//                         + 0x40), one per axle, immediately below RideHeight
//                         at +2042. Parsed, saved — and read by nothing. The
//                         ctor sub_82395138 zeroes the dword at 0x82395550 and
//                         hands defaults only to RideHeight and TireWidth.
//
//   sub_82354230          mcCarModel per-wheel matrix build, one pass over the
//                         four wheels. r31 = mcCarModel. Every wheel transform
//                         the renderer uses is (re)derived here each frame from
//                         the wheel/hub bone matrices (pointers at model+4192 /
//                         model+4224) times the rim/tire dimensions, so this is
//                         the last point where a shift stays purely visual and
//                         cannot accumulate: nothing here is read back into the
//                         source it was built from.
//
//   sub_8235A628          the tyre draw loop, and the proof of which block is
//                         which. At 0x8235A7B4 it loads four rows relative to
//                         `model + 1120 + 64*wheel` (-32, -16, 0, +16) into one
//                         Matrix44 and hands it to sub_8217C088 (set world) and
//                         sub_82323250 (copy into the drawable). That is the
//                         block at model+1088+64*wheel, rows at +0/16/32/48.
//
// The other blocks sub_82354230 writes (model+1344 / +1600 with stride 64,
// model+1856 / +1920 with stride 128) belong to the rest of the wheel — rim,
// brake, blur — and which is which was not traced. `tire_offset_targets` is a
// bitmask over all five so the split can be found in one run instead of one
// build per guess; bit 0 (the tyre) is the one confirmed above.
//
// Direction is taken from the matrices themselves, not from a wheel-side table:
// row0 is the axle axis, and its sign against (wheel - centre of the four
// wheels) says whether it points outward. Mirrored wheels therefore both move
// out on a positive value without a per-side sign to get wrong.
//
// Physics is untouched: contact patches, suspension and collision keep running
// on the car the game built. Past a certain value the tyre simply intersects
// the arch (outward) or the body (inward) — geometry is the limit, and there is
// no clearance test to consult because the shipped one (sub_82392F68) only ever
// looked at radius and ride height.
// ─────────────────────────────────────────────────────────────────────────

REXCVAR_DEFINE_DOUBLE(tire_offset_step, 0.012, "MCLA/Patches",
    "Metres of lateral wheel movement per TIRE OFFSET notch (17 notches, "
    "-8..+8, so the default spans about +/-10 cm). 0 disables the effect while "
    "leaving the garage row usable.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(tire_offset_targets, 31, "MCLA/Patches",
    "Bitmask (decimal) of the per-wheel matrix blocks TIRE OFFSET moves: 1 = "
    "model+1088 (the tyre), 2 = +1344, 4 = +1600, 8 = +1856, 16 = +1920. "
    "31 moves all five; drop bits to find which block owns which part.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

constexpr uint32_t kModelVehicle    = 20;    // mcCarModel -> mcVehicle
constexpr uint32_t kVehicleCustom   = 132;   // mcVehicle -> CustomData
constexpr uint32_t kReflFromCustom  = 64;    // reflection base = CustomData+0x40
constexpr uint32_t kReflTireOffset  = 2040;  // +0 front, +1 rear

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

uint8_t* GetMembase() {
    auto* rt = rex::Runtime::instance();
    return rt ? rt->virtual_membase() : nullptr;
}

uint32_t ReadBE32(const uint8_t* base, uint32_t ea) {
    const uint8_t* p = base + ea;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

float ReadBEFloat(const uint8_t* base, uint32_t ea) {
    const uint32_t bits = ReadBE32(base, ea);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

void WriteBEFloat(uint8_t* base, uint32_t ea, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    uint8_t* p = base + ea;
    p[0] = uint8_t(bits >> 24);
    p[1] = uint8_t(bits >> 16);
    p[2] = uint8_t(bits >> 8);
    p[3] = uint8_t(bits);
}

bool IsPlausibleGuestPtr(uint32_t ea) {
    return ea >= 0x82000000u && ea < 0xC0000000u;
}

struct Vec3 {
    float x, y, z;
};

Vec3 ReadVec3(const uint8_t* base, uint32_t ea) {
    return {ReadBEFloat(base, ea), ReadBEFloat(base, ea + 4),
            ReadBEFloat(base, ea + 8)};
}

bool Finite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

float Dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// Shifts one block of four wheel matrices. front/rear are the raw notch values.
void ShiftBlock(uint8_t* base, uint32_t model, const WheelBlock& block,
                int front, int rear, float step) {
    uint32_t row0_ea[kNumWheels];
    uint32_t row3_ea[kNumWheels];
    Vec3 axis[kNumWheels];
    Vec3 pos[kNumWheels];
    bool valid[kNumWheels];

    Vec3 centre{0.0f, 0.0f, 0.0f};
    int count = 0;

    for (int i = 0; i < kNumWheels; ++i) {
        const uint32_t m = model + block.base + block.stride * uint32_t(i);
        row0_ea[i] = m + kMatrixRow0;
        row3_ea[i] = m + kMatrixRow3;
        axis[i] = ReadVec3(base, row0_ea[i]);
        pos[i] = ReadVec3(base, row3_ea[i]);

        // A bike leaves the unused wheel slots untouched, so only count the
        // ones that hold a usable transform.
        valid[i] = Finite(axis[i]) && Finite(pos[i]) && Dot(axis[i], axis[i]) > 1e-12f;
        if (valid[i]) {
            centre.x += pos[i].x;
            centre.y += pos[i].y;
            centre.z += pos[i].z;
            ++count;
        }
    }

    if (count < 2) return;
    centre.x /= float(count);
    centre.y /= float(count);
    centre.z /= float(count);

    for (int i = 0; i < kNumWheels; ++i) {
        if (!valid[i]) continue;

        const int notches = (i < 2) ? front : rear;
        if (!notches) continue;

        const float len = std::sqrt(Dot(axis[i], axis[i]));
        if (!(len > 1e-6f)) continue;

        const Vec3 outward{pos[i].x - centre.x, pos[i].y - centre.y,
                           pos[i].z - centre.z};
        const float sign = (Dot(axis[i], outward) >= 0.0f) ? 1.0f : -1.0f;
        const float amount = float(notches) * step * sign / len;

        WriteBEFloat(base, row3_ea[i] + 0, pos[i].x + axis[i].x * amount);
        WriteBEFloat(base, row3_ea[i] + 4, pos[i].y + axis[i].y * amount);
        WriteBEFloat(base, row3_ea[i] + 8, pos[i].z + axis[i].z * amount);
    }
}

}  // namespace

void Hook_TireOffsetWheels(PPCRegister& r31) {
    uint8_t* base = GetMembase();
    if (!base) return;

    const uint32_t model = uint32_t(r31.u64);
    if (!IsPlausibleGuestPtr(model)) return;

    const uint32_t vehicle = ReadBE32(base, model + kModelVehicle);
    if (!IsPlausibleGuestPtr(vehicle)) return;

    const uint32_t custom = ReadBE32(base, vehicle + kVehicleCustom);
    if (!IsPlausibleGuestPtr(custom)) return;

    const uint32_t refl = custom + kReflFromCustom;
    const int front = int(int8_t(base[refl + kReflTireOffset + 0]));
    const int rear = int(int8_t(base[refl + kReflTireOffset + 1]));
    if (!front && !rear) return;  // stock car: nothing to do, no cost

    const float step = float(REXCVAR_GET(tire_offset_step));
    if (!(step > 0.0f)) return;

    const uint32_t targets = REXCVAR_GET(tire_offset_targets);
    for (int b = 0; b < kNumBlocks; ++b) {
        if (targets & (1u << b))
            ShiftBlock(base, model, kBlocks[b], front, rear, step);
    }
}

#else  // REXGLUE_HAS_XEO3_TARGET

#include "tire_offset.h"

void Hook_TireOffsetWheels(PPCRegister& r31) {}

#endif  // REXGLUE_HAS_XEO3_TARGET
