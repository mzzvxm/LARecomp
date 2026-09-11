#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — shader identity
// ===========================================================================
// The Xbox 360 D3D runtime rewrites vertex-fetch instructions inside the
// shader microcode when a vertex declaration is bound (stride / offset /
// format / dst_swiz / mini-fetch all come from the declaration and ship
// zeroed in the RPF/XEX containers). The microcode the GPU executes is
// therefore NOT the microcode on disk, and a raw hash of runtime microcode
// never matches the offline shader database (measured: 0% hit rate).
//
// Identity key: FNV-1a 64 over the microcode with every kVertexFetch
// instruction normalized (payload zeroed, opcode kept). Validated offline on
// all 2056 shipped containers (1937 RPF + 119 XEX): 2056 distinct keys, zero
// collisions, and 447/447 runtime-captured blobs resolve (100.000% of draws).
//
// Everything here is pure and dependency-free on purpose: it must be
// testable against the offline table without booting the game.
//
// Microcode layout notes (see rex/graphics/format/ucode.h):
//  - dwords are big-endian in guest memory; `ucode` params here take the raw
//    guest byte stream and byte-swap internally.
//  - control flow: two 48-bit instructions per 3 dwords, at the start of the
//    blob; the CF section ends at the lowest exec.address * 3.
//  - exec.sequence has 2 bits per instruction; bit 0 set = fetch (not ALU).
//  - a fetch instruction is 3 dwords; word0 low 5 bits = FetchOpcode,
//    kVertexFetch == 0.
// ===========================================================================

#include <cstddef>
#include <cstdint>

namespace mcla::native_gfx {

// FNV-1a 64 over raw bytes. Matches the offline table generator exactly.
uint64_t Fnv1a64(const uint8_t* data, size_t size);

// Zeroes the payload of every vertex-fetch instruction in `ucode` (raw
// big-endian guest bytes, modified in place). Returns the number of vfetch
// instructions normalized. Safe on truncated/garbage input: anything that
// does not decode as expected is left untouched.
size_t NormalizeVertexFetches(uint8_t* ucode, size_t size);

// True when any vertex-fetch instruction in `ucode` takes its index from a
// register component other than r0.x -- the component the hardware preloads
// with the vertex index (rexglue's own translator does it at
// DxbcShaderTranslator::StartVertexShader_LoadVertexIndex, which writes the
// index to GPR 0.x and zeroes every other register). Such a shader computes
// its own fetch index, so the attribute the input assembler hands it at index
// i is NOT the one the hardware would have fetched.
//
// Measured over all 1279 shipped vertex shaders: 4 do this
// (xPropFoliageImpostor x3 and xrain_system__ParticleRenderVS), every one
// multiplying the index by 0.25 -- four corners folded onto one source vertex.
// Every other vfetch in the game reads r0.x straight.
//
// Safe on the RUNTIME microcode, not just the shipped copy: the D3D vfetch
// patcher (sub_82423A38) rebuilds word 0 as `(slot bits & 0x7F00000) | (old &
// 0xC00FFFFF)`, and that mask preserves both srcRegister (bits 5-10) and
// srcSwizzle (bits 30-31). Only constIndex/constIndexSelect are rewritten.
bool HasComputedVertexFetchIndex(const uint8_t* ucode, size_t size);

// The runtime shader identity: copy, normalize, hash. `ucode` is the raw
// big-endian microcode as read from guest memory (D3D shader object).
// Returns 0 for degenerate input (null/empty/oversized).
uint64_t ShaderIdentity(const uint8_t* ucode, size_t size);

// Upper bound accepted by ShaderIdentity. The largest shipped microcode is
// ~2 KB; 64 KB leaves two orders of magnitude of slack while still rejecting
// a corrupted size field before it can hash garbage.
inline constexpr size_t kMaxUcodeBytes = 64u * 1024u;

}  // namespace mcla::native_gfx
