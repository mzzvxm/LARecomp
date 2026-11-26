#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace mcla::gpu::cpu_timing {

inline constexpr std::size_t kWindowFrames = 600;

enum class Section : uint8_t {
  kResolveCall,      // ResolveCache::Execute: all of one resolve record
  kResolveEdram,     // ... the EDRAM ownership notes and the inherited copies it drains
  kResolveBefore,    // ... ResolveMemory::BeforeResolve: the waiting encodes it walks
  kResolveActivate,  // ... ResolveCache::Activate: the product's lookup, creation, overlap walk
  kResolveLayout,    // ... ... the guest layout of the key, worked out when a product is made
  kResolveArm,       // ... ... the Arm calls of a new activation (the SDK's page protection)
  kResolveCells,     // ... the coverage and inherited cell bits a copy sets
  kEncode,           // ResolveMemory::Encode: one copy into the resolve memory
  kEncodeLayout,     // ... GetTextureLevelStorage: the destination's layout, built at every call
  kEncodePages,      // ... the pages it touched: chunks placed, partial pages to upload
  kEncodeOrder,      // ... the waiting encodes over its pages, recorded first (FlushFor)
  kEncodeMark,       // ... the page flags, sequence numbers and writers
  kTouchPages,       // ResolveMemory::TouchPages: the pages a rectangle writes, tile or row walk
  kUploadPages,      // ResolveMemory::UploadTable and UploadPages: guest pages into the upload ring
  kArmPages,         // ResolveMemory::ArmPages: written pages armed for CPU writes
  kTouchDecoded,     // ResolveMemory::TouchDecoded: the decoded textures over a written range
  kBatchChoose,      // ResolveMemory::Choose: which waiting encodes a flush records
  kBatchRecord,      // ResolveMemory::Record: the waiting encodes' dispatches into the list
  kHolds,            // ResolveMemory::Holds: does the resolve memory hold a texture
  kHoldsLayout,      // ... the guest ranges of a key the memo did not have
  kMemoryBind,       // ResolveMemory::Bind: a texture decoded from the resolve memory
  kLock,             // ResolveCache::OnLock: a CPU read of resolved memory, with its write-back
  kLockLayout,       // ... LockReadAheadRanges: a layout for every level of the locked texture
  kLayoutOther,      // GetTextureLevelStorage elsewhere (an inherited copy, a read-back)
  kSync,             // TextureCache::Sync: the CPU writes seen since the last draw
  kTextureTables,    // Scene::FillTextureTables: the fetch memo, texture binds, sampler lookups
  kConstantsUpload,  // Scene::UploadConstants: the constant banks into the upload ring
  kBindConstants,    // the root descriptors or push constants of a draw
  kDrawDispatch,
  kDraw,
  kResolveDispatch,
  kBanks,
  kPrepare,
  kDecode,
  kInputLayout,
  kGeometry,
  kPipelineKey,
  kPipelineLookup,
  kPipelineWait,
  kRecord,
  kTargets,
  kDrawBindings,
  kTextureBind,
  kTextureUpload,
  kGeometryUpload,
  kUploadRing,
  kVariantConstants,
  kEnvironment,
  kPlanar,
  kShadowView,
  kUiView,
  kMirrorPrepare,
  kMirrorRecord,
  kMirrorStamp,
  kMirrorOverlay,
  kMirrorRt,
  kCarCollect,
  kCarShadow,
  kCarHistory,
  kShadowCollect,
  kShadowReplay,
  kGBufferKey,
  kGBufferAttach,
  kGBufferPublish,
  kRtBuild,
  kRtCarStage,
  kRtCity,
  kRtInstances,
  kRtDescriptors,
  kRtWorld,
  kFrameBegin,
  kFrameEnd,
  kPresentBookkeeping,
  kPresent,
  kEndOfList,
  kDrawPrefetch,
  kAmbientOcclusion,
  kScreenCapture,
  kReflections,
  kVolumetrics,
  kShafts,
  kLocalLights,
  kGiField,
  kGiApply,
  kTemporal,
  kHdrDuplicate,
  kUpscalerMask,
  kUiSharp,
  kClear,
  kCount,
};


namespace detail {
inline std::atomic<bool> g_on{false};
bool Begin(Section section);
void End();
}  // namespace detail

// Only the render thread arms the stopwatch. When off each scope reads a cached flag.
inline bool Enabled() { return detail::g_on.load(std::memory_order_relaxed); }

class Scope {
 public:
  explicit Scope(Section section) {
    if (Enabled() && section != Section::kCount) {
      active_ = detail::Begin(section);
    }
  }
  ~Scope() { Stop(); }
  void Stop() {
    if (active_) {
      active_ = false;
      detail::End();
    }
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  bool active_ = false;
};

// On the render thread. FinishFrame runs after the guest present and switches the setting for the
// next guest frame. Publish one line per complete scene window. An idle present has no boundary.
void BeginSession();
void FinishFrame(uint64_t guest_frames);
void EndSession();

}  // namespace mcla::gpu::cpu_timing
