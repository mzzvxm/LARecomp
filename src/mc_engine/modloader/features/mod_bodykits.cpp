#ifndef REXGLUE_HAS_XEO3_TARGET
//
// More than one wide body kit per car, picked from the garage's `body` part.
//
// MCLA's `body` menu is a switch, not a list. Whatever line of the car's
// Parts.gtun is chosen, the game only records whether it is line 0 or not:
//
//   sub_8269CC70 (garage: apply the pending part changes), case 18 (`body`)
//     r20 = the chosen line's index column
//     sub_82391EA8(cfg, r20 != 0)        stores cfg+2139 WideBodyKit, nothing else
//
// and the drawable each slot shows is cfg[2080 + slot] for every slot, the
// wide ones included (sub_8235D500 draws, sub_8235C410 streams). Nothing in the
// menu ever writes cfg[2080 + 38], so `body_1` and `body_2` both show
// widebody0. WideBodyKit itself only swaps the visibility mask
// (byte_82050D9C -> byte_82050DDC): slots 2..21 and 36/37 hide, 38..43 --
// widebody, spoiler_wide, hood_wide, door_l_wide, door_r_wide, trunk_wide --
// show.
//
// So line N of the `body` part becomes kit N-1: WideBodyKit = (N != 0) as
// before, and with N >= 1 the six wide slots take index N-1. A car the game
// ships has only body_0 and body_1, which lands on index 0 -- what it always
// drew. The six bytes live in the car config the save already stores.
//
// The getter sub_82391F48 answers slot 38 with WideBodyKit itself; the garage
// uses that answer to mark the installed line and to put it back when the menu
// is cancelled, so it answers N instead.
//
// sub_82391EA8 has exactly one caller (0x8269CD00) and r20 is the line index at
// that call in the recompiled code too (ctx.r20, loaded by `lwzx r20,r31,r26`).
//

#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>

#include <cstdint>

#include "../../logging.h"
#include "larecomp_log.h"

REXCVAR_DEFINE_BOOL(mod_bodykits, true, "MCLA/Garage",
    "Line N of a car's `body` part shows wide body kit N-1 (widebody, spoiler_wide, "
    "hood_wide, door_l_wide, door_r_wide, trunk_wide index N-1). Shipped cars only "
    "have body_0/body_1 and are unaffected.")
    .debug_only();

REX_EXTERN(__imp__rex_sub_82391EA8);
REX_EXTERN(__imp__rex_sub_82391F48);

namespace {

constexpr uint32_t kSlotIndexAt = 2080;   // cfg: one index byte per part slot
constexpr uint32_t kWideBodyKitAt = 2139;  // cfg: WideBodyKit
constexpr uint32_t kWidebodySlot = 38;     // widebody .. trunk_wide = 38..43
constexpr uint32_t kWideSlots = 6;
constexpr uint32_t kMaxIndex = 9;          // the loader asks for indices 0..9

// Single bytes: no byte order to undo.
uint8_t LoadU8(const uint8_t* base, uint32_t address) { return base[address]; }
void StoreU8(uint8_t* base, uint32_t address, uint8_t value) { base[address] = value; }

}  // namespace

// cfg->SetWideBodyKit(bool). r3 = cfg, r4 = (line != 0), r20 = line (caller).
extern "C" REX_FUNC(rex_sub_82391EA8) {
    const uint32_t cfg = ctx.r3.u32;
    const uint32_t line = ctx.r20.u32;
    __imp__rex_sub_82391EA8(ctx, base);
    if (!REXCVAR_GET(mod_bodykits) || !cfg || line == 0) return;

    const uint32_t index = line - 1 > kMaxIndex ? kMaxIndex : line - 1;
    const uint8_t before = LoadU8(base, cfg + kSlotIndexAt + kWidebodySlot);
    for (uint32_t i = 0; i < kWideSlots; ++i) {
        StoreU8(base, cfg + kSlotIndexAt + kWidebodySlot + i, static_cast<uint8_t>(index));
    }
    if (before != index) {
        LARECOMP_APP_INFO("[mods] body line {} -> wide body kit {} (widebody{} .. trunk_wide{})",
                          line, index, index, index);
    }
}

// cfg->GetPartIndex(slot). Slot 38 answers the `body` line, not the bool.
extern "C" REX_FUNC(rex_sub_82391F48) {
    const uint32_t cfg = ctx.r3.u32;
    const uint32_t slot = ctx.r4.u32;
    __imp__rex_sub_82391F48(ctx, base);
    if (!REXCVAR_GET(mod_bodykits) || slot != kWidebodySlot || !cfg) return;
    if (LoadU8(base, cfg + kWideBodyKitAt) == 0) return;  // r3 already 0
    const uint32_t index = LoadU8(base, cfg + kSlotIndexAt + kWidebodySlot);
    ctx.r3.u64 = (index > kMaxIndex ? kMaxIndex : index) + 1;
}

#endif  // REXGLUE_HAS_XEO3_TARGET
