// Player-count limits in the lobby gamer list and the post-race standings that
// stay at 16 when the session allows more players.
//
// Lobby gamer list (mcNavGamerList, vtable 0x82024C44): sub_82286AB8 fills the
// list through sub_82286498(list, team, group, paged, max_rows). In the lobby
// layout (vtbl+668 returns true for this class) the three groups share a fixed
// budget of 16 rows: max_rows = 16 for the first group (li r7,16 @0x82286CCC),
// 16 - first for the second (subfic r7,r11,16 @0x82286D14) and
// 16 - second - first for the private-slot group (subfic r9,r10,16 @0x82286D40).
// Rows are added by sub_82284788 (vtbl+664), which returns null once the row
// index reaches the count of rows the list provider holds, so a larger budget
// never writes past the list; it only lets rows 17..N appear when the provider
// has them.
//
// Post-race standings (StandingsForm, sub_82674FF8): a local finishing
// position above 16 is shown as RACE_RESULT_DNF (cmpwi r25,16 ; bgt
// @0x826750C4/0x826750CC). Positions up to the session limit take the "%d" +
// suffix path instead; the 4-byte buffer at form+1004 holds "31".
//
// With net_max_players left at 16 every hook is a no-op.

#include "max_players.h"

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/ppc/recompiled_code.h>
#include <rex/ppc/context.h>
#include "larecomp_guest.h"

#include <cstdint>

namespace {
constexpr int kGameRows = 16;

int RowBudget() {
    const int n = net_limits::MaxPlayers();
    return n > kGameRows ? n : kGameRows;
}
}  // namespace

// 0x82286CCC (after li r7,16) in sub_82286AB8: max_rows of the first group.
void NetSweep_82286CCC(PPCRegister& r7) {
    if (!net_limits::MaxPlayersChanged()) return;
    r7.u64 = uint64_t(RowBudget());
}

// 0x82286D14 (after subfic r7,r11,16) in sub_82286AB8: r7 = 16 - rows of the
// first group; the budget becomes N - rows.
void NetSweep_82286D14(PPCRegister& r7) {
    if (!net_limits::MaxPlayersChanged()) return;
    r7.u64 = uint64_t(int64_t(r7.s32 + (RowBudget() - kGameRows)));
}

// 0x82286D40 (after subfic r9,r10,16) in sub_82286AB8: r9 = 16 - rows of the
// second group; the following subf removes the first group's rows.
void NetSweep_82286D40(PPCRegister& r9) {
    if (!net_limits::MaxPlayersChanged()) return;
    r9.u64 = uint64_t(int64_t(r9.s32 + (RowBudget() - kGameRows)));
}

// 0x826750CC (before bgt) in sub_82674FF8: r25 = local finishing position.
// Returning true skips the branch to the DNF text, so 17..N print as numbers.
// r25 itself is left alone: it is the value printed and the suffix selector.
bool NetSweep_826750CC(PPCRegister& r25) {
    if (!net_limits::MaxPlayersChanged()) return false;
    return r25.s32 > kGameRows && r25.s32 <= net_limits::MaxPlayers();
}

#else
#include <rex/ppc/context.h>
void NetSweep_82286CCC(PPCRegister&) {}
void NetSweep_82286D14(PPCRegister&) {}
void NetSweep_82286D40(PPCRegister&) {}
bool NetSweep_826750CC(PPCRegister&) { return false; }
#endif
