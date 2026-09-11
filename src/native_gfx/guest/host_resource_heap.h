#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — guest-addressable host objects
// ===========================================================================
// Allocates one object out of HostHeap (so it lives in guest memory and the
// game can hold a pointer to it), placement-news the host type into it, and
// records the guest VA in a registry.
//
// The contract every type used here must honour: its FIRST bytes are the
// Xbox 360 D3D layout from d3d_structs.h, because the engine reads those
// fields off offset 0 without going through any D3D entry point at all --
// D3DResource::Common for the type bits, ReferenceCount which it increments
// and decrements directly. Host-side fields live after that prefix.
//
// The registry is what makes FromGuest safe. The game seeds its render-target
// and texture slots with sentinel values that are not resources, so a bare
// "translate the VA and cast" would hand back a garbage object; a VA that was
// never allocated here resolves to nullptr instead.
// ===========================================================================

#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace mcla::native_gfx {

enum class ResourceType : uint32_t {
  kTexture = 0,
  kVolumeTexture = 1,
  kVertexBuffer = 2,
  kIndexBuffer = 3,
  kRenderTarget = 4,
  kDepthStencil = 5,
  kVertexDeclaration = 6,
  kVertexShader = 7,
  kPixelShader = 8,
};

class HostResourceHeap {
 public:
  // Allocates T in guest memory and constructs it. T must expose `type`
  // (ResourceType) and `self_va` (uint32_t); self_va is filled in here.
  // Returns nullptr when the heap is exhausted (HostHeap logs once).
  template <typename T, typename... Args>
  static T* Alloc(Args&&... args) {
    static_assert(std::is_destructible_v<T>, "T must be destructible");
    void* host = AllocRaw(sizeof(T), 16);
    if (!host) {
      return nullptr;
    }
    // Zero first so the Xbox 360 header bytes are defined before the create
    // path writes the real type bits: the engine may read them before the
    // object is fully set up.
    std::memset(host, 0, sizeof(T));
    T* obj = new (host) T(std::forward<Args>(args)...);
    const uint32_t guest = GuestAddressOf(host);
    obj->self_va = guest;
    Register(guest, obj->type);
    return obj;
  }

  template <typename T>
  static void Free(T* host_ptr) {
    if (!host_ptr) {
      return;
    }
    Unregister(GuestAddressOf(host_ptr));
    host_ptr->~T();
    FreeRaw(host_ptr);
  }

  // Resolves a guest VA back to its host object, or nullptr when the VA was
  // not allocated here. There is no runtime type information in the header,
  // so the caller has to know which T it is asking for; GetType tells it.
  template <typename T>
  static T* FromGuest(uint32_t guest_va) {
    if (!guest_va || !IsRegistered(guest_va)) {
      return nullptr;
    }
    return static_cast<T*>(HostAddressOf(guest_va));
  }

  template <typename T>
  static uint32_t ToGuest(T* host_ptr) {
    return host_ptr ? GuestAddressOf(host_ptr) : 0u;
  }

  // The type a VA was registered with. False when the VA is not ours.
  static bool GetType(uint32_t guest_va, ResourceType* out_type);

  // Live object count, for diagnostics.
  static uint64_t LiveCount();

 private:
  static void* AllocRaw(uint32_t size, uint32_t alignment);
  static void FreeRaw(void* host_ptr);
  static uint32_t GuestAddressOf(const void* host_ptr);
  static void* HostAddressOf(uint32_t guest_va);

  static void Register(uint32_t guest_va, ResourceType type);
  static void Unregister(uint32_t guest_va);
  static bool IsRegistered(uint32_t guest_va);
};

}  // namespace mcla::native_gfx
