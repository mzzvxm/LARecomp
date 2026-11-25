#ifndef REXGLUE_HAS_XEO3_TARGET
// Fixes for car meshes the game ships broken, applied in memory the first time
// the part is drawn. The game's files are not touched.
//
//   Impala 64, rear bumper 2 (bumper_r2, both LODs): modelled about 2.7 cm
//   lower than bumper_r0 and r1, which leaves a gap under the body. Its box is
//   y -0.1343..+0.0811 where theirs is -0.1074..+0.1074; x matches. The fix
//   raises its vertices. The part is recognised by its first bone, "bumr02",
//   and that box: of the 90 vehicle parts in the game with that bone, only the
//   two files of this bumper have it. Once raised it no longer matches.
//
// The vertices are changed rather than a draw matrix because the part is drawn
// through several paths with matrices from different places.
//
// A part is its own rmcDrawable:
//   +0x08  three LOD pointers          LOD    +0 model array, +4 u16 count
//   +0x24  skeleton: +0 bone array, +0x14 u16 count; bone +0 name
//   model  +4 geometry array, +8 u16 count
//   geometry +12 vertex buffer, +52 u16 vertex count
//   vertex buffer +12 stride, +24 data; position is three floats at offset 0
//
//   sub_823252B8, sub_82324258   the two draws every car part goes through,
//                                both with r3 = rmcDrawable.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "settings/larecomp_settings.h"
#include <rex/ppc/recompiled_code.h>
#include <rex/ppc/context.h>
#include "larecomp_guest.h"
#include <rex/kernel/title_runtime.h>

#include "larecomp_log.h"

REX_EXTERN(__imp__rex_sub_823252B8);  // draw a drawable's LOD
REX_EXTERN(__imp__rex_sub_82324258);  // draw a drawable's LOD, skinned path

namespace {

constexpr float kRaise = 0.0266f;
constexpr uint32_t kMaxModels = 32;
constexpr uint32_t kMaxGeometries = 64;

uint8_t* Host(uint32_t ea) {
    return larecomp::guest::AddressSpace()->TranslateVirtual<uint8_t*>(ea);
}

bool Plausible(uint32_t ea) { return ea >= 0x10000u && (ea & 3u) == 0; }

uint32_t Load32(uint32_t ea) {
    const uint8_t* p = Host(ea);
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint32_t Load16(uint32_t ea) {
    const uint8_t* p = Host(ea);
    return (uint32_t(p[0]) << 8) | uint32_t(p[1]);
}

float LoadFloat(uint32_t ea) {
    const uint32_t bits = Load32(ea);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

void StoreFloat(uint32_t ea, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    uint8_t* p = Host(ea);
    p[0] = uint8_t(bits >> 24);
    p[1] = uint8_t(bits >> 16);
    p[2] = uint8_t(bits >> 8);
    p[3] = uint8_t(bits);
}

bool FirstBoneIs(uint32_t drawable, const char* name) {
    const uint32_t skeleton = Load32(drawable + 0x24);
    if (!Plausible(skeleton)) return false;
    const uint32_t bones = Load32(skeleton);
    if (!Plausible(bones) || Load16(skeleton + 0x14) == 0) return false;
    const uint32_t text = Load32(bones);
    if (text < 0x10000u) return false;
    return std::strncmp(reinterpret_cast<const char*>(Host(text)), name,
                        std::strlen(name) + 1) == 0;
}

struct Buffer {
    uint32_t data, stride, count;
};

// Every vertex buffer of the drawable once, whatever LOD slot names it.
std::vector<Buffer> VertexBuffers(uint32_t drawable) {
    std::vector<Buffer> out;
    for (uint32_t slot = 0; slot < 3; ++slot) {
        const uint32_t lod = Load32(drawable + 8 + slot * 4);
        if (!Plausible(lod)) continue;
        const uint32_t models = Load32(lod);
        const uint32_t model_count = Load16(lod + 4);
        if (!Plausible(models) || model_count > kMaxModels) return {};
        for (uint32_t m = 0; m < model_count; ++m) {
            const uint32_t model = Load32(models + m * 4);
            if (!Plausible(model)) continue;
            const uint32_t list = Load32(model + 4);
            const uint32_t geometry_count = Load16(model + 8);
            if (!Plausible(list) || geometry_count > kMaxGeometries) return {};
            for (uint32_t g = 0; g < geometry_count; ++g) {
                const uint32_t geometry = Load32(list + g * 4);
                if (!Plausible(geometry)) continue;
                const uint32_t vb = Load32(geometry + 12);
                if (!Plausible(vb)) continue;
                const Buffer buffer{Load32(vb + 24), Load32(vb + 12),
                                    Load16(geometry + 52)};
                if (!Plausible(buffer.data) || buffer.stride < 12 ||
                    buffer.stride > 128 || !buffer.count)
                    return {};
                bool known = false;
                for (const Buffer& other : out)
                    known = known || other.data == buffer.data;
                if (!known) out.push_back(buffer);
            }
        }
    }
    return out;
}

// True for the Impala's bumper_r2 as it ships, false once it has been raised.
bool SitsLow(const std::vector<Buffer>& buffers) {
    float low_x = 1e9f, low_y = 1e9f, high_y = -1e9f;
    for (const Buffer& b : buffers) {
        for (uint32_t i = 0; i < b.count; ++i) {
            const uint32_t at = b.data + i * b.stride;
            const float x = LoadFloat(at), y = LoadFloat(at + 4);
            if (!std::isfinite(x) || !std::isfinite(y)) return false;
            if (x < low_x) low_x = x;
            if (y < low_y) low_y = y;
            if (y > high_y) high_y = y;
        }
    }
    return low_x > -0.940f && low_x < -0.925f && low_y > -0.138f &&
           low_y < -0.130f && high_y > 0.077f && high_y < 0.085f;
}

void FixIfBroken(uint32_t drawable) {
    if (!Plausible(drawable) || !FirstBoneIs(drawable, "bumr02")) return;

    // Where the first buffer is and what its first height reads: unchanged
    // means this drawable was already looked at. A part streamed in again
    // brings the shipped heights back and is looked at again.
    const uint32_t lod = Load32(drawable + 8);
    if (!Plausible(lod)) return;
    static std::mutex mutex;
    static std::map<uint32_t, std::pair<uint32_t, uint32_t>> settled;
    std::lock_guard<std::mutex> lock(mutex);

    const auto stamp = [](uint32_t data) {
        return std::make_pair(data, Load32(data + 4));
    };
    if (const auto it = settled.find(drawable);
        it != settled.end() && it->second == stamp(it->second.first))
        return;

    const std::vector<Buffer> buffers = VertexBuffers(drawable);
    if (buffers.empty()) return;
    if (SitsLow(buffers)) {
        uint32_t vertices = 0;
        for (const Buffer& b : buffers) {
            for (uint32_t i = 0; i < b.count; ++i) {
                const uint32_t y = b.data + i * b.stride + 4;
                StoreFloat(y, LoadFloat(y) + kRaise);
            }
            vertices += b.count;
        }
        LARECOMP_APP_INFO("[mesh-fix] Impala 64 bumper_r2 raised: drawable "
                          "{:#010x}, {} vertices", drawable, vertices);
    }
    settled[drawable] = stamp(buffers.front().data);
}

}  // namespace

extern "C" REX_FUNC(rex_sub_823252B8) {
    if (larecomp::settings::car_mesh_fixes.Get()) FixIfBroken(ctx.r3.u32);
    __imp__rex_sub_823252B8(ctx, base);
}

extern "C" REX_FUNC(rex_sub_82324258) {
    if (larecomp::settings::car_mesh_fixes.Get()) FixIfBroken(ctx.r3.u32);
    __imp__rex_sub_82324258(ctx, base);
}

#endif  // REXGLUE_HAS_XEO3_TARGET
