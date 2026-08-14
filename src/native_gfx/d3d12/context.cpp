#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — D3D12 context.
// Infrastructure extracted from the Phase 2 smoke test, generalized for the
// full runtime: allocator ring, frame fence, upload ring, deferred release.

#include "context.h"

#include <chrono>

#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_util.h>

namespace mcla::native_gfx {

namespace {
// 16 MB of transient upload space per in-flight frame. Sized for constants
// and small per-draw uploads; bulk resource uploads use their own staging.
constexpr uint64_t kUploadCapacity = 16ull << 20;
}  // namespace

D3D12Context::~D3D12Context() { Shutdown(); }

bool D3D12Context::Initialize(const rex::ui::d3d12::D3D12Provider& provider) {
  if (initialized_) {
    return true;
  }
  device_ = provider.GetDevice();
  queue_ = provider.GetDirectQueue();
  submit_mutex_ = &provider.DirectQueueSubmitMutex();
  if (!device_ || !queue_) {
    REXLOG_ERROR("[native_gfx] D3D12 provider has no device/queue");
    return false;
  }

  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&allocators_[i])))) {
      REXLOG_ERROR("[native_gfx] CreateCommandAllocator failed");
      Shutdown();
      return false;
    }
  }
  if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(),
                                        nullptr, IID_PPV_ARGS(&command_list_)))) {
    REXLOG_ERROR("[native_gfx] CreateCommandList failed");
    Shutdown();
    return false;
  }
  command_list_->SetName(L"mcla_native_gfx");
  command_list_->Close();

  if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
    REXLOG_ERROR("[native_gfx] CreateFence failed");
    Shutdown();
    return false;
  }
  fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!fence_event_) {
    REXLOG_ERROR("[native_gfx] fence event creation failed");
    Shutdown();
    return false;
  }

  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kUploadCapacity;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device_->CreateCommittedResource(
            &rex::ui::d3d12::util::kHeapPropertiesUpload, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload_buffers_[i])))) {
      REXLOG_ERROR("[native_gfx] upload ring buffer creation failed");
      Shutdown();
      return false;
    }
    const D3D12_RANGE no_read = {0, 0};
    void* mapped = nullptr;
    if (FAILED(upload_buffers_[i]->Map(0, &no_read, &mapped))) {
      REXLOG_ERROR("[native_gfx] upload ring buffer map failed");
      Shutdown();
      return false;
    }
    upload_mapped_[i] = static_cast<uint8_t*>(mapped);
  }
  upload_capacity_ = kUploadCapacity;

  initialized_ = true;
  return true;
}

ID3D12GraphicsCommandList* D3D12Context::BeginFrame() {
  if (!initialized_ || frame_open_) {
    return nullptr;
  }
  const uint32_t slot = uint32_t(frame_index_ % kFramesInFlight);
  const uint64_t completed = fence_->GetCompletedValue();
  if (completed < slot_fence_value_[slot]) {
    fence_->SetEventOnCompletion(slot_fence_value_[slot], fence_event_);
    const auto wait_begin = std::chrono::steady_clock::now();
    WaitForSingleObject(fence_event_, INFINITE);
    gpu_wait_us_ += std::chrono::duration<double, std::micro>(
                        std::chrono::steady_clock::now() - wait_begin)
                        .count();
  }
  ReleaseCompleted(fence_->GetCompletedValue());
  allocators_[slot]->Reset();
  if (FAILED(command_list_->Reset(allocators_[slot].Get(), nullptr))) {
    REXLOG_ERROR("[native_gfx] command list Reset failed");
    return nullptr;
  }
  upload_offset_[slot] = 0;
  frame_open_ = true;
  return command_list_.Get();
}

bool D3D12Context::EndFrame() {
  if (!initialized_ || !frame_open_) {
    return false;
  }
  frame_open_ = false;
  if (FAILED(command_list_->Close())) {
    REXLOG_ERROR("[native_gfx] command list Close failed");
    DrainDebugMessages("command list Close");
    return false;
  }
  // A removed device turns every later creation into a failure somewhere
  // unrelated (textures, PSOs, buffers all at once), which hides the cause.
  // Report it once, at the frame that follows the fault.
  if (!device_removed_reported_) {
    const HRESULT removed = device_->GetDeviceRemovedReason();
    if (FAILED(removed)) {
      device_removed_reported_ = true;
      REXLOG_ERROR("[native_gfx] DEVICE REMOVED: {:#010x}", uint32_t(removed));
      DrainDebugMessages("device removed");
      DumpDeviceRemovedData();
    }
  }
  ID3D12CommandList* lists[] = {command_list_.Get()};
  const uint32_t slot = uint32_t(frame_index_ % kFramesInFlight);
  {
    // Serialize against the Xenia command processor, which submits to this same
    // queue from another thread (see D3D12Provider::DirectQueueSubmitMutex).
    std::unique_lock<std::mutex> submit_lock;
    if (submit_mutex_) {
      submit_lock = std::unique_lock<std::mutex>(*submit_mutex_);
    }
    queue_->ExecuteCommandLists(1, lists);
    queue_->Signal(fence_.Get(), ++fence_value_);
  }
  slot_fence_value_[slot] = fence_value_;
  ++frame_index_;
  return true;
}

void D3D12Context::DrainDebugMessages(const char* context_label) {
  if (!device_) {
    return;
  }
  Microsoft::WRL::ComPtr<ID3D12InfoQueue> info_queue;
  if (FAILED(device_->QueryInterface(IID_PPV_ARGS(&info_queue)))) {
    REXLOG_ERROR("[native_gfx] {}: no D3D12 InfoQueue (debug layer off?)", context_label);
    return;
  }
  const UINT64 count = info_queue->GetNumStoredMessages();
  if (count == 0) {
    REXLOG_ERROR("[native_gfx] {}: InfoQueue empty", context_label);
    return;
  }
  std::vector<uint8_t> buffer;
  for (UINT64 i = 0; i < count; ++i) {
    SIZE_T length = 0;
    if (FAILED(info_queue->GetMessage(i, nullptr, &length)) || length == 0) {
      continue;
    }
    buffer.resize(length);
    auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
    if (FAILED(info_queue->GetMessage(i, message, &length))) {
      continue;
    }
    const char* text = message->pDescription ? message->pDescription : "(no description)";
    // The InfoQueue belongs to the device, which is shared with the Xenia
    // command processor, so warnings here are often not ours. Only
    // CORRUPTION/ERROR get error level; anything milder would be noise.
    if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
      REXLOG_ERROR("[native_gfx] {}: D3D12 [sev {} id {}] {}", context_label,
                   uint32_t(message->Severity), uint32_t(message->ID), text);
    } else if (message->Severity == D3D12_MESSAGE_SEVERITY_WARNING) {
      REXLOG_WARN("[native_gfx] {}: D3D12 [warning id {}] {}", context_label,
                  uint32_t(message->ID), text);
    }
  }
  info_queue->ClearStoredMessages();
}

bool D3D12Context::DrainDebugMessagesIfAny(const char* context_label) {
  if (!device_) {
    return false;
  }
  Microsoft::WRL::ComPtr<ID3D12InfoQueue> info_queue;
  if (FAILED(device_->QueryInterface(IID_PPV_ARGS(&info_queue)))) {
    return false;
  }
  const UINT64 count = info_queue->GetNumStoredMessages();
  if (count == 0) {
    return false;
  }
  std::vector<uint8_t> buffer;
  bool logged = false;
  for (UINT64 i = 0; i < count; ++i) {
    SIZE_T length = 0;
    if (FAILED(info_queue->GetMessage(i, nullptr, &length)) || length == 0) {
      continue;
    }
    buffer.resize(length);
    auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
    if (FAILED(info_queue->GetMessage(i, message, &length))) {
      continue;
    }
    const char* text = message->pDescription ? message->pDescription : "(no description)";
    if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
      REXLOG_ERROR("[native_gfx] {}: D3D12 [sev {} id {}] {}", context_label,
                   uint32_t(message->Severity), uint32_t(message->ID), text);
      logged = true;
    }
  }
  info_queue->ClearStoredMessages();
  return logged;
}

void D3D12Context::ReportCreateFailure(const char* what, long hr) {
  static bool reported = false;
  if (reported) {
    return;
  }
  reported = true;
  const HRESULT removed = device_ ? device_->GetDeviceRemovedReason() : 0;
  REXLOG_ERROR("[native_gfx] FIRST creation failure: {} hr={:#010x} device_removed_reason={:#010x}",
               what, uint32_t(hr), uint32_t(removed));
  DrainDebugMessages("first creation failure");
  DumpDeviceRemovedData();
}

namespace {

// The subset of breadcrumb ops this runtime can emit; anything else is
// printed numerically rather than guessed at.
const char* BreadcrumbOpName(D3D12_AUTO_BREADCRUMB_OP op) {
  switch (op) {
    case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "SetMarker";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "BeginEvent";
    case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "EndEvent";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DrawIndexedInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "ExecuteIndirect";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "Dispatch";
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return "ResolveSubresource";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "ClearRenderTargetView";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return "ClearUAV";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "ClearDepthStencilView";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
    case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "Present";
    default: return nullptr;
  }
}

}  // namespace

void D3D12Context::DumpDeviceRemovedData() {
  if (!device_) {
    return;
  }
  Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData> dred;
  if (FAILED(device_->QueryInterface(IID_PPV_ARGS(&dred)))) {
    REXLOG_ERROR("[native_gfx] DRED unavailable (needs the debug layer / d3d12_debug)");
    return;
  }

  // DRED1 first: the v1 breadcrumb output came back empty on a real TDR here,
  // while DRED1 carries per-node context and is what current runtimes fill in.
  Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData1> dred1;
  if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&dred1)))) {
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 bc1 = {};
    if (SUCCEEDED(dred1->GetAutoBreadcrumbsOutput1(&bc1))) {
      uint32_t idx = 0;
      for (const D3D12_AUTO_BREADCRUMB_NODE1* n = bc1.pHeadAutoBreadcrumbNode;
           n && idx < 8; n = n->pNext, ++idx) {
        const uint32_t completed = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
        REXLOG_ERROR("[native_gfx] DRED1 node {}: list={} queue={} {} ops, {} completed",
                     idx, n->pCommandListDebugNameA ? n->pCommandListDebugNameA : "?",
                     n->pCommandQueueDebugNameA ? n->pCommandQueueDebugNameA : "?",
                     n->BreadcrumbCount, completed);
        const uint32_t first = completed > 3 ? completed - 3 : 0;
        const uint32_t last =
            n->BreadcrumbCount < completed + 3 ? n->BreadcrumbCount : completed + 3;
        for (uint32_t i = first; i < last; ++i) {
          const char* name = BreadcrumbOpName(n->pCommandHistory[i]);
          REXLOG_ERROR("[native_gfx]   op[{}] {} {}", i,
                       name ? name : "(other)",
                       i == completed ? "  <-- FIRST NOT COMPLETED" : "");
        }
      }
      if (!bc1.pHeadAutoBreadcrumbNode) {
        REXLOG_ERROR("[native_gfx] DRED1: no breadcrumb nodes either");
      }
    }
  }

  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs = {};
  if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs))) {
    uint32_t node_index = 0;
    for (const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs.pHeadAutoBreadcrumbNode;
         node && node_index < 8; node = node->pNext, ++node_index) {
      const uint32_t completed = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
      REXLOG_ERROR("[native_gfx] DRED node {}: list='{}' queue='{}' {} ops, {} completed",
                   node_index,
                   node->pCommandListDebugNameA ? node->pCommandListDebugNameA : "?",
                   node->pCommandQueueDebugNameA ? node->pCommandQueueDebugNameA : "?",
                   node->BreadcrumbCount, completed);
      // Everything at or after the completed count never finished; the first
      // of those is the operation that hung.
      const uint32_t first = completed > 4 ? completed - 4 : 0;
      const uint32_t last = node->BreadcrumbCount < completed + 4 ? node->BreadcrumbCount
                                                                 : completed + 4;
      for (uint32_t i = first; i < last; ++i) {
        const D3D12_AUTO_BREADCRUMB_OP op = node->pCommandHistory[i];
        const char* name = BreadcrumbOpName(op);
        if (name) {
          REXLOG_ERROR("[native_gfx]   op[{}] {} {}", i, name,
                       i == completed ? "  <-- FIRST NOT COMPLETED" : "");
        } else {
          REXLOG_ERROR("[native_gfx]   op[{}] #{} {}", i, uint32_t(op),
                       i == completed ? "  <-- FIRST NOT COMPLETED" : "");
        }
      }
    }
    if (!breadcrumbs.pHeadAutoBreadcrumbNode) {
      REXLOG_ERROR("[native_gfx] DRED: no breadcrumb nodes");
    }
  }

  D3D12_DRED_PAGE_FAULT_OUTPUT page_fault = {};
  if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&page_fault))) {
    if (page_fault.PageFaultVA) {
      REXLOG_ERROR("[native_gfx] DRED page fault at VA {:#018x}",
                   uint64_t(page_fault.PageFaultVA));
      uint32_t n = 0;
      for (const D3D12_DRED_ALLOCATION_NODE* a = page_fault.pHeadExistingAllocationNode;
           a && n < 6; a = a->pNext, ++n) {
        REXLOG_ERROR("[native_gfx]   existing allocation: '{}' type {}",
                     a->ObjectNameA ? a->ObjectNameA : "?", uint32_t(a->AllocationType));
      }
      n = 0;
      for (const D3D12_DRED_ALLOCATION_NODE* a = page_fault.pHeadRecentFreedAllocationNode;
           a && n < 6; a = a->pNext, ++n) {
        REXLOG_ERROR("[native_gfx]   recently freed: '{}' type {}",
                     a->ObjectNameA ? a->ObjectNameA : "?", uint32_t(a->AllocationType));
      }
    } else {
      REXLOG_ERROR("[native_gfx] DRED: no page fault recorded (the hang was not a bad address)");
    }
  }
}

void D3D12Context::ClearDebugMessages() {
  if (!device_) {
    return;
  }
  Microsoft::WRL::ComPtr<ID3D12InfoQueue> info_queue;
  if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&info_queue)))) {
    info_queue->ClearStoredMessages();
  }
}

bool D3D12Context::AllocateUpload(uint64_t size, uint64_t alignment, UploadAlloc& out) {
  if (!frame_open_ || size == 0) {
    return false;
  }
  const uint32_t slot = uint32_t(frame_index_ % kFramesInFlight);
  const uint64_t offset = (upload_offset_[slot] + alignment - 1) & ~(alignment - 1);
  if (offset + size > upload_capacity_) {
    REXLOG_ERROR("[native_gfx] upload ring exhausted ({} + {} > {})", offset, size,
                 upload_capacity_);
    return false;
  }
  upload_offset_[slot] = offset + size;
  out.cpu = upload_mapped_[slot] + offset;
  out.gpu = upload_buffers_[slot]->GetGPUVirtualAddress() + offset;
  out.buffer = upload_buffers_[slot].Get();
  out.offset = offset;
  return true;
}

void D3D12Context::DeferRelease(IUnknown* resource) {
  if (!resource) {
    return;
  }
  // Hold UNTAGGED (sentinel fence value): a resource retired mid-frame is still
  // referenced by the frame's ~30 later batch submissions, so it must not be
  // freed until the WHOLE frame that retired it has finished. EndFrameReleases()
  // stamps the real fence at the frame boundary; until then this never satisfies
  // the `<= completed` test in ReleaseCompleted.
  constexpr uint64_t kUntagged = ~uint64_t(0);
  pending_releases_.push_back(PendingRelease{kUntagged, resource});
}

void D3D12Context::EndFrameReleases() {
  constexpr uint64_t kUntagged = ~uint64_t(0);
  for (auto& p : pending_releases_) {
    if (p.fence_value == kUntagged) {
      p.fence_value = fence_value_;  // the frame's final submission
    }
  }
}

void D3D12Context::ReleaseCompleted(uint64_t completed_value) {
  size_t kept = 0;
  for (size_t i = 0; i < pending_releases_.size(); ++i) {
    if (pending_releases_[i].fence_value <= completed_value) {
      pending_releases_[i].resource->Release();
    } else {
      pending_releases_[kept++] = pending_releases_[i];
    }
  }
  pending_releases_.resize(kept);
}

double D3D12Context::TakeGpuWaitUs() {
  const double v = gpu_wait_us_;
  gpu_wait_us_ = 0.0;
  return v;
}

bool D3D12Context::WaitLastSubmitTimeout(uint32_t timeout_ms) {
  if (!fence_ || !fence_event_) {
    return true;
  }
  if (fence_->GetCompletedValue() >= fence_value_) {
    return true;
  }
  fence_->SetEventOnCompletion(fence_value_, fence_event_);
  return WaitForSingleObject(fence_event_, timeout_ms) == WAIT_OBJECT_0;
}

void D3D12Context::WaitForIdle() {
  if (!queue_ || !fence_ || !fence_event_) {
    return;
  }
  queue_->Signal(fence_.Get(), ++fence_value_);
  if (fence_->GetCompletedValue() < fence_value_) {
    fence_->SetEventOnCompletion(fence_value_, fence_event_);
    WaitForSingleObject(fence_event_, INFINITE);
  }
  // The GPU is fully idle here, so any resource retired this frame (still
  // UNTAGGED) is safe to free now — stamp then reclaim.
  EndFrameReleases();
  ReleaseCompleted(fence_value_);
}

void D3D12Context::Shutdown() {
  if (!device_) {
    return;
  }
  WaitForIdle();
  for (auto& p : pending_releases_) {
    p.resource->Release();
  }
  pending_releases_.clear();
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    if (upload_buffers_[i] && upload_mapped_[i]) {
      upload_buffers_[i]->Unmap(0, nullptr);
      upload_mapped_[i] = nullptr;
    }
    upload_buffers_[i].Reset();
    allocators_[i].Reset();
  }
  command_list_.Reset();
  fence_.Reset();
  if (fence_event_) {
    CloseHandle(fence_event_);
    fence_event_ = nullptr;
  }
  device_ = nullptr;
  queue_ = nullptr;
  initialized_ = false;
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET
