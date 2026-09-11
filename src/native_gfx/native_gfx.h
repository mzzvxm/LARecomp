#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — entry point
// ===========================================================================
// Gated entirely behind the `mcla_native_gfx` cvar (default OFF). With the
// cvar off every hook is a pass-through and the Xenia-provided path is
// untouched. With it on, the guest swap is intercepted: the native backend
// renders and presents through the existing rex::ui::Presenter (the real
// MCLA window), and the guest's own PM4 swap is suppressed so the two
// producers do not fight over the guest output texture.
//
// Current stage: Phase 2 smoke test — a colored triangle. See
// d3d12/d3d12_smoke_triangle.h.
// ===========================================================================

#include <cstdint>

namespace mcla::native_gfx {

// True when the cvar is on AND the D3D12 backend/presenter are available.
// First call performs lazy initialization; failures latch to disabled and
// the game continues on the normal path.
bool Active();

// True when the native runtime should take over presentation (suppressing
// the guest swap). Requires both mcla_native_gfx and mcla_native_gfx_present.
// Keep off while the Xenia CP still consumes the game's draws — see the
// cvar description (TDR hazard).
bool PresentTakeover();

// Attempts the first real MCLA draw through the native pipeline, once, when
// mcla_native_gfx_firstdraw is on. Renders to its own target and writes a
// readback + full diagnostic instead of presenting (see d3d12/first_draw.h).
void TryFirstDraw(const uint8_t* base, uint32_t dev, uint32_t primitive_type,
                  uint32_t element_count, uint32_t start_element, int32_t base_vertex,
                  bool indexed);

// Presents one native frame through the Presenter. Called from the guest
// swap hook. No-op (returns false) when PresentTakeover() is false.
// Called from the D3DDevice_Resolve hook. Ties the guest address a resolve
// writes to the native render target that produced it, so a later texture
// fetch at that address finds a real resource instead of stale guest memory.
void NotifyResolve(const uint8_t* base, uint32_t dev, uint32_t flags, uint32_t dest_texture,
                   uint32_t source_rect, uint32_t dest_point);

// Called from the rage::grcDevice::EndFrame hook.
void NotifyFrameBoundary();

// Called from the D3DDevice_Swap hook AFTER the guest swap, in continuous mode:
// presents the native frame last so it wins the shared guest output, and
// advances to the next frame. No-op outside continuous mode.
void PresentContinuousAtSwap();

// D3DDevice_CreateTexture (sub_82410C50), AFTER the original, which returns
// the new D3DTexture in r3 (0 on failure). This is the road that ALLOCATES:
// header, base pixels and mip pixels are three separate guest allocations, and
// the pages land in the fetch constant at +32 and +48. The site pixel
// ownership would take over.
void NoteD3DTextureCreated(const uint8_t* base, uint32_t d3d_texture_va);

// Whether the capture should write out the resolve destinations.
bool ShouldDumpRenderTargets();

bool PresentFrame();

// TEMP INSTRUMENTATION: which entry point the guest used to issue a draw.
// Only DrawIndexedVertices ever reached the renderer — DrawVertices was
// telemetry-only and DrawVerticesUP was not handled at all — so everything the
// game draws through those two is absent from every image produced so far.
void NoteGuestDraw(int kind);  // 0 = indexed, 1 = non-indexed, 2 = UP

// D3DDevice_BeginVertices: records the shape of an inline-geometry draw. The
// vertices are NOT readable yet — the caller writes them into `buffer` after
// this returns, which is why the draw is issued from NoteEndVertices instead.
void NoteBeginVertices(uint32_t dev, uint32_t primitive_type, uint32_t vertex_count,
                       uint32_t stride, uint32_t buffer);

// D3DDevice_EndVertices: the inline vertices are complete here.
void NoteEndVertices(const uint8_t* base, uint32_t dev);

// Arms a RenderDoc capture of the next whole guest frame. Callable from any
// thread; the request is consumed at the next frame boundary, which is where
// this runtime's capture has to be bracketed (see renderdoc_hook.h). Same
// effect as dropping a `native_gfx_rdc_trigger` file next to the exe, which
// still works for scripts.
void RequestRenderDocCapture();

}  // namespace mcla::native_gfx
