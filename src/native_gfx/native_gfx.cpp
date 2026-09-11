#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — entry point, cvar and guest swap hook.

// The diagnostic loggers in this runtime use fopen/fprintf; MSVC deprecates the
// former in favour of fopen_s and the warning is noise here.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "native_gfx.h"

#include <atomic>
#include <cstdio>
#include <string>

#include <rex/cvar.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/keybinds.h>

#include <rex/ui/d3d12/d3d12_presenter.h>

#include "d3d12/blit_pass.h"
#include "d3d12/presenter_output.h"
#include "d3d12/d3d12_smoke_triangle.h"
#include "d3d12/context.h"
#include "d3d12/first_draw.h"
#include "d3d12/external_blit.h"
#include "d3d12/frame_capture.h"
#include "d3d12/memory_census.h"
#include "guest/occlusion.h"
#include "guest/render_state.h"
#include "guest/resource_lock.h"
#include "guest/texture_ownership.h"
#include "guest/vblank_probe.h"
#include "nocp/nocp_app.h"
#include "guest/texture_format.h"
#include "d3d12/pipeline_cache.h"
#include "d3d12/render_target_pool.h"
#include "d3d12/renderdoc_hook.h"
#include "d3d12/resource_cache.h"
#include "d3d12/shader_db.h"
#include "d3d12/texture_binding.h"
#include "d3d12/texture_cache.h"

// Defined in guest/texture_ownership.cpp, read here for bring-up and for the
// periodic report. Same global-scope rule as the declaration above.
REXCVAR_DECLARE(uint32_t, mcla_native_gfx_own_textures);

// Defined in guest/vblank_probe.cpp.
REXCVAR_DECLARE(bool, mcla_native_gfx_vblank_probe);

REXCVAR_DEFINE_UINT32(
    mcla_native_gfx_skip_draw_first, 0, "MCLA/NativeGfx",
    "Bisection: first per-frame draw number to skip. The number is the draw's position within the "
    "frame, the same counter the report calls `offered`, and it restarts every frame. Inert until "
    "mcla_native_gfx_skip_draw_last is non-zero.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(
    mcla_native_gfx_skip_draw_last, 0, "MCLA/NativeGfx",
    "Bisection: last per-frame draw number to skip; 0 disables skipping entirely. "
    "Set a range, look at the screen, halve the range that still shows the artefact. Eleven runs "
    "isolate one draw out of two thousand, and unlike a hypothesis it cannot be wrong -- which is "
    "why it exists after nine guesses failed on the black shards.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(
    mcla_native_gfx_dump_draw_first, 0, "MCLA/NativeGfx",
    "First per-frame draw number to dump to native_gfx_draws.txt. Same numbering as the skip "
    "range, so a range narrowed by bisection can be pasted straight in. Inert until "
    "mcla_native_gfx_dump_draw_last is non-zero.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(
    mcla_native_gfx_dump_draw_last, 0, "MCLA/NativeGfx",
    "Last per-frame draw number to dump; 0 disables. Writes shader ids, primitive type, render "
    "target, every vertex stream's guest base and stride, and the world-view-projection -- the "
    "fields that have actually explained artefacts on this path. The file is appended to and grows "
    "fast: narrow the range with the skip cvars first.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(
    mcla_native_gfx_fetch_size_unit, 4, "MCLA/NativeGfx",
    "Bytes per unit of the vertex fetch constant's 24-bit size field. 0 uses the built-in 4. "
    "4 is what BeginVertices' own packet implies -- it writes dword1 = (4*dwords) | endian with "
    "dwords = (count*stride)/4, so the field IS a dword count. "
    "16 is what the bound-stream kQuadList draws measure: the field times sixteen equals "
    "element_count * stride exactly, on eight of eight draws from 384 to 12928 bytes, while times "
    "four leaves the vertex view a quarter short -- which is how a quad gets one real corner and "
    "three at the origin, and why huge black shards cross the map. "
    "Both cannot be right about the same field. Run once with 16: if the shards go and nothing "
    "else regresses, the unit is 16; if geometry that works today starts reading past its buffer, "
    "it is 4 and the quad draws are misread somewhere else.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(
    mcla_native_gfx_unsupplied_drop, false, "MCLA/NativeGfx",
    "Bisect switch: drop a draw whose shader declares a vertex attribute the vertex declaration "
    "does not supply, instead of binding a zero stream for it. "
    "The zero stream is the normal behaviour and the better one -- dropping loses real geometry, "
    "measured at 9243 display-shaped draws gutted per report when this was the default. But the "
    "zero stream rests on the assumption that an unpatched vfetch reads zeros on hardware, and it "
    "is one of the few things that can produce vertices collapsed toward the origin: a shader "
    "blending POSITION0 with a zeroed POSITION1 gives exactly the long stretched triangles seen "
    "across the scene. Turn this on for one run: if an artefact disappears, the zero stream is "
    "producing it; if it stays, this branch is eliminated.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(
    mcla_native_gfx_unlock_invalidate, false, "MCLA/NativeGfx",
    "Invalidate cached textures from the guest's own unlock instead of waiting for the page "
    "write watch to fault. D3DResource_Unlock flushes a dirty range the resource has been "
    "accumulating (BaseFlush at +0x14, MipFlush at +0x18, packed 16.16 in 128-byte units), and "
    "reading it costs two loads. Exact bytes instead of whole pages, no page-protection fault, "
    "and it fires when the guest considers the data final rather than on the next touch. "
    "Does NOT replace the write watch: writes that never go through a lock (streaming straight "
    "into resource memory) are only caught by the watch. Off by default until the counters show "
    "the ranges are sane -- see the lock line in the periodic report.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(mcla_native_gfx, false, "MCLA/NativeGfx",
                    "MCLA Native Graphics Runtime. OFF (default): the game renders through the "
                    "normal Xenia-based path, nothing changes. ON: the native D3D12 backend is "
                    "initialized and the bring-up instrumentation runs (see "
                    "mcla_native_gfx_present for the presentation takeover).")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(mcla_native_gfx_firstdraw, false, "MCLA/NativeGfx",
                    "Execute ONE real MCLA draw through the native pipeline, into a dedicated "
                    "render target, and write mcla_native_gfx_firstdraw.txt/.tga. Does not "
                    "present and does not touch the guest render path, so it cannot fight the "
                    "Xenia command processor. Runs once per session.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(mcla_native_gfx_capture, 0, "MCLA/NativeGfx",
                      "Accumulate this many guest draws of the main scene pass into one native "
                      "render target and write mcla_native_gfx_frame.txt/.tga. 0 disables. "
                      "Supersedes mcla_native_gfx_firstdraw when non-zero.");

REXCVAR_DEFINE_UINT32(mcla_native_gfx_capture_delay, 0, "MCLA/NativeGfx",
                      "Frame boundaries to skip before arming the one-shot capture. The early "
                      "frames after boot show low-detail placeholder textures (LZX streaming has "
                      "not caught up) and an unconverged HDR exposure, giving a dark, muddy "
                      "image. Set e.g. 300 to capture a fully-streamed, well-exposed frame.");

REXCVAR_DEFINE_UINT32(mcla_native_gfx_auxstage, 0, "MCLA/NativeGfx",
                      "Diagnostic: how far to take auxiliary (non-anchor) passes. "
                      "0 = not rendered at all. 1 = acquire the pooled target only. "
                      "2 = also bind it and clear. 3 = also record the draw. "
                      "A single auxiliary draw hangs the GPU, so this bisects which "
                      "stage does it: resource creation, binding, or the draw itself.");

REXCVAR_DEFINE_BOOL(mcla_native_gfx_msaa_depth_cs, true, "MCLA/NativeGfx",
                    "Resolve a multisampled depth surface with the compute pass. Off restores "
                    "the shipped behaviour, an all-zero depth copy, which is what killed the "
                    "distance fog, the per-object contact shadow under vehicles and the depth of "
                    "field's circle of confusion at the same time. Diagnostic only.");

REXCVAR_DEFINE_BOOL(mcla_native_gfx_depth_reclear_infer, true, "MCLA/NativeGfx",
                    "Let an ATLAS-shaped full-source depth resolve arm the depth re-clear on its "
                    "own, on top of the clear the guest's resolve asked for. The inference "
                    "exists for the shadow cascades: four of them share ONE 640x640 surface, "
                    "each resolved into its own quadrant of the 1280x1280 atlas, and without a "
                    "re-clear cascades 1..3 render against cascade 0's depth. It used to fire on "
                    "ANY full-source depth resolve, which also caught the scene target -- there "
                    "the game resolves its full depth so the ambient-occlusion pass can SAMPLE "
                    "it and then keeps drawing into the same buffer, so the re-clear wiped "
                    "1280x720 depth to the far plane and xAmbientOcclusionShadows (black quad, "
                    "alpha 0.813, depth GREATER_EQUAL under reverse-Z) passed on every pixel it "
                    "covered, multiplying a car's lower body to 18.7% (0.02278 -> 0.00426, exact "
                    "in three channels). Suppressing the inference entirely fixed the car but "
                    "moved the daytime shadows, so it is narrowed instead: only a resolve whose "
                    "DESTINATION is larger than the source is an atlas page. Off leaves only the "
                    "guest's declared clear, which is the shape that showed the shadow change.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(mcla_native_gfx_reclear_probe, false, "MCLA/NativeGfx",
                    "TEMP DIAG: log which path armed needs_depth_reclear and on what surface "
                    "shape. Two places arm the same flag -- the shape inference in "
                    "RenderTargetPool::NoteResolve and the guest's own clear request in "
                    "NotifyResolve -- and once it is set they are indistinguishable, so "
                    "attributing a wrong re-clear needs this.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(mcla_native_gfx_shadow_bias, 16, "MCLA/NativeGfx",
                     "Depth bias, in D24 quanta, added to the 640x640 shadow pass. Measured "
                     "against the emulated path on the same camera and hour: the native shadow "
                     "atlas comes out 7..17 quanta BELOW the emulated one in all four cascades "
                     "(0.0% of texels within one quantum), while run-to-run drift between two "
                     "native captures is only +-4. __PS_ShadowBlend compares four PCF taps "
                     "against a receiver depth the shader computes in float, so an atlas that "
                     "low makes lit ground shadow itself: the whole ground plane sat at 2 of 4 "
                     "taps and only 68.5% of the frame was fully lit. At 16 that is 92.5%, and "
                     "every probe point matches the emulated HDR to 3-4 decimals (sunlit "
                     "pavement 0.1694 vs 0.1699, shaded asphalt 0.1255 vs 0.1260, sky "
                     "unchanged). The auto-exposure then stops compensating, which is what the "
                     "washed-out look was. WHY the rasterised depth lands low is not explained: "
                     "the viewport is [0,1], ZSCALE/ZOFFSET are neutral, the guest programs no "
                     "polygon offset (the registers read zero and the enable bits are set), and "
                     "the resolve into the atlas is bit-exact against the 640x640 target. 0 "
                     "restores the unbiased behaviour.");

REXCVAR_DEFINE_BOOL(mcla_native_gfx_rectlist_nocull, true, "MCLA/NativeGfx",
                    "Disable face culling for kRectangleList draws. The Xenos rect primitive "
                    "describes an AREA and generates its own triangles; synthesising the fourth "
                    "corner on the CPU gives them a winding the guest never chose. Measured on "
                    "the minimap: the circular punch came out clockwise on a Y-down target "
                    "against cull_back with CCW-front, so every fragment was culled and the map "
                    "kept its square corners. Off restores the old behaviour, for A/B.");
REXCVAR_DEFINE_BOOL(mcla_native_gfx_exp_bias_unit, false, "MCLA/NativeGfx",
                    "Neutraliza gInvColorExpBias para 1.0 em vez de multiplicar o valor "
                    "enviado pelo 2^bias do alvo. Diagnostico do mar estourado: no passe de "
                    "reflexo da agua o produto da 0.25 enquanto na cena da 1.0.");
REXCVAR_DEFINE_UINT32(mcla_native_gfx_skip_water, 0, "MCLA/NativeGfx",
                      "Diagnostico: pula draws de agua por familia, para saber qual pinta a "
                      "faixa branca. Bitmask: 1 = xCityOceanShore, 2 = xCityOceanWater, "
                      "4 = xCityOceanWaterLOD, 8 = xCityPondWater.");
REXCVAR_DEFINE_BOOL(mcla_native_gfx_skip_punch, false, "MCLA/NativeGfx",
                    "Diagnostic: drop the minimap's circular punch draw. On, the 220x220 "
                    "target should keep its square corners; if it looks identical with the "
                    "punch on, the punch is changing no pixel at all.");
REXCVAR_DEFINE_BOOL(mcla_native_gfx_verify_textures, true, "MCLA/NativeGfx",
                    "Re-check a cached TEXTURE against the guest memory it was decoded from, "
                    "once per entry per frame, and drop it when they differ. Same lost-invalidation "
                    "hole as the buffer cache: measured, the minimap's circular mask entry at "
                    "0x02D64000 held an unrelated texture for the whole session while guest "
                    "memory still held the circle, so the punch sampled a near-constant mask "
                    "and the map kept its square corners. Off restores the old behaviour, for "
                    "A/B.");
REXCVAR_DEFINE_BOOL(mcla_native_gfx_verify_regions, true, "MCLA/NativeGfx",
                    "Re-check a clean buffer region against guest memory once per frame "
                    "before binding it, and re-upload when they differ. The write watch "
                    "loses notifications: measured on MCLA, five xPed vertex regions held "
                    "bytes that differed from guest memory for 200 consecutive observations "
                    "each while still marked clean, which is what draws a pedestrian with "
                    "the previous mesh's vertices. Off restores the old behaviour, for A/B; "
                    "the cost is one hash of each region something actually binds, once a "
                    "frame.");
REXCVAR_DEFINE_BOOL(mcla_native_gfx_decl_float, true, "MCLA/NativeGfx",
                    "Read the two 32-bit float vertex declaration types at their measured "
                    "width: 0x002C23A5 is FLOAT2 and 0x001A23A6 is FLOAT4, not FLOAT1 and "
                    "FLOAT2. Off restores the previous (short) reading, for A/B. Measured "
                    "with DECLGAP, which counts the bytes an element actually occupies: "
                    "0x001A23A6 spans 16 bytes across 7170 samples, 0x002C23A5 spans 8.");
REXCVAR_DEFINE_BOOL(mcla_native_gfx_reclear, true, "MCLA/NativeGfx",
                    "Honour a guest clear that arrives after the target was already cleared "
                    "this frame, restricted to the pass's viewport. One pooled target is "
                    "shared by every pass of the same shape, so without this the second pass "
                    "of a frame inherits the first one's pixels: the foliage impostor atlases "
                    "came back with other species smeared over the background, which put every "
                    "texel above the shadow shader's 10/255 cut and made every tree shadow a "
                    "square.");

REXCVAR_DEFINE_BOOL(mcla_native_gfx_dumprt, false, "MCLA/NativeGfx",
                    "Diagnostic: write every large resolve destination to a .tga at the end "
                    "of the capture. Counting a fetch as resolved only proves a resource was "
                    "handed back, not that it holds the right pixels. Costs a full GPU stall "
                    "per target, so it is off by default.");

REXCVAR_DEFINE_BOOL(mcla_native_gfx_present, false, "MCLA/NativeGfx",
                    "Native presentation takeover (smoke-test stage): suppress the guest swap "
                    "and present the native frame instead. MUST stay off while the Xenia command "
                    "processor is still consuming the game's draws: without swaps its D3D12 "
                    "submission never closes and the GPU eventually TDRs (observed DEVICE_HUNG). "
                    "It becomes the default path once the native runtime executes the draws "
                    "itself and the guest command stream is empty.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(mcla_native_gfx_continuous, false, "MCLA/NativeGfx",
                    "EXPERIMENTAL gameplay mode: render every guest frame through the native "
                    "pipeline and present it, instead of the one-shot capture. Keeps all caches "
                    "across frames, so the HDR exposure adaptation converges. Does NOT suppress "
                    "the guest swap (avoids the command-processor TDR), so the guest still runs "
                    "underneath. Requires mcla_native_gfx.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_UINT32(mcla_native_gfx_upload_mb, 64, "MCLA/NativeGfx",
                      "Transient upload ring per in-flight frame, in MiB. 0 uses the built-in "
                      "default. Geometry region uploads share this ring with constants and "
                      "textures; at the old fixed 16 MiB the ring was exhausted by geometry on "
                      "every frame, so no draw was ever recorded and the native runtime only "
                      "published a frame when the geometry happened to fit. Raise it if the "
                      "exhaustion report still fires, lower it to save host memory "
                      "(the buffer is committed once per in-flight frame).\n"
                      "\n"
                      "Was 32, which silently halved the 64 the built-in default and its comment "
                      "in context.cpp argue for. Raised because the report fired on the frame the "
                      "HUD first loads: 21 texture uploads had taken 30952 KiB of the 32 MiB ring "
                      "and the 22nd -- the 1024x1024 minimap mask atlas, 4 MiB -- did not fit. "
                      "That is not a cosmetic loss: the mask falls back to the neutral 1x1 white, "
                      "and xAlphaModulate__PS_Textured computes `1 - mask` for a "
                      "One/One/ReversedSubtract punch, so a white mask subtracts nothing and the "
                      "minimap keeps its square corners instead of being clipped to a circle. "
                      "Intermittent by nature -- it only bites on a frame where the burst leaves "
                      "less than 4 MiB. Measured need was 31.7 MiB in use plus the 4 MiB request.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_UINT32(mcla_native_gfx_region_kb, 128, "MCLA/NativeGfx",
                      "Ceiling on a merged geometry region, in KiB. 0 uses the built-in "
                      "default. The buffer cache merges a request with every region it "
                      "overlaps, and that union only grows -- one request bridging two regions "
                      "swallows everything between them, which makes the result overlap the "
                      "next request, and so on. A region is re-uploaded whole whenever any byte "
                      "in it is dirtied, so runaway merging turns small dynamic writes into "
                      "tens of MiB of re-upload per frame. Lower this if the exhaustion report "
                      "still shows geometry filling the ring.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(
    mcla_native_gfx_resolve_shape_fallback, true, "MCLA/NativeGfx",
    "When a colour resolve misses, retry the pool by SHAPE alone (same size, same "
    "depth format, same sample count) instead of also demanding the colour format the "
    "resolve-time registers name. MCLA has two 320x180 post-process passes, one HDR and "
    "one LDR, and the LDR resolve at 0x02DE6000 misses every frame against a pool that "
    "only ever rendered the HDR one -- so the tonemap samples a BLACK 320x180 where the "
    "emulated path has real data. Measured on menu camera 53: the native frame comes out "
    "a uniform 1.8x brighter than the emulated one and clips 11.6% of the screen against "
    "2.6%, with the HDR SCENE target identical between the paths (0.0312 against 0.0327), "
    "so the gain enters at this stage. Turn off to get the dropped resolve back.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(
    mcla_native_gfx_clear_only_resolve, true, "MCLA/NativeGfx",
    "Satisfy a colour resolve whose source pass was only CLEARED, never drawn into. "
    "Such a pass never asks the pool for a target, so the resolve is dropped and the "
    "destination keeps whatever guest memory held -- zero. MCLA does this for its small "
    "shadow collectors (8x8 at 0x0329C000 and 0x0329D000, cleared to 0xFF7F7F7F once at "
    "load), which the character eye material samples on its NIGHT path and multiplies "
    "into its light as saturate(-0.25 + s): reading zero paints every eye black after "
    "dark, while day worked because the day path samples the 256x256 collector that a "
    "real pass renders. Gated to a small surface whose fetch, viewport key and resolve "
    "rect all agree AND whose guest clear is still pending -- the draw path consumes a "
    "pending clear on a pass's first draw, so it surviving until the resolve is the "
    "no-draw signal. Turn off to get the dropped resolve back while bisecting.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(
    mcla_native_gfx_gen_mips, false, "MCLA/NativeGfx",
    "Gerar cadeia de mip no host para textura de nivel unico, por box filter 2x2, "
    "restrito a 32 bits por texel sem compressao. Anisotropia so escolhe um nivel mais "
    "fino ao longo do eixo maior: sem cadeia, um sampler ANISOTROPIC 16x perfeito nao "
    "muda pixel nenhum. Medido no passe de cena: de 16.1M de binds >= 512x512, 12.7M sao "
    "kBaseMap e ZERO deles carrega cadeia -- o jogo marca base-map porque a textura tem "
    "um nivel so -- e 70% desses (9.0M) sao formato 6 (k_8_8_8_8), que e box filter na "
    "CPU sem precisar recomprimir BC. E MELHORIA, nao paridade: o 360 tambem aliasava "
    "nessas superficies, por isso o default e off.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(mcla_native_gfx_gamma_ramp, true, "MCLA/NativeGfx",
                    "Apply the guest display gamma ramp at present, which is what the "
                    "emulated path does as the DC_LUT and the no-CP path never did.");
REXCVAR_DEFINE_BOOL(mcla_native_gfx_texture_swizzle, true, "MCLA/NativeGfx",
                    "Apply the fetch constant's 12-bit swizzle to the SRV's "
                    "Shader4ComponentMapping instead of the D3D12 default. The two encodings "
                    "match one-for-one (0..3 component, 4 = zero, 5 = one), and 34 of MCLA's "
                    "k_8_8_8_8 textures -- the colour-grading LUTs bound on every draw -- carry "
                    "0x60A = (Z,Y,X,W), so in principle the default mapping swaps red and blue. "
                    "In practice turning it on tints the sky dome red, which means the untiler's "
                    "8-in-32 endian swap already puts the channels in host order for at least "
                    "some of them and the swizzle is then applied twice. OFF until a per-format "
                    "host swizzle (the piece Xenia composes with the guest one) is worked out.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(
    mcla_native_gfx_color_exp_bias, true, "MCLA/NativeGfx",
    "Fold the bound render target's colour exponent bias back into "
    "gInvColorExpBias (constant register c27.x) on upload, for both shader "
    "banks. The bias is read from RB_COLOR_INFO in the device's register "
    "shadow, so this needs no command processor, no PM4 and no EDRAM. "
    "Xenos biases a render target's colour exponent to keep precision in EDRAM's "
    "fixed-point formats; the game uploads the reciprocal and its shaders "
    "pre-divide, because the output merger multiplies it back on write. A D3D12 "
    "R16G16B16A16_FLOAT target applies no such bias, so the division survives and "
    "nothing undoes it. Measured on the car body: gInvColorExpBias.x = 0.0625, "
    "exactly 1/16, with gLightAmbient.w = 1.0, so PS_CarPaint's "
    "oC0.w = gInvColorExpBias.x * gLightAmbient.w hands the blend 0.0625 -- pixel "
    "history confirms it -- and the paint blends SrcAlpha/InvSrcAlpha, painting "
    "the body at six percent opacity. 178 shaders read this constant, 12 of them "
    "straight into oC0.w. Positionally safe: c27 is gInvColorExpBias in all 165 "
    "shaders that declare it and nothing else ever occupies c27, and only .x is "
    "ever read. A target the game gave no bias is left untouched, and a bias "
    "that changes between targets within a frame stays correct because the "
    "register is read per draw rather than assumed. "
    "Turn off to get the pre-fix behaviour back while bisecting.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(mcla_native_gfx_bind_memo, true, "MCLA/NativeGfx",
                    "Reuse the previous draw's texture binding when its 32 fetch constants are "
                    "byte-identical and nothing has moved under them. Texture binding measured at "
                    "22ms of a 95ms frame -- about 63000 slot binds per frame at 350ns each -- and "
                    "the per-slot work was already correct, so the only thing left to remove was "
                    "doing it twice. Measured in the MENU it does not pay: 15.7% hit rate, every "
                    "single miss a genuine key mismatch (guard invalidations: zero), and bind went "
                    "22.5ms -> 21.8ms, which is noise. That scene is 4200 mostly-distinct draws; "
                    "GAMEPLAY repeats far more (traffic is one model drawn many times) and has not "
                    "been measured yet. Kept behind this cvar for exactly that A/B -- turn it off "
                    "to get the unconditional rebind back.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(
    mcla_native_gfx_aniso, -1, "MCLA/NativeGfx",
    "Anisotropic filtering override, with the same numbering as the emulated path's\n"
    "anisotropic_override: -1 leaves the guest's own choice alone (the default), 0 disables\n"
    "it, and 1 to 5 force 1x, 2x, 4x, 8x or 16x. The native runtime already honoured what\n"
    "the game asks for -- it reads the ratio out of the fetch constant -- but had no way to\n"
    "raise it, while the emulated path was running with anisotropic_override=5 from the\n"
    "same toml. That is a texture-sharpness difference at oblique angles (road and ground\n"
    "surfaces above all) that has nothing to do with MSAA.\n"
    "\n"
    "Applies only to a sampler that is already filtering with mipmaps, which is the same\n"
    "eligibility rule the emulated path uses: forcing anisotropy onto a point-sampled or\n"
    "base-map sampler would change what the game asked for rather than how sharply it is\n"
    "filtered.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(
    mcla_native_gfx_alias_missed_resolve, false, "MCLA/NativeGfx",
    "EXPERIMENT. A colour resolve with no matching pooled source is dropped; the emulated path "
    "cannot drop one, because it resolves out of EDRAM by address and always produces something. "
    "With this on, a missed colour resolve is aliased onto the most recent colour copy so the "
    "question 'does anything sample that destination' can be answered by looking at the screen. "
    "The content is deliberately wrong; this is a probe, not a fix.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(
    mcla_native_gfx_surface_key, false, "MCLA/NativeGfx",
    "Include what the guest asked for -- its requested sample count and surface pitch -- in the "
    "render target pool's key, so two guest surfaces of the same shape stop sharing one pooled "
    "target.\n"
    "\n"
    "OFF BY DEFAULT BECAUSE IT MEASURED WORSE. The HUD is drawn spread across TWO 1280x720 "
    "R8G8B8A8 surfaces, and merging them by shape is what lands all of it in the target that "
    "gets displayed -- with this on, the whole 2D layer disappears, HUD included. The merge is "
    "load-bearing: it stands in for the composition the native runtime deliberately does not "
    "model. Kept as a bisection switch, not as a fix.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_UINT32(
    mcla_native_gfx_mrt, 0x3F, "MCLA/NativeGfx",
    "Second render target support, as a bitmask -- one bit per piece, so a fault can be\n"
    "bisected without a rebuild. 0 is exactly what the runtime did before; 0x3F is the lot.\n"
    "\n"
    "  0x01  the resolve honours RB_COPY_CONTROL.copy_src_select instead of always\n"
    "        reading colour target 0.\n"
    "  0x02  RB_COLOR1_INFO attaches a second colour surface to the pass's target.\n"
    "  0x04  the pass's clear wipes that second surface too.\n"
    "  0x08  it is transitioned back to RENDER_TARGET with target 0.\n"
    "  0x10  a resolve naming target 1 copies FROM it.\n"
    "  0x20  both targets are bound and the PSO declares two, with RB_BLENDCONTROL1 as\n"
    "        target 1's equation.\n"
    "\n"
    "MCLA needs this for the impostor bake: xPropFoliage__PSGenerateImposterNight writes the\n"
    "tree's colour to oC0 and its packed normal to oC1, and the game resolves the two into\n"
    "different addresses. With one target bound, both resolves copied target 0, the impostor\n"
    "shader read the colour atlas as its normal atlas, and normalize(colour*2-1) came out a\n"
    "constant -- a flat, uniformly lit canopy.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_UINT32(
    mcla_native_gfx_msaa, 0, "MCLA/NativeGfx",
    "Multisample count for the HDR scene target: 2, 4 or 8. Anything else (including the "
    "default) keeps every target single-sampled, which is what the runtime did unconditionally "
    "and why the native path has no antialiasing while the emulated one does -- measured, all "
    "2737 textures of a native capture are msSamp=1 against six at 2x and sixteen at 4x in an "
    "emulated capture of the same game. Scoped to the scene: the LDR composite is read back for "
    "presentation and the aux passes (shadow 640x640, minimap 220x220) are left alone. Costs "
    "memory and bandwidth in proportion to the count.\n"
    "\n"
    "Working since 2026-09-03. Four things had to agree with the sample count, and each one\n"
    "was found by measurement, not by reading: the pooled target and the PSO (the two that\n"
    "hardcoded 1), the RESOLVE key in native_gfx.cpp (its Find() asked for one sample and\n"
    "missed the target, so the scene resolved 1200 times into nothing and every fetch of it\n"
    "took the neutral white fallback -- a white screen), and every readback of a pooled\n"
    "target (CopyTextureRegion refuses a multisampled source, and that single error killed\n"
    "the command list for the rest of the session: Close failed, then every Reset failed).\n"
    "\n"
    "Depth is resolved with ResolveSubresourceRegion in MAX mode -- D3D12 has no sample-0\n"
    "depth resolve, and MAX keeps the nearest surface under the reverse-Z main pass.\n"
    "Measured at 4x on the menu camera: silhouette transitions widened 34-39% against the\n"
    "same scene at 1x, with zero validation errors.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(mcla_native_gfx_guest_clear, true, "MCLA/NativeGfx",
                    "Start a render target at the colour the GUEST asked for (D3DDevice_Clear, "
                    "sub_824195E8) instead of the runtime's kClearColor {0.02,0.02,0.04}. The "
                    "guest clear reaches EDRAM through the command processor, which no-CP mode "
                    "does not run, so today every fresh target is painted the debug colour. "
                    "Measured in \"cap_capture.rdc\": the ShadowBlend target is 100% kClearColor "
                    "before the pass and still 80.6% after it -- only 18% of the screen gets a "
                    "ShadowBlend draw, and the rest reads as full shadow, which is the sun "
                    "switching off past a distance. That surface's own guest clear is 0xFF7F7F7F "
                    "= 0.498, exactly the neutral its draws write. On by default since the user "
                    "confirmed in game that it brings the distant sun back; the single global "
                    "pending slot is the part still to be checked, since a clear that belongs to "
                    "one surface would then be handed to whichever pass draws next.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(mcla_native_gfx_alpha_ref, true, "MCLA/NativeGfx",
                    "Feed SharedConstants.g_AlphaThreshold from RB_ALPHA_REF and RB_COLORCONTROL "
                    "instead of the fixed 0.5 the bring-up used. The ALPHA lines the diagnostic "
                    "asked for came back, and they settle it: every (func, ref) pair MCLA uses is "
                    "func=4 (GREATER) with ref in {0, 0.003922, 0.007843, 0.015686} plus one "
                    "func=5 (NOTEQUAL) ref=0. The reference is never anywhere near 0.5, so the "
                    "bring-up constant discarded every fragment whose alpha fell between the real "
                    "reference and 0.5 -- which is the entire car body: measured in \"car.rdc\", "
                    "the paint draws (26615/26653) upload g_AlphaThreshold=0.5 and RenderDoc "
                    "reports shaderDiscarded over the whole body, while the same car's full "
                    "silhouette is present in the depth buffer. Debugging the discarded pixel at "
                    "(700, 400) of draw 26653 gives the number outright: the shader computes "
                    "alpha = 0.04375, subtracts the 0.5 threshold and discards on the negative "
                    "result -- against the real reference (<= 0.015686) that fragment is kept. "
                    "Wheels and glass survived because "
                    "their alpha is 1 or the test is off, which is exactly the 'only rims and "
                    "windows render' symptom. The earlier counter-evidence (palm fronds turning "
                    "into solid dark quads with this ON) was measured BEFORE the texcoord swap "
                    "fix, i.e. with transposed UVs, so the foliage was sampling the wrong texels "
                    "and its alpha mask meant nothing -- it has to be re-measured. ON by default "
                    "now; the switch stays so the fixed 0.5 can be put back while bisecting.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(mcla_native_gfx_depth32, false, "MCLA/NativeGfx",
                    "Give a guest kD24S8 depth surface a D32_FLOAT_S8X24 host format instead of "
                    "the literal D24_UNORM_S8. The shadow atlas is why: each 640x640 cascade "
                    "occupies 1449..6300 of the 16777215 D24 levels, because the light projection "
                    "spans the city and a cascade is a sliver of it. Adjacent stored values differ "
                    "by one quantum, so nothing is mis-quantised -- there is just no headroom, and "
                    "__PS_ShadowBlend's four PCF taps straddle the receiver depth by about three "
                    "quanta, making the comparison a coin flip per texel. Measured near the camera: "
                    "63.6% of pixels on intermediate PCF steps, 8.0% fully lit, and the 0.75 pixels "
                    "cohere with their neighbours 48.7% of the time against a 40.7% base rate, "
                    "which is noise rather than penumbra. Costs 8 bytes per texel instead of 4. "
                    "Turn off to get the literal mapping back while bisecting.\n"
                    "\n"
                    "DEFAULT IS OFF. It was turned ON on a hypothesis about shadow acne and never "
                    "verified -- the guest asks for kD24S8, which is what the 360 had, so forcing float "
                    "may move us AWAY from the hardware rather than toward it. It also changes the depth "
                    "format of EVERY kD24S8 target, not just the shadow atlas, which changes render target "
                    "keys and pool behaviour globally. Shadow artefacts and missing geometry were reported "
                    "with it on; it goes back off until someone measures a frame with and without.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(mcla_native_gfx_half_pixel, true, "MCLA/NativeGfx",
                    "Fill SharedConstants.g_HalfPixelOffset with (-1/viewport_w, +1/viewport_h). "
                    "23 of the 25 vertex shaders in a captured frame end with "
                    "SV_Position.xy += g_HalfPixelOffset.xy * w, the D3D9 -> D3D10+ pixel-centre "
                    "correction; left at zero every draw lands half a pixel off the texel grid, "
                    "which is where the soft 2D edges come from. On by default because the "
                    "shaders demand it; the switch exists so it can be taken out of the picture "
                    "while bisecting a colour or coverage regression.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(mcla_native_gfx_swapped_texcoords, true, "MCLA/NativeGfx",
                    "Fill SharedConstants.g_SwappedTexcoords from the vertex declaration. A RAGE "
                    "vertex packs a 32-bit POSITION and a 16-bit TEXCOORD in one stream, but a "
                    "stream can only be byte-swapped at one width; swapped as 8in32 the 16-bit "
                    "pair arrives as (v, u) and every texture renders transposed, which looks "
                    "like a 90-degree rotation to the left. The translated shaders already carry "
                    "the fix (tfetchTexcoord -> value.yxwz) and only needed the mask. On by "
                    "default; the switch exists to take it back out while bisecting.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(mcla_native_gfx_texcache_mb, 768, "MCLA/NativeGfx",
                      "Byte budget (MiB) for the native texture cache. The cache is keyed by the "
                      "fetch constant, so a streaming game keeps producing new keys as it pages "
                      "geometry and textures in and out; without a budget every one of them stays "
                      "resident for the session and the footprint grows without bound. Over the "
                      "budget, least-recently-used entries are released (fence-gated, never one "
                      "the current frame bound). 0 = unbounded, the old behaviour.");

REXCVAR_DEFINE_UINT32(mcla_native_gfx_census, 120, "MCLA/NativeGfx",
                      "Frame boundaries between memory census reports written to "
                      "native_gfx_mem.txt: process commit/working set, DXGI local (VRAM) and "
                      "non-local usage, and the live size of every native cache. The pool whose "
                      "delta tracks the process delta is the leak. 0 disables.");

namespace mcla::native_gfx {

namespace {

enum class InitState { kNotStarted, kActive, kFailed };
std::atomic<InitState> g_state{InitState::kNotStarted};
// Set by the keybind (UI thread), consumed at the frame boundary (guest thread).
std::atomic<bool> g_rdc_capture_request{false};
D3D12SmokeTriangle g_triangle;
rex::ui::Presenter* g_presenter = nullptr;

// The real-draw subsystems. Created lazily on the first attempt so a session
// that never enables the first-draw path pays nothing.
D3D12Context g_draw_context;
ShaderDatabase g_draw_shaders;
BufferCache g_buffers;
TextureCache g_textures;
TextureBinder g_binder;
PipelineCache g_pipelines;
RenderTargetPool g_render_targets;
bool g_draw_ready = false;
bool g_draw_init_failed = false;
const rex::ui::d3d12::D3D12Provider* g_provider = nullptr;

// Continuous-mode present path: a presenter output and a fullscreen blit on the
// SAME device as g_draw_context (both come from the shared provider), so the
// pooled display target can be sampled straight into the guest output.
PresenterOutput g_present_output;
BlitPass g_blit;
bool g_present_ready = false;

// TEMP DIAG (remove after): why the continuous present does or does not reach
// the guest output. The only thing this path logs today is the blit pipeline,
// and it logs once, so "the swap hook never fired", "the takeover was off" and
// "the refresh failed" all look identical in the log -- an absent line. These
// counters separate them; they are reported with the frame-boundary line.
std::atomic<uint32_t> g_swap_hook_calls{0};
std::atomic<uint32_t> g_present_calls{0};
std::atomic<uint32_t> g_present_ok{0};
std::atomic<uint32_t> g_present_no_takeover{0};
std::atomic<uint32_t> g_present_no_presenter{0};
std::atomic<uint32_t> g_present_no_display{0};
std::atomic<uint32_t> g_present_refresh_false{0};
std::atomic<uint32_t> g_present_no_cmdlist{0};
std::atomic<uint32_t> g_present_blit_false{0};

// One-time lazy setup: resolve the SDK graphics system, require the D3D12
// backend, initialize the smoke-test renderer. Any failure latches kFailed
// and logs once; the guest swap then proceeds normally.
bool TryInitialize() {
  auto* runtime = rex::Runtime::instance();
  if (!runtime) {
    REXLOG_ERROR("[native_gfx] no Runtime instance");
    return false;
  }
  // Where the D3D12 provider and presenter come from, and the difference is
  // the whole point of the no-command-processor mode.
  //
  // Normally they come from the graphics system -- which lives in the
  // rexgpu-xenos plugin, so asking for it is asking for the emulator. In nocp
  // mode there is no graphics system at all; the app created the provider and
  // presenter itself out of rex::ui::d3d12 (a swapchain and descriptor pools,
  // no PM4, no EDRAM, no register file). Attaching to those makes this runtime
  // the ONLY thing drawing, instead of the second renderer beside an emulator.
  const bool nocp_mode = nocp::WantNoCommandProcessor();
  rex::system::IGraphicsSystem* graphics = nullptr;
  if (!nocp_mode) {
    // The concrete GraphicsSystem lives inside the rexgpu-xenos plugin, which
    // consumers never link; provider() and presenter() are on the interface.
    graphics = runtime->graphics_system();
    if (!graphics || !graphics->has_presentation()) {
      REXLOG_ERROR("[native_gfx] graphics system has no presentation, cannot attach");
      return false;
    }
  } else if (!nocp::Provider() || !nocp::PresenterPtr()) {
    REXLOG_ERROR("[native_gfx] nocp mode has no provider/presenter yet, cannot attach");
    return false;
  }
  // `graphics_backend` is a fork-local cvar. REXCVAR_QUERY looks it up by
  // STRING, so against a stock SDK this compiles and then quietly answers ""
  // ("value-initialized T when the cvar is missing"), which fails both
  // comparisons and refuses to attach for a backend nobody ever selected.
  // Ask whether the flag exists before believing its value: where there is no
  // backend selector there is nothing to disagree with, and the provider cast
  // below is the real check either way. A registered flag still refuses, so
  // behaviour on this tree is unchanged.
  if (rex::cvar::GetFlagInfo("graphics_backend")) {
    const std::string backend = REXCVAR_QUERY(std::string, graphics_backend);
    if (backend != "auto" && backend != "d3d12") {
      REXLOG_ERROR("[native_gfx] graphics_backend is '{}', native runtime needs d3d12", backend);
      return false;
    }
  }
  auto* provider = nocp_mode ? nocp::Provider()
                             : static_cast<rex::ui::d3d12::D3D12Provider*>(graphics->provider());
  if (!provider) {
    REXLOG_ERROR("[native_gfx] no graphics provider");
    return false;
  }
  g_provider = provider;
  if (!g_triangle.Initialize(*provider)) {
    return false;
  }
  g_presenter = nocp_mode ? nocp::PresenterPtr() : graphics->presenter();
  if (g_presenter == nullptr) {
    return false;
  }
  // Take F11 over from the SDK's bind. The SDK one brackets the capture around
  // the EMULATED command processor's swap, which is the wrong window once this
  // runtime owns the frame: our whole batch is recorded and submitted between
  // frame boundaries, so a capture opened at the guest swap contains the
  // presenter blit and nothing else. Registering a second bind on the same key
  // would never fire -- ProcessKeyEvent returns on the first match and the SDK
  // registered first -- so the SDK's is unregistered (which only nulls its
  // callback) and ours is registered after it.
  //
  // Only done once this runtime is actually active; when it is off the SDK bind
  // stays as it was.
  rex::ui::UnregisterBind("bind_renderdoc_capture");
  rex::ui::RegisterBind("bind_native_gfx_renderdoc", "F11",
                        "Capture the next guest frame with RenderDoc (native runtime)",
                        [] { RequestRenderDocCapture(); });

  // Resource ownership: reserve the guest-visible heap up front so its one
  // "HostHeap ready" line lands in the log next to the rest of bring-up rather
  // than in the middle of a texture create. Only when ownership is actually
  // wanted -- the reservation is 32 MiB of the guest's physical window, which
  // is not free on a title this tight on memory.
  if (REXCVAR_GET(mcla_native_gfx_own_textures) != 0) {
    InitTextureOwnership();
  }

  // Continuous mode is the HYBRID path, and it is deprecated. It renders next
  // to the command processor and cannot stop it from presenting without an
  // edit to the SDK -- which was made and then reverted, because a runtime
  // that needs the command processor changed to work is not a native runtime.
  // The replacement is nocp/: no graphics system at all.
  // Only true when there IS a command processor. In nocp mode this runtime is
  // the only thing drawing, which is the whole point, and saying otherwise
  // would be a stale claim in the log.
  if (REXCVAR_GET(mcla_native_gfx_continuous) && !nocp_mode) {
    REXLOG_WARN(
        "[native_gfx] continuous mode renders alongside the command processor, which also "
        "presents -- expect the emulated frame on screen. Superseded by the no-command-processor "
        "path (src/native_gfx/nocp/).");
  }
  return true;
}

}  // namespace

// Defined near the bottom of the file; the frame boundary above calls it.
bool PresentContinuousDisplay();

bool Active() {
  if (!REXCVAR_GET(mcla_native_gfx)) {
    return false;
  }
  InitState state = g_state.load(std::memory_order_acquire);
  if (state == InitState::kActive) {
    return true;
  }
  if (state == InitState::kFailed) {
    return false;
  }
  const bool ok = TryInitialize();
  g_state.store(ok ? InitState::kActive : InitState::kFailed, std::memory_order_release);
  if (!ok) {
    REXLOG_ERROR("[native_gfx] initialization failed; staying on the normal path");
  }
  return ok;
}


void TryFirstDraw(const uint8_t* base, uint32_t dev, uint32_t primitive_type,
                  uint32_t element_count, uint32_t start_element, int32_t base_vertex,
                  bool indexed) {
  const bool want_continuous = REXCVAR_GET(mcla_native_gfx_continuous);
  const bool want_capture = REXCVAR_GET(mcla_native_gfx_capture) > 0 || want_continuous;
  const bool want_first = REXCVAR_GET(mcla_native_gfx_firstdraw);
  if ((!want_first && !want_capture) || g_draw_init_failed) {
    return;
  }
  // Continuous mode never "finishes"; the one-shot done-check only applies to
  // the bounded capture.
  if (!want_continuous && (want_capture ? FrameCaptureDone() : FirstRealDrawDone())) {
    return;
  }
  if (!g_draw_ready) {
    // Report which subsystem failed: collapsing these into one condition
    // makes an initialization failure impossible to diagnose from the log.
    const char* failed = nullptr;
    if (!g_provider) {
      failed = "no D3D12 provider";
    } else if (!g_draw_context.Initialize(*g_provider)) {
      failed = "D3D12Context";
    } else if (!g_pipelines.Initialize(g_draw_context)) {
      failed = "PipelineCache (root signature)";
    } else if (!g_binder.Initialize(g_draw_context)) {
      failed = "TextureBinder (descriptor heaps)";
    } else if (!g_draw_shaders.Load()) {
      failed = "ShaderDatabase (assets/mcla_shaders.pack)";
    }
    // The bridge that lets a texture fetch find the render target that
    // produced it, instead of decoding never-written guest memory.
    g_textures.SetRenderTargetLookup(&g_render_targets);
    // Guest writes to an uploaded vertex/index range have to invalidate it.
    // Without this a range was uploaded once and never again, so a mesh
    // streamed into recycled memory rendered with the previous mesh's bytes.
    // A failure here is not fatal: it only restores that old behaviour.
    g_buffers.StartWatchingGuestWrites();
    // Same hole on the texture side: a tile sampled while it was still
    // streaming in stayed half-decoded forever, which is the grid of black
    // squares on the map screen.
    g_textures.StartWatchingGuestWrites();
    if (failed) {
      REXLOG_ERROR("[native_gfx] first-draw initialization failed at: {}", failed);
      g_draw_init_failed = true;
      return;
    }
    g_draw_ready = true;

    // Continuous mode takes the swap over: the guest swap is suppressed and the
    // composite is presented straight through rex::ui::Presenter. It has to be
    // a takeover rather than a second refresh after the guest swap, because the
    // Presenter's guest output is single-producer -- refreshing it from this
    // thread while the command processor refreshes it from its own inside
    // IssueSwap is a data race on the mailbox.
    if (REXCVAR_GET(mcla_native_gfx_continuous)) {
      g_present_ready = true;
      SetContinuousMode(true);
      REXLOG_INFO("[native_gfx] continuous mode active (presenting at the guest swap)");
    }
  }
  // Continuous mode has no draw limit (the finish-on-limit path is gated off in
  // frame_capture); it just needs a non-zero value so the draw is not skipped.
  const uint32_t capture_limit =
      want_continuous ? 1000000u : uint32_t(REXCVAR_GET(mcla_native_gfx_capture));
  // Auxiliary passes render fully in continuous mode (aux_stage 3) by default,
  // BUT an explicit mcla_native_gfx_auxstage override is honoured even in
  // continuous mode. This matters because a single auxiliary-pass draw can hang
  // the GPU (DEVICE_HUNG TDR, confirmed by DRED: an auxiliary DrawIndexedInstanced
  // never completes). Setting auxstage=2 (bind+clear, no aux draw) lets that be
  // bisected — and used as a stopgap — without leaving continuous mode. 0 is
  // treated as "use the default 3" so the untouched default keeps full passes.
  const uint32_t aux_override = uint32_t(REXCVAR_GET(mcla_native_gfx_auxstage));
  const uint32_t aux_stage = want_continuous ? (aux_override ? aux_override : 3u) : aux_override;
  if (capture_limit) {
    // Multi-draw capture supersedes the single-draw diagnostic: both write to
    // the same subsystems, and running them together would interleave two
    // unrelated render targets in one command list.
    CaptureDraw(base, dev, primitive_type, element_count, start_element, base_vertex, indexed,
                capture_limit, g_draw_context, g_draw_shaders, g_buffers, g_textures, g_binder,
                g_pipelines, g_render_targets, aux_stage);
    return;
  }
  TryFirstRealDraw(base, dev, primitive_type, element_count, start_element, base_vertex, indexed,
                   g_draw_context, g_draw_shaders, g_buffers, g_textures, g_binder, g_pipelines);
}


// Upper bound on a surface the clear-only path is allowed to allocate. The
// collectors this exists for are 8x8; a full-screen resolve miss is a real
// dropped pass and has to keep reporting itself rather than be filled with a
// clear colour.
constexpr uint32_t kClearOnlyResolveMaxDimension = 64u;

void NotifyResolve(const uint8_t* base, uint32_t dev, uint32_t flags, uint32_t dest_texture,
                   uint32_t source_rect, uint32_t dest_point, uint32_t clear_color_ptr) {
  if (!Active() || !dest_texture) {
    return;
  }
  // The destination is a D3D texture object holding the same six-dword fetch
  // constant a later draw samples, at +28. Confirmed against the guest's own
  // resolve (sub_82420BA8), which reads the address as
  // *(dest+32) & 0xFFFFF000 and the pitch as (*(dest+28) >> 17) & 0x3FE0 —
  // bit for bit what DecodeTextureFetch computes from d[1] and d[0].
  constexpr uint32_t kDestFetchOffset = 28;
  uint32_t d[6];
  for (uint32_t i = 0; i < 6; ++i) {
    uint32_t v;
    std::memcpy(&v, rex::memory::GuestPtr(const_cast<uint8_t*>(base),
                                          dest_texture + kDestFetchOffset + 4 * i),
                4);
    d[i] = __builtin_bswap32(v);
  }
  const TextureFetch fetch = DecodeTextureFetch(d);
  if (!fetch.type_valid || fetch.width == 0 || fetch.height == 0) {
    return;
  }
  // The address the GPU actually writes — and therefore the address a later
  // fetch constant carries — is not the texture base: the guest adds a 4 KiB
  // page depending on bits 20+ before storing it in RB_COPY_DEST_BASE. From
  // sub_82420BA8 verbatim:
  //
  //   v64 = (((v54 >> 20) + 512) & 0x1000) + (v54 & 0x1FFFFFFF)
  //
  // For the shadow atlas that is 0xE6E64000 -> 0x06E64000 + 0x1000 =
  // 0x06E65000, which is exactly the address slot 15 samples in every
  // material shader. Without this the registration misses by one page and
  // every lookup fails.
  const uint32_t raw = fetch.base_address;
  const uint32_t dest = (((raw >> 20) + 512u) & 0x1000u) + (raw & 0x1FFFFFFFu);
  // flags & 7 == 4 selects the depth buffer as the source; 0..3 are colour.
  const bool from_depth = (flags & 7u) == 4u;
  // TEMP DIAG (remove after): WHICH colour target the guest is resolving.
  // `flags & 7` is the source index: 0..3 pick a colour target, 4 picks depth.
  // Everything below ignores the index and resolves the ONE colour surface the
  // pool holds for this shape, so a resolve of target 1 hands target 0's pixels
  // to target 1's destination address.
  {
    const uint32_t rt_index = flags & 7u;
    static uint32_t seen[32];
    static uint32_t seen_n = 0;
    const uint32_t sig = dest ^ (rt_index << 28);
    bool fresh = true;
    for (uint32_t i = 0; i < seen_n; ++i) {
      if (seen[i] == sig) { fresh = false; break; }
    }
    if (fresh && seen_n < 32) {
      seen[seen_n++] = sig;
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        std::fprintf(f, "RESOLVE_SRCIDX idx=%u dest=0x%08X %ux%u flags=0x%08X\n", rt_index, dest,
                     fetch.width, fetch.height, flags);
        std::fflush(f);
        std::fclose(f);
      }
    }
  }
  // Every destination, colour included: the address is what marks the data as
  // GPU-produced, and a colour target is invisible to a format-based test.
  NoteFrameCaptureResolve(dest, fetch.width, fetch.height, from_depth);

  // Tie the destination to the target that produced it. The source is
  // whatever the guest was rendering into at this moment, so the render state
  // read here is the same state the draws of that pass used.
  if (!g_draw_ready) {
    return;
  }
  const GuestRenderState rs = ReadRenderState(base, dev);
  const HostViewport hv = ComputeHostViewport(rs);
  RenderTargetKey key;
  key.rt_format = ColorRenderTargetFormatToDxgi(rs.color_format);
  // The same two fields the draw path keys on, or the resolve looks up a target
  // the draws never rendered into.
  if (REXCVAR_GET(mcla_native_gfx_surface_key)) {
    key.guest_msaa = rs.msaa_samples;
    key.surface_pitch = rs.surface_info & 0x3FFFu;
  }
  key.ds_format = DepthRenderTargetFormatToDxgi(rs.depth_format);
  key.width = uint32_t(hv.top_left_x + hv.width + 0.5f);
  key.height = uint32_t(hv.top_left_y + hv.height + 0.5f);
  // AFTER width: the rule reads it, and computing this first made every lookup
  // ask for one sample and miss the multisampled target.
  key.sample_count = PooledSampleCountForShape(key.rt_format, key.ds_format, key.width);
  if (key.rt_format == 0 || key.width == 0 || key.height == 0) {
    return;
  }
  RenderTarget* source = g_render_targets.Find(key);
  // Same shape, different colour format. MCLA has two 320x180 post-process
  // passes -- HDR (rt_format 10) and LDR (28) -- and the key built here takes
  // rs.color_format, so the LDR resolve misses a pool that only rendered the HDR
  // one. Measured: `RESOLVE_MISS dest=0x02DE6000 320x180 rt_fmt=28 count=600`
  // every frame, and the tonemap then samples a BLACK 320x180 (mean 0.0004)
  // where the emulated path has real data -- while the HDR scene target itself
  // matches between the two paths (0.0312 emulated against 0.0327 native), so
  // the divergence is at this stage and not in scene rendering.
  //
  // Only after the exact lookup has already failed, and only for a shape the
  // pool really rendered: this is not a lookup a draw can take.
  if (!source && !from_depth && REXCVAR_GET(mcla_native_gfx_resolve_shape_fallback)) {
    source = g_render_targets.FindByShape(key.width, key.height, key.ds_format,
                                          key.sample_count);
    if (source) {
      static uint32_t n = 0;
      if (n++ < 12u) {
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f, "RESOLVE_SHAPE dest=0x%08X %ux%u pediu fmt=%u achou fmt=%u\n", dest,
                       key.width, key.height, key.rt_format, source->key.rt_format);
          std::fflush(f);
          std::fclose(f);
        }
      }
    }
  }
  // The resolve's own source rectangle, read here rather than after the miss
  // check: when the lookup fails it is the only thing that says what shape the
  // guest was actually copying out, and the viewport registers at resolve time
  // have already been shown to disagree with the one the draws used.
  const auto read_be32_early = [&](uint32_t ea) -> int32_t {
    if (ea < 0x1000u) return 0;
    uint32_t v;
    std::memcpy(&v, rex::memory::GuestPtr(const_cast<uint8_t*>(base), ea), 4);
    return int32_t(__builtin_bswap32(v));
  };
  if (source) {
    // RB_COPY_CONTROL rides in `flags`: bit 8 clears colour, bit 9 clears
    // depth, as part of the same resolve. That is how the guest separates one
    // pass from the next on a surface it reuses -- impostor atlas generation
    // bakes one tree species, resolves it, and expects the tile to come back
    // empty for the next species. Dropping this made the silhouettes the union
    // of every species baked that frame: a palm and two trees came out as two
    // solid bushes.
    const bool clear_color = (flags & 0x100u) != 0u;
    const bool clear_depth = (flags & 0x200u) != 0u;
    if (clear_color) {
      float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      if (clear_color_ptr >= 0x1000u) {
        for (uint32_t i = 0; i < 4; ++i) {
          uint32_t v;
          std::memcpy(&v, rex::memory::GuestPtr(const_cast<uint8_t*>(base),
                                                clear_color_ptr + 4 * i),
                      4);
          v = __builtin_bswap32(v);
          std::memcpy(&rgba[i], &v, 4);
          if (!std::isfinite(rgba[i])) {
            rgba[i] = 0.0f;
          }
        }
      }
      SetPendingResolveClear(rgba);
      source->cleared = false;
    }
    if (clear_depth) {
      // The DECLARED path: the guest's own resolve asked for the depth to be
      // cleared (D3DRESOLVE_CLEARDEPTHSTENCIL on the console). Unlike the shape
      // inference in RenderTargetPool::NoteResolve this is intent, not a guess,
      // so it is never suppressed.
      source->needs_depth_reclear = true;
      NoteReclearArmed("guest", source->key.width, source->key.height);
    }
  }
  if (!source && !from_depth && REXCVAR_GET(mcla_native_gfx_clear_only_resolve)) {
    // CLEAR-ONLY PASS.
    //
    // The guest sets a small surface, clears it, and resolves it WITHOUT
    // issuing a single draw. Nothing ever asks the pool for that target, so
    // Find() cannot answer and the resolve is dropped -- guest memory at the
    // destination stays whatever it was, which is zero.
    //
    // Measured case: MCLA's small shadow collectors, 8x8 R8G8B8A8 resolved to
    // 0x0329C000 and 0x0329D000 once at load, cleared to 0xFF7F7F7F. The
    // character eye material (Character_eyes_normalmap) samples that collector
    // on its NIGHT path and scales its light by `saturate(-0.25 + s)`, so a
    // zero there paints the eyes black -- day worked because the day path hits
    // the 256x256 collector at 0x0329E000, which a real pass does render.
    //
    // Deliberately narrow, because Find()'s own comment is right: a resolve is
    // not an allocation request, and trusting one as such allocates a target
    // per bogus viewport until the GPU runs out. Three conditions together mean
    // "a cleared surface nobody drew into":
    //   * a guest clear is still PENDING -- the draw path consumes it on a
    //     pass's first draw, so it surviving to the resolve IS the no-draw
    //     signal;
    //   * the fetch, the viewport key and the resolve rect all agree;
    //   * the surface is small. A full-screen miss is a real dropped pass and
    //     must keep reporting itself, not be papered over with a clear.
    const bool shape_agrees = key.width == fetch.width && key.height == fetch.height;
    // NOT named `small`: rpcndr.h, pulled in through the Windows headers,
    // #defines that to `char`.
    const bool is_small = key.width <= kClearOnlyResolveMaxDimension &&
                          key.height <= kClearOnlyResolveMaxDimension;
    if (shape_agrees && is_small) {
      GuestClearRequest clear;
      if (TakeGuestClear(&clear) && clear.color) {
        RenderTarget* created = g_render_targets.Acquire(g_draw_context, key, clear.z);
        if (created && created->color) {
          g_render_targets.RequestClearOnlyFill(*created, clear.rgba);
          source = created;
          static uint32_t n = 0;
          if (n++ < 16u) {
            if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
              std::fprintf(f,
                           "CLEARONLY dest=0x%08X %ux%u rgba=%.3f,%.3f,%.3f,%.3f\n", dest,
                           key.width, key.height, clear.rgba[0], clear.rgba[1], clear.rgba[2],
                           clear.rgba[3]);
              std::fflush(f);
              std::fclose(f);
            }
          }
        }
      }
    }
  }
  if (!source) {
    // TEMP DIAG (remove after): a colour resolve with no source. Print the key
    // built from the viewport registers, the fetch the destination carries, and
    // the resolve's OWN rectangle -- then ask the pool what it does hold in that
    // format. The pause menu panel is produced into a 960x640 target and
    // resolved with a 1024x1024 key, so the three disagree.
    if (!from_depth) {
      // Once per destination address, not a global cap: the post-process chain
      // misses hundreds of times a frame at one address and used to spend the
      // whole budget before the interesting one (the UI surface) ever printed.
      static uint32_t seen_miss[64];
      static uint32_t seen_miss_n = 0;
      bool fresh = true;
      for (uint32_t i = 0; i < seen_miss_n; ++i) {
        if (seen_miss[i] == dest) { fresh = false; break; }
      }
      if (fresh && seen_miss_n < 64u) {
        seen_miss[seen_miss_n++] = dest;
        const int32_t rx0 = source_rect ? read_be32_early(source_rect) : 0;
        const int32_t ry0 = source_rect ? read_be32_early(source_rect + 4) : 0;
        const int32_t rx1 = source_rect ? read_be32_early(source_rect + 8) : 0;
        const int32_t ry1 = source_rect ? read_be32_early(source_rect + 12) : 0;
        const int32_t dx = dest_point ? read_be32_early(dest_point) : 0;
        const int32_t dy = dest_point ? read_be32_early(dest_point + 4) : 0;
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f,
                       "RESMISS dest=0x%08X fetch=%ux%u key=%ux%u/fmt%u/ds%u/s%u "
                       "rect=(%d,%d..%d,%d) at (%d,%d) flags=0x%08X pitch=%u vp=%.0fx%.0f\n",
                       dest, fetch.width, fetch.height, key.width, key.height, key.rt_format,
                       key.ds_format, key.sample_count, rx0, ry0, rx1, ry1, dx, dy, flags,
                       rs.surface_info & 0x3FFFu, hv.width, hv.height);
          std::fflush(f);
          std::fclose(f);
        }
        g_render_targets.LogTargetsForFormat(key.rt_format, "resolve-miss");
      }
    }
    // A pass we never rendered; nothing to hand to the destination.
    if (!from_depth && REXCVAR_GET(mcla_native_gfx_alias_missed_resolve)) {
      if (g_render_targets.AliasMissedColourResolve(dest, fetch.width, fetch.height)) {
        static uint32_t n = 0;
        if (n++ < 8u) {
          if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
            std::fprintf(f, "RESALIAS dest=0x%08X %ux%u\n", dest, fetch.width, fetch.height);
            std::fflush(f);
            std::fclose(f);
          }
        }
      }
    }
    NoteFrameCaptureResolveMiss(dest, fetch.width, fetch.height, key.width, key.height,
                                key.rt_format, key.ds_format);
    return;
  }
  // The destination is not simply "this target": the shadow map resolves each
  // 640x640 cascade into a quadrant of a 1280x1280 atlas, so the source is
  // SMALLER than the destination and registering the source could never
  // satisfy a fetch for the whole atlas. sub_82420BA8 computes the offset from
  // pSourceRect (a3: x0,y0,x1,y1) and pDestPoint (a5: x,y), so both are needed
  // to place the copy.
  const auto read_be32 = [&](uint32_t ea) -> int32_t {
    if (ea < 0x1000u) {
      return 0;
    }
    uint32_t v;
    std::memcpy(&v, rex::memory::GuestPtr(const_cast<uint8_t*>(base), ea), 4);
    return int32_t(__builtin_bswap32(v));
  };
  ResolveRegion region;
  if (source_rect) {
    region.src_x = read_be32(source_rect);
    region.src_y = read_be32(source_rect + 4);
    region.width = uint32_t(read_be32(source_rect + 8) - region.src_x);
    region.height = uint32_t(read_be32(source_rect + 12) - region.src_y);
  } else {
    region.width = source->key.width;
    region.height = source->key.height;
  }
  if (dest_point) {
    region.dst_x = read_be32(dest_point);
    region.dst_y = read_be32(dest_point + 4);
  }
  // RB_COPY_CONTROL.copy_src_select rides in `flags`: 0..3 name a colour
  // target, 4 names depth. Passing it on is what keeps the impostor bake's two
  // resolves apart -- measured on cam 44, the pair arrives as idx=1 into the
  // normal atlas and idx=0 into the colour atlas, and collapsing both onto
  // target 0 is what made the two byte-identical.
  g_render_targets.NoteResolve(*source, from_depth, dest, fetch.width, fetch.height, region,
                               (REXCVAR_GET(mcla_native_gfx_mrt) & 0x1u) ? (flags & 7u) : 0u);
  // Marked GPU-produced only now, once the copy is actually on its way. Marking
  // it up front — before the `g_draw_ready` guard above — condemned every
  // address we cannot bridge to the neutral fallback forever: resolves that
  // happen once outside a capture window (g_draw_ready still false) registered
  // the address and never produced a resource, so the object sampling it lost
  // its texture entirely instead of decoding guest memory. Bridge when we can,
  // decode when we cannot, and never claim ownership of an image we do not have.
  g_render_targets.NoteDestination(dest, fetch.width, fetch.height);
}

void NoteGuestDraw(int kind) { NoteFrameCaptureGuestDraw(kind); }

namespace {
// One slot is enough: the guest clears a surface immediately before the pass
// that renders into it, on the same thread that issues the draws, so the pool's
// policy clear for that pass's first draw is the very next consumer.
std::atomic<bool> g_guest_clear_pending{false};
std::atomic<uint32_t> g_guest_clear_color{0};
std::atomic<uint32_t> g_guest_clear_flags{0};
std::atomic<float> g_guest_clear_z{0.0f};
std::atomic<uint32_t> g_guest_clear_calls{0};
}  // namespace

void NoteGuestClear(uint32_t flags, uint32_t color, float z, uint32_t stencil) {
  const uint32_t n = g_guest_clear_calls.fetch_add(1, std::memory_order_relaxed);
  // TEMP DIAG: the point of the first build is to see WHICH clears arrive and
  // in what order relative to the policy clear, not just to apply them.
  if (n < 96u || (n % 512u) == 0u) {
    // native_gfx_diag.txt, not REXLOG: under RenderDoc the game is launched by
    // ExecuteAndInject and its stdout goes nowhere this session can read.
    if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
      std::fprintf(f, "GUESTCLEAR #%u flags=0x%X color=0x%08X z=%.4f stencil=%u\n", n, flags, color,
                   static_cast<double>(z), stencil);
      std::fclose(f);
    }
  }
  // Depth-only clears used to return here. They are kept now because the
  // re-clear path below needs them: a pass that re-uses a shared pooled target
  // must get its depth back too, or the second species is depth-tested against
  // the first one still sitting in the buffer.
  g_guest_clear_color.store(color, std::memory_order_relaxed);
  g_guest_clear_flags.store(flags, std::memory_order_relaxed);
  g_guest_clear_z.store(z, std::memory_order_relaxed);
  g_guest_clear_pending.store(true, std::memory_order_release);
}

void SetPendingResolveClear(const float rgba[4]) {
  // Same one-slot mechanism D3DDevice_Clear uses: the next draw into the
  // surface consumes it. A resolve that clears is always followed immediately
  // by the pass that renders into the wiped surface, so one slot is enough.
  uint32_t c = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    const float v = rgba[i] < 0.0f ? 0.0f : (rgba[i] > 1.0f ? 1.0f : rgba[i]);
    const uint32_t byte = uint32_t(v * 255.0f + 0.5f);
    // D3DCOLOR order, matching what NoteGuestClear stores: 0xAARRGGBB.
    const uint32_t shift = i == 0 ? 16 : (i == 1 ? 8 : (i == 2 ? 0 : 24));
    c |= byte << shift;
  }
  g_guest_clear_color.store(c, std::memory_order_relaxed);
  g_guest_clear_flags.store(0xFu, std::memory_order_relaxed);
  g_guest_clear_pending.store(true, std::memory_order_release);
}

bool TakeGuestClear(GuestClearRequest* out) {
  if (!g_guest_clear_pending.exchange(false, std::memory_order_acquire)) {
    return false;
  }
  const uint32_t flags = g_guest_clear_flags.load(std::memory_order_relaxed);
  const uint32_t c = g_guest_clear_color.load(std::memory_order_relaxed);
  if (out) {
    out->color = (flags & 0xFu) != 0u;
    out->depth = (flags & 0x30u) != 0u;
    out->rgba[0] = static_cast<float>((c >> 16) & 0xFFu) / 255.0f;
    out->rgba[1] = static_cast<float>((c >> 8) & 0xFFu) / 255.0f;
    out->rgba[2] = static_cast<float>(c & 0xFFu) / 255.0f;
    out->rgba[3] = static_cast<float>((c >> 24) & 0xFFu) / 255.0f;
    out->z = g_guest_clear_z.load(std::memory_order_relaxed);
  }
  return true;
}

bool TakeGuestClearColor(float rgba[4]) {
  GuestClearRequest req;
  if (!TakeGuestClear(&req) || !req.color) {
    return false;
  }
  const uint32_t c = g_guest_clear_color.load(std::memory_order_relaxed);
  rgba[0] = static_cast<float>((c >> 16) & 0xFFu) / 255.0f;
  rgba[1] = static_cast<float>((c >> 8) & 0xFFu) / 255.0f;
  rgba[2] = static_cast<float>(c & 0xFFu) / 255.0f;
  rgba[3] = static_cast<float>((c >> 24) & 0xFFu) / 255.0f;
  return true;
}

namespace {
// Begin/EndVertices are strictly paired on the guest draw thread, so one
// pending slot is enough. Kept out of the device shadow deliberately: the
// guest writes the fetch constant for this buffer straight into the command
// stream, not into the shadow BuildGeometrySnapshot reads.
struct PendingInlineDraw {
  uint32_t primitive_type = 0;
  uint32_t vertex_count = 0;
  uint32_t stride = 0;
  uint32_t buffer = 0;  // guest address returned by BeginVertices
  bool valid = false;
};
PendingInlineDraw g_pending_inline;
}  // namespace

void NoteBeginVertices(uint32_t dev, uint32_t primitive_type, uint32_t vertex_count,
                       uint32_t stride, uint32_t buffer) {
  (void)dev;
  g_pending_inline = PendingInlineDraw{primitive_type, vertex_count, stride, buffer,
                                       buffer != 0 && vertex_count != 0 && stride != 0};
}

void NoteEndVertices(const uint8_t* base, uint32_t dev) {
  if (!g_pending_inline.valid) {
    return;
  }
  const PendingInlineDraw d = g_pending_inline;
  g_pending_inline.valid = false;
  // Inline geometry (BeginVertices/EndVertices) is how the guest issues the
  // fullscreen composite/tonemap pass AND the 2D/HUD/UI. It must be recorded in
  // CONTINUOUS mode too, not only the one-shot capture: without it the composite
  // target got only its ~2 bound-stream draws and came out flat (no tonemap, no
  // UI). Mirror TryFirstDraw's mode handling.
  const bool want_continuous = REXCVAR_GET(mcla_native_gfx_continuous);
  const uint32_t capture_limit =
      want_continuous ? 1000000u : uint32_t(REXCVAR_GET(mcla_native_gfx_capture));
  if (capture_limit == 0 || !g_draw_ready || g_draw_init_failed ||
      (!want_continuous && FrameCaptureDone())) {
    return;
  }
  const uint32_t aux_override = uint32_t(REXCVAR_GET(mcla_native_gfx_auxstage));
  const uint32_t aux_stage = want_continuous ? (aux_override ? aux_override : 3u) : aux_override;
  // The RAW return value, not the page-fixed one. sub_8241CD88 applies
  //   (((v >> 20) + 512) & 0x1000) + (v & 0x1FFFFFFF)
  // only to the copy it writes into the FETCH CONSTANT, i.e. the address the
  // GPU reads through. What it returns is the pointer the guest itself writes
  // the vertices through — sub_8217A140 does `*(float *)result = x` on it — so
  // that is the address to read the data back from. Using the fixed one here
  // read a page past the data and crashed with an access violation.
  CaptureInlineDraw(base, dev, d.primitive_type, d.vertex_count, d.stride, d.buffer, capture_limit,
                    g_draw_context, g_draw_shaders, g_buffers, g_textures, g_binder, g_pipelines,
                    g_render_targets, aux_stage);
}

bool ShouldDumpRenderTargets() { return REXCVAR_GET(mcla_native_gfx_dumprt); }

void NoteResourceLocked(const uint8_t* base, uint32_t resource_va) {
  NoteResourceLock(base, resource_va);
}

void NoteResourceUnlocked(const uint8_t* base, uint32_t resource_va, uint32_t base_address,
                          uint32_t mip_address) {
  const ResourceUnlock unlock = ReadResourceUnlock(base, resource_va, base_address, mip_address);
  NoteResourceUnlock(unlock);
  if (!unlock.last || !REXCVAR_GET(mcla_native_gfx_unlock_invalidate)) {
    return;
  }
  if (!IsTextureUnlock(unlock)) {
    // Everything that is not a texture goes to the buffer cache, and measured,
    // that is where this signal is worth something: resource type 1 (vertex
    // buffers) is locked thousands of times per minute while textures are
    // locked only at load. The re-upload traffic it attacks was 126k per 600
    // frames, about 210 per frame.
    if (unlock.base_range.valid) {
      g_buffers.NoteGuestWrite(unlock.base_range.address, unlock.base_range.size);
    }
    return;
  }
  const GuestFlushRange& base_range = unlock.base_range;
  const GuestFlushRange& mip_range = unlock.mip_range;
  // Deliberately NOT gated on g_draw_ready. Measured at boot: all 388 unlocks
  // of a session's first frame happen before the first native draw, so gating
  // on it threw away exactly the writes that matter -- the streaming uploads
  // that land while a texture is being decoded, which is the half-decoded-tile
  // bug this signal exists to fix.
  //
  // Safe this early: NoteGuestWrite only pushes a pair of integers onto a
  // vector under a mutex, on an object that is valid from static
  // initialization. Queued, not applied -- dropping an entry needs the context
  // for the fence-gated release, so the queue is drained at the top of the next
  // Resolve, exactly like the write watch's.
  if (base_range.valid) {
    g_textures.NoteGuestWrite(base_range.address, base_range.size);
  }
  if (mip_range.valid) {
    g_textures.NoteGuestWrite(mip_range.address, mip_range.size);
  }
  // The periodic line is 600 boundaries apart and the interesting part happens
  // during loading, so say it once as soon as it actually works.
  static bool first_logged = false;
  if (!first_logged) {
    first_logged = true;
    REXLOG_INFO("[native_gfx] unlock invalidation live: first range {:#010x}+{}",
                base_range.valid ? base_range.address : mip_range.address,
                base_range.valid ? base_range.size : mip_range.size);
  }
}

void NoteD3DTextureCreated(const uint8_t* base, uint32_t d3d_texture_va) {
  NoteTextureCreate(base, d3d_texture_va);
}

void NotifyFrameBoundary() {
  // Geometry is what exhausts the upload ring (measured: ~16 MiB in ~93
  // allocations every frame, while constants and textures never get a turn).
  // This split says whether those are fresh regions or re-uploads of dirtied
  // ones -- different bugs, different fixes.
  g_buffers.ReportPeriodic();

  // Ownership counters, on the same cadence as the registry dump. Reported
  // here too because ownership does not depend on the registry cvar, and the
  // numbers that matter (validate_failed, orphaned) have to be visible without
  // turning a second diagnostic on.
  if (REXCVAR_GET(mcla_native_gfx_own_textures) != 0) {
    static uint64_t own_report = 0;
    if ((own_report++ % 600u) == 0u) {
      REXLOG_INFO("[native_gfx] texture {}", TextureOwnershipSummary());
    }
  }

  // Occlusion queries cannot run without a command processor, and MCLA gates a
  // vehicle's body on one. Selecting the guest's own no-occlusion path every
  // boundary is what keeps the body submitted; see guest/occlusion.h for the
  // slot-refresh mechanism that makes it permanent rather than intermittent.
  DisableGuestOcclusionQueries(rex::Runtime::instance()
                                   ? rex::Runtime::instance()->virtual_membase()
                                   : nullptr);
  {
    static uint64_t occ_report = 0;
    if ((occ_report++ % 600u) == 0u) {
      REXLOG_INFO("[native_gfx] {}", OcclusionSummary());
    }
  }

  // Vblank/flip machinery, for the no-command-processor port. Sampled every
  // boundary (two loads) and reported on the same cadence as the rest.
  ProbeVblankState(rex::Runtime::instance() ? rex::Runtime::instance()->virtual_membase()
                                            : nullptr);
  {
    static uint64_t vb_report = 0;
    if (REXCVAR_GET(mcla_native_gfx_vblank_probe) && (vb_report++ % 600u) == 0u) {
      REXLOG_INFO("[native_gfx] vblank {}", VblankProbeSummary());
    }
  }

  // The lock/unlock traffic is worth reporting whatever ownership is doing:
  // it is what says whether the guest's own dirty ranges can replace the page
  // write watch.
  {
    static uint64_t lock_report = 0;
    if ((lock_report++ % 600u) == 0u) {
      // Reported together with the cache's own invalidation split: the number
      // that matters is how much of `invalidated` the unlock signal accounts
      // for. If it covers nearly all of it, the page write watch is paying for
      // faults it no longer needs to take.
      const TextureCache::Stats& ts = g_textures.stats();
      REXLOG_INFO(
          "[native_gfx] resource {} | tex dropped={} (unlock={} watch={}) unlock_ranges={} "
          "(no_hit={})",
          ResourceLockSummary(), ts.invalidated, ts.invalidated_by_unlock,
          ts.invalidated_by_watch, ts.unlock_ranges, ts.unlock_ranges_no_hit);
    }
  }

  // TEMP DIAG (remove after): is the frame-boundary hook firing, and do the
  // continuous gates pass?
  {
    static unsigned fb = 0;
    ++fb;
    if (fb <= 10u || (fb % 60u) == 0u) {
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        std::fprintf(f,
                     "frameboundary#%u continuous=%d present_ready=%d draw_ready=%d | swap_hook=%u "
                     "present=%u ok=%u no_takeover=%u no_presenter=%u no_display=%u "
                     "refresh_false=%u no_cmdlist=%u blit_false=%u\n",
                     fb, ContinuousMode() ? 1 : 0, g_present_ready ? 1 : 0, g_draw_ready ? 1 : 0,
                     g_swap_hook_calls.load(std::memory_order_relaxed),
                     g_present_calls.load(std::memory_order_relaxed),
                     g_present_ok.load(std::memory_order_relaxed),
                     g_present_no_takeover.load(std::memory_order_relaxed),
                     g_present_no_presenter.load(std::memory_order_relaxed),
                     g_present_no_display.load(std::memory_order_relaxed),
                     g_present_refresh_false.load(std::memory_order_relaxed),
                     g_present_no_cmdlist.load(std::memory_order_relaxed),
                     g_present_blit_false.load(std::memory_order_relaxed));
        std::fflush(f);
        std::fclose(f);
      }
    }
  }
  // Every retired resource is parked UNTAGGED (sentinel fence ~0) until
  // EndFrameReleases() stamps it, and ReleaseCompleted only frees entries whose
  // fence value the GPU has passed. So a boundary that does not reach a stamp
  // leaks EVERYTHING retired since the last one -- retired buffer regions,
  // evicted textures, orphaned resolve destinations -- for the rest of the
  // session. The call used to sit inside the continuous branch under
  // `g_present_ready && g_draw_ready`, so the one-shot capture mode never
  // stamped at all, and continuous mode did not stamp until the presenter came
  // up. Every path below now ends at a stamp, and it is taken AFTER that path's
  // last submission so the fence value covers the whole frame.
  const auto stamp_releases = [] {
    if (g_draw_ready) {
      g_draw_context.EndFrameReleases();
      MemoryCensusTick(g_draw_context, g_buffers, g_textures, g_pipelines, g_render_targets);
      DumpResolveMisses();
    }
  };
  // Continuous mode, guest thread: submit the frame and publish its display
  // target for the command processor's swap callback to blit. Then reset the
  // per-frame state so the next frame re-accumulates. The actual present
  // happens in the CP's swap (NativeGuestOutputCallback), not here.
  if (ContinuousMode()) {
    if (g_present_ready && g_draw_ready) {
      const bool prepared = PrepareContinuousDisplay(g_draw_context, g_render_targets);
      // Present HERE, not from the swap hook.
      //
      // The guest swap is issued from INSIDE rage::grcDevice::EndFrame
      // (generated/larecomp_recomp.66.cpp:1798, in the body of
      // DEFINE_REX_FUNC(grcDevice_EndFrame)), so the swap hook runs while the
      // original is still on the stack -- before this boundary hook, which runs
      // after it returns. At that moment the frame's last draw batch is still
      // open on the runtime's command list, and D3D12Context::BeginFrame
      // refuses to open a second one:
      //
      //   if (!initialized_ || frame_open_) return nullptr;
      //
      // so the present failed silently and the hook fell through to the guest
      // swap, letting the command processor paint the guest output. The only
      // frames the native runtime ever presented were the ones where the draw
      // count happened to land exactly on a batch flush, leaving the list
      // closed -- the one-frame flashes.
      //
      // PrepareContinuousDisplay ends with EndFrame(), so here the list is
      // always closed. This also keeps the blit inside the RenderDoc bracket
      // below and ahead of stamp_releases(), whose fence value must cover it.
      if (prepared) {
        PresentContinuousDisplay();
      }
      // Programmatic RenderDoc capture of one native frame. The native present
      // bypasses the swapchain, so RenderDoc's Present-driven frame delimiter
      // (and its overlay) stop working once native_gfx takes over -- but the
      // in-application StartFrameCapture/EndFrameCapture bracket submissions on
      // the device directly, no swapchain needed. A capture spans boundary N
      // (Begin, below) to boundary N+1 (End, here), so it records the full
      // native frame between them. Triggered by dropping a file next to the exe
      // (created on demand once the desired scene is on screen), consumed
      // atomically with std::remove so it fires exactly once.
      // The 180+ stacked Begins that produced one 270 MB unreplayable capture
      // did not come from here -- they came from a second, unpaired
      // RenderDocBeginCapture in frame_capture.cpp that continuous mode re-armed
      // every frame. With that gone and RenderDocBeginCapture refusing to nest,
      // this can re-arm: the latch now trips only when EndFrameCapture actually
      // fails to close, which is the case the latch was protecting against.
      // Drop the trigger file again for another capture.
      static bool rdc_capturing = false;
      static bool rdc_done = false;
      if (rdc_capturing) {
        rdc_capturing = false;
        if (!RenderDocEndCapture(g_draw_context.device())) {
          rdc_done = true;
        }
      }
      // After PrepareContinuousDisplay: that call makes the frame's LAST
      // submission, so the fence value stamped here covers every batch that
      // could still reference a resource retired during this frame.
      stamp_releases();
      // Flip the descriptor heaps to the other half and drop the per-frame SRV/
      // sampler caches so next frame allocates fresh descriptors (a freed+recycled
      // resource can no longer alias a stale pointer-keyed SRV) without clobbering
      // this frame's still-in-flight half.
      g_binder.BeginFrame();
      ResetContinuousFrame(g_render_targets);
      // Arm the next frame's capture if the trigger file is present. std::remove
      // returns 0 only when it existed and was deleted, consuming it atomically.
      // Two producers, one consumer: the keybind (any thread, atomic flag) and
      // the trigger file kept for scripts. exchange/remove both consume once.
      const bool key_request = g_rdc_capture_request.exchange(false, std::memory_order_acq_rel);
      const bool file_request = std::remove("native_gfx_rdc_trigger") == 0;
      if (!rdc_capturing && !rdc_done && (key_request || file_request)) {
        if (RenderDocBeginCapture(g_draw_context.device())) {
          rdc_capturing = true;
          REXLOG_INFO("[native_gfx] RenderDoc capture armed for the next guest frame");
        } else {
          REXLOG_ERROR(
              "[native_gfx] RenderDoc capture could not be started (not running under RenderDoc, "
              "or a capture is already open)");
        }
      }
    } else {
      // Continuous mode with no frame submitted this boundary (the presenter is
      // not up yet). Nothing new references what was retired, so the last
      // submitted fence value is already a safe stamp.
      stamp_releases();
    }
    return;
  }
  stamp_releases();
  if (REXCVAR_GET(mcla_native_gfx_capture) > 0) {
    // Skip the first mcla_native_gfx_capture_delay frame boundaries before
    // arming, so the capture lands on a frame where the LZX texture streaming
    // has caught up and the scene is fully detailed (early frames show
    // low-detail placeholders and unconverged exposure -> a dark, muddy image).
    static uint32_t skipped = 0;
    const uint32_t delay = uint32_t(REXCVAR_GET(mcla_native_gfx_capture_delay));
    if (skipped < delay) {
      ++skipped;
      return;
    }
    ArmFrameCapture();
    SetFrameCaptureDumpTargets(REXCVAR_GET(mcla_native_gfx_dumprt));
  }
}

void RequestRenderDocCapture() {
  if (!RenderDocAvailable()) {
    REXLOG_ERROR("[native_gfx] RenderDoc is not attached to this process; cannot capture");
    return;
  }
  g_rdc_capture_request.store(true, std::memory_order_release);
  REXLOG_INFO("[native_gfx] RenderDoc capture requested");
}

// Continuous-mode present. Called from the guest swap hook AFTER the guest swap
// ran, so the command processor has already closed its submission (skipping it
// leaves that submission open and the GPU eventually TDRs) and painted its own,
// suppressed, guest output. Writing ours last makes the native frame the one
// the vsync worker shows.
//
// This drives rex::ui::Presenter directly instead of going through a renderer
// registered with the command processor: the whole point of that callback was
// to reach a blit implementation living inside the rexgpu-xenos plugin, and
// that blit is now recorded here on the runtime's own command list.
// Presents the display target the guest thread published, by driving
// rex::ui::Presenter directly.
//
// Presenter::RefreshGuestOutput is single-producer by design (it mutates
// guest_output_mailbox_writable_ and the properties array without
// synchronization; only the consumer handoff is atomic). So this may only run
// when the command processor is NOT also refreshing -- which is why continuous
// mode suppresses the guest swap instead of running after it.
bool PresentContinuousDisplay() {
  g_present_calls.fetch_add(1, std::memory_order_relaxed);
  if (!g_presenter || !g_draw_context.initialized()) {
    g_present_no_presenter.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  uint32_t fmt = 0, w = 0, h = 0;
  auto* display = static_cast<ID3D12Resource*>(GetContinuousDisplayResource(&fmt, &w, &h));
  if (display == nullptr || w == 0 || h == 0) {
    // Nothing published yet (the first frames, before the guest thread has
    // produced a display). Let the normal swap path run this frame.
    g_present_no_display.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  const bool ok = g_presenter->RefreshGuestOutput(
      w, h, w, h, [&](rex::ui::Presenter::GuestOutputRefreshContext& refresh) -> bool {
        auto& ctx =
            static_cast<rex::ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(refresh);
        ID3D12GraphicsCommandList* cl = g_draw_context.BeginFrame();
        if (cl == nullptr) {
          // BeginFrame returns null without logging when a frame is already
          // open. That is what made this failure invisible for three sessions.
          g_present_no_cmdlist.fetch_add(1, std::memory_order_relaxed);
          static bool logged = false;
          if (!logged) {
            logged = true;
            REXLOG_ERROR(
                "[native_gfx] continuous present: no command list (a frame is already open); "
                "the guest swap will paint instead");
          }
          return false;
        }
        if (!RecordExternalBlitToGuestOutput(
                g_draw_context.device(), cl, ctx.resource_uav_capable(), display, fmt, w, h,
                rex::ui::d3d12::D3D12Presenter::kGuestOutputInternalState)) {
          g_present_blit_false.fetch_add(1, std::memory_order_relaxed);
          g_draw_context.EndFrame();
          return false;
        }
        return g_draw_context.EndFrame();
      });
  if (ok) {
    g_present_ok.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_present_refresh_false.fetch_add(1, std::memory_order_relaxed);
  }
  return ok;
}

bool PresentTakeover() {
  return Active() &&
         (REXCVAR_GET(mcla_native_gfx_present) || REXCVAR_GET(mcla_native_gfx_continuous));
}

void NoteSwapHook() {
  g_swap_hook_calls.fetch_add(1, std::memory_order_relaxed);
  // Emulated path only. RenderDoc's own Present boundary holds nothing there
  // but the presenter blit -- two captures of a scene the native runtime draws
  // with ~1700 draws came back with 1 each -- because the game's work runs on
  // the command processor thread, outside those boundaries. Bracketing
  // swap-to-swap here puts a whole guest frame inside one capture, which is
  // what makes a draw-by-draw comparison against the native runtime possible.
  // Same trigger file as the native path, so the tooling does not change; the
  // native path has its own bracket at the frame boundary and is left alone.
  if (REXCVAR_GET(mcla_native_gfx)) {
    return;
  }
  static bool emu_capturing = false;
  if (emu_capturing) {
    emu_capturing = false;
    // Null device: the capture spans every device in the process, which is what
    // this needs -- the D3D12 device doing the work belongs to the command
    // processor, not to anything this runtime holds a pointer to.
    RenderDocEndCapture(nullptr);
    return;
  }
  const bool key_request = g_rdc_capture_request.exchange(false, std::memory_order_acq_rel);
  const bool file_request = std::remove("native_gfx_rdc_trigger") == 0;
  if ((key_request || file_request) && RenderDocBeginCapture(nullptr)) {
    emu_capturing = true;
  }
}

bool PresentFrame() {
  if (!PresentTakeover()) {
    g_present_no_takeover.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  // Continuous mode NEVER suppresses the guest swap, and that is the whole
  // point. The swap packet is what makes the command processor end its frame,
  // and ending the frame is what recycles its per-frame pools -- view and
  // sampler descriptor heaps, the constant-buffer pool, shared memory, the
  // render-target cache (D3D12CommandProcessor::EndSubmission(is_swap), which
  // calls EndFrame and ClearCache on each). Swallowing the swap call left that
  // frame open forever: measured at about a gigabyte of host private memory
  // per thousand frames, ending in "no free bindless view descriptors" and a
  // removed device a couple of minutes into gameplay.
  //
  // So the guest swap runs, the command processor closes its frame, and it
  // also presents its own image over the native one. That is the honest state
  // of the hybrid path with the SDK untouched, and the reason it is being
  // replaced rather than tuned.
  if (REXCVAR_GET(mcla_native_gfx_continuous)) {
    return false;
  }
  return g_triangle.Present(g_presenter);
}

}  // namespace mcla::native_gfx

// NOTE: the D3DDevice_Swap hook (D3DDevice_Swap) lives in hooks.cpp, the
// single owner of the guest graphics hooks, together with the draw, tiling
// and frame-boundary hooks shared with the passive probe.

#endif // REXGLUE_HAS_XEO3_TARGET
