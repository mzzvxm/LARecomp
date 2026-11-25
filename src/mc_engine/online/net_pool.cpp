// Remote-gamer item pool of the online gamer manager, sized for MaxPlayers.
//
// How the game does it (IDB):
//   sub_8226B988 allocates the gamer manager (138004 bytes) and runs its ctor
//   sub_82279CC8: 31 gamer/connection objects of 220 bytes at +131152 (slot 0
//   is the local gamer, sub_82275400 hands out remote slots 1..30), a u8[30]
//   at +137972 indexed by owned-car slot, and a pointer at +131148 to a pool
//   built by sub_82278620(pool, 15) @0x82279D50: a 20-byte header (+0 buffer,
//   +4 u16 capacity, +6 u16 free count, +8 item size 22304, +12 free-list head)
//   and a buffer of capacity * 22304 bytes from the game allocator
//   (sub_82130528). The manager's destructor sub_8227A2E0 frees both.
//   vtbl+16 sub_8227CDF0 gives a remote gamer an item (gamer+212) through the
//   pop sub_8227CD20; vtbl+20 sub_8227CE30 and sub_8227CEF0 give it back
//   through sub_8227CD80. The pop reads head->next @0x8227CD50 before it tests
//   the head for null, so the 16th remote gamer (the 17th player) dereferences
//   guest address 0 once the 15 items are taken.
//   Nothing else reads the pool header: the only accesses to +131148 are the
//   ctor, the dtor, the pop and the two releases. Items carry no player index
//   (+22296 is an owned-car slot, +22292 a sequence number) and every reader of
//   gamer+212 tests it for null before use.
//
// With net_max_players above 16 the pool gets MaxPlayers - 1 items, one per
// remote player, the game's own ratio (15 for 16). The buffer still comes from
// the game allocator and is freed by the game's destructor; the manager's
// layout does not change. The pop also returns null instead of reading address
// 0 when the pool is empty, leaving the pool untouched; that gamer then simply
// has no item, which every reader already handles. With net_max_players = 16
// every hook here is a no-op.

#include "max_players.h"

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/ppc/recompiled_code.h>
#include <rex/ppc/context.h>
#include "larecomp_guest.h"
#include <rex/kernel/title_runtime.h>

#include <atomic>
#include <cstdint>

#include "larecomp_log.h"

namespace {

constexpr int kGamePoolItems = 15;
constexpr uint32_t kItemBytes = 22304;

std::atomic<bool> g_empty_pop_logged{false};

inline uint32_t Rd32(const uint8_t* b, uint32_t a) {
    return (uint32_t(b[a]) << 24) | (uint32_t(b[a + 1]) << 16) | (uint32_t(b[a + 2]) << 8) |
           uint32_t(b[a + 3]);
}
inline uint16_t Rd16(const uint8_t* b, uint32_t a) {
    return uint16_t((b[a] << 8) | b[a + 1]);
}

int PoolItems() {
    // One item per remote player: MaxPlayers - 1, never below the game's 15.
    const int items = net_limits::MaxPlayers() - 1;
    return items > kGamePoolItems ? items : kGamePoolItems;
}

}  // namespace

// 0x82279D50 (after li r4,15) in sub_82279CC8: r4 = capacity passed to
// sub_82278620, which allocates r4 * 22304 bytes.
void NetPool_82279D50(PPCRegister& r4) {
    if (!net_limits::MaxPlayersChanged()) return;
    r4.u64 = uint64_t(PoolItems());
}

// 0x82279D58 (before b 0x82279D60) in sub_82279CC8: r3 = the pool header that
// sub_82278620 just built. Logs what the game allocated.
void NetPool_82279D58(PPCRegister& r3) {
    if (!net_limits::MaxPlayersChanged()) return;
    const uint8_t* b = larecomp::guest::Membase();
    const uint32_t pool = r3.u32;
    if (pool == 0) {
        LARECOMP_APP_ERROR("[net-pool] gamer item pool header was not allocated");
        return;
    }
    const uint32_t buffer = Rd32(b, pool + 0);
    const uint32_t capacity = Rd16(b, pool + 4);
    LARECOMP_APP_INFO("[net-pool] gamer item pool: {} items of {} bytes at {:08X} ({} KB)",
                      capacity, kItemBytes, buffer, (capacity * kItemBytes) >> 10);
}

// 0x8227CD50 (before lwz r6,0(r31)) in sub_8227CD20: r31 = free-list head,
// already copied to r3. Returning true jumps to 0x8227CD6C (the epilogue), so
// an empty pool returns null without touching the free count or the head.
bool NetPool_8227CD50(PPCRegister& r31) {
    if (!net_limits::MaxPlayersChanged() || r31.u32 != 0) return false;
    if (!g_empty_pop_logged.exchange(true, std::memory_order_relaxed))
        LARECOMP_APP_ERROR(
            "[net-pool] gamer item pool is empty ({} items); a remote gamer gets no item",
            PoolItems());
    return true;
}

#else
#include <rex/ppc/context.h>
void NetPool_82279D50(PPCRegister&) {}
void NetPool_82279D58(PPCRegister&) {}
bool NetPool_8227CD50(PPCRegister&) { return false; }
#endif
