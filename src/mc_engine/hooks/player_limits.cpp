// Per-race data that only holds as many players as the race file describes.
//
// Starting grid: sub_82388E48 reads NumGridPositions and that many GridPosition
// vec4s (x, y, z, heading) from the race description into a heap array of
// exactly N entries (desc+76 = array, desc+80 = u16 count, desc+82 = u16
// capacity, allocated by sub_82388D78). The script binding of
// RaceDescription_GetGridPosition lands in sub_82388C88(desc, index, outPos,
// outHeading), which loads entry `index` with no bound check. Almost every race
// ships 16 positions, so a 17th racer would read past the array.
//
// With more than 16 players allowed, an index past the array gets a position
// extrapolated behind the last two rows: it keeps the column parity of the
// last row and steps back by the distance between the last two rows of that
// column, using the heading of the last row. Indexes inside the array, and every
// call while the session size is the game's own, run the original code.

#include "hooks_internal.h"
#include "../online/max_players.h"

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/ppc.h>

#include <atomic>

#include "larecomp_log.h"

REX_EXTERN(__imp__sub_82388C88);  // RaceDescription::GetGridPosition

namespace {
std::atomic<bool> g_grid_extrapolated_logged{false};

uint16_t ReadGuestU16(const uint8_t* base, uint32_t addr) {
    return uint16_t((base[addr] << 8) | base[addr + 1]);
}
}  // namespace

extern "C" REX_FUNC(sub_82388C88) {
    const uint32_t desc = ctx.r3.u32;
    const int32_t index = ctx.r4.s32;
    if (!net_limits::MaxPlayersChanged() || desc == 0) {
        __imp__sub_82388C88(ctx, base);
        return;
    }
    const uint32_t positions = ReadGuestU32(base, desc + 76);
    const int32_t count = ReadGuestU16(base, desc + 80);
    if (index < count || count < 4 || positions == 0 || index >= 64) {
        __imp__sub_82388C88(ctx, base);
        return;
    }

    const int32_t extra = index - count;
    const int32_t column = extra & 1;
    const float rows_back = float(extra / 2 + 1);
    const uint32_t last = positions + 16u * uint32_t(count - 2 + column);
    const uint32_t prev = positions + 16u * uint32_t(count - 4 + column);

    float out[4];
    for (int i = 0; i < 3; ++i) {
        const float a = ReadGuestF32(base, last + 4u * i);
        const float b = ReadGuestF32(base, prev + 4u * i);
        out[i] = a + rows_back * (a - b);
    }
    out[3] = ReadGuestF32(base, last + 12);

    // The original stores the whole vector with stvx (16-byte aligned) and the
    // heading lane with stfs.
    const uint32_t out_pos = ctx.r5.u32 & ~0xFu;
    for (int i = 0; i < 4; ++i) WriteGuestF32(base, out_pos + 4u * i, out[i]);
    WriteGuestF32(base, ctx.r6.u32, out[3]);

    if (!g_grid_extrapolated_logged.exchange(true, std::memory_order_relaxed))
        LARECOMP_APP_INFO("[player_limits] grid slot {} placed behind the {} the race defines",
                          index, count);
}

#endif  // REXGLUE_HAS_XEO3_TARGET
