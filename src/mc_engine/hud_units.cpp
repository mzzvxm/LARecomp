#ifndef REXGLUE_HAS_XEO3_TARGET
//
// km/h on the live HUD speedometer. See hud_units.h for why this patches guest
// memory instead of shipping a modified resources/ui/hud/hud.xsf.
//
// RE map (default.xex + resources/ui/hud/hud.xsf):
//
//   mcUILogic = dword_8286D8D4, allocated 333248 bytes in sub_8220ED58.
//   sub_82203740(mcUILogic)  caches the HUD movie's GFx value handles:
//       +0x51484 d_Damagevalue   +0x51488 d_Speedvalue
//   sub_822112A8 @0x822117C0..0x82211800 writes |velocity| (m/s) into the
//       d_Speedvalue handle. Those two are the only references to +0x51488 in
//       the whole image, so nothing else consumes the value.
//
//   hud.xsf action block @0xb982, file offset 0xDBB7:
//       96 09 00 06 <double 2.237> 0C 18 4F
//       = Push "text",_global; GetMember d_Speedvalue; Push 2.237; Multiply;
//         ToInteger; SetMember
//     AVM1 stores a double with its two 32-bit halves swapped, so 3.6 is
//     CC CC 0C 40 CD CC CC CC.
//
//   hud.xsf file offset 0x74A30, the unit label as static text:
//       00 05 | 00 03 | {004D 01BD}{0050 0136}{0048 0148}
//         ^y    ^count   ^{u16 glyph index, u16 advance} x3
//     The movie's fonts are contiguous ASCII 0x20..0x7E (95 glyphs), so the
//     index is ord(c) - 0x20: 0x4D='m', 0x50='p', 0x48='h'. Rewriting them to
//     k,m,h keeps the record the same length, which is what makes this a poke
//     rather than a re-serialisation. 'k' borrows 'h' advance.
//
// The speed-limit sign is deliberately left alone: its number is produced by
// the guest in mph. sub_822068F8 @0x82206E7C does have a km/h branch (|v| *
// 3.6 * 0.2 + 1, then x5, i.e. rounded to the nearest 5) but it is dead code --
// it needs bit 0x80 of the byte at mcUILogic+0x515B0, and that bit is only set
// when sub_82387928() returns 6, which it never does (it returns 0..4).
//

#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "hud_units.h"
#include "logging.h"

// Same switch that drives the game's own metric formatter, defined in hooks.cpp:
// one knob for units, not two. "kmh" converts the HUD, "game"/"mph" leave it.
REXCVAR_DECLARE(std::string, speed_units);

namespace {

// ── The two signatures, as they sit in the loaded movie ────────────────

// Push <double 2.237>; Multiply; ToInteger; SetMember. Unique in hud.xsf, and
// hud_dev.xsf carries the same line -- patching both is correct.
constexpr uint8_t kMultSig[] = {0x96, 0x09, 0x00, 0x06, 0x60, 0xE5, 0x01, 0x40,
                                0x4C, 0x37, 0x89, 0x41, 0x0C, 0x18, 0x4F};
constexpr uint32_t kMultDoubleOffset = 4;  // into the signature
// 3.6 as AVM1 stores it (high half first).
constexpr uint8_t kMultKmh[8] = {0xCC, 0xCC, 0x0C, 0x40, 0xCD, 0xCC, 0xCC, 0xCC};
constexpr uint8_t kMultMph[8] = {0x60, 0xE5, 0x01, 0x40, 0x4C, 0x37, 0x89, 0x41};

// The static-text record for "mph": y, glyph count, then three pairs.
constexpr uint8_t kGlyphSig[] = {0x00, 0x05, 0x00, 0x03, 0x00, 0x4D, 0x01, 0xBD,
                                 0x00, 0x50, 0x01, 0x36, 0x00, 0x48, 0x01, 0x48};
constexpr uint32_t kGlyphPairsOffset = 4;  // into the signature
// k,m,h with the advances following their glyph.
constexpr uint8_t kGlyphKmh[12] = {0x00, 0x4B, 0x01, 0x48, 0x00, 0x4D,
                                   0x01, 0xBD, 0x00, 0x48, 0x01, 0x48};
constexpr uint8_t kGlyphMph[12] = {0x00, 0x4D, 0x01, 0xBD, 0x00, 0x50,
                                   0x01, 0x36, 0x00, 0x48, 0x01, 0x48};

// Guest virtual range worth walking. The image itself starts at 0x82000000 and
// the heaps run out to the physical window; resources land well inside.
constexpr uint32_t kScanBegin = 0x82000000u;
constexpr uint32_t kScanEnd   = 0xC0000000u;
// Guest heaps are 64 KB granular, so that is the unit the walk asks about: one
// range query per chunk instead of sixteen per-page ones, and an unmapped chunk
// costs a single heap lookup.
constexpr uint32_t kChunk = 0x10000u;
// Per-frame budget of address space. The range is ~1 GB, so a cold sweep takes
// ~4 s of gameplay and then never runs again. Kept small on purpose: the heap
// queries take the memory lock, and a fat budget turns the sweep into a hitch.
constexpr uint32_t kBytesPerFrame = 4u * 1024u * 1024u;

enum class Want { kGame, kKmh };

struct Site {
    const uint8_t* sig;
    uint32_t        sig_len;
    uint32_t        field_offset;  // where the bytes to write start, into sig
    const uint8_t*  kmh;
    const uint8_t*  mph;
    uint32_t        field_len;
    const char*     name;
    uint32_t        found_at = 0;  // guest address of the signature start
};

Site g_sites[] = {
    {kMultSig, sizeof(kMultSig), kMultDoubleOffset, kMultKmh, kMultMph, 8, "speed multiplier"},
    {kGlyphSig, sizeof(kGlyphSig), kGlyphPairsOffset, kGlyphKmh, kGlyphMph, 12, "unit label"},
};

uint32_t g_cursor = kScanBegin;
Want     g_applied = Want::kGame;
bool     g_scanning = false;
bool     g_logged_miss = false;
// A cold sweep can run before the HUD movie exists, so a miss is retried rather
// than final. Frames, not seconds, so the pacing follows whatever fps the game
// is at -- this only needs to be "not every frame".
uint32_t g_retry_countdown = 0;
uint32_t g_attempts = 0;
constexpr uint32_t kRetryFrames = 600;
constexpr uint32_t kMaxAttempts = 20;

Want WantedUnits() {
    return REXCVAR_GET(speed_units) == "kmh" ? Want::kKmh : Want::kGame;
}

// A page we can touch without risking a fault. QueryProtect lives on BaseHeap,
// not on Memory, and it fails outright for anything that was never committed --
// which is most of the address space.
bool PageAccessible(rex::memory::Memory* mem, uint32_t address, bool need_write) {
    auto* heap = mem->LookupHeap(address);
    if (!heap) return false;
    uint32_t protect = 0;
    if (!heap->QueryProtect(address, &protect)) return false;
    const uint32_t want = rex::memory::kMemoryProtectRead |
                          (need_write ? rex::memory::kMemoryProtectWrite : 0u);
    return (protect & want) == want;
}

// TranslateVirtual rather than virtual_membase() + address: heaps carry a host
// offset (the physical window does), and raw arithmetic gets those wrong.
uint8_t* HostPointer(rex::memory::Memory* mem, uint32_t address) {
    return mem->TranslateVirtual<uint8_t*>(address);
}

// Writes one site's field. `to_kmh` picks which of the two byte strings goes in.
void WriteSite(rex::memory::Memory* mem, Site& site, bool to_kmh) {
    const uint32_t at = site.found_at + site.field_offset;
    if (!PageAccessible(mem, at, true) ||
        !PageAccessible(mem, at + site.field_len - 1, true)) {
        MC_WARN("[hud_units] {} at 0x{:08X} is not writable, skipped", site.name, at);
        return;
    }
    uint8_t* p = HostPointer(mem, at);
    if (!p) return;
    std::memcpy(p, to_kmh ? site.kmh : site.mph, site.field_len);
}

// True when the site still holds one of the two shapes we know. A movie reload
// leaves the old address holding something else entirely.
bool SiteStillOurs(rex::memory::Memory* mem, const Site& site) {
    const uint32_t at = site.found_at + site.field_offset;
    if (!PageAccessible(mem, at, false) ||
        !PageAccessible(mem, at + site.field_len - 1, false)) {
        return false;
    }
    const uint8_t* p = HostPointer(mem, at);
    if (!p) return false;
    return std::memcmp(p, site.kmh, site.field_len) == 0 ||
           std::memcmp(p, site.mph, site.field_len) == 0;
}

bool AllFound() {
    for (const Site& s : g_sites) {
        if (!s.found_at) return false;
    }
    return true;
}

void RestartScan() {
    for (Site& s : g_sites) s.found_at = 0;
    g_cursor = kScanBegin;
    g_scanning = true;
    g_logged_miss = false;
    g_retry_countdown = 0;
    g_attempts = 0;
}

// Longest signature, for the tail overlap between blocks.
constexpr uint32_t kMaxSigLen = sizeof(kGlyphSig) > sizeof(kMultSig) ? sizeof(kGlyphSig)
                                                                    : sizeof(kMultSig);

// Readable in 64 KB steps. One QueryRangeAccess beats sixteen QueryProtect
// calls, and the whole point of the chunk granularity is that most of the
// address space is not mapped at all: LookupHeap returning null lets a chunk be
// dismissed without touching the page tables.
bool ChunkReadable(rex::memory::Memory* mem, uint32_t chunk_base) {
    auto* heap = mem->LookupHeap(chunk_base);
    if (!heap) return false;
    return heap->QueryRangeAccess(chunk_base, chunk_base + kChunk - 1) !=
           rex::memory::PageAccess::kNoAccess;
}

// Walks up to kBytesPerFrame of guest address space looking for whichever
// signatures are still missing. Runs of consecutive readable 64 KB chunks are
// searched as one block; when a block is cut short by the frame budget the
// cursor steps back by kMaxSigLen-1 so a match straddling the cut is still seen.
// Dismissed chunks are charged against the budget too, otherwise a sweep over a
// mostly-empty address space would run the whole range in a single frame.
void ScanStep(rex::memory::Memory* mem) {
    uint32_t budget = kBytesPerFrame;

    while (budget >= kChunk && g_cursor < kScanEnd) {
        if (!ChunkReadable(mem, g_cursor)) {
            g_cursor += kChunk;
            budget -= kChunk;
            continue;
        }

        // Grow the block while the chunks stay readable and the budget lasts.
        uint32_t block_end = g_cursor + kChunk;
        while (block_end < kScanEnd && (block_end - g_cursor) < budget &&
               ChunkReadable(mem, block_end)) {
            block_end += kChunk;
        }
        const bool cut_by_budget = (block_end < kScanEnd) && ChunkReadable(mem, block_end);

        if (const uint8_t* block = HostPointer(mem, g_cursor)) {
            const uint32_t span = block_end - g_cursor;
            for (Site& site : g_sites) {
                if (site.found_at || span < site.sig_len) continue;
                for (uint32_t i = 0; i + site.sig_len <= span; ++i) {
                    if (block[i] == site.sig[0] &&
                        std::memcmp(block + i, site.sig, site.sig_len) == 0) {
                        site.found_at = g_cursor + i;
                        MC_INFO("[hud_units] {} found at 0x{:08X}", site.name, site.found_at);
                        break;
                    }
                }
            }
        }

        const uint32_t consumed = block_end - g_cursor;
        budget -= consumed < budget ? consumed : budget;
        g_cursor = (cut_by_budget && consumed > kMaxSigLen) ? block_end - (kMaxSigLen - 1)
                                                            : block_end;
        if (AllFound()) break;
    }

    if (AllFound()) {
        g_scanning = false;
        for (Site& site : g_sites) WriteSite(mem, site, true);
        g_applied = Want::kKmh;
        MC_INFO("[hud_units] km/h applied");
        return;
    }

    if (g_cursor >= kScanEnd) {
        g_scanning = false;
        ++g_attempts;
        if (g_attempts >= kMaxAttempts) {
            if (!g_logged_miss) {
                g_logged_miss = true;
                for (const Site& site : g_sites) {
                    if (!site.found_at) {
                        MC_WARN("[hud_units] {} never found after {} sweeps -- giving up",
                                site.name, g_attempts);
                    }
                }
            }
            return;
        }
        // Probably swept before the HUD movie was built. Try again shortly.
        g_retry_countdown = kRetryFrames;
    }
}

}  // namespace

void TickHudUnits() {
    const Want want = WantedUnits();

    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* mem = rt->memory();
    if (!mem) return;

    // Back to stock: undo in place, keep the addresses so a later flip does not
    // have to scan again.
    if (want == Want::kGame) {
        if (g_applied == Want::kKmh && AllFound()) {
            for (Site& site : g_sites) {
                if (SiteStillOurs(mem, site)) WriteSite(mem, site, false);
            }
            MC_INFO("[hud_units] reverted to mph");
        }
        g_applied = Want::kGame;
        g_scanning = false;
        return;
    }

    if (AllFound()) {
        // The movie survives for the whole session normally, but a title
        // relaunch (the DLC path does one) rebuilds it somewhere else. Two
        // comparisons a frame is cheap enough to keep this honest.
        for (Site& site : g_sites) {
            if (!SiteStillOurs(mem, site)) {
                MC_INFO("[hud_units] signature moved, rescanning");
                RestartScan();
                return;
            }
        }
        if (g_applied != Want::kKmh) {
            for (Site& site : g_sites) WriteSite(mem, site, true);
            g_applied = Want::kKmh;
            MC_INFO("[hud_units] km/h applied");
        }
        return;
    }

    if (g_logged_miss) return;  // gave up for this session

    if (!g_scanning) {
        if (g_retry_countdown) {
            --g_retry_countdown;
            return;
        }
        g_cursor = kScanBegin;
        g_scanning = true;
    }
    ScanStep(mem);
}

#else  // REXGLUE_HAS_XEO3_TARGET

void TickHudUnits() {}

#endif  // REXGLUE_HAS_XEO3_TARGET
