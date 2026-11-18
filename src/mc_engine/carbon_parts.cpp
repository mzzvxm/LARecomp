#ifndef REXGLUE_HAS_XEO3_TARGET
#include "carbon_parts.h"
#include "larecomp_log.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "settings/larecomp_settings.h"
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
//   * writes every mask change into the owned car's record in the save as
//     well as into the live config, so the mask stays with that car.
//
// Where the byte lives per car:
//   The 0x898 byte belongs to a CarConfig (8176 bytes). Every owned car has
//   its own CarConfig in the LocalOptions save block (0x8288E920 + 1360 +
//   8176 * index; +1340 owned count, +1344 current index, +254032 dirty), and
//   the game moves the WHOLE config, carbon included, both ways:
//     slot -> live   LoadOwnedVehicle sub_826A4718 -> sub_822AAE48 stages a
//                    copy, sub_822AAA18 copies it into the live config once
//                    the car has streamed in.
//     live -> slot   mcGarage::LeaveGarage sub_826AA930, FinishCheckout
//                    sub_826AA3C0, sub_826A7170, sub_8226D8F8.
//   The copy is sub_8238EE58 -> sub_823934E8, whose 64-byte block at
//   reflection +2080..+2143 covers the 55 part variants and CarbonFiberHood
//   (+2136). A mask set outside the garage would only reach the slot on the
//   next commit, so the setter writes the slot itself too.
// ─────────────────────────────────────────────────────────────────────────

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

// LocalOptions save block (sub_82387B90 returns this static object).
constexpr uint32_t kLocalOptions = 0x8288E920;
constexpr uint32_t kLoOwnedCount = 1340;
constexpr uint32_t kLoCurrentIndex = 1344;
constexpr uint32_t kLoCarConfigs = 1360;
constexpr uint32_t kLoDirty = 254032;
constexpr uint32_t kCarConfigSize = 8176;
constexpr int kMaxOwnedCars = 30;
// CarConfig vehicle name, char[32]. sub_8238AB50 matches owned cars by it.
constexpr uint32_t kCarConfigName = 8109;
constexpr uint32_t kCarConfigNameLen = 32;

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

// The live config (CustomData) is one block the player keeps across cars: the
// pointer does not change on a car swap, the game copies the next car's
// config into it. The draw loop caches its address for the menus.
std::atomic<uint32_t> g_player_custom{0};
std::atomic<uint32_t> g_player_car{0};   // model data of the player's car, for the log

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

// CarConfig +8100: non-zero when the config belongs to an owned car.
// LeaveGarage (sub_826AA930) only commits the live config to the slot when it
// is set; a showroom or test car falls through to "not owned".
constexpr uint32_t kCarConfigOwned = 8100;

// Copy the mask into the save record of the car the live config belongs to:
// the current owned slot, and only when that slot really holds the same car.
// Same condition LeaveGarage uses, plus the name check, because right after a
// swap the current index already points at the next car while the live config
// still holds the previous one until it streams in (sub_822AAA18).
void StoreInOwnedRecord(uint8_t* base, uint32_t custom, uint8_t mask) {
    if (ReadGuestBE32(base, custom + kCarConfigOwned) == 0) return;

    const int32_t count =
        static_cast<int32_t>(ReadGuestBE32(base, kLocalOptions + kLoOwnedCount));
    const int32_t index =
        static_cast<int32_t>(ReadGuestBE32(base, kLocalOptions + kLoCurrentIndex));
    if (count <= 0 || count > kMaxOwnedCars || index < 0 || index >= count)
        return;

    const uint32_t record =
        kLocalOptions + kLoCarConfigs + kCarConfigSize * uint32_t(index);
    if (record == custom) return;  // the live config is the record itself

    const char* live_name =
        reinterpret_cast<const char*>(base + custom + kCarConfigName);
    const char* slot_name =
        reinterpret_cast<const char*>(base + record + kCarConfigName);
    if (std::strncmp(live_name, slot_name, kCarConfigNameLen) != 0) {
        LARECOMP_APP_INFO("[Carbon] slot {} holds another car, mask left in the live config",
                          index);
        return;
    }

    if (base[record + kCustomCarbonMask] != mask) {
        base[record + kCustomCarbonMask] = mask;
        base[kLocalOptions + kLoDirty] = 1;
    }
}

// Shared predicate for both draw-loop sites. veh = the vehicle being drawn
// (r31 in sub_8235D500).
bool SlotWantsCarbon(uint8_t* base, uint32_t sp, int slot, uint32_t veh) {
    if (slot < 0 || slot >= int(sizeof(kSlotGroup))) return false;
    if (!base[sp + kSpAllowCarbon]) return false;  // caller's a2

    const uint32_t custom = ReadGuestBE32(base, sp + kSpCustomData);
    if (!IsPlausibleGuestPtr(custom)) return false;

    // Remember the player's live config for the menus. The byte itself is
    // left alone: the game loads it from the car's own save record.
    if (veh && veh == ReadGuestBE32(base, kPlayerVehicle)) {
        g_player_custom.store(custom, std::memory_order_relaxed);

        const uint32_t holder = ReadGuestBE32(base, veh + 20);
        const uint32_t car = holder ? ReadGuestBE32(base, holder + 80) : 0;
        if (car && g_player_car.exchange(car, std::memory_order_relaxed) != car) {
            LARECOMP_APP_INFO("[Carbon] car 0x{:08X}, mask 0x{:02X}", car,
                              base[custom + kCustomCarbonMask]);
        }
    }

    const uint8_t mask = base[custom + kCustomCarbonMask];
    if (!mask) return false;

    if (!larecomp::settings::carbon_all_parts.Get())
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

    // Live config for the renderer, owned record for the save.
    base[custom + kCustomCarbonMask] = mask;
    StoreInOwnedRecord(base, custom, mask);
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
    if (!larecomp::settings::carbon_all_parts.Get()) return false;  // let vanilla run

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
    StoreInOwnedRecord(base, refl - 0x40, kPresets[next]);

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
