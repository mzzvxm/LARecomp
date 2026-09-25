#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — GPU Profiler & Diagnostics

#include "gpu_profiler.h"

#include <cstring>
#include <sstream>
#include <iomanip>

#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_util.h>

#include "context.h"

namespace mcla::native_gfx {

GpuProfiler& GpuProfiler::Instance() {
  static GpuProfiler s_instance;
  return s_instance;
}

GpuProfiler::~GpuProfiler() {
  Shutdown();
}

bool GpuProfiler::Initialize(D3D12Context& context) {
  if (initialized_) {
    return true;
  }
  if (!context.device() || !context.queue()) {
    return false;
  }

  ID3D12Device* device = context.device();
  ID3D12CommandQueue* queue = context.queue();

  if (FAILED(queue->GetTimestampFrequency(&timestamp_frequency_))) {
    REXLOG_WARN("[native_gfx] GetTimestampFrequency failed; GPU profiling disabled");
    return false;
  }

  D3D12_QUERY_HEAP_DESC qd = {};
  qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
  qd.Count = kMaxQueries;
  qd.NodeMask = 0;

  if (FAILED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&query_heap_)))) {
    REXLOG_WARN("[native_gfx] CreateQueryHeap failed; GPU profiling disabled");
    return false;
  }

  D3D12_RESOURCE_DESC rb = {};
  rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rb.Width = kMaxQueries * sizeof(uint64_t);
  rb.Height = 1;
  rb.DepthOrArraySize = 1;
  rb.MipLevels = 1;
  rb.SampleDesc.Count = 1;
  rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  for (uint32_t i = 0; i < kRingSize; ++i) {
    if (FAILED(device->CreateCommittedResource(
            &rex::ui::d3d12::util::kHeapPropertiesReadback, D3D12_HEAP_FLAG_NONE, &rb,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slots_[i].readback)))) {
      REXLOG_WARN("[native_gfx] Failed to create GPU query readback buffer for slot {}", i);
      Shutdown();
      return false;
    }
    slots_[i].active = false;
    slots_[i].scopes.reserve(32);
  }

  initialized_ = true;
  REXLOG_INFO("[native_gfx] GpuProfiler initialized (freq: {} Hz, {} slots)",
              timestamp_frequency_, kRingSize);
  return true;
}

void GpuProfiler::Shutdown() {
  initialized_ = false;
  query_heap_.Reset();
  for (uint32_t i = 0; i < kRingSize; ++i) {
    slots_[i].readback.Reset();
    slots_[i].scopes.clear();
    slots_[i].active = false;
  }
  open_scopes_.clear();
  last_scope_times_ms_.clear();
  last_frame_time_ms_ = 0.0;
}

void GpuProfiler::BeginFrame(D3D12Context& context, ID3D12GraphicsCommandList* cl) {
  if (!initialized_ || !REXCVAR_GET(mcla_native_gfx_gpu_profiler)) {
    return;
  }

  current_slot_ = uint32_t(context.frame_index() % kRingSize);
  const uint32_t prev_slot = (current_slot_ + 1) % kRingSize;
  ReadbackSlot(prev_slot);

  FrameSlot& slot = slots_[current_slot_];
  slot.scopes.clear();
  slot.active = true;
  slot.frame_index = context.frame_index();
  next_query_ = 0;
  open_scopes_.clear();

  if (cl) {
    cl->EndQuery(query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, next_query_++);
  }
}

void GpuProfiler::EndFrame(D3D12Context& context, ID3D12GraphicsCommandList* cl) {
  (void)context;
  if (!initialized_ || !REXCVAR_GET(mcla_native_gfx_gpu_profiler)) {
    return;
  }

  FrameSlot& slot = slots_[current_slot_];
  if (!slot.active || !cl) {
    return;
  }

  while (!open_scopes_.empty()) {
    EndScope(cl);
  }

  if (next_query_ < kMaxQueries) {
    cl->EndQuery(query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
  }
  slot.query_count = next_query_;

  if (next_query_ > 0) {
    cl->ResolveQueryData(query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, next_query_,
                         slot.readback.Get(), 0);
  }
}

void GpuProfiler::BeginScope(ID3D12GraphicsCommandList* cl, const char* name) {
  if (REXCVAR_GET(mcla_native_gfx_pix_markers) && cl && name) {
    cl->BeginEvent(0, name, static_cast<UINT>(std::strlen(name) + 1));
  }

  if (!initialized_ || !REXCVAR_GET(mcla_native_gfx_gpu_profiler) || !cl || !name) {
    return;
  }

  FrameSlot& slot = slots_[current_slot_];
  if (!slot.active || next_query_ + 2 >= kMaxQueries) {
    return;
  }

  const uint32_t q = next_query_++;
  cl->EndQuery(query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q);

  ScopeRecord rec;
  rec.name = name;
  rec.start_query = q;
  rec.end_query = 0;
  slot.scopes.push_back(std::move(rec));
  open_scopes_.push_back(static_cast<uint32_t>(slot.scopes.size() - 1));
}

void GpuProfiler::EndScope(ID3D12GraphicsCommandList* cl) {
  if (REXCVAR_GET(mcla_native_gfx_pix_markers) && cl) {
    cl->EndEvent();
  }

  if (!initialized_ || !REXCVAR_GET(mcla_native_gfx_gpu_profiler) || !cl || open_scopes_.empty()) {
    return;
  }

  FrameSlot& slot = slots_[current_slot_];
  if (!slot.active || next_query_ >= kMaxQueries) {
    open_scopes_.pop_back();
    return;
  }

  const uint32_t scope_idx = open_scopes_.back();
  open_scopes_.pop_back();

  const uint32_t q = next_query_++;
  cl->EndQuery(query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q);
  slot.scopes[scope_idx].end_query = q;
}

void GpuProfiler::SetMarker(ID3D12GraphicsCommandList* cl, const char* name) {
  if (REXCVAR_GET(mcla_native_gfx_pix_markers) && cl && name) {
    cl->SetMarker(0, name, static_cast<UINT>(std::strlen(name) + 1));
  }
}

void GpuProfiler::ReadbackSlot(uint32_t slot_idx) {
  FrameSlot& slot = slots_[slot_idx];
  if (!slot.active || slot.query_count < 2 || !slot.readback) {
    return;
  }

  const D3D12_RANGE read_range = {0, slot.query_count * sizeof(uint64_t)};
  void* mapped = nullptr;
  if (SUCCEEDED(slot.readback->Map(0, &read_range, &mapped)) && mapped) {
    const uint64_t* ts = static_cast<const uint64_t*>(mapped);
    const double freq = double(timestamp_frequency_);

    if (ts[1] >= ts[0] && freq > 0.0) {
      last_frame_time_ms_ = double(ts[1] - ts[0]) * 1000.0 / freq;
    }

    last_scope_times_ms_.clear();
    for (const auto& scope : slot.scopes) {
      if (scope.end_query > scope.start_query && scope.end_query < slot.query_count) {
        const uint64_t start = ts[scope.start_query];
        const uint64_t end = ts[scope.end_query];
        if (end >= start && freq > 0.0) {
          last_scope_times_ms_[scope.name] = double(end - start) * 1000.0 / freq;
        }
      }
    }

    const D3D12_RANGE write_range = {0, 0};
    slot.readback->Unmap(0, &write_range);
  }
  slot.active = false;
}

double GpuProfiler::scope_time_ms(std::string_view name) const {
  auto it = last_scope_times_ms_.find(std::string(name));
  return it != last_scope_times_ms_.end() ? it->second : 0.0;
}

std::string GpuProfiler::Summary() const {
  std::ostringstream ss;
  ss << std::fixed << std::setprecision(2);
  ss << "GPU: " << last_frame_time_ms_ << "ms";
  for (const auto& [name, ms] : last_scope_times_ms_) {
    ss << " | " << name << ": " << ms << "ms";
  }
  return ss.str();
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET
