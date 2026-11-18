// Symbols the Windows-only code defines, for the build where those files are
// not compiled: the D3D12 native graphics runtime. Plume is the renderer here.
// (The DualSense and Xbox trigger effects build everywhere, through SDL off Windows.)

#include <rex/cvar.h>

#include "native_gfx/native_gfx.h"


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
