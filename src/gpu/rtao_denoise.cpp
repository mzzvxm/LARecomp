#include "rtao_denoise.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <string_view>

#include "car_dump_rules.h"
#include "descriptors.h"
#include "device.h"
#include "frame.h"
#include "host_shaders.h"
#include "rt_denoise_v2_rules.h"
#include "rtao_denoise_rules.h"
#include "settings/larecomp_settings.h"

namespace mcla::gpu {
namespace {
namespace words = rt_denoise::plain;
constexpr uint32_t kVehicleSlotHigh = 24;
constexpr uint32_t kHistoryCap = 25;
constexpr uint32_t kRawSignal = 26;
constexpr uint32_t kSpatialResolve = 27;
constexpr uint32_t kInvalidHalfSlot = 0xFFFFu;

bool Matches(const HostImage* image, const uint32_t (&size)[2]) {
  return image && image->texture && image->slot < kInvalidHalfSlot && image->width == size[0] && image->height == size[1];
}

uint32_t Pack(uint32_t lo, uint32_t hi) { return lo | (hi << 16u); }
}  // namespace

namespace rtao_denoise {
bool Enabled() {
  return std::string_view(larecomp::settings::mcla_gpu_rtao_denoise.Get()) != "off";
}

uint32_t RawScale() {
  // CHECK: compare half-size and full-size noise in the RR-6 picture run.
  return larecomp::settings::mcla_gpu_rtao_raw_scale.Get() == 1 ? 1u : 2u;
}
}  // namespace rtao_denoise

void RtAoDenoise::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes) {
  device_ = &device;
  frames_ = &frames;
  const HostShaderBlob spatial = RtDenoiseAoShader(false);
  const HostShaderBlob temporal = RtDenoiseAoShader(true);
  denoiser_.Initialize(device, frames, heap, passes, 5, "rtao", {spatial.data, spatial.size, spatial.name}, {temporal.data, temporal.size, temporal.name},
                       rt_denoise::Format::kRgba16f);
  const auto v2_spatial = RtDenoiseV2AoShader(false);
  const auto v2_temporal = RtDenoiseV2AoShader(true);
  denoiser_.InitializeV2({v2_spatial.data, v2_spatial.size, v2_spatial.name}, {v2_temporal.data, v2_temporal.size, v2_temporal.name});
}

void RtAoDenoise::Shutdown() {
  denoiser_.Shutdown();
  ResetHistory();
  device_ = nullptr;
  frames_ = nullptr;
}

bool RtAoDenoise::available() const {
  if (!denoiser_.available()) return false;
  const bool spatial = denoiser_.SpatialReady();
  const bool temporal = denoiser_.TemporalReady();
  return spatial && temporal;
}

const HostImage* RtAoDenoise::Record(const Frame& in) {
  return Record(in, rtao_denoise::kDefaultCap);
}

const HostImage* RtAoDenoise::Record(const Frame& in, uint32_t history_limit) {
  if (!rtao_denoise::Enabled() || !in.list || in.traced[0] == 0 || in.traced[1] == 0 || in.traced[0] > 0xFFFFu || in.traced[1] > 0xFFFFu ||
      !(in.tan_x > 0.0f) || !std::isfinite(in.tan_x) || !(in.tan_y > 0.0f) || !std::isfinite(in.tan_y) ||
      !Matches(in.signal, in.traced) || !Matches(in.depth, in.traced) ||
      !Matches(in.normal, in.traced) || !Matches(in.id_share, in.traced) || !available()) {
    ResetHistory();
    return nullptr;
  }
  const uint32_t cap = rtao_denoise::HistoryCap(larecomp::settings::mcla_gpu_rtao_history.Get());
  const uint32_t frame_cap = std::clamp(history_limit, 1u, cap);
  const bool spatial_resolve = larecomp::settings::mcla_gpu_rtao_spatial_resolve.Get();
  const bool lenses_valid = std::isfinite(in.prev_tan_x) && std::isfinite(in.prev_tan_y) && in.prev_tan_x > 0.0f && in.prev_tan_y > 0.0f;
  const bool guides_valid = in.history_valid && lenses_valid && Matches(in.depth_prev, in.traced);
  const bool read_history = frame_cap > 1 && spatial_resolve == previous_spatial_resolve_ &&
                            rtao_denoise::CanReadHistory(guides_valid, recorded_, in.frame, previous_frame_, cap, previous_cap_);
  if (!read_history && recorded_) ResetHistory();
  const uint32_t table_slot = in.vehicle_table_slot < kBindlessTextureCount ? in.vehicle_table_slot : 0;
  RtDenoise::Frame pass;
  pass.list = in.list;
  pass.signal = in.signal;
  pass.reads = {in.depth, in.normal, in.id_share, read_history ? in.depth_prev : nullptr};
  pass.width = in.traced[0];
  pass.height = in.traced[1];
  pass.history_valid = read_history;
  pass.patch = words::PatchFor();
  auto& push = pass.push;
  push[words::kSignal] = Pack(in.signal->slot, in.depth->slot);
  push[words::kGuides] = Pack(in.normal->slot, in.id_share->slot);
  push[words::kHistory] = Pack(0, read_history ? in.depth_prev->slot : 0);
  push[words::kSize] = Pack(pass.width, pass.height);
  push[words::kTanX] = std::bit_cast<uint32_t>(in.tan_x);
  push[words::kTanY] = std::bit_cast<uint32_t>(in.tan_y);
  push[words::kPrevTanX] = std::bit_cast<uint32_t>(in.prev_tan_x);
  push[words::kPrevTanY] = std::bit_cast<uint32_t>(in.prev_tan_y);
  for (uint32_t i = 0; i < 12; ++i) push[words::kMotion + i] = std::bit_cast<uint32_t>(in.motion[i]);
  push[words::kFrame] = uint32_t(in.frame);
  push[words::kVehicle] = rtao_denoise::VehicleWord(table_slot, table_slot ? in.vehicle_ids : 0);
  push[kVehicleSlotHigh] = table_slot >> 8u;
  push[kHistoryCap] = frame_cap;
  push[kRawSignal] = in.signal->slot;
  push[kSpatialResolve] = spatial_resolve ? 1u : 0u;

  // CHECK: validate the shared vehicle table's graphics-to-compute handoff (ao.cpp makes it
  // compute-readable before the trace) in a play frame on both backends. The "rtao denoise"
  // timing span is marked by RtAo::RecordFilter. Run the four NOISE-NA6 takes and compare the
  // cockpit and exterior views.
  RtDenoise::Frame v2 = pass;
  v2.iterations = rt_denoise::v2::kAmbientIterations;
  if (in.motion_still) {
    // A still camera has valid identity reprojection for world receivers.
    v2.push[words::kMotion] = v2.push[words::kMotion + 5] = v2.push[words::kMotion + 10] = std::bit_cast<uint32_t>(1.0f);
  }
  const HostImage* result = denoiser_.RecordV2(v2);
  if (!result) {
    const HostImage* temporal = denoiser_.RecordTemporal(pass);
    if (temporal) {
      pass.signal = const_cast<HostImage*>(temporal);
      pass.reads[3] = in.signal;
      pass.iterations = rt_denoise::kMaxIterations;
      result = denoiser_.RecordSpatial(pass);
    }
  }
  if (!result) {
    ResetHistory();
    return nullptr;
  }
  previous_frame_ = in.frame;
  previous_cap_ = cap;
  previous_spatial_resolve_ = spatial_resolve;
  recorded_ = true;
  if (!denoiser_.last_history_read()) mean_length_ = 1.0f;
  if (denoiser_.temporal_passes() % 600 == 4) SampleHistory(in.list, *result);
  return result;
}

void RtAoDenoise::SampleHistory(plume::RenderCommandList* list, const HostImage& image) {
  // A stratified sample for the census. Each region is copied off the GPU
  // once per interval and mapped after the submitting slot's fence.
  constexpr uint32_t kGrid = 4, kRegion = 8, kRowBytes = 256;
  constexpr uint32_t kStride = kRowBytes * kRegion;
  const uint32_t width = std::min(image.width, kRegion);
  const uint32_t height = std::min(image.height, kRegion);
  auto readback = std::shared_ptr(device_->CreateBuffer(plume::RenderBufferDesc::ReadbackBuffer(kGrid * kGrid * kStride), "rtao history census"));
  if (!readback) return;
  using Layout = plume::RenderTextureLayout;
  namespace Stage = plume::RenderBarrierStage;
  list->barriers(Stage::COPY, plume::RenderTextureBarrier(image.texture.get(), Layout::COPY_SOURCE));
  for (uint32_t y = 0; y < kGrid; ++y) {
    for (uint32_t x = 0; x < kGrid; ++x) {
      const uint32_t left = (image.width - width) * (2 * x + 1) / (2 * kGrid);
      const uint32_t top = (image.height - height) * (2 * y + 1) / (2 * kGrid);
      const plume::RenderBox box(left, top, left + width, top + height);
      const auto dst = plume::RenderTextureCopyLocation::PlacedFootprint(readback.get(), image.format, width, height, 1,
                                                                         kRowBytes / 8, (y * kGrid + x) * kStride);
      list->copyTextureRegion(dst, plume::RenderTextureCopyLocation::Subresource(image.texture.get()), 0, 0, 0, &box);
    }
  }
  list->barriers(Stage::GRAPHICS_AND_COMPUTE, plume::RenderTextureBarrier(image.texture.get(), Layout::SHADER_READ));
  const uint64_t generation = history_generation_;
  // CHECK: the sparse copy and fence-delayed census on both APIs.
  frames_->RetireAction([this, readback = std::move(readback), generation, width, height] {
    if (generation != history_generation_) return;
    const auto* mapped = static_cast<const uint8_t*>(readback->map());
    if (!mapped) return;
    double sum = 0.0;
    uint32_t samples = 0;
    for (uint32_t cell = 0; cell < kGrid * kGrid; ++cell) {
      for (uint32_t y = 0; y < height; ++y) {
        const auto* row = mapped + cell * kStride + y * kRowBytes;
        for (uint32_t x = 0; x < width; ++x) {
          uint16_t half;
          std::memcpy(&half, row + x * 8 + 6, sizeof(half));
          const float length = car_dump::HalfToFloat(half);
          if (length >= 1.0f && length <= 32.0f) {
            sum += length;
            ++samples;
          }
        }
      }
    }
    readback->unmap();
    mean_length_ = samples ? float(sum / samples) : 0.0f;
  });
}

void RtAoDenoise::ResetHistory() {
  denoiser_.ResetHistory();
  recorded_ = false;
  ++history_generation_;
  mean_length_ = 0.0f;
}

float RtAoDenoise::LastMeanLength() const { return mean_length_; }
void RtAoDenoise::OnPresent() { denoiser_.OnPresent(); }
uint64_t RtAoDenoise::HeldBytes() const { return denoiser_.HeldBytes(); }

}  // namespace mcla::gpu
