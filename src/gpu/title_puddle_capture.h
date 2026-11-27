#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <plume_render_interface.h>

#include "mc_engine/hooks/title_puddle_probe.h"
#include "resolve.h"
#include "title_capture_rules.h"

namespace mcla::gpu {

class BindlessHeap;
class Device;
class DrawView;
class FrameRing;
class ResolveCache;
class UploadRing;
struct SharedConstants;

// Opt-in snapshots of guest resolve outputs, in submission order.
class TitlePuddleCapture {
 public:
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap);
  bool enabled() const {
    using mc::title_puddles::Arm;
    return !plan_.seconds.empty() || arm_ == Arm::NoPlanar ||
           arm_ == Arm::NoBloom || arm_ == Arm::FixedExposure;
  }
  void Shutdown();
  void BeginFrame(uint64_t frame);
  void EndFrame();
  bool sampling() const { return sample_.has_value(); }
  void RecordResolve(plume::RenderCommandList& list, const ResolveRecord& record,
                     const ResolveCache::CaptureWrite& write);
  bool Wants(uint64_t ps) const;
  bool NeedsUpload() const {
    return arm_ == mc::title_puddles::Arm::FixedExposure && exposure_slot_ == UINT32_MAX;
  }

  void Record(plume::RenderCommandList& list, ResolveCache& resolve, UploadRing& upload,
              uint32_t frame_slot, const DrawView& draw, uint32_t vs_mask, uint32_t ps_mask,
              SharedConstants& shared);

 private:
  uint32_t ExposureTexture(plume::RenderCommandList& list, UploadRing& upload, uint32_t slot);
  const char* Save(plume::RenderCommandList& list, const ResolveCache::CaptureWrite& write,
                   const std::string& name, const std::string& detail);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  mc::title_puddles::Arm arm_ = mc::title_puddles::Arm::Off;
  title_capture::Plan plan_;
  size_t next_sample_ = 0;
  uint64_t frame_ = 0;
  std::optional<uint32_t> sample_;
  int64_t elapsed_ms_ = 0;
  uint32_t resolve_number_ = 0;
  bool title_ = false;
  bool scene_seen_ = false;
  bool switch_logged_ = false;
  std::unique_ptr<plume::RenderTexture> exposure_;
  std::unique_ptr<plume::RenderTextureView> exposure_view_;
  uint32_t exposure_slot_ = UINT32_MAX;
};

}  // namespace mcla::gpu
