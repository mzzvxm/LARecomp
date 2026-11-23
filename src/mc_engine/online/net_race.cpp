// Online races keep at most 16 racers when the session holds more players.
//
// How the game does it (IDB + the race scripts):
//   A race has room for 16 racers everywhere it is described. The RaceData the
//   scripts hand to the game (Race_SetRaceData sub_822566E0 / Race_GetRaceData
//   sub_82256708, copied by sub_82255E68) is a script struct: a script array of
//   16 entries of 192 bytes at +4 (the u32 at +4 is the array's size, 16), the
//   racer count at +3080 and the race's settings after it. NetworkCore.sc
//   builds it at race start: count = Net_NumRacers(), then for i < count it
//   stores Net_GetRacerFromList(i) into racers[i]. The script VM bounds-checks
//   every array index against the array's size (sub_82554E20, op 0x41 ARRAY:
//   "Array overrun", the thread is stopped), so a 17th racer stops NetworkCore
//   at the start of the race on every machine. The race scripts (Ordered,
//   UnOrdered, BaseWar...) receive the same 802-dword struct as their statics.
//   The locked racer list behind Net_GetRacerFromList is 16 pointers at
//   0x82873E28 with its count right after them (+64). Net_LockRacerList
//   (sub_82293770) walks the 31 racers and appends every one taking part with
//   no bound, so a 17th racer is written over the count and the 18th through a
//   pointer-sized index.
//   Who takes part is the gamer attribute ParticipantStatus (index 5, 3 bits,
//   replicated): each player sets its own to 1 when it accepts the race
//   (sub_82282788 -> sub_822919D8). Net_NumRacers (sub_82291CC0) counts the
//   session gamers whose ParticipantStatus is not 0; Net_IsRaceFinished
//   (sub_822923D8) waits until none is 1. When the race launches,
//   Net_RaceRequested (sub_82291A80) asks sub_8227E3D8 whether the local player
//   may race; on no it sets the local ParticipantStatus back to 0 and the
//   player stays out of that race.
//
// With more than 16 players allowed, the game's own two decisions get one more
// condition, so a race never has more than 16 racers:
//   - accepting a race (sub_82282788 @0x82282904): refused, by the game's own
//     refusal path, while 16 other gamers already take part;
//   - launching (sub_82291A80 @0x82291B1C): if more than 16 take part anyway
//     (players accepting at the same moment), the local player stays out unless
//     it is among the first 16 by gamer handle, so every machine drops the same
//     players;
// and the two places that would overflow are capped as a last guard:
//   - Net_LockRacerList (@0x82293904): a racer past the 16th takes the path of
//     a racer that is not in the race (RacerIndex -1, not listed);
//   - Net_NumRacers (@0x82291D7C): never more than 16.
// With net_max_players = 16 every hook here is a no-op.

#include "max_players.h"

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/ppc.h>
#include <rex/runtime.h>

#include <atomic>
#include <cstdint>

#include "larecomp_log.h"

namespace {

constexpr int kRaceSlots = 16;           // RaceData entries and the locked racer list
constexpr int kSessionGamers = 31;       // gamer table: 31 pointers from +12

// Local gamer: sub_82270170(*(0x8286D800), *(sub_82388580(0) + 4)).
constexpr uint32_t kProfileRoot = 0x8288E5ACu;   // sub_82388580: ***root, +4 = index
constexpr uint32_t kNetSession = 0x8286D800u;    // dword_8286D800
constexpr uint32_t kLocalGamersOff = 4284;       // sub_82270170: +4284 array
constexpr uint32_t kLocalGamerSize = 10832;      //               index * 10832
constexpr uint32_t kGamerTableOff = 208;         // gamer+208 = session gamer table
constexpr uint32_t kGamerPresentOff = 279;       // u8, tested first by every attribute read
constexpr uint32_t kGamerHandleOff = 64;         // gamer+16 info, +48 handle (sub_82502300)

// Gamer attributes: a bit buffer at gamer+112 (+0 data, +4 first bit), the bit
// offset of attribute k at 0x828721D0 + 4k (sub_82272870), read MSB first
// (sub_82618318).
constexpr uint32_t kAttrBits = 112;
constexpr uint32_t kAttrOffsets = 0x828721D0u;
constexpr int kAttrParticipantStatus = 5;

std::atomic<bool> g_join_logged{false};
std::atomic<bool> g_launch_logged{false};
std::atomic<bool> g_list_logged{false};
std::atomic<bool> g_count_logged{false};

// Set by the hook at the top of sub_82291A80 for the gate further down.
uint32_t g_launch_local = 0;
uint32_t g_launch_table = 0;

inline const uint8_t* Base() { return rex::Runtime::instance()->virtual_membase(); }

inline uint32_t Rd32(const uint8_t* b, uint32_t a) {
    return (uint32_t(b[a]) << 24) | (uint32_t(b[a + 1]) << 16) | (uint32_t(b[a + 2]) << 8) |
           uint32_t(b[a + 3]);
}
inline uint64_t Rd64(const uint8_t* b, uint32_t a) {
    return (uint64_t(Rd32(b, a)) << 32) | Rd32(b, a + 4);
}

uint32_t LocalGamer(const uint8_t* b) {
    uint32_t p = Rd32(b, kProfileRoot);
    if (p == 0) return 0;
    p = Rd32(b, p);
    if (p == 0) return 0;
    p = Rd32(b, p);
    if (p == 0) return 0;
    const uint32_t index = Rd32(b, p + 4);
    const uint32_t session = Rd32(b, kNetSession);
    if (session == 0 || index > 3) return 0;
    const uint32_t gamers = Rd32(b, session + kLocalGamersOff);
    return gamers == 0 ? 0 : gamers + index * kLocalGamerSize;
}

uint32_t ReadAttr(const uint8_t* b, uint32_t gamer, int k) {
    const uint32_t data = Rd32(b, gamer + kAttrBits);
    const uint32_t first = Rd32(b, gamer + kAttrBits + 4);
    const uint32_t off = Rd32(b, kAttrOffsets + 4u * k);
    const uint32_t end = Rd32(b, kAttrOffsets + 4u * (k + 1));
    if (data == 0 || end <= off || end - off > 32) return 0;
    uint32_t v = 0;
    for (uint32_t bit = first + off; bit < first + end; ++bit)
        v = (v << 1) | ((b[data + (bit >> 3)] >> (7 - (bit & 7))) & 1u);
    return v;
}

bool TakesPart(const uint8_t* b, uint32_t gamer) {
    return gamer != 0 && b[gamer + kGamerPresentOff] != 0 &&
           ReadAttr(b, gamer, kAttrParticipantStatus) != 0;
}

// Gamers of the session table taking part, the local one excluded when given.
int CountTakingPart(const uint8_t* b, uint32_t table, uint32_t except) {
    int n = 0;
    for (int i = 0; i < kSessionGamers; ++i) {
        const uint32_t g = Rd32(b, table + 12 + 4u * i);
        if (g != except && TakesPart(b, g)) ++n;
    }
    return n;
}

// How many gamers taking part sort before the local one. The gamer handle is
// the same on every machine, so every machine keeps the same 16.
int LocalRank(const uint8_t* b, uint32_t table, uint32_t local) {
    const uint64_t mine = Rd64(b, local + kGamerHandleOff);
    int rank = 0;
    for (int i = 0; i < kSessionGamers; ++i) {
        const uint32_t g = Rd32(b, table + 12 + 4u * i);
        if (g == local || !TakesPart(b, g)) continue;
        const uint64_t other = Rd64(b, g + kGamerHandleOff);
        if (other < mine || (other == mine && g < local)) ++rank;
    }
    return rank;
}

}  // namespace

// 0x82282904 (before clrlwi r11,r3,24) in sub_82282788, accepting a race:
// r3 = sub_8227E038(), the last of the game's own conditions. 0 takes the
// refusal path (0x82282C14) before the local ParticipantStatus is set.
void NetRace_82282904(PPCRegister& r3) {
    if (!net_limits::MaxPlayersChanged() || (r3.u32 & 0xFF) == 0) return;
    const uint8_t* b = Base();
    const uint32_t local = LocalGamer(b);
    const uint32_t table = local ? Rd32(b, local + kGamerTableOff) : 0;
    if (table == 0) return;
    const int others = CountTakingPart(b, table, local);
    if (others < kRaceSlots) return;
    r3.u64 = 0;
    if (!g_join_logged.exchange(true, std::memory_order_relaxed))
        LARECOMP_APP_INFO("[net-race] race already has {} racers; the local player stays out",
                          others);
}

// 0x82291AB0 (before lbz r11,15764(r29)) in sub_82291A80 (Net_RaceRequested):
// r3 = the local gamer, r10 = its session gamer table.
void NetRace_82291AB0(PPCRegister& r3, PPCRegister& r10) {
    if (!net_limits::MaxPlayersChanged()) return;
    g_launch_local = r3.u32;
    g_launch_table = r10.u32;
}

// 0x82291B1C (before clrlwi r11,r3,24) in sub_82291A80: r3 = sub_8227E3D8(),
// "may the local player race". 0 takes the game's path that sets the local
// ParticipantStatus to 0 and leaves the player out of this race.
void NetRace_82291B1C(PPCRegister& r3) {
    if (!net_limits::MaxPlayersChanged() || (r3.u32 & 0xFF) == 0) return;
    const uint8_t* b = Base();
    const uint32_t local = g_launch_local;
    const uint32_t table = g_launch_table;
    if (local == 0 || table == 0) return;
    const int total = CountTakingPart(b, table, 0);
    if (total <= kRaceSlots) return;
    const int rank = LocalRank(b, table, local);
    if (rank < kRaceSlots) return;
    r3.u64 = 0;
    if (!g_launch_logged.exchange(true, std::memory_order_relaxed))
        LARECOMP_APP_INFO("[net-race] {} racers at the start; the local player (rank {}) stays out",
                          total, rank);
}

// 0x82293904 (before li r4,7) in sub_82293770 (Net_LockRacerList), on the path
// that appends a racer: r28 = the list (16 pointers, count at +64). With the
// list full, true jumps to 0x82293930, the path of a racer that is not in the
// race (RacerIndex -1, not listed).
bool NetRace_82293904(PPCRegister& r28) {
    if (!net_limits::MaxPlayersChanged()) return false;
    const int count = int32_t(Rd32(Base(), r28.u32 + 64));
    if (count < kRaceSlots) return false;
    if (!g_list_logged.exchange(true, std::memory_order_relaxed))
        LARECOMP_APP_WARN("[net-race] racer list is full ({}); a racer is left out of it", count);
    return true;
}

// 0x82291D7C (before mr r3,r28) in sub_82291CC0 (Net_NumRacers): r28 = gamers
// taking part.
void NetRace_82291D7C(PPCRegister& r28) {
    if (!net_limits::MaxPlayersChanged() || r28.s32 <= kRaceSlots) return;
    if (!g_count_logged.exchange(true, std::memory_order_relaxed))
        LARECOMP_APP_WARN("[net-race] {} gamers take part; the race counts {}", r28.s32,
                          kRaceSlots);
    r28.u64 = uint64_t(kRaceSlots);
}

#else
#include <rex/ppc/context.h>
void NetRace_82282904(PPCRegister&) {}
void NetRace_82291AB0(PPCRegister&, PPCRegister&) {}
void NetRace_82291B1C(PPCRegister&) {}
bool NetRace_82293904(PPCRegister&) { return false; }
void NetRace_82291D7C(PPCRegister&) {}
#endif
