#ifndef REXGLUE_HAS_XEO3_TARGET
#include "carbon_parts.h"
#include "larecomp_log.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>

#include <rex/cvar.h>
#include <rex/runtime.h>

// ─────────────────────────────────────────────────────────────────────────
// Extended carbon fiber parts
//
// Vanilla MCLA has exactly ONE carbon flag and it only reaches the hood.
// Reverse engineering (default.xex):
//
//   CustomData        = vehicle->[+20]->[+132]      (per-car customization)
//     +0x860 + slot   = part variant index, 55 slots (names @ off_827E9608)
//     +0x898          = CarbonFiberHood, a plain bool
//   The save reflection (sub_82394388) registers the same byte as an u8
//   named "CarbonFiberHood" at reflectionBase+2136, where
//   reflectionBase = CustomData + 0x40.
//
//   Carbon is NOT a separate model. sub_82377950 resolves the technique
//   group id of "carbonfiberhood" into dword_827E7614, and the part draw
//   loop sub_8235D500 forces it around the part being drawn:
//
//     8235DED0  cmpwi r27, 2       ; slot 2  = "hood"
//     8235DED8  cmpwi r27, 0x28    ; slot 40 = "hood_wide"
//     8235DEE4  lbz   r10, 0x898(r11)
//     8235DF00  bl    sub_82377C30 ; dword_827D5C60 = dword_827E7614
//     ...
//     8235E068  bl    sub_82377C50 ; dword_827D5C60 = -1
//
//   So the only thing keeping carbon off the trunk/roof/doors is the
//   hardcoded `slot == 2 || slot == 40` test. Widening it needs no new art
//   and no part sacrifice — the save-editing trick of stealing another
//   part's model index is working around this test, not around missing
//   geometry.
//
// What this file does:
//   * reinterprets the +0x898 byte as a BITMASK of part groups. bit0 keeps
//     its vanilla meaning (hood), so old saves and real-360 saves stay
//     valid — vanilla only ever tests the byte for non-zero.
//   * replaces the slot test in the draw loop with the mask.
//   * turns the shop's carbon button (garage action 147) from an on/off
//     toggle into a cycle through presets, so the extra groups are
//     reachable in-game without editing anything.
// ─────────────────────────────────────────────────────────────────────────

REXCVAR_DEFINE_BOOL(carbon_all_parts, true, "MCLA/Patches",
    "Carbon fiber can be applied to any body part, not just the hood.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Menu instrumentation. Off by default because it is chatty, but it is what
// mapped the garage UI in the first place: the state tree, the field dump that
// found state+12, the activate/command stream. Keep it — turning it on again
// beats rediscovering any of it.
REXCVAR_DEFINE_BOOL(carbon_menu_diag, false, "MCLA/Carbon",
    "Log the garage menu state tree, per-state field dumps and the "
    "activate/command stream while the paint screen is open.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// There is no cvar for the part selection on purpose. The mask is per car —
// it lives in that car's own customization data and rides along in the save,
// exactly like paint or rims — so the menus edit the car in front of you, not
// a global setting. See CarbonGetMask / CarbonSetMask at the bottom.

namespace {

// Forced technique group (grmShader) — sub_82187BE0 writes it, -1 = none.
constexpr uint32_t kForcedTechniqueGroup = 0x827D5C60;
// Technique group id of "carbonfiberhood", filled by sub_82377950 at boot.
constexpr uint32_t kCarbonGroupId = 0x827E7614;

// sub_8235D500 stack: var_5EC holds CustomData, arg_1F holds the caller's
// "carbon allowed in this pass" byte (a2).
constexpr uint32_t kSpCustomData = 0x64;
constexpr uint32_t kSpAllowCarbon = 0x66F;

constexpr uint32_t kCustomCarbonMask = 0x898;   // from CustomData
constexpr uint32_t kReflCarbonMask = 0x858;     // from CustomData + 0x40

// dword_8288DCF8 — the vehicle the local player is driving. sub_8235D500
// itself compares a1 against it to tell the hero car from traffic.
constexpr uint32_t kPlayerVehicle = 0x8288DCF8;

// Part group bits. Slot indices come from off_827E9608 (55 entries).
constexpr uint8_t kGrpHood = 0x01;    // hood, hood_wide
constexpr uint8_t kGrpTrunk = 0x02;   // trunk_swap, trunk_wide
constexpr uint8_t kGrpDoors = 0x04;   // door_l/r, door_l/r_wide
constexpr uint8_t kGrpRoof = 0x08;    // conv_top, conv_topup, conv_topdown
constexpr uint8_t kGrpBumpers = 0x10; // bumper_f, bumper_r
constexpr uint8_t kGrpBody = 0x20;    // fenders, skirts, widebody
constexpr uint8_t kGrpSpoiler = 0x40; // spoiler, spoiler_b, spoiler_wide
constexpr uint8_t kGrpExtras = 0x80;  // grill, intercooler, exhaust, bike shells

// slot -> group bit. 0 means the slot never receives carbon: lights, glass,
// interior, wheels-related and occluders keep their own shaders, and a part
// whose shader has no "carbonfiberhood" technique draws with zero passes
// (i.e. vanishes) if the group is forced on it.
//
// The roof slots (0, 1, 22) are a measured half-case: on a convertible the
// top has no carbon technique and vanishes instead of turning carbon. That is
// kept as an opt-in — most cars carry a fixed roof rather than a top, and a
// deliberately roofless build is a look some players want. Off by default.
//
// Trunks are shader-dependent per car: some models turn carbon, others keep
// their paint. Nothing to fix on our side — the technique simply is not in
// that car's trunk shader.
constexpr uint8_t kSlotGroup[55] = {
    /*  0 conv_top      */ kGrpRoof,
    /*  1 conv_topup    */ kGrpRoof,
    /*  2 hood          */ kGrpHood,
    /*  3 bumper_r      */ kGrpBumpers,
    /*  4 trunk_swap    */ kGrpTrunk,
    /*  5 intercooler   */ kGrpExtras,
    /*  6 bumper_f      */ kGrpBumpers,
    /*  7 fenders       */ kGrpBody,
    /*  8 skirts        */ kGrpBody,
    /*  9 spoiler       */ kGrpSpoiler,
    /* 10 spoiler_b     */ kGrpSpoiler,
    /* 11 headlight     */ 0,
    /* 12 taillight     */ 0,
    /* 13 taillight_b   */ 0,
    /* 14 frontgrill    */ kGrpExtras,
    /* 15 wheeliebar    */ 0,
    /* 16 blower        */ kGrpExtras,
    /* 17 bike_tail     */ kGrpExtras,
    /* 18 bike_tank     */ kGrpExtras,
    /* 19 bike_cowl     */ kGrpExtras,
    /* 20 door_l        */ kGrpDoors,
    /* 21 door_r        */ kGrpDoors,
    /* 22 conv_topdown  */ kGrpRoof,
    /* 23 exh_pipe      */ 0,
    /* 24 exh_side      */ 0,
    /* 25 armfl         */ 0,
    /* 26 armfr         */ 0,
    /* 27 armrl         */ 0,
    /* 28 armrr         */ 0,
    /* 29 shockfl       */ 0,
    /* 30 shockfr       */ 0,
    /* 31 shockrl       */ 0,
    /* 32 shockrr       */ 0,
    /* 33 guiderl       */ 0,
    /* 34 guiderr       */ 0,
    /* 35 axle          */ 0,
    /* 36 subbumf       */ 0,
    /* 37 subbumr       */ 0,
    /* 38 widebody      */ kGrpBody,
    /* 39 spoiler_wide  */ kGrpSpoiler,
    /* 40 hood_wide     */ kGrpHood,
    /* 41 door_l_wide   */ kGrpDoors,
    /* 42 door_r_wide   */ kGrpDoors,
    /* 43 trunk_wide    */ kGrpTrunk,
    /* 44 interior      */ 0,
    /* 45 seats_f       */ 0,
    /* 46 seats_p       */ 0,
    /* 47 door_l_int    */ 0,
    /* 48 door_r_int    */ 0,
    /* 49 stereo        */ 0,
    /* 50 stereo_l      */ 0,
    /* 51 stereo_r      */ 0,
    /* 52 steer_whl     */ 0,
    /* 53 post_gauge    */ 0,
    /* 54 occluder_     */ 0,
};

// Presets the shop button cycles through. Index 0 is "no carbon", index 1 is
// the vanilla hood-only value so a car customised here still reads correctly
// on a stock console.
constexpr uint8_t kPresets[] = {
    0x00,                                                            // off
    kGrpHood,                                                        // hood
    uint8_t(kGrpHood | kGrpTrunk),                                   // + trunk
    uint8_t(kGrpHood | kGrpTrunk | kGrpSpoiler),                     // + spoiler
    uint8_t(kGrpHood | kGrpTrunk | kGrpSpoiler | kGrpDoors),         // + doors
    uint8_t(kGrpHood | kGrpTrunk | kGrpSpoiler | kGrpDoors |
            kGrpBumpers | kGrpBody),
    uint8_t(0xFF & ~kGrpRoof),                                       // full
};
constexpr int kNumPresets = int(sizeof(kPresets) / sizeof(kPresets[0]));

constexpr const char* kPresetNames[kNumPresets] = {
    "off", "hood", "hood+trunk", "hood+trunk+spoiler",
    "+doors", "+bumpers+body", "full carbon",
};

// CustomData is NOT per car. Measured: it hangs off the player, not off the
// vehicle — sub_826A5528 reaches the same block through
// sub_822A3998(player, 0) + 48 + 132 — and swapping cars in the garage leaves
// the pointer unchanged AND does not reload the carbon byte, so a mask set on
// one car showed up on the next one.
//
// So identity comes from our side: the vehicle's model data (veh->[+20]->[+80],
// the object the draw loop resolves parts from) is one instance per car model
// and stable while loaded, which makes a usable key. The table below is the
// real per-car store; the game's byte is kept in sync underneath it so the
// save still carries something sane.
std::atomic<uint32_t> g_player_custom{0};
std::atomic<uint32_t> g_player_car{0};   // model-data key of the player's car

std::mutex g_car_mask_mutex;
std::map<uint32_t, uint8_t> g_car_masks;

uint8_t LookupCarMask(uint32_t car, uint8_t fallback, bool* known) {
    std::lock_guard<std::mutex> lock(g_car_mask_mutex);
    auto it = g_car_masks.find(car);
    if (it != g_car_masks.end()) {
        if (known) *known = true;
        return it->second;
    }
    // First time this car is seen: adopt whatever it already carries, which is
    // what the save loaded for it.
    g_car_masks.emplace(car, fallback);
    if (known) *known = false;
    return fallback;
}

void StoreCarMask(uint32_t car, uint8_t mask) {
    std::lock_guard<std::mutex> lock(g_car_mask_mutex);
    g_car_masks[car] = mask;
}

uint8_t* GetMembase() {
    auto* rt = rex::Runtime::instance();
    return rt ? rt->virtual_membase() : nullptr;
}

bool IsPlausibleGuestPtr(uint32_t ea) {
    return ea >= 0x1000 && ea < 0xC0000000;
}

uint32_t ReadGuestBE32(const uint8_t* base, uint32_t ea) {
    const uint8_t* p = base + ea;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void WriteGuestBE32(uint8_t* base, uint32_t ea, uint32_t v) {
    uint8_t* p = base + ea;
    p[0] = uint8_t((v >> 24) & 0xFF);
    p[1] = uint8_t((v >> 16) & 0xFF);
    p[2] = uint8_t((v >> 8) & 0xFF);
    p[3] = uint8_t(v & 0xFF);
}

// Shared predicate for both draw-loop sites. veh = the vehicle being drawn
// (r31 in sub_8235D500).
bool SlotWantsCarbon(uint8_t* base, uint32_t sp, int slot, uint32_t veh) {
    if (slot < 0 || slot >= int(sizeof(kSlotGroup))) return false;
    if (!base[sp + kSpAllowCarbon]) return false;  // caller's a2

    const uint32_t custom = ReadGuestBE32(base, sp + kSpCustomData);
    if (!IsPlausibleGuestPtr(custom)) return false;

    // The player's car drives the shared block back to whatever this specific
    // car is supposed to look like.
    if (veh && veh == ReadGuestBE32(base, kPlayerVehicle)) {
        g_player_custom.store(custom, std::memory_order_relaxed);

        const uint32_t holder = ReadGuestBE32(base, veh + 20);
        const uint32_t car = holder ? ReadGuestBE32(base, holder + 80) : 0;
        if (car) {
            const uint32_t prev =
                g_player_car.exchange(car, std::memory_order_relaxed);
            // Only the first car of the session inherits the byte: that one
            // really was filled from the save. Every car after it would just
            // be inheriting the previous car's mask out of the shared block,
            // which is the bug this table exists to kill — those start clean.
            static bool first_car = true;
            const uint8_t seed =
                first_car ? base[custom + kCustomCarbonMask] : 0;
            first_car = false;

            bool known = false;
            const uint8_t want = LookupCarMask(car, seed, &known);
            if (base[custom + kCustomCarbonMask] != want)
                base[custom + kCustomCarbonMask] = want;
            if (prev != car) {
                LARECOMP_APP_INFO("[Carbon] car 0x{:08X} ({}), mask 0x{:02X}",
                                  car, known ? "known" : "new", want);
            }
        }
    }

    const uint8_t mask = base[custom + kCustomCarbonMask];
    if (!mask) return false;

    if (!REXCVAR_GET(carbon_all_parts))
        return (slot == 2 || slot == 40);  // vanilla behaviour

    return (kSlotGroup[slot] & mask) != 0;
}

}  // namespace

uint8_t CarbonBitForSlot(int slot) {
    if (slot < 0 || slot >= int(sizeof(kSlotGroup))) return 0;
    return kSlotGroup[slot];
}

// ── Per-car mask ────────────────────────────────────────────────────────

bool CarbonHaveCar() {
    return g_player_custom.load(std::memory_order_relaxed) != 0 &&
           GetMembase() != nullptr;
}

uint8_t CarbonGetMask() {
    uint8_t* base = GetMembase();
    const uint32_t custom = g_player_custom.load(std::memory_order_relaxed);
    if (!base || !IsPlausibleGuestPtr(custom)) return 0;
    return base[custom + kCustomCarbonMask];
}

void CarbonSetMask(uint8_t mask) {
    uint8_t* base = GetMembase();
    const uint32_t custom = g_player_custom.load(std::memory_order_relaxed);
    const uint32_t car = g_player_car.load(std::memory_order_relaxed);
    if (!base || !IsPlausibleGuestPtr(custom)) return;

    // The table is what makes this per car; the guest byte is the live value
    // the renderer and the save both read.
    if (car) StoreCarMask(car, mask);
    base[custom + kCustomCarbonMask] = mask;
    LARECOMP_APP_INFO("[Carbon] mask 0x{:02X} on car 0x{:08X}", mask, car);
}

bool CarbonHasGroup(uint8_t group) {
    return (CarbonGetMask() & group) != 0;
}

void CarbonToggleGroup(uint8_t group) {
    CarbonSetMask(uint8_t(CarbonGetMask() ^ group));
}

void CarbonCyclePreset() {
    const uint8_t cur = CarbonGetMask();
    int idx = 0;
    for (int i = 0; i < kNumPresets; ++i) {
        if (kPresets[i] == cur) { idx = i; break; }
    }
    CarbonSetMask(kPresets[(idx + 1) % kNumPresets]);
}

int CarbonPresetCount() { return kNumPresets; }

const char* CarbonPresetNameAt(int index) {
    if (index < 0 || index >= kNumPresets) return "";
    return kPresetNames[index];
}

uint8_t CarbonPresetMaskAt(int index) {
    if (index < 0 || index >= kNumPresets) return 0;
    return kPresets[index];
}

void CarbonApplyPresetIndex(int index) {
    if (index < 0 || index >= kNumPresets) return;
    if (CarbonGetMask() == kPresets[index]) return;
    CarbonSetMask(kPresets[index]);
}

const char* CarbonPresetName() {
    if (!CarbonHaveCar()) return "NO CAR";
    const uint8_t cur = CarbonGetMask();
    for (int i = 0; i < kNumPresets; ++i)
        if (kPresets[i] == cur) return kPresetNames[i];
    return "custom";
}

// 0x8235DED0 — replaces `slot == 2 || slot == 40` + flag test + the call to
// sub_82377C30. Always returns true so the original block is skipped and the
// recompiled code jumps straight to 0x8235DF04. r31 = the vehicle (a1).
bool Hook_CarbonPartPush(PPCRegister& r1, PPCRegister& r27, PPCRegister& r31) {
    uint8_t* base = GetMembase();
    if (!base) return true;

    const uint32_t sp = static_cast<uint32_t>(r1.u64);
    const int slot = static_cast<int>(static_cast<int32_t>(r27.u64));
    const uint32_t veh = static_cast<uint32_t>(r31.u64);

    if (SlotWantsCarbon(base, sp, slot, veh)) {
        const uint32_t group = ReadGuestBE32(base, kCarbonGroupId);
        WriteGuestBE32(base, kForcedTechniqueGroup, group);
    }
    return true;
}

// 0x8235E038 — mirror of the push site; restores the forced group to -1.
// The predicate is identical, so the push/pop pairing stays symmetric.
bool Hook_CarbonPartPop(PPCRegister& r1, PPCRegister& r27, PPCRegister& r31) {
    uint8_t* base = GetMembase();
    if (!base) return true;

    const uint32_t sp = static_cast<uint32_t>(r1.u64);
    const int slot = static_cast<int>(static_cast<int32_t>(r27.u64));
    const uint32_t veh = static_cast<uint32_t>(r31.u64);

    if (SlotWantsCarbon(base, sp, slot, veh))
        WriteGuestBE32(base, kForcedTechniqueGroup, 0xFFFFFFFFu);

    return true;
}

// 0x826AE248 — garage action 147. Vanilla is `flag = !flag`; with the
// extension on, walk the preset list instead. r11 = CustomData + 0x40.
bool Hook_CarbonToggle(PPCRegister& r11) {
    if (!REXCVAR_GET(carbon_all_parts)) return false;  // let vanilla run

    uint8_t* base = GetMembase();
    if (!base) return false;

    const uint32_t refl = static_cast<uint32_t>(r11.u64);
    if (!IsPlausibleGuestPtr(refl)) return false;

    uint8_t* field = base + refl + kReflCarbonMask;
    const uint8_t cur = *field;

    int idx = 0;
    for (int i = 0; i < kNumPresets; ++i) {
        if (kPresets[i] == cur) { idx = i; break; }
    }
    const int next = (idx + 1) % kNumPresets;
    *field = kPresets[next];

    LARECOMP_APP_INFO("[Carbon] preset {} -> 0x{:02X} ({})", next,
                      kPresets[next], kPresetNames[next]);
    return true;  // skip the vanilla toggle
}

#else  // REXGLUE_HAS_XEO3_TARGET

#include "carbon_parts.h"

bool Hook_CarbonPartPush(PPCRegister& r1, PPCRegister& r27, PPCRegister& r31) { return false; }
bool Hook_CarbonPartPop(PPCRegister& r1, PPCRegister& r27, PPCRegister& r31) { return false; }
bool Hook_CarbonToggle(PPCRegister& r11) { return false; }
uint8_t CarbonBitForSlot(int slot) { return 0; }
bool CarbonHaveCar() { return false; }
uint8_t CarbonGetMask() { return 0; }
void CarbonSetMask(uint8_t mask) {}
bool CarbonHasGroup(uint8_t group) { return false; }
void CarbonToggleGroup(uint8_t group) {}
void CarbonCyclePreset() {}
const char* CarbonPresetName() { return ""; }
int CarbonPresetCount() { return 0; }
const char* CarbonPresetNameAt(int index) { return ""; }
uint8_t CarbonPresetMaskAt(int index) { return 0; }
void CarbonApplyPresetIndex(int index) {}

#endif  // REXGLUE_HAS_XEO3_TARGET
