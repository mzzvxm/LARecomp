// The cars the player owns, moved out of the player profile so a save can hold
// more than 30 of them.
//
// LocalOptions, the player profile at 0x8288E920 (sub_82387B90), keeps the
// owned cars as 30 CarConfigs of 8176 bytes at +1360, their damage as 30 x 24
// bytes at +253108, the count at +1340 and the current slot at +1344. Right
// after car 29 comes the parts table at +246640, so the arrays cannot grow in
// place, and the profile goes into mc4.sav verbatim (254032 bytes, checked by
// size and version on load), so the save cannot grow either. Seven places refuse
// a 31st car with a `cmpwi 30` on the count: AddCar (sub_8238B6E8, which then
// pays the car's price out instead), the showroom purchase checks of
// sub_826AB520 (cases 149 and 150), the finishing steps of a purchase
// (sub_826A9C60) and of a trade-in (sub_826A9A88), the Rate My Ride purchase
// (sub_826AFFD8) and the new-game loop of sub_8238C278, which only fills the
// first 30 slots. A third array outside the profile, one byte per owned car at
// +137972 of the object at *(0x8286D800)+4296, has room for 30 and is written by
// sub_82275E60 for every owned car without a bound (car 30 would land on the
// object's busy flag at +138002).
//
// With owned_cars_limit above 30 the three arrays get one block of their own,
// sized once to the limit, and every place that forms an address in them is
// moved there by a hook right after the instruction that forms it (the table in
// owned_cars_sites.inc). The purchase checks compare against the limit. The
// profile's own 30 slots become a mirror the constructor, the destructor and the
// save's copy still use: right after the pre-save pass (sub_8238B3F8, which
// writes every owned car's vehicle id into it) the first 30 cars and their
// damage are copied into it, the count written to the save is held to 30 and
// the current slot to one of those 30, and the cars past that go to
// mc4_owned_cars.bin beside the save's content folder, tied to the save by a
// hash of what it carries. Loading copies the 30 back into the block and appends
// the rest when a file matches. A build without this reads such a save as one
// with 30 cars. With the option at 30 nothing here changes anything.
//
// The game serializes the profile into a buffer and writes that buffer to the
// device later and asynchronously (mcSaveGameDataManager: the buffer is built
// through the save file's vtable at 0x822017F8 and 0x82201824, the write is
// sub_82617550 from 0x82200D0C and 0x82201A40), so a file written here may
// belong to a save that never reached the disk. No file is therefore replaced
// unless another one still holds what it held:
//   mc4_owned_cars.bin          the newest serialize
//   mc4_owned_cars.bin.prev     the one before
//   mc4_owned_cars.bin.loaded   a copy of the file the loaded save matched, kept
//                               until a later load matches another one
//   *.orphanN                   anything that had to make room and is not known
//                               to be held elsewhere
// Every file names its line of saves (a random id) and its serialize number. A
// load that matches one file proves which save is on the disk; only then are the
// set-aside files of the same line removed (owned_cars_keep_all_generations keeps
// them). Files of another line are never removed. Cars past owned_cars_limit
// that a file holds stay out of the game but go into every file written after.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include "settings/larecomp_settings.h"
#include <rex/ppc/recompiled_code.h>
#include <rex/ppc/context.h>
#include "larecomp_guest.h"
#include <rex/kernel/title_runtime.h>
#include <rex/xam/content_manager.h>
#include <rex/xam/user_profile.h>
#include <rex/path_settings.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "hooks_internal.h"
#include "larecomp_log.h"
#include "owned_cars.h"

namespace {

namespace fs = std::filesystem;

using owned_cars::kGameCap;
using owned_cars::kMaxCap;

constexpr uint32_t kProfile = 0x8288E920;               // LocalOptions
constexpr uint32_t kCount = kProfile + 1340;            // owned cars (s32)
constexpr uint32_t kCurrent = kProfile + 1344;          // current slot (s32)
constexpr uint32_t kCars = kProfile + 1360;             // 30 CarConfigs
constexpr uint32_t kCarSize = 8176;
constexpr uint32_t kDamage = kProfile + 253108;         // 30 x 6 floats
constexpr uint32_t kDamageSize = 24;
constexpr uint32_t kSavedSize = 254032;                 // what mc4.sav stores
constexpr uint32_t kPlatesOffset = 137972;              // u8[30] in the plate owner
constexpr uint32_t kStaticPartsRecords = 77;            // parts table left in the profile
constexpr uint32_t kImageLow = 0x82000000;
constexpr uint32_t kImageHigh = 0x83000000;

static_assert(kCars + kGameCap * kCarSize == kProfile + 246640,
              "car 30 would be the parts table");

// The parts table is read with the owned slot as its index (0x8279ABDC, the
// loop of sub_8238C5A0): no slot may lie past its records. Moved by
// vehicle_records.cpp it has room for at least 256; left in the profile, 77.
uint32_t ComputeCap() {
    const int32_t want = larecomp::settings::owned_cars_limit.Get();
    if (want <= int32_t(kGameCap)) return kGameCap;
    const uint32_t ceiling = larecomp::settings::vehicle_records_relocate.Get() ? kMaxCap : kStaticPartsRecords;
    const uint32_t cap = std::min<uint32_t>(uint32_t(want), ceiling);
    if (cap < uint32_t(want))
        LARECOMP_APP_WARN("[owned-cars] owned_cars_limit {} held to {}", want, cap);
    return cap;
}

std::atomic<uint32_t> g_cars{0};          // guest address of the moved cars; 0 = not moved
std::atomic<uint32_t> g_damage{0};
std::atomic<uint32_t> g_plates{0};
std::atomic<uint32_t> g_car_delta{0};     // g_cars - kCars, mod 2^32; 0 = not moved
std::atomic<uint32_t> g_damage_delta{0};  // g_damage - kDamage
std::atomic<uint32_t> g_plates_owner{0};  // the object the plate sites saw last
std::atomic<uint32_t> g_plates_owners{0}; // how many different ones so far

// A slot and its damage as the constructor (sub_8238CBC8: sub_8238F550 per slot,
// six 0.0f per damage slot) leaves them, taken at the first reset, which the
// constructor itself runs with no car owned (slot 0). The CarConfig constructor
// writes data and its own vtable pointers into the image, nothing that points
// into the slot.
std::vector<uint8_t> g_blank_car;
std::array<uint8_t, kDamageSize> g_blank_damage{};

// The mirror is written only by the copy before a save, the load and the
// constructor/destructor; its hash is taken after each copy, and a different one
// at the next copy means a write the hooks did not move.
uint64_t g_mirror_hash = 0;
bool g_mirror_hash_valid = false;
bool g_profile_copied = false;  // BeforeProfileCopy ran in this serialize

uint64_t Fnv1a(uint64_t h, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}
constexpr uint64_t kFnvBasis = 1469598103934665603ull;

uint64_t MirrorHash(const uint8_t* b) {
    uint64_t h = Fnv1a(kFnvBasis, b + kCars, size_t(kGameCap) * kCarSize);
    return Fnv1a(h, b + kDamage, size_t(kGameCap) * kDamageSize);
}

// What ties the file to a save: the 30 slots and their damage as the save holds
// them, plus the count it holds.
uint64_t SaveHash(const uint8_t* b, uint32_t in_save) {
    uint8_t c[4] = {uint8_t(in_save >> 24), uint8_t(in_save >> 16), uint8_t(in_save >> 8),
                    uint8_t(in_save)};
    return Fnv1a(MirrorHash(b), c, sizeof(c));
}

void NoteMirror(const uint8_t* b) {
    g_mirror_hash = MirrorHash(b);
    g_mirror_hash_valid = true;
}

void CheckMirror(const uint8_t* b, const char* when) {
    if (g_mirror_hash_valid && MirrorHash(b) != g_mirror_hash)
        LARECOMP_APP_ERROR("[owned-cars] the profile's own car slots changed before the {} - "
                           "some write to the owned cars is not being moved", when);
}

int32_t ReadS32(const uint8_t* b, uint32_t a) { return static_cast<int32_t>(ReadGuestU32(b, a)); }

uint32_t OwnedCount(const uint8_t* b) {
    const int32_t n = ReadS32(b, kCount);
    return n > 0 ? std::min<uint32_t>(uint32_t(n), owned_cars::Cap()) : 0;
}

// The blank car comes from the first unused slot, which only the constructor's
// reset is sure to have fresh; later it is taken only while a slot is unused.
bool TakeBlank(const uint8_t* b, const char* why) {
    if (g_blank_car.size() == kCarSize) return true;
    const int32_t count = ReadS32(b, kCount);
    if (count < 0 || count >= int32_t(kGameCap)) {
        LARECOMP_APP_ERROR("[owned-cars] no unused slot to take a blank car from ({} owned, {})",
                           count, why);
        return false;
    }
    const uint32_t slot = uint32_t(count);
    g_blank_car.assign(b + kCars + slot * kCarSize, b + kCars + (slot + 1) * kCarSize);
    std::memcpy(g_blank_damage.data(), b + kDamage + slot * kDamageSize, kDamageSize);
    LARECOMP_APP_INFO("[owned-cars] blank car taken from slot {} ({})", slot, why);
    return true;
}

// A recognizable pattern over the mirror. Words that hold an address in the
// image (the vtables of the slot and its parts) are kept, so the destructor at
// exit still finds them.
void PoisonMirror(uint8_t* b) {
    if (!larecomp::settings::owned_cars_poison_static.Get() || g_blank_car.size() != kCarSize) return;
    for (uint32_t s = 0; s < kGameCap; ++s) {
        uint8_t* slot = b + kCars + s * kCarSize;
        for (uint32_t o = 0; o + 4 <= kCarSize; o += 4) {
            const uint32_t w = ReadGuestU32(g_blank_car.data(), o);
            if (w >= kImageLow && w < kImageHigh) continue;
            std::memset(slot + o, 0xCD, 4);
        }
    }
    std::memset(b + kDamage, 0xCD, size_t(kGameCap) * kDamageSize);
}

void FillSpare(uint8_t* b, uint32_t from) {
    const uint32_t cars = g_cars.load(), damage = g_damage.load(), cap = owned_cars::Cap();
    for (uint32_t i = from; i < cap; ++i) {
        std::memcpy(b + cars + i * kCarSize, g_blank_car.data(), kCarSize);
        std::memcpy(b + damage + i * kDamageSize, g_blank_damage.data(), kDamageSize);
    }
}

// The block is made once, at the first reset - the profile's constructor
// (sub_8238CBC8, from the static initializer sub_827AF020 that the entry point
// runs through sub_82132740 before main) builds the 30 slots and resets right
// away, so the first unused slot is a fresh one - and never moved again.
bool EnsureBlock(uint8_t* b, const char* why, uint32_t parts_room) {
    const uint32_t cap = owned_cars::Cap();
    if (cap <= kGameCap || !b) return false;
    if (g_cars.load()) return true;
    if (cap > parts_room) {
        LARECOMP_APP_ERROR("[owned-cars] the parts table has room for {} records, fewer than {} "
                           "owned cars ({}); the game keeps 30", parts_room, cap, why);
        return false;
    }
    if (!TakeBlank(b, why)) {
        LARECOMP_APP_ERROR("[owned-cars] the owned cars stay at 30 ({})", why);
        return false;
    }

    const uint32_t cars_bytes = cap * kCarSize;
    const uint32_t damage_bytes = cap * kDamageSize;
    const uint32_t plates_bytes = (cap + 63) & ~63u;
    auto* memory = larecomp::guest::AddressSpace();
    const uint32_t block =
        memory ? memory->SystemHeapAlloc(cars_bytes + damage_bytes + plates_bytes, 0x1000) : 0;
    if (!block) {
        LARECOMP_APP_ERROR("[owned-cars] no guest memory for {} owned cars ({}); the game keeps 30",
                           cap, why);
        return false;
    }

    const uint32_t cars = block, damage = block + cars_bytes, plates = damage + damage_bytes;
    std::memcpy(b + cars, b + kCars, size_t(kGameCap) * kCarSize);
    std::memcpy(b + damage, b + kDamage, size_t(kGameCap) * kDamageSize);
    std::memset(b + plates, 0, plates_bytes);
    g_cars.store(cars);
    g_damage.store(damage);
    FillSpare(b, kGameCap);
    g_damage_delta.store(damage - kDamage);
    g_plates.store(plates);
    g_car_delta.store(cars - kCars);  // last: the sites and the gates act once this is set
    PoisonMirror(b);
    NoteMirror(b);
    LARECOMP_APP_INFO("[owned-cars] {} owned cars ({}, parts room {})", cap, why, parts_room);
    LARECOMP_APP_INFO("[owned-cars] block cars=0x{:08X} damage=0x{:08X} plates=0x{:08X} cap={}",
                      cars, damage, plates, cap);
    return true;
}

// --- the file beside mc4.sav -----------------------------------------------

constexpr char kSidecarMagic[4] = {'L', 'O', 'C', 'R'};
constexpr uint32_t kSidecarVersion = 2;
constexpr char kSidecarName[] = "mc4_owned_cars.bin";
constexpr size_t kHeaderSizeV1 = 40;   // version 1: no line id, no serialize number

struct SidecarHeader {
    char magic[4];
    uint32_t version;
    uint32_t car_size;
    uint32_t damage_size;
    uint32_t total;       // cars of the save this file belongs to, those past the limit too
    uint32_t in_save;     // how many of them mc4.sav carries (min(owned, 30))
    int32_t saved_cur;    // the current slot written to mc4.sav
    int32_t real_cur;     // the current slot it stood for
    uint64_t save_hash;   // SaveHash of the 30 slots exactly as mc4.sav got them
    uint64_t line;        // random id shared by the files of one line of saves; 0 = unknown
    uint64_t serial;      // serialize number within the line
};
static_assert(sizeof(SidecarHeader) == 56);
static_assert(offsetof(SidecarHeader, line) == kHeaderSizeV1);

size_t HeaderSize(const SidecarHeader& h) {
    return h.version == 1 ? kHeaderSizeV1 : sizeof(SidecarHeader);
}

struct Generation {
    fs::path path;
    SidecarHeader h{};
    int rank = 9;  // which to prefer among equals: main, pin, prev, the rest
};

// The line of saves the loaded save (or the new game) belongs to, and the cars
// past the limit its file held. All of it belongs to one profile's folder.
struct SaveLine {
    fs::path file;        // the main file of the profile this state belongs to
    uint64_t id = 0;      // 0: no line yet, the next save starts one
    uint64_t serial = 0;  // highest serialize number of the line seen
};
SaveLine g_line;
std::vector<uint8_t> g_over_cars;    // cars past Cap() from the file the save matched
std::vector<uint8_t> g_over_damage;
bool g_recovered = false;            // owned_cars_recover_unmatched used this start

uint32_t OverCount() { return uint32_t(g_over_cars.size() / kCarSize); }

// The directory the content store files the signed in profile under (the same
// answer mc4_vehicle_records.bin uses): the content manager's own.
std::string ProfileDirFor() {
    auto* rt = REX_RUNTIME();
    if (!rt) return {};
    return rt->GetContentManager().ProfileDirName(rt->GetUserProfile().GetXUID());
}

fs::path SidecarPath() {
    auto* rt = REX_RUNTIME();
    const fs::path root = rt ? rex::GetPathSettings().user_root.Get() : fs::path{};
    const std::string dir = ProfileDirFor();
    if (root.empty() || dir.empty()) return {};
    return root / dir / "545407F8" / kSidecarName;
}

fs::path WithSuffix(const fs::path& file, const char* suffix) {
    fs::path p = file;
    p += suffix;
    return p;
}

uint64_t NewLineId() {
    std::random_device rd;
    uint64_t v = (uint64_t(rd()) << 32) ^ uint64_t(rd()) ^
                 uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
    return v ? v : 1;
}

void DropOverflow(const char* why) {
    if (OverCount())
        LARECOMP_APP_WARN("[owned-cars] {}: the {} cars past owned_cars_limit stay in the files "
                          "beside the save they came with", why, OverCount());
    g_over_cars.clear();
    g_over_damage.clear();
}

void StartNewLine(const char* why) {
    DropOverflow(why);
    g_line.id = 0;
    g_line.serial = 0;
}

// The state follows the profile: another profile's folder starts afresh.
void FollowProfile(const fs::path& file) {
    if (g_line.file == file) return;
    if (!g_line.file.empty()) StartNewLine("another profile");
    g_line.file = file;
}

// Moves a file out of the way under a new name; false when it could not.
bool SetAside(const fs::path& p) {
    std::error_code ec;
    for (int n = 1; n < 10000; ++n) {
        fs::path q = p;
        q += ".orphan" + std::to_string(n);
        if (fs::exists(q, ec)) continue;
        fs::rename(p, q, ec);
        if (ec) {
            LARECOMP_APP_ERROR("[owned-cars] cannot set {} aside: {}", p.filename().string(),
                               ec.message());
            return false;
        }
        LARECOMP_APP_INFO("[owned-cars] {} kept as {}", p.filename().string(),
                          q.filename().string());
        return true;
    }
    LARECOMP_APP_ERROR("[owned-cars] too many set-aside files beside {}", p.string());
    return false;
}

bool ReadHeader(const fs::path& path, SidecarHeader& h) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    h = SidecarHeader{};
    in.read(reinterpret_cast<char*>(&h), std::streamsize(kHeaderSizeV1));
    if (!in || std::memcmp(h.magic, kSidecarMagic, sizeof(h.magic)) != 0) return false;
    if (h.version == 2) {
        in.read(reinterpret_cast<char*>(&h) + kHeaderSizeV1,
                std::streamsize(sizeof(SidecarHeader) - kHeaderSizeV1));
        if (!in) return false;
    } else if (h.version == 1) {
        h.line = 0;
        h.serial = 0;
    } else {
        return false;
    }
    return h.car_size == kCarSize && h.damage_size == kDamageSize && h.total >= h.in_save &&
           h.in_save <= kGameCap && h.total <= kMaxCap;
}

// The cars past in_save and their damage.
bool ReadBody(const Generation& g, std::vector<uint8_t>& cars, std::vector<uint8_t>& damage) {
    std::ifstream in(g.path, std::ios::binary);
    if (!in) return false;
    in.seekg(std::streamoff(HeaderSize(g.h)));
    const uint32_t extra = g.h.total - g.h.in_save;
    cars.resize(size_t(extra) * kCarSize);
    damage.resize(size_t(extra) * kDamageSize);
    if (extra) {
        in.read(reinterpret_cast<char*>(cars.data()), std::streamsize(cars.size()));
        in.read(reinterpret_cast<char*>(damage.data()), std::streamsize(damage.size()));
    }
    if (!in) {
        LARECOMP_APP_WARN("[owned-cars] {} is cut short, not used", g.path.filename().string());
        return false;
    }
    return true;
}

// Every owned-car file beside the save, the unfinished ones too (a .tmp counts
// only when it is complete).
std::vector<Generation> ListGenerations(const fs::path& file) {
    std::vector<Generation> out;
    std::error_code ec;
    const std::string base = file.filename().string();
    for (fs::directory_iterator it(file.parent_path(), ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string name = it->path().filename().string();
        if (name.compare(0, base.size(), base) != 0) continue;
        Generation g;
        g.path = it->path();
        if (!ReadHeader(g.path, g.h)) {
            LARECOMP_APP_WARN("[owned-cars] {} is not an owned-car file of this version, left "
                              "as it is", name);
            continue;
        }
        const std::string suffix = name.substr(base.size());
        g.rank = suffix.empty() ? 0 : suffix == ".loaded" ? 1 : suffix == ".prev" ? 2 : 3;
        out.push_back(std::move(g));
    }
    return out;
}

// Newest first; among the same serialize number the main file, then the pin.
bool Preferred(const Generation& a, const Generation& b) {
    if (a.h.serial != b.h.serial) return a.h.serial > b.h.serial;
    return a.rank < b.rank;
}

// The whole file in memory: header, the cars past in_save, their damage.
std::vector<uint8_t> Blob(const SidecarHeader& h, const uint8_t* cars_a, uint32_t n_a,
                          const uint8_t* cars_b, uint32_t n_b, const uint8_t* dmg_a,
                          const uint8_t* dmg_b) {
    std::vector<uint8_t> blob(sizeof(h) + size_t(n_a + n_b) * (kCarSize + kDamageSize));
    size_t o = 0;
    std::memcpy(blob.data(), &h, sizeof(h));
    o += sizeof(h);
    if (n_a) std::memcpy(blob.data() + o, cars_a, size_t(n_a) * kCarSize);
    o += size_t(n_a) * kCarSize;
    if (n_b) std::memcpy(blob.data() + o, cars_b, size_t(n_b) * kCarSize);
    o += size_t(n_b) * kCarSize;
    if (n_a) std::memcpy(blob.data() + o, dmg_a, size_t(n_a) * kDamageSize);
    o += size_t(n_a) * kDamageSize;
    if (n_b) std::memcpy(blob.data() + o, dmg_b, size_t(n_b) * kDamageSize);
    return blob;
}

bool WriteOnce(const fs::path& path, const std::vector<uint8_t>& blob) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(blob.data()), std::streamsize(blob.size()));
    out.flush();
    const bool ok = bool(out);
    out.close();
    return ok && !out.fail();
}

// A second try covers a file held open for a moment by another program.
bool WriteWhole(const fs::path& path, const std::vector<uint8_t>& blob) {
    return WriteOnce(path, blob) || WriteOnce(path, blob);
}

// Writes the cars past the 30th for the profile just serialized. Nothing beside
// the save is replaced unless the new file is complete and what it replaces is
// held by another file (the pin) or set aside first; on any failure the files
// stay as they were and the next save tries again.
void WriteSidecar(const uint8_t* b, uint32_t total, uint32_t in_save, int32_t saved_cur,
                  int32_t real_cur) {
    const fs::path file = SidecarPath();
    const uint32_t over = OverCount();
    if (file.empty()) {
        if (total > in_save || over)
            LARECOMP_APP_ERROR("[owned-cars] no profile folder for {}: {} cars past the 30th "
                               "are not saved", kSidecarName, total - in_save + over);
        return;
    }
    FollowProfile(file);
    if (!g_line.id) {
        g_line.id = NewLineId();
        g_line.serial = 0;
    }
    SidecarHeader h{};
    std::memcpy(h.magic, kSidecarMagic, sizeof(h.magic));
    h.version = kSidecarVersion;
    h.car_size = kCarSize;
    h.damage_size = kDamageSize;
    h.total = total + over;
    h.in_save = in_save;
    h.saved_cur = saved_cur;
    h.real_cur = real_cur;
    h.save_hash = SaveHash(b, in_save);
    h.line = g_line.id;
    h.serial = g_line.serial + 1;
    const uint32_t extra = total - in_save;
    const std::vector<uint8_t> blob =
        Blob(h, b + g_cars.load() + in_save * kCarSize, extra, g_over_cars.data(), over,
             b + g_damage.load() + in_save * kDamageSize, g_over_damage.data());

    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path tmp = WithSuffix(file, ".tmp");
    if (!WriteWhole(tmp, blob)) {
        LARECOMP_APP_ERROR("[owned-cars] cannot write {}: the files beside the save were left as "
                           "they were, the next save tries again", tmp.string());
        return;
    }

    // Make room: .prev goes unless it is a copy of the pin, then the main file
    // becomes .prev (or is set aside when another line wrote it).
    const fs::path prev = WithSuffix(file, ".prev");
    const fs::path pin = WithSuffix(file, ".loaded");
    SidecarHeader ph{}, mh{}, lh{};
    if (fs::exists(prev, ec)) {
        const bool pinned = ReadHeader(prev, ph) && ph.line && ph.line == g_line.id &&
                            ReadHeader(pin, lh) && lh.line == ph.line && lh.serial == ph.serial;
        if (!pinned && !SetAside(prev)) {
            LARECOMP_APP_ERROR("[owned-cars] {} kept, nothing replaced", tmp.filename().string());
            return;
        }
    }
    if (fs::exists(file, ec)) {
        if (ReadHeader(file, mh) && mh.line && mh.line == g_line.id) {
            fs::rename(file, prev, ec);
            if (ec) {
                LARECOMP_APP_ERROR("[owned-cars] cannot keep {} as {}: {}; {} kept, nothing "
                                   "replaced", file.filename().string(), prev.filename().string(),
                                   ec.message(), tmp.filename().string());
                return;
            }
        } else if (!SetAside(file)) {
            LARECOMP_APP_ERROR("[owned-cars] {} kept, nothing replaced", tmp.filename().string());
            return;
        }
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        LARECOMP_APP_ERROR("[owned-cars] cannot put {} in place: {} ({} kept)", file.string(),
                           ec.message(), tmp.filename().string());
        return;
    }
    g_line.serial = h.serial;
    LARECOMP_APP_INFO("[owned-cars] saved {} cars: {} in mc4.sav, {} in {} (current {} -> {} "
                      "in mc4.sav, {} past the limit kept, serialize {})", total, in_save, extra,
                      file.filename().string(), real_cur, saved_cur, over, h.serial);
}

// Keeps a copy of the file the loaded save matched as .loaded. The pin it
// replaces belongs to the same line and is not what the save on disk uses (it is
// another of its generations), or it is set aside: always when it belongs to
// another line, and when `keep_old` says it matches the save too.
void PinLoaded(const fs::path& file, const Generation& g, const std::vector<uint8_t>& cars,
               const std::vector<uint8_t>& damage, bool keep_old) {
    const fs::path pin = WithSuffix(file, ".loaded");
    if (g.path == pin && g.h.line == g_line.id) return;
    std::error_code ec;
    if (fs::exists(pin, ec)) {
        SidecarHeader old{};
        const bool same_line = ReadHeader(pin, old) && old.line && old.line == g_line.id;
        if ((!same_line || keep_old) && !SetAside(pin)) {
            LARECOMP_APP_ERROR("[owned-cars] {} not pinned", g.path.filename().string());
            return;
        }
    }
    SidecarHeader h = g.h;
    h.version = kSidecarVersion;
    h.line = g_line.id;
    const uint32_t extra = h.total - h.in_save;
    const std::vector<uint8_t> blob =
        Blob(h, cars.data(), extra, nullptr, 0, damage.data(), nullptr);
    const fs::path tmp = WithSuffix(pin, ".tmp");
    if (!WriteWhole(tmp, blob)) {
        LARECOMP_APP_ERROR("[owned-cars] cannot write {}: {} not pinned", tmp.string(),
                           g.path.filename().string());
        return;
    }
    fs::rename(tmp, pin, ec);
    if (ec)
        LARECOMP_APP_ERROR("[owned-cars] cannot put {} in place: {}", pin.string(), ec.message());
}

// Once a load proved which file the save on disk uses, the files the same line
// set aside are not used by it.
void RemoveSetAside(const std::vector<Generation>& gens, const Generation& used) {
    if (larecomp::settings::owned_cars_keep_all_generations.Get() || !g_line.id) return;
    std::error_code ec;
    for (const Generation& g : gens) {
        if (g.rank != 3 || g.h.line != g_line.id || g.path == used.path) continue;
        const std::string name = g.path.filename().string();
        if (name.find(".orphan") == std::string::npos) continue;
        if (fs::remove(g.path, ec))
            LARECOMP_APP_INFO("[owned-cars] removed {} (serialize {} of this line, not the one on "
                              "disk)", name, g.h.serial);
    }
}

// Puts a file's cars past in_save after the save's own, up to the limit; the
// rest wait in the overflow for the files written next.
uint32_t AppendCars(uint8_t* b, uint32_t in_save, const std::vector<uint8_t>& cars,
                    const std::vector<uint8_t>& damage) {
    const uint32_t extra = uint32_t(cars.size() / kCarSize);
    const uint32_t room = owned_cars::Cap() - in_save;
    const uint32_t take = std::min(extra, room);
    if (take) {
        std::memcpy(b + g_cars.load() + in_save * kCarSize, cars.data(), size_t(take) * kCarSize);
        std::memcpy(b + g_damage.load() + in_save * kDamageSize, damage.data(),
                    size_t(take) * kDamageSize);
    }
    g_over_cars.assign(cars.begin() + std::ptrdiff_t(size_t(take) * kCarSize), cars.end());
    g_over_damage.assign(damage.begin() + std::ptrdiff_t(size_t(take) * kDamageSize), damage.end());
    if (extra > take)
        LARECOMP_APP_ERROR("[owned-cars] the save has {} cars, owned_cars_limit allows {}: the {} "
                           "past it stay out of the game and in every file saved from now on",
                           in_save + extra, owned_cars::Cap(), extra - take);
    return in_save + take;
}

// A save made while the option was off owns at most 30 cars: exactly 30 when it
// was only saved again, fewer when a car was sold meanwhile. The newest file of
// another line with more than 30 may be that save's. `require_full` keeps the
// automatic check (a save that did match a file) to the 30-car case.
const Generation* Recoverable(const std::vector<Generation>& gens, uint32_t in_save,
                              uint64_t skip_line, bool require_full) {
    if (in_save > kGameCap || (require_full && in_save != kGameCap)) return nullptr;
    for (const Generation& g : gens)  // sorted: the first is the newest
        if (g.h.in_save == kGameCap && g.h.total > kGameCap && (!skip_line || g.h.line != skip_line))
            return &g;
    return nullptr;
}

// owned_cars_recover_unmatched: adds `best`'s cars past the 30th, once per start.
// `adopt` makes its line this save's when the save matched none.
bool TryRecover(uint8_t* b, uint32_t in_save, const Generation& best,
                const std::vector<Generation>& gens, bool adopt) {
    if (!larecomp::settings::owned_cars_recover_unmatched.Get() || g_recovered) {
        LARECOMP_APP_WARN("[owned-cars] {} holds {} cars past the 30th of another save (perhaps "
                          "this one, saved with the option off; it now owns {}). If they are this "
                          "save's, start once with owned_cars_recover_unmatched = true",
                          best.path.filename().string(), best.h.total - kGameCap, in_save);
        return false;
    }
    std::vector<uint8_t> cars, damage;
    if (!ReadBody(best, cars, damage)) return false;
    g_recovered = true;
    if (adopt) {
        g_line.id = best.h.line ? best.h.line : NewLineId();
        g_line.serial = best.h.serial;
        for (const Generation& o : gens)
            if (o.h.line == g_line.id) g_line.serial = std::max(g_line.serial, o.h.serial);
    }
    const uint32_t count = AppendCars(b, in_save, cars, damage);
    WriteGuestU32(b, kCount, count);
    LARECOMP_APP_WARN("[owned-cars] owned_cars_recover_unmatched: {} cars past the 30th added from "
                      "{}; turn the option off again", count - in_save,
                      best.path.filename().string());
    return true;
}

// After the load: finds the file the save matches and appends its cars.
// Returns where they came from.
std::string LoadSidecar(uint8_t* b, uint32_t in_save, int32_t cur) {
    const fs::path file = SidecarPath();
    if (file.empty()) return "none (no profile folder)";
    FollowProfile(file);
    StartNewLine("load");
    std::vector<Generation> gens = ListGenerations(file);
    std::sort(gens.begin(), gens.end(), Preferred);
    const uint64_t hash = SaveHash(b, in_save);

    // The hash covers the 30 slots mc4.sav holds, so two files can match one save
    // and still differ in the cars past the 30th (saved twice with only those
    // changed, the second write never reaching the disk). The newest is used;
    // when they differ nothing that matched is replaced or removed.
    struct Match {
        const Generation* g;
        std::vector<uint8_t> cars, damage;
        uint64_t body;
    };
    std::vector<Match> matches;
    for (const Generation& g : gens) {
        if (g.h.in_save != in_save || g.h.save_hash != hash) continue;
        Match m{&g, {}, {}, 0};
        if (!ReadBody(g, m.cars, m.damage)) continue;
        uint8_t tail[8];
        std::memcpy(tail, &g.h.total, 4);
        std::memcpy(tail + 4, &g.h.real_cur, 4);
        m.body = Fnv1a(Fnv1a(Fnv1a(kFnvBasis, m.cars.data(), m.cars.size()), m.damage.data(),
                             m.damage.size()), tail, sizeof(tail));
        matches.push_back(std::move(m));
    }
    if (!matches.empty()) {
        const Match& m = matches.front();
        const Generation& g = *m.g;
        bool differ = false, pin_differs = false;
        for (const Match& o : matches)
            if (o.body != m.body) {
                differ = true;
                if (o.g->rank == 1) pin_differs = true;
            }
        if (differ)
            LARECOMP_APP_WARN("[owned-cars] {} files match this save with different cars past the "
                              "30th; using {} (serialize {}), every one of them is kept",
                              matches.size(), g.path.filename().string(), g.h.serial);
        g_line.id = g.h.line ? g.h.line : NewLineId();
        g_line.serial = g.h.serial;
        for (const Generation& o : gens)
            if (o.h.line == g_line.id) g_line.serial = std::max(g_line.serial, o.h.serial);
        const uint32_t count = AppendCars(b, in_save, m.cars, m.damage);
        WriteGuestU32(b, kCount, count);
        // The current slot comes back only if the save still has the one written.
        if (cur == g.h.saved_cur && g.h.real_cur >= 0 && uint32_t(g.h.real_cur) < count)
            WriteGuestU32(b, kCurrent, uint32_t(g.h.real_cur));
        PinLoaded(file, g, m.cars, m.damage, pin_differs);
        if (!differ) RemoveSetAside(gens, g);
        // Matched by a file with no car past the 30th: the save may have been
        // made with the option off after a line that had more.
        if (g.h.total == kGameCap && in_save == kGameCap)
            if (const Generation* best = Recoverable(gens, in_save, g_line.id, true))
                if (TryRecover(b, in_save, *best, gens, false))
                    return g.path.filename().string() + ", " + best->path.filename().string() +
                           " (recovered)";
        return g.path.filename().string();
    }

    // No file is this save's.
    if (const Generation* best = Recoverable(gens, in_save, 0, false)) {
        if (TryRecover(b, in_save, *best, gens, true))
            return best->path.filename().string() + " (recovered)";
    } else if (!gens.empty()) {
        LARECOMP_APP_WARN("[owned-cars] {} owned-car file(s) beside the save, none of them for "
                          "it: left as they are", gens.size());
    }
    return "none";
}

// Test help: copies the cars the save already owns, in turn, until it owns
// `want` of them.
void TestFill(uint8_t* b) {
    const int32_t want_cvar = larecomp::settings::owned_cars_test_fill.Get();
    if (want_cvar <= 0) return;
    const uint32_t want = std::min<uint32_t>(uint32_t(want_cvar), owned_cars::Cap());
    const uint32_t count = OwnedCount(b);
    if (count == 0 || count >= want) return;
    const uint32_t cars = g_cars.load(), damage = g_damage.load();
    for (uint32_t i = count; i < want; ++i) {
        const uint32_t src = (i - count) % count;
        std::memcpy(b + cars + i * kCarSize, b + cars + src * kCarSize, kCarSize);
        std::memcpy(b + damage + i * kDamageSize, g_blank_damage.data(), kDamageSize);
    }
    WriteGuestU32(b, kCount, want);
    LARECOMP_APP_INFO("[owned-cars] test fill: slots 0..{} copied in turn, {} -> {} cars",
                      count - 1, count, want);
}

enum SiteIndex : size_t {
#define OWNED_CARS_SITE(kind, addr, reg) kSite_##addr,
#define OWNED_CARS_SITE_OWNER(kind, addr, reg, owner_reg) kSite_##addr,
#include "owned_cars_sites.inc"
#undef OWNED_CARS_SITE
#undef OWNED_CARS_SITE_OWNER
    kSiteCount
};
std::array<std::atomic<bool>, kSiteCount> g_site_seen{};

void FirstAccess(size_t site, const char* name) {
    if (!g_site_seen[site].exchange(true, std::memory_order_relaxed))
        LARECOMP_APP_DEBUG("[owned-cars] first access moved at {}", name);
}

void MoveBy(PPCRegister& reg, uint32_t delta) {
    reg.u64 = static_cast<uint32_t>(reg.u32 + delta);
}

// OWNED_CARS_SITE(CAR, ...): an address (or the product about to become one)
// into the car slots.
void MoveCAR(PPCRegister& reg, size_t site, const char* name) {
    const uint32_t d = g_car_delta.load(std::memory_order_relaxed);
    if (!d) return;
    MoveBy(reg, d);
    FirstAccess(site, name);
}

// OWNED_CARS_SITE(DAMAGE, ...): the same into the damage slots.
void MoveDAMAGE(PPCRegister& reg, size_t site, const char* name) {
    if (!g_car_delta.load(std::memory_order_relaxed)) return;
    MoveBy(reg, g_damage_delta.load(std::memory_order_relaxed));
    FirstAccess(site, name);
}

void NotePlatesOwner(uint32_t owner, const char* name) {
    const uint32_t was = g_plates_owner.exchange(owner, std::memory_order_relaxed);
    if (was == owner) return;
    const uint32_t n = g_plates_owners.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!was)
        LARECOMP_APP_DEBUG("[owned-cars] plate bytes of {:08X} moved (first seen at {})", owner, name);
    else
        LARECOMP_APP_WARN("[owned-cars] plate bytes now of {:08X}, was {:08X} (seen at {}, {} "
                          "owners so far): they share the moved array", owner, was, name, n);
}

// OWNED_CARS_SITE(PLATES, ...): reg holds owner+137972, the plate bytes.
void MovePLATES(PPCRegister& reg, size_t site, const char* name) {
    const uint32_t plates = g_plates.load(std::memory_order_relaxed);
    if (!plates || !g_car_delta.load(std::memory_order_relaxed)) return;
    NotePlatesOwner(reg.u32 - kPlatesOffset, name);
    reg.u64 = plates;
    FirstAccess(site, name);
}

// OWNED_CARS_SITE_OWNER(PLATES, ...): reg is owner+slot, read with +137972.
void MoveOwnerPLATES(PPCRegister& reg, const PPCRegister& owner, size_t site, const char* name) {
    const uint32_t plates = g_plates.load(std::memory_order_relaxed);
    if (!plates || !g_car_delta.load(std::memory_order_relaxed)) return;
    NotePlatesOwner(owner.u32, name);
    MoveBy(reg, plates - (owner.u32 + kPlatesOffset));
    FirstAccess(site, name);
}

// Room for one more car: the gates below jump past the game's `cmpwi 30` then.
bool Room(int32_t count) {
    return g_car_delta.load(std::memory_order_relaxed) && count >= 0 &&
           uint32_t(count) < owned_cars::Cap();
}

}  // namespace

namespace owned_cars {

uint32_t Cap() {
    static const uint32_t cap = ComputeCap();
    return cap;
}

bool Enabled() { return g_car_delta.load(std::memory_order_relaxed) != 0; }

uint32_t CarAddress(uint32_t slot) {
    if (!Enabled()) return kCars + slot * kCarSize;
    return slot < Cap() ? g_cars.load(std::memory_order_relaxed) + slot * kCarSize : 0;
}

void AfterReset(uint8_t* base, uint32_t parts_room) {
    if (Cap() <= kGameCap || !base) return;
    TakeBlank(base, "reset");
    EnsureBlock(base, "reset", parts_room);
}

// The game's new-game loop filled slots 0..29 (moved); the rest go blank too.
// The new game is a line of saves of its own: the files of the old one are set
// aside by its first save.
void AfterNewGame(uint8_t* base) {
    if (!g_cars.load()) return;
    FillSpare(base, kGameCap);
    StartNewLine("new game");
    LARECOMP_APP_INFO("[owned-cars] new game: slots 30..{} blank", Cap() - 1);
}

// sub_822D20D8 runs sub_8238B3F8 (its loop writes the vehicle id into +8088 of
// every owned car, in the block) and then copies the profile into the save
// buffer (sub_8238D240). In between, called from the wrapper of sub_8238B3F8,
// the mirror gets the first 30 cars and their damage.
void BeforeProfileCopy(uint8_t* base) {
    if (!g_cars.load()) return;
    CheckMirror(base, "save");
    std::memcpy(base + kCars, base + g_cars.load(), size_t(kGameCap) * kCarSize);
    std::memcpy(base + kDamage, base + g_damage.load(), size_t(kGameCap) * kDamageSize);
    g_profile_copied = true;
}

// The buffer (r4 of sub_822D20D8, room r5) is the profile byte for byte, no
// header (sub_8238D240 copies 254032 bytes from the profile to it).
void AfterSerialize(uint8_t* base, uint32_t buffer, uint32_t room) {
    if (!g_cars.load()) return;
    if (!g_profile_copied)
        LARECOMP_APP_ERROR("[owned-cars] the profile was serialized without the pre-save pass: "
                           "mc4.sav got the mirror as it was");
    g_profile_copied = false;
    const uint32_t total = OwnedCount(base);
    const uint32_t in_save = std::min(total, kGameCap);
    const int32_t real_cur = ReadS32(base, kCurrent);
    int32_t saved_cur = real_cur;
    if (saved_cur >= int32_t(kGameCap)) saved_cur = 0;  // what AddCar sets with no current car
    if (buffer && room >= kSavedSize) {
        if (total > in_save) WriteGuestU32(base, buffer + (kCount - kProfile), in_save);
        if (saved_cur != real_cur)
            WriteGuestU32(base, buffer + (kCurrent - kProfile), uint32_t(saved_cur));
    } else if (total > in_save || saved_cur != real_cur) {
        LARECOMP_APP_ERROR("[owned-cars] save buffer {:08X} has room for {} bytes, not {}: its "
                           "count was left at {} and its current slot at {}", buffer, room,
                           kSavedSize, total, real_cur);
    }
    WriteSidecar(base, total, in_save, saved_cur, real_cur);
    PoisonMirror(base);
    NoteMirror(base);
}

void BeforeLoad(uint8_t* base) {
    if (g_cars.load()) CheckMirror(base, "load");
}

// After mc4.sav's copy of the profile is in place: its 30 slots back into the
// block, then the cars the file beside it kept.
void AfterLoad(uint8_t* base, uint32_t parts_room) {
    if (Cap() <= kGameCap || !EnsureBlock(base, "load", parts_room)) return;

    int32_t count = ReadS32(base, kCount);
    if (count < 0) count = 0;
    if (count > int32_t(kGameCap)) {
        // A count the save's 30 slots cannot back: slots 30.. were read from the
        // parts table. Only the 30 real ones are kept.
        LARECOMP_APP_WARN("[owned-cars] this save counts {} cars in its 30 slots: kept 30", count);
        count = int32_t(kGameCap);
        WriteGuestU32(base, kCount, uint32_t(count));
    }
    const uint32_t in_save = uint32_t(count);
    const int32_t cur = ReadS32(base, kCurrent);

    std::memcpy(base + g_cars.load(), base + kCars, size_t(kGameCap) * kCarSize);
    std::memcpy(base + g_damage.load(), base + kDamage, size_t(kGameCap) * kDamageSize);
    FillSpare(base, kGameCap);

    const std::string from = LoadSidecar(base, in_save, cur);
    PoisonMirror(base);
    NoteMirror(base);
    TestFill(base);
    LARECOMP_APP_INFO("[owned-cars] loaded {} cars: {} from mc4.sav, the rest from {} (current {}, "
                      "{} past the limit kept aside)", OwnedCount(base), in_save, from,
                      ReadS32(base, kCurrent), OverCount());
}

}  // namespace owned_cars

// The places that form an owned-car address; each moves the register the
// instruction at its address just wrote.
#define OWNED_CARS_SITE(kind, addr, reg)                                       \
    void OwnedCars_##addr(PPCRegister& reg) { Move##kind(reg, kSite_##addr, #addr); }
#define OWNED_CARS_SITE_OWNER(kind, addr, reg, owner_reg)                      \
    void OwnedCars_##addr(PPCRegister& reg, PPCRegister& owner_reg) {          \
        MoveOwner##kind(reg, owner_reg, kSite_##addr, #addr);                  \
    }
#include "owned_cars_sites.inc"
#undef OWNED_CARS_SITE
#undef OWNED_CARS_SITE_OWNER

// Six of the seven `cmpwi 30` on the count. Each hook sits on the compare
// itself and, when the block has room, jumps where the game goes with fewer than
// 30 cars; the compared register is left as it was (in AddCar it is the new
// slot). With no room the compare runs as before: AddCar still pays the price
// out and returns -1, the menus still show the garage full.
bool OwnedCars_Gate_8238B714(PPCRegister& r30) { return Room(r30.s32); }  // AddCar
bool OwnedCars_Gate_826AFA70(PPCRegister& r11) { return Room(r11.s32); }  // showroom purchase check
bool OwnedCars_Gate_826AF660(PPCRegister& r11) { return Room(r11.s32); }  // purchase with trade-in check
bool OwnedCars_Gate_826A9AA4(PPCRegister& r11) { return Room(r11.s32); }  // finish trade-in
bool OwnedCars_Gate_826A9C7C(PPCRegister& r11) { return Room(r11.s32); }  // finish purchase
bool OwnedCars_Gate_826B00DC(PPCRegister& r11) { return Room(r11.s32); }  // Rate My Ride purchase
// The new-game loop (0x8238C2D8) keeps its 30: it fills slots 0..29 through the
// moved base and AfterNewGame blanks the rest.

#else  // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.
#include <rex/ppc/context.h>

#include <cstdint>

#include "owned_cars.h"

namespace owned_cars {
uint32_t Cap() { return kGameCap; }
bool Enabled() { return false; }
uint32_t CarAddress(uint32_t slot) { return 0x8288E920 + 1360 + slot * 8176; }
void AfterReset(uint8_t*, uint32_t) {}
void AfterNewGame(uint8_t*) {}
void BeforeProfileCopy(uint8_t*) {}
void AfterSerialize(uint8_t*, uint32_t, uint32_t) {}
void BeforeLoad(uint8_t*) {}
void AfterLoad(uint8_t*, uint32_t) {}
}  // namespace owned_cars

#define OWNED_CARS_SITE(kind, addr, reg) void OwnedCars_##addr(PPCRegister&) {}
#define OWNED_CARS_SITE_OWNER(kind, addr, reg, owner_reg) \
    void OwnedCars_##addr(PPCRegister&, PPCRegister&) {}
#include "owned_cars_sites.inc"
#undef OWNED_CARS_SITE
#undef OWNED_CARS_SITE_OWNER
bool OwnedCars_Gate_8238B714(PPCRegister&) { return false; }
bool OwnedCars_Gate_826AFA70(PPCRegister&) { return false; }
bool OwnedCars_Gate_826AF660(PPCRegister&) { return false; }
bool OwnedCars_Gate_826A9AA4(PPCRegister&) { return false; }
bool OwnedCars_Gate_826A9C7C(PPCRegister&) { return false; }
bool OwnedCars_Gate_826B00DC(PPCRegister&) { return false; }
#endif  // REXGLUE_HAS_XEO3_TARGET
