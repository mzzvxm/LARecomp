// Display gamma: the console's TV ramp, or an sRGB one for a PC monitor.
//
// The game writes gamma-encoded pixels into its front buffer and leaves the display transfer to
// the gamma ramp the device holds (dev+15004: 256 entries per channel, 10 bits, big-endian). The
// ramp is set once, when the device is created: sub_82426028 gives the identity ramp
// (sub_82427008) to D3DDevice_SetGammaRamp (sub_82416000), which converts it for the display
// through a table that sub_82426B68 builds from VdGetCurrentDisplayGamma. Display type 1 (sRGB)
// makes that table the identity. Type 2 (TV, BT.709) makes it BT.709 OETF(sRGB EOTF(x)). The SDK
// answers 2 for every title (kDisplayGammaType in xboxkrnl_video.cpp), as a console on an HDTV
// would, so the Plume present (gpu/present.cpp) and the SDK's presenter both show the TV ramp.
// A PC monitor expects sRGB, and the TV ramp makes its shadows darker: the 8-bit codes 64, 128
// and 192 leave the ramp as 193, 462 and 741 of 1023.
//
// display_gamma picks the type the helper sees: tv leaves the SDK's answer alone (2), pc writes
// 1. The game never sets its ramp again, so a change during play gives the device the identity
// ramp again at the start of the next Swap, through the same two calls. That Swap then sends the
// new ramp to the display with the frame, the way a game changing its ramp would. The renderer
// needs nothing: gpu/guest_capture.cpp copies dev+15004 at every Swap and gpu/present.cpp uploads
// the table whenever its bytes change.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include "display_gamma.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <atomic>
#include <cstdint>
#include <string>

#include "larecomp_log.h"

REXCVAR_DEFINE_STRING(display_gamma, "tv", "MCLA/Video",
    "Display gamma ramp. tv: the console's for an HDTV (BT.709), as the game ships. pc: sRGB, "
    "for PC monitors, with brighter shadows. Live.")
    .allowed({"tv", "pc"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Generated guest code: the identity ramps (a 256-entry table and a PWL ramp), then
// D3DDevice_SetGammaRamp and D3DDevice_SetPWLGamma.
REX_EXTERN(rex_sub_82427008);
REX_EXTERN(rex_sub_82427058);
REX_EXTERN(rex_sub_82416000);
REX_EXTERN(rex_sub_824160C8);

namespace {

// VdGetCurrentDisplayGamma's display types.
constexpr uint32_t kGammaTypeSrgb = 1;

enum Mode : int { kModeUnknown = -1, kModeTv = 0, kModePc = 1 };

// The device's swap control dword (low 6 bits: the front buffer format, as the device creation
// stores it; the 2_10_10_10 formats 7 and 54 use the PWL ramp, others the table; MCLA's is
// 0x28280106, k_8_8_8_8) and its converted ramp: red, green, blue, 256 entries each, 16 bits with
// the 10-bit value in the top bits.
constexpr uint32_t kDevSwapControl = 13608;
constexpr uint32_t kDevGammaRamp = 15004;
constexpr uint32_t kRampBytes = 1536;

// The mode the helper last built its table for (set by the hook below), and the mode last asked
// of the device, so that a request that never reached the helper is not repeated every frame.
std::atomic<int> g_applied{kModeUnknown};
std::atomic<int> g_requested{kModeUnknown};
std::atomic<bool> g_logged{false};

uint32_t ReadU32(const uint8_t* base, uint32_t ea) {
    const uint8_t* p = base + ea;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint16_t ReadU16(const uint8_t* base, uint32_t ea) {
    const uint8_t* p = base + ea;
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

void WriteU32(uint8_t* base, uint32_t ea, uint32_t value) {
    uint8_t* p = base + ea;
    p[0] = uint8_t(value >> 24);
    p[1] = uint8_t(value >> 16);
    p[2] = uint8_t(value >> 8);
    p[3] = uint8_t(value);
}

// The pause menu sets the option on another thread than the one that renders, so it is read
// through the registry, which hands out a copy.
int WantedMode() {
    return rex::cvar::GetFlagByName("display_gamma") == "pc" ? kModePc : kModeTv;
}

const char* ModeName(int mode) {
    return mode == kModePc ? "PC monitor (sRGB)" : "TV (BT.709)";
}

bool UsesPwlRamp(const uint8_t* base, uint32_t device) {
    const uint32_t format = ReadU32(base, device + kDevSwapControl) & 0x3F;
    return format == 7 || format == 54;
}

// The ramp the device now holds, at a few points of the red channel.
void LogRamp(const uint8_t* base, uint32_t device, int mode) {
    if (UsesPwlRamp(base, device)) {
        LARECOMP_APP_INFO("[DisplayGamma] {} (PWL ramp)", ModeName(mode));
        return;
    }
    auto at = [&](uint32_t index) { return ReadU16(base, device + kDevGammaRamp + 2 * index) >> 6; };
    LARECOMP_APP_INFO("[DisplayGamma] {}; ramp at 0, 64, 128, 192, 255: {}, {}, {}, {}, {} of 1023",
                      ModeName(mode), at(0), at(64), at(128), at(192), at(255));
}

// Sets the game's ramp again, as the device creation did: the identity, through SetGammaRamp (or
// SetPWLGamma), which converts it for the display type the helper now gets. The ramp is built on
// the guest stack below the caller's frame.
void SetRampAgain(PPCContext& ctx, uint8_t* base, uint32_t device) {
    const bool pwl = UsesPwlRamp(base, device);
    rex::CallFrame frame(ctx);
    PPCContext& call = frame;
    const uint32_t sp = (ctx.r1.u32 - kRampBytes - 256) & ~UINT32_C(15);
    WriteU32(base, sp, ctx.r1.u32);  // back chain
    const uint32_t ramp = sp + 128;
    call.r1.u64 = sp;
    call.r3.u64 = ramp;
    call.r4.u64 = 0;
    if (pwl) {
        rex_sub_82427058(call, base);
    } else {
        rex_sub_82427008(call, base);
    }
    call.r1.u64 = sp;
    call.r3.u64 = device;
    call.r4.u64 = 0;
    call.r5.u64 = ramp;
    if (pwl) {
        rex_sub_824160C8(call, base);
    } else {
        rex_sub_82416000(call, base);
    }
}

}  // namespace

// 0x82426B98 in sub_82426B68: the instruction after its `bl VdGetCurrentDisplayGamma`, which
// wrote the display type at r1+84 (and the power at r1+80). tv keeps the SDK's answer.
void Hook_DisplayGammaType(PPCRegister& r1) {
    const int mode = WantedMode();
    if (mode == kModePc) {
        auto* runtime = rex::Runtime::instance();
        uint8_t* base = runtime ? runtime->virtual_membase() : nullptr;
        if (!base) {
            return;
        }
        WriteU32(base, r1.u32 + 84, kGammaTypeSrgb);
    }
    g_applied.store(mode, std::memory_order_relaxed);
}

void DisplayGammaOnSwap(PPCContext& ctx, uint8_t* base) {
    const uint32_t device = ctx.r3.u32;
    if (!device) {
        return;
    }
    const int wanted = WantedMode();
    const int applied = g_applied.load(std::memory_order_relaxed);
    if (applied == wanted) {
        g_requested.store(kModeUnknown, std::memory_order_relaxed);
        if (!g_logged.exchange(true, std::memory_order_relaxed)) {
            LogRamp(base, device, applied);  // the ramp the device was created with
        }
    } else if (g_requested.exchange(wanted, std::memory_order_relaxed) != wanted) {
        SetRampAgain(ctx, base, device);
        LogRamp(base, device, wanted);
        g_logged.store(true, std::memory_order_relaxed);
    }
}
#endif  // REXGLUE_HAS_XEO3_TARGET
