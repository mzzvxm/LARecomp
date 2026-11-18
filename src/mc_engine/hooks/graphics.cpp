// Display and render switches: aspect ratio, single-tile predicated tiling and
// the EDRAM bound it needs, motion blur, MSAA, depth of field, foliage imposter
// shadows, and the traffic and city LOD scales.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include "settings/larecomp_settings.h"
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/guest_memory.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "hooks.h"
#include "hooks_internal.h"
#include "larecomp_log.h"
#include "../ultrawide.h"
#if defined(LARECOMP_HAS_GPU)
#include "gpu/gpu.h"
#include "gpu/gpu_class.h"
#endif

// The aspect ratio constant the game's cameras load (16:9, 0x3FE38E39) takes the aspect ratio in
// force (mc_engine/ultrawide.h). Called at startup and whenever it changes.
void ApplyAspectRatioPatch() {
    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* mem = rt->memory();
    if (!mem) return;

    constexpr uint32_t addr = 0x8201E7EC;
    if (auto* heap = mem->LookupHeap(addr)) {
        heap->Protect(addr, sizeof(uint32_t),
                      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
    }

    auto* patch_ptr = mem->TranslateVirtual<uint8_t*>(addr);
    if (!patch_ptr) return;

    const float aspect = float(mc::ultrawide::DisplayAspect());
    uint32_t val;
    std::memcpy(&val, &aspect, sizeof(val));

    // Write the 4 bytes in Big-Endian at the correct address
    patch_ptr[0] = (val >> 24) & 0xFF; // MSB
    patch_ptr[1] = (val >> 16) & 0xFF;
    patch_ptr[2] = (val >> 8)  & 0xFF;
    patch_ptr[3] = val         & 0xFF; // LSB
}

// The four camera sites below load the same constant straight into a register; they take the aspect
// ratio in force instead (false: the game's own 16:9 stays).
static bool GetAspectRatio(double& out_val) {
    if (!mc::ultrawide::Active()) {
        return false;
    }
    out_val = double(float(mc::ultrawide::DisplayAspect()));
    return true;
}


// The ortho viewport sub_82233E18 makes for the rear-view mirror: its width is f30 (the height, 720 as a
// float) times the aspect ratio (`fmuls f31,f30,f0` at 0x82233EB8), so a wide picture makes it wider and the
// squeeze of the picture squeezes the mirror with it (Hor+). In a TALL picture the width must stay the game's
// 16:9 one and the HEIGHT grow, which is the same thing turned round: f30 (read after this, as the ortho's
// height) is given 16:9 over the aspect ratio times its value and f0 the aspect ratio, so that f30 * f0 is the
// game's width. A no-op at 16:9 and in a wide picture.
// CHECK: that this ortho viewport is the mirror's (its picture in a wide one), and that the mirror keeps its
// shape and its place in a 1920x1200 and a 1440x1080 picture (ultrawide.h item 9).
bool Patch_AspectRatio_82233EB4(PPCRegister& f0, PPCRegister& f30) {
    if (!GetAspectRatio(f0.f64)) {
        return false;
    }
    if (mc::ultrawide::Tall()) {
        f30.f64 = double(float(f30.f64 * mc::ultrawide::VertPlusScale()));
    }
    return true;
}
bool Patch_AspectRatio_82214BB8(PPCRegister& f10) {
    return GetAspectRatio(f10.f64);
}
// sub_822E5E10 (the camera builder: f30 = the vertical field of view in degrees, f12 the aspect ratio before
// its blend): the one place the game's cameras get their perspective. A wide picture keeps the vertical field
// of view (Hor+: the aspect ratio alone widens the view). A TALL one keeps the game's HORIZONTAL field of view
// and grows the vertical one (Vert+, mc::frame::VertPlusFov): f30 is rewritten before it goes to
// grcViewport::Perspective (`fmr f1,f30` at 0x822E5E94), so the projection, the frustum the game culls with and
// everything read from the viewport's stored angle (+696) follow. The blend applies to the aspect ratio only,
// so the angle does not depend on it. Patch_FOVScale (the player's FOV setting) has run before: the setting
// scales the 16:9 angle and Vert+ keeps that horizontal one.
// CHECK: that every camera the game's own 16:9 aspect constant reaches is built here (the hooks of
// 0x82233EB4, 0x82214BB8 and 0x8223E5E0 load it for an ortho viewport and ratios, not a perspective), and that
// the explicit-aspect path (a negative f4) is only used by cameras that are not the picture's.
bool Patch_AspectRatio_822E5E68(PPCRegister& f12, PPCRegister& f30) {
    if (!GetAspectRatio(f12.f64)) {
        return false;
    }
    if (mc::ultrawide::Tall()) {
        f30.f64 = double(float(mc::ultrawide::CameraFov(f30.f64)));
    }
    return true;
}
bool Patch_AspectRatio_8223E5E0(PPCRegister& f13) {
    return GetAspectRatio(f13.f64);
}

// Single-tile predicated tiling — hook at 0x8217A700 in
// grcDevice::BeginTiledRendering (sub_8217A470), the convergence point right
// after the per-orientation tile size math and before the tile rect loop.
// r7 = tile width, r8 = tile height (both feed the 160/32-aligned dimensions,
// the tile rect array and the PredictedTile RT allocation); r28/r25 = screen
// width/height. Forcing tile size = screen size makes the tile count land on
// 1, so the scene is submitted once instead of once per tile. Tile count is
// also stored at 0x827D42A4 before this point — overwrite it to 1 for the
// resolve/end path that reads the global. Needs the SDK's enlarged virtual
// EDRAM (720p 2xMSAA color+depth = 2880 tiles > the real 2048).
void Patch_SingleTile(PPCRegister& r7, PPCRegister& r8, PPCRegister& r17, PPCRegister& r25, PPCRegister& r28) {
    if (!larecomp::settings::single_tile.Get()) return;

    // BadassBaboon: ONLY apply single_tile forcing to the main screen scene (a1 == nullptr / 0)!
    // In sub_8217A470, r17 preserves a1 (grcRenderTarget*). When r17 != 0, an offscreen
    // render target (pause snapshot, boot orbital camera, bloom, shadows) is being rendered.
    // Forcing 1280x720 single tile onto offscreen targets breaks their layout and EDRAM allocation.
    if (r17.u64 != 0) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    r7.u64 = r28.u64;  // tile width  = screen width
    r8.u64 = r25.u64;  // tile height = screen height

    // dword_827D42A4 = tile count (already computed and stored) -> 1
    base[0x827D42A4 + 0] = 0;
    base[0x827D42A4 + 1] = 0;
    base[0x827D42A4 + 2] = 0;
    base[0x827D42A4 + 3] = 1;
}

// EDRAM capacity check bypass — sub_82410D70 (guest D3D CreateSurface,
// auto-allocation path) validates alloc_base + size_in_tiles <= 0x800 (2048,
// the real console EDRAM) at 0x82410E34 and destroys the surface / returns
// NULL past it. Single-tile 720p 2xMSAA needs 2880 tiles, so every render
// target fails and the screen collapses into aliased EDRAM bands. The SDK's
// virtual EDRAM is 4096 tiles and the guest surface header keeps 12-bit base
// fields (max 4095), so allocations up to 4096 are safe. r11 = base + size;
// returning true jumps to the success branch (0x82410E48).
bool Patch_EdramLimit(PPCRegister& r3, PPCRegister& r30, PPCRegister& r11) {
    if (!larecomp::settings::single_tile.Get()) return false;
    const uint32_t base = static_cast<uint32_t>(r3.u32);
    const uint32_t size = static_cast<uint32_t>(r30.u32);
    const uint32_t end  = static_cast<uint32_t>(r11.u32);
    // Base tile must fit in the 12-bit hardware register field (max 4095).
    // The SDK's virtual EDRAM is 4096 tiles (20 MB).
    return base < 4096 && end <= 4096;
}

// The same hook works for both Motion Blur instructions!
bool Patch_DisableMotionBlur(PPCRegister& r3) {
    if (larecomp::settings::disable_motion_blur.Get()) {
        r3.u64 = 0; // li r3, 0
        return true;
    }
    return false;
}

// The game's MSAA switch. The NATIVE renderer never turns it off: the game
// always draws its own 2 sample layout, and the renderer's MSAA row
// (mcla_gpu_msaa, OFF/2X/4X/8X) and an upscaler that runs set how many samples
// the host gives the game's targets, 1 for OFF, live (gpu/msaa_level.h). So
// OFF and an upscaler start are the same game as 2X and none of them restart
// it. disable_msaa -- the XENIA row, hidden under the native renderer -- is
// read only for the Xenia renderer, which keeps the game's own switch. The
// call also decides the upscaler's start (gpu.h GetStartUpscale, which waits
// a moment for the upscaler probe): this hook runs once, when the game makes
// its renderer, so that wait is the game thread's and not the renderer's.
static bool MsaaOff() {
    if (larecomp::cvar::GetFlagByName("mcla_gpu") == "true") {
#if defined(LARECOMP_HAS_GPU)
        (void)mcla::gpu::GetStartUpscale();
#endif
        return false;
    }
    return larecomp::settings::disable_msaa.Get();
}

bool Patch_DisableMSAA(PPCRegister& r11) {
    if (MsaaOff()) {
        r11.u64 = 1; // li r11, 1
        return true;
    }
    return false;
}

// BadassBaboon's Recomp Adjustments: Foliage imposter shadow bypass
bool Patch_DisableImposterShadows(PPCRegister& r11) {
    if (larecomp::settings::disable_imposter_shadows.Get()) {
        r11.u64 = 0; // li r11, 0
        return true;
    }
    return false;
}

bool Patch_DisableDoF() {
    return larecomp::settings::disable_dof.Get();
}

// Disable DoF at the composite. Hooked at sub_8260EBB8 entry where r3 = dofObj
// (dword_829054A0). Zeroing the circle-of-confusion vector at dofObj+0xF0 collapses
// the per-pixel blur to sharp with the scene fully intact — verified in gameplay,
// menu AND freecam. The composite runs every frame DoF is drawn, so this covers
// every state without touching the setters (blocking those left stale DoF in the
// menu). The other composite inputs (+0x158/+0x128/+0x138/+0x1B0) are NOT safe to
// zero — they white-out / desaturate the frame — so only +0xF0 is touched.
void Patch_DofComposite(PPCRegister& r3) {
    if (!larecomp::settings::disable_dof.Get()) return;
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    uint32_t o = static_cast<uint32_t>(r3.u64);  // dofObj guest address
    if (!o) return;
    for (int i = 0; i < 16; ++i) base[o + 0xF0 + i] = 0;  // CoC vector = 0 -> no blur
}

void Patch_ScaleTrafficLOD(PPCRegister& f0) {
    f0.f64 = f0.f64 * larecomp::settings::lod_traffic_scale.Get();
}

// The scale in force for the cvar's value: the player's own when it is above 0, else what auto comes
// to: 2.0 on a Mid or High GPU class, else 1.0, the game's own distance: on Low, while the class is not
// known (before the renderer's device opens; always with the Xenia renderer, which never finds one) and
// in a build without the native renderer. The pause row's label for it is gpu_class::CityLodLabel.
static double CityLodInForce(double user_value) {
#if defined(LARECOMP_HAS_GPU)
    return mcla::gpu::gpu_class::CityLodScale(user_value);
#else
    return user_value > 0.0 ? user_value : 1.0;
#endif
}

// CITY LOD works by writing the scale times 300 to 0x827E0DE0 every frame (called from frame_timing.cpp).
// A second hook at 0x822D5BC4 (Patch_ScaleCityLOD) ran after the multiply it targets and changed
// nothing; it is removed.
void UpdateCityLODMemory() {
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    const double user_value = larecomp::settings::lod_city_scale.Get();
    const float scale = static_cast<float>(CityLodInForce(user_value));
    float final_lod = scale * 300.0f;

    constexpr uint32_t city_lod_addr = 0x827E0DE0;
    if (ReadGuestF32(base, city_lod_addr) != final_lod) {
        WriteGuestF32(base, city_lod_addr, final_lod);
    }

    // One line each time the scale in force changes (the first frame, the class becoming known, a row or
    // the class override changed), not one a frame.
    static float logged_scale = -1.0f;
    if (scale != logged_scale) {
        logged_scale = scale;
        std::string why = "set by lod_city_scale";
        if (!(user_value > 0.0)) {
#if defined(LARECOMP_HAS_GPU)
            namespace gc = mcla::gpu::gpu_class;
            why = gc::Known() ? "auto, GPU class " + std::string(gc::ClassName(gc::Current()))
                              : std::string("auto, GPU class not known yet");
#else
            why = "auto, no GPU class in this build";
#endif
        }
        LARECOMP_APP_INFO("[lod] city LOD scale {} ({} m), {}", scale, final_lod, why);
    }
}
#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

bool Patch_AspectRatio_82233EB4(PPCRegister& f0) { return false; }
bool Patch_AspectRatio_82214BB8(PPCRegister& f10) { return false; }
bool Patch_AspectRatio_822E5E68(PPCRegister& f12) { return false; }
bool Patch_AspectRatio_8223E5E0(PPCRegister& f13) { return false; }
void Patch_SingleTile(PPCRegister& r7, PPCRegister& r8, PPCRegister& r17, PPCRegister& r25, PPCRegister& r28) {}
bool Patch_EdramLimit(PPCRegister& r3, PPCRegister& r30, PPCRegister& r11) { return false; }
bool Patch_DisableMotionBlur(PPCRegister& r3) { return false; }
bool Patch_DisableMSAA(PPCRegister& r11) { return false; }
bool Patch_DisableDoF() { return false; }
void Patch_DofComposite(PPCRegister& r3) {}
void Patch_ScaleTrafficLOD(PPCRegister& f0) {}
void UpdateCityLODMemory() {}
bool Patch_DisableImposterShadows(PPCRegister& r11) { return false; }
#endif // REXGLUE_HAS_XEO3_TARGET
