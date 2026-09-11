#ifndef REXGLUE_HAS_XEO3_TARGET
// ===========================================================================
// MCLA graphics guest hooks — SINGLE OWNER
// ===========================================================================
// A rex_sub_* weak-symbol override can only be defined once per binary, and
// two subsystems need these interception points:
//
//   - the Milestone 0 passive probe (src/native_probe/, cvar mcla_gfx_probe)
//   - the Native Graphics Runtime  (src/native_gfx/,   cvar mcla_native_gfx)
//
// So every graphics hook body lives here and dispatches to both. Behavior
// with both cvars off is a byte-for-byte pass-through.
//
// Hooked functions (all confirmed by decompilation during Milestone 0):
//   sub_8241D620  D3DDevice_DrawIndexedVertices(dev, prim, baseVtx, startIdx, count)
//   sub_8241D230  D3DDevice_DrawVertices(dev, prim, startVertex, count)
//   sub_8241CD88  D3DDevice_BeginVertices(dev, prim, count, stride) — NOT a
//                 draw. It ALLOCATES a command-buffer region and returns its
//                 address in r3; the caller then writes the vertices into it.
//                 Reading the data here yields uninitialised memory.
//   sub_8241D220  D3DDevice_EndVertices(dev) — commits the write pointer
//                 (dev+48 = dev+13444). This is the moment the vertices exist
//                 and the draw becomes real, so it is the hook that matters.
//   sub_8217A470  rage::grcDevice::BeginTiledRendering
//   sub_8241BE78  D3DDevice_BeginTiling
//   sub_8217B430  rage::grcDevice::EndTiledRendering
//   sub_8241C308  D3DDevice_EndTiling (the resolve)
//   sub_8217B7B0  rage::grcDevice::EndFrame (frame boundary)
//   sub_82410C50  D3DDevice_CreateTexture — header + base pixels + mip pixels
//   sub_82421CA0  D3DResource_Lock — the one funnel every lock reaches
//   sub_82412990  D3DDevice_BlockUntilIdle — fence wait plus a raw spin on
//                 dev+11008 that only a real GPU ever clears
//   sub_82426468  D3DDevice_InitializeEngines — registers the guest's graphics
//                 interrupt handler (sub_82411478) and owns the flip queue
//   sub_82421F38  D3DResource_Unlock — the one funnel every unlock reaches;
//                 carries the guest's own dirty range
//   sub_82177CB0  grcTextureFactoryXenon::DestroyTexture(D3DTexture**) — the one
//                 funnel every texture teardown passes through
//   sub_82184458  grcTextureXenon::grcTextureXenon(datResource&) — the road every
//                 STREAMED texture takes; nothing is created, the embedded header
//                 is only fixed up
//   sub_82419E98  D3DDevice_Swap (only caller of VdSwap)
//   sub_82420BA8  D3DDevice_Resolve (EDRAM -> main memory; the only way a
//                 rendered image becomes samplable, so the only place a
//                 texture fetch address can be tied to a render target)
// ===========================================================================

#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc.h>

#include "../native_probe/mcla_gfx_probe.h"
#include "guest/guest_constants.h"
#include "guest/guest_fence.h"
#include "guest/texture_ownership.h"
#include "guest/vblank_probe.h"
#include "nocp/nocp_app.h"
#include "native_gfx.h"
#include "telemetry.h"

// Defined in native_gfx.cpp at global scope, so the declaration goes here at
// global scope too. Read directly rather than through Active(), which latches
// its failure and would be the wrong thing to touch this early.
REXCVAR_DECLARE(bool, mcla_native_gfx);

REX_EXTERN(__imp__D3DDevice_DrawIndexedVertices);
REX_EXTERN(__imp__D3DDevice_DrawVertices);
REX_EXTERN(__imp__D3DDevice_BeginVertices);
REX_EXTERN(__imp__D3DDevice_EndVertices);
REX_EXTERN(__imp__grcDevice_BeginTiledRendering);
REX_EXTERN(__imp__D3DDevice_BeginTiling);
REX_EXTERN(__imp__grcDevice_EndTiledRendering);
REX_EXTERN(__imp__D3DDevice_EndTiling);
REX_EXTERN(__imp__grcDevice_EndFrame);
REX_EXTERN(__imp__D3DDevice_Swap);
REX_EXTERN(__imp__D3DDevice_Resolve);
REX_EXTERN(__imp__grcTextureXenon_Init);
REX_EXTERN(__imp__grcTextureXenon_ResourceCtor);
REX_EXTERN(__imp__grcTextureFactoryXenon_DestroyTexture);
REX_EXTERN(__imp__D3DResource_Lock);
REX_EXTERN(__imp__D3DDevice_InitializeEngines);
REX_EXTERN(__imp__D3DDevice_BlockUntilIdle);
REX_EXTERN(__imp__rex_sub_82421F38);
REX_EXTERN(__imp__rex_sub_824195E8);
REX_EXTERN(__imp__D3DDevice_CreateTexture);
REX_EXTERN(__imp__grcTextureXenon_dtor);
// --- vblank / flip ---------------------------------------------------------

// D3DDevice_InitializeEngines(device) -- sub_82426468. Registers the guest's
// graphics interrupt handler:
//
//   VdInitializeEngines(0x1B540000, sub_82425D78, 0, ...)
//   VdSetGraphicsInterruptCallback(sub_82411478, device)
//
// so the callback is a constant and the device is r3. Both are what a runtime
// with no command processor has to drive itself: with config.graphics =
// nullptr the SDK's Vd export drops the registration on the floor and no
// vblank is ever delivered.
//
// Observed AFTER the original, read-only.
extern "C" REX_FUNC(D3DDevice_InitializeEngines) {
  mcla::native_gfx::nocp::NoteHook("D3DDevice_InitializeEngines");
  const uint32_t device = ctx.r3.u32;
  __imp__D3DDevice_InitializeEngines(ctx, base);
  mcla::native_gfx::NoteGraphicsEnginesInitialized(base, device);
}

// D3DDevice_BlockUntilIdle -- sub_82412990. Waits for everything submitted to
// finish:
//
//   BlockOnFence(dev, dev[10908], 4, 0, 0);   // the issued fence
//   while ( dev[11008] ) ;                    // then a raw spin, no sleep
//
// The fence half is satisfied by the vblank thread retiring fences. The spin
// is not: dev+11008 is cleared by the GPU side, and in this mode there is no
// GPU side. Measured: the title released from the fence, ran a little further
// (ring kicks 4 -> 8, 63 fences issued) and then stopped dead with the fence
// pair fully caught up -- which is exactly this spin.
//
// With no command processor, "block until the GPU is idle" is trivially true:
// nothing was ever handed to a GPU. So the whole call goes away. This is the
// same answer reblue reached (REX_STUB(D3DDevice_BlockUntilIdle)).
extern "C" REX_FUNC(D3DDevice_BlockUntilIdle) {
  mcla::native_gfx::nocp::NoteHook("D3DDevice_BlockUntilIdle");
  if (mcla::native_gfx::nocp::WantNoCommandProcessor()) {
    return;
  }
  __imp__D3DDevice_BlockUntilIdle(ctx, base);
}


// --- draws -----------------------------------------------------------------

// D3DDevice_DrawIndexedVertices(dev, primType, baseVertexIndex, startIndex, indexCount)
extern "C" REX_FUNC(D3DDevice_DrawIndexedVertices) {
  mcla::native_gfx::nocp::NoteHook("D3DDevice_DrawIndexedVertices");
  const uint32_t dev = ctx.r3.u32;
  const uint32_t prim = ctx.r4.u32;
  const uint32_t count = ctx.r7.u32;
  const uint32_t base_vertex = ctx.r5.u32;
  const uint32_t start_index = ctx.r6.u32;
  // The ALU constant dirty masks MUST be sampled before the original draw:
  // it flushes them and writes zero back (sub_8241D620 / sub_824238E0).
  const mcla::native_gfx::ConstantDirtyMasks dirty =
      mcla::native_gfx::Active() ? mcla::native_gfx::ReadConstantDirtyMasks(base, dev)
                                 : mcla::native_gfx::ConstantDirtyMasks{};
  __imp__D3DDevice_DrawIndexedVertices(ctx, base);
  if (mcla::gfx_probe::Enabled()) {
    mcla::gfx_probe::RecordDraw(base, dev, prim, count, /*indexed=*/1, /*is_up=*/false);
  }
  if (mcla::native_gfx::Active()) {
    // Read-only, self-throttled, stops after a handful of lines. Proves the
    // fence offsets before the resource handover is built on them.
    mcla::native_gfx::ProbeFenceState(base, dev);
    mcla::native_gfx::NoteGuestDraw(0);
    mcla::native_gfx::TelemetryRecordDraw(base, dev, prim, count, /*indexed=*/true);
    mcla::native_gfx::TelemetryRecordGeometry(base, dev, prim, count, start_index,
                                              int32_t(base_vertex), /*indexed=*/true, dirty.vs,
                                              dirty.ps);
    mcla::native_gfx::TryFirstDraw(base, dev, prim, count, start_index, int32_t(base_vertex),
                                   /*indexed=*/true);
  }
}

// D3DDevice_DrawVertices(dev, primType, startVertex, vertexCount)
extern "C" REX_FUNC(D3DDevice_DrawVertices) {
  mcla::native_gfx::nocp::NoteHook("D3DDevice_DrawVertices");
  const uint32_t dev = ctx.r3.u32;
  const uint32_t prim = ctx.r4.u32;
  const uint32_t count = ctx.r6.u32;
  const uint32_t start_vertex = ctx.r5.u32;
  const mcla::native_gfx::ConstantDirtyMasks dirty =
      mcla::native_gfx::Active() ? mcla::native_gfx::ReadConstantDirtyMasks(base, dev)
                                 : mcla::native_gfx::ConstantDirtyMasks{};
  __imp__D3DDevice_DrawVertices(ctx, base);
  if (mcla::gfx_probe::Enabled()) {
    mcla::gfx_probe::RecordDraw(base, dev, prim, count, /*indexed=*/0, /*is_up=*/false);
  }
  if (mcla::native_gfx::Active()) {
    mcla::native_gfx::NoteGuestDraw(1);
    mcla::native_gfx::TelemetryRecordDraw(base, dev, prim, count, /*indexed=*/false);
    mcla::native_gfx::TelemetryRecordGeometry(base, dev, prim, count, start_vertex,
                                              /*base_vertex=*/0, /*indexed=*/false, dirty.vs,
                                              dirty.ps);
    // Non-indexed draws reach the renderer too. Leaving this out was not a
    // filter but an omission: CaptureDraw's `!indexed` rejection never fired
    // because these draws were never offered to it in the first place.
    mcla::native_gfx::TryFirstDraw(base, dev, prim, count, start_vertex, /*base_vertex=*/0,
                                   /*indexed=*/false);
  }
}

// D3DDevice_BeginVertices(dev, primType, vertexCount, stride) -> void* buffer
//
// Allocates a region of the command buffer and returns its guest address in
// r3. The vertices do NOT exist yet: the caller fills the region afterwards and
// then calls EndVertices. So this hook only records the shape of the pending
// draw; the data is read at EndVertices.
extern "C" REX_FUNC(D3DDevice_BeginVertices) {
  mcla::native_gfx::nocp::NoteHook("D3DDevice_BeginVertices");
  const uint32_t dev = ctx.r3.u32;
  const uint32_t prim = ctx.r4.u32;
  const uint32_t count = ctx.r5.u32;
  const uint32_t stride = ctx.r6.u32;
  __imp__D3DDevice_BeginVertices(ctx, base);
  const uint32_t buffer = ctx.r3.u32;  // return value: where the caller writes
  if (mcla::gfx_probe::Enabled()) {
    mcla::gfx_probe::RecordDraw(base, dev, prim, count, /*indexed=*/0, /*is_up=*/true);
  }
  if (mcla::native_gfx::Active()) {
    mcla::native_gfx::NoteGuestDraw(2);
    mcla::native_gfx::NoteBeginVertices(dev, prim, count, stride, buffer);
  }
}

// D3DDevice_EndVertices(dev). The vertices written since BeginVertices are now
// complete, so this is where an inline-geometry draw can actually be issued.
extern "C" REX_FUNC(D3DDevice_EndVertices) {
  mcla::native_gfx::nocp::NoteHook("D3DDevice_EndVertices");
  const uint32_t dev = ctx.r3.u32;
  __imp__D3DDevice_EndVertices(ctx, base);
  if (mcla::native_gfx::Active()) {
    mcla::native_gfx::NoteEndVertices(base, dev);
  }
}

// --- tiling ----------------------------------------------------------------

// rage::grcDevice::BeginTiledRendering(rt, ds, clearParams, fmtSel, flags, overlap)
extern "C" REX_FUNC(grcDevice_BeginTiledRendering) {
  mcla::native_gfx::nocp::NoteHook("grcDevice_BeginTiledRendering");
  const uint32_t fmt_sel = ctx.r6.u32;
  if (mcla::gfx_probe::Enabled()) {
    mcla::gfx_probe::OpenBracket(/*caller_marker=*/0xA470u, fmt_sel);
  }
  __imp__grcDevice_BeginTiledRendering(ctx, base);
}

// D3DDevice_BeginTiling(dev, flags, tileCount, rects, clearColor, clearZ)
extern "C" REX_FUNC(D3DDevice_BeginTiling) {
  mcla::native_gfx::nocp::NoteHook("D3DDevice_BeginTiling");
  const uint32_t tiles = ctx.r5.u32;
  if (mcla::gfx_probe::Enabled()) {
    mcla::gfx_probe::SetBracketTiles(tiles);
  }
  __imp__D3DDevice_BeginTiling(ctx, base);
}

// rage::grcDevice::EndTiledRendering — per-tile predication loop, then
// D3DDevice_EndTiling, then tiling off (dword_827D42A4 = -1).
extern "C" REX_FUNC(grcDevice_EndTiledRendering) {
  mcla::native_gfx::nocp::NoteHook("grcDevice_EndTiledRendering");
  __imp__grcDevice_EndTiledRendering(ctx, base);
  if (mcla::gfx_probe::Enabled()) {
    mcla::gfx_probe::CloseBracket();
  }
}

// D3DDevice_EndTiling — the resolve of the tiled surfaces into the destination.
//
// This is the SECOND resolve path, and until now only the probe watched it: the
// native render-target bridge never saw a tiled resolve at all, so anything the
// guest produced this way was never registered as GPU-produced. A later fetch
// of such an address therefore fell through to decoding guest memory. Measured
// consequence: the composite's screen-sized k_8_8_8_8 input at 0x06ACD000 read
// 1280x720 of zeros (nothing writes it -- the emulated side suppresses the
// producing pass at pitch 1280, and we registered nothing), which is garbage in
// the lighting term.
//
// The destination encoding is the same as D3DDevice_Resolve's: r6 is a D3D
// texture object carrying the fetch constant at +28, which is exactly what
// NotifyResolve already decodes (including the RB_COPY_DEST_BASE page fixup).
// source_rect/dest_point are absent here -- a tiled resolve covers the whole
// surface -- and NotifyResolve reads 0 as "full source extent at origin".
extern "C" REX_FUNC(D3DDevice_EndTiling) {
  mcla::native_gfx::nocp::NoteHook("D3DDevice_EndTiling");
  const uint32_t dev = ctx.r3.u32;
  const uint32_t flags = ctx.r4.u32;
  const uint32_t dest_texture = ctx.r6.u32;
  if (mcla::gfx_probe::Enabled()) {
    mcla::gfx_probe::NoteEndTiling(flags, ctx.r5.u32, dest_texture);
  }
  __imp__D3DDevice_EndTiling(ctx, base);
  // Notified AFTER the original, so the source surface is complete. The address
  // guard keeps a non-pointer argument from faulting the game's own draw thread:
  // NotifyResolve dereferences the destination without a readability check.
  if (dest_texture >= 0x1000u && dest_texture < 0x40000000u && mcla::native_gfx::Active()) {
    mcla::native_gfx::NotifyResolve(base, dev, flags, dest_texture, /*source_rect=*/0,
                                    /*dest_point=*/0);
  }
}

// --- frame boundary --------------------------------------------------------

// rage::grcDevice::EndFrame.
extern "C" REX_FUNC(grcDevice_EndFrame) {
  mcla::native_gfx::nocp::NoteHook("grcDevice_EndFrame");
  mcla::native_gfx::nocp::NoteFrameEnd();
  const bool probe_was_enabled = mcla::gfx_probe::Enabled();
  __imp__grcDevice_EndFrame(ctx, base);
  if (probe_was_enabled) {
    mcla::gfx_probe::OnFrameEnd();
    if (!mcla::gfx_probe::Enabled()) {
      mcla::gfx_probe::WriteReport();
    }
  }
  if (mcla::native_gfx::Active()) {
    mcla::native_gfx::TelemetryOnFrameEnd();
    // A frame boundary is the only point from which the WHOLE frame can be
    // observed. Starting a capture at the first main-scene draw misses every
    // pass that runs earlier -- notably the shadow map, whose 640x640 cascades
    // are what fill the atlas every material shader samples.
    mcla::native_gfx::NotifyFrameBoundary();
  }
}

// --- swap ------------------------------------------------------------------

// D3DDevice_Swap.
//
// CONTINUOUS MODE PASSES THROUGH, ALWAYS. The packet this call writes is what
// makes the command processor end its frame, and ending the frame is the only
// thing that recycles its per-frame pools: EndSubmission(is_swap) calls
// EndFrame on the texture cache and primitive processor and ClearCache on the
// view, sampler, constant-buffer, render-target and shared-memory pools.
// Swallowing the call kept that frame open forever -- host private memory grew
// about a gigabyte per thousand frames until allocation failed ("no free
// bindless view descriptors", "Failed to create a D3D12 upload buffer") and
// the device was removed a couple of minutes into gameplay.
//
// Which leaves the command processor presenting its own image over the native
// one. Taking presentation away from it needed an edit to the SDK, and that
// edit was reverted: a runtime that requires the command processor to be
// changed is not a native runtime, it is a tuned emulator. The replacement is
// to not have a command processor at all -- see src/native_gfx/nocp/.
//
// The smoke-test stage (mcla_native_gfx_present without continuous) still
// suppresses: it presents a triangle and has no command processor frame to
// care about.
extern "C" REX_FUNC(D3DDevice_Swap) {
  mcla::native_gfx::nocp::NoteHook("D3DDevice_Swap");
  mcla::native_gfx::nocp::NoteSwapCall();
  if (mcla::native_gfx::PresentTakeover()) {
    if (mcla::native_gfx::PresentFrame()) {
      ctx.r3.u64 = 0;  // fence value the caller would have received
      return;
    }
    // Native present failed mid-run: fall through to the normal path.
  }
  // Not taken over: the Xenia command processor owns the frame, including the
  // guest-output refresh. Nothing native happens here -- Presenter's guest
  // output is single-producer, so a second refresh from this thread would race
  // the one the command processor does inside IssueSwap.
  __imp__D3DDevice_Swap(ctx, base);
}

// --- resolve ---------------------------------------------------------------

// D3DDevice_Resolve(dev, flags, pSourceRect, pDestTexture, pDestPoint,
//                   DestLevel, DestSliceOrFace, pClearColor, ClearZ, ...)
//
// flags & 7 == 4 selects the depth buffer as the source; 0..3 select colour
// render targets. pDestTexture is a D3D texture object whose dwords at
// +28/+32/+36 are the fetch constant a later draw will sample, which is why
// the destination can be decoded with the ordinary texture path.
//
// The resolve is notified AFTER the original runs, so the guest has already
// programmed RB_COPY_DEST_BASE and the source surface is complete.
extern "C" REX_FUNC(D3DDevice_Resolve) {
  mcla::native_gfx::nocp::NoteHook("D3DDevice_Resolve");
  const uint32_t dev = ctx.r3.u32;
  const uint32_t flags = ctx.r4.u32;
  const uint32_t source_rect = ctx.r5.u32;   // a3: x0, y0, x1, y1
  const uint32_t dest_texture = ctx.r6.u32;  // a4
  const uint32_t dest_point = ctx.r7.u32;    // a5: x, y
  __imp__D3DDevice_Resolve(ctx, base);
  if (mcla::native_gfx::Active()) {
    mcla::native_gfx::NotifyResolve(base, dev, flags, dest_texture, source_rect, dest_point);
  }
}

#endif // REXGLUE_HAS_XEO3_TARGET
