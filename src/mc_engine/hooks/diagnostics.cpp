// Diagnostic probes, observers only: RAGE's silent fatal error, the streamer and
// file open traces, the resource fixup / segment / inflate traces, the LZX
// decompression stats, and the tune field registration probe (which also hands
// the camera free look the addresses of its fields).

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <thread>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#include "hooks.h"
#include "larecomp_log.h"
#include "../camera_look.h"
#include "../online/online_common.h"  // shared guest-memory helpers (GuestRead32, ...)

// Diagnostic: dumps every tune field as it registers. Off by default -- it fires for
// every tune in the game (about 1100 unique names across ~28 classes) -- but it is the
// only way to see a tune's live layout, so it stays.
REXCVAR_DEFINE_BOOL(tune_field_probe, false, "MCLA/Diagnostics",
                    "Log every tune field name as it registers.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(dbg_print, false, "MCLA/Patches", "Enable DbgPrint console outputs.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(lzx_stats, false, "MCLA/Debug",
    "Measure pgStreamer LZX decompression (XMemDecompressStream): per-2s window stats "
    "appended to <exe>/lzx_stats.txt. For diagnosing streaming stutter (South Central).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(stream_trace, "", "MCLA/Diag",
    "Log every resource the streamer is asked for whose path contains this "
    "text, and whether the file was found.\n"
    "\n"
    "The hook sits one instruction after pgStreamer::Open returns inside "
    "sub_821E2940, where the resolved path is still on the stack at r1+0x60 and "
    "r3 holds the handle -- minus one when the file was not found. That pair is "
    "the only thing that separates \"the game never asked for it\" from \"the "
    "game asked and the archive did not have it\", which is the question a car "
    "that loads forever comes down to.\n"
    "\n"
    "Empty turns it off. `resources/vehicle` is the useful setting for a car; "
    "`/` logs every file the game opens, which is thousands of lines.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Shared by the two open probes: read the path the call was handed off the
// guest stack, hold it against the filter, and say whether it was found.
static void TraceOpen(const char* kind, uint32_t stack, uint32_t offset, bool found) {
    const std::string filter = REXCVAR_GET(stream_trace);
    if (filter.empty()) return;

    const auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    char path[192];
    size_t n = 0;
    while (n < sizeof(path) - 1) {
        const char c = static_cast<char>(base[stack + offset + n]);
        if (!c) break;
        path[n++] = c;
    }
    path[n] = '\0';
    if (n == 0) return;
    if (filter != "/" && std::string_view(path).find(filter) == std::string_view::npos) return;

    LARECOMP_APP_INFO("[stream] {} {} {}", kind, found ? "opened   " : "NOT FOUND", path);
}

// Entry of sub_821BD618, RAGE's fatal error.
//
// It is `__noreturn { if (handler) handler(msg); while (1) ; }` -- and the
// handler pointer is null in the shipped build, so every fatal in this game is
// a silent infinite spin on whatever thread hit it. Nothing is printed, no
// watchdog fires, and if the thread was the game's own the frame simply stops
// while the render thread keeps drawing the last one. That is what a MCLA
// "infinite loading" actually looks like, and 223 call sites can produce it.
//
// This makes it speak. r3 is the printf format, r4 the first argument, which
// between them name the failure ("Unknown manufacturer %s in vehicle list
// file", "global array [%s] does not exist", and so on). It does not stop the
// spin -- the callers are written assuming this never returns -- it only says
// what happened before the game stops.
void MCLA_RageFatal(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) {
    const auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    // Most of these messages take a plain integer -- "zlibInflater::InflateBegin
    // - error %08x" is the one that brought the game down here -- and an integer
    // that happens to look like an address is not one. So every byte is read
    // only after the host page behind it is known to be committed and readable:
    // walking off the end of a mapped region is what turned the one assert that
    // finally fired into an access violation inside this hook.
    auto readable = [&](uint32_t ea) {
#if defined(_WIN32)
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(base + ea, &mbi, sizeof(mbi))) return false;
        constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                    PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                    PAGE_EXECUTE_WRITECOPY;
        return mbi.State == MEM_COMMIT && (mbi.Protect & kReadable) != 0 &&
               (mbi.Protect & PAGE_GUARD) == 0;
#else
        (void)ea;
        return false;
#endif
    };

    auto read = [&](uint32_t ea) {
        std::string out;
        if (ea < 0x1000 || ea >= 0xC0000000) return out;
        for (size_t i = 0; i < 256; ++i) {
            const uint32_t at = ea + i;
            // Probe once per page rather than once per byte.
            if (i == 0 || (at & 0xFFFu) == 0) {
                if (!readable(at)) break;
            }
            const char c = static_cast<char>(base[at]);
            if (!c) break;
            // A string argument is text. Anything else is an integer that
            // happened to point at mapped memory, and printing it is noise.
            if (static_cast<unsigned char>(c) < 0x20 ||
                static_cast<unsigned char>(c) > 0x7E) {
                return std::string();
            }
            out.push_back(c);
        }
        return out;
    };

    const std::string format = read(static_cast<uint32_t>(r3.u64));
    // Several of these messages carry two names -- "invalid group (%s) on car
    // %s" -- and which car it was is the whole answer, so the next three
    // argument registers are read as strings too and the ones that are not
    // simply come back empty.
    const std::string a1 = read(static_cast<uint32_t>(r4.u64));
    const std::string a2 = read(static_cast<uint32_t>(r5.u64));
    const std::string a3 = read(static_cast<uint32_t>(r6.u64));
    LARECOMP_APP_ERROR("[rage-fatal] {} | args: '{}' '{}' '{}' (r4={:#x} r5={:#x} r6={:#x})",
                       format.empty() ? "<unreadable>" : format, a1, a2, a3,
                       static_cast<uint32_t>(r4.u64), static_cast<uint32_t>(r5.u64),
                       static_cast<uint32_t>(r6.u64));

    // "zlibInflater::InflateBegin - not in XCompress format" means the four
    // bytes at the inflater's input pointer are not 0x0FF512EF. Which four they
    // are says where the stream really is: eight bytes on from the magic means
    // the header check ran a second time on a stream that had already been
    // opened, which happens when the first decode step consumed nothing.
    // r4 is the pointer the comparison was made through.
    if (format.rfind("zlibInflater", 0) == 0) {
        const uint32_t ctx = static_cast<uint32_t>(r4.u64);
        auto hex = [&](uint32_t from, uint32_t count, uint32_t mark) {
            std::string out;
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t ea = from + i;
                if (i == 0 || (ea & 0xFFFu) == 0) {
                    if (!readable(ea)) break;
                }
                char byte[8] = {};
                std::snprintf(byte, sizeof byte, "%s%02X", ea == mark ? "|" : " ", base[ea]);
                out += byte;
            }
            return out;
        };

        if (ctx >= 0x1000 && ctx < 0xC0000000 - 0x40) {
            // r4 is the inflater's context: +0 input left, +4 input pointer,
            // +8 consumed, +12 stream length, +16 output space.
            auto word = [&](uint32_t offset) {
                const uint8_t* p = base + ctx + offset;
                return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                       (uint32_t(p[2]) << 8) | uint32_t(p[3]);
            };
            LARECOMP_APP_ERROR("[rage-fatal] inflater ctx={:#010x} in_left={} in_ptr={:#010x} "
                               "consumed={} len={} out_left={}",
                               ctx, word(0), word(4), word(8), word(12), word(16));

            // The comparison that failed dereferences the input pointer, so the
            // bytes it read are the answer: the read window starts twelve bytes
            // before it, at the file's own first byte, and a resource begins
            // 05 43 53 52 -- "RSC" with its version. If those four are there and
            // 0F F5 12 EF follows at twelve, the file arrived intact and the
            // fault is the frame, not the bytes.
            const uint32_t in_ptr = word(4);
            if (in_ptr >= 0x1000 + 12 && in_ptr < 0xC0000000 - 0x40) {
                LARECOMP_APP_ERROR("[rage-fatal] file as read, offset 0 (| is the pointer, "
                                   "at offset 12):{}",
                                   hex(in_ptr - 12, 40, in_ptr));
            }
        }
    }
}

// One instruction after `bl sub_821BCE68` in sub_821E2940 (0x821E29C4). The
// resolved path is the stack buffer at r1+0x60 that the call was handed, and r3
// is the handle it came back with: -1 means pgStreamer::Open could not find the
// file. Pure observer.
void MCLA_StreamOpenResult(PPCRegister& r1, PPCRegister& r3) {
    TraceOpen("rsc ", static_cast<uint32_t>(r1.u64), 0x60u,
              static_cast<int32_t>(static_cast<uint32_t>(r3.u64)) != -1);
}

// 0x821CA708, one instruction after `bl sub_821BDF20` inside sub_821CA6A8 --
// fiDevice's plain-file open, the one every tune, camera and garage file goes
// through. The resource streamer above never sees any of those, which is the
// half of the file system a car that loads forever could be stuck in. The path
// the call was handed is the stack buffer at r1+0x50 and r3 is the stream, zero
// when nothing opened. Pure observer.
void MCLA_FileOpenResult(PPCRegister& r1, PPCRegister& r3) {
    TraceOpen("file", static_cast<uint32_t>(r1.u64), 0x50u, r3.u32 != 0);
}

// datResource's fixup error, at 0x821D2378, one instruction of cost because it
// is only reached on the way to the fatal.
//
// r3 is the resource descriptor: +0 the segment table, +8 the name. r5 is the
// address that fit no segment. The table starts with two 16-bit counts -- how
// many virtual segments and how many physical -- followed by twelve-byte
// entries, and the two lookups that disagree here (sub_82187A38 and
// sub_8217D828) read it from different offsets, so the raw words go in the log
// and the layout can be settled by looking at them rather than by guessing.
void MCLA_ResourceFixupError(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {
    const auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    auto word = [&](uint32_t ea) {
        const uint8_t* p = base + ea;
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) |
               uint32_t(p[3]);
    };
    auto text = [&](uint32_t ea) {
        std::string out;
        if (ea < 0x1000 || ea >= 0xC0000000) return out;
        for (int i = 0; i < 96; ++i) {
            const char c = static_cast<char>(base[ea + i]);
            if (c < 0x20 || c > 0x7E) break;
            out.push_back(c);
        }
        return out;
    };

    const uint32_t desc = static_cast<uint32_t>(r3.u64);
    const uint32_t addr = static_cast<uint32_t>(r5.u64);
    if (desc < 0x1000 || desc >= 0xC0000000) return;

    LARECOMP_APP_ERROR("[fixup] {} | resource '{}' | address {:#010x}",
                       text(static_cast<uint32_t>(r4.u64)), text(word(desc + 8)), addr);

    // The descriptor first: whatever says how many of the table's entries
    // belong to this resource lives here, not in the table. The table's own
    // first word turned out to be the first entry's size, so reading it as a
    // pair of counts was wrong.
    std::string head;
    for (uint32_t i = 0; i < 12; ++i) {
        char part[16] = {};
        std::snprintf(part, sizeof part, " %08X", word(desc + i * 4));
        head += part;
    }
    LARECOMP_APP_ERROR("[fixup] descriptor {:#010x}:{}", desc, head);

    const uint32_t table = word(desc + 0);
    if (table < 0x1000 || table >= 0xC0000000) {
        LARECOMP_APP_ERROR("[fixup] segment table pointer {:#010x} is not readable", table);
        return;
    }

    // An entry is twelve bytes: size, the address the resource was built for,
    // and the address it actually landed at. sub_8217D890 takes its delta from
    // the second and third, which is what pins the order down.
    uint64_t virtual_total = 0, physical_total = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t at = table + i * 12;
        const uint32_t size = word(at), declared = word(at + 4), real = word(at + 8);
        if (declared == 0 && size == 0) break;
        if ((declared & 0xF0000000u) == 0x50000000u) virtual_total += size;
        if ((declared & 0xF0000000u) == 0x60000000u) physical_total += size;
        LARECOMP_APP_ERROR("[fixup]   entry {:2}: size {:#010x} built for {:#010x} landed at "
                           "{:#010x} delta {:#010x}{}",
                           i, size, declared, real, real - declared,
                           (addr >= declared && addr < declared + size) ? "   <-- holds it" : "");
    }
    LARECOMP_APP_ERROR("[fixup] totals so far: virtual {} bytes, physical {} bytes",
                       virtual_total, physical_total);
}

REXCVAR_DEFINE_STRING(segment_trace, "", "MCLA/Diag",
    "Dump the segment table of every resource whose file name contains this "
    "text, as the streamer allocates it.\n"
    "\n"
    "A resource does not arrive in memory as one block. sub_821E58F0 walks a "
    "table of twelve-byte entries -- destination, size, and the address the "
    "resource was built for -- and either allocates them one by one (each landing "
    "wherever the heap had room) or sums them into a single allocation, with "
    "every size rounded up to 128 either way. Every pointer inside the resource "
    "is then resolved by finding which entry's range it falls in (sub_8217D828) "
    "and adding that entry's own delta.\n"
    "\n"
    "So a buffer that starts in one segment and runs past its end is read as if "
    "the next segment followed it, and from the boundary on the GPU fetches "
    "whatever the heap put there -- a mesh that is perfect on disk and blades on "
    "screen. This prints the boundaries so a rebuilt drawable's buffers can be "
    "held against them.\n"
    "\n"
    "Empty turns it off. `/` dumps every resource. `>N` dumps only the ones "
    "whose segments total N bytes or more, which is the setting that works: a "
    "resource read out of an archive has no name to match -- that is what the "
    "fixup error means by 'Unknown file in an archive' -- and a rebuilt car body "
    "is the only thing in the game past a megabyte. Anything else is matched "
    "against the name.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// 0x821E5908, just after sub_821E57B8 has filled the table. r31 is the table.
// Pure observer -- it runs before the allocator, so the destination column is
// still whatever was there last, and only the sizes and declared bases are real.
void MCLA_ResourceSegments(PPCRegister& r31) {
    const std::string filter = REXCVAR_GET(segment_trace);
    if (filter.empty()) return;

    const auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    const uint32_t table = static_cast<uint32_t>(r31.u64);
    if (table < 0x1000 || table >= 0xC0000000) return;

    auto word = [&](uint32_t ea) {
        const uint8_t* p = base + ea;
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) |
               uint32_t(p[3]);
    };
    auto half = [&](uint32_t ea) {
        return static_cast<uint32_t>((base[ea] << 8) | base[ea + 1]);
    };

    const uint32_t virtuals = half(table), physicals = half(table + 2);
    const uint32_t total = virtuals + physicals;
    // The table lives in a 1540-byte streamer slot starting at +44, so 124
    // entries is all the room there is. A count past that is its own answer, and
    // reading it as a real count would walk off the slot.
    if (total == 0 || total > 124) {
        if (total > 124) {
            LARECOMP_APP_ERROR("[segments] {} + {} blocks, past the 124 the streamer slot holds",
                               virtuals, physicals);
        }
        return;
    }

    // An entry is twelve bytes from +4: the address the resource was built for,
    // the address it will land at, and the size. sub_82187A38 reads the pair at
    // +4/+8 and sub_821E58F0 writes the destination at +4 and takes the size
    // from +8, which pins all three down.
    uint64_t virtual_bytes = 0, physical_bytes = 0;
    for (uint32_t i = 0; i < total; ++i) {
        const uint32_t size = word(table + 4 + i * 12 + 8);
        (i < virtuals ? virtual_bytes : physical_bytes) += size;
    }

    if (filter != "/" && filter[0] == '>') {
        const uint64_t least = std::strtoull(filter.c_str() + 1, nullptr, 10);
        if (virtual_bytes + physical_bytes < least) return;
    }

    // The byte the next instructions branch on. Non-zero means every block of
    // this resource comes out of ONE allocation and lands end to end, so a
    // buffer crossing a block boundary costs nothing. Zero means each block is
    // allocated on its own and a crossing buffer reads whatever followed the
    // block in the heap -- which is the whole question this probe exists for.
    const bool one_allocation = base[0x8286CE81] != 0;

    LARECOMP_APP_INFO("[segments] {} virtual block(s) totalling {}, {} physical totalling {}; "
                      "blocks are {}",
                      virtuals, virtual_bytes, physicals, physical_bytes,
                      one_allocation ? "ONE allocation, contiguous"
                                     : "allocated SEPARATELY, not contiguous");

    uint64_t running = 0;
    for (uint32_t i = 0; i < total; ++i) {
        const uint32_t at = table + 4 + i * 12;
        const uint32_t built_for = word(at), size = word(at + 8);
        if (i == virtuals) running = 0;
        LARECOMP_APP_INFO("[segments]   {:3} {:<8} {:#010x}..{:#010x} size {:>9}, {:#x}..{:#x} of "
                          "the segment's own half",
                          i, i < virtuals ? "virtual" : "physical", built_for, built_for + size,
                          size, running, running + size);
        running += size;
    }
}

REXCVAR_DEFINE_BOOL(inflate_trace, false, "MCLA/Diag",
    "Log every call into zlibInflater's decode step, for the first few hundred.\n"
    "\n"
    "A repacked resource that never arrives leaves no other trace: the streamer "
    "opens the file, reads it, and then either spins in sub_821BC140 or asserts "
    "that the stream is not in XCompress format. Both of those follow from one "
    "number -- how much input the decoder actually consumed on a call -- and "
    "this prints it. Read consecutive lines: input left falling says it ate "
    "something, output left falling says it produced something, and a pair of "
    "identical lines is the decoder refusing to move.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Entry of zlibInflater's step. r4 is the context: +0 input left, +4 input
// pointer, +8 input consumed, +12 stream length, +16 output space left,
// +20 output pointer. Pure observer, capped so a healthy boot cannot drown the
// log.
void MCLA_InflateStep(PPCRegister& r3, PPCRegister& r4) {
    if (!REXCVAR_GET(inflate_trace)) return;

    const auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    const uint32_t ctx = static_cast<uint32_t>(r4.u64);
    if (ctx < 0x1000 || ctx >= 0xC0000000) return;

    auto word = [&](uint32_t offset) {
        const uint8_t* p = base + ctx + offset;
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) |
               uint32_t(p[3]);
    };

    struct Step {
        uint32_t ctx, in_left, in_ptr, consumed, len, out_left;
    };
    const Step now{ctx, word(0), word(4), word(8), word(12), word(16)};

    // A healthy boot makes tens of thousands of these calls, so printing them
    // all buries the one that matters. What matters is a call that changes
    // nothing: the decoder was handed input and output room and moved neither,
    // which is the shape of both failures seen here -- the read loop only
    // refills when the input buffer empties, so a decoder that will not move
    // leaves the game turning on the spot with no I/O and no assert. So keep a
    // short history, watch for repeats, and print only when one is found.
    static std::mutex mutex;
    static Step ring[12]{};
    static uint32_t depth = 0, repeats = 0;
    static bool reported = false;

    std::lock_guard<std::mutex> lock(mutex);

    // Every resource starts 05 43 53 52 -- "RSC" and its version -- and the read
    // window begins twelve bytes before the inflater's pointer, so a first call
    // whose head is anything else is a resource that did not arrive. That is the
    // whole anomaly worth printing: the game's own files all read correctly, so a
    // filter on the head is quiet on a healthy boot and names the broken file by
    // its size. A few good ones go in first as the reference.
    static uint32_t good = 0, bad = 0;
    if (now.consumed == 0) {
        const uint32_t from = now.in_ptr - 12;
        if (from >= 0x1000 && from < 0xC0000000 - 0x40) {
            const bool is_resource = base[from] == 0x05 && base[from + 1] == 0x43 &&
                                     base[from + 2] == 0x53 && base[from + 3] == 0x52;
            const bool want = is_resource ? (now.len > 20000 && good < 4) : (bad < 12);
            if (want) {
                if (is_resource) ++good; else ++bad;
                std::string head;
                for (uint32_t i = 0; i < 24; ++i) {
                    char byte[8] = {};
                    std::snprintf(byte, sizeof byte, " %02X", base[from + i]);
                    head += byte;
                }
                if (is_resource) {
                    LARECOMP_APP_INFO("[inflate] arrived: in_left={} len={} out_left={} "
                                      "head:{}",
                                      now.in_left, now.len, now.out_left, head);
                } else {
                    LARECOMP_APP_ERROR("[inflate] NOT A RESOURCE: the read window holds no "
                                       "RSC5 header. in_left={} len={} out_left={} "
                                       "in_ptr={:#010x} head:{}",
                                       now.in_left, now.len, now.out_left, now.in_ptr, head);
                }
            }
        }
    }

    if (reported) return;

    // The streamer reuses one context, and the game ships families of resources
    // whose sizes are identical, so two different files in a row can look like
    // the same call repeating. Eight in a row is past coincidence.
    const Step& last = ring[(depth + 11) % 12];
    const bool stuck = depth != 0 && now.ctx == last.ctx && now.in_left == last.in_left &&
                       now.out_left == last.out_left && now.consumed == last.consumed &&
                       now.len == last.len && now.in_ptr == last.in_ptr;
    repeats = stuck ? repeats + 1 : 0;
    ring[depth % 12] = now;
    ++depth;

    if (repeats < 8) return;
    reported = true;

    LARECOMP_APP_ERROR("[inflate] STALLED: the decoder has taken nothing and produced "
                       "nothing {} times running. Last {} calls, oldest first:",
                       repeats + 1, depth < 12 ? depth : 12u);
    const uint32_t count = depth < 12 ? depth : 12u;
    for (uint32_t i = 0; i < count; ++i) {
        const Step& s = ring[(depth - count + i) % 12];
        LARECOMP_APP_ERROR("[inflate]   ctx={:#010x} in_left={:6} in_ptr={:#010x} "
                           "consumed={:7} len={:7} out_left={:6}",
                           s.ctx, s.in_left, s.in_ptr, s.consumed, s.len, s.out_left);
    }
    (void)r3;
}

// Tune field registration probe. sub_824DF200(owner, type, name, &field, ...).
//
// `owner` is the class descriptor for a top-level field, but a field of type 13 is a
// nested sub-object and everything registered after it reports that sub-object as its
// owner -- so owner/field together give the tree, not a flat list.
//
// Known limit: the vehicle handling tune does NOT come through here. A full capture
// (boot through gameplay) yields ~1100 fields across cameras, HUD, effects, AI, input
// and cop lights, and no vehicle physics class at all -- no SteeringLimit, TurnBias,
// SlidingFric or OptSlipPercent. Do not spend another session looking for them here.
void MCLA_TuneFieldProbe(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) {
    const auto* base = rex::Runtime::instance()->virtual_membase();
    const auto name_addr = static_cast<uint32_t>(r5.u64);
    if (!base || !name_addr) return;

    char name[64];
    size_t n = 0;
    while (n < sizeof(name) - 1) {
        const char c = static_cast<char>(base[name_addr + n]);
        if (!c) break;
        name[n++] = c;
    }
    name[n] = '\0';
    if (n == 0) return;

    // r6 is the address the field will live at, which is the only thing that
    // makes a tune writable from here without hardcoding an offset into one
    // instance of it. The camera free look listens for its own field names on
    // the way past; this runs whether or not the diagnostic below is on,
    // because the addresses are only announced once, at load.
    CameraLookOnTuneField(name, static_cast<uint32_t>(r3.u64),
                          static_cast<uint32_t>(r6.u64));

    if (!REXCVAR_GET(tune_field_probe)) return;

    static std::set<std::string> seen;
    if (!seen.insert(name).second) return;

    LARECOMP_APP_INFO("[TuneField] {:<26} type={} owner={:#010x} field={:#010x}", name,
                      static_cast<uint32_t>(r4.u64), static_cast<uint32_t>(r3.u64),
                      static_cast<uint32_t>(r6.u64));
}

// LZX streaming decompression probe. pgStreamer worker threads decompress
// world resources through zlibInflater::InflateBegin (sub_821D5E10), which
// wraps the statically linked XMemDecompressStream (sub_8244FF20, XCompress
// LZX, 128KB window) — all of it recompiled guest code. The pair of hooks
// brackets that call: Pre fires at 0x821D5EB4 (just before the bl), Post at
// 0x821D5EBC (first instruction after it). The wrapper keeps its in/out sizes
// in stack slots: [r1+0x50] holds the source bytes offered (consumed after the
// call) and [r1+0x54] the destination capacity (bytes produced after the
// call). Two worker threads run this concurrently, hence thread_local pairing
// and atomic totals. Results append to <exe>/lzx_stats.txt every 2 seconds
// while the lzx_stats cvar is on.
namespace {

struct LzxWindow {
    uint64_t calls = 0;
    uint64_t ns = 0;
    uint64_t src_bytes = 0;
    uint64_t dst_bytes = 0;
    uint64_t errors = 0;
};

std::atomic<uint64_t> g_lzx_calls{0};
std::atomic<uint64_t> g_lzx_ns{0};
std::atomic<uint64_t> g_lzx_src_bytes{0};
std::atomic<uint64_t> g_lzx_dst_bytes{0};
std::atomic<uint64_t> g_lzx_max_ns{0};
std::atomic<uint64_t> g_lzx_errors{0};
std::atomic<int64_t> g_lzx_last_dump_ns{0};
std::mutex g_lzx_dump_mutex;
LzxWindow g_lzx_prev;

thread_local int64_t tl_lzx_start_ns = 0;

int64_t LzxNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void LzxDump(int64_t now_ns, int64_t prev_dump_ns) {
    std::lock_guard<std::mutex> lock(g_lzx_dump_mutex);

    LzxWindow cur;
    cur.calls = g_lzx_calls.load(std::memory_order_relaxed);
    cur.ns = g_lzx_ns.load(std::memory_order_relaxed);
    cur.src_bytes = g_lzx_src_bytes.load(std::memory_order_relaxed);
    cur.dst_bytes = g_lzx_dst_bytes.load(std::memory_order_relaxed);
    cur.errors = g_lzx_errors.load(std::memory_order_relaxed);
    uint64_t max_ns = g_lzx_max_ns.exchange(0, std::memory_order_relaxed);

    double wall_ms = double(now_ns - prev_dump_ns) / 1e6;
    double busy_ms = double(cur.ns - g_lzx_prev.ns) / 1e6;
    double out_mb = double(cur.dst_bytes - g_lzx_prev.dst_bytes) / (1024.0 * 1024.0);
    double in_mb = double(cur.src_bytes - g_lzx_prev.src_bytes) / (1024.0 * 1024.0);
    uint64_t calls = cur.calls - g_lzx_prev.calls;
    uint64_t errors = cur.errors - g_lzx_prev.errors;

    std::error_code ec;
    std::filesystem::path file = std::filesystem::current_path(ec) / "lzx_stats.txt";
    if (ec) return;
    std::ofstream out(file, std::ios::app);
    if (!out) return;

    char line[320];
    std::snprintf(line, sizeof(line),
                  "wall=%.0fms calls=%llu busy=%.2fms busy_pct=%.1f%% in=%.2fMB out=%.2fMB "
                  "out_rate=%.1fMB/s max_call=%.0fus errors=%llu | total: calls=%llu busy=%.0fms "
                  "out=%.1fMB\n",
                  wall_ms, static_cast<unsigned long long>(calls), busy_ms,
                  wall_ms > 0.0 ? busy_ms * 100.0 / wall_ms : 0.0, in_mb, out_mb,
                  wall_ms > 0.0 ? out_mb * 1000.0 / wall_ms : 0.0, double(max_ns) / 1e3,
                  static_cast<unsigned long long>(errors),
                  static_cast<unsigned long long>(cur.calls), double(cur.ns) / 1e6,
                  double(cur.dst_bytes) / (1024.0 * 1024.0));
    out << line;

    g_lzx_prev = cur;
}

}  // namespace

void Hook_LzxDecompressPre(PPCRegister& r1) {
    (void)r1;
    if (!REXCVAR_GET(lzx_stats)) {
        tl_lzx_start_ns = 0;
        return;
    }
    tl_lzx_start_ns = LzxNowNs();
}

void Hook_LzxDecompressPost(PPCRegister& r1, PPCRegister& r3) {
    if (!tl_lzx_start_ns) return;
    int64_t now = LzxNowNs();
    uint64_t dur = uint64_t(now - tl_lzx_start_ns);
    tl_lzx_start_ns = 0;

    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* mem = rt->memory();
    if (!mem) return;

    uint32_t sp = static_cast<uint32_t>(r1.u64);
    // After XMemDecompressStream returns: [sp+0x50] = source bytes consumed,
    // [sp+0x54] = destination bytes produced (the wrapper advances its
    // pointers by exactly these values right after the call).
    uint32_t consumed = GuestRead32(mem, sp + 0x50);
    uint32_t produced = GuestRead32(mem, sp + 0x54);

    g_lzx_calls.fetch_add(1, std::memory_order_relaxed);
    g_lzx_ns.fetch_add(dur, std::memory_order_relaxed);
    g_lzx_src_bytes.fetch_add(consumed, std::memory_order_relaxed);
    g_lzx_dst_bytes.fetch_add(produced, std::memory_order_relaxed);

    uint64_t prev_max = g_lzx_max_ns.load(std::memory_order_relaxed);
    while (dur > prev_max &&
           !g_lzx_max_ns.compare_exchange_weak(prev_max, dur, std::memory_order_relaxed)) {
    }

    // 0x81DE2001 is the "needs more input" status the game itself tolerates.
    int32_t status = static_cast<int32_t>(r3.u64);
    if (status < 0 && status != int32_t(0x81DE2001)) {
        g_lzx_errors.fetch_add(1, std::memory_order_relaxed);
    }

    int64_t last = g_lzx_last_dump_ns.load(std::memory_order_relaxed);
    if (now - last >= 2'000'000'000 &&
        g_lzx_last_dump_ns.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        // First window after enabling has no baseline timestamp — skip the dump,
        // the totals still carry into the next one.
        if (last != 0) LzxDump(now, last);
    }
}

REXCVAR_DEFINE_BOOL(fxc_probe, false, "MCLA/Diag",
    "Write the bytes of the two static rage streams' buffer (the effect loader's, at "
    "0x82860C18 and 0x82860C40) to fxc_probe_<n>_<stream>_<count>.bin in the working "
    "directory, once per distinct content, for the first two hundred.\n"
    "\n"
    "Compares what the effect (.fxc) parser is handed on two platforms: the same file should "
    "give the same bytes. Observer only; off by default.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// sub_821BE250 entry (the rage stream's read): r3 is the stream. The buffer is the
// pointer at +8, +24 is the read position and +28 the number of bytes the last refill put
// in it, so a non-zero count means the buffer holds a file's bytes.
void MCLA_StreamBufferProbe(PPCRegister& r3) {
    if (!REXCVAR_GET(fxc_probe)) return;
    const uint32_t stream = r3.u32;
    if (stream != 0x82860C18u && stream != 0x82860C40u) return;

    const auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    auto word = [&](uint32_t offset) {
        const uint8_t* p = base + stream + offset;
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) |
               uint32_t(p[3]);
    };
    const uint32_t buffer = word(8);
    const uint32_t count = word(28);
    if (count == 0 || count > 0x10000u || buffer < 0x10000u || buffer >= 0xC0000000u) return;

    uint64_t hash = 1469598103934665603ull;
    for (uint32_t i = 0; i < count; ++i) hash = (hash ^ base[buffer + i]) * 1099511628211ull;
    hash ^= uint64_t(stream) << 1;

    static std::mutex mutex;
    static std::set<uint64_t> seen;
    std::lock_guard<std::mutex> lock(mutex);
    if (seen.size() >= 200 || !seen.insert(hash).second) return;

    char name[96];
    std::snprintf(name, sizeof name, "fxc_probe_%zu_%08X_%u.bin", seen.size(), stream, count);
    if (FILE* f = std::fopen(name, "wb")) {
        std::fwrite(base + buffer, 1, count, f);
        std::fclose(f);
    }
    LARECOMP_APP_INFO("[fxc-probe] {} <- stream {:#010x} buffer {:#010x}, {} bytes, hash {:016x}",
                      name, stream, buffer, count, hash);
}

REXCVAR_DEFINE_BOOL(input_probe, false, "MCLA/Diag",
    "Log what the SDK's input system hands the game for users 0 to 3 whenever it changes: "
    "the status, the packet number, the buttons and the sticks. Off by default. Polling it "
    "pumps the same input path the game uses, so it can only make a pad look better, never worse; "
    "if a pad shows nothing here, the game gets nothing either.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// Started once the runtime exists. One thread asks the InputSystem for every user's state twice a second.
void MCLA_StartInputProbe() {
    if (!REXCVAR_GET(input_probe)) return;
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    std::thread([] {
        struct Seen {
            uint32_t status = 0xFFFFFFFFu;
            uint32_t packet = 0;
            uint16_t buttons = 0xFFFFu;
            bool any = false;
        } seen[4];
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            auto* rt = rex::Runtime::instance();
            auto* sys = rt ? static_cast<rex::input::InputSystem*>(rt->input_system()) : nullptr;
            if (!sys) continue;
            for (uint32_t user = 0; user < 4; ++user) {
                rex::input::X_INPUT_STATE state{};
                const uint32_t status = uint32_t(sys->GetState(user, &state));
                const uint16_t buttons = state.gamepad.buttons;
                Seen& s = seen[user];
                if (status != s.status || (status == 0 && (buttons != s.buttons || uint32_t(state.packet_number) != s.packet))) {
                    if (status == 0 || s.any || user == 0) {
                        LARECOMP_APP_INFO("[input-probe] user {}: status {:#x} packet {} buttons {:#06x} triggers {}/{} left stick {},{} right stick {},{}",
                                          user, status, uint32_t(state.packet_number), buttons,
                                          state.gamepad.left_trigger, state.gamepad.right_trigger,
                                          int16_t(state.gamepad.thumb_lx), int16_t(state.gamepad.thumb_ly),
                                          int16_t(state.gamepad.thumb_rx), int16_t(state.gamepad.thumb_ry));
                    }
                    s.status = status;
                    s.buttons = buttons;
                    s.packet = uint32_t(state.packet_number);
                    s.any = s.any || status == 0;
                }
            }
        }
    }).detach();
}

#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

void Hook_LzxDecompressPre(PPCRegister& r1) {}
void Hook_LzxDecompressPost(PPCRegister& r1, PPCRegister& r3) {}
void MCLA_StreamOpenResult(PPCRegister& r1, PPCRegister& r3) {}
void MCLA_RageFatal(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) {}
void MCLA_FileOpenResult(PPCRegister& r1, PPCRegister& r3) {}
void MCLA_InflateStep(PPCRegister& r3, PPCRegister& r4) {}
void MCLA_StreamBufferProbe(PPCRegister& r3) {}
void MCLA_StartInputProbe() {}
void MCLA_ResourceFixupError(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {}
void MCLA_ResourceSegments(PPCRegister& r31) {}
void MCLA_TuneFieldProbe(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) {}
#endif // REXGLUE_HAS_XEO3_TARGET
