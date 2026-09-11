#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — texture / sampler binding
// ===========================================================================
// The whole chain, end to end, taken from the XenosRecomp generator rather
// than assumed (shader_recompiler.cpp, RegisterSet::Sampler case):
//
//   fetchSlot = isPixelShader ? registerIndex : registerIndex + 16
//               ^ VERTEX_SHADER_FETCH_CONSTANT_BASE
//
//   Texture2D index   at SharedConstants byte  0*32*4 + fetchSlot*4
//   Texture3D index   at                       1*32*4 + fetchSlot*4
//   TextureCube index at                       2*32*4 + fetchSlot*4
//   Sampler index     at                       3*32*4 + fetchSlot*4
//
// and the cbuffer form of the same thing:
//   packoffset(c{dimension*8 + fetchSlot/4}.{xyzw[fetchSlot%4]})
// which is byte (dimension*32 + fetchSlot)*4 — identical. Verified against a
// real shader: DiffuseSampler at c0.x (byte 0, slot 0) with its sampler at
// c24.x (byte 384 = 3*32*4 + 0), NeonSampler at c0.y (byte 4, slot 1).
//
// So the value written at slot N is a DESCRIPTOR HEAP INDEX, and the slot
// number is the Xenos texture fetch constant slot — with vertex shaders
// offset by 16, because the guest splits the 32 texture fetch slots as
// 0..15 for the pixel shader and 16..31 for the vertex shader.
//
// IMPORTANT: those +16 slots are the ones the shader ADDRESSES. The device's
// fetch constant shadow is indexed by the same slot number, so no extra
// translation is needed — but the offset must not be applied twice.
// ===========================================================================

#include <cstdint>
#include <unordered_map>

#include <rex/ui/d3d12/d3d12_api.h>

#include "../guest/sampler_state.h"
#include "../guest/texture_format.h"
#include "../guest/guest_resources.h"  // kFetchGroupDwords
#include "texture_cache.h"  // TextureSource

namespace mcla::native_gfx {

class D3D12Context;

// SharedConstants layout (bytes), matching the generator exactly.
inline constexpr uint32_t kFetchConstantSlotCount = 32;
inline constexpr uint32_t kSharedTexture2DTableByteOffset = 0;
inline constexpr uint32_t kSharedTexture3DTableByteOffset = 1 * kFetchConstantSlotCount * 4;
inline constexpr uint32_t kSharedTextureCubeTableByteOffset = 2 * kFetchConstantSlotCount * 4;
inline constexpr uint32_t kSharedSamplerTableByteOffset = 3 * kFetchConstantSlotCount * 4;

// Pixel shaders address fetch slots 0..15; vertex shaders 16..31.
inline constexpr uint32_t kVertexShaderFetchSlotBase = 16;

// Byte offset of the descriptor index for a slot, per texture dimension.
inline uint32_t SharedTextureIndexByteOffset(uint32_t dimension, uint32_t fetch_slot) {
  return dimension * kFetchConstantSlotCount * 4 + fetch_slot * 4;
}
inline uint32_t SharedSamplerIndexByteOffset(uint32_t fetch_slot) {
  return kSharedSamplerTableByteOffset + fetch_slot * 4;
}

// What was bound at one fetch slot, for diagnosis and for the draw.
struct BoundTexture {
  uint32_t fetch_slot = 0;
  TextureFetch fetch;
  SamplerDescription sampler;
  uint32_t srv_descriptor_index = 0;
  uint32_t sampler_descriptor_index = 0;
  ID3D12Resource* resource = nullptr;
  bool resolved = false;
  // Which path produced `resource`. `resolved` only says something came back;
  // this says whether to trust it. See TextureSource in texture_cache.h.
  TextureSource source = TextureSource::kUnresolved;
};

// Owns the shader-visible descriptor heaps and the guest -> descriptor
// caches. One SRV heap and one sampler heap, both shader-visible, since the
// shaders index them directly.
class TextureBinder {
 public:
  struct Stats {
    uint64_t srv_hits = 0;
    uint64_t srv_misses = 0;
    uint64_t sampler_hits = 0;
    uint64_t sampler_misses = 0;
    uint64_t unresolved = 0;
    // Descriptor half ran out. This is the worst failure mode in the binder: the
    // acquire returns index 0, so the draw proceeds and samples whatever sits at
    // slot 0 -- a WRONG PICTURE, silently, with only a latched one-shot log.
    // Surfaced so raising the draw cap cannot trade "missing lighting" for
    // "wrong texture" unnoticed.
    uint64_t srv_exhausted = 0;
    uint64_t sampler_exhausted = 0;
    // Consecutive draws that bound byte-identical fetch constants and reused
    // the previous draw's whole result instead of re-resolving 32 slots.
    uint64_t memo_hits = 0;
    uint64_t memo_misses = 0;
    // A result that was correct but deliberately NOT memoised, and why. A
    // render-target-sourced or fallback bind is refused because the resource
    // behind it can be replaced between two draws whose fetch constants never
    // change, which the byte comparison alone would not notice.
    uint64_t memo_refused_volatile = 0;
    // Split of the above, because "every draw is volatile" names two very
    // different problems: a bind the cache could not serve at all, and a bind
    // served by the render-target bridge.
    uint64_t memo_refused_fallback = 0;
    // Not a refusal any more, kept as a tally: measured at 11919 per frame with
    // EVERY draw binding at least one, which is what killed the first version
    // of the memo (it refused any result containing one, so it never stored a
    // single entry). Almost certainly the shadow atlas, which every material
    // shader samples. These are covered by `volatile_guard` instead.
    uint64_t memo_bridge_binds = 0;
    // Split of memo_misses. A miss because the GUARD moved is a pool resolve
    // or texture upload invalidating the memo; a miss because the KEY differs
    // is simply the next draw binding other textures. The two point at
    // opposite fixes, and only the first one is ours to tune.
    uint64_t memo_miss_guard = 0;
    uint64_t memo_miss_key = 0;
  };

  bool Initialize(D3D12Context& context);
  void Shutdown(D3D12Context& context);

  ID3D12DescriptorHeap* srv_heap() const { return srv_heap_.Get(); }
  ID3D12DescriptorHeap* sampler_heap() const { return sampler_heap_.Get(); }

  // Resolves every texture fetch constant the device has bound, fills the
  // SharedConstants tables in `shared_bytes` (kSharedConstantsBytes) and
  // returns what was bound for diagnosis.
  // `volatile_guard` is any monotone counter from OUTSIDE the binder whose
  // change means a resource may have moved under an unchanged fetch constant.
  // The caller passes the render target pool's creation/resolve tallies: the
  // bridge hands back whatever resource currently backs a guest address, and a
  // resolve mid-frame can replace it while the fetch constants never move.
  void BindAll(D3D12Context& context, ID3D12GraphicsCommandList* cl, const uint8_t* base,
               uint32_t dev, TextureCache& textures, uint8_t* shared_bytes,
               BoundTexture* out, uint32_t* out_count, uint32_t out_capacity,
               uint64_t volatile_guard);

  const Stats& stats() const { return stats_; }

  // Starts a new native frame's descriptor allocation in the OTHER half of the
  // (double-buffered) shader-visible heaps and clears the per-frame descriptor
  // caches. Because the SRV cache keys on the raw ID3D12Resource* (which D3D12
  // recycles after a free), a descriptor cached last frame can otherwise alias a
  // freed/reused resource; and the monotonic cursor overran the heap in a few
  // frames. Resetting into the alternate half each frame gives fresh descriptors
  // without overwriting the previous (still in-flight) frame's half. Call once
  // per continuous frame at the frame boundary.
  void BeginFrame();

 private:
  uint32_t AcquireSrv(D3D12Context& context, ID3D12Resource* resource, const TextureFetch& fetch,
                      TextureSource source);
  // Neutral stand-in for a fetch the cache cannot serve. Leaving the index at
  // 0 makes the shader sample descriptor 0 — an unrelated texture — which is
  // worse than a defined value: for the shadow atlas every material shader
  // then shades against arbitrary image data. White reads as "unshadowed" /
  // "fully lit", the neutral term for the depth and mask lookups this covers.
  uint32_t FallbackSrv(D3D12Context& context, ID3D12GraphicsCommandList* cl);
  Microsoft::WRL::ComPtr<ID3D12Resource> fallback_texture_;  // persistent, created once
  uint32_t fallback_srv_ = 0;
  bool fallback_ready_ = false;      // fallback_texture_ has been created
  bool fallback_filled_ = false;     // white pixel uploaded + transitioned (one time)
  bool fallback_srv_valid_ = false;  // fallback SRV allocated in the CURRENT frame's half
  uint32_t AcquireSampler(D3D12Context& context, const SamplerDescription& sampler);

  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_heap_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> sampler_heap_;
  uint32_t srv_increment_ = 0;
  uint32_t sampler_increment_ = 0;
  uint32_t srv_next_ = 0;
  uint32_t sampler_next_ = 0;
  // Double-buffering: each native frame allocates from one half of the heap, the
  // previous frame's half stays untouched while the GPU still reads it.
  uint32_t frame_parity_ = 0;    // 0 or 1: which half the current frame uses
  uint32_t srv_base_ = 0;        // current half's first SRV index
  uint32_t sampler_base_ = 0;    // current half's first sampler index
  bool srv_warned_ = false;      // exhaustion logged once this frame (no I/O spiral)
  bool sampler_warned_ = false;
  // Guest identity -> descriptor index.
  std::unordered_map<uint64_t, uint32_t> srv_cache_;
  std::unordered_map<uint64_t, uint32_t> sampler_cache_;

  // ---- One-draw memo -------------------------------------------------------
  // Measured baseline: ~15 bound slots x ~4200 draws = ~63000 slot binds per
  // frame at ~350ns each, which was 22ms of a 95ms frame. The per-slot work was
  // already correct -- the cost was doing it again for consecutive draws that
  // bind exactly the same textures, which in a pass is most of them.
  //
  // The memo keys on the RAW fetch constant shadow, all 32 slots, byte for
  // byte. That is the same memory the loop reads anyway, so taking the key
  // costs nothing extra. `dev` is part of the key because two devices have
  // separate shadows.
  //
  // Validity is the delicate part, and three things bound it:
  //   * BeginFrame() drops it. Descriptor indices live in one half of a
  //     double-buffered heap and are re-allocated each frame, so an index from
  //     the previous frame names a descriptor that no longer holds this view.
  //   * The texture cache's mutation counters are part of the guard. An upload,
  //     eviction or decode failure between two draws can change which resource
  //     a fetch resolves to while the fetch constants stay identical.
  //   * A result containing a FALLBACK texture is never stored: white is a
  //     placeholder for a bind that failed, and reusing it would keep serving
  //     white after the texture became resolvable.
  // A render-target-sourced bind is NOT refused, though it is the same hazard:
  // refusing measured out at 100% of draws (every one binds at least one, very
  // likely the shadow atlas), so the memo never stored anything. The pool's
  // creation/resolve tallies ride in `volatile_guard` instead, which breaks the
  // memo exactly when a target is created or resolved and not otherwise.
  static constexpr uint32_t kMemoShadowDwords = kFetchConstantSlotCount * kFetchGroupDwords;
  static constexpr uint32_t kMemoSharedBytes = kSharedSamplerTableByteOffset +
                                               kFetchConstantSlotCount * 4;
  bool memo_valid_ = false;
  uint32_t memo_dev_ = 0;
  uint64_t memo_guard_ = 0;
  uint32_t memo_shadow_[kMemoShadowDwords] = {};
  uint8_t memo_shared_[kMemoSharedBytes] = {};
  BoundTexture memo_out_[kFetchConstantSlotCount] = {};
  uint32_t memo_out_count_ = 0;

  Stats stats_;
};

}  // namespace mcla::native_gfx
