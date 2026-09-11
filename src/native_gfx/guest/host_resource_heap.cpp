#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — guest-addressable host objects.
// See host_resource_heap.h for the contract.

#include "host_resource_heap.h"

#include <mutex>
#include <unordered_map>

#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "host_heap.h"

namespace mcla::native_gfx {

namespace {

struct Registry {
  std::mutex mutex;
  std::unordered_map<uint32_t, ResourceType> by_va;
};

Registry& registry() {
  static Registry r;
  return r;
}

rex::memory::Memory* GuestMemory() {
  auto* kernel = rex::system::KernelState::shared();
  return kernel ? kernel->memory() : nullptr;
}

}  // namespace

void* HostResourceHeap::AllocRaw(uint32_t size, uint32_t alignment) {
  return HostHeap::Get().Alloc(size, alignment);
}

void HostResourceHeap::FreeRaw(void* host_ptr) {
  HostHeap::Get().Free(host_ptr);
}

uint32_t HostResourceHeap::GuestAddressOf(const void* host_ptr) {
  auto* memory = GuestMemory();
  if (!memory || !host_ptr) {
    return 0;
  }
  return memory->HostToGuestVirtual(const_cast<void*>(host_ptr));
}

void* HostResourceHeap::HostAddressOf(uint32_t guest_va) {
  auto* memory = GuestMemory();
  if (!memory || !guest_va) {
    return nullptr;
  }
  return memory->TranslateVirtual<void*>(guest_va);
}

void HostResourceHeap::Register(uint32_t guest_va, ResourceType type) {
  if (!guest_va) {
    return;
  }
  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  r.by_va[guest_va] = type;
}

void HostResourceHeap::Unregister(uint32_t guest_va) {
  if (!guest_va) {
    return;
  }
  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  r.by_va.erase(guest_va);
}

bool HostResourceHeap::IsRegistered(uint32_t guest_va) {
  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  return r.by_va.find(guest_va) != r.by_va.end();
}

bool HostResourceHeap::GetType(uint32_t guest_va, ResourceType* out_type) {
  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  const auto it = r.by_va.find(guest_va);
  if (it == r.by_va.end()) {
    return false;
  }
  if (out_type) {
    *out_type = it->second;
  }
  return true;
}

uint64_t HostResourceHeap::LiveCount() {
  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  return uint64_t(r.by_va.size());
}

}  // namespace mcla::native_gfx

#endif  // REXGLUE_HAS_XEO3_TARGET
