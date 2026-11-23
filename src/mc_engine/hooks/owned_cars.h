#pragma once
//
// The owned-car arrays of LocalOptions moved to a block of their own
// (owned_cars.cpp). The reset, new-game, pre-save, serialize and load paths of
// LocalOptions are already wrapped in vehicle_records.cpp, which calls these:
//   rex_sub_8238C9B0 (reset)      AfterReset         after __imp__ and the records' reset
//   rex_sub_8238C278 (new game)   AfterNewGame       after __imp__
//   rex_sub_8238B3F8 (pre-save)   BeforeProfileCopy  after __imp__ and the records' copy
//   rex_sub_822D20D8 (serialize)  AfterSerialize     after __imp__ (buffer r4, room r5)
//   rex_sub_822D2258 (load)       BeforeLoad         before __imp__,
//                                 AfterLoad          after __imp__ and the records' restore
// `parts_room` is how many records the parts table has room for: the game
// reads that table with the owned slot as index, so no slot may lie past it.
// All of them do nothing while owned_cars_limit is 30.

#include <cstdint>

struct PPCContext;

namespace owned_cars {

// The game's own limit, and the most the slot can be: sub_8238BA38 keeps the
// current slot in a byte.
inline constexpr uint32_t kGameCap = 30;
inline constexpr uint32_t kMaxCap = 255;

// Owned cars the game may keep: 30 with the option off.
uint32_t Cap();
// True once the cars live in the block of their own (Cap() above 30 and the
// block made).
bool Enabled();
// Guest address of owned slot `slot`'s CarConfig, for host code that reads or
// writes an owned car directly: the profile's own slot while the cars are not
// moved (any index, as the game would form it), the block's slot once they
// are, 0 for a slot past Cap() then.
uint32_t CarAddress(uint32_t slot);

void AfterReset(uint8_t* base, uint32_t parts_room);
void AfterNewGame(uint8_t* base);
void BeforeProfileCopy(uint8_t* base);
void AfterSerialize(uint8_t* base, uint32_t buffer, uint32_t room);
void BeforeLoad(uint8_t* base);
void AfterLoad(uint8_t* base, uint32_t parts_room);

}  // namespace owned_cars

// owned_cars_ui.cpp, called from ShowroomRows_8269381C (showroom.cpp) right
// after the statechart loader parsed an <include>'s count into `count_addr`;
// true when the include was one of the owned-vehicle lists.
bool OwnedCarsUi_RaiseIncludeCount(uint8_t* base, uint32_t src_addr, uint32_t count_addr);

// showroom.cpp: grows garage.xsf's `_global.menu.elements` to `want` elements.
// Returns the length afterwards, -1 when the movie or the array is not there.
int32_t ShowroomGrowMenuElements(PPCContext& ctx, uint8_t* base, uint32_t want);
