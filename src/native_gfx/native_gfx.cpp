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

#include <rex/graphics/native_guest_renderer.h>
#include <rex/ui/d3d12/d3d12_presenter.h>

#include "d3d12/blit_pass.h"
#include "d3d12/presenter_output.h"
#include "d3d12/d3d12_smoke_triangle.h"
#include "d3d12/context.h"
#include "d3d12/first_draw.h"
#include "d3d12/frame_capture.h"
#include "d3d12/memory_census.h"
#include "guest/occlusion.h"
#include "guest/render_state.h"
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

REXCVAR_DEFINE_BOOL(mcla_native_gfx_texture_swizzle, false, "MCLA/NativeGfx",
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

// Registered with the command processor via SetNativeGuestOutputRenderer. Runs
// on the CP thread at swap. Blits the display target the guest thread published
// (PrepareContinuousDisplay) into the guest output through the SDK's external
// blit, which records on the CP's own command list. Returns true when it served
// the frame -- which is what keeps the emulated-draw suppression engaged.
bool NativeGuestOutputCallback(const rex::graphics::NativeGuestOutputRenderContext& ctx,
                               void* /*user*/) {
  uint32_t fmt = 0, w = 0, h = 0;
  auto* display = static_cast<ID3D12Resource*>(GetContinuousDisplayResource(&fmt, &w, &h));
  static int cb = 0;
  if (cb < 4) {
    REXLOG_INFO("[native_gfx] callback#{} display={}", cb++, (void*)display);
  }
  // TEMP DIAG (remove after): ground-truth whether the swap callback fires and
  // whether the guest thread ever published a display, since the logger is blind
  // on non-TTY stdout.
  {
    static unsigned total = 0, null_cnt = 0, ok_cnt = 0;
    ++total;
    if (display) ++ok_cnt; else ++null_cnt;
    if (total <= 10u || (total % 60u) == 0u) {
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        std::fprintf(f, "callback total=%u display_ok=%u display_null=%u last=%ux%u fmt=%u\n",
                     total, ok_cnt, null_cnt, w, h, fmt);
        std::fflush(f);
        std::fclose(f);
      }
    }
  }
  // Always claim the frame once continuous mode is on. Returning false here
  // falls through to the emulated gamma/FXAA blit -- but the command processor
  // already wrapped the guest output for us (NativeRhiBeginFrame), and that
  // emulated path then runs against a guest output in the wrong state and
  // faults. When there is no display yet (the first frames, before the guest
  // thread has published one) we simply present the untouched output.
  if (display) {
    // Through the RHI device: the blit implementation lives in the rexgpu-xenos
    // plugin, which this executable never links.
    ctx.device->BlitExternalToGuestOutput(ctx.guest_output, display, fmt, ctx.guest_output_width,
                                          ctx.guest_output_height);
  }
  return true;
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

    // Continuous mode registers a native guest-output renderer with the
    // command processor (Skate 3 model). At swap, the CP calls our callback to
    // paint the guest output, and suppresses the emulated draws we replace
    // (native_render_suppress_* cvars). One producer, in the CP's own frame --
    // no separate present, no race, no TDR.
    if (REXCVAR_GET(mcla_native_gfx_continuous)) {
      rex::graphics::SetNativeGuestOutputRenderer(&NativeGuestOutputCallback, nullptr);
      g_present_ready = true;
      SetContinuousMode(true);
      REXLOG_INFO("[native_gfx] continuous mode active (native guest-output renderer registered)");
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

void NotifyFrameBoundary() {
  // TEMP DIAG (remove after): is the frame-boundary hook firing, and do the
  // continuous gates pass?
  {
    static unsigned fb = 0;
    ++fb;
    if (fb <= 10u || (fb % 60u) == 0u) {
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        std::fprintf(f, "frameboundary#%u continuous=%d present_ready=%d draw_ready=%d\n", fb,
                     ContinuousMode() ? 1 : 0, g_present_ready ? 1 : 0, g_draw_ready ? 1 : 0);
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
      PrepareContinuousDisplay(g_draw_context, g_render_targets);
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

// Legacy no-op: continuous present moved into the command processor's native
// guest-output callback. Kept so the swap hook still links.
void PresentContinuousAtSwap() {}

bool PresentTakeover() { return Active() && REXCVAR_GET(mcla_native_gfx_present); }

bool PresentFrame() {
  if (!PresentTakeover()) {
    return false;
  }
  return g_triangle.Present(g_presenter);
}

}  // namespace mcla::native_gfx

// NOTE: the D3DDevice_Swap hook (D3DDevice_Swap) lives in hooks.cpp, the
// single owner of the guest graphics hooks, together with the draw, tiling
// and frame-boundary hooks shared with the passive probe.

#endif // REXGLUE_HAS_XEO3_TARGET
