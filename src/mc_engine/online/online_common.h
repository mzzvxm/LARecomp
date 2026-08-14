#pragma once
//
// Shared plumbing for the MCLA online hook modules (tournament, System Link,
// Xbox Live). The guest-memory helpers below are defined once in
// mc_engine/hooks.cpp; the online translation units use them through these
// declarations so the online code can live outside the giant hooks.cpp.
//

#include <cstdint>
#include <string>

#include <rex/ppc/context.h>  // PPCRegister
#include <rex/runtime.h>      // rex::Runtime

#include "larecomp_log.h"     // LARECOMP_APP_INFO

namespace rex {
namespace memory {
class Memory;
}
}  // namespace rex

// Guest-memory helpers (big-endian). Defined in mc_engine/hooks.cpp.
bool IsGuestPtr(uint32_t ea);
uint32_t GuestRead32(rex::memory::Memory* mem, uint32_t addr);
uint16_t GuestRead16(rex::memory::Memory* mem, uint32_t addr);
std::string GuestCStr(rex::memory::Memory* mem, uint32_t addr, uint32_t maxlen = 256);
void GuestWrite32(rex::memory::Memory* mem, uint32_t addr, uint32_t v);
void GuestWrite16(rex::memory::Memory* mem, uint32_t addr, uint16_t v);
void GuestWrite8(rex::memory::Memory* mem, uint32_t addr, uint8_t v);
