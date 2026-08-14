#pragma once
// ===========================================================================
// MILESTONE 0 — TEMPORARY INSTRUMENTATION. NOT PART OF THE GAME.
// ===========================================================================
// Passive D3D9-Xenon probe. See mcla_gfx_probe.cpp for the full contract.
//
// Calling Install() is optional: the hooks are weak-symbol overrides of the
// recompiled functions and are always linked. Install() only logs that the
// capture is armed. Everything is gated behind the `mcla_gfx_probe` cvar,
// which defaults to false.
// ===========================================================================

#include <cstdint>

namespace mcla::gfx_probe {

// Announce the capture window. Safe to call more than once; no-op when the
// cvar is off.
void Install();

// Tiling bracket bookkeeping, driven by the graphics hooks (which live in
// native_gfx/hooks.cpp — single owner of the guest graphics hooks, shared
// with the Native Graphics Runtime).
//   OpenBracket    grcDevice::BeginTiledRendering  (sub_8217A470)
//   SetBracketTiles D3DDevice_BeginTiling          (sub_8241BE78)
//   NoteEndTiling  D3DDevice_EndTiling             (sub_8241C308)
//   CloseBracket   grcDevice::EndTiledRendering    (sub_8217B430)
void OpenBracket(uint32_t caller_marker, uint32_t surface_info);
void SetBracketTiles(uint32_t tiles);
void NoteEndTiling(uint32_t flags, uint32_t rects, uint32_t dest);
void CloseBracket();

// Capture entry points, also called from native_gfx/hooks.cpp.
bool Enabled();
void RecordDraw(const uint8_t* base, uint32_t dev, uint32_t prim_type, uint32_t elements,
                uint32_t indexed, bool is_up);
void OnFrameEnd();
void WriteReport();

}  // namespace mcla::gfx_probe
