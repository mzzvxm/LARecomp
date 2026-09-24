#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — D3D12 context.
// Infrastructure extracted from the Phase 2 smoke test, generalized for the
// full runtime: allocator ring, frame fence, upload ring, deferred release.

#include "context.h"
#include "device_manager.h"
#include "../diag.h"

#include <chrono>
#include <cstdio>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_util.h>

// Defined at global scope in native_gfx.cpp, so the declaration has to sit
// outside the namespace too: the macro builds the storage accessor from the
// enclosing scope, and declaring it inside mcla::native_gfx asks the linker
// for a symbol nobody defines.
REXCVAR_DECLARE(uint32_t, mcla_native_gfx_upload_mb);
REXCVAR_DECLARE(bool, mcla_native_gfx_record_replay);
REXCVAR_DECLARE(bool, mcla_native_gfx_submit_thread);
REXCVAR_DECLARE(bool, mcla_native_gfx_diag);

namespace mcla::native_gfx {

namespace {
// Transient upload space per in-flight frame.
//
// This used to be a fixed 16 MB, with the comment "sized for constants and
// small per-draw uploads; bulk resource uploads use their own staging". That
// assumption was wrong: BufferCache::UploadRegion allocates whole geometry
// regions here, and measurement put that at ~16 MiB in ~93 allocations EVERY
// frame -- the ring died during geometry, so constants and textures never got
// a turn and not a single draw was ever recorded (0 constant uploads across
// 17778 frames). The native runtime published a frame only when the geometry
// happened to fit, which is why it appeared roughly once every ten seconds and
// Xenia drew the rest.
//
// The re-uploads behind that are legitimate work -- dynamic geometry the game
// rewrote -- so the ring has to be big enough to hold a frame of them rather
// than failing. Reducing how much gets dirtied is a separate fix (the region
// merge coalesces dynamic writes into large static regions).
constexpr uint64_t kUploadCapacityDefaultMiB = 64;

// D3D12Provider::DirectQueueSubmitMutex() is a fork-local addition: it exists
// so the native runtime can serialize its submissions against the emulated
// command processor, which submits to the same direct queue from its own
// thread. A stock RexGlue SDK has no such accessor -- and in nocp mode there is
// no command processor to race with in the first place -- so detect it instead
// of requiring it at compile time.
//
// Where the accessor exists this returns exactly the pointer the code always
// took, so behaviour on this tree is unchanged; where it does not, the null
// result feeds the `if (submit_mutex_)` guard that Submit() already has.
template <typename Provider>
std::mutex* DirectQueueSubmitMutexOrNull(const Provider& provider) {
  if constexpr (requires { provider.DirectQueueSubmitMutex(); }) {
    return &provider.DirectQueueSubmitMutex();
  } else {
    return nullptr;
  }
}
}  // namespace

D3D12Context::~D3D12Context() { Shutdown(); }

bool D3D12Context::Initialize(const rex::ui::d3d12::D3D12Provider& provider) {
  if (initialized_) {
    return true;
  }
  device_ = provider.GetDevice();
  queue_ = provider.GetDirectQueue();
  submit_mutex_ = DirectQueueSubmitMutexOrNull(provider);
  if (!device_ || !queue_) {
    REXLOG_ERROR("[native_gfx] D3D12 provider has no device/queue");
    return false;
  }
  return FinishInitialize();
}

bool D3D12Context::Initialize(DeviceManager& manager) {
  if (initialized_) {
    return true;
  }
  device_ = manager.device();
  queue_ = manager.direct_queue();
  submit_mutex_ = nullptr;  // Owned direct command queue; zero mutex overhead!
  if (!device_ || !queue_) {
    REXLOG_ERROR("[native_gfx] DeviceManager has no device/queue");
    return false;
  }
  return FinishInitialize();
}

bool D3D12Context::FinishInitialize() {
  {  // TEMP DIAG (IQPROBE): a InfoQueue existe E grava? Sem isso, "fila vazia"
     // nao distingue "sem erro" de "camada desligada".
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> iq;
    const bool have = SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&iq))) && iq;
    UINT64 before = 0, after = 0;
    if (have) {
      iq->SetMuteDebugOutput(FALSE);
      // The queue comes back with a storage limit of ZERO, so nothing is ever
      // recorded and "InfoQueue empty" cannot be told apart from "no error".
      // Raise it here: the limit is a property of the queue, not of the layer.
      iq->SetMessageCountLimit(4096);
      before = iq->GetNumStoredMessages();
      iq->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_ERROR, "native_gfx infoqueue probe");
      after = iq->GetNumStoredMessages();
    }
    if (REXCVAR_GET(mcla_native_gfx_diag)) {
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        std::fprintf(f, "IQPROBE have=%d before=%llu after=%llu limit=%llu\n", have ? 1 : 0,
                     (unsigned long long)before, (unsigned long long)after,
                     have ? (unsigned long long)iq->GetMessageCountLimit() : 0ull);
        std::fclose(f);
      }
    }
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
  command_list_->QueryInterface(IID_PPV_ARGS(&command_list7_));
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

  // Read once: the ring size and the buffers that back it must not disagree.
  const uint64_t requested_mib = REXCVAR_GET(mcla_native_gfx_upload_mb);
  upload_capacity_ = (requested_mib ? requested_mib : kUploadCapacityDefaultMiB) << 20;

  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = upload_capacity_;
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
  REXLOG_INFO("[native_gfx] upload ring: {} MiB x {} frames in flight",
              upload_capacity_ >> 20, kFramesInFlight);
  initialized_ = true;
  return true;
}

ID3D12GraphicsCommandList* D3D12Context::BeginFrame() {
  if (!initialized_ || frame_open_) {
    if (REXCVAR_GET(mcla_native_gfx_diag)) {  // TEMP DIAG (BEGINFAIL)
      static uint32_t n = 0;
      if (n++ < 8u) {
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f, "BEGINFAIL init=%d frame_open=%d\n", initialized_ ? 1 : 0,
                       frame_open_ ? 1 : 0);
          std::fclose(f);
        }
      }
    }
    return nullptr;
  }
  // Decided per submission, so the A/B harness can flip them between frames.
  // The submission thread replays recorded streams, so it implies recording.
  const bool threaded = REXCVAR_GET(mcla_native_gfx_submit_thread);
  const bool recording = threaded || REXCVAR_GET(mcla_native_gfx_record_replay);
  if (threaded) {
    StartWorker();
  } else {
    // The direct path resets and records the real list here, which the
    // submission thread may still be replaying into.
    FlushWorker();
  }
  const uint32_t slot = uint32_t(frame_index_ % kFramesInFlight);
  // With a submission thread this fence only completes once that thread has
  // replayed and submitted the slot AND the GPU has finished it, so it also
  // guarantees the slot's recorder, allocator and upload ring are free.
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
  if (!threaded) {
    allocators_[slot]->Reset();
    if (FAILED(command_list_->Reset(allocators_[slot].Get(), nullptr))) {
      REXLOG_ERROR("[native_gfx] command list Reset failed");
      do {  // TEMP DIAG (BEGINFAIL), throttled: this fires on EVERY draw once the
        static uint32_t reset_lines = 0;
        if (reset_lines++ >= 8u) break;
        const HRESULT removed = device_ ? device_->GetDeviceRemovedReason() : S_OK;
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f, "BEGINFAIL reset_failed removed_reason=0x%08X\n",
                       unsigned(removed));
          std::fclose(f);
        }
        DrainDebugMessages("command list Reset");
      } while (false);
      return nullptr;
    }
  }
  upload_offset_[slot] = 0;
  ResetUploadAccounting();
  frame_open_ = true;
  recording_ = recording;
  threaded_ = threaded;
  recording_slot_ = slot;
  if (recording_) {
    recorders_[slot].BeginRecording(device_);
    return &recorders_[slot];
  }
  return command_list_.Get();
}

bool D3D12Context::EndFrame() {
  if (!initialized_ || !frame_open_) {
    return false;
  }
  frame_open_ = false;
  const uint32_t slot = uint32_t(frame_index_ % kFramesInFlight);
  // The fence value is assigned here, on the recording thread, and signalled
  // unconditionally by whoever submits. BeginFrame and EndFrameReleases both
  // reason in these values, so they must exist before the submission does.
  const uint64_t fence = ++fence_value_;
  slot_fence_value_[slot] = fence;
  ++frame_index_;
  const bool recording = recording_;
  const bool threaded = threaded_;
  recording_ = false;
  threaded_ = false;
  if (threaded) {
    {
      std::lock_guard<std::mutex> lock(worker_mutex_);
      worker_jobs_.push_back(SubmitJob{slot, fence, nullptr});
    }
    worker_cv_.notify_one();
    return true;
  }
  return ReplayAndSubmit(slot, fence, recording, /*reset_first=*/false);
}

bool D3D12Context::ReplayAndSubmit(uint32_t slot, uint64_t fence_value, bool replay,
                                   bool reset_first) {
  const auto replay_begin = std::chrono::steady_clock::now();
  bool ok = true;
  if (reset_first) {
    allocators_[slot]->Reset();
    if (FAILED(command_list_->Reset(allocators_[slot].Get(), nullptr))) {
      REXLOG_ERROR("[native_gfx] submission thread: command list Reset failed");
      ok = false;
    }
  }
  if (ok && replay) {
    recorders_[slot].Replay(command_list_.Get());
    replay_commands_.fetch_add(recorders_[slot].command_count(), std::memory_order_relaxed);
  }
  if (ok) {
    const HRESULT close_hr = command_list_->Close();
    if (FAILED(close_hr)) {
      ok = false;
      REXLOG_ERROR("[native_gfx] command list Close failed {:#010x}", uint32_t(close_hr));
      if (REXCVAR_GET(mcla_native_gfx_diag)) {  // TEMP DIAG (CLOSEFAIL)
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f, "CLOSEFAIL hr=0x%08X removed=0x%08X\n", unsigned(close_hr),
                       unsigned(device_ ? device_->GetDeviceRemovedReason() : S_OK));
          std::fclose(f);
        }
      }
      DrainDebugMessages("command list Close");
    }
  }
  // A removed device turns every later creation into a failure somewhere
  // unrelated (textures, PSOs, buffers all at once), which hides the cause.
  // Report it once, at the frame that follows the fault.
  if (ok && !device_removed_reported_) {
    const HRESULT removed = device_->GetDeviceRemovedReason();
    if (FAILED(removed)) {
      device_removed_reported_ = true;
      REXLOG_ERROR("[native_gfx] DEVICE REMOVED: {:#010x}", uint32_t(removed));
      DrainDebugMessages("device removed");
      DumpDeviceRemovedData();
    }
  }
  {
    // Serialize against the Xenia command processor, which submits to this same
    // queue from another thread (see D3D12Provider::DirectQueueSubmitMutex).
    std::unique_lock<std::mutex> submit_lock;
    if (submit_mutex_) {
      submit_lock = std::unique_lock<std::mutex>(*submit_mutex_);
    }
    if (ok) {
      ID3D12CommandList* lists[] = {command_list_.Get()};
      queue_->ExecuteCommandLists(1, lists);
    }
    // Signalled even when nothing was executed: the recording thread waits on
    // this value to reuse the slot, and a value that never arrives would hang
    // it for good.
    queue_->Signal(fence_.Get(), fence_value);
  }
  replay_ns_.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - replay_begin)
                                    .count()),
                       std::memory_order_relaxed);
  return ok;
}

void D3D12Context::StartWorker() {
  if (worker_.joinable()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    worker_stop_ = false;
  }
  worker_ = std::thread([this] { WorkerMain(); });
  SetThreadDescription(worker_.native_handle(), L"MCLA Native Submit");
  REXLOG_INFO("[native_gfx] submission thread started");
}

void D3D12Context::StopWorker() {
  if (!worker_.joinable()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    worker_stop_ = true;
  }
  worker_cv_.notify_all();
  worker_.join();
}

void D3D12Context::WorkerMain() {
  std::unique_lock<std::mutex> lock(worker_mutex_);
  for (;;) {
    worker_cv_.wait(lock, [this] { return worker_stop_ || !worker_jobs_.empty(); });
    if (worker_jobs_.empty()) {
      break;  // stop requested and nothing left to submit
    }
    const SubmitJob job = worker_jobs_.front();
    worker_jobs_.pop_front();
    worker_busy_ = true;
    lock.unlock();
    if (job.task) {
      job.task();
    } else {
      ReplayAndSubmit(job.slot, job.fence_value, /*replay=*/true, /*reset_first=*/true);
    }
    lock.lock();
    worker_busy_ = false;
    if (worker_jobs_.empty()) {
      worker_idle_cv_.notify_all();
    }
  }
  worker_busy_ = false;
  worker_idle_cv_.notify_all();
}

void D3D12Context::FlushWorker() {
  if (!worker_.joinable()) {
    return;
  }
  std::unique_lock<std::mutex> lock(worker_mutex_);
  if (worker_jobs_.empty() && !worker_busy_) {
    return;
  }
  const auto wait_begin = std::chrono::steady_clock::now();
  worker_idle_cv_.wait(lock, [this] { return worker_jobs_.empty() && !worker_busy_; });
  flush_wait_ns_.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - wait_begin)
                                        .count()),
                           std::memory_order_relaxed);
}

namespace {
thread_local bool t_side_list_open = false;
}  // namespace

bool SideListOpenOnThisThread() { return t_side_list_open; }

bool D3D12Context::SubmitThreadActive() {
  return REXCVAR_GET(mcla_native_gfx_submit_thread) && worker_.joinable();
}

void D3D12Context::EnqueueWorkerTask(std::function<void()> task) {
  if (!task) {
    return;
  }
  if (!SubmitThreadActive()) {
    task();
    return;
  }
  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    worker_jobs_.push_back(SubmitJob{0, 0, std::move(task)});
  }
  worker_cv_.notify_one();
}

bool D3D12Context::EnsureSideList() {
  if (side_list_) {
    return true;
  }
  if (side_tried_ || !device_) {
    return false;
  }
  side_tried_ = true;
  for (uint32_t i = 0; i < kSideSlots; ++i) {
    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&side_allocators_[i])))) {
      REXLOG_ERROR("[native_gfx] side list: CreateCommandAllocator failed");
      return false;
    }
  }
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
  if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        side_allocators_[0].Get(), nullptr,
                                        IID_PPV_ARGS(&list)))) {
    REXLOG_ERROR("[native_gfx] side list: CreateCommandList failed");
    return false;
  }
  list->SetName(L"mcla_native_gfx_side");
  list->Close();
  if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&side_fence_)))) {
    REXLOG_ERROR("[native_gfx] side list: CreateFence failed");
    return false;
  }
  side_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!side_event_) {
    REXLOG_ERROR("[native_gfx] side list: event creation failed");
    side_fence_.Reset();
    return false;
  }
  side_list_ = list;
  return true;
}

ID3D12GraphicsCommandList* D3D12Context::BeginSideList() {
  if (side_open_ || !EnsureSideList()) {
    return nullptr;
  }
  const uint32_t slot = side_index_ % kSideSlots;
  if (side_fence_->GetCompletedValue() < side_slot_fence_[slot]) {
    side_fence_->SetEventOnCompletion(side_slot_fence_[slot], side_event_);
    WaitForSingleObject(side_event_, INFINITE);
  }
  side_allocators_[slot]->Reset();
  if (FAILED(side_list_->Reset(side_allocators_[slot].Get(), nullptr))) {
    REXLOG_ERROR("[native_gfx] side list Reset failed");
    return nullptr;
  }
  side_open_ = true;
  t_side_list_open = true;
  return side_list_.Get();
}

bool D3D12Context::EndSideList() {
  if (!side_open_) {
    return false;
  }
  side_open_ = false;
  t_side_list_open = false;
  const uint32_t slot = side_index_ % kSideSlots;
  ++side_index_;
  const bool closed = SUCCEEDED(side_list_->Close());
  if (!closed) {
    REXLOG_ERROR("[native_gfx] side list Close failed");
  }
  {
    std::unique_lock<std::mutex> submit_lock;
    if (submit_mutex_) {
      submit_lock = std::unique_lock<std::mutex>(*submit_mutex_);
    }
    if (closed) {
      ID3D12CommandList* lists[] = {side_list_.Get()};
      queue_->ExecuteCommandLists(1, lists);
    }
    queue_->Signal(side_fence_.Get(), ++side_fence_value_);
  }
  side_slot_fence_[slot] = side_fence_value_;
  return closed;
}

double D3D12Context::TakeFlushWaitUs() {
  return double(flush_wait_ns_.exchange(0, std::memory_order_relaxed)) / 1000.0;
}

void D3D12Context::DrainDebugMessages(const char* context_label) {
  if (!device_) {
    return;
  }
  Microsoft::WRL::ComPtr<ID3D12InfoQueue> info_queue;
  if (FAILED(device_->QueryInterface(IID_PPV_ARGS(&info_queue)))) {
    REXLOG_ERROR("[native_gfx] {}: no D3D12 InfoQueue (debug layer off?)", context_label);
    static uint32_t iq_lines = 0;
    if (REXCVAR_GET(mcla_native_gfx_diag) && iq_lines++ < 8u) {
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        std::fprintf(f, "D3DERR %s: no_infoqueue\n", context_label);
        std::fclose(f);
      }
    }
    return;
  }
  const UINT64 count = info_queue->GetNumStoredMessages();
  if (count == 0) {
    REXLOG_ERROR("[native_gfx] {}: InfoQueue empty", context_label);
    static uint32_t iq_lines = 0;
    if (REXCVAR_GET(mcla_native_gfx_diag) && iq_lines++ < 8u) {
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        std::fprintf(f, "D3DERR %s: infoqueue_empty\n", context_label);
        std::fclose(f);
      }
    }
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
      // TEMP DIAG: also to the diag file -- under RenderDoc's ExecuteAndInject
      // the process stdout is not readable, and this is the only place that
      // says WHY a command list failed to close.
      static uint32_t iq_lines = 0;
      if (REXCVAR_GET(mcla_native_gfx_diag) && iq_lines++ < 8u) {
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f, "D3DERR %s: [sev %d id %d] %s\n", context_label,
                       int(message->Severity), int(message->ID), text);
          std::fclose(f);
        }
      }
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

namespace {
// Per-frame accounting of who consumed the upload ring, reset at BeginFrame.
constexpr size_t kUploadTagCount = size_t(D3D12Context::UploadTag::kCount);
uint64_t g_upload_bytes[kUploadTagCount] = {};
uint32_t g_upload_calls[kUploadTagCount] = {};
bool g_upload_reported = false;

const char* UploadTagName(D3D12Context::UploadTag t) {
  switch (t) {
    case D3D12Context::UploadTag::kConstants: return "constants";
    case D3D12Context::UploadTag::kTexture:   return "texture";
    case D3D12Context::UploadTag::kGeometry:  return "geometry";
    case D3D12Context::UploadTag::kCapture:   return "capture";
    default:                                  return "other";
  }
}
}  // namespace

void D3D12Context::ResetUploadAccounting() {
  for (size_t i = 0; i < kUploadTagCount; ++i) {
    g_upload_bytes[i] = 0;
    g_upload_calls[i] = 0;
  }
  g_upload_reported = false;
}

bool D3D12Context::AllocateUpload(uint64_t size, uint64_t alignment, UploadAlloc& out,
                                  UploadTag tag) {
  if (!frame_open_ || size == 0) {
    return false;
  }
  const uint32_t slot = uint32_t(frame_index_ % kFramesInFlight);
  const uint64_t offset = (upload_offset_[slot] + alignment - 1) & ~(alignment - 1);
  if (offset + size > upload_capacity_) {
    // One breakdown per frame, not one line per failed allocation: the old
    // message fired thousands of times and never said who filled the ring.
    if (!g_upload_reported) {
      g_upload_reported = true;
      REXLOG_ERROR(
          "[native_gfx] upload ring exhausted at {}/{} bytes, {} request by {}. This frame: "
          "constants {} KiB/{} allocs, texture {} KiB/{}, geometry {} KiB/{}, capture {} KiB/{}, "
          "other {} KiB/{}",
          offset, upload_capacity_, size, UploadTagName(tag),
          g_upload_bytes[0] >> 10, g_upload_calls[0], g_upload_bytes[1] >> 10, g_upload_calls[1],
          g_upload_bytes[2] >> 10, g_upload_calls[2], g_upload_bytes[3] >> 10, g_upload_calls[3],
          g_upload_bytes[4] >> 10, g_upload_calls[4]);
    }
    return false;
  }
  g_upload_bytes[size_t(tag)] += size;
  ++g_upload_calls[size_t(tag)];
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

double D3D12Context::TakeReplayUs(uint64_t* out_commands) {
  const uint64_t ns = replay_ns_.exchange(0, std::memory_order_relaxed);
  const uint64_t cmds = replay_commands_.exchange(0, std::memory_order_relaxed);
  if (out_commands) {
    *out_commands = cmds;
  }
  return double(ns) / 1000.0;
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
  // Everything handed to the submission thread has to be on the queue before
  // the idle signal, or "idle" would not include it.
  FlushWorker();
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
  StopWorker();
  if (side_event_) {
    CloseHandle(side_event_);
    side_event_ = nullptr;
  }
  side_list_.Reset();
  side_fence_.Reset();
  for (auto& a : side_allocators_) {
    a.Reset();
  }
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
  command_list7_.Reset();
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
