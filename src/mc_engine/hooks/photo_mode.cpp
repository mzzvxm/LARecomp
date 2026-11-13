// Photo mode: a readback of just the two front buffers around each shot, so the
// picture the game JPEG-encodes on the CPU is the frame that was on screen.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/xmemory.h>
#include <cstdint>

#include "hooks.h"
#include "hooks_internal.h"
#include "../logging.h"

REXCVAR_DEFINE_BOOL(photo_auto_readback, true, "MCLA/PhotoMode",
    "Fix: photo mode previews show an old frame or garbage. The game takes its "
    "picture on the CPU, by locking the front buffer and JPEG-encoding it, so it "
    "needs the GPU resolve copied back to guest memory. This asks for a readback "
    "of just the two front buffers for a few frames around each shot, instead of "
    "the global readback_resolve cvar which stalls every resolve of every frame "
    "(on an SDK without the scoped readback, the global one for about a second "
    "around each shot). Leave ON.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(photo_readback_debug, false, "MCLA/PhotoMode",
    "Diagnostics for photo_auto_readback: logs the photo album state machine's "
    "phase transitions and the front buffer addresses the readback is armed for. "
    "Pair with the GPU-side gpu_log_resolve_readback_misses to see whether the "
    "resolve that fills the front buffer actually lands in the armed range.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// ---------------------------------------------------------------------------
// Photo mode: front buffer readback
// ---------------------------------------------------------------------------
// Photo mode never touches the GPU for its picture. sub_82178B20 locks the front
// buffer texture dword_828393AC[dword_82839374] with LockRect, untiles it with
// XGUntileSurface, endian-swaps it, and hands the raw pixels to libjpeg
// (sub_8263C728, quality 85). Both the discard/save preview and the saved slot
// are decoded back from that one JPEG, so if the locked guest memory is stale
// the preview shows an old frame or garbage.
//
// The front buffer is only ever filled by the end-of-frame EDRAM resolve in
// sub_8217B7B0, which the emulator keeps GPU-side. Instead of turning on the
// global readback_resolve cvar (every resolve, every frame, full GPU drain), arm
// an address-scoped request for just the two front buffers while the photo state
// machine winds up to the capture.
//
// The address-scoped request is the fork's SDK's (IGraphicsSystem::
// RequestResolveReadback). The stock SDK has no such call, and there the shot
// holds the global readback open instead, through the same window the vinyls
// use (readback_window.cpp): every resolve for about a second around the shot,
// which costs frame rate in photo mode and nowhere else.

// Reads the base address out of a guest D3DTexture's GPU fetch constant, the
// same field sub_82410440 feeds to LockRect: dword 1 of the fetch constant
// (texture + 32), whose top 20 bits are the base address in 4 KB pages.
//
// That address lives in the 0xE0000000 physical alias window, not in the
// physical space the resolve reports, so it still has to go through
// GetPhysicalAddress: 0xE7C47000 -> 0x07C48000 (mask to 0x1FFFFFFF plus the
// 0x1000 offset the 0xE0 heap is mapped at). Returns 0 if the pointer isn't a
// plausible object or the address isn't in a physical heap.
static uint32_t GuestTextureBaseAddress(uint32_t texture_ea) {
    if (texture_ea < 0x10000u || texture_ea >= 0xFFFF0000u) return 0;
    auto* rt = rex::Runtime::instance();
    if (!rt) return 0;
    auto* mem = rt->memory();
    if (!mem) return 0;
    const auto* p = mem->TranslateVirtual<const uint8_t*>(texture_ea + 32);
    if (!p) return 0;
    uint32_t fetch_dword =
        (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    uint32_t base_ea = fetch_dword & 0xFFFFF000u;
    if (!base_ea) return 0;
    uint32_t physical = mem->GetPhysicalAddress(base_ea);
    return physical == UINT32_MAX ? 0 : physical;
}

// Asks the SDK to read back [base, base + span) for the next `frames` frames.
// False when it cannot: a template so that, on an SDK without the call, the
// requires-expression is simply false instead of a compile error.
template <typename Gfx>
static bool RequestScopedResolveReadback(Gfx* gfx, uint32_t base, uint32_t span,
                                         uint32_t frames) {
    if constexpr (requires { gfx->RequestResolveReadback(base, span, frames); }) {
        gfx->RequestResolveReadback(base, span, frames);
        return true;
    } else {
        return false;
    }
}

// Arms readback for both entries of the front buffer array. The game alternates
// dword_82839374 on every swap and the capture locks whichever is current, so
// covering only one of them would be a coin flip.
static void ArmPhotoFrontBufferReadback() {
    if (!REXCVAR_GET(photo_auto_readback)) return;
    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* mem = rt->memory();
    auto* gfx = rt->graphics_system();
    if (!mem || !gfx) return;

    const auto* array = mem->TranslateVirtual<const uint8_t*>(0x828393ACu);  // front buffers[2]
    if (!array) return;

    // 1280x720 at 32 bpp tiled is 3768320 bytes (the resolve length the GPU
    // reports); 4 MB covers it without decoding the pitch out of the fetch
    // constant. The slack matters: MCLA resolves each frame as three tiled
    // bands into the same buffer (predicated tiling, sub_8241C308), so the
    // request has to span all three, not just the base one.
    constexpr uint32_t kFrontBufferSpan = 4u * 1024u * 1024u;
    // The grab fires a few frames after the phase we arm on, and the resolve
    // that fills the buffer happened the frame before that.
    constexpr uint32_t kArmedFrames = 8;
    // The global fallback copies a resolve back a frame late, so it gets more
    // than 8 frames of slack; it is re-armed every frame of the approach anyway.
    constexpr int64_t kGlobalHoldNs = 1'000'000'000LL;  // 1s

    uint32_t bases[2] = {0, 0};
    bool scoped = true;
    for (int i = 0; i < 2; ++i) {
        const uint8_t* e = array + i * 4;
        uint32_t texture_ea =
            (uint32_t(e[0]) << 24) | (uint32_t(e[1]) << 16) | (uint32_t(e[2]) << 8) | e[3];
        bases[i] = GuestTextureBaseAddress(texture_ea);
        if (bases[i] &&
            !RequestScopedResolveReadback(gfx, bases[i], kFrontBufferSpan, kArmedFrames)) {
            scoped = false;
        }
    }
    if (!scoped) {
        HoldGlobalReadback(ReadbackClient::kPhoto, kGlobalHoldNs);
    }
    if (REXCVAR_GET(photo_readback_debug)) {
        static uint32_t last_logged[2] = {0, 0};
        if (bases[0] != last_logged[0] || bases[1] != last_logged[1]) {
            last_logged[0] = bases[0];
            last_logged[1] = bases[1];
            MC_INFO("photo: armed front buffer readback ({}), base0={:08X} base1={:08X} span={} KB",
                    scoped ? "scoped" : "global window", bases[0], bases[1],
                    kFrontBufferSpan >> 10);
        }
    }
}

// Entry hook on sub_8263CB78, the photo album vhsm update. r3 is the state
// object (photo album object + 456); its phase lives at +132:
//   3 = capture pipeline running (substate 9 grabs, 12 encodes, 10 finishes)
//   4 = decode the fresh JPEG into the PreviewPicture texture
//   5/6 = fade/settle frames before the grab is triggered
// Arming from 3/5/6 puts the readback window several frames ahead of the
// LockRect in sub_82178B20, which is what the front buffer resolve needs.
void Hook_PhotoModeCapture(PPCRegister& r3) {
    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* mem = rt->memory();
    if (!mem) return;
    uint32_t state_ea = static_cast<uint32_t>(r3.u64);
    if (state_ea < 0x10000u || state_ea >= 0xFFFF0000u) return;
    const auto* p = mem->TranslateVirtual<const uint8_t*>(state_ea + 132);
    if (!p) return;
    uint32_t phase = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    if (REXCVAR_GET(photo_readback_debug)) {
        static uint32_t last_phase = 0xFFFFFFFFu;
        if (phase != last_phase) {
            last_phase = phase;
            const auto* s = mem->TranslateVirtual<const uint8_t*>(state_ea + 136);
            uint32_t substate =
                s ? ((uint32_t(s[0]) << 24) | (uint32_t(s[1]) << 16) | (uint32_t(s[2]) << 8) | s[3])
                  : 0;
            MC_INFO("photo: vhsm phase={} substate={} (state={:08X})", phase, substate, state_ea);
        }
    }
    // Arm across the whole snapshot approach, not just the phase that grabs.
    // The grab in phase 3 substate 9 runs inside this very call, so a request
    // armed there is already too late for it - only the resolves of earlier
    // frames can still fill the buffer it locks.
    if (phase >= 2 && phase <= 6) {
        ArmPhotoFrontBufferReadback();
    }
}
#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

void Hook_PhotoModeCapture(PPCRegister& r3) {}
#endif // REXGLUE_HAS_XEO3_TARGET
