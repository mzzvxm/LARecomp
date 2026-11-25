// Host-side MaxPlayers above 16 through the game's own "host a race" menu.
//
// How the game does it (IDB):
//   sub_8227D3B0 returns the rage parameter "netMaxPlayers" (default 16).
//   sub_8265A4A0 fills the MaxPlayers spinner (string list at menu+1088, row at
//       menu+3568) with off_82826DAC = {"2","4",...,"16"} and count
//       netMaxPlayers/2 (call through vtbl+24 at 0x8265A690).
//   sub_826583C0 @0x826586DC..0x82658700 sets the initial index to
//       min(netMaxPlayers, GamePolicies.DefaultMaxPlayers)/2 - 1.
//   sub_82656A88 fills the private-slots list (menu+1072) into the static
//       10-entry buffer 0x82906D10: "Public Event", off_82826DAC[0..idx-1],
//       "Private Event" (idx = MaxPlayers index).
//   sub_8265DBF8 (confirm) writes the session settings table
//       *(*(dword_8286D800+4304)): +680 MaxPlayers = 2*(idx+1) @0x8265DEA8 and
//       +756 NumPrivateSlots = 2*private_idx @0x8265DFB0.
//
// With net_max_players = N the spinner offers 2,4,...,2*floor(N/2) and N itself
// when N is odd: index i means min(2*(i+1), N). Every place that turns an index
// into a value or a value into an index is adjusted to that rule; the string
// tables live in guest memory owned by this file and are never read past their
// end. N = 16 leaves every hook a no-op.
//
// Drawing the two lists. Both rows are of the value-row class whose render is
// sub_82631A20: it takes the row's Flash element (sub_82630158:
// `_global.menu.elements[menu.count++]`), then writes `name` into
// `element.elements[i]` for every value of the row's string list, each name
// being the string-table lookup of the value's key (sub_82218310). It never
// checks how many elements the clip has. The menu movies build every
// `_global.menu.elements[i].elements` as an array of 11 objects (64 in the
// garage, 25 in the race editor), and the array's element read (vtable slot
// 80, sub_825FBAE8) is `values + 12 * index` without a bound, so a list of 12
// values or more reads past the array's buffer; a value there that is not an
// object makes sub_827227B8 call through guest 0x0. Above 16
// the spinner has up to 16 values and the private list up to 17, so here the
// row's element array is grown to the row's value count first, with plain
// objects the way the movie's own `new Object()` makes them.
//
// The string table has keys "2".."16" but not "18", "22", "24", ... (the
// lookup falls back to "!!key!!"), so values above 16 show the number itself:
// right after the lookup a value string of this file's table takes the place
// of the looked-up text.

#include "max_players.h"

#ifndef REXGLUE_HAS_XEO3_TARGET
#include "settings/larecomp_settings.h"
#include <rex/ppc/recompiled_code.h>
#include <rex/ppc/context.h>
#include "larecomp_guest.h"
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/function_dispatcher.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>

#include "larecomp_log.h"

REX_EXTERN(__imp__rex_sub_82631A20);  // value row: writes its values into its Flash element
REX_EXTERN(__imp__rex_sub_82630158);  // the next `_global.menu.elements` entry of the movie

namespace {

constexpr int kGameMax = 16;
constexpr int kHardMax = 31;
constexpr uint32_t kSpinnerSlots = 16;   // ceil(31 / 2)
constexpr uint32_t kPrivateSlots = 32;   // "Public Event" + 15 values + "Private Event", padded

uint32_t g_value_table = 0;    // guest: kSpinnerSlots pointers to the value strings
uint32_t g_value_strings = 0;  // guest: the value strings, 8 bytes each
uint32_t g_private_buf = 0;    // guest: kPrivateSlots pointers, replaces 0x82906D10
std::once_flag g_once;

// The value row (class of sub_82631A20) and its string list.
constexpr uint32_t kRowValues = 200;           // string list, 0 = the row's children
constexpr uint32_t kRowChildCount = 192;       // u16, used when +200 is 0
constexpr uint32_t kStrListVtable = 0x82091D04;  // {vtbl, char** keys, count, stride}
constexpr uint32_t kStrListKeys = 4, kStrListCount = 8, kStrListStride = 12;

// The Flash side (same calls and layouts the showroom uses, showroom.cpp).
constexpr uint32_t kMovieRowElements = 11;     // `menu.elements[i].elements` in the menu movies
constexpr uint32_t kMaxRowElements = 64;
constexpr uint32_t kHashFn = 0x821C9790;       // (string, 0) -> hash
constexpr uint32_t kToIntFn = 0x825EF808;      // (value*) -> int
constexpr uint32_t kMallocFn = 0x82130528;     // the game's heap
constexpr uint32_t kFreeFn = 0x82130588;
constexpr uint32_t kGetMemberSlot = 36;        // object vfunc: (key*, out value*)
constexpr uint32_t kScriptObjectVtable = 0x8208530C;  // rage::swfSCRIPTOBJECT
constexpr uint32_t kScriptObjectSize = 24;
// rage::swfSCRIPTARRAY: slot 56 is the bounded store sub_825FBB00, slot 80 the
// unchecked `values + 12 * index` sub_825FBAE8 that sub_82631A20 reads with.
constexpr uint32_t kScriptArrayVtable = 0x82087474;
constexpr uint32_t kValueSize = 12;            // {data, -, type}
constexpr uint32_t kTypeString = 1, kTypeObject = 5, kTypeUndefined = 7;
constexpr uint32_t kArrayValues = 24, kArrayCount = 28, kArrayRoom = 30;  // swfSCRIPTARRAY

// Scratch for the calls above.
constexpr uint32_t kScratchSize = 0x80;
constexpr uint32_t kKey = 0x00, kOut = 0x10, kStrElements = 0x40, kStrLength = 0x50;
uint32_t g_scratch = 0;
uint32_t g_hash_elements = 0, g_hash_length = 0;
uint32_t g_logged_grow = 0;
bool g_logged_mismatch = false;

// Values of the row sub_82631A20 is drawing, set only while it runs and only
// when the row has more values than a menu movie's element array.
thread_local uint32_t g_row_need = 0;

inline uint8_t* Base() { return larecomp::guest::Membase(); }

inline uint32_t Rd32(const uint8_t* b, uint32_t a) {
    return (uint32_t(b[a]) << 24) | (uint32_t(b[a + 1]) << 16) | (uint32_t(b[a + 2]) << 8) |
           uint32_t(b[a + 3]);
}
inline uint32_t Rd16(const uint8_t* b, uint32_t a) { return (uint32_t(b[a]) << 8) | b[a + 1]; }
inline void Wr32(uint8_t* b, uint32_t a, uint32_t v) {
    b[a] = uint8_t(v >> 24); b[a + 1] = uint8_t(v >> 16); b[a + 2] = uint8_t(v >> 8);
    b[a + 3] = uint8_t(v);
}
inline void Wr16(uint8_t* b, uint32_t a, uint32_t v) { b[a] = uint8_t(v >> 8); b[a + 1] = uint8_t(v); }

int SpinnerCount(int n) { return (n + 1) / 2; }
int IndexToValue(int i, int n) { return std::min(2 * (i + 1), n); }

// Builds the value strings once: entry i = IndexToValue(i, N), 8 bytes each.
bool EnsureTables() {
    std::call_once(g_once, [] {
        auto* memory = larecomp::guest::AddressSpace();
        if (!memory) return;
        const uint32_t ptr_bytes = kSpinnerSlots * 4;
        const uint32_t str_bytes = kSpinnerSlots * 8;
        const uint32_t priv_bytes = kPrivateSlots * 4;
        const uint32_t block =
            memory->SystemHeapAlloc(ptr_bytes + str_bytes + priv_bytes, 0x1000);
        if (!block) {
            LARECOMP_APP_ERROR("[max-players] no guest memory for the spinner tables");
            return;
        }
        uint8_t* b = Base();
        std::memset(b + block, 0, ptr_bytes + str_bytes + priv_bytes);
        const int n = net_limits::MaxPlayers();
        const int count = SpinnerCount(n);
        for (int i = 0; i < int(kSpinnerSlots); ++i) {
            // Slots past count repeat the last value; the game never indexes them.
            const int v = IndexToValue(std::min(i, count - 1), n);
            const uint32_t s = block + ptr_bytes + uint32_t(i) * 8;
            std::snprintf(reinterpret_cast<char*>(b + s), 8, "%d", v);
            Wr32(b, block + uint32_t(i) * 4, s);
        }
        g_value_strings = block + ptr_bytes;
        g_private_buf = block + ptr_bytes + str_bytes;
        g_value_table = block;
        LARECOMP_APP_INFO("[max-players] host menu offers up to {} players ({} entries)", n,
                          count);
    });
    return g_value_table != 0;
}

// The number a value string of this file's table holds, 0 for any other string.
int OwnValue(const uint8_t* b, uint32_t s) {
    if (!g_value_table || s < g_value_strings || s >= g_value_strings + kSpinnerSlots * 8)
        return 0;
    int v = 0;
    for (const char* p = reinterpret_cast<const char*>(b + s); *p >= '0' && *p <= '9'; ++p)
        v = v * 10 + (*p - '0');
    return v;
}

// How many values sub_82631A20 writes for this row: the string list's count, or
// the row's child count when it has no list. 0 for a list of another class.
uint32_t RowValueCount(const uint8_t* b, uint32_t row) {
    const uint32_t list = Rd32(b, row + kRowValues);
    if (!list) return Rd16(b, row + kRowChildCount);
    if (Rd32(b, list) != kStrListVtable) return 0;
    const int32_t n = static_cast<int32_t>(Rd32(b, list + kStrListCount));
    return n > 0 ? uint32_t(n) : 0;
}

uint32_t Call(uint32_t fn_addr, PPCContext&, uint8_t* base,
              std::initializer_list<uint32_t> gpr) {
    auto* rt = REX_RUNTIME();
    PPCFunc* fn = rt ? rt->FindFunction(fn_addr).value_or(nullptr) : nullptr;
    if (!fn) return 0;
    larecomp::guest::ThreadFile file;
    if (!file) return 0;
    PPCContext& c = file.ctx();
    c.r1.u32 -= 0x70;
    PPCRegister* regs[] = {&c.r3, &c.r4, &c.r5, &c.r6, &c.r7, &c.r8};
    size_t i = 0;
    for (uint32_t v : gpr) regs[i++]->u64 = v;
    fn(c, base);
    return c.r3.u32;
}

bool EnsureScratch(PPCContext& ctx, uint8_t* b) {
    if (g_scratch) return true;
    auto* memory = larecomp::guest::AddressSpace();
    const uint32_t s = memory ? memory->SystemHeapAlloc(kScratchSize, 0x1000) : 0;
    if (!s) return false;
    std::memset(b + s, 0, kScratchSize);
    std::memcpy(b + s + kStrElements, "elements", 9);
    std::memcpy(b + s + kStrLength, "length", 7);
    g_hash_elements = Call(kHashFn, ctx, b, {s + kStrElements, 0});
    g_hash_length = Call(kHashFn, ctx, b, {s + kStrLength, 0});
    g_scratch = s;
    return true;
}

// object[name] through the object's own GetMember; the value lands at scratch+kOut.
void GetMember(PPCContext& ctx, uint8_t* b, uint32_t object, uint32_t name, uint32_t hash) {
    const uint32_t s = g_scratch;
    Wr32(b, s + kKey, name);
    Wr32(b, s + kKey + 4, hash);
    Wr32(b, s + kKey + 8, kTypeString);
    Wr32(b, s + kKey + 12, 0);
    Wr32(b, s + kOut, 0);
    Wr32(b, s + kOut + 4, 0);
    Wr32(b, s + kOut + 8, kTypeUndefined);
    Wr32(b, s + kOut + 12, 0);
    Call(Rd32(b, Rd32(b, object) + kGetMemberSlot), ctx, b, {object, s + kKey, s + kOut});
}

// Grows `element.elements` of the row's Flash element (the value* sub_82630158
// returned) to `want` plain objects. The array keeps its values at +24, 12
// bytes each, the count at +28 and the room at +30 (u16) and its own stores
// refuse an index past the count, so the buffer is replaced from the game's
// heap the way the showroom grows garage.xsf's arrays.
void GrowRowElements(PPCContext& ctx, uint8_t* b, uint32_t element_value, uint32_t want) {
    if (!element_value || Rd32(b, element_value + 8) != kTypeObject) return;
    const uint32_t element = Rd32(b, element_value);
    if (!element || !EnsureScratch(ctx, b)) return;
    const uint32_t s = g_scratch;
    GetMember(ctx, b, element, s + kStrElements, g_hash_elements);
    if (Rd32(b, s + kOut + 8) != kTypeObject) return;
    const uint32_t array = Rd32(b, s + kOut);
    if (!array || Rd32(b, array) != kScriptArrayVtable) return;
    const uint32_t count = Rd16(b, array + kArrayCount);
    want = std::min(want, kMaxRowElements);
    if (want <= count) return;

    GetMember(ctx, b, array, s + kStrLength, g_hash_length);
    const int32_t length = static_cast<int32_t>(Call(kToIntFn, ctx, b, {s + kOut}));
    if (length < 0 || uint32_t(length) != count) {
        if (!g_logged_mismatch) {
            g_logged_mismatch = true;
            LARECOMP_APP_WARN("[max-players] a row's Flash elements: length {} is not count {}, "
                              "left as it is", length, count);
        }
        return;
    }
    const uint32_t old_values = Rd32(b, array + kArrayValues);
    const uint32_t values = Call(kMallocFn, ctx, b, {want * kValueSize});
    if (!values) return;
    if (count) std::memcpy(b + values, b + old_values, size_t(count) * kValueSize);
    for (uint32_t i = count; i < want; ++i) {
        const uint32_t at = values + i * kValueSize;
        const uint32_t object = Call(kMallocFn, ctx, b, {kScriptObjectSize});
        if (object) {
            std::memset(b + object, 0, kScriptObjectSize);
            Wr32(b, object, kScriptObjectVtable);
        }
        Wr32(b, at, object);
        Wr32(b, at + 4, 0);
        Wr32(b, at + 8, object ? kTypeObject : kTypeUndefined);
    }
    Wr32(b, array + kArrayValues, values);
    Wr16(b, array + kArrayCount, want);
    Wr16(b, array + kArrayRoom, want);
    if (old_values) Call(kFreeFn, ctx, b, {old_values});
    if (want > g_logged_grow) {
        g_logged_grow = want;
        LARECOMP_APP_INFO("[max-players] a value row's Flash elements grown {} -> {}", count, want);
    }
}

}  // namespace

namespace net_limits {
int MaxPlayers() {
    const int v = larecomp::settings::net_max_players.Get();
    return std::clamp(v, 2, kHardMax);
}
bool MaxPlayersChanged() { return MaxPlayers() != kGameMax; }
}  // namespace net_limits

// 0x8227D3D4 (after lwz r3,80(r1)) in sub_8227D3B0: r3 = netMaxPlayers.
void MaxPlayers_8227D3D4(PPCRegister& r3) {
    if (!net_limits::MaxPlayersChanged()) return;
    r3.u64 = uint64_t(net_limits::MaxPlayers());
}

// 0x8265A68C (before mtctr) in sub_8265A4A0: r4 = off_82826DAC, r5 = r29/2,
// r29 = netMaxPlayers (already capped to 8 for the 8-player mode).
void MaxPlayers_8265A68C(PPCRegister& r4, PPCRegister& r5, PPCRegister& r29) {
    if (!net_limits::MaxPlayersChanged() || !EnsureTables()) return;
    const int cap = std::clamp(int(r29.s32), 2, net_limits::MaxPlayers());
    r4.u64 = g_value_table;
    r5.u64 = uint64_t(SpinnerCount(cap));
}

// 0x826586F4 (before rlwinm r11,r3,31,1,31) in sub_826583C0: r3 = initial
// MaxPlayers value; the index becomes ceil(r3/2) - 1 so an odd value finds
// its own entry.
void MaxPlayers_826586F4(PPCRegister& r3) {
    if (!net_limits::MaxPlayersChanged()) return;
    if (r3.u32 & 1) r3.u64 = uint64_t(r3.u32 + 1);
}

// 0x82656AA8 (after addi r4,r11,27920) in sub_82656A88: r4 = 0x82906D10, the
// 10-entry private-slots buffer; the list can now hold up to 17 entries.
void MaxPlayers_82656AA8(PPCRegister& r4) {
    if (!net_limits::MaxPlayersChanged() || !EnsureTables()) return;
    r4.u64 = g_private_buf;
}

// 0x82656AC0 (after addi r10,r10,28076) in sub_82656A88: r10 = off_82826DAC,
// the values copied between "Public Event" and "Private Event".
void MaxPlayers_82656AC0(PPCRegister& r10) {
    if (!net_limits::MaxPlayersChanged() || !EnsureTables()) return;
    r10.u64 = g_value_table;
}

// 0x8265DEA4 (after rlwinm r8,r9,1,0,30) in sub_8265DBF8: r8 = 2*(idx+1), the
// MaxPlayers stored at settings+680.
void MaxPlayers_8265DEA4(PPCRegister& r8) {
    if (!net_limits::MaxPlayersChanged()) return;
    r8.u64 = uint64_t(std::min<int>(r8.s32, net_limits::MaxPlayers()));
}

// 0x8265DFB0 (before stw r7,756(r9)) in sub_8265DBF8: r7 = 2*private_idx, the
// NumPrivateSlots; "Private Event" with an odd MaxPlayers would give N+1.
void MaxPlayers_8265DFB0(PPCRegister& r7, PPCRegister& r9) {
    if (!net_limits::MaxPlayersChanged()) return;
    const int max_players = int(Rd32(Base(), r9.u32 + 680));
    if (r7.s32 > max_players) r7.u64 = uint64_t(max_players);
}

// 0x822859BC (before cmpwi r11,16) in sub_822855D8: r11 = finishing position;
// above 16 the game prints RACE_RESULT_DNF. Positions up to MaxPlayers take the
// "%d" + "th" path instead (the game already turns 32+ into 32 = DNF).
void MaxPlayers_822859BC(PPCRegister& r11) {
    if (!net_limits::MaxPlayersChanged()) return;
    if (r11.s32 > kGameMax && r11.s32 <= net_limits::MaxPlayers()) r11.u64 = kGameMax;
}

// 0x82631B1C (before mr r5,r3) in sub_82631A20: r3 = the text sub_82218310
// looked up for value r31 of row r30, keys[stride * r31] of the row's string
// list. A value string of this file's table above 16 has no string-table key,
// so the row shows the number itself.
void MaxPlayers_82631B1C(PPCRegister& r3, PPCRegister& r30, PPCRegister& r31) {
    if (!net_limits::MaxPlayersChanged() || !g_value_table) return;
    const uint8_t* b = Base();
    const uint32_t list = Rd32(b, r30.u32 + kRowValues);
    if (!list || Rd32(b, list) != kStrListVtable) return;
    const uint32_t keys = Rd32(b, list + kStrListKeys);
    const uint32_t stride = Rd32(b, list + kStrListStride);
    if (!keys) return;
    const uint32_t key = Rd32(b, keys + 4 * (stride * r31.u32));
    if (OwnValue(b, key) > kGameMax) r3.u64 = key;
}

// sub_82631A20 draws one value row. When the row has more values than the menu
// movies give a row element, the element sub_82630158 hands it is grown first.
extern "C" REX_FUNC(rex_sub_82631A20) {
    uint32_t need = 0;
    if (net_limits::MaxPlayersChanged()) need = RowValueCount(base, ctx.r3.u32);
    g_row_need = need > kMovieRowElements ? need : 0;
    __imp__rex_sub_82631A20(ctx, base);
    g_row_need = 0;
}

extern "C" REX_FUNC(rex_sub_82630158) {
    __imp__rex_sub_82630158(ctx, base);
    const uint32_t need = g_row_need;
    if (!need) return;
    g_row_need = 0;
    GrowRowElements(ctx, base, ctx.r3.u32, need);
}

#else
#include <rex/ppc/context.h>
namespace net_limits {
int MaxPlayers() { return 16; }
bool MaxPlayersChanged() { return false; }
}  // namespace net_limits
void MaxPlayers_8227D3D4(PPCRegister&) {}
void MaxPlayers_8265A68C(PPCRegister&, PPCRegister&, PPCRegister&) {}
void MaxPlayers_826586F4(PPCRegister&) {}
void MaxPlayers_82656AA8(PPCRegister&) {}
void MaxPlayers_82656AC0(PPCRegister&) {}
void MaxPlayers_8265DEA4(PPCRegister&) {}
void MaxPlayers_8265DFB0(PPCRegister&, PPCRegister&) {}
void MaxPlayers_822859BC(PPCRegister&) {}
void MaxPlayers_82631B1C(PPCRegister&, PPCRegister&, PPCRegister&) {}
#endif
