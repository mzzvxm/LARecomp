#ifndef REXGLUE_HAS_XEO3_TARGET
// Serves the garage tune fixes (garage_fixes.h) at the moment the game opens
// the file, from guest memory. Nothing is written to disk.
//
//   sub_821CA6A8  fiAssetManager open: r4 = name without folder, r5 = ext,
//                 r7 = read only. Returns an fiStream*, 0 when nothing opened.
//   sub_821BDF20  fiStream::Open(path, read only). A path of the form
//                 "memory:$<hex address>,<size>,<free on close>:<name>" goes to
//                 the game's own fiDeviceMemory (open at 0x821CAE50).
//   sub_821BE610  fiStream::Close.
//   fiStream      +0 device, +4 handle. Device vtable +24 is
//                 Read(handle, buffer, length).
//
// The game opens these files each time a garage menu is built, so the original
// is read and amended once; the amended text then stays in guest memory.
#include "garage_fixes.h"

#include "larecomp_log.h"
#include "mc_engine/modloader/modloader.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "settings/larecomp_settings.h"
#include <rex/ppc/recompiled_code.h>
#include <rex/ppc/context.h>
#include "larecomp_guest.h"
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/function_dispatcher.h>

REX_EXTERN(__imp__rex_sub_821CA6A8);  // asset manager open

namespace {

constexpr uint32_t kStreamOpen = 0x821BDF20;
constexpr uint32_t kStreamClose = 0x821BE610;
constexpr uint32_t kDeviceRead = 24;        // vtable offset
constexpr uint32_t kChunk = 4096;
constexpr size_t kMaxText = 64 * 1024;      // the largest of these is ~2 KB
constexpr uint32_t kPathRoom = 160;

struct Served {
    uint32_t path = 0;  // guest "memory:..." string, followed by the text
};

std::mutex g_mutex;
std::map<std::string, Served> g_served;     // by archive path
uint32_t g_scratch = 0;                     // read buffer, kChunk bytes

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

uint32_t Load32(const uint8_t* base, uint32_t ea) {
    const uint8_t* p = base + ea;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

std::string GuestString(const uint8_t* base, uint32_t ea) {
    std::string out;
    if (!ea) return out;
    for (int i = 0; i < 256 && base[ea + i]; ++i) out.push_back(char(base[ea + i]));
    return out;
}

// The fix this open is asking for, as its archive path; empty for every other
// file. `name` and `ext` are what the asset manager was handed.
std::string WantedFix(const uint8_t* base, uint32_t name, uint32_t ext) {
    if (Lower(GuestString(base, ext)) != "gtun") return {};
    std::string leaf = Lower(GuestString(base, name));
    if (const size_t slash = leaf.find_last_of("/\\"); slash != std::string::npos)
        leaf.erase(0, slash + 1);
    if (leaf.size() < 5 || leaf.compare(leaf.size() - 5, 5, ".gtun") != 0)
        leaf += ".gtun";
    static const std::vector<std::string> paths = mc::garage_fixes::Paths();
    for (const std::string& path : paths)
        if (Lower(path.substr(path.rfind('/') + 1)) == leaf) return path;
    return {};
}

// Calls a guest function on the thread's own register file, which comes back as
// it was (larecomp_guest.h: a copy of the context is not safe on this SDK).
PPCFunc* GuestFunction(uint32_t address) {
    auto* rt = REX_RUNTIME();
    return rt ? rt->FindFunction(address).value_or(nullptr) : nullptr;
}

uint32_t CallGuest(uint32_t address, uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0) {
    return larecomp::guest::Call<uint32_t>(GuestFunction(address), a0, a1, a2);
}

// Reads a stream that was just opened, to its end, through its device.
bool ReadAll(uint8_t* base, uint32_t stream, std::string& out) {
    const uint32_t device = Load32(base, stream);
    const uint32_t handle = Load32(base, stream + 4);
    PPCFunc* read = GuestFunction(Load32(base, Load32(base, device) + kDeviceRead));
    if (!read) return false;
    while (out.size() <= kMaxText) {
        const int32_t got =
            larecomp::guest::Call<int32_t>(read, device, handle, g_scratch, kChunk);
        if (got <= 0) return !out.empty();
        if (got > int32_t(kChunk)) return false;
        out.append(reinterpret_cast<const char*>(base + g_scratch), size_t(got));
    }
    return false;
}

// Puts the amended text in guest memory and builds the path that opens it.
bool Keep(uint8_t* base, const std::string& archive_path, const std::string& text) {
    auto* memory = larecomp::guest::AddressSpace();
    if (!memory) return false;
    const uint32_t block =
        memory->SystemHeapAlloc(kPathRoom + uint32_t(text.size()), 0x10);
    if (!block) return false;
    const uint32_t data = block + kPathRoom;
    std::memcpy(base + data, text.data(), text.size());
    const std::string leaf = archive_path.substr(archive_path.rfind('/') + 1);
    std::snprintf(reinterpret_cast<char*>(base + block), kPathRoom,
                  "memory:$%08X,%u,0:%s", data, unsigned(text.size()), leaf.c_str());
    g_served[archive_path].path = block;
    return true;
}

uint32_t OpenServed(const Served& served) {
    return CallGuest(kStreamOpen, served.path, 1 /* read only */);
}

}  // namespace

extern "C" REX_FUNC(rex_sub_821CA6A8) {
    const std::string path =
        larecomp::settings::garage_menu_fixes.Get() && (ctx.r7.u32 & 0xFF)
            ? WantedFix(base, ctx.r4.u32, ctx.r5.u32)
            : std::string();
    // A file a mod ships is the mod's to keep.
    if (path.empty() || !mc::modloader::ModRawFile(path).empty()) {
        __imp__rex_sub_821CA6A8(ctx, base);
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (const auto it = g_served.find(path); it != g_served.end()) {
        ctx.r3.u64 = OpenServed(it->second);
        return;
    }

    const PPCContext entry = ctx;  // the arguments, for a second open
    __imp__rex_sub_821CA6A8(ctx, base);
    const uint32_t original = ctx.r3.u32;
    const bool new_file = mc::garage_fixes::IsNewFile(path);
    if (new_file ? original != 0 : original == 0) return;

    std::string text;
    bool amended = false;
    if (new_file) {
        amended = mc::garage_fixes::Apply(path, text);
    } else {
        if (auto* memory = larecomp::guest::AddressSpace(); !g_scratch && memory)
            g_scratch = memory->SystemHeapAlloc(kChunk, 0x10);
        if (!g_scratch) return;  // the original is untouched, hand it over
        amended = ReadAll(base, original, text) &&
                  mc::garage_fixes::Apply(path, text);
        // Read or not, this stream has moved: it is not handed to the game.
        CallGuest(kStreamClose, original);
    }

    if (amended && Keep(base, path, text)) {
        ctx.r3.u64 = OpenServed(g_served[path]);
        if (ctx.r3.u32) {
            LARECOMP_APP_INFO("[garage-fix] {} served amended ({} bytes)", path,
                              text.size());
            return;
        }
        g_served.erase(path);
        LARECOMP_APP_WARN("[garage-fix] {}: the memory device refused it", path);
    } else {
        LARECOMP_APP_INFO("[garage-fix] {}: nothing to change", path);
    }

    if (!new_file) {  // give the game a fresh stream of its own file
        ctx = entry;  // the same register file, with the caller's arguments again
        __imp__rex_sub_821CA6A8(ctx, base);
    }
}

#endif  // REXGLUE_HAS_XEO3_TARGET
