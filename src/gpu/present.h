#pragma once
// ===========================================================================
// MCLA GPU renderer -- the guest's front buffer on the window
// ===========================================================================
// D3DDevice_Swap
// (sub_82419E98) hands the display the front buffer EndFrame resolved RT0
// into, through a fetch constant it builds from the texture's header: format
// 50 -> 6 and 7 -> 54, the signs, num_format and swizzle from the device's
// swap control dword (dev+13608; 0x28280106 in MCLA: swizzle (Z, Y, X, 1),
// unsigned). The front buffer is read through that fetch like any texture
// (textures.h): the resolve product of the UI composite, or guest memory when
// there is none -- what the display would scan out (logged once).
//
// The display's gamma table (dev+15004: 256 entries per channel, 10 bits,
// built by D3DDevice_SetGammaRamp from the identity through the BT.709
// display table at 0x828CDA48) lives in a 256x1 float texture, written again
// whenever the bytes change; an all-zero table (the device not built yet)
// presents through the identity instead, logged once.
//
// Fit: the image keeps its aspect, centred, black around it (letterbox),
// sampled with the table applied per texel before the bilinear mix
// (hlsl/present_ps.hlsl). An idle present (the guest not swapping) shows the
// last front buffer again. The SDK presenter's present_effect does not apply in
// this mode (logged at startup); the renderer's own FSR1 does:
//
// FSR1 (M6c.1; mcla_gpu_fsr, live): when the letterbox is larger
// than the front buffer on both axes, the SDK presenter's chain -- the table
// applied into an intermediate at the front buffer's size (the present pass
// itself, 1:1), EASU passes of at most 2x per axis up to the letterbox (at most
// mcla_gpu_fsr_max_passes), RCAS 1:1 into the back buffer with the sharpness
// 2^-mcla_gpu_fsr_sharpness. When the passes stop short of the letterbox, RCAS
// goes into an intermediate and the present's bilinear stretch (without the
// table) finishes. Otherwise -- no upscale on an axis -- the bilinear present.
// The intermediates are R16G16B16A16_FLOAT (the table's 10 bits survive), made at the sizes
// asked for and retired when they change.
//
// HDR10 (hdr.h): with an HDR10 back buffer the passes that write it -- the present, the stretch and
// RCAS's last pass -- are the variants whose last step is hlsl/hdr_output.hlsli (the SDR code
// values decoded, placed at paper white, rolled off, PQ encoded). The table, EASU and the
// intermediates stay SDR code values, so the picture is mixed and sharpened as it is in SDR and
// DISPLAY GAMMA means the same. A guest frame that kept its UI as a layer (frame_layers.h) is read
// from the float image HdrScene composed (hdr_scene.h) instead of the front buffer: its code values
// may exceed 1, so the table is read as before up to 1 and goes on above 1 by the slope of its last
// step (kHdrFlagExtendedCodes); FSR1's passes read that image in float16.
//
// Threading: render thread only, after Initialize.
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include <plume_render_interface.h>

#include "guest_device.h"
#include "hdr.h"
#include "host_passes.h"
#include "records.h"

namespace mcla::gpu {

class BindlessHeap;
class Device;
class FrameRing;
class HdrScene;
class PassTiming;
class TextureCache;
class UploadRing;

class PresentPass {
 public:
  static constexpr uint32_t kMaxFsrPasses = 4;  // EASU passes, as the SDK's maximum

  PresentPass() = default;
  ~PresentPass();
  PresentPass(const PresentPass&) = delete;
  PresentPass& operator=(const PresentPass&) = delete;

  // `hdr`: the back buffer is HDR10 (SwapChain::hdr()), `target_format` its format.
  bool Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, UploadRing& upload,
                  HostPasses& passes, TextureCache& textures, PassTiming& pass_timing,
                  plume::RenderFormat target_format, bool hdr);
  // GPU idle, before HostPasses and the BindlessHeap shut down.
  void Shutdown();

  // A guest present: the front buffer and the table it shows from now on.
  void OnGuestPresent(const PresentRecord& record);

  // HDR10 only: the levels Draw puts in its constants, each frame before it.
  void SetHdrLevels(const HdrLevels& levels) { hdr_levels_ = levels; }
  // HDR10 only: where Prepare takes the frame from when the guest frame kept its UI as a layer
  // (hdr_scene.h) -- the float image of the scene and the UI composed -- instead of the front
  // buffer. Not owned; null: the front buffer always.
  void SetHdrScene(HdrScene* scene) { hdr_scene_ = scene; }

  // Outside any render pass: binds the front buffer (its upload, if it comes
  // from memory) and writes the table when it changed, into `list`; with FSR1
  // on and an upscale to a letterbox in `width` x `height`, records the passes
  // before the back buffer's (the table, EASU, and RCAS when a stretch follows).
  void Prepare(plume::RenderCommandList* list, uint32_t slot, uint32_t width, uint32_t height);
  // Inside the back buffer's render pass, cleared by the caller: draws the
  // front buffer fitted into `width` x `height` -- the size Prepare had. Nothing
  // before the first guest present.
  void Draw(plume::RenderCommandList* list, uint32_t width, uint32_t height);
  // The same pass 1:1 into a target of source_width() x source_height()
  // (the flicker probe); not counted in the summary.
  void DrawUnscaled(plume::RenderCommandList* list);

  // What Prepare bound for this frame.
  bool source_ready() const { return source_ready_; }
  uint32_t source_width() const { return source_width_; }
  uint32_t source_height() const { return source_height_; }

  std::string TakeSummary();

 private:
  // The letterbox: the largest rectangle of the source's aspect, centred, on
  // whole pixels, in a `width` x `height` target.
  struct Fit {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t w = 0;
    uint32_t h = 0;
  };
  static Fit FitOf(uint32_t source_width, uint32_t source_height, uint32_t width,
                   uint32_t height);
  // Draw's body: false when there is nothing to draw.
  bool RecordDraw(plume::RenderCommandList* list, uint32_t width, uint32_t height);
  // The bilinear present of `source` (`width` x `height` texels) into `fit`,
  // with `pipeline` (the table applied, or not); inside a render pass.
  void RecordFitted(plume::RenderCommandList* list, plume::RenderPipeline* pipeline,
                    uint32_t source, uint32_t source_width, uint32_t source_height,
                    const Fit& fit);

  struct Push {
    uint32_t source;
    uint32_t ramp;
    uint32_t source_width;
    uint32_t source_height;
    float origin_x;
    float origin_y;
    float scale_x;
    float scale_y;
    // The HDR10 variants only (hlsl/hdr_output.hlsli HdrLevels); the others read the 32 bytes above.
    float hdr_white;
    float hdr_peak;
    float hdr_knee;
    uint32_t hdr_flags;
  };
  static_assert(sizeof(Push) == 48 && sizeof(Push) <= HostPasses::kPushConstantBytes);

  // FSR1 (M6c.1).
  struct EasuPush {  // hlsl/fsr_easu_ps.hlsl
    uint32_t source;
    uint32_t reserved;
    float ratio_x;  // input size / output size
    float ratio_y;
    float input_size_inv_x;
    float input_size_inv_y;
  };
  struct RcasPush {  // hlsl/fsr_rcas_ps.hlsl
    uint32_t source;
    float sharpness;  // 2^-stops
    int32_t output_x;
    int32_t output_y;
    int32_t input_max_x;
    int32_t input_max_y;
    // The HDR10 variant only (hlsl/hdr_output.hlsli HdrLevels).
    // CHECK: these land at byte 24, 28, 32 and 36, as in the HLSL cbuffer (and with -fvk-use-dx-layout
    // on Vulkan); the SDK's dxc here has no SPIR-V back end, so only the DXIL builds were compiled.
    float hdr_white;
    float hdr_peak;
    float hdr_knee;
    uint32_t hdr_flags;
  };
  static_assert(sizeof(EasuPush) <= HostPasses::kPushConstantBytes &&
                sizeof(RcasPush) <= HostPasses::kPushConstantBytes);
  // chain_[0] the table's output at the source size, [1..n] the EASU passes',
  // [n + 1] RCAS's when the stretch follows.
  // kRamped: the table applied 1:1 into ramped_ first, then the bilinear
  // stretch without the table (PrepareRamped).
  enum class Route : uint8_t { kBilinear, kFsr, kFsrStretch, kRamped };
  // `index` at `width` x `height` (HostPasses::EnsureImage); counted when it
  // cannot be made.
  bool EnsureIntermediate(uint32_t index, uint32_t width, uint32_t height);
  // Prepare's FSR1 half: the passes before the back buffer's, and route_.
  void PrepareFsr(plume::RenderCommandList* list, const Fit& fit);
  // Prepare's bilinear half when the present scales: the table once per
  // source texel into ramped_ (kRamped), instead of once per texel each
  // back buffer pixel reads (four).
  void PrepareRamped(plume::RenderCommandList* list);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  UploadRing* upload_ = nullptr;
  HostPasses* passes_ = nullptr;
  TextureCache* textures_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  std::unique_ptr<plume::RenderShader> shader_;
  std::unique_ptr<plume::RenderPipeline> pipeline_;  // into the back buffer (the HDR10 variant when hdr_)
  // FSR1.
  std::unique_ptr<plume::RenderShader> easu_shader_;
  std::unique_ptr<plume::RenderShader> rcas_shader_;
  std::unique_ptr<plume::RenderShader> stretch_shader_;
  // HDR10: the shaders of the passes that write the back buffer (shader_, rcas_shader_ and
  // stretch_shader_ stay for the intermediates).
  std::unique_ptr<plume::RenderShader> hdr_shader_;
  std::unique_ptr<plume::RenderShader> rcas_hdr_shader_;
  std::unique_ptr<plume::RenderShader> stretch_hdr_shader_;
  std::unique_ptr<plume::RenderPipeline> ramp_pipeline_;          // present_ps into an intermediate
  std::unique_ptr<plume::RenderPipeline> easu_pipeline_;
  std::unique_ptr<plume::RenderPipeline> rcas_pipeline_;          // into the back buffer
  std::unique_ptr<plume::RenderPipeline> rcas_offscreen_pipeline_;  // into an intermediate
  std::unique_ptr<plume::RenderPipeline> stretch_pipeline_;       // into the back buffer
  std::array<HostImage, kMaxFsrPasses + 2> chain_;
  // kRamped: present_ps 1:1 into ramped_ (R32G32B32A32_FLOAT, which holds the
  // table's values exactly, so the stretch mixes the same numbers as before).
  std::unique_ptr<plume::RenderPipeline> ramped_pipeline_;
  HostImage ramped_;
  bool hdr_ = false;    // the back buffer is HDR10
  HdrLevels hdr_levels_;
  HdrScene* hdr_scene_ = nullptr;
  // Prepare bound the HdrScene's image: its codes may exceed 1 (kHdrFlagExtendedCodes).
  bool extended_ = false;
  Route route_ = Route::kBilinear;
  uint32_t final_ = 0;  // the chain_ entry Draw reads
  float sharpness_ = 1.0f;
  Fit fit_;             // Prepare's letterbox
  // What the log said last (a line when the route or the sizes change).
  std::string reported_route_;
  std::unique_ptr<plume::RenderTexture> ramp_texture_;
  std::unique_ptr<plume::RenderTextureView> ramp_view_;
  uint32_t ramp_slot_ = 0xFFFFFFFFu;
  plume::RenderTextureLayout ramp_layout_ = plume::RenderTextureLayout::UNKNOWN;

  // The last guest present.
  bool have_front_ = false;
  uint32_t front_dwords_[6] = {};  // the fetch as the Swap builds it, host order
  std::array<uint8_t, guest::kGammaRampBytes> ramp_bytes_{};
  bool ramp_dirty_ = false;
  bool ramp_written_ = false;
  // Bound by Prepare for Draw.
  uint32_t source_slot_ = 0;
  uint32_t source_width_ = 0;
  uint32_t source_height_ = 0;
  bool source_ready_ = false;

  bool reported_unreadable_ = false;
  bool reported_zero_ramp_ = false;
  bool reported_signs_ = false;

  struct Stats {
    uint64_t guest_presents = 0;
    uint64_t drawn = 0;
    uint64_t from_memory = 0;  // front buffer not from a resolve product
    uint64_t ramp_writes = 0;
    uint64_t fsr = 0;          // presents through FSR1 (stretched ones among them)
    uint64_t fsr_stretched = 0;
    uint64_t fsr_failed = 0;   // FSR1 asked, an intermediate could not be made
    uint64_t ramped = 0;       // scaled presents with the table applied first (kRamped)
    uint64_t ramped_failed = 0;  // ramped_ could not be made: the table per texel read
  };
  Stats stats_;
};

}  // namespace mcla::gpu
