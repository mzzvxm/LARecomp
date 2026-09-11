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
#include <cstdio>

#include "guest/guest_constants.h"
#include "guest/guest_resources.h"
#include "guest/guest_fence.h"
#include "guest/texture_ownership.h"
#include "guest/vblank_probe.h"
#include "nocp/nocp_app.h"
#include "guest/texture_registry.h"
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

// D3DDevice_Clear(dev, Count, pRects, Flags, Color, Z, Stencil, ...) --
// sub_824195E8, the single funnel every guest clear reaches. grcDevice::Clear
// (sub_82178370) is the only caller and it builds Flags itself: 0xF for the
// four colour targets, 0x10 depth, 0x20 stencil, with Colour a D3DCOLOR in r7.
// Z rides in fp1, so its GPR slot (r8) is reserved and carries nothing.
//
// Observed BEFORE the original: in no-CP mode the original writes a clear into
// a command stream nothing consumes, so there is no ordering to preserve here.
extern "C" REX_FUNC(rex_sub_824195E8) {
  mcla::native_gfx::nocp::NoteHook("rex_sub_824195E8");
  if (REXCVAR_GET(mcla_native_gfx)) {
    mcla::native_gfx::NoteGuestClear(ctx.r6.u32, ctx.r7.u32, static_cast<float>(ctx.f1.f64),
                                     ctx.r9.u32);
  }
  __imp__rex_sub_824195E8(ctx, base);
}

// --- texture identity ------------------------------------------------------

// grcTextureXenon::Init(this, const char* name, image, flags)
//
// The funnel both texture creation roads pass through -- the placed one, which
// binds a header over memory a streamed resource already owns, and the
// allocated one, which goes to D3DDevice_CreateTexture. Observed AFTER the
// original: the D3DTexture at this+28 does not exist until it returns.
//
// This is the site resource ownership will take over. For now it only reads.
extern "C" REX_FUNC(grcTextureXenon_Init) {
  mcla::native_gfx::nocp::NoteHook("grcTextureXenon_Init");
  const uint32_t grc_texture = ctx.r3.u32;
  const uint32_t name = ctx.r4.u32;
  __imp__grcTextureXenon_Init(ctx, base);
  // Ownership first: it can move the D3DTexture into the runtime's own
  // guest-visible heap, and the registry has to record the address the game
  // will actually be using from here on.
  mcla::native_gfx::AdoptInitTexture(base, grc_texture);
  mcla::native_gfx::NoteTextureCreated(base, grc_texture, name);
}

// grcTextureXenon::grcTextureXenon(datResource&) -- sub_82184458.
//
// The road every STREAMED texture takes. Nothing is created here: the object
// arrives inside the resource block and the constructor only re-points it at
// where the block actually landed. It writes the vtable, adds the resource
// base to grc+24 (name) and grc+28 (the D3DTexture), then patches the two page
// fields inside the fetch constant itself -- D3DTexture+32 (base) and +48
// (mip) -- through the same fixup table that reports "Invalid fixup, address
// is neither virtual nor physical" when a page is neither.
//
// Observed AFTER the original, because before it the pointers are still file
// offsets. This is the hook that was missing while the registry sat frozen at
// the 111 textures Init builds during boot.
extern "C" REX_FUNC(grcTextureXenon_ResourceCtor) {
  mcla::native_gfx::nocp::NoteHook("grcTextureXenon_ResourceCtor");
  const uint32_t grc_texture = ctx.r3.u32;
  __imp__grcTextureXenon_ResourceCtor(ctx, base);
  // No Active() gate here, deliberately. Active() lazily initializes the whole
  // runtime and LATCHES failure, and a streamed texture can be deserialized
  // long before the graphics system is up -- one early call would disable
  // native_gfx for the rest of the session. The registry gates itself on its
  // own cvar, exactly as the Init hook above does.
  mcla::native_gfx::NoteTextureFromResource(base, grc_texture);
}

// grcTextureFactoryXenon::DestroyTexture(D3DTexture** ppTexture) -- sub_82177CB0.
//
// Takes the ADDRESS of the field holding the pointer, not the pointer: it
// reads *ppTexture, frees it (D3DResource_Release when Common & 0x100000, the
// raw allocator otherwise) and writes 0 back. Every teardown route ends here,
// which makes it the one place that can guarantee the guest allocator is only
// ever handed blocks it produced.
//
// Observed BEFORE the original: afterwards the slot is zero and the block is
// gone.
extern "C" REX_FUNC(grcTextureFactoryXenon_DestroyTexture) {
  mcla::native_gfx::nocp::NoteHook("grcTextureFactoryXenon_DestroyTexture");
  mcla::native_gfx::ReleaseOwnedTextureSlot(base, ctx.r3.u32);
  __imp__grcTextureFactoryXenon_DestroyTexture(ctx, base);
}

// grcTextureXenon::~grcTextureXenon(this, deleting). Observed BEFORE the
// original, which is the last moment this+28 still points at the D3DTexture.
extern "C" REX_FUNC(grcTextureXenon_dtor) {
  mcla::native_gfx::nocp::NoteHook("grcTextureXenon_dtor");
  mcla::native_gfx::NoteTextureDestroyed(base, ctx.r3.u32);
  // Hand the header back BEFORE the original runs: DestroyTexture frees the
  // block through the guest allocator that produced it, and that allocator
  // knows nothing about the runtime's heap.
  mcla::native_gfx::ReleaseOwnedTexture(base, ctx.r3.u32);
  __imp__grcTextureXenon_dtor(ctx, base);
}

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

// --- texture memory --------------------------------------------------------

// These three run on the game's own threads at whatever rate it locks
// textures, so they are gated on the cvar rather than on Active(): Active()
// lazily initializes the whole runtime and LATCHES failure, and a texture lock
// can happen long before the graphics system exists.

// D3DDevice_CreateTexture(w, h, depth, levels, usage, format, pool, type)
// -- sub_82410C50. THREE guest allocations: the 52-byte header, the base pixel
// chain and, when there is one, the mip chain; the last two are written into
// the fetch constant as pages at +32 and +48. Returns the header in r3, or 0,
// which the caller turns into E_OUTOFMEMORY.
//
// Observed AFTER the original: before it there is no object to look at.
extern "C" REX_FUNC(D3DDevice_CreateTexture) {
  mcla::native_gfx::nocp::NoteHook("D3DDevice_CreateTexture");
  __imp__D3DDevice_CreateTexture(ctx, base);
  if (REXCVAR_GET(mcla_native_gfx)) {
    mcla::native_gfx::NoteD3DTextureCreated(base, ctx.r3.u32);
  }
}

// D3DResource_Lock -- sub_82421CA0, the funnel on the way in. Same three entry
// points as the unlock side and nothing else: D3DTexture_LockRectBody
// (sub_82410440), D3DVertexBuffer_Lock (sub_82422320) and D3DIndexBuffer_Lock
// (sub_82422430). r3 is the resource.
//
// Observation only for now. This is where the guest blocks on the resource
// fence, through sub_82411E98, which is the machinery an ownership step has to
// take over before it can serve a lock itself.
extern "C" REX_FUNC(D3DResource_Lock) {
  mcla::native_gfx::nocp::NoteHook("D3DResource_Lock");
  if (REXCVAR_GET(mcla_native_gfx)) {
    mcla::native_gfx::NoteResourceLocked(base, ctx.r3.u32);
    // TEMP DIAG (LOCKADDR): which guest addresses the CPU actually locks. The
    // native runtime keeps a resolve on the GPU and bridges it by address; it
    // never writes the pixels back into guest memory, the way the emulated path
    // does through SharedMemory::RangeWrittenByGpu. That difference only bites
    // if the guest READS the resolved surface, so this says whether it does.
    {
      const uint32_t res = ctx.r3.u32;
      if (res >= 0x1000u) {
        uint32_t raw;
        std::memcpy(&raw, rex::memory::GuestPtr(const_cast<uint8_t*>(base), res + 0x20), 4);
        const uint32_t addr = __builtin_bswap32(raw) & ~0xFFFu;
        // Only the surfaces the GPU produced. Logging every lock filled the
        // budget with ordinary texture streaming in the 0xE5..0xE7 window and
        // never reached the question, which is whether the CPU touches a
        // RESOLVE DESTINATION -- the UI panel at 0x06C5D000 and the minimap
        // art around 0x0FA20000, in the low half of the 0xE0000000 window.
        const uint32_t low = addr & 0x1FFFFFFFu;
        const bool interesting = (low >= 0x02000000u && low < 0x08000000u) ||
                                 (low >= 0x0F800000u && low < 0x10000000u);
        static uint32_t seen[128];
        static uint32_t seen_n = 0;
        bool fresh = addr != 0 && interesting;
        for (uint32_t i = 0; i < seen_n && fresh; ++i) {
          if (seen[i] == addr) fresh = false;
        }
        if (fresh && seen_n < 128u) {
          seen[seen_n++] = addr;
          if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
            std::fprintf(f, "LOCKADDR res=0x%08X data=0x%08X\n", res, addr);
            std::fflush(f);
            std::fclose(f);
          }
        }
      }
    }
  }
  __imp__D3DResource_Lock(ctx, base);
}

// D3DResource_Unlock(resource, base_address, mip_address) -- sub_82421F38.
//
// THE funnel. Xref says exactly three thunks reach it and nothing else in the
// binary unlocks anything:
//   sub_8240F220  texture       r4 = tex+0x20 & ~0xFFF, r5 = tex+0x30 & ~0xFFF
//   sub_82422370  vertex buffer r4 = vb+0x18 & ~3,      r5 = 0
//   0x82422478    index buffer  (the chunk right after D3DIndexBuffer_Lock)
// All three tail-call, so r3/r4/r5 arrive here unchanged.
//
// Hooked here rather than at the texture thunk for two reasons. It sees every
// resource type, which is the open question -- the vertex and index buffer
// locks are virtual methods (grcVertexBufferD3D vtable off_820106BC, slots 1
// and 2), so no static analysis reaches their callers. And hooking both the
// thunk and the funnel would count textures twice.
//
// Observed BEFORE the original, and it has to be: the original resets the two
// flush words and decrements the lock count this reads.
extern "C" REX_FUNC(rex_sub_82421F38) {
  mcla::native_gfx::nocp::NoteHook("rex_sub_82421F38");
  if (REXCVAR_GET(mcla_native_gfx)) {
    mcla::native_gfx::NoteResourceUnlocked(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  }
  __imp__rex_sub_82421F38(ctx, base);
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
                                    /*dest_point=*/0, /*clear_color_ptr=*/0);
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
  // TEMP DIAG (remove after): the minimap circular mask lives at 0x074AA000 and
  // reads back as 100% 0xFF on the native path, which makes `1 - mask` zero and
  // the REV_SUBTRACT punch a no-op -- the square minimap. The emulated path
  // clips it correctly with the SAME guest code, so something writes that
  // memory there and not here. This probe sits OUTSIDE the Active() gate on
  // purpose: grcDevice_EndFrame runs on both paths, so the same build can
  // answer "is it white on emulated too" with only a cvar flip.
  //
  // It is not a resolve: NoteFrameCaptureResolve logs every destination and
  // 0x074AA000 never appears among them.
  {
    static uint32_t tick = 0;
    if ((tick++ % 600u) == 0u) {
      // Two candidates, because the first conclusion was drawn from the wrong
      // one. 0x074AA000 (32x32) reads 100% 0xFF on BOTH paths, yet the emulated
      // path clips the circle correctly -- so that texture is a white
      // placeholder, not the mask. 0x02D64000 (256x256 DXT1) is the other
      // texture seen in fetch slot 0 of a REV_SUBTRACT draw, and is the size a
      // mask for a 220x220 target would actually be.
      constexpr uint32_t kAddrs[2] = {0x074AA000u, 0x02D64000u};
      constexpr uint32_t kProbe = 4096u;
      for (uint32_t ai = 0; ai < 2; ++ai) {
      const uint32_t kMaskAddr = kAddrs[ai];
      const uint8_t* g = mcla::native_gfx::IsPhysicalRangeReadable(kMaskAddr, kProbe)
                             ? mcla::native_gfx::TranslatePhysicalGuest(kMaskAddr)
                             : nullptr;
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        if (!g) {
          std::fprintf(f, "MASKPROBE 0x%08X nao legivel (native=%d)\n", kMaskAddr,
                       mcla::native_gfx::Active() ? 1 : 0);
        } else {
          uint32_t ones = 0, zeros = 0, other = 0;
          for (uint32_t k = 0; k < kProbe; ++k) {
            if (g[k] == 0xFFu) ++ones; else if (g[k] == 0x00u) ++zeros; else ++other;
          }
          uint32_t head[4];
          for (uint32_t k = 0; k < 4; ++k) std::memcpy(&head[k], g + k * 4, 4);
          std::fprintf(f, "MASKPROBE 0x%08X native=%d | 0xFF=%.1f%% 0x00=%.1f%% outros=%.1f%% | head %08X %08X %08X %08X\n",
                       kMaskAddr, mcla::native_gfx::Active() ? 1 : 0,
                       100.0 * ones / kProbe, 100.0 * zeros / kProbe, 100.0 * other / kProbe,
                       head[0], head[1], head[2], head[3]);
        }
        std::fflush(f);
        std::fclose(f);
      }
      }
    }
  }
  if (mcla::native_gfx::Active()) {
    mcla::native_gfx::TelemetryOnFrameEnd();
    mcla::native_gfx::DumpTextureRegistry();
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
  mcla::native_gfx::NoteSwapHook();
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
  // a8: pClearColor. On the console a resolve can CLEAR the surface it just
  // copied out of, which is how the guest separates one impostor bake from the
  // next: it renders a species, resolves it to that species' atlas, and the
  // resolve wipes the tile for the following one. Ignoring it made every bake
  // accumulate on top of the last -- measured, one 256x256 target took 1 clear
  // and then 29 draws -- so each atlas ended up holding the union of several
  // trees and every distant tree rendered as one solid bush.
  const uint32_t clear_color_ptr = ctx.r10.u32;
  __imp__D3DDevice_Resolve(ctx, base);
  if (mcla::native_gfx::Active()) {
    mcla::native_gfx::NotifyResolve(base, dev, flags, dest_texture, source_rect, dest_point,
                                    clear_color_ptr);
  }
}

#endif // REXGLUE_HAS_XEO3_TARGET
