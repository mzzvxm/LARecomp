// The garage's showroom lists every vehicle on sale, however large the roster.
//
// The game caps the showroom three times over:
//
// - The rows. The showroom list in tune/ui/garage/Main.sc.xml is built from
//   `<include src="elements/Vehicle.sc" count="70">`: the statechart loader
//   (sub_82692440) reads `count` with sscanf("%d") into a local and parses the
//   included file that many times, one Dynamic child state per pass.
//   mcGarage::ListShowroomVehicles (sub_826A9D58) takes a row per vehicle on sale
//   with sub_8268DB80 and stops when none is left. The hook below runs right
//   after that sscanf and raises the count of this one include to the roster.
//
// - The names. ListShowroomVehicles prints each vehicle's name and class into
//   garage+28 + 168 * the vehicle's ROSTER index (80 + 80 bytes; the row's
//   fs_text1/fs_text2 point there). That pool is scratch shared by every garage
//   list and has room for at most 120 entries - the part list keeps its own
//   field at garage+20268 - so a roster past 120 would write the names over the
//   rest of the garage. Here the listing writes them into a block of its own,
//   sized to the roster: a hook after `addi r15,r14,0x6C` (the pointer the loop
//   walks) moves r15 there.
//
// - The Flash side. garage.xsf builds the garage menu's `_global.menu.elements`
//   as an array of 70 objects, and the movie draws a row's texts from its
//   element: past the 70th row the texts read "0". It also builds
//   `_global.vehicle_ID` and `_global.sh_bigpicture_all` as arrays of 70 (63
//   factory ids and 7 free slots of -1); the game's setter (sub_825ED4A0)
//   refuses an index past the length and the listing puts a mod's id and picture
//   in the first -1 slot it finds, so from the 8th mod on a vehicle has no
//   picture. An AS array refuses any store past its count, so before each
//   listing the three are grown to the roster here, at the array itself: new
//   elements are plain objects as `new Object()` makes them, new slots -1.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "hooks_internal.h"
#include "larecomp_log.h"

REXCVAR_DEFINE_BOOL(showroom_unlimited, true, "MCLA/Patches",
    "The garage's showroom lists every vehicle on sale. Off = the game's own showroom: 70 rows, "
    "and a picture for at most 7 vehicles past the game's own.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REX_EXTERN(__imp__rex_sub_826A9D58);  // mcGarage::ListShowroomVehicles
REX_EXTERN(__imp__rex_sub_8268DB80);  // next free row of a garage list

namespace {

constexpr uint32_t kRosterCount = 0x828CD0AC;  // u16, entries of dword_828CD0A8
constexpr uint32_t kIncludeSrc = 0x82908788;   // the loader's `src` attribute buffer
constexpr uint32_t kCountLocal = 0x54;         // r1 + this: the parsed `count`
constexpr char kShowroomTemplate[] = "elements/vehicle.sc";
constexpr uint32_t kRowsWithoutRoster = 256;   // when the roster is not read yet

constexpr uint32_t kNameEntry = 168;           // name 80 + class 80 + 8
constexpr uint32_t kNameSlack = 64;

// Guest functions of the Flash side.
constexpr uint32_t kMovieTable = 0x8286D804;   // what sub_821F9FB8 looks a movie up in
constexpr uint32_t kFindMovieFn = 0x821F9FB8;  // (table, name) -> movie entry; +0x44 loaded, +0x38 movie
constexpr uint32_t kHashFn = 0x821C9790;       // (string, 0) -> hash
constexpr uint32_t kFindGlobalFn = 0x825ED480; // (movie, hash, name) -> variable slot
constexpr uint32_t kSelectFn = 0x825ED3A8;     // (movie, 1): make its AS context current
constexpr uint32_t kSetElementFn = 0x825ED4A0; // (movie, slot, index, value*): bounded store
constexpr uint32_t kToIntFn = 0x825EF808;      // (value*) -> int
constexpr uint32_t kMallocFn = 0x82130528;     // the game's heap
constexpr uint32_t kFreeFn = 0x82130588;
constexpr uint32_t kGetMemberSlot = 36;        // object vfunc: (key*, out value*)
constexpr uint32_t kSetMemberSlot = 56;        // object vfunc: (key*, value*)
constexpr uint32_t kScriptObjectVtable = 0x8208530C;  // rage::swfSCRIPTOBJECT
constexpr uint32_t kScriptObjectSize = 24;
constexpr uint32_t kValueSize = 12;
constexpr uint32_t kTypeString = 1, kTypeNumber = 3, kTypeObject = 5, kTypeUndefined = 7;

// Scratch for the calls above.
constexpr uint32_t kScratchSize = 0x100;
constexpr uint32_t kKey = 0x00, kValue = 0x10, kOut = 0x20;
constexpr uint32_t kStrLength = 0x40, kStrVehicleId = 0x50, kStrPictures = 0x68, kStrMovie = 0x80;
constexpr uint32_t kStrMenu = 0x90, kStrElements = 0x98, kStrProperty1 = 0xA8, kStrType = 0xB8;

bool Enabled() { return REXCVAR_GET(showroom_unlimited); }

uint32_t RosterCount(const uint8_t* b) {
    return (uint32_t(b[kRosterCount]) << 8) | uint32_t(b[kRosterCount + 1]);
}

uint32_t Alloc(uint32_t size) {
    auto* rt = rex::Runtime::instance();
    return rt && rt->memory() ? rt->memory()->SystemHeapAlloc(size, 0x1000) : 0;
}

uint32_t Call(uint32_t fn_addr, PPCContext& ctx, uint8_t* base,
              std::initializer_list<uint32_t> gpr) {
    auto* rt = rex::Runtime::instance();
    PPCFunc* fn = rt && rt->function_dispatcher() ? rt->function_dispatcher()->GetFunction(fn_addr)
                                                  : nullptr;
    if (!fn) return 0;
    PPCContext c{};
    c.r1 = ctx.r1;
    c.r1.u32 -= 0x70;
    c.r13 = ctx.r13;
    c.fpscr = ctx.fpscr;
    PPCRegister* regs[] = {&c.r3, &c.r4, &c.r5, &c.r6, &c.r7, &c.r8};
    size_t i = 0;
    for (uint32_t v : gpr) regs[i++]->u64 = v;
    fn(c, base);
    ctx.fpscr = c.fpscr;
    return c.r3.u32;
}

// --- rows --------------------------------------------------------------------

std::atomic<bool> g_listing{false};
uint32_t g_rows_taken = 0;
bool g_rows_ran_out = false;
uint32_t g_logged_rows = UINT32_MAX;
bool g_logged_ran_out = false;

bool IsShowroomTemplate(const uint8_t* b) {
    const char* src = reinterpret_cast<const char*>(b + kIncludeSrc);
    const size_t n = sizeof(kShowroomTemplate) - 1;
    for (size_t i = 0; i < n; ++i) {
        char c = src[i];
        if (c == '\\') c = '/';
        if (std::tolower(static_cast<unsigned char>(c)) != kShowroomTemplate[i]) return false;
    }
    return src[n] == '\0';
}

// --- names -------------------------------------------------------------------

std::atomic<uint32_t> g_names{0};  // guest block, kNameEntry per roster vehicle
uint32_t g_names_capacity = 0;
std::atomic<bool> g_names_seen{false};

void EnsureNames(uint8_t* b, uint32_t roster) {
    if (g_names.load() && roster <= g_names_capacity) return;
    const uint32_t want = std::max<uint32_t>(roster + kNameSlack, 256);
    const uint32_t block = Alloc(want * kNameEntry);
    if (!block) {
        LARECOMP_APP_ERROR("[showroom] no guest memory for {} vehicle names", want);
        return;
    }
    std::memset(b + block, 0, size_t(want) * kNameEntry);
    // The old block stays allocated: the rows listed last still point into it.
    g_names.store(block);
    g_names_capacity = want;
    LARECOMP_APP_INFO("[showroom] vehicle names at {:08X}, room for {} (roster {})", block, want,
                      roster);
}

// --- pictures ----------------------------------------------------------------

uint32_t g_scratch = 0;
uint32_t g_logged_ids = UINT32_MAX, g_logged_pictures = UINT32_MAX, g_logged_elements = UINT32_MAX;

void PutString(uint8_t* b, uint32_t at, const char* s) { std::memcpy(b + at, s, std::strlen(s) + 1); }

bool EnsureScratch(uint8_t* b) {
    if (g_scratch) return true;
    g_scratch = Alloc(kScratchSize);
    if (!g_scratch) return false;
    std::memset(b + g_scratch, 0, kScratchSize);
    PutString(b, g_scratch + kStrLength, "length");
    PutString(b, g_scratch + kStrVehicleId, "vehicle_ID");
    PutString(b, g_scratch + kStrPictures, "sh_bigpicture_all");
    PutString(b, g_scratch + kStrMovie, "GARAGEMOVIE");
    PutString(b, g_scratch + kStrMenu, "menu");
    PutString(b, g_scratch + kStrElements, "elements");
    PutString(b, g_scratch + kStrProperty1, "property1");
    PutString(b, g_scratch + kStrType, "type");
    return true;
}

void SetValue(uint8_t* b, uint32_t at, uint32_t v, uint32_t type) {
    WriteGuestU32(b, at, v);
    WriteGuestU32(b, at + 4, 0);
    WriteGuestU32(b, at + 8, type);
    WriteGuestU32(b, at + 12, 0);
}

int32_t ArrayLength(PPCContext& ctx, uint8_t* b, uint32_t object, uint32_t hash_length) {
    const uint32_t s = g_scratch;
    WriteGuestU32(b, s + kKey, s + kStrLength);
    WriteGuestU32(b, s + kKey + 4, hash_length);
    WriteGuestU32(b, s + kKey + 8, kTypeString);
    SetValue(b, s + kOut, 0, kTypeUndefined);
    const uint32_t vt = ReadGuestU32(b, object);
    Call(ReadGuestU32(b, vt + kGetMemberSlot), ctx, b, {object, s + kKey, s + kOut});
    return static_cast<int32_t>(Call(kToIntFn, ctx, b, {s + kOut}));
}

// A new plain AS object, built the way the player builds its own (sub_825FD100):
// 24 bytes from the game's heap, the swfSCRIPTOBJECT vtable, the rest zero.
uint32_t NewScriptObject(PPCContext& ctx, uint8_t* b) {
    const uint32_t object = Call(kMallocFn, ctx, b, {kScriptObjectSize});
    if (!object) return 0;
    std::memset(b + object, 0, kScriptObjectSize);
    WriteGuestU32(b, object, kScriptObjectVtable);
    return object;
}

// Sets a numeric member of an AS object through its own store.
void SetNumberMember(PPCContext& ctx, uint8_t* b, uint32_t object, uint32_t name, int32_t v) {
    const uint32_t s = g_scratch;
    WriteGuestU32(b, s + kKey, name);
    WriteGuestU32(b, s + kKey + 4, Call(kHashFn, ctx, b, {name, 0}));
    WriteGuestU32(b, s + kKey + 8, kTypeString);
    WriteGuestU32(b, s + kKey + 12, 0);
    SetValue(b, s + kValue, static_cast<uint32_t>(v), kTypeNumber);
    Call(ReadGuestU32(b, ReadGuestU32(b, object) + kSetMemberSlot), ctx, b,
         {object, s + kKey, s + kValue});
}

enum class Fill { kMinusOne, kNewElement };

// Grows the AS array `array` to `want` elements. The array (swfSCRIPTARRAY, see
// its constructor sub_825FC6F8) keeps its values at +24, 12 bytes each
// ({data, -, type}), the count at +28 and the room at +30 (u16); its own stores
// refuse an index past the count, so the buffer is replaced here the way the
// constructor allocates it (sub_825FA0E0: the game's heap, every value undefined).
// Returns the length afterwards.
int32_t GrowArray(PPCContext& ctx, uint8_t* b, uint32_t array, uint32_t want, Fill fill,
                  uint32_t hash_length) {
    const int32_t length = ArrayLength(ctx, b, array, hash_length);
    const uint32_t count = (uint32_t(b[array + 28]) << 8) | b[array + 29];
    if (length < 0 || uint32_t(length) != count) {
        LARECOMP_APP_WARN("[showroom] an array's length ({}) is not its count ({}), left as it is",
                          length, count);
        return length;
    }
    if (want <= count) return length;
    if (want > 0xFFFF) want = 0xFFFF;
    const uint32_t old_values = ReadGuestU32(b, array + 24);
    const uint32_t values = Call(kMallocFn, ctx, b, {want * kValueSize});
    if (!values) return length;
    if (count) std::memcpy(b + values, b + old_values, size_t(count) * kValueSize);
    for (uint32_t i = count; i < want; ++i) {
        const uint32_t at = values + i * kValueSize;
        WriteGuestU32(b, at + 4, 0);
        if (fill == Fill::kMinusOne) {
            WriteGuestU32(b, at, static_cast<uint32_t>(-1));
            WriteGuestU32(b, at + 8, kTypeNumber);
            continue;
        }
        const uint32_t element = NewScriptObject(ctx, b);
        WriteGuestU32(b, at, element);
        WriteGuestU32(b, at + 8, element ? kTypeObject : kTypeUndefined);
        if (element) {
            // What garage.xsf's InitData gives every element of the menu.
            SetNumberMember(ctx, b, element, g_scratch + kStrProperty1, 1);
            SetNumberMember(ctx, b, element, g_scratch + kStrType, 0);
        }
    }
    WriteGuestU32(b, array + 24, values);
    b[array + 28] = uint8_t(want >> 8);
    b[array + 29] = uint8_t(want);
    b[array + 30] = uint8_t(want >> 8);
    b[array + 31] = uint8_t(want);
    if (old_values) Call(kFreeFn, ctx, b, {old_values});
    return ArrayLength(ctx, b, array, hash_length);
}

// The object a global variable of `movie` holds, 0 when it holds none.
uint32_t GlobalObject(PPCContext& ctx, uint8_t* b, uint32_t movie, uint32_t name,
                      uint32_t* slot_out = nullptr) {
    const uint32_t hash = Call(kHashFn, ctx, b, {name, 0});
    const uint32_t slot = Call(kFindGlobalFn, ctx, b, {movie, hash, name});
    if (slot_out) *slot_out = slot;
    if (!slot || ReadGuestU32(b, slot + 8) != kTypeObject) return 0;
    return ReadGuestU32(b, slot);
}

// The object member `name` of `object` holds, 0 when none.
uint32_t MemberObject(PPCContext& ctx, uint8_t* b, uint32_t object, uint32_t name) {
    const uint32_t s = g_scratch;
    WriteGuestU32(b, s + kKey, name);
    WriteGuestU32(b, s + kKey + 4, Call(kHashFn, ctx, b, {name, 0}));
    WriteGuestU32(b, s + kKey + 8, kTypeString);
    WriteGuestU32(b, s + kKey + 12, 0);
    SetValue(b, s + kOut, 0, kTypeUndefined);
    Call(ReadGuestU32(b, ReadGuestU32(b, object) + kGetMemberSlot), ctx, b,
         {object, s + kKey, s + kOut});
    return ReadGuestU32(b, s + kOut + 8) == kTypeObject ? ReadGuestU32(b, s + kOut) : 0;
}

void GrowFlashArrays(PPCContext& ctx, uint8_t* b, uint32_t roster) {
    if (!EnsureScratch(b)) return;
    const uint32_t s = g_scratch;
    const uint32_t entry = Call(kFindMovieFn, ctx, b, {ReadGuestU32(b, kMovieTable), s + kStrMovie});
    if (!entry || !b[entry + 0x44]) return;
    const uint32_t movie = ReadGuestU32(b, entry + 0x38);
    if (!movie) return;
    const uint32_t hash_length = Call(kHashFn, ctx, b, {s + kStrLength, 0});

    Call(kSelectFn, ctx, b, {movie, 1});
    uint32_t slot = 0;
    int32_t ids = -1, pictures = -1, elements = -1;
    if (const uint32_t a = GlobalObject(ctx, b, movie, s + kStrVehicleId, &slot))
        ids = GrowArray(ctx, b, a, roster, Fill::kMinusOne, hash_length);
    if (const uint32_t a = GlobalObject(ctx, b, movie, s + kStrPictures))
        pictures = GrowArray(ctx, b, a, roster, Fill::kMinusOne, hash_length);
    if (const uint32_t menu = GlobalObject(ctx, b, movie, s + kStrMenu))
        if (const uint32_t a = MemberObject(ctx, b, menu, s + kStrElements))
            elements = GrowArray(ctx, b, a, roster, Fill::kNewElement, hash_length);
    // A store the game's setter refuses (index -1): it changes nothing but puts
    // the AS context back the way the game's own stores leave it.
    if (slot) {
        SetValue(b, s + kValue, static_cast<uint32_t>(-1), kTypeNumber);
        Call(kSetElementFn, ctx, b, {movie, slot, static_cast<uint32_t>(-1), s + kValue});
    }

    if (uint32_t(ids) != g_logged_ids || uint32_t(pictures) != g_logged_pictures ||
        uint32_t(elements) != g_logged_elements) {
        g_logged_ids = uint32_t(ids);
        g_logged_pictures = uint32_t(pictures);
        g_logged_elements = uint32_t(elements);
        const int32_t r = static_cast<int32_t>(roster);
        if (ids < r || pictures < r || elements < r)
            LARECOMP_APP_WARN("[showroom] Flash arrays short of the roster of {}: vehicle_ID {}, "
                              "sh_bigpicture_all {}, menu.elements {}", roster, ids, pictures,
                              elements);
        else
            LARECOMP_APP_INFO("[showroom] Flash arrays: vehicle_ID {}, sh_bigpicture_all {}, "
                              "menu.elements {} (roster {})", ids, pictures, elements, roster);
    }
}

}  // namespace

// Right after the loader parsed an <include>'s count (sub_82692440, both paths
// meet here): the showroom's template gets a row per roster vehicle.
void ShowroomRows_8269381C(PPCRegister& r1) {
    if (!Enabled()) return;
    auto* rt = rex::Runtime::instance();
    uint8_t* b = rt ? rt->virtual_membase() : nullptr;
    if (!b || !IsShowroomTemplate(b)) return;
    const uint32_t at = r1.u32 + kCountLocal;
    const int32_t count = static_cast<int32_t>(ReadGuestU32(b, at));
    const uint32_t roster = RosterCount(b);
    const uint32_t want = roster ? roster : kRowsWithoutRoster;
    if (count < static_cast<int32_t>(want)) WriteGuestU32(b, at, want);
    LARECOMP_APP_INFO("[showroom] list rows: {} -> {} (roster {} when the menu loaded)", count,
                      std::max<int32_t>(count, int32_t(want)), roster);
}

// The pointer ListShowroomVehicles walks through the name pool (garage+28+80,
// +168 per roster vehicle) moves to the block of its own.
void ShowroomNames_826A9EA8(PPCRegister& r15) {
    const uint32_t names = g_names.load(std::memory_order_relaxed);
    if (!names || !g_listing.load(std::memory_order_relaxed)) return;
    r15.u64 = names + 80;
    if (!g_names_seen.exchange(true, std::memory_order_relaxed))
        LARECOMP_APP_INFO("[showroom] names listed into the block of their own");
}

extern "C" REX_FUNC(rex_sub_8268DB80) {
    __imp__rex_sub_8268DB80(ctx, base);
    if (g_listing.load(std::memory_order_relaxed)) {
        if (ctx.r3.u32)
            ++g_rows_taken;
        else
            g_rows_ran_out = true;
    }
}

extern "C" REX_FUNC(rex_sub_826A9D58) {
    const uint32_t roster = RosterCount(base);
    if (Enabled()) {
        EnsureNames(base, roster);
        GrowFlashArrays(ctx, base, roster);
    }
    g_rows_taken = 0;
    g_rows_ran_out = false;
    g_listing.store(true, std::memory_order_relaxed);
    __imp__rex_sub_826A9D58(ctx, base);
    g_listing.store(false, std::memory_order_relaxed);
    if (g_rows_taken != g_logged_rows || g_rows_ran_out != g_logged_ran_out) {
        g_logged_rows = g_rows_taken;
        g_logged_ran_out = g_rows_ran_out;
        if (g_rows_ran_out)
            LARECOMP_APP_WARN("[showroom] listed {} vehicles and ran out of list rows; the rest "
                              "of the roster on sale is not shown (roster {})", g_rows_taken, roster);
        else
            LARECOMP_APP_INFO("[showroom] listed {} vehicles (roster {})", g_rows_taken, roster);
    }
}

#endif  // REXGLUE_HAS_XEO3_TARGET
