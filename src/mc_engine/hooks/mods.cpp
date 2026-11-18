// Modloader glue: xarchive_mods.rpf mounted after the shipped archives, and the
// driver animation names of a car the game does not ship.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <cstddef>
#include <cstdint>

#include "hooks.h"
#include "../modloader/modloader.h"

// Archive list injection for the modloader. sub_822C4630 mounts a ';'-separated
// list of packfiles, all at "a:/archive/". Which list it uses is decided across
// three branches (the caller's argument, the "audlo" fallback, the
// dword_8288BA44 override) that merge at 0x822C4858, and 0x822C4860 copies the
// winner into a 511-byte stack buffer at r1+0xD0. This hook sits on the
// instruction right after that copy and appends to the buffer in place.
//
// Appending rather than repointing matters twice over: it keeps whichever list
// the game picked, and it writes to the guest stack. Parking a string in the
// dead stub region instead is not an option -- those pages belong to the XEX
// image and are mapped read-only, so writing there faults.
//
// xarchive_mods.rpf ends up mounted last, and fiDevice::GetDevice
// (sub_821CB488) searches a mount point's devices last-registered-first,
// falling through when one does not hold the file -- so the mod archive
// overrides per file and everything else still comes from the shipped ones.
void Patch_ArchiveList(PPCRegister& r1) {
    constexpr uint32_t kListBufferOffset = 0xD0;  // v43 in sub_822C4630's frame
    constexpr size_t kListBufferSize = 512;       // copied with a 511-byte bound
    mc::modloader::AppendModArchiveTo(static_cast<uint32_t>(r1.u64) + kListBufferOffset,
                                      kListBufferSize);
}

// Driver animations of a new car. The game files them under the car's name
// minus "vp_" and, when the pack is missing, falls back to the Challenger's
// (sub_823D2AB0) -- which in a cloned Impala's seat put the driver into the
// seat back with his hands off the wheel. The three buffers are renamed right
// after they are formatted, so the pack, its dictionary and every animation in
// it are the ones the donor's seat was authored for. See hooks.h.
void MCLA_DriverAnimPack(PPCRegister& r1) {
    mc::modloader::AliasDriverAnimName(static_cast<uint32_t>(r1.u64) + 0x80, 0x80);
}

void MCLA_DriverAnimDict(PPCRegister& r28, PPCRegister& r29) {
    mc::modloader::AliasDriverAnimName(static_cast<uint32_t>(r29.u64),
                                       static_cast<uint32_t>(r28.u64));
}

void MCLA_DriverAnimName(PPCRegister& r1) {
    mc::modloader::AliasDriverAnimName(static_cast<uint32_t>(r1.u64) + 0x50, 0x80);
}
#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

void MCLA_DriverAnimPack(PPCRegister& r1) {}
void MCLA_DriverAnimDict(PPCRegister& r28, PPCRegister& r29) {}
void MCLA_DriverAnimName(PPCRegister& r1) {}
#endif // REXGLUE_HAS_XEO3_TARGET
