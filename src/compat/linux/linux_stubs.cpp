// Symbols the Windows-only code defines, for the build where those files are
// not compiled: the D3D12 native graphics runtime. Plume is the renderer here.
// (The DualSense and Xbox trigger effects build everywhere, through SDL off Windows.)

#include <rex/cvar.h>

#include "native_gfx/native_gfx.h"

// The cvars native_gfx.cpp defines and the guest-side files Plume shares read.
REXCVAR_DEFINE_BOOL(mcla_native_gfx, false, "MCLA/NativeGfx",
                    "The D3D12 native graphics runtime. Not built on this platform.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart)
    .debug_only();
REXCVAR_DEFINE_BOOL(mcla_native_gfx_decl_float, true, "MCLA/NativeGfx",
                    "Read the two 32-bit float vertex declaration types at their measured width.")
    .debug_only();
REXCVAR_DEFINE_BOOL(mcla_native_gfx_depth32, false, "MCLA/NativeGfx",
                    "Give a guest kD24S8 depth surface a D32_FLOAT_S8X24 host format.")
    .debug_only();
REXCVAR_DEFINE_BOOL(mcla_native_gfx_diag, false, "MCLA/NativeGfx",
                    "Native graphics diagnostics. Not built on this platform.")
    .debug_only();
REXCVAR_DEFINE_UINT32(mcla_native_gfx_fetch_size_unit, 4, "MCLA/NativeGfx",
                      "Bytes per unit of the vertex fetch constant's 24-bit size field. 0 uses 4.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();

namespace mcla::native_gfx {
// Only the D3D12 pipeline cache listens for it.
void NotifyWindowCloseRequested() {}

// hooks.cpp feeds the D3D12 runtime through these. It is not built here, so
// Active() is false and every observer is a no-op; Plume gets the same guest
// events through the mcla::gpu calls beside them.
bool Active() { return false; }
bool PresentTakeover() { return false; }
bool PresentFrame() { return false; }
void NoteSwapHook() {}
void NotifyFrameBoundary() {}
void NotifyResolve(const uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                   uint32_t) {}
void TryFirstDraw(const uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t, int32_t, bool) {}
void NoteResourceLocked(const uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t) {}
void NoteResourceUnlocked(const uint8_t*, uint32_t, uint32_t, uint32_t) {}
void DumpLockedMippedTextureForDiag(const uint8_t*, uint32_t, uint32_t) {}
void NoteD3DTextureCreated(const uint8_t*, uint32_t) {}
void NoteGuestDraw(int) {}
void NoteBeginVertices(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
void NoteEndVertices(const uint8_t*, uint32_t) {}
void NoteGuestClear(uint32_t, uint32_t, float, uint32_t, uint32_t, uint32_t) {}
void TelemetryRecordDraw(const uint8_t*, uint32_t, uint32_t, uint32_t, bool) {}
void TelemetryOnFrameEnd() {}
void TelemetryRecordGeometry(const uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t, int32_t, bool,
                             uint64_t, uint64_t) {}
}  // namespace mcla::native_gfx
