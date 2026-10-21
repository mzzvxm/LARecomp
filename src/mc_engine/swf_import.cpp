// A sprite of another movie, brought into the loaded ones. See swf_import.h
// and.

#include "swf_import.h"

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/ppc.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "hooks/hooks_internal.h"
#include "logging.h"
#include "modloader/rpf3.h"
#include "modloader/rsc5.h"
#include "modloader/xcompress.h"

REX_EXTERN(__imp__rex_sub_825FDA90);  // swfOBJECT::Fixup: relocate, then the class vtable
REX_EXTERN(__imp__rex_sub_825FB0D8);  // swfINSTANCE update (recursive)
REX_EXTERN(__imp__rex_sub_8260B038);  // PlaceObject2 execute (command vtable +4)
REX_EXTERN(__imp__rex_sub_8260B250);  // named PlaceObject2 execute
REX_EXTERN(__imp__rex_sub_8260AA48);  // RemoveObject2 execute

namespace larecomp::swf {
namespace {

// A resource's pointers before fixup: the virtual segment sits at 0x50000000.
constexpr uint32_t kVirtualBase = 0x50000000;
// dword_828FF990: the file PlaceObject2 resolves characters in. The movie
// update (sub_825EDC08) and draw (sub_825EE428) set it around their walks and
// clear it after.
constexpr uint32_t kCurrentFile = 0x828FF990;
// *(r13) + 0x3C: the resource being fixed up. swfOBJECT's own fixup
// (sub_821D2970 -> sub_821B5A60) relocates the object's +4 against it, and
// registers the object with it, when it is set; when it is clear +4 is zeroed
// and nothing is registered -- what a character, whose +4 is 0, wants.
constexpr uint32_t kTlsResource = 0x3C;

// swfFILE (vtable 0x82085364), the movie resource's root object. Of it the
// player reads, for the characters of a file, +24 (PlaceObject2), +29 (the
// update's input pass) and +50 (sub_825ED5F8's font search).
constexpr uint32_t kFileVtable  = 0x82085364;
constexpr uint32_t kFileChars   = 24;  // character table
constexpr uint32_t kFileVersion = 28;  // u8, SWF version
constexpr uint32_t kFileInput   = 29;  // u8, input pass in the update; 0 once loaded
constexpr uint32_t kFileWidth   = 36;  // float, stage size
constexpr uint32_t kFileHeight  = 44;
constexpr uint32_t kFileRate    = 48;  // u16, frames per second * 256
constexpr uint32_t kFileCount   = 50;  // u16, characters in the table
constexpr uint32_t kFileBytes   = 64;

// Characters.
constexpr uint32_t kCharType     = 8;
constexpr uint8_t  kShape        = 1;
constexpr uint8_t  kSprite       = 2;
constexpr uint32_t kShapeVtable  = 0x820884E4;
constexpr uint32_t kSpriteVtable = 0x8208521C;
// sprite: +12 frames (8 bytes each, +4 the first command), +16 u16 frame count.
// shape:  +12 fills (40 bytes each: +32 bitmap, +36 gradient, whose +24 and
//         +28 are pointers too), +16 line styles, +20 edges, +28 u16 fills.
constexpr uint32_t kSpriteFrames     = 12;
constexpr uint32_t kSpriteFrameCount = 16;
constexpr uint32_t kShapeFills       = 12;
constexpr uint32_t kShapeLines       = 16;
constexpr uint32_t kShapeEdges       = 20;
constexpr uint32_t kShapeFillCount   = 28;
constexpr uint32_t kFillBytes        = 40;

// Timeline commands: +4 u8 type, +12 next. PlaceObject2 (0, and 1 named):
// +16 matrix, +20 u16 depth, +24 u16 character, +28 color transform; named
// also +52 clip actions (12 bytes each, +4 a pointer), +56 their count, +60
// name. RemoveObject2 (2): +6 u16 depth. DoAction (3) / InitAction (4): +16
// bytecode. Anything else is fatal to the fixup (sub_8260B5F8).
constexpr uint32_t kCmdType  = 4;
constexpr uint32_t kCmdNext  = 12;
constexpr uint32_t kCmdPlaceVtable  = 0x82088784;
constexpr uint32_t kCmdNamedVtable  = 0x8208883C;
constexpr uint32_t kCmdRemoveVtable = 0x820887EC;

constexpr uint32_t kInstChar = 4;  // an instance's character

constexpr const char* kSourceArchive = "xarchive_cache.rpf";
// Copied past the last block the fixup knows the start of, for what that
// block runs on into (line styles and gradient records carry no length the
// fixup reads).
constexpr uint32_t kTailSlack = 16 * 1024;
constexpr uint32_t kScratchBlock = 4096;
constexpr int kMaxImports = 4;

// The bytes the fixup of one character, and of every character it places,
// touches -- walked on the host copy the way sub_825FDA90 walks them, so the
// guest copy can be made to cover every pointer it moves. A pointer it moves
// that the page map does not cover is fatal ("Invalid fixup", sub_821D2378).
class Closure {
 public:
    Closure(const uint8_t* data, uint32_t size) : d_(data), size_(size) {}

    bool Add(int index) {
        if (!chars_.insert(index).second) return true;
        const uint32_t table = U32(kVirtualBase + kFileChars);
        const uint32_t count = U16(kVirtualBase + kFileCount);
        if (index < 0 || uint32_t(index) >= count || !In(table + 4u * uint32_t(index), 4))
            return Fail("character #" + std::to_string(index) + " is not in the table");
        const uint32_t ch = U32(table + 4u * uint32_t(index));
        if (!Block(ch, 32, "character")) return false;
        switch (U8(ch + kCharType)) {
        case kSprite: return AddSprite(ch);
        case kShape:  return AddShape(ch);
        default:
            return Fail("character #" + std::to_string(index) + " is of type " +
                        std::to_string(U8(ch + kCharType)) + "; only shapes and sprites travel");
        }
    }

    uint32_t Char(int index) const {
        return U32(U32(kVirtualBase + kFileChars) + 4u * uint32_t(index));
    }
    uint8_t Type(int index) const { return U8(Char(index) + kCharType); }

    // [lo, hi): page-aligned start, slack past the end, inside the segment.
    void Finish() {
        lo_ &= ~0xFFFu;
        const uint64_t end = std::min<uint64_t>(uint64_t(hi_) + kTailSlack,
                                                uint64_t(kVirtualBase) + size_);
        hi_ = uint32_t(end);
    }
    bool Covers() const {
        for (const uint32_t p : pointers_)
            if (p < lo_ || p >= hi_) return false;
        return true;
    }

    const std::set<int>& chars() const { return chars_; }
    uint32_t lo() const { return lo_; }
    uint32_t hi() const { return hi_; }
    size_t pointer_count() const { return pointers_.size(); }
    const std::string& error() const { return error_; }

    uint8_t U8(uint32_t p) const { return In(p, 1) ? d_[p - kVirtualBase] : 0; }
    uint16_t U16(uint32_t p) const {
        return In(p, 2) ? uint16_t((d_[p - kVirtualBase] << 8) | d_[p - kVirtualBase + 1]) : 0;
    }
    uint32_t U32(uint32_t p) const {
        if (!In(p, 4)) return 0;
        const uint8_t* b = d_ + (p - kVirtualBase);
        return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
    }

 private:
    bool In(uint32_t p, uint32_t n) const {
        return p >= kVirtualBase && p - kVirtualBase <= size_ && n <= size_ - (p - kVirtualBase);
    }
    bool Fail(std::string why) {
        error_ = std::move(why);
        return false;
    }
    bool Block(uint32_t p, uint32_t n, const char* what) {
        if (!In(p, n)) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s at 0x%08X runs past the file", what, p);
            return Fail(buf);
        }
        lo_ = std::min(lo_, p);
        hi_ = std::max(hi_, p + n);
        return true;
    }
    // A pointer field the fixup moves when it is not 0, and `n` bytes it leads to.
    bool Pointer(uint32_t field, uint32_t n, const char* what, uint32_t* out = nullptr) {
        const uint32_t p = U32(field);
        if (out) *out = p;
        if (!p) return true;
        pointers_.push_back(p);
        return Block(p, n, what);
    }

    bool AddSprite(uint32_t ch) {
        const uint32_t nf = U16(ch + kSpriteFrameCount);
        uint32_t frames = 0;
        if (!Pointer(ch + kSpriteFrames, 8 * std::max<uint32_t>(nf, 1), "frames", &frames))
            return false;
        if (!frames) return true;
        for (uint32_t f = 0; f < nf; ++f) {
            // sub_8260A830: for (i = frame + 4; *i; i = *i + 12) relocate *i, fix it up.
            uint32_t link = frames + 8 * f + 4;
            for (int guard = 0;; ++guard) {
                const uint32_t cmd = U32(link);
                if (!cmd) break;
                if (guard > 4096) return Fail("a frame's command chain does not end");
                pointers_.push_back(cmd);
                if (!AddCommand(cmd)) return false;
                link = cmd + kCmdNext;
            }
        }
        return true;
    }

    bool AddCommand(uint32_t cmd) {
        if (!In(cmd, 8)) return Fail("command outside the file");
        const uint8_t type = U8(cmd + kCmdType);
        switch (type) {
        case 0:
        case 1: {
            if (!Block(cmd, type == 0 ? 0x24 : 0x44, "PlaceObject2")) return false;
            if (!Pointer(cmd + 16, 12, "matrix") || !Pointer(cmd + 28, 8, "color transform"))
                return false;
            if (type == 1) {
                // sub_8260B398: the array, then +4 of each entry.
                const uint32_t n = U32(cmd + 56);
                if (n > 256) return Fail("too many clip actions");
                uint32_t arr = 0;
                if (!Pointer(cmd + 52, 12 * std::max<uint32_t>(n, 1), "clip actions", &arr))
                    return false;
                for (uint32_t k = 0; arr && k < n; ++k)
                    if (!Pointer(arr + 12 * k + 4, 16, "clip action")) return false;
                const uint32_t name = U32(cmd + 60);
                if (name) {
                    uint32_t len = 0;
                    while (In(name + len, 1) && U8(name + len) && len < 256) ++len;
                    if (!Pointer(cmd + 60, len + 1, "name")) return false;
                }
            }
            const uint16_t placed = U16(cmd + 24);
            return placed == 0xFFFF || Add(placed);
        }
        case 2:
            return Block(cmd, 0x10, "RemoveObject2");
        case 3:
        case 4:
            return Block(cmd, 0x14, "DoAction") && Pointer(cmd + 16, 16, "actions");
        default:
            return Fail("timeline command of type " + std::to_string(type));
        }
    }

    bool AddShape(uint32_t ch) {
        // sub_826091D8 / sub_82608F88 / sub_82608D70.
        const uint32_t n = U16(ch + kShapeFillCount);
        uint32_t fills = 0;
        if (!Pointer(ch + kShapeFills, kFillBytes * std::max<uint32_t>(n, 1), "fills", &fills))
            return false;
        for (uint32_t k = 0; fills && k < n; ++k) {
            const uint32_t fill = fills + kFillBytes * k;
            if (U32(fill + 32))
                return Fail("a bitmap fill: it needs its movie's textures");
            uint32_t grad = 0;
            if (!Pointer(fill + 36, 32, "gradient", &grad)) return false;
            if (grad && (!Pointer(grad + 24, 16, "gradient records") ||
                         !Pointer(grad + 28, 16, "gradient records")))
                return false;
        }
        if (!Pointer(ch + kShapeLines, 16, "line styles")) return false;
        const uint32_t edges = U32(ch + kShapeEdges);
        return Pointer(ch + kShapeEdges, edges ? EdgesLength(edges) : 0, "edges");
    }

    // The edge records run until a 0 op; what an unknown op would leave out is
    // inside the tail slack.
    uint32_t EdgesLength(uint32_t p) const {
        uint64_t q = p;
        for (int guard = 0; guard < 65536 && q + 4 <= uint64_t(kVirtualBase) + size_; ++guard) {
            const uint32_t op = U32(uint32_t(q));
            q += 4;
            switch (op) {
            case 0: return uint32_t(q - p);
            case 1: case 2: case 7: q += 4 + 8ull * U32(uint32_t(q)); break;
            case 3: q += 8; break;
            case 4: case 5: q += 4; break;
            case 6: q += 4 + 16ull * U32(uint32_t(q)); break;
            default: guard = 65536; break;
            }
        }
        const uint32_t left = size_ - (p - kVirtualBase);
        return std::min<uint32_t>(256, left);
    }

    const uint8_t* d_;
    uint32_t size_;
    std::set<int> chars_;
    std::vector<uint32_t> pointers_;
    uint32_t lo_ = ~0u;
    uint32_t hi_ = 0;
    std::string error_;
};

struct Import {
    std::string path;
    int index = -1;
    ImportedSprite sprite;  // character 0 = failed
    // Read from any thread by the update wrapper.
    std::atomic<uint32_t> lo{0};    // guest copy of the window
    std::atomic<uint32_t> size{0};
    std::atomic<uint32_t> file{0};
    std::atomic<uint32_t> hook_sprite{0};
    std::atomic<BeforeUpdateFn> hook{nullptr};
};
Import g_imports[kMaxImports];
std::atomic<int> g_import_count{0};

uint32_t g_scratch = 0;
uint32_t g_scratch_used = kScratchBlock;

// A guest function on a context of its own, below the frame of the one
// running -- what GuestToHostFunction does, but from `ctx`, which is the live
// stack inside a guest call (GuestToHostFunction starts from the thread's
// outer context).
void Call(PPCFunc* fn, PPCContext& ctx, uint8_t* base, uint32_t r3, uint32_t r4) {
    PPCContext c{};
    c.r1 = ctx.r1;
    c.r1.u32 -= 0x70;
    c.r13 = ctx.r13;
    c.fpscr = ctx.fpscr;
    c.r3.u64 = r3;
    c.r4.u64 = r4;
    fn(c, base);
    ctx.fpscr = c.fpscr;
}

uint16_t R16(const uint8_t* base, uint32_t ea) {
    return uint16_t((uint32_t(base[ea]) << 8) | base[ea + 1]);
}

void W16(uint8_t* base, uint32_t ea, uint16_t v) {
    base[ea] = uint8_t(v >> 8);
    base[ea + 1] = uint8_t(v);
}

bool Load(Import& im) {
    using namespace mc::modloader;
    const auto t0 = std::chrono::steady_clock::now();
    auto* rt = rex::Runtime::instance();
    uint8_t* base = rt ? rt->virtual_membase() : nullptr;
    auto* mem = rt ? rt->memory() : nullptr;
    if (!base || !mem) return false;

    const std::filesystem::path archive_path = rt->game_data_root() / kSourceArchive;
    Rpf3Reader archive;
    Rpf3Entry entry;
    std::vector<uint8_t> raw;
    if (!archive.Open(archive_path) || !archive.Find(im.path, entry) ||
        !archive.ReadFile(entry, raw)) {
        MC_ERROR("[swf-import] {} not readable from {}", im.path, archive_path.string());
        return false;
    }
    Rsc5Resource res;
    size_t payload_offset = 0, payload_size = 0;
    std::string error;
    if (!ParseRsc5Header(raw, res, payload_offset, payload_size, error) ||
        !LzxDecompress(raw.data() + payload_offset, payload_size, res.data,
                       size_t(res.virtual_size) + res.physical_size) ||
        res.data.size() < res.virtual_size) {
        MC_ERROR("[swf-import] {}: cannot unpack ({})", im.path, error.empty() ? "LZX" : error);
        return false;
    }

    Closure cl(res.data.data(), res.virtual_size);
    if (!cl.Add(im.index)) {
        MC_ERROR("[swf-import] {} #{}: {}", im.path, im.index, cl.error());
        return false;
    }
    if (cl.Type(im.index) != kSprite) {
        MC_ERROR("[swf-import] {} #{} is not a sprite", im.path, im.index);
        return false;
    }
    cl.Finish();
    if (!cl.Covers()) {
        MC_ERROR("[swf-import] {} #{}: a pointer falls outside the copy", im.path, im.index);
        return false;
    }

    // The copy, then the stand-in file and the fixup's arguments after it, on
    // a 16-byte boundary.
    const uint32_t lo = cl.lo(), copied = cl.hi() - cl.lo();
    const uint32_t window = (copied + 15) & ~15u;
    const uint32_t count = cl.U16(kVirtualBase + kFileCount);
    const std::string label = im.path + " (larecomp import)";
    const uint32_t table_bytes = 4 * count;
    const uint32_t total = window + kFileBytes + table_bytes + 16 + 16 + uint32_t(label.size()) + 1;
    const uint32_t guest = mem->SystemHeapAlloc(total, 0x1000);
    if (!guest) {
        MC_ERROR("[swf-import] {}: no guest memory for {} KB", im.path, total / 1024);
        return false;
    }
    std::memcpy(base + guest, res.data.data() + (lo - kVirtualBase), copied);
    const uint32_t file = guest + window;
    const uint32_t table = file + kFileBytes;
    const uint32_t rsc = table + table_bytes;
    const uint32_t map = rsc + 16;
    const uint32_t name = map + 16;
    std::memcpy(base + name, label.c_str(), label.size() + 1);

    // The page map the fixup moves pointers through (sub_8217D828 / D890):
    // u16 virtual chunks, u16 physical ones, then {source, dest, size}. One
    // chunk: the copy. The datResource the fixup takes: +0 that map, +8 the
    // name it puts in its errors.
    W16(base, map, 1);
    W16(base, map + 2, 0);
    WriteGuestU32(base, map + 4, lo);
    WriteGuestU32(base, map + 8, guest);
    WriteGuestU32(base, map + 12, copied);
    WriteGuestU32(base, rsc, map);
    WriteGuestU32(base, rsc + 8, name);

    auto* ts = rex::runtime::ThreadState::Get();
    const uint32_t r13 = ts && ts->context() ? ts->context()->r13.u32 : 0;
    const uint32_t tls = r13 ? ReadGuestU32(base, r13) : 0;
    if (!tls) {
        MC_ERROR("[swf-import] {}: no thread block to fix up under", im.path);
        return false;
    }
    const uint32_t resource_was = ReadGuestU32(base, tls + kTlsResource);
    WriteGuestU32(base, tls + kTlsResource, 0);
    for (const int idx : cl.chars())
        rex::ppc::GuestToHostFunction<uint32_t>(__imp__rex_sub_825FDA90, rsc,
                                                guest + (cl.Char(idx) - lo));
    WriteGuestU32(base, tls + kTlsResource, resource_was);

    // The stand-in file: the table holds the imported characters at their own
    // indices, so the sprite's commands find them as at home.
    WriteGuestU32(base, file, kFileVtable);
    WriteGuestU32(base, file + kFileChars, table);
    base[file + kFileVersion] = cl.U8(kVirtualBase + kFileVersion);
    base[file + kFileInput] = 0;
    WriteGuestU32(base, file + kFileWidth, cl.U32(kVirtualBase + kFileWidth));
    WriteGuestU32(base, file + kFileHeight, cl.U32(kVirtualBase + kFileHeight));
    W16(base, file + kFileRate, cl.U16(kVirtualBase + kFileRate));
    W16(base, file + kFileCount, uint16_t(count));
    int bad = 0;
    for (const int idx : cl.chars()) {
        const uint32_t at = guest + (cl.Char(idx) - lo);
        const uint32_t want = cl.Type(idx) == kSprite ? kSpriteVtable : kShapeVtable;
        if (ReadGuestU32(base, at) != want) ++bad;
        WriteGuestU32(base, table + 4u * uint32_t(idx), at);
    }
    if (bad) {
        MC_ERROR("[swf-import] {} #{}: {} characters came out of the fixup without their class",
                 im.path, im.index, bad);
        return false;
    }

    im.sprite.character = guest + (cl.Char(im.index) - lo);
    im.sprite.file = file;
    im.file.store(file, std::memory_order_relaxed);
    im.size.store(copied, std::memory_order_relaxed);
    im.lo.store(guest, std::memory_order_release);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    MC_INFO("[swf-import] {} #{}: {} characters, {} pointers, bytes 0x{:08X}..0x{:08X} "
            "({} KB) -> guest 0x{:08X}, stand-in file 0x{:08X}, sprite 0x{:08X} ({} ms)",
            im.path, im.index, cl.chars().size(), cl.pointer_count(), lo, cl.hi(),
            copied / 1024, guest, file, im.sprite.character, ms);
    return true;
}

}  // namespace

const ImportedSprite* ImportSprite(const char* archive_path, int index) {
    const int n = g_import_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (g_imports[i].index == index && g_imports[i].path == archive_path)
            return g_imports[i].sprite.character ? &g_imports[i].sprite : nullptr;
    if (n >= kMaxImports) return nullptr;
    Import& im = g_imports[n];
    im.path = archive_path;
    im.index = index;
    g_import_count.store(n + 1, std::memory_order_release);  // tried once, either way
    return Load(im) ? &im.sprite : nullptr;
}

uint32_t FrameCommand(const uint8_t* base, uint32_t sprite, int frame, int type, uint16_t depth) {
    if (!sprite || base[sprite + kCharType] != kSprite) return 0;
    const uint32_t frames = ReadGuestU32(base, sprite + kSpriteFrames);
    if (!frames || frame < 0 || frame >= R16(base, sprite + kSpriteFrameCount)) return 0;
    for (uint32_t c = ReadGuestU32(base, frames + 8 * uint32_t(frame) + 4), guard = 0;
         c && guard < 4096; c = ReadGuestU32(base, c + kCmdNext), ++guard) {
        const int t = base[c + kCmdType];
        if (t != type) continue;
        const uint16_t d = t == 2 ? R16(base, c + 6) : R16(base, c + 20);
        if (d == depth) return c;
    }
    return 0;
}

void SetBeforeUpdate(uint32_t sprite, BeforeUpdateFn fn) {
    const int n = g_import_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        if (g_imports[i].sprite.character != sprite) continue;
        g_imports[i].hook.store(fn, std::memory_order_relaxed);
        g_imports[i].hook_sprite.store(sprite, std::memory_order_release);
    }
}

void RunCommand(PPCContext& ctx, uint8_t* base, uint32_t cmd, uint32_t parent) {
    if (!cmd || !parent) return;
    PPCFunc* fn = nullptr;
    switch (ReadGuestU32(base, cmd)) {
    case kCmdPlaceVtable:  fn = __imp__rex_sub_8260B038; break;
    case kCmdNamedVtable:  fn = __imp__rex_sub_8260B250; break;
    case kCmdRemoveVtable: fn = __imp__rex_sub_8260AA48; break;
    default:
        MC_WARN("[swf-import] command 0x{:08X} has vtable 0x{:08X}; not run", cmd,
                ReadGuestU32(base, cmd));
        return;
    }
    Call(fn, ctx, base, cmd, parent);
}

uint32_t GuestScratch(uint32_t bytes) {
    bytes = (bytes + 15) & ~15u;
    if (bytes > kScratchBlock) return 0;
    if (g_scratch_used + bytes > kScratchBlock) {
        auto* rt = rex::Runtime::instance();
        const uint32_t block = rt && rt->memory() ? rt->memory()->SystemHeapAlloc(kScratchBlock) : 0;
        if (!block) return 0;
        g_scratch = block;
        g_scratch_used = 0;
    }
    const uint32_t at = g_scratch + g_scratch_used;
    g_scratch_used += bytes;
    return at;
}

uint32_t GuestString(const char* text) {
    const uint32_t len = uint32_t(std::strlen(text));
    const uint32_t at = GuestScratch(len + 1);
    auto* rt = rex::Runtime::instance();
    if (!at || !rt) return 0;
    std::memcpy(rt->virtual_membase() + at, text, len + 1);
    return at;
}

}  // namespace larecomp::swf

// Every instance of every movie updates through here (the recursion goes
// through the overridable symbol). An imported one updates with its own file
// current, as at home; the movie's comes back after it.
extern "C" REX_FUNC(rex_sub_825FB0D8) {
    using namespace larecomp::swf;
    const uint32_t inst = ctx.r3.u32;
    const int n = g_import_count.load(std::memory_order_acquire);
    if (n && inst >= 0x10000u) {
        const uint32_t ch = ReadGuestU32(base, inst + kInstChar);
        for (int i = 0; i < n; ++i) {
            Import& im = g_imports[i];
            const uint32_t lo = im.lo.load(std::memory_order_acquire);
            if (!lo || ch - lo >= im.size.load(std::memory_order_relaxed)) continue;
            const uint32_t was = ReadGuestU32(base, kCurrentFile);
            WriteGuestU32(base, kCurrentFile, im.file.load(std::memory_order_relaxed));
            if (ch == im.hook_sprite.load(std::memory_order_acquire)) {
                if (const BeforeUpdateFn fn = im.hook.load(std::memory_order_relaxed)) {
                    fn(ctx, base, inst);
                    ctx.r3.u64 = inst;
                }
            }
            __imp__rex_sub_825FB0D8(ctx, base);
            WriteGuestU32(base, kCurrentFile, was);
            return;
        }
    }
    __imp__rex_sub_825FB0D8(ctx, base);
}

#else  // REXGLUE_HAS_XEO3_TARGET

namespace larecomp::swf {
const ImportedSprite* ImportSprite(const char*, int) { return nullptr; }
uint32_t FrameCommand(const uint8_t*, uint32_t, int, int, uint16_t) { return 0; }
void SetBeforeUpdate(uint32_t, BeforeUpdateFn) {}
void RunCommand(PPCContext&, uint8_t*, uint32_t, uint32_t) {}
uint32_t GuestScratch(uint32_t) { return 0; }
uint32_t GuestString(const char*) { return 0; }
}  // namespace larecomp::swf

#endif  // REXGLUE_HAS_XEO3_TARGET
