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
// last front buffer again. The SDK presenter's present_effect (FSR and the
// rest) does not apply in this mode: logged at startup, host post is M6.
//
// Threading: render thread only, after Initialize.
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include <plume_render_interface.h>

#include "guest_device.h"
#include "records.h"

namespace mcla::gpu {

class BindlessHeap;
class Device;
class FrameRing;
class HostPasses;
class TextureCache;
class UploadRing;

class PresentPass {
 public:
  PresentPass() = default;
  ~PresentPass();
  PresentPass(const PresentPass&) = delete;
  PresentPass& operator=(const PresentPass&) = delete;

  bool Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, UploadRing& upload,
                  HostPasses& passes, TextureCache& textures, plume::RenderFormat target_format);
  // GPU idle, before HostPasses and the BindlessHeap shut down.
  void Shutdown();

  // A guest present: the front buffer and the table it shows from now on.
  void OnGuestPresent(const PresentRecord& record);

  // Outside any render pass: binds the front buffer (its upload, if it comes
  // from memory) and writes the table when it changed, into `list`.
  void Prepare(plume::RenderCommandList* list, uint32_t slot);
  // Inside the back buffer's render pass, cleared by the caller: draws the
  // front buffer fitted into `width` x `height`. Nothing before the first
  // guest present.
  void Draw(plume::RenderCommandList* list, uint32_t width, uint32_t height);

  std::string TakeSummary();

 private:
  struct Push {
    uint32_t source;
    uint32_t ramp;
    uint32_t source_width;
    uint32_t source_height;
    float origin_x;
    float origin_y;
    float scale_x;
    float scale_y;
  };
  static_assert(sizeof(Push) == 32);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  UploadRing* upload_ = nullptr;
  HostPasses* passes_ = nullptr;
  TextureCache* textures_ = nullptr;
  std::unique_ptr<plume::RenderShader> shader_;
  std::unique_ptr<plume::RenderPipeline> pipeline_;
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
  };
  Stats stats_;
};

}  // namespace mcla::gpu
