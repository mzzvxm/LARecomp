// The lists of owned vehicles get a row per vehicle the save can hold.
//
// Three native lists show the owned vehicles, each built from a fixed number of
// rows:
//
// - The garage's "manage ride" list, tune/ui/garage/Main.sc.xml:
//   `<include src="elements/VehicleManageRide.sc" count="40">`, filled by
//   garage.ListOwnedVehicles (sub_826A7170).
// - Rate My Ride's "choose ride", tune/ui/garage/rmr/Main.sc.xml:
//   `<include src="Vehicle.sc" count="40">`, filled by the same sub_826A7170.
//   (The same file also includes Vehicle.sc with count="10" for the
//   leaderboard; that one is left alone.)
// - The pause menu's "change ride", tune/ui/ChangeRide.sc.xml:
//   `<include src="garage\elements\Ride.sc" count ="32">`, filled by
//   net.ListOwnedVehicles (sub_8228B018).
//
// Both fill loops walk the owned slots 0..count-1, take one row per slot with
// sub_8268DB80 and stop when no row is left, so a vehicle past the last row is
// neither shown nor selectable (the row index is the slot). The statechart
// loader (sub_82692440) parses an include `count` times; the showroom hook that
// runs right after it parsed the count (ShowroomRows_8269381C) calls
// OwnedCarsUi_RaiseIncludeCount, which raises these three to the owned-car cap.
//
// sub_826A7170 also prints each vehicle's name and class into the garage's name
// pool, garage+28 + 168 * slot (r21 walks garage+108), which has room for 120
// entries, and sets `_global.menu.elements[slot].ID` in garage.xsf, an array of
// 70. Past 70 slots the elements are grown first (the showroom's grower), past
// 120 the pool moves to a block of its own sized to the cap.
//
// Nothing here acts while the owned cars are not moved (owned_cars_limit 30).

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/ppc.h>
#include <rex/runtime.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>

#include "hooks_internal.h"
#include "larecomp_log.h"
#include "owned_cars.h"

REX_EXTERN(__imp__rex_sub_826A7170);  // mcGarage::ListOwnedVehicles

namespace {

constexpr uint32_t kNameEntry = 168;   // name 80 + class 80 + 8
constexpr uint32_t kPoolEntries = 120; // garage+28 .. garage+20268
constexpr uint32_t kFlashElements = 70;

struct OwnedList {
    const char* src;     // the include's src, lower case, '/' for '\'
    int32_t game_count;  // the count the game's file gives it
    const char* what;
};

constexpr OwnedList kLists[] = {
    {"elements/vehiclemanageride.sc", 40, "garage"},
    {"vehicle.sc", 40, "rate my ride"},
    {"garage/elements/ride.sc", 32, "pause menu"},
};

// Rows each owned list needs: 0 while the owned cars are where the game keeps them.
uint32_t Rows() {
    if (!owned_cars::Enabled()) return 0;
    return owned_cars::Cap();
}

bool SrcIs(const uint8_t* b, uint32_t src_addr, const char* want) {
    const char* src = reinterpret_cast<const char*>(b + src_addr);
    size_t i = 0;
    for (; want[i]; ++i) {
        char c = src[i];
        if (c == '\\') c = '/';
        if (std::tolower(static_cast<unsigned char>(c)) != want[i]) return false;
    }
    return src[i] == '\0';
}

// --- names -------------------------------------------------------------------

std::atomic<uint32_t> g_names{0};  // guest block, kNameEntry per owned slot
std::atomic<bool> g_names_seen{false};

void EnsureNames(uint8_t* b, uint32_t rows) {
    if (g_names.load() || rows <= kPoolEntries) return;
    auto* rt = rex::Runtime::instance();
    const uint32_t block =
        rt && rt->memory() ? rt->memory()->SystemHeapAlloc(rows * kNameEntry, 0x1000) : 0;
    if (!block) {
        LARECOMP_APP_ERROR("[owned-cars-ui] no guest memory for {} vehicle names", rows);
        return;
    }
    std::memset(b + block, 0, size_t(rows) * kNameEntry);
    g_names.store(block);
    LARECOMP_APP_INFO("[owned-cars-ui] owned vehicle names at {:08X}, room for {}", block, rows);
}

uint32_t g_logged_elements = UINT32_MAX;

}  // namespace

bool OwnedCarsUi_RaiseIncludeCount(uint8_t* b, uint32_t src_addr, uint32_t count_addr) {
    const uint32_t rows = Rows();
    if (!b || rows <= owned_cars::kGameCap) return false;
    const int32_t count = static_cast<int32_t>(ReadGuestU32(b, count_addr));
    for (const OwnedList& list : kLists) {
        if (count != list.game_count || !SrcIs(b, src_addr, list.src)) continue;
        if (count < static_cast<int32_t>(rows)) WriteGuestU32(b, count_addr, rows);
        LARECOMP_APP_INFO("[owned-cars-ui] {} list rows: {} -> {}", list.what, count,
                          std::max<int32_t>(count, int32_t(rows)));
        return true;
    }
    return false;
}

// `addi r21,r28,108` in ListOwnedVehicles: the pointer into the garage's name
// pool that the loop walks (+168 per slot) moves to the block of its own.
void OwnedCarsUi_826A72E8(PPCRegister& r21) {
    const uint32_t names = g_names.load(std::memory_order_relaxed);
    if (!names) return;
    r21.u64 = names + 80;
    if (!g_names_seen.exchange(true, std::memory_order_relaxed))
        LARECOMP_APP_INFO("[owned-cars-ui] owned vehicle names listed into the block of their own");
}

extern "C" REX_FUNC(rex_sub_826A7170) {
    const uint32_t rows = Rows();
    if (rows > kFlashElements) {
        EnsureNames(base, rows);
        const int32_t elements = ShowroomGrowMenuElements(ctx, base, rows);
        if (uint32_t(elements) != g_logged_elements) {
            g_logged_elements = uint32_t(elements);
            if (elements < static_cast<int32_t>(rows))
                LARECOMP_APP_WARN("[owned-cars-ui] menu.elements is {}, short of {} owned rows",
                                  elements, rows);
            else
                LARECOMP_APP_INFO("[owned-cars-ui] menu.elements {} for {} owned rows", elements,
                                  rows);
        }
    }
    __imp__rex_sub_826A7170(ctx, base);
}

#else  // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.
#include <rex/ppc/context.h>

#include <cstdint>

bool OwnedCarsUi_RaiseIncludeCount(uint8_t*, uint32_t, uint32_t) { return false; }
void OwnedCarsUi_826A72E8(PPCRegister&) {}

#endif  // REXGLUE_HAS_XEO3_TARGET
