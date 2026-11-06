// The parts bought for each car model, moved out of the player profile so the
// roster can hold any number of vehicles.
//
// LocalOptions, the player profile at 0x8288E920 (sub_82387B90), keeps one
// 84-byte record per roster vehicle at +246640, at the vehicle's index in the
// roster: +4 the vehicle id, +8 what was spent on the model, +12..+81 the
// bitfield of parts already bought (sub_823901C0 tests a bit, sub_823900E0 sets
// one: "Part is already purchased, consider it free"), +82 a u16 the sale price
// adds. There are 77 of them - the constructor builds 77, the reset clears 6468
// bytes - and right behind sit the 30 owned cars' damage floats (+253108), more
// profile fields, the dirty flag at +254032 and then the globals the image puts
// after the profile. The roster has no limit, and the two functions that fill
// the table, the new-game defaults (sub_8238C278) and the reconcile after a
// roster change or a load (sub_8238C5A0), write one record per roster entry with
// no bound check. From the 78th vehicle on they write over the damage floats,
// from the 89th on past the profile into the globals after it; a large enough
// roster crashes the boot on a clobbered option string in sub_8238BA38 (a roster
// of 189 did).
//
// Here the table gets a block of its own, sized from the roster, and every
// access goes there. The game forms the table's address in 25 places, always in
// one of two ways: `lis rY,3; ori rX,rY,0xC3xx` (an offset for an indexed load
// or store) or `addis rX,rP,4; addi rX,rX,-0x3C90` (a record pointer). A hook
// right after each of those instructions moves the register by the distance
// from the profile's table to the new block. The constructor, the destructor and
// the reset's memset still work on the profile's 77 slots, which now only feed
// mc4.sav.
//
// mc4.sav holds the profile verbatim (254032 bytes, sub_822D2130), so its layout
// stays as it was: just before the profile is serialized the first 77 records
// are copied into the profile's slots, the count written to the save is held to
// 77, and the records past that go to mc4_vehicle_records.bin next to the save's
// content folder - not inside it, since a CREATE_ALWAYS save wipes that folder.
// Loading copies the 77 back and appends the rest when the file matches the save.
// The showroom's own limits are in showroom.cpp.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "hooks_internal.h"
#include "larecomp_log.h"

REXCVAR_DEFINE_BOOL(vehicle_records_relocate, true, "MCLA/Patches",
    "Keep the parts bought for each car model in a table sized to the roster, so the game "
    "takes more than 77 vehicles. Off = the game's own 77-slot table: past 77 vehicles it "
    "overwrites other profile data, and a large roster crashes the boot.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REX_EXTERN(__imp__rex_sub_8238C9B0);  // LocalOptions reset
REX_EXTERN(__imp__rex_sub_8238C278);  // new-game defaults: a record per roster vehicle
REX_EXTERN(__imp__rex_sub_8238C5A0);  // reconcile the records with the roster
REX_EXTERN(__imp__rex_sub_8238B3F8);  // before a save: recompute +8 of each record
REX_EXTERN(__imp__rex_sub_822D20D8);  // serialize LocalOptions into the save buffer
REX_EXTERN(__imp__rex_sub_822D2258);  // deserialize LocalOptions from the save buffer
REX_EXTERN(__imp__rex_sub_823971D0);  // load roster list vehicle%04d

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kProfile = 0x8288E920;               // LocalOptions
constexpr uint32_t kRecordCount = kProfile + 1348;      // records in use (s32)
constexpr uint32_t kProfileTable = kProfile + 246640;   // the profile's 77 slots
constexpr uint32_t kOwnedFloats = kProfile + 253108;    // 30 owned cars x 24 bytes
constexpr uint32_t kOwnedFloatSlot = 24;
constexpr uint32_t kOwnedFloatsEnd = kOwnedFloats + 30 * kOwnedFloatSlot;
constexpr uint32_t kSavedSize = 254032;                 // what mc4.sav stores
constexpr uint32_t kRecordSize = 84;
constexpr uint32_t kProfileSlots = 77;
constexpr uint32_t kRosterCount = 0x828CD0AC;           // u16, entries of dword_828CD0A8
constexpr uint32_t kSlack = 64;                         // free records kept past the need

constexpr uint32_t kSites[] = {
    0x8238ACB8, 0x8238B40C, 0x8238C2C0, 0x8238C5D4, 0x8238C5D8, 0x8238C5FC, 0x8238C7B4,
    0x8269A2FC, 0x8269B724, 0x8269B810, 0x8269BE7C, 0x8269DC84, 0x8269E438, 0x826A7328,
    0x826A96A8, 0x826A9964, 0x826A9A30, 0x826A9B0C, 0x826A9B84, 0x826AA490, 0x826AEE00,
    0x826AEF3C, 0x826AF6F8, 0x826B01B0, 0x8279ABF0,
};
constexpr size_t kSiteCount = sizeof(kSites) / sizeof(kSites[0]);

std::atomic<uint32_t> g_table{0};     // guest address of the moved table
std::atomic<uint32_t> g_capacity{0};  // records it has room for
std::atomic<uint32_t> g_delta{0};     // g_table - kProfileTable, mod 2^32; 0 = not moved
std::array<std::atomic<bool>, kSiteCount> g_site_seen{};

// The profile's 77 slots are written only by the copy before a save, the load
// and the reset. Their hash is taken after each of those; a different hash at
// the next one means something wrote them in between - an access the hooks miss.
uint64_t g_slots_hash = 0;
bool g_slots_hash_valid = false;

bool Enabled() { return REXCVAR_GET(vehicle_records_relocate); }

uint16_t ReadGuestU16(const uint8_t* b, uint32_t a) {
    return static_cast<uint16_t>((uint32_t(b[a]) << 8) | uint32_t(b[a + 1]));
}

uint32_t RosterCount(const uint8_t* b) { return ReadGuestU16(b, kRosterCount); }

uint32_t RecordCount(const uint8_t* b) {
    const int32_t n = static_cast<int32_t>(ReadGuestU32(b, kRecordCount));
    return n > 0 ? static_cast<uint32_t>(n) : 0;
}

uint64_t Fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

uint64_t SlotsHash(const uint8_t* b) {
    return Fnv1a(b + kProfileTable, size_t(kProfileSlots) * kRecordSize);
}

void NoteSlots(const uint8_t* b) {
    g_slots_hash = SlotsHash(b);
    g_slots_hash_valid = true;
}

void CheckSlots(const uint8_t* b, const char* when) {
    if (g_slots_hash_valid && SlotsHash(b) != g_slots_hash)
        LARECOMP_APP_ERROR("[vehicle-records] the profile's own record slots changed before the {} "
                           "- some access to the table is not being moved", when);
}

// Grows the moved table to at least `needed` records. The old block is left
// allocated: a pointer the game formed into it a moment ago stays readable.
bool EnsureCapacity(uint8_t* b, uint32_t needed, const char* why) {
    if (!Enabled() || !b) return false;
    const uint32_t cap = g_capacity.load();
    const uint32_t old = g_table.load();
    if (old && needed <= cap) return true;
    uint32_t want = std::max<uint32_t>({needed + kSlack, cap * 2, 256});
    want = (want + 63) & ~63u;
    auto* rt = rex::Runtime::instance();
    const uint32_t block =
        rt && rt->memory() ? rt->memory()->SystemHeapAlloc(want * kRecordSize, 0x1000) : 0;
    if (!block) {
        LARECOMP_APP_ERROR("[vehicle-records] no guest memory for {} records ({})", want, why);
        return old != 0;
    }
    std::memset(b + block, 0, size_t(want) * kRecordSize);
    if (old) std::memcpy(b + block, b + old, size_t(cap) * kRecordSize);
    g_table.store(block);
    g_capacity.store(want);
    g_delta.store(block - kProfileTable);
    LARECOMP_APP_INFO("[vehicle-records] parts table at {:08X}, room for {} records ({}, roster {})",
                      block, want, why, RosterCount(b));
    return true;
}

void Rebase(PPCRegister& reg, size_t site) {
    const uint32_t delta = g_delta.load(std::memory_order_relaxed);
    if (!delta) return;
    reg.u64 = static_cast<uint32_t>(reg.u32 + delta);
    if (!g_site_seen[site].exchange(true, std::memory_order_relaxed))
        LARECOMP_APP_INFO("[vehicle-records] first access moved at {:08X}", kSites[site]);
}

// --- the file beside mc4.sav -----------------------------------------------

constexpr char kSidecarMagic[4] = {'L', 'V', 'R', 'T'};
constexpr uint32_t kSidecarVersion = 1;

struct SidecarHeader {
    char magic[4];
    uint32_t version;
    uint32_t record_size;
    uint32_t total;       // records in use when the profile was serialized
    uint32_t in_save;     // how many of them mc4.sav carries (min(total, 77))
    uint32_t reserved;
    uint64_t slots_hash;  // hash of the 77 slots exactly as mc4.sav got them
};
static_assert(sizeof(SidecarHeader) == 32);

// The directory the content store files this profile under: the fork names it
// after the configured profile (UserProfile::ConfiguredName), the stock SDK
// after the hex XUID.
template <typename Profile>
std::string ProfileDirFor() {
    if constexpr (requires { Profile::ConfiguredName(); }) {
        std::string name = Profile::ConfiguredName();
        if (!name.empty()) return name;
    }
    auto* ks = rex::system::kernel_state();
    const Profile* profile = ks ? ks->user_profile() : nullptr;
    if (!profile) return {};
    char xuid[17];
    std::snprintf(xuid, sizeof(xuid), "%016llX", static_cast<unsigned long long>(profile->xuid()));
    return xuid;
}

fs::path SidecarPath() {
    auto* rt = rex::Runtime::instance();
    const fs::path root = rt ? rt->user_data_root() : fs::path{};
    const std::string dir = ProfileDirFor<rex::system::xam::UserProfile>();
    if (root.empty() || dir.empty()) return {};
    return root / dir / "545407F8" / "mc4_vehicle_records.bin";
}

void WriteSidecar(const uint8_t* b, uint32_t total, uint32_t in_save) {
    const fs::path path = SidecarPath();
    if (path.empty()) return;
    SidecarHeader h{};
    std::memcpy(h.magic, kSidecarMagic, sizeof(h.magic));
    h.version = kSidecarVersion;
    h.record_size = kRecordSize;
    h.total = total;
    h.in_save = in_save;
    h.slots_hash = SlotsHash(b);
    const size_t extra = size_t(total - in_save) * kRecordSize;

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            LARECOMP_APP_ERROR("[vehicle-records] cannot write {}", tmp.string());
            return;
        }
        out.write(reinterpret_cast<const char*>(&h), sizeof(h));
        if (extra)
            out.write(reinterpret_cast<const char*>(b + g_table.load() + in_save * kRecordSize),
                      static_cast<std::streamsize>(extra));
        if (!out) {
            LARECOMP_APP_ERROR("[vehicle-records] short write to {}", tmp.string());
            return;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        LARECOMP_APP_ERROR("[vehicle-records] cannot replace {}: {}", path.string(), ec.message());
        return;
    }
    LARECOMP_APP_INFO("[vehicle-records] saved {} records: {} in mc4.sav, {} in {}", total,
                      in_save, total - in_save, path.filename().string());
}

// Reads the records mc4.sav could not hold. Only a file written with this exact
// save is used: same count in the save and the same 77 slots.
bool ReadSidecar(const uint8_t* b, uint32_t in_save, std::vector<uint8_t>& extra,
                 uint32_t& total) {
    const fs::path path = SidecarPath();
    if (path.empty()) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    SidecarHeader h{};
    in.read(reinterpret_cast<char*>(&h), sizeof(h));
    if (!in || std::memcmp(h.magic, kSidecarMagic, sizeof(h.magic)) != 0 ||
        h.version != kSidecarVersion || h.record_size != kRecordSize) {
        LARECOMP_APP_WARN("[vehicle-records] {} is not a record file of this version, ignored",
                          path.filename().string());
        return false;
    }
    if (h.in_save != in_save || h.slots_hash != SlotsHash(b) || h.total < h.in_save) {
        LARECOMP_APP_WARN("[vehicle-records] {} belongs to another save ({} records in it, this "
                          "save has {}), ignored", path.filename().string(), h.in_save, in_save);
        return false;
    }
    extra.resize(size_t(h.total - h.in_save) * kRecordSize);
    if (!extra.empty()) in.read(reinterpret_cast<char*>(extra.data()),
                                static_cast<std::streamsize>(extra.size()));
    if (!in) {
        LARECOMP_APP_WARN("[vehicle-records] {} is cut short, ignored", path.filename().string());
        return false;
    }
    total = h.total;
    return true;
}

// After mc4.sav's copy of the profile is in place: its 77 slots back into the
// moved table, then the records the file beside it kept.
void RestoreAfterLoad(uint8_t* b) {
    const uint32_t saved = RecordCount(b);
    const uint32_t in_save = std::min(saved, kProfileSlots);
    std::vector<uint8_t> extra;
    uint32_t total = in_save;
    const bool have_extra = ReadSidecar(b, in_save, extra, total);
    if (!EnsureCapacity(b, std::max(total, RosterCount(b)), "load")) return;

    uint8_t* table = b + g_table.load();
    std::memset(table, 0, size_t(g_capacity.load()) * kRecordSize);
    std::memcpy(table, b + kProfileTable, size_t(kProfileSlots) * kRecordSize);
    if (have_extra && !extra.empty())
        std::memcpy(table + size_t(in_save) * kRecordSize, extra.data(), extra.size());

    if (saved > kProfileSlots && !have_extra) {
        // Written by a build without this fix while the roster had more than 77
        // vehicles: records 77.. went over the owned cars' damage floats, which
        // are noise now. The records themselves are given up; the reconcile adds
        // them back with no parts bought.
        uint32_t end = kProfileTable + saved * kRecordSize;
        end = kOwnedFloats + ((end - kOwnedFloats + kOwnedFloatSlot - 1) / kOwnedFloatSlot) *
                                 kOwnedFloatSlot;
        end = std::min(end, kOwnedFloatsEnd);
        if (end > kOwnedFloats) std::memset(b + kOwnedFloats, 0, end - kOwnedFloats);
        LARECOMP_APP_WARN("[vehicle-records] this save counted {} records, more than its 77 slots, "
                          "with no record file: kept 77, cleared the damage of {} owned car(s)",
                          saved, (end - kOwnedFloats) / kOwnedFloatSlot);
    }
    WriteGuestU32(b, kRecordCount, total);
    NoteSlots(b);
    LARECOMP_APP_INFO("[vehicle-records] loaded {} records: {} from mc4.sav, {} from the record file",
                      total, in_save, total - in_save);
}

}  // namespace

// The 25 places that form the table's address; each moves the register the
// instruction before it just wrote.
#define VEHICLE_RECORDS_SITE(index, addr, reg) \
    void VehicleRecords_##addr(PPCRegister& reg) { Rebase(reg, index); }

VEHICLE_RECORDS_SITE(0, 8238ACB8, r6)    // sub_8238AC80: set +82 of record r
VEHICLE_RECORDS_SITE(1, 8238B40C, r28)   // sub_8238B3F8: +8 recompute, both loops
VEHICLE_RECORDS_SITE(2, 8238C2C0, r31)   // sub_8238C278: new-game defaults
VEHICLE_RECORDS_SITE(3, 8238C5D4, r21)   // sub_8238C5A0: +82
VEHICLE_RECORDS_SITE(4, 8238C5D8, r23)   // sub_8238C5A0: table base (compact, append, sort)
VEHICLE_RECORDS_SITE(5, 8238C5FC, r25)   // sub_8238C5A0: +8
VEHICLE_RECORDS_SITE(6, 8238C7B4, r24)   // sub_8238C5A0: +4, the id search
VEHICLE_RECORDS_SITE(7, 8269A2FC, r23)   // sub_8269A290: the current car's record
VEHICLE_RECORDS_SITE(8, 8269B724, r29)   // sub_8269B6D8
VEHICLE_RECORDS_SITE(9, 8269B810, r26)   // sub_8269B7C0
VEHICLE_RECORDS_SITE(10, 8269BE7C, r3)   // sub_8269BE00: mark a part bought
VEHICLE_RECORDS_SITE(11, 8269DC84, r27)  // sub_8269DBF8
VEHICLE_RECORDS_SITE(12, 8269E438, r29)  // sub_8269E408: price of the parts picked
VEHICLE_RECORDS_SITE(13, 826A7328, r22)  // sub_826A7170: owned list, sale value
VEHICLE_RECORDS_SITE(14, 826A96A8, r27)  // sub_826A92E0: part list
VEHICLE_RECORDS_SITE(15, 826A9964, r7)   // sub_826A98E0: sell, +82
VEHICLE_RECORDS_SITE(16, 826A9A30, r3)   // sub_826A98E0: sell, clear the record
VEHICLE_RECORDS_SITE(17, 826A9B0C, r7)   // sub_826A9A88: trade-in, +82
VEHICLE_RECORDS_SITE(18, 826A9B84, r3)   // sub_826A9A88: trade-in, clear the record
VEHICLE_RECORDS_SITE(19, 826AA490, r28)  // sub_826AA3C0: checkout, mark the parts bought
VEHICLE_RECORDS_SITE(20, 826AEE00, r3)   // sub_826AB520
VEHICLE_RECORDS_SITE(21, 826AEF3C, r29)  // sub_826AB520
VEHICLE_RECORDS_SITE(22, 826AF6F8, r5)   // sub_826AB520: +82
VEHICLE_RECORDS_SITE(23, 826B01B0, r8)   // sub_826AFFD8: Rate My Ride purchase, +82
VEHICLE_RECORDS_SITE(24, 8279ABF0, r8)   // sub_8279AB90: +82

#undef VEHICLE_RECORDS_SITE

// Reset: the game clears the profile's slots, the moved table is cleared with them.
extern "C" REX_FUNC(rex_sub_8238C9B0) {
    const bool on = Enabled();
    if (on) CheckSlots(base, "reset");
    __imp__rex_sub_8238C9B0(ctx, base);
    if (!on) return;
    if (EnsureCapacity(base, RosterCount(base), "reset"))
        std::memset(base + g_table.load(), 0, size_t(g_capacity.load()) * kRecordSize);
    NoteSlots(base);
}

// New-game defaults write one record per roster vehicle.
extern "C" REX_FUNC(rex_sub_8238C278) {
    const bool on = Enabled();
    if (on) EnsureCapacity(base, RosterCount(base), "new-game defaults");
    __imp__rex_sub_8238C278(ctx, base);
    if (on)
        LARECOMP_APP_INFO("[vehicle-records] new-game defaults: {} records for a roster of {}",
                          RecordCount(base), RosterCount(base));
}

// The reconcile keeps the records whose vehicle is still in the roster and
// appends one for each roster vehicle without a record.
extern "C" REX_FUNC(rex_sub_8238C5A0) {
    if (!Enabled()) {
        __imp__rex_sub_8238C5A0(ctx, base);
        return;
    }
    const uint32_t before = RecordCount(base);
    EnsureCapacity(base, before + RosterCount(base), "reconcile");
    __imp__rex_sub_8238C5A0(ctx, base);
    LARECOMP_APP_INFO("[vehicle-records] reconcile: {} -> {} records, roster {}", before,
                      RecordCount(base), RosterCount(base));
}

// Before a save the game sets +8 of records 0..76 from +82 (its loop is fixed
// at 77) and adds each owned car to its model's. The records past 76 get the
// first step here; then the first 77 go into the profile's slots, which the save
// copies right after this returns.
extern "C" REX_FUNC(rex_sub_8238B3F8) {
    const uint32_t table = g_table.load();
    if (!Enabled() || !table) {
        __imp__rex_sub_8238B3F8(ctx, base);
        return;
    }
    const uint32_t count = std::min(RecordCount(base), g_capacity.load());
    for (uint32_t i = kProfileSlots; i < count; ++i) {
        const uint32_t record = table + i * kRecordSize;
        WriteGuestU32(base, record + 8, uint32_t(ReadGuestU16(base, record + 82)) - 1);
    }
    __imp__rex_sub_8238B3F8(ctx, base);
    CheckSlots(base, "save");
    std::memcpy(base + kProfileTable, base + g_table.load(), size_t(kProfileSlots) * kRecordSize);
    NoteSlots(base);
}

// The profile goes into the save buffer (r4, room r5). Past 77 records the count
// in the buffer is held to what its slots carry, and the rest go to the file.
extern "C" REX_FUNC(rex_sub_822D20D8) {
    const uint32_t buffer = ctx.r4.u32;
    const uint32_t room = ctx.r5.u32;
    __imp__rex_sub_822D20D8(ctx, base);
    if (!Enabled() || !g_table.load()) return;
    const uint32_t total = std::min(RecordCount(base), g_capacity.load());
    const uint32_t in_save = std::min(total, kProfileSlots);
    if (total > kProfileSlots && buffer && room >= kSavedSize)
        WriteGuestU32(base, buffer + (kRecordCount - kProfile), in_save);
    WriteSidecar(base, total, in_save);
}

// The profile comes back from the save buffer.
extern "C" REX_FUNC(rex_sub_822D2258) {
    __imp__rex_sub_822D2258(ctx, base);
    if (Enabled()) RestoreAfterLoad(base);
}

// Each roster list read can grow the roster; the table keeps room for it.
extern "C" REX_FUNC(rex_sub_823971D0) {
    __imp__rex_sub_823971D0(ctx, base);
    if (Enabled() && g_table.load()) EnsureCapacity(base, RosterCount(base), "roster list");
}

#endif  // REXGLUE_HAS_XEO3_TARGET
