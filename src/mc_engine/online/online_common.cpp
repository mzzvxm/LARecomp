#ifndef REXGLUE_HAS_XEO3_TARGET
//
// Guest-memory helpers shared by the online hook modules and hooks.cpp.
//
// These used to live in an anonymous namespace in hooks.cpp (internal linkage),
// so the split-out online translation units could not link against them. They
// now have external linkage here; hooks.cpp and online/*.cpp both use them via
// online_common.h.
//

#include "online_common.h"

#include <rex/system/xmemory.h>  // rex::memory::Memory::TranslateVirtual

// Guest pointers span the whole address space: the XEX image sits at
// 0x82000000, but heap objects (paint/vinyl block, e.g. 0xBF4FE930) live in the
// physical heaps far above 0x90000000. Only reject null / obviously-tiny values.
bool IsGuestPtr(uint32_t ea) { return ea >= 0x10000u && ea < 0xFFFF0000u; }

uint32_t GuestRead32(rex::memory::Memory* mem, uint32_t addr) {
    const auto* p = mem->TranslateVirtual<const uint8_t*>(addr);
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint16_t GuestRead16(rex::memory::Memory* mem, uint32_t addr) {
    const auto* p = mem->TranslateVirtual<const uint8_t*>(addr);
    return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

// Reads a NUL-terminated ASCII string from guest memory (bounded).
std::string GuestCStr(rex::memory::Memory* mem, uint32_t addr, uint32_t maxlen) {
    std::string s;
    if (!IsGuestPtr(addr)) return s;
    for (uint32_t i = 0; i < maxlen; ++i) {
        char c = static_cast<char>(*mem->TranslateVirtual<const uint8_t*>(addr + i));
        if (!c) break;
        s.push_back(c);
    }
    return s;
}

void GuestWrite32(rex::memory::Memory* mem, uint32_t addr, uint32_t v) {
    auto* p = mem->TranslateVirtual<uint8_t*>(addr);
    p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;  p[3] = v & 0xFF;
}

void GuestWrite16(rex::memory::Memory* mem, uint32_t addr, uint16_t v) {
    auto* p = mem->TranslateVirtual<uint8_t*>(addr);
    p[0] = (v >> 8) & 0xFF; p[1] = v & 0xFF;
}

void GuestWrite8(rex::memory::Memory* mem, uint32_t addr, uint8_t v) {
    *mem->TranslateVirtual<uint8_t*>(addr) = v;
}

#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

#endif // REXGLUE_HAS_XEO3_TARGET
