#pragma once

#include "rt_denoise.h"

namespace mcla::gpu {

class RtAoDenoise {
 public:
  void Initialize(Device&, FrameRing&, BindlessHeap&, HostPasses&);
  void Shutdown();
  bool available() const;

  struct Frame {
    plume::RenderCommandList* list = nullptr;
    HostImage* signal = nullptr;
    HostImage* depth = nullptr;
    HostImage* depth_prev = nullptr;
    HostImage* normal = nullptr;
    HostImage* id_share = nullptr;
    uint32_t traced[2] = {};
    float tan_x = 0, tan_y = 0, prev_tan_x = 0, prev_tan_y = 0;
    // Leave these rows zero when the camera step is unknown (motion_still).
    // Vehicle reprojection reads the independently validated vehicle table.
    float motion[12] = {};
    uint32_t vehicle_table_slot = 0xFFFFFFFFu;
    uint32_t vehicle_ids = 0;
    bool history_valid = false;
    uint64_t frame = 0;
  };
  const HostImage* Record(const Frame&);
  // The trace planner can temporarily shorten history after a nearby TLAS update.
  const HostImage* Record(const Frame&, uint32_t history_limit);
  void ResetHistory();
  float LastMeanLength() const;
  void OnPresent();
  uint64_t HeldBytes() const;

 private:
  void SampleHistory(plume::RenderCommandList*, const HostImage&);

  mutable RtDenoise denoiser_;
  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  uint64_t previous_frame_ = 0;
  uint64_t history_generation_ = 0;
  uint32_t previous_cap_ = 0;
  bool recorded_ = false;
  float mean_length_ = 0.0f;
};

namespace rtao_denoise {
bool Enabled();
uint32_t RawScale();
}

}  // namespace mcla::gpu
