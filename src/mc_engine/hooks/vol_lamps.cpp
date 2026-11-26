//
// The game's per-frame street lamp list, for the renderer's volumetric light (src/gpu/volumetrics_inputs.h, vol_lamp_list.h).
//
// The prop lamp pass (sub_82729438 -> AddLight sub_823029D0) appends every spot lamp within 300 m of the eye to a list at 0x8288B8D8, and the cone drawer
// (sub_82302810) sorts it, draws a cone mesh for each record (in rain only) and empties it. The list is complete at the drawer's entry and gone after
// it (perf/out/NIGHT-PROBE.txt C3 and C5: mean 64 lamps and at most 83 in a clear night, 451 empty frames of 15600 by day and in menus). This hook runs
// at the drawer's entry and copies the whole list into gpu/vol_lamp_list.h's table, once a frame, while the volumetric light is on; the render thread
// takes the newest copy at its trigger. Reads only, nothing is written into the guest, no file is written.
//
// The hook sits at 0x82302814, one instruction after the entry, on purpose: a midasm hook is keyed by its address (the SDK keeps the last of two), and the
// night probe's MCLANightConeDraw (test/night-probe, a test branch that is never merged to main) is at 0x82302810. This is the only hook in the cone
// drawer on main. The instruction at 0x82302810 is `mflr r12`, which writes only r12; the hook reads no register (the list is at a fixed address) and
// nothing else branches to 0x82302814, so both hooks run when the probe is merged over this.
//
// A record is 128 bytes (vol_lamp_list.h): +0 the distance to the eye, +16 the game's unpacked 96-byte light (position, colour, direction, intensity,
// cone angle, cone spread, type, on), +112 AddLight's scale. The header at 0x8288B8D8 is {u32 array pointer, u16 count, u16 capacity}. The array is read
// the way the guest reads it itself a few instructions later (the drawer takes the count from the same header), after one readable-memory check of the
// whole range (a VirtualQuery or two a frame: the list can be a stale pointer in a menu).
//
#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/ppc/context.h>
#include <rex/kernel/title_runtime.h>

#include <algorithm>
#include <array>
#include <cstdint>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#include "gpu/menu_world.h"
#include "gpu/vol_lamp_list.h"
#include "hooks_internal.h"

namespace {

const uint8_t* Base() {
    auto* rt = REX_RUNTIME();
    return rt ? rt->GetMembase().GetBase() : nullptr;
}

bool PlausiblePtr(uint32_t p) { return p >= 0x10000u && p < 0xE0000000u; }

// Is [ea, ea + len) committed, readable host memory? (The night probe's and traffic_probe.cpp's test.)
bool Mapped(const uint8_t* base, uint32_t ea, uint32_t len) {
    if (!base || ea < 0x10000u || len == 0 || uint64_t(ea) + len > 0x100000000ull) return false;
#if defined(_WIN32)
    constexpr DWORD kRead = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    uint64_t at = ea;
    const uint64_t end = uint64_t(ea) + len;
    while (at < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(base + at, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0 || (mbi.Protect & kRead) == 0) return false;
        const uint64_t region_end = uint64_t(static_cast<const uint8_t*>(mbi.BaseAddress) - base) + mbi.RegionSize;
        if (region_end <= at) return false;
        at = region_end;
    }
    return true;
#else
    return ea < 0xE0000000u;
#endif
}

}  // namespace

// 0x82302814, one instruction into the cone drawer (larecomp_config.toml): the per-frame list is complete.
void MCLAVolLampList() {
    namespace vl = mcla::gpu::vol_lamps;
    if (!vl::Wanted()) return;
    const uint8_t* base = Base();
    if (!base || !Mapped(base, vl::kListHeaderAddress, 8)) return;
    const uint32_t array = ReadGuestU32(base, vl::kListHeaderAddress);
    const uint32_t count = std::min<uint32_t>(vl::BeU16(base + vl::kListHeaderAddress + 4), vl::kMaxRecords);
    thread_local std::array<vl::GameLamp, vl::kMaxRecords> lamps;
    uint32_t n = 0;
    if (count != 0 && PlausiblePtr(array) && Mapped(base, array, count * vl::kRecordBytes)) {
        for (uint32_t i = 0; i < count; ++i) {
            const vl::GameLamp l = vl::DecodeRecord(base + array + i * vl::kRecordBytes);
            if (vl::Plausible(l)) lamps[n++] = l;
        }
    }
    vl::Global().Publish(lamps.data(), n, vl::Table::Clock::now(), mcla::gpu::menu_world::GarageLighting());  // an empty list is published too: a day, a menu
}

#else  // REXGLUE_HAS_XEO3_TARGET
#include <rex/ppc/context.h>
void MCLAVolLampList() {}
#endif  // REXGLUE_HAS_XEO3_TARGET
