#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>
#include "settings/immortal.h"

namespace larecomp::settings {

class NativeGfxSettings final : public rex::SettingOwner {
 public:
  NativeGfxSettings() : SettingOwner("larecomp") {}

  REX_SETTING(u32, mcla_exposure_log, 0, "TEMP DIAG: log the guest auto-exposure value this many times, one sample every "
    "120 front-end camera updates. Runs on a GUEST hook, so it works with the native "
    "runtime on OR off -- which is the point: the native path renders menu camera 53 "
    "a uniform 1.8x brighter than the emulated one, and the emulated exposure has "
    "never been read.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_exposure_log_addr, 0x02D6C000, "TEMP DIAG: physical guest address mcla_exposure_log reads (the 1x1 luminance "
    "target).", rex::kDebugOnly);

  // The cvars native_gfx.cpp defines and the guest-side files Plume shares read.
  REX_SETTING(bool, mcla_native_gfx, false, "The D3D12 native graphics runtime. Not built on this platform.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(std::string, mcla_native_gfx_ab_cvar, "", "Performance A/B within one session: the name of a boolean cvar to flip "
                      "every mcla_native_gfx_ab_frames frames. Each perfwin line in "
                      "native_gfx_diag.txt is tagged ab=0/1 with the state it ran under, and "
                      "settle=1 on the window right after a flip. Empty disables it.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_ab_frames, 240, "Frames between flips of mcla_native_gfx_ab_cvar; rounded to whole "
                      "60-frame report windows.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_alias_missed_resolve, false, "EXPERIMENT. A colour resolve with no matching pooled source is dropped; the emulated path "
    "cannot drop one, because it resolves out of EDRAM by address and always produces something. "
    "With this on, a missed colour resolve is aliased onto the most recent colour copy so the "
    "question 'does anything sample that destination' can be answered by looking at the screen. "
    "The content is deliberately wrong; this is a probe, not a fix.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_alpha_ref, true, "Feed SharedConstants.g_AlphaThreshold from RB_ALPHA_REF and RB_COLORCONTROL "
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
                    "now; the switch stays so the fixed 0.5 can be put back while bisecting.", rex::kDebugOnly);

  REX_SETTING(i32, mcla_native_gfx_aniso, -1, "Anisotropic filtering override, with the same numbering as the emulated path's\n"
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
    "filtered.", rex::kDebugOnly, rex::kRequiresRestart);

  REX_SETTING(bool, mcla_native_gfx_aniso_census, false, "TEMP DIAG: count texture binds by whether the sampler is eligible for "
                    "anisotropy and whether the texture actually has a mip chain to filter "
                    "down. Anisotropy picks a finer level along the major axis, so a "
                    "single-level texture makes a correct 16x sampler change nothing.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_auxcap, 8192, "Per-frame cap on AUXILIARY draws (shadow, light, reflection passes) the "
                      "native runtime records. 0 = uncapped. Was a hardcoded 2600, which "
                      "measurement showed saturating in dense scenes and silently dropping the "
                      "very lighting passes it bounded. A cap still exists because the documented "
                      "failure is aggregate GPU load exceeding the TDR window on weak hardware, "
                      "not one resource running out -- lower it if frame time regresses. The "
                      "shipped default should come from the measured peak (peak_aux in the prepare "
                      "log), not a guess; 8192 is a deliberately loose value for that measurement.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_auxstage, 0, "Continuous mode: automatic auxiliary-pass safety policy. "
                      "0 = stage 3 normally, but stage 2 on adapters with under 512 MiB "
                      "dedicated VRAM; display/UI passes are not suppressed by this policy. "
                      "1 = acquire auxiliary targets, 2 = bind and clear without non-display "
                      "auxiliary draws, 3 = draw all auxiliary passes.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_bind_memo, true, "Reuse the previous draw's texture binding when its 32 fetch constants are "
                    "byte-identical and nothing has moved under them. Texture binding measured at "
                    "22ms of a 95ms frame -- about 63000 slot binds per frame at 350ns each -- and "
                    "the per-slot work was already correct, so the only thing left to remove was "
                    "doing it twice. Measured in the MENU it does not pay: 15.7% hit rate, every "
                    "single miss a genuine key mismatch (guard invalidations: zero), and bind went "
                    "22.5ms -> 21.8ms, which is noise. That scene is 4200 mostly-distinct draws; "
                    "GAMEPLAY repeats far more (traffic is one model drawn many times) and has not "
                    "been measured yet. Kept behind this cvar for exactly that A/B -- turn it off "
                    "to get the unconditional rebind back.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_capture, 0, "Accumulate this many guest draws of the main scene pass into one native "
                      "render target and write mcla_native_gfx_frame.txt/.tga. 0 disables. "
                      "Supersedes mcla_native_gfx_firstdraw when non-zero.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_capture_delay, 0, "Frame boundaries to skip before arming the one-shot capture. The early "
                      "frames after boot show low-detail placeholder textures (LZX streaming has "
                      "not caught up) and an unconverged HDR exposure, giving a dark, muddy "
                      "image. Set e.g. 300 to capture a fully-streamed, well-exposed frame.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_census, 0, "Frame boundaries between memory census reports written to "
                      "native_gfx_mem.txt: process commit/working set, DXGI local (VRAM) and "
                      "non-local usage, and the live size of every native cache. The pool whose "
                      "delta tracks the process delta is the leak. 0 disables (default: each "
                      "report walks the whole address space with VirtualQuery, measured at 2.8% "
                      "of the render thread at 120).", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_clear_only_found, true, "Extend mcla_native_gfx_clear_only_resolve to a clear-only pass whose shape the pool "
    "already holds. MCLA clears its two 8x8 fake shadow collectors back to back at load "
    "(sub_823120C8): the FastCollector white, then the Collector 0xFF7F7F7F. The second "
    "resolve found the target the first one made and copied white. With shadows off "
    "(perf_no_shadows, by day) sub_82311450 binds the Collector as ShadowCollectorTex and "
    "every city material lights the sun with 2 * s, so the sunlit world came out twice as "
    "bright as the emulated path. Applied only when the pending clear was made on the "
    "surface being resolved. Off restores the old behaviour, for A/B.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_clear_only_resolve, true, "Satisfy a colour resolve whose source pass was only CLEARED, never drawn into. "
    "Such a pass never asks the pool for a target, so the resolve is dropped and the "
    "destination keeps whatever guest memory held -- zero. MCLA does this for its small "
    "shadow collectors (8x8 at 0x0329C000 and 0x0329D000, cleared white and 0xFF7F7F7F "
    "once at load), which the character eye material samples on its NIGHT path and multiplies "
    "into its light as saturate(-0.25 + s): reading zero paints every eye black after "
    "dark, while day worked because the day path samples the 256x256 collector that a "
    "real pass renders. Gated to a small surface whose fetch, viewport key and resolve "
    "rect all agree AND whose guest clear is still pending -- the draw path consumes a "
    "pending clear on a pass's first draw, so it surviving until the resolve is the "
    "no-draw signal. Turn off to get the dropped resolve back while bisecting.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_color_exp_bias, true, "Fold the bound render target's colour exponent bias back into "
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
    "Turn off to get the pre-fix behaviour back while bisecting.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_continuous, false, "EXPERIMENTAL gameplay mode: render every guest frame through the native "
                    "pipeline and present it, instead of the one-shot capture. Keeps all caches "
                    "across frames, so the HDR exposure adaptation converges. Does NOT suppress "
                    "the guest swap (avoids the command-processor TDR), so the guest still runs "
                    "underneath. Requires mcla_native_gfx.", rex::kDebugOnly, rex::kRequiresRestart);

  REX_SETTING(bool, mcla_native_gfx_decl_float, true, "Read the two 32-bit float vertex declaration types at their measured width.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_depth32, false, "Give a guest kD24S8 depth surface a D32_FLOAT_S8X24 host format.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_depth_reclear_infer, true, "Let an ATLAS-shaped full-source depth resolve arm the depth re-clear on its "
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
                    "guest's declared clear, which is the shape that showed the shadow change.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_diag, false, "Native graphics diagnostics. Not built on this platform.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_disable_occlusion, true, "Select the guest's no-occlusion path (byte_8288D5D1 = 0) while the native "
    "backend is active. "
    "MCLA gates a vehicle's body on an occlusion query: sub_82323808 keeps one "
    "slot per object, answers 'occluded' whenever a slot exists, and refreshes "
    "the very timestamp the reaper ages slots by -- so a slot survives only "
    "until the query completes and releases it. This backend has no command "
    "processor and cannot run the query, so the slot is refreshed forever and "
    "the body is never submitted again. Measured: the body block in "
    "sub_8235D500 is reached exactly once and never after, while the wheels, "
    "discs and exhaust keep drawing because their submit paths consult no "
    "query, and traffic shows only its contact shadow for the same reason. "
    "The game itself supports running with occlusion off (sub_82323808 returns "
    "'draw' immediately when this byte is zero), which is where a backend "
    "without query support belongs. Cost: geometry that is genuinely hidden is "
    "still drawn -- GPU time, never a missing object. "
    "Turn off only to reproduce the vanishing body, or once real queries exist.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_drawcap, 0, "Continuous mode: cap the number of native ANCHOR (main scene) draws "
                      "recorded per frame. 0 = uncapped. A low value (e.g. 300) tests whether "
                      "the DEVICE_HUNG TDR is aggregate GPU load rather than one bad draw: if "
                      "capping stops the crash, it is load. Also a direct performance lever.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_dump_draw_first, 0, "First per-frame draw number to dump to native_gfx_draws.txt. Same numbering as the skip "
    "range, so a range narrowed by bisection can be pasted straight in. Inert until "
    "mcla_native_gfx_dump_draw_last is non-zero.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_dump_draw_last, 0, "Last per-frame draw number to dump; 0 disables. Writes shader ids, primitive type, render "
    "target, every vertex stream's guest base and stride, and the world-view-projection -- the "
    "fields that have actually explained artefacts on this path. The file is appended to and grows "
    "fast: narrow the range with the skip cvars first.", rex::kDebugOnly);

  REX_SETTING(std::string, mcla_native_gfx_dump_ps, "", "Diagnostico: despeja TODO slot de textura ligado do draw cujo PIXEL "
                      "SHADER tem esta identidade, em hexadecimal (ex. "
                      "\"0x0F681CC854B766D4\", o motion blur). Sai como linhas PSSLOTS em "
                      "native_gfx_diag.txt, uma por combinacao distinta de slot/endereco/"
                      "formato/swizzle/origem. Existe porque a sonda DEPTHFETCH filtra por "
                      "IsRenderTargetSourcedFormat, e e justamente esse predicado que decide "
                      "se o fetch recebe a copia empacotada de profundidade+stencil ou a "
                      "superficie crua de dois planos: um fetch que ele rejeita nunca aparece "
                      "la. Vazio = nao despeja nada.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_dumprt, false, "Diagnostic: write every large resolve destination to a .tga at the end "
                    "of the capture. Counting a fetch as resolved only proves a resource was "
                    "handed back, not that it holds the right pixels. Costs a full GPU stall "
                    "per target, so it is off by default.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_enhanced_barriers, true, "Use D3D12 Enhanced Barriers (ID3D12GraphicsCommandList7) when supported by "
                    "hardware and OS to minimize GPU pipeline flushes and transition overhead.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_exp_bias_unit, false, "Neutraliza gInvColorExpBias para 1.0 em vez de multiplicar o valor "
                    "enviado pelo 2^bias do alvo. Diagnostico do mar estourado: no passe de "
                    "reflexo da agua o produto da 0.25 enquanto na cena da 1.0.", rex::kDebugOnly);

  REX_SETTING(f64, mcla_native_gfx_exposure, 8.0, "Exposure multiplier applied by the present-time tonemap (HDR anchor -> "
                      "LDR). The raw scene target is very dark (linear, unconverged exposure); "
                      "this lifts it before the ACES curve and gamma. Only used when "
                      "mcla_native_gfx_present_anchor is on.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_exposure_probe, 0, "TEMP DIAG: read back the auto-exposure targets (1x1 at 0x02D6C000 and "
                      "4x4 at 0x02D6D000) this many times, one every 200 published frames, as "
                      "raw floats. The native path renders cam 53 with its midtones +84 and "
                      "11.6% of the frame clipped white against the emulated path's 2.6%, with "
                      "matching shadows -- the shape of a gain before the tonemap.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_eye_probe, 0, "TEMP DIAG: report this many draws of the character eye/teeth material "
                      "(xCharacter_teeth_normalmap) with its tintColor constant and its bound "
                      "textures. Chasing eyes that render black in the native path and normal "
                      "in the emulated one.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_fetch_size_unit, 4, "Bytes per unit of the vertex fetch constant's 24-bit size field. 0 uses 4.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_firstdraw, false, "Execute ONE real MCLA draw through the native pipeline, into a dedicated "
                    "render target, and write mcla_native_gfx_firstdraw.txt/.tga. Does not "
                    "present and does not touch the guest render path, so it cannot fight the "
                    "Xenia command processor. Runs once per session.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_fxaa, false, "Run NVIDIA FXAA over the composite at present. The console had no "
                    "anti-aliasing to inherit here -- the game renders unresolved -- so this "
                    "is an addition, not a fidelity fix, and it is a whole-frame filter that "
                    "deserves its own A/B. It goes before the display gamma ramp, which is "
                    "where the console's own ramp sits (the DC_LUT is scanout, after "
                    "everything the GPU drew). Independent of mcla_native_gfx_msaa: MSAA "
                    "resolves geometry edges only, FXAA also catches shader and alpha-test "
                    "edges, and the two compose. Ignored on the present_anchor path, whose "
                    "source is the raw HDR scene target.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_fxaa_dump, 0, "TEMP DIAG: write this many pre/post TGA pairs of the FXAA pass "
                      "(native_gfx_fxaa_pre_N.tga / _post_N.tga). Both come from the same "
                      "frame and the same submission, so the difference between them is the "
                      "filter and nothing else.", rex::kDebugOnly);

  REX_SETTING(f64, mcla_native_gfx_fxaa_subpixel, 0.75, "How much of FXAA's sub-pixel term is allowed through. That term is what "
                      "handles thin features and lone pixels the edge walk cannot resolve -- "
                      "wires, railings, distant lamp posts -- and it is also the part that "
                      "blurs. 0 turns it off and keeps the image sharpest, 1 is the softest.", rex::kDebugOnly);

  REX_SETTING(f64, mcla_native_gfx_fxaa_threshold, 0.125, "FXAA edge threshold: the minimum local luma contrast, as a fraction of "
                      "the brighter luma, before a pixel is filtered at all. Lower catches "
                      "more edges and softens more of the frame; 0.333 is NVIDIA's fastest "
                      "preset, 0.125 the default quality one, 0.063 the most aggressive.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_gamma_ramp, true, "Apply the guest display gamma ramp at present, which is what the "
                    "emulated path does as the DC_LUT and the no-CP path never did.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_gen_mips, false, "Gerar cadeia de mip no host para textura de nivel unico, por box filter 2x2, "
    "restrito a 32 bits por texel sem compressao. Anisotropia so escolhe um nivel mais "
    "fino ao longo do eixo maior: sem cadeia, um sampler ANISOTROPIC 16x perfeito nao "
    "muda pixel nenhum. Medido no passe de cena: de 16.1M de binds >= 512x512, 12.7M sao "
    "kBaseMap e ZERO deles carrega cadeia -- o jogo marca base-map porque a textura tem "
    "um nivel so -- e 70% desses (9.0M) sao formato 6 (k_8_8_8_8), que e box filter na "
    "CPU sem precisar recomprimir BC. E MELHORIA, nao paridade: o 360 tambem aliasava "
    "nessas superficies, por isso o default e off.", rex::kDebugOnly, rex::kRequiresRestart);

  REX_SETTING(bool, mcla_native_gfx_geomdump, false, "Dump a per-draw geometry snapshot (streams, strides, input layout, index "
                    "buffer, slicing) built from guest state, to "
                    "mcla_native_gfx_geometry.txt. One entry per distinct draw shape.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_gpu_profiler, true, "Query GPU hardware timestamps via ID3D12QueryHeap to measure exact GPU frame "
                    "and pass execution durations.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_gpu_untile, true, "Use compute shader untiling and endian swapping on GPU for uploaded textures "
                    "to eliminate CPU decode overhead.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_guest_clear, true, "Start a render target at the colour the GUEST asked for (D3DDevice_Clear, "
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
                    "one surface would then be handed to whichever pass draws next.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_half_pixel, true, "Fill SharedConstants.g_HalfPixelOffset with (-1/viewport_w, +1/viewport_h). "
                    "23 of the 25 vertex shaders in a captured frame end with "
                    "SV_Position.xy += g_HalfPixelOffset.xy * w, the D3D9 -> D3D10+ pixel-centre "
                    "correction; left at zero every draw lands half a pixel off the texel grid, "
                    "which is where the soft 2D edges come from. On by default because the "
                    "shaders demand it; the switch exists so it can be taken out of the picture "
                    "while bisecting a colour or coverage regression.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_hangfind, false, "Diagnostic: in continuous mode, submit each recorded draw on its own and "
                    "wait for it with a timeout. The draw whose shader spins forever (the "
                    "DEVICE_HUNG TDR) is the one that times out; its vs/ps identity is written "
                    "to native_gfx_hang.txt. Extremely slow — a one-shot to name the culprit.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_inline_direct, false, "Copy every inline (BeginVertices) draw straight into the upload ring "
                    "instead of using BufferCache. Off is the default because the direct path "
                    "can stall MCLA; cached inline regions use exact per-block hashes on every "
                    "draw so same-frame command-buffer rewrites do not flicker the 2D/UI.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_inline_fenced, true, "Take rectangle-list inline vertices from the fence-protected per-slot "
                    "upload ring instead of the inline vertex ring, which continuous mode "
                    "rewinds at the start of every frame without waiting for the GPU. Off "
                    "restores the old ring, for A/B.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_keep_idle_display, true, "Fix: the second Rate My Ride photo (and any picture grabbed late) comes "
                    "out black. On the console the presented surface keeps its pixels from one "
                    "frame to the next unless the game clears it; here every frame started "
                    "from the runtime's stand-in clear. While a picture is taken the game stops "
                    "rendering the world and only draws a blit or a UI quad on top of the frozen "
                    "frame, so the frame it grabbed held nothing but that clear. With this on, "
                    "the presented R8G8B8A8 surface keeps its colour on a frame where no other "
                    "screen-sized pass has drawn and the guest asked for no clear. Depth is "
                    "cleared as before. Off for A/B.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_lock_dump_level0, false, "TEMP DIAG: when the CPU locks level 0 of a k_8_8_8_8 texture with a mip chain (the vinyl "
    "composite), untile what guest memory holds at that moment into "
    "lockdump_<renderer>_<address>_<size>_<n>.tga. Works in both renderers, so the emulated path "
    "is the reference the native write-back is compared against.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_lock_writeback, true, "Fix: photo mode and Rate My Ride pictures come out black. The game takes a picture on the "
    "CPU: it locks the front buffer, untiles it and JPEG-encodes it (sub_82178B20). On the "
    "console a resolve writes main memory; here the resolved image only exists as a GPU copy, so "
    "the CPU read zeros. When the guest locks a surface the GPU produced, this copies that "
    "resolve back into guest memory at that moment, in the guest's own layout. Also fixes car "
    "vinyls outside the Vinyl Editor: the vinyl compressor locks every mip level of the "
    "composite render target and DXT-compresses it on the CPU, so a surface with a mip chain "
    "gets its smaller levels as well, derived from level 0. Costs one GPU wait per lock of such "
    "a surface and nothing otherwise. Off for A/B.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_lock_writeback_dump, false, "TEMP DIAG for mcla_native_gfx_lock_writeback: after each write-back, read the guest memory "
    "back the way the photo grab does (untile, swap every dword, bytes 1..3 as R,G,B) and write "
    "it to native_gfx_lockwb_<address>.tga, plus the average colour in the log line.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_lowres_addr, 0x02DE6000, "TEMP DIAG: guest address mcla_native_gfx_lowres_dump reads.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_lowres_dump, 0, "TEMP DIAG: dump this many raw copies of the guest memory behind the "
                      "320x180 post-process buffer at 0x02DE6000, one every 300 published "
                      "frames. That buffer's resolve misses every frame (the key asks for "
                      "R8G8B8A8, the only 320x180 pass in the pool is the HDR one), so a fetch "
                      "of it reads whatever the title left in memory.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_lowres_h, 180, "TEMP DIAG: height for mcla_native_gfx_lowres_dump.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_lowres_w, 320, "TEMP DIAG: width for mcla_native_gfx_lowres_dump.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_mrt, 0x3F, "Second render target support, as a bitmask -- one bit per piece, so a fault can be\n"
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
    "constant -- a flat, uniformly lit canopy.", rex::kDebugOnly, rex::kRequiresRestart);

  REX_SETTING(u32, mcla_native_gfx_msaa, 0, "Multisample count for the HDR scene target: 2, 4 or 8. Anything else (including the "
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
    "same scene at 1x, with zero validation errors.", rex::kDebugOnly, rex::kRequiresRestart);

  REX_SETTING(bool, mcla_native_gfx_msaa_depth_cs, true, "Resolve a multisampled depth surface with the compute pass. Off restores "
                    "the shipped behaviour, an all-zero depth copy, which is what killed the "
                    "distance fog, the per-object contact shadow under vehicles and the depth of "
                    "field's circle of confusion at the same time. Diagnostic only.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_msaa_stencil, true, "With mcla_native_gfx_msaa on, resolve the scene depth's stencil plane too "
                    "(sample 0, as the emulated resolve does for depth) into the resolved copy. "
                    "The depth goes through a single-plane compute resolve, so without this the "
                    "copy's stencil stayed zero and the car motion blur took vehicle index 0 "
                    "for every pixel, blurring the player's car with the world. Off restores "
                    "the old behaviour, for A/B.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_nocp_vsync, false, "Frame pacing for the no-command-processor mode: on = the guest video mode's refresh rate, "
    "off = 1000 Hz, matching what the emulated path does with vsync off. "
    "This exists because the SDK's `vsync` cvar is defined in "
    "src/graphics/command_processor.cpp, so it lives in the GPU plugin and is simply ABSENT once "
    "that plugin is not loaded -- a `vsync` line in larecomp.toml is an unknown key in this mode, "
    "and querying it by name would silently answer false whatever it was set to.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_overlap_index, true, "Find the geometry regions a guest write touches with a walk that stays "
                    "correct when regions overlap (declined merges leave overlaps). The old "
                    "binary search assumed disjoint regions and could skip a large region "
                    "containing the write, losing the invalidation. Off restores it, for A/B.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_own_device, false, "Use native DeviceManager to own the D3D12 device and direct command queue "
                    "instead of borrowing from RexGlue/Xenia provider. Enables high-performance "
                    "adapter selection, Agility SDK features, and drops the shared submit mutex. "
                    "Off by default, like mcla_native_gfx_own_swapchain: without that one the "
                    "presenter shows the frame from its own queue, which nothing orders after the "
                    "blit on this one.", rex::kDebugOnly, rex::kRequiresRestart);

  REX_SETTING(u32, mcla_native_gfx_own_heap_kb, 0, "Size of the guest-addressable block the runtime allocates its D3D objects out of, in KiB. "
    "0 uses the built-in default (4096). Rounded down to a power of two -- o1heap only uses the "
    "largest power-of-two span of its arena. "
    "Taken from the guest VIRTUAL window, never from a physical one: the physical windows are "
    "views of the console's 512 MiB of RAM, and taking 32 MiB of it killed the title during "
    "loading with 'PhysicalHeap::Alloc unable to alloc physical memory in parent heap'. "
    "Raise this if the exhaustion report fires.", rex::kDebugOnly, rex::kRequiresRestart);

  REX_SETTING(bool, mcla_native_gfx_own_swapchain, false, "Use NativeSwapChain (FLIP_DISCARD, tearing, frame latency waitable object) "
                    "presenting directly to the HWND backbuffer, eliminating intermediate blits "
                    "and enabling full ReShade / Reno DX DevKit visibility. Off by default: it "
                    "bypasses rex::ui::Presenter, so present_effect (FSR) and every ImGui overlay "
                    "(console, F3, toasts) are gone and the frame is stretched bilinearly. Needs "
                    "mcla_native_gfx_own_device.", rex::kDebugOnly, rex::kRequiresRestart);

  REX_SETTING(u32, mcla_native_gfx_own_textures, 0, "Resource ownership, step two: who allocates the 52-byte D3DTexture header. "
    "0 = the guest, as always. "
    "1 = mirror and validate: the runtime allocates the header out of its own guest-visible "
    "heap, copies the guest's bytes in, checks the guest VA translates back to the same bytes, "
    "then frees it. Nothing the game can observe changes -- this is the proof that objects the "
    "runtime allocates are addressable from guest code. "
    "2 = hand over: grcTextureXenon+16/+28 are repointed at the runtime's copy, so every guest "
    "read of that texture header reads runtime memory. The guest's original block is left "
    "untouched and restored at destruction, so its allocator only frees what it allocated. "
    "Covers only textures born in grcTextureXenon::Init; streamed ones are deserialized with "
    "the header embedded in the resource and stay with the guest.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_pack_depth_stencil, true, "Monta por compute, em cada resolve de profundidade de resolucao "
                    "cheia, duas copias dos dois planos: R32G32_FLOAT (profundidade em R, "
                    "stencil/256 em G) para um fetch k_24_8, e R8G8B8A8_UNORM na ordem de "
                    "componentes que o fetch do Xenos entrega para um fetch k_8_8_8_8 do "
                    "mesmo resolve. E pela segunda que o motion blur da MCLA le o id de "
                    "veiculo que a cena escreveu no stencil; sem ela o carro do jogador e "
                    "reprojetado como cenario estatico e borra. Custa duas alocacoes do "
                    "tamanho do alvo por endereco de resolve. Desligar so para bissectar.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_partial_reupload, true, "Re-send only the 4 KiB blocks of a geometry region that the page write "
                    "watch or a guest unlock reported as written, instead of the whole region. "
                    "Those ranges are exact to the page; measured in gameplay they add up to "
                    "~2 MB a frame while ~18 MB were being re-sent. Dirt found by the sampled "
                    "hash, which only reads part of each block, still re-sends the whole "
                    "region. Off restores whole-region re-uploads, for A/B.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_pipeline_library, true, "Persist compiled D3D12 pipeline state objects (PSOs) to disk using "
                    "ID3D12PipelineLibrary to eliminate runtime shader compilation stutter.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_pix_markers, true, "Emit BeginEvent/EndEvent and SetMarker on D3D12 graphics command lists for "
                    "RenderDoc, PIX, and NSight pass hierarchy inspection.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_present, false, "Native presentation takeover (smoke-test stage): suppress the guest swap "
                    "and present the native frame instead. MUST stay off while the Xenia command "
                    "processor is still consuming the game's draws: without swaps its D3D12 "
                    "submission never closes and the GPU eventually TDRs (observed DEVICE_HUNG). "
                    "It becomes the default path once the native runtime executes the draws "
                    "itself and the guest command stream is empty.", rex::kDebugOnly, rex::kRequiresRestart);

  REX_SETTING(bool, mcla_native_gfx_present_anchor, false, "Continuous mode: present the anchor (raw scene target) instead of the "
                    "readback (composite) pass. The continuous readback comes out flat/black "
                    "while the anchor holds the real rendered scene, so this makes the native "
                    "world visible while the composite-selection bug is still open.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_ps_census, 0, "TEMP DIAG: every distinct pixel shader the scene runs -- draw count, the "
                      "vertex declaration's COLOR0 format and offset, and the first bound "
                      "textures -- dumped every N draws. The runtime's shader identity is NOT "
                      "the identity column of shader_report.tsv; translate these with "
                      "build_shader_pack.collect_shaders(). This is what NAMES an unknown "
                      "material: run the scene that shows the bug and read the list.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_reclear, true, "Honour a guest clear that arrives after the target was already cleared "
                    "this frame, restricted to the pass's viewport. One pooled target is "
                    "shared by every pass of the same shape, so without this the second pass "
                    "of a frame inherits the first one's pixels: the foliage impostor atlases "
                    "came back with other species smeared over the background, which put every "
                    "texel above the shadow shader's 10/255 cut and made every tree shadow a "
                    "square.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_reclear_depth, true, "Extend mcla_native_gfx_reclear to a guest clear of depth/stencil ONLY "
                    "(no colour), applied when the draw that consumes it is still on the "
                    "surfaces the guest cleared. The UI clears depth+stencil right before its "
                    "first 3D object (sub_82722170); dropped, the 3D menu cards were depth "
                    "tested against whatever an earlier pass of the same shape left in the "
                    "pooled depth buffer -- the Rate My Ride leaderboard lost everything below "
                    "the garage floor's depth. Off restores colour-only re-clears, for A/B.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_reclear_depth_probe, 0, "TEMP DIAG: log depth/stencil-only guest clears that reach an "
                      "already-cleared target (applied or not, and why) to native_gfx_diag.txt, "
                      "one line per distinct case (target shape, shader, flags, decision) up to "
                      "N cases, repeated at counts 10, 100, 1000... 0 = off.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_reclear_probe, false, "TEMP DIAG: log which path armed needs_depth_reclear and on what surface "
                    "shape. Two places arm the same flag -- the shape inference in "
                    "RenderTargetPool::NoteResolve and the guest's own clear request in "
                    "NotifyResolve -- and once it is set they are indistinguishable, so "
                    "attributing a wrong re-clear needs this.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_record_replay, false, "Record each submission's D3D12 commands into a buffer and replay them onto "
                    "the real command list at submit time, instead of issuing them as they are "
                    "made. The first step towards a submission thread: every cache and state "
                    "decision stays where it is, only the driver calls move. Off issues them "
                    "directly, for A/B.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_rectlist_nocull, true, "Disable face culling for kRectangleList draws. The Xenos rect primitive "
                    "describes an AREA and generates its own triangles; synthesising the fourth "
                    "corner on the CPU gives them a winding the guest never chose. Measured on "
                    "the minimap: the circular punch came out clockwise on a Y-down target "
                    "against cull_back with CCW-front, so every fragment was culled and the map "
                    "kept its square corners. Off restores the old behaviour, for A/B.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_region_kb, 128, "Ceiling on a merged geometry region, in KiB. 0 uses the built-in "
                      "default. The buffer cache merges a request with every region it "
                      "overlaps, and that union only grows -- one request bridging two regions "
                      "swallows everything between them, which makes the result overlap the "
                      "next request, and so on. A region is re-uploaded whole whenever any byte "
                      "in it is dirtied, so runaway merging turns small dynamic writes into "
                      "tens of MiB of re-upload per frame. Lower this if the exhaustion report "
                      "still shows geometry filling the ring.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_region_memo, true, "Remember the geometry region each address resolved to, so a draw does "
                    "not walk the region map again for a buffer the draw before it already "
                    "used. The map holds several thousand regions and is searched twice per "
                    "draw, for the vertex stream and the index buffer. Entries are dropped "
                    "whenever a region is created, merged or erased, and the remembered "
                    "extent is re-checked against each request. Off restores the map walk, "
                    "for A/B.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_resolve_cpu_writes, true, "Fix: the photo preview (and anything else the CPU writes over a resolve destination) shows "
    "an old GPU image. MCLA puts the photo preview texture at 0x06ACD000, the memory the motion "
    "blur's depth resolve uses, and the render-target bridge kept answering that address with "
    "the depth copy -- a near-black picture with the scene's outline. With this on, the SDK's "
    "write watch covers every resolve destination, and a CPU write there makes fetches decode "
    "guest memory until the guest resolves to it again. Off for A/B.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_resolve_shape_fallback, true, "When a colour resolve misses, retry the pool by SHAPE alone (same size, same "
    "depth format, same sample count) instead of also demanding the colour format the "
    "resolve-time registers name. MCLA has two 320x180 post-process passes, one HDR and "
    "one LDR, and the LDR resolve at 0x02DE6000 misses every frame against a pool that "
    "only ever rendered the HDR one -- so the tonemap samples a BLACK 320x180 where the "
    "emulated path has real data. Measured on menu camera 53: the native frame comes out "
    "a uniform 1.8x brighter than the emulated one and clips 11.6% of the screen against "
    "2.6%, with the HDR SCENE target identical between the paths (0.0312 against 0.0327), "
    "so the gain enters at this stage. Turn off to get the dropped resolve back.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_resolve_variants, true, "Keep a resolve destination's copy when the guest resolves a different shape "
                    "to the same address, and reuse it when that shape comes back, instead of "
                    "destroying and recreating it. The guest alternates shapes at one address "
                    "and the recreation ran about 25 times a second (a 1280x720 copy plus its "
                    "packed depth+stencil twin), ~8% of the render thread in driver allocate/free. "
                    "Lookups are unchanged: they see the most recent resolve at each address. "
                    "Off restores destroy-and-recreate, for A/B.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_rim_probe, 0, "TEMP DIAG: report this many xRimMain draws -- the vertex declaration, "
                      "TEXCOORD0 of the first vertices, and VS constants 206..215 (tintColors). "
                      "The wheel picks its paint slot with a0 = trunc(TEXCOORD0.z), so a wrong "
                      "third component reads a neighbouring part's colour.", rex::kDebugOnly);

  REX_SETTING(i32, mcla_native_gfx_shadow_bias, 16, "Depth bias, in D24 quanta, added to the 640x640 shadow pass. Measured "
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
                     "restores the unbiased behaviour.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_shadow_quads, 0, "TEMP DIAG: check this many times whether the four 640x640 quadrants of "
                      "the resolved shadow atlas (0x06E65000) are distinct, one check every 180 "
                      "published frames. Two identical quadrants mean a cascade inherited the "
                      "previous cascade depth and rejected every fragment -- the 'sun only "
                      "inside a box' failure, which on the GPS map (the 3D city at low LOD) "
                      "reads as a whole map with no sun. Reads the RESOLVED copy: a pooled "
                      "1280x1280 hands back its clear colour through recycling and would read "
                      "as a false negative.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_skip_draw_first, 0, "Bisection: first per-frame draw number to skip. The number is the draw's position within the "
    "frame, the same counter the report calls `offered`, and it restarts every frame. Inert until "
    "mcla_native_gfx_skip_draw_last is non-zero.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_skip_draw_last, 0, "Bisection: last per-frame draw number to skip; 0 disables skipping entirely. "
    "Set a range, look at the screen, halve the range that still shows the artefact. Eleven runs "
    "isolate one draw out of two thousand, and unlike a hypothesis it cannot be wrong -- which is "
    "why it exists after nine guesses failed on the black shards.", rex::kDebugOnly);

  REX_SETTING(std::string, mcla_native_gfx_skip_ps, "", "Diagnostico: pula todo draw cujo PIXEL SHADER tem esta identidade, "
                      "em hexadecimal (ex. \"0xF43F1D5258D0F6EF\"). Vazio = nao pula nada. "
                      "A bisseccao por numero de draw (skip_draw_first/last) reindexa a cada "
                      "frame e nao serve para um passe de pos-processamento, que aparece uma "
                      "vez por frame em posicao variavel; a identidade do shader nao muda. "
                      "O ps_id sai no log em DEPTHFETCH e nos dumps de draw.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_skip_punch, false, "Diagnostic: drop the minimap's circular punch draw. On, the 220x220 "
                    "target should keep its square corners; if it looks identical with the "
                    "punch on, the punch is changing no pixel at all.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_skip_water, 0, "Diagnostico: pula draws de agua por familia, para saber qual pinta a "
                      "faixa branca. Bitmask: 1 = xCityOceanShore, 2 = xCityOceanWater, "
                      "4 = xCityOceanWaterLOD, 8 = xCityPondWater.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_slot_cache, true, "Cache each texture SLOT bind for the rest of the frame, keyed by its six "
                    "raw fetch-constant dwords: a hit hands back the descriptor indices without "
                    "the texture cache, the render-target bridge or the descriptor maps. Slots "
                    "repeat across draws far more than whole draws do (the shadow atlas, light "
                    "grids and LUTs are bound by nearly every material draw), which is what the "
                    "one-draw memo above cannot exploit. Emptied at every frame and whenever the "
                    "texture cache or render-target pool changes what backs an address; queued "
                    "invalidations are drained before each draw's lookups. Off restores the "
                    "resolve on every slot, for A/B.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_state_cache, true, "Send a draw only the pipeline state that changed since the last draw on the "
                    "same command list: descriptor heaps, root signature, the four descriptor "
                    "tables, PSO, stencil ref, blend factor, the three constant-buffer views, "
                    "render targets, viewport, scissor, topology and the vertex/index buffers. "
                    "Every draw used to re-set all of it, about nineteen driver calls, and "
                    "consecutive draws share nearly all of them; the user-mode driver was 20% of "
                    "the render thread. The copy is dropped whenever the list is reset or another "
                    "pass (a compute pass, a blit) records its own state onto it. Off restores the "
                    "full set per draw, for A/B.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_stencil_probe, 0, "Diagnostic. Every N full-size depth+stencil packs, read back the stencil "
                      "channel of the depth-as-8888 copy the car motion blur samples and log "
                      "STENCILPROBE to native_gfx_diag.txt: share of pixels with a non-zero "
                      "vehicle id and the ids seen. 0 disables it.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_streaming_demote_frames, 30, "With mcla_native_gfx_streaming_sticky on, for fully hashed regions: clean frames (frames in which "
                      "the region was drawn and its hashed blocks matched) before a streaming "
                      "region goes back under the write watch. 0 never puts it back, which "
                      "leaves every rewrite to the full hash. Sampled-hash geometry always demotes "
                      "after four clean frames regardless of this value.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_streaming_frames, 3, "Consecutive frames of being written to after which a geometry region "
                      "comes off the page write watch, or 0 to keep every region watched. The "
                      "watch costs an access violation per guest write, taken through the memory "
                      "system's global lock, plus a VirtualProtect over three heaps to re-arm "
                      "after every upload -- measured at ~300 faults and ~360 re-uploads a "
                      "frame, about 9% of the render thread, to report writes the next frame "
                      "was going to re-upload anyway. An unwatched region is checked by the "
                      "sampled block hash instead, once a frame over the blocks a draw reads, "
                      "so unchanged bytes still cost no upload; four unchanged frames put it "
                      "back under the watch.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_streaming_sticky, false, "Hysteresis for fully hashed regions only. Sampled-hash geometry keeps the "
                    "normal four-frame demotion so old configurations cannot leave it permanently "
                    "outside the write watch. Promotion counts an exact-hash region "
                    "written every other frame as a streak (a double-buffered dynamic buffer "
                    "never had two consecutive dirty frames, so it stayed watched and paid a "
                    "fault, a VirtualProtect and a re-upload on every write), and demotion "
                    "waits mcla_native_gfx_streaming_demote_frames clean frames instead of four. "
                    "Measured while driving before this existed: ~25 promotions and ~24 "
                    "demotions a frame, each demotion re-arming the watch and re-sending the "
                    "whole region, and ~690 watch faults a frame. Measured on: promotions and "
                    "demotions -85%, re-uploads and watch faults -29%, but no frame time gain "
                    "above noise -- and with it on for a whole session stale meshes showed up "
                    "(stretched triangles across the screen, HUD pieces missing): more regions "
                    "off the watch means more rewrites left to the sampled hash, which misses "
                    "them. Sampled-hash regions therefore ignore this setting.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_submit_thread, false, "Hand each recorded submission to a dedicated thread that replays it onto "
                    "the D3D12 command list and submits it, so the driver calls, Close and "
                    "ExecuteCommandLists leave the guest's render thread. Implies "
                    "mcla_native_gfx_record_replay. Off submits on the render thread, for A/B.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_surface_key, false, "Include what the guest asked for -- its requested sample count and surface pitch -- in the "
    "render target pool's key, so two guest surfaces of the same shape stop sharing one pooled "
    "target.\n"
    "\n"
    "OFF BY DEFAULT BECAUSE IT MEASURED WORSE. The HUD is drawn spread across TWO 1280x720 "
    "R8G8B8A8 surfaces, and merging them by shape is what lands all of it in the target that "
    "gets displayed -- with this on, the whole 2D layer disappears, HUD included. The merge is "
    "load-bearing: it stands in for the composition the native runtime deliberately does not "
    "model. Kept as a bisection switch, not as a fix.", rex::kDebugOnly, rex::kRequiresRestart);

  REX_SETTING(bool, mcla_native_gfx_swapped_texcoords, true, "Fill SharedConstants.g_SwappedTexcoords from the vertex declaration. A RAGE "
                    "vertex packs a 32-bit POSITION and a 16-bit TEXCOORD in one stream, but a "
                    "stream can only be byte-swapped at one width; swapped as 8in32 the 16-bit "
                    "pair arrives as (v, u) and every texture renders transposed, which looks "
                    "like a 90-degree rotation to the left. The translated shaders already carry "
                    "the fix (tfetchTexcoord -> value.yxwz) and only needed the mask. On by "
                    "default; the switch exists to take it back out while bisecting.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_swizzle_by_source, true, "Decide whether a k_8_8_8_8 fetch's 0x60A swizzle still applies by where the "
                    "texture came from instead of by its tiling. A render target served by the "
                    "bridge is already in host RGBA order, so the guest's swizzle is cancelled; "
                    "anything decoded from guest memory is in the guest's own layout and gets it, "
                    "tiled or not. Off: the old rule (tiled = cancelled), which swaps red and blue "
                    "on the photo album thumbnails and in the car paint's flake noise.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_telemetry, false, "Collect resource-usage telemetry (bound buffers, index buffers, texture "
                    "fetch constants) while the native graphics runtime is active. Written to "
                    "mcla_native_gfx_resources_*.csv. Off by default: it takes a mutex and two "
                    "map lookups per fetch slot on every draw.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_tex_cached_decode, true, "Untile and endian-swap texture levels in ordinary cached memory and copy "
                    "the result to the upload ring in one pass, instead of swapping in place "
                    "inside the ring. The ring is write-combined, so the in-place swap read "
                    "uncached memory: copy_and_swap_16 was 16% of the render thread while "
                    "driving. The uploaded bytes are identical. Off restores the old path, "
                    "for A/B.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_texcache_mb, 2048, "Byte budget (MiB) for the native texture cache. The cache is keyed by the "
                      "fetch constant, so a streaming game keeps producing new keys as it pages "
                      "geometry and textures in and out; without a budget every one of them stays "
                      "resident for the session and the footprint grows without bound. Over the "
                      "budget, least-recently-used entries are released (fence-gated, never one "
                      "the current frame bound). 0 = unbounded, the old behaviour.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_texdump, false, "Decode the first textures the game binds straight out of guest memory "
                    "(fetch constant -> format -> untile) and write them to "
                    "mcla_native_gfx_textures/ for offline inspection. This is the correctness "
                    "test for the texture decoder against real game data; off by default "
                    "because it writes tens of MB.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_texinv_incremental, true, "Keep the texture invalidation index sorted by insertion instead of "
                    "re-sorting all of it after every new texture, and leave removed entries "
                    "in it until they outnumber the live ones. While driving it was being "
                    "rebuilt several times a frame, ~3.4% of the render thread. Off restores "
                    "the rebuild-on-change, for A/B.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_texinv_index, true, "Apply queued texture invalidations through an index of the cache entries "
                    "sorted by guest address, instead of walking every entry for every queued "
                    "range. The write watch hands the texture cache every fault in the process, "
                    "about 600 ranges a frame in gameplay against ~970 entries, and the walk "
                    "measured 16.9% of the render thread. Same entries dropped either way; off "
                    "restores the walk, for A/B.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_texture_registry, false, "Record every texture the game creates: name, dimensions, format, and "
                    "which road built it (a header placed over streamed resource memory, or a "
                    "real D3DDevice_CreateTexture). Observation only -- the guest still owns "
                    "and creates everything, the hook just reads the result. Answers what the "
                    "address-keyed texture cache cannot: which asset a given guest address is. "
                    "Overwrites mcla_native_gfx_textures.txt every 600 frame boundaries, so "
                    "the file always reflects the current state rather than the boot state.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_texture_swizzle, true, "Apply the fetch constant's 12-bit swizzle to the SRV's "
                    "Shader4ComponentMapping instead of the D3D12 default. The two encodings "
                    "match one-for-one (0..3 component, 4 = zero, 5 = one), and 34 of MCLA's "
                    "k_8_8_8_8 textures -- the colour-grading LUTs bound on every draw -- carry "
                    "0x60A = (Z,Y,X,W), so in principle the default mapping swaps red and blue. "
                    "In practice turning it on tints the sky dome red, which means the untiler's "
                    "8-in-32 endian swap already puts the channels in host order for at least "
                    "some of them and the swizzle is then applied twice. OFF until a per-format "
                    "host swizzle (the piece Xenia composes with the guest one) is worked out.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_unlock_invalidate, false, "Invalidate cached textures from the guest's own unlock instead of waiting for the page "
    "write watch to fault. D3DResource_Unlock flushes a dirty range the resource has been "
    "accumulating (BaseFlush at +0x14, MipFlush at +0x18, packed 16.16 in 128-byte units), and "
    "reading it costs two loads. Exact bytes instead of whole pages, no page-protection fault, "
    "and it fires when the guest considers the data final rather than on the next touch. "
    "Does NOT replace the write watch: writes that never go through a lock (streaming straight "
    "into resource memory) are only caught by the watch. Off by default until the counters show "
    "the ranges are sane -- see the lock line in the periodic report.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_unsupplied_drop, false, "Bisect switch: drop a draw whose shader declares a vertex attribute the vertex declaration "
    "does not supply, instead of binding a zero stream for it. "
    "The zero stream is the normal behaviour and the better one -- dropping loses real geometry, "
    "measured at 9243 display-shaped draws gutted per report when this was the default. But the "
    "zero stream rests on the assumption that an unpatched vfetch reads zeros on hardware, and it "
    "is one of the few things that can produce vertices collapsed toward the origin: a shader "
    "blending POSITION0 with a zeroed POSITION1 gives exactly the long stretched triangles seen "
    "across the scene. Turn this on for one run: if an artefact disappears, the zero stream is "
    "producing it; if it stays, this branch is eliminated.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_native_gfx_upload_mb, 64, "Transient upload ring per in-flight frame, in MiB. 0 uses the built-in "
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
                      "less than 4 MiB. Measured need was 31.7 MiB in use plus the 4 MiB request.", rex::kDebugOnly, rex::kRequiresRestart);

  REX_SETTING(bool, mcla_native_gfx_vblank_probe, false, "Read-only probe of the guest's vblank and flip machinery, for the port to a runtime with no "
    "command processor. Reports the vblank counter, the flip counter and the pending-flip queue "
    "indices in the D3DDevice, so the offsets the replacement will drive are proven while the "
    "emulated path still works. Writes nothing.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_verify_per_frame, true, "Re-check a cached texture against guest memory once per guest frame, as "
                    "mcla_native_gfx_verify_textures describes, instead of once per submitted "
                    "batch. The batch counter advances ~14 times a frame, so every bound "
                    "texture was being re-hashed that many times. Off restores the per-batch "
                    "check, for A/B.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_verify_regions, true, "Re-check a clean buffer region against guest memory once per frame "
                    "before binding it, and re-upload when they differ. The write watch "
                    "loses notifications: measured on MCLA, five xPed vertex regions held "
                    "bytes that differed from guest memory for 200 consecutive observations "
                    "each while still marked clean, which is what draws a pedestrian with "
                    "the previous mesh's vertices. Off restores the old behaviour, for A/B; "
                    "the cost is one hash of each region something actually binds, once a "
                    "frame.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_verify_textures, true, "Re-check a cached TEXTURE against the guest memory it was decoded from, "
                    "once per entry per frame, and drop it when they differ. Same lost-invalidation "
                    "hole as the buffer cache: measured, the minimap's circular mask entry at "
                    "0x02D64000 held an unrelated texture for the whole session while guest "
                    "memory still held the circle, so the punch sampled a near-constant mask "
                    "and the map kept its square corners. Off restores the old behaviour, for "
                    "A/B.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_vinyl_diag, false, "TEMP DIAG: log every resolve into a texture with a mip chain (rage GenerateMipMaps "
    "targets, the vinyl composite among them) with its DestLevel, source lookup and the "
    "capture's draw tallies, and every lock of such a texture with its level.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_native_gfx_watch_before_copy, true, "Re-arm a geometry region's page write watch before copying its bytes "
                    "instead of after. Armed after, a write another guest thread makes between "
                    "the copy and the re-arm raises no fault and leaves stale bytes in a region "
                    "marked clean. Off restores the old order, for A/B.", rex::kDebugOnly);

};

inline NativeGfxSettings& g_nativegfx = larecomp::Leak<NativeGfxSettings>();

inline auto& mcla_exposure_log = g_nativegfx.mcla_exposure_log;
inline auto& mcla_exposure_log_addr = g_nativegfx.mcla_exposure_log_addr;
inline auto& mcla_native_gfx = g_nativegfx.mcla_native_gfx;
inline auto& mcla_native_gfx_ab_cvar = g_nativegfx.mcla_native_gfx_ab_cvar;
inline auto& mcla_native_gfx_ab_frames = g_nativegfx.mcla_native_gfx_ab_frames;
inline auto& mcla_native_gfx_alias_missed_resolve = g_nativegfx.mcla_native_gfx_alias_missed_resolve;
inline auto& mcla_native_gfx_alpha_ref = g_nativegfx.mcla_native_gfx_alpha_ref;
inline auto& mcla_native_gfx_aniso = g_nativegfx.mcla_native_gfx_aniso;
inline auto& mcla_native_gfx_aniso_census = g_nativegfx.mcla_native_gfx_aniso_census;
inline auto& mcla_native_gfx_auxcap = g_nativegfx.mcla_native_gfx_auxcap;
inline auto& mcla_native_gfx_auxstage = g_nativegfx.mcla_native_gfx_auxstage;
inline auto& mcla_native_gfx_bind_memo = g_nativegfx.mcla_native_gfx_bind_memo;
inline auto& mcla_native_gfx_capture = g_nativegfx.mcla_native_gfx_capture;
inline auto& mcla_native_gfx_capture_delay = g_nativegfx.mcla_native_gfx_capture_delay;
inline auto& mcla_native_gfx_census = g_nativegfx.mcla_native_gfx_census;
inline auto& mcla_native_gfx_clear_only_found = g_nativegfx.mcla_native_gfx_clear_only_found;
inline auto& mcla_native_gfx_clear_only_resolve = g_nativegfx.mcla_native_gfx_clear_only_resolve;
inline auto& mcla_native_gfx_color_exp_bias = g_nativegfx.mcla_native_gfx_color_exp_bias;
inline auto& mcla_native_gfx_continuous = g_nativegfx.mcla_native_gfx_continuous;
inline auto& mcla_native_gfx_decl_float = g_nativegfx.mcla_native_gfx_decl_float;
inline auto& mcla_native_gfx_depth32 = g_nativegfx.mcla_native_gfx_depth32;
inline auto& mcla_native_gfx_depth_reclear_infer = g_nativegfx.mcla_native_gfx_depth_reclear_infer;
inline auto& mcla_native_gfx_diag = g_nativegfx.mcla_native_gfx_diag;
inline auto& mcla_native_gfx_disable_occlusion = g_nativegfx.mcla_native_gfx_disable_occlusion;
inline auto& mcla_native_gfx_drawcap = g_nativegfx.mcla_native_gfx_drawcap;
inline auto& mcla_native_gfx_dump_draw_first = g_nativegfx.mcla_native_gfx_dump_draw_first;
inline auto& mcla_native_gfx_dump_draw_last = g_nativegfx.mcla_native_gfx_dump_draw_last;
inline auto& mcla_native_gfx_dump_ps = g_nativegfx.mcla_native_gfx_dump_ps;
inline auto& mcla_native_gfx_dumprt = g_nativegfx.mcla_native_gfx_dumprt;
inline auto& mcla_native_gfx_enhanced_barriers = g_nativegfx.mcla_native_gfx_enhanced_barriers;
inline auto& mcla_native_gfx_exp_bias_unit = g_nativegfx.mcla_native_gfx_exp_bias_unit;
inline auto& mcla_native_gfx_exposure = g_nativegfx.mcla_native_gfx_exposure;
inline auto& mcla_native_gfx_exposure_probe = g_nativegfx.mcla_native_gfx_exposure_probe;
inline auto& mcla_native_gfx_eye_probe = g_nativegfx.mcla_native_gfx_eye_probe;
inline auto& mcla_native_gfx_fetch_size_unit = g_nativegfx.mcla_native_gfx_fetch_size_unit;
inline auto& mcla_native_gfx_firstdraw = g_nativegfx.mcla_native_gfx_firstdraw;
inline auto& mcla_native_gfx_fxaa = g_nativegfx.mcla_native_gfx_fxaa;
inline auto& mcla_native_gfx_fxaa_dump = g_nativegfx.mcla_native_gfx_fxaa_dump;
inline auto& mcla_native_gfx_fxaa_subpixel = g_nativegfx.mcla_native_gfx_fxaa_subpixel;
inline auto& mcla_native_gfx_fxaa_threshold = g_nativegfx.mcla_native_gfx_fxaa_threshold;
inline auto& mcla_native_gfx_gamma_ramp = g_nativegfx.mcla_native_gfx_gamma_ramp;
inline auto& mcla_native_gfx_gen_mips = g_nativegfx.mcla_native_gfx_gen_mips;
inline auto& mcla_native_gfx_geomdump = g_nativegfx.mcla_native_gfx_geomdump;
inline auto& mcla_native_gfx_gpu_profiler = g_nativegfx.mcla_native_gfx_gpu_profiler;
inline auto& mcla_native_gfx_gpu_untile = g_nativegfx.mcla_native_gfx_gpu_untile;
inline auto& mcla_native_gfx_guest_clear = g_nativegfx.mcla_native_gfx_guest_clear;
inline auto& mcla_native_gfx_half_pixel = g_nativegfx.mcla_native_gfx_half_pixel;
inline auto& mcla_native_gfx_hangfind = g_nativegfx.mcla_native_gfx_hangfind;
inline auto& mcla_native_gfx_inline_direct = g_nativegfx.mcla_native_gfx_inline_direct;
inline auto& mcla_native_gfx_inline_fenced = g_nativegfx.mcla_native_gfx_inline_fenced;
inline auto& mcla_native_gfx_keep_idle_display = g_nativegfx.mcla_native_gfx_keep_idle_display;
inline auto& mcla_native_gfx_lock_dump_level0 = g_nativegfx.mcla_native_gfx_lock_dump_level0;
inline auto& mcla_native_gfx_lock_writeback = g_nativegfx.mcla_native_gfx_lock_writeback;
inline auto& mcla_native_gfx_lock_writeback_dump = g_nativegfx.mcla_native_gfx_lock_writeback_dump;
inline auto& mcla_native_gfx_lowres_addr = g_nativegfx.mcla_native_gfx_lowres_addr;
inline auto& mcla_native_gfx_lowres_dump = g_nativegfx.mcla_native_gfx_lowres_dump;
inline auto& mcla_native_gfx_lowres_h = g_nativegfx.mcla_native_gfx_lowres_h;
inline auto& mcla_native_gfx_lowres_w = g_nativegfx.mcla_native_gfx_lowres_w;
inline auto& mcla_native_gfx_mrt = g_nativegfx.mcla_native_gfx_mrt;
inline auto& mcla_native_gfx_msaa = g_nativegfx.mcla_native_gfx_msaa;
inline auto& mcla_native_gfx_msaa_depth_cs = g_nativegfx.mcla_native_gfx_msaa_depth_cs;
inline auto& mcla_native_gfx_msaa_stencil = g_nativegfx.mcla_native_gfx_msaa_stencil;
inline auto& mcla_native_gfx_nocp_vsync = g_nativegfx.mcla_native_gfx_nocp_vsync;
inline auto& mcla_native_gfx_overlap_index = g_nativegfx.mcla_native_gfx_overlap_index;
inline auto& mcla_native_gfx_own_device = g_nativegfx.mcla_native_gfx_own_device;
inline auto& mcla_native_gfx_own_heap_kb = g_nativegfx.mcla_native_gfx_own_heap_kb;
inline auto& mcla_native_gfx_own_swapchain = g_nativegfx.mcla_native_gfx_own_swapchain;
inline auto& mcla_native_gfx_own_textures = g_nativegfx.mcla_native_gfx_own_textures;
inline auto& mcla_native_gfx_pack_depth_stencil = g_nativegfx.mcla_native_gfx_pack_depth_stencil;
inline auto& mcla_native_gfx_partial_reupload = g_nativegfx.mcla_native_gfx_partial_reupload;
inline auto& mcla_native_gfx_pipeline_library = g_nativegfx.mcla_native_gfx_pipeline_library;
inline auto& mcla_native_gfx_pix_markers = g_nativegfx.mcla_native_gfx_pix_markers;
inline auto& mcla_native_gfx_present = g_nativegfx.mcla_native_gfx_present;
inline auto& mcla_native_gfx_present_anchor = g_nativegfx.mcla_native_gfx_present_anchor;
inline auto& mcla_native_gfx_ps_census = g_nativegfx.mcla_native_gfx_ps_census;
inline auto& mcla_native_gfx_reclear = g_nativegfx.mcla_native_gfx_reclear;
inline auto& mcla_native_gfx_reclear_depth = g_nativegfx.mcla_native_gfx_reclear_depth;
inline auto& mcla_native_gfx_reclear_depth_probe = g_nativegfx.mcla_native_gfx_reclear_depth_probe;
inline auto& mcla_native_gfx_reclear_probe = g_nativegfx.mcla_native_gfx_reclear_probe;
inline auto& mcla_native_gfx_record_replay = g_nativegfx.mcla_native_gfx_record_replay;
inline auto& mcla_native_gfx_rectlist_nocull = g_nativegfx.mcla_native_gfx_rectlist_nocull;
inline auto& mcla_native_gfx_region_kb = g_nativegfx.mcla_native_gfx_region_kb;
inline auto& mcla_native_gfx_region_memo = g_nativegfx.mcla_native_gfx_region_memo;
inline auto& mcla_native_gfx_resolve_cpu_writes = g_nativegfx.mcla_native_gfx_resolve_cpu_writes;
inline auto& mcla_native_gfx_resolve_shape_fallback = g_nativegfx.mcla_native_gfx_resolve_shape_fallback;
inline auto& mcla_native_gfx_resolve_variants = g_nativegfx.mcla_native_gfx_resolve_variants;
inline auto& mcla_native_gfx_rim_probe = g_nativegfx.mcla_native_gfx_rim_probe;
inline auto& mcla_native_gfx_shadow_bias = g_nativegfx.mcla_native_gfx_shadow_bias;
inline auto& mcla_native_gfx_shadow_quads = g_nativegfx.mcla_native_gfx_shadow_quads;
inline auto& mcla_native_gfx_skip_draw_first = g_nativegfx.mcla_native_gfx_skip_draw_first;
inline auto& mcla_native_gfx_skip_draw_last = g_nativegfx.mcla_native_gfx_skip_draw_last;
inline auto& mcla_native_gfx_skip_ps = g_nativegfx.mcla_native_gfx_skip_ps;
inline auto& mcla_native_gfx_skip_punch = g_nativegfx.mcla_native_gfx_skip_punch;
inline auto& mcla_native_gfx_skip_water = g_nativegfx.mcla_native_gfx_skip_water;
inline auto& mcla_native_gfx_slot_cache = g_nativegfx.mcla_native_gfx_slot_cache;
inline auto& mcla_native_gfx_state_cache = g_nativegfx.mcla_native_gfx_state_cache;
inline auto& mcla_native_gfx_stencil_probe = g_nativegfx.mcla_native_gfx_stencil_probe;
inline auto& mcla_native_gfx_streaming_demote_frames = g_nativegfx.mcla_native_gfx_streaming_demote_frames;
inline auto& mcla_native_gfx_streaming_frames = g_nativegfx.mcla_native_gfx_streaming_frames;
inline auto& mcla_native_gfx_streaming_sticky = g_nativegfx.mcla_native_gfx_streaming_sticky;
inline auto& mcla_native_gfx_submit_thread = g_nativegfx.mcla_native_gfx_submit_thread;
inline auto& mcla_native_gfx_surface_key = g_nativegfx.mcla_native_gfx_surface_key;
inline auto& mcla_native_gfx_swapped_texcoords = g_nativegfx.mcla_native_gfx_swapped_texcoords;
inline auto& mcla_native_gfx_swizzle_by_source = g_nativegfx.mcla_native_gfx_swizzle_by_source;
inline auto& mcla_native_gfx_telemetry = g_nativegfx.mcla_native_gfx_telemetry;
inline auto& mcla_native_gfx_tex_cached_decode = g_nativegfx.mcla_native_gfx_tex_cached_decode;
inline auto& mcla_native_gfx_texcache_mb = g_nativegfx.mcla_native_gfx_texcache_mb;
inline auto& mcla_native_gfx_texdump = g_nativegfx.mcla_native_gfx_texdump;
inline auto& mcla_native_gfx_texinv_incremental = g_nativegfx.mcla_native_gfx_texinv_incremental;
inline auto& mcla_native_gfx_texinv_index = g_nativegfx.mcla_native_gfx_texinv_index;
inline auto& mcla_native_gfx_texture_registry = g_nativegfx.mcla_native_gfx_texture_registry;
inline auto& mcla_native_gfx_texture_swizzle = g_nativegfx.mcla_native_gfx_texture_swizzle;
inline auto& mcla_native_gfx_unlock_invalidate = g_nativegfx.mcla_native_gfx_unlock_invalidate;
inline auto& mcla_native_gfx_unsupplied_drop = g_nativegfx.mcla_native_gfx_unsupplied_drop;
inline auto& mcla_native_gfx_upload_mb = g_nativegfx.mcla_native_gfx_upload_mb;
inline auto& mcla_native_gfx_vblank_probe = g_nativegfx.mcla_native_gfx_vblank_probe;
inline auto& mcla_native_gfx_verify_per_frame = g_nativegfx.mcla_native_gfx_verify_per_frame;
inline auto& mcla_native_gfx_verify_regions = g_nativegfx.mcla_native_gfx_verify_regions;
inline auto& mcla_native_gfx_verify_textures = g_nativegfx.mcla_native_gfx_verify_textures;
inline auto& mcla_native_gfx_vinyl_diag = g_nativegfx.mcla_native_gfx_vinyl_diag;
inline auto& mcla_native_gfx_watch_before_copy = g_nativegfx.mcla_native_gfx_watch_before_copy;

}  // namespace larecomp::settings
