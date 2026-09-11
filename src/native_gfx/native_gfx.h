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
void NotifyResolve(const uint8_t* base, uint32_t dev, uint32_t flags,
                   uint32_t dest_texture, uint32_t source_rect, uint32_t dest_point,
                   uint32_t clear_color_ptr);

// Called from the rage::grcDevice::EndFrame hook.
void NotifyFrameBoundary();

// D3DResource_Lock (sub_82421CA0), the funnel every lock reaches.
// Observation only: the lock is where the guest waits on the resource fence,
// which is the machinery a future ownership step has to take over.
void NoteResourceLocked(const uint8_t* base, uint32_t resource_va);

// D3DResource_Unlock (sub_82421F38), BEFORE the original -- it resets the two
// flush words it is read for and decrements the lock count.
//
// Hooked at the funnel rather than at the texture thunk: xref says exactly
// three thunks reach it (texture, vertex buffer, index buffer) and nothing
// else in the binary unlocks anything, so this one hook sees every unlock of
// every type. The vertex and index buffer locks are virtual methods
// (grcVertexBufferD3D's vtable at off_820106BC, slots 1 and 2), so no amount
// of static analysis finds their callers -- counting them at runtime is the
// only way to know whether anything is locked per frame.
void NoteResourceUnlocked(const uint8_t* base, uint32_t resource_va, uint32_t base_address,
                          uint32_t mip_address);

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

// D3DDevice_Clear (sub_824195E8), the one funnel every guest clear reaches:
// grcDevice::Clear (sub_82178370) builds the flags -- 0xF colour, 0x10 depth,
// 0x20 stencil -- and forwards colour in r7 as D3DCOLOR (0xAARRGGBB).
//
// The runtime never sees this clear otherwise: it lands in EDRAM through the
// command processor, which no-CP mode does not run, so the pool's own policy
// clear paints kClearColor over a surface the game asked to be something else.
// Measured in "cap_capture.rdc": the ShadowBlend target starts 100% at
// kClearColor and only 18% of it is ever drawn, so the far 80% reads as full
// shadow -- the "lighting turns off at distance". The shadow driver's own
// clear of that surface is 0xFF7F7F7F, i.e. exactly the 0.5 neutral its draws
// write.
void NoteGuestClear(uint32_t flags, uint32_t color, float z, uint32_t stencil);

// Consumes the last guest colour clear, if one arrived since the previous
// call, writing it as linear RGBA. The pool's policy clear calls this so the
// surface starts at the colour the game asked for.
bool TakeGuestClearColor(float rgba[4]);

// The same pending slot, but reporting WHICH buffers the guest asked to clear
// and the depth value it asked for. A guest clear that arrives after a target
// was already cleared this frame still has to be honoured: one pooled target
// is shared by every pass of the same shape, so the second pass of a frame
// (impostor atlas generation renders one tree species per pass into the same
// 256x256 target) would otherwise inherit the previous pass's pixels. Measured:
// the foliage impostor atlases came back with the previous species smeared over
// the background instead of black, which puts every texel above the shadow
// shader's 10/255 cut and turns every tree shadow into a square.
struct GuestClearRequest {
  bool color = false;
  bool depth = false;
  float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float z = 0.0f;
};
bool TakeGuestClear(GuestClearRequest* out);

// A resolve that also clears its source (RB_COPY_CONTROL colour/depth clear
// bits) arms the same pending slot D3DDevice_Clear uses.
void SetPendingResolveClear(const float rgba[4]);

// Arms a RenderDoc capture of the next whole guest frame. Callable from any
// thread; the request is consumed at the next frame boundary, which is where
// this runtime's capture has to be bracketed (see renderdoc_hook.h). Same
// effect as dropping a `native_gfx_rdc_trigger` file next to the exe, which
// still works for scripts.
void RequestRenderDocCapture();

}  // namespace mcla::native_gfx
