// Display and render switches: aspect ratio, single-tile predicated tiling and
// the EDRAM bound it needs, motion blur, MSAA, depth of field, foliage imposter
// shadows, and the traffic and city LOD scales.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <cstdint>
#include <string>
#include <string_view>

#include "hooks.h"
#include "hooks_internal.h"
#include "larecomp_log.h"

REXCVAR_DEFINE_BOOL(disable_motion_blur, false, "MCLA/Patches", "Disable Motion Blur completely.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(disable_imposter_shadows, true, "MCLA/Patches", "Performance Mode: Foliage won't cast shadows.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(disable_msaa, false, "MCLA/Patches", "Disable Anti-Aliasing (MSAA).")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(disable_dof, true, "MCLA/Patches", "Disable Depth of Field (DoF) completely.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(lod_traffic_scale, 1.0, "MCLA/LOD", "Escala de LOD do Tráfego (0.1 - 10.0)")
    .range(0.1, 10.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(lod_city_scale, 1.0, "MCLA/LOD", "Escala de LOD da Cidade (0.1 - 10.0)")
    .range(0.1, 10.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(aspect_ratio, "16:9", "MCLA/Patches", "Screen Aspect Ratio")
    .allowed({"16:9", "16:10", "21:9", "32:9"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(single_tile, false, "MCLA/Performance",
    "Render the scene in a single predicated-tiling tile instead of two. Halves draw calls "
    "and state traffic with MSAA on. Requires the enlarged virtual EDRAM (SDK >= this build).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Off, because it was measured off. e0d9e94 turned the SDK flag on for the
// overlap it is supposed to buy, but on the bench (menu_cam slot 8, 40-116 s,
// three interleaved pairs against a build of ac2453d - the commit before that
// batch - with the same SDK DLLs) it costs about 7%:
//
//   pre-batch build                40.26 fps  (40.47 / 40.25 / 40.07)
//   this build, flag off           40.88 fps  (40.75 / 40.68 / 41.20)
//   this build, flag on            38.04 fps  (37.72 / 38.25 / 38.15)
//
// Ending a submission at every PM4 primary buffer end trades one batched
// submit per frame for many small ones; the driver overhead of the extra
// ExecuteCommandLists and fence signals outweighs the overlap here. It may
// still win on a faster GPU, which is why this stays a cvar instead of a
// deletion - set it true in larecomp.toml to measure that.
REXCVAR_DEFINE_BOOL(submit_on_primary_buffer_end, false, "MCLA/Performance",
    "Drive the SDK's d3d12_submit_on_primary_buffer_end. Ends a D3D12 submission at every PM4 "
    "primary buffer end instead of batching the frame. Measured ~7% slower on a GTX 1650. "
    "With graphics_backend = vulkan the SDK's own vulkan_submit_on_primary_buffer_end (on by "
    "default) applies instead; not measured here, so it is left alone.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// Function to apply/revert the Aspect Ratio patch in GPU memory
void ApplyAspectRatioPatch(std::string_view ratio) {
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

    LARECOMP_APP_INFO("ApplyAspectRatioPatch called! Ratio: {}, Memory Before: {:02X} {:02X} {:02X} {:02X}", 
        ratio, patch_ptr[0], patch_ptr[1], patch_ptr[2], patch_ptr[3]);

    uint32_t val = 0x3FE38E39; // 16:9 Default (1.777777f)
    if (ratio == "16:10") {
        val = 0x3FCCCCCD; // 16:10 (1.600000f)
    } else if (ratio == "21:9") {
        val = 0x40155555; // 21:9 (2.333333f)
    } else if (ratio == "32:9") {
        val = 0x40638E39; // 32:9 (3.555555f)
    }

    // Write the 4 bytes in Big-Endian at the correct address
    patch_ptr[0] = (val >> 24) & 0xFF; // MSB
    patch_ptr[1] = (val >> 16) & 0xFF;
    patch_ptr[2] = (val >> 8)  & 0xFF;
    patch_ptr[3] = val         & 0xFF; // LSB

    LARECOMP_APP_INFO("Memory After: {:02X} {:02X} {:02X} {:02X}", 
        patch_ptr[0], patch_ptr[1], patch_ptr[2], patch_ptr[3]);
}

static bool GetAspectRatio(double& out_val) {
    std::string ratio = REXCVAR_GET(aspect_ratio);
    if (ratio == "4:3") {
        out_val = 1.3333333;
        return true;
    } else if (ratio == "16:10") {
        out_val = 1.6000000;
        return true;
    } else if (ratio == "21:9") {
        out_val = 2.3333333;
        return true;
    } else if (ratio == "32:9") {
        out_val = 3.5555556;
        return true;
    }
    return false;
}


bool Patch_AspectRatio_82233EB4(PPCRegister& f0) {
    return GetAspectRatio(f0.f64);
}
bool Patch_AspectRatio_82214BB8(PPCRegister& f10) {
    return GetAspectRatio(f10.f64);
}
bool Patch_AspectRatio_822E5E68(PPCRegister& f12) {
    return GetAspectRatio(f12.f64);
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
    if (!REXCVAR_GET(single_tile)) return;

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
    if (!REXCVAR_GET(single_tile)) return false;
    const uint32_t base = static_cast<uint32_t>(r3.u32);
    const uint32_t size = static_cast<uint32_t>(r30.u32);
    const uint32_t end  = static_cast<uint32_t>(r11.u32);
    // Base tile must fit in the 12-bit hardware register field (max 4095).
    // The SDK's virtual EDRAM is 4096 tiles (20 MB).
    return base < 4096 && end <= 4096;
}

// The same hook works for both Motion Blur instructions!
bool Patch_DisableMotionBlur(PPCRegister& r3) {
    if (REXCVAR_GET(disable_motion_blur)) {
        r3.u64 = 0; // li r3, 0
        return true;
    }
    return false;
}

// The game's MSAA switch. Under the NATIVE renderer its MSAA row
// (mcla_gpu_msaa, OFF/2X/4X/8X) owns it and disable_msaa -- the XENIA row,
// hidden there -- is not read; both cvars are read by name or flag here, the
// renderer's lives with the renderer.
static bool MsaaOff() {
    if (rex::cvar::GetFlagByName("mcla_gpu") == "true") {
        return rex::cvar::GetFlagByName("mcla_gpu_msaa") == "off";
    }
    return REXCVAR_GET(disable_msaa);
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
    if (REXCVAR_GET(disable_imposter_shadows)) {
        r11.u64 = 0; // li r11, 0
        return true;
    }
    return false;
}

bool Patch_DisableDoF() {
    return REXCVAR_GET(disable_dof);
}

// Disable DoF at the composite. Hooked at sub_8260EBB8 entry where r3 = dofObj
// (dword_829054A0). Zeroing the circle-of-confusion vector at dofObj+0xF0 collapses
// the per-pixel blur to sharp with the scene fully intact — verified in gameplay,
// menu AND freecam. The composite runs every frame DoF is drawn, so this covers
// every state without touching the setters (blocking those left stale DoF in the
// menu). The other composite inputs (+0x158/+0x128/+0x138/+0x1B0) are NOT safe to
// zero — they white-out / desaturate the frame — so only +0xF0 is touched.
void Patch_DofComposite(PPCRegister& r3) {
    if (!REXCVAR_GET(disable_dof)) return;
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    uint32_t o = static_cast<uint32_t>(r3.u64);  // dofObj guest address
    if (!o) return;
    for (int i = 0; i < 16; ++i) base[o + 0xF0 + i] = 0;  // CoC vector = 0 -> no blur
}

void Patch_ScaleTrafficLOD(PPCRegister& f0) {
    f0.f64 = f0.f64 * REXCVAR_GET(lod_traffic_scale);
}

void UpdateCityLODMemory() {
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    float scale = static_cast<float>(REXCVAR_GET(lod_city_scale));
    float final_lod = scale * 300.0f;

    constexpr uint32_t city_lod_addr = 0x827E0DE0;
    if (ReadGuestF32(base, city_lod_addr) != final_lod) {
        WriteGuestF32(base, city_lod_addr, final_lod);
    }
}

void Patch_ScaleCityLOD(PPCRegister& f13) {
    // Multiplicamos o valor que a engine acabou de ler da memória pelo nosso slider
    f13.f64 = f13.f64 * REXCVAR_GET(lod_city_scale);
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
void Patch_ScaleCityLOD(PPCRegister& f13) {}
bool Patch_DisableImposterShadows(PPCRegister& r11) { return false; }
#endif // REXGLUE_HAS_XEO3_TARGET
