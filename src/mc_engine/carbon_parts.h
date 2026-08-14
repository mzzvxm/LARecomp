#pragma once

#include <cstdint>
#include <rex/ppc/context.h>

// Extended carbon fiber: lets the carbon technique group apply to any body
// part instead of only the hood. See carbon_parts.cpp for the RE notes.

// Part groups, one bit each in the car's own carbon byte.
enum CarbonGroup : uint8_t {
    kCarbonHood    = 0x01,  // hood, hood_wide
    kCarbonTrunk   = 0x02,  // trunk_swap, trunk_wide
    kCarbonDoors   = 0x04,  // door_l/r, door_l/r_wide
    kCarbonRoof    = 0x08,  // conv_top(up/down) — drops a convertible's top
    kCarbonBumpers = 0x10,  // bumper_f, bumper_r
    kCarbonBody    = 0x20,  // fenders, skirts, widebody
    kCarbonSpoiler = 0x40,  // spoiler, spoiler_b, spoiler_wide
    kCarbonExtras  = 0x80,  // grill, intercooler, blower, bike bodywork
};

// sub_8235D500 part draw loop — force / restore the "carbonfiberhood"
// technique group around the part currently being drawn.
bool Hook_CarbonPartPush(PPCRegister& r1, PPCRegister& r27, PPCRegister& r31);
bool Hook_CarbonPartPop(PPCRegister& r1, PPCRegister& r27, PPCRegister& r31);

// Garage action 147 (sub_826AB520) — the shop's carbon toggle. Dormant: the
// retail menu has no entry that fires it.
bool Hook_CarbonToggle(PPCRegister& r11);

// ── Per-car mask, as seen by the menus ──────────────────────────────────
//
// The mask lives in the car's own customization data (the byte the save
// serialises as "CarbonFiberHood"), so it is per car and persists with the
// save. The draw loop caches the address of the car the player is driving;
// these return false / 0 until a player car has been drawn at least once.

bool CarbonHaveCar();
uint8_t CarbonGetMask();
void CarbonSetMask(uint8_t mask);
bool CarbonHasGroup(uint8_t group);
void CarbonToggleGroup(uint8_t group);

// Preset cycle, for menus that only have room for a single entry (the garage
// paint screen). Walks the same per-car mask the toggles edit.
void CarbonCyclePreset();
const char* CarbonPresetName();

// Preset list, for the garage row: garage menu items are value rows whose
// values are child states (PaintType carries Metallic / Pearl / ...), and the
// value changes as the highlight moves between them.
int CarbonPresetCount();
const char* CarbonPresetNameAt(int index);
uint8_t CarbonPresetMaskAt(int index);
void CarbonApplyPresetIndex(int index);

// Group bit for a part slot (0 = slot never gets carbon).
uint8_t CarbonBitForSlot(int slot);
