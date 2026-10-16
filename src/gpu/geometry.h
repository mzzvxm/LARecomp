#pragma once
// ===========================================================================
// MCLA GPU renderer -- the guest's vertex and index data on the GPU
// ===========================================================================
// For every draw,
// Prepare resolves what the input assembler reads -- one vertex buffer view
// per stream of the input layout (+ the zero stream), the index buffer view,
// the final host topology and the draw call -- from guest memory or from the
// record itself (inline vertices).
//
// Regions. A region is one exact physical range (RegionIndex, the unit the
// write watch arms); a region has one VIEW per interpretation of its bytes --
// the swap plan of a vertex stream (input_layout.h), or the width and endian
// of an index buffer. Each region is in one of two modes:
//   watched  armed; a view's device copy is valid while the region's version
//            (bumped by every write notification) is the one it was copied at;
//   hashed   not armed; every frame of use compares the XXH3 of the raw bytes
//            with the device copy's. A region enters it after changing in
//            kDynamicStreak consecutive frames of use (faults on every frame
//            cost more than one hash), or when the watch cannot cover it (a
//            read-only page, guest_memory.h IsWatchable); it returns to
//            watched after kStableStreak frames of use without a change
//            (never, when it cannot be watched), and every time it has to come
//            back to hashed the wait doubles, up to kMaxStableStreak: a region
//            the guest rewrites every frame with the same bytes would
//            otherwise bounce between the modes (measured, 24.24).
// A view that is not valid on the device is copied from guest memory into the
// frame slot's upload ring (watched: armed BEFORE the copy) and bound from
// there. The ring is write-combined upload memory: everything is built in
// cached scratch memory and written to the ring in one sequential copy --
// reading the ring back (an index buffer's largest index, say) is an uncached
// read per access and cost 2.5 ms/frame for 0.6 MB when it was done (24.24) -- upload memory is readable by the GPU as is, so no barrier happens
// inside a frame -- and its promotion is queued: at the end of the frame, one
// batch allocates device ranges (BufferHeap, the old range retired), copies
// ring -> device and moves the touched pages to their read state.
//
// Host-made data, per draw in the ring: inline vertices (swap plan of the
// stream with the record's stride), rectangle lists (fourth vertex
// synthesized, topology.h), the expansion of the four computed-fetch vertex
// shaders (vertex v = source (start + v) / 4 of every stream, memory
// project_mcla_quad_fold_shaders), indices converted from an indexed draw.
// Generated 0-based indices of the converted types live in one buffer per
// conversion, grown on demand: their pattern for N vertices is a prefix of the
// pattern for more (line loops excepted: per draw).
//
// Strip cut: Plume always enables primitive restart at 0xFFFF on strips
// (topology.h). An indexed strip whose restart semantics differ -- 32-bit
// indices, a 16-bit 0xFFFF with the guest's reset off, a reset index other
// than 0xFFFF -- is drawn as a list converted on the CPU; the final topology is
// the geometry's, which the pipeline key then uses.
//
// Threading: render thread only, after Initialize.
// ===========================================================================

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>

#include "buffer_heap.h"
#include "draw_decode.h"
#include "draw_state.h"
#include "guest_device.h"
#include "guest_memory.h"
#include "input_layout.h"
#include "region_index.h"
#include "topology.h"
#include "upload.h"

namespace mcla::gpu {

class Device;
class FrameRing;

struct DrawGeometry {
  static constexpr uint32_t kMaxSlots = guest::kStreamCount + 1;

  plume::RenderPrimitiveTopology topology = plume::RenderPrimitiveTopology::UNKNOWN;
  std::array<plume::RenderVertexBufferView, kMaxSlots> vertex_views{};
  uint32_t slot_count = 0;  // = the input layout's slots
  bool indexed = false;
  plume::RenderIndexBufferView index_view;
  uint32_t count = 0;       // host indices (indexed) or vertices
  uint32_t first = 0;       // first index (indexed) or first vertex
  int32_t base_vertex = 0;  // indexed only
};

class GeometryCache {
 public:
  // mcla_gpu_scene_profile: time the stages of Prepare (the Scene sets it).
  void SetProfile(bool on) { profile_ = on; }
  // Frames of use in a row with a change before a watched region is hashed.
  static constexpr uint32_t kDynamicStreak = 3;
  // Frames of use in a row without a change before a hashed region is watched
  // again; doubled at every return to hashed, up to kMaxStableStreak.
  static constexpr uint32_t kStableStreak = 60;
  static constexpr uint32_t kMaxStableStreak = 7680;

  enum class Result : uint8_t {
    kOk,
    kEmpty,             // the count makes no primitive: the draw has no effect
    kNotVertexFetch,    // a stream's fetch constant is not a vertex fetch
    kStreamEndian,      // a stream with an endian other than 8in32 (24.23)
    kUnreadable,        // a range is not committed guest memory
    kUpload,            // ring or device memory could not be had (logged)
    kFoldStart,         // computed fetch with a start that is not a multiple of 4
    kFoldPrimitive,     // computed fetch on anything but an auto quad list
    kRectNoPosition,    // rectangle list without a float POSITION to pick the diagonal
    kIndexedRectList,   // its fourth vertex has no place in the guest's buffer
    kInlineLayout,      // inline vertices with another stream or a stride not in dwords
    kCount,
  };
  static const char* ResultName(Result result);

  GeometryCache() = default;
  ~GeometryCache();
  GeometryCache(const GeometryCache&) = delete;
  GeometryCache& operator=(const GeometryCache&) = delete;

  bool Initialize(Device& device, FrameRing& frames, UploadRing& upload, GuestMemory& memory);
  // The GPU must be idle and the FrameRing drained.
  void Shutdown();

  void BeginFrame(uint32_t slot);
  // Records the frame's promotions into `list` (outside any render pass, the
  // frame's last list). Null `list`: the frame is abandoned, nothing promoted.
  void EndFrame(plume::RenderCommandList* list);

  // `primitive`: the host primitive of the guest's type (topology.h);
  // `computed_fetch`: the vertex shader takes a vfetch index from a register.
  Result Prepare(const DrawView& view, const XenosState& state, const InputLayout& layout,
                 HostPrimitive primitive, bool computed_fetch, DrawGeometry& out,
                 std::string* detail);

  // Regions unused for this many frames give their device memory back.
  static constexpr uint64_t kEvictFrames = 600;

  // One line of statistics since the last call, per frame over `frames`.
  std::string TakeSummary(uint64_t frames);

 private:
  enum class Mode : uint8_t { kWatched, kHashed };

  struct View {
    uint64_t interpretation = 0;  // plan hash, or index width|endian
    BufferHeap::Allocation device;
    uint64_t device_version = 0;  // watched: region version of the device copy (0 = none)
    uint64_t device_hash = 0;     // hashed: XXH3 of the raw bytes of the device copy
    bool device_hash_valid = false;
    UploadSpan ring;              // copy in this frame's ring
    uint64_t ring_frame = 0;
    uint64_t ring_version = 0;
    uint64_t ring_hash = 0;
    bool promotion_queued = false;
    bool index = false;           // lives in the index heap
    uint32_t host_bytes = 0;      // bytes of the converted copy
    uint32_t max_index = 0;       // index views: largest index of the copy
    bool has_ffff = false;        // 16-bit index views: a 0xFFFF in the copy
  };
  struct Region {
    bool live = false;
    Mode mode = Mode::kWatched;
    bool watchable = true;
    bool armed = false;
    uint64_t version = 1;
    uint64_t last_frame = 0;      // frame of the last use
    bool changed_in_frame = false;
    uint32_t changed_streak = 0;
    uint32_t stable_streak = 0;
    uint32_t stable_required = kStableStreak;
    bool was_hashed = false;      // has been hashed before: the next return backs off
    uint64_t last_hash = 0;       // hashed: the raw bytes at the last frame of use
    bool last_hash_valid = false;
    uint64_t hash_frame = 0;      // frame `frame_hash` was computed in
    uint64_t frame_hash = 0;
    bool copied_once = false;     // a copy was made: the next one is a change
    uint64_t copied_version = 0;  // watched: the version of the last copy
    std::vector<View> views;
  };
  // One generated-index buffer per conversion: the prefix property.
  struct Generated {
    BufferHeap::Allocation device;
    uint32_t capacity = 0;        // host indices held
    UploadSpan ring;
    uint64_t ring_frame = 0;
    uint32_t ring_capacity = 0;
    bool promotion_queued = false;
  };
  struct Promotion {
    bool generated = false;
    uint32_t region = 0;          // region id, or the conversion
    uint32_t view = 0;
  };
  // How the bytes of a view are produced from the guest's raw bytes.
  struct Interpretation {
    bool index = false;
    // vertices
    const StreamPlan* plan = nullptr;
    int32_t position_offset = -1;  // a float POSITION in the stream: checked when copied
    // indices
    bool index32 = false;
    uint32_t endian = 0;
    uint64_t Key() const;
  };

  Result PrepareDraw(const DrawView& view, const XenosState& state, const InputLayout& layout,
                     HostPrimitive primitive, bool computed_fetch, DrawGeometry& out,
                     std::string* detail);
  void DrainInvalidations();
  void EvictSome();
  bool ReadIndices(uint32_t address, uint32_t count, bool index32, uint32_t endian,
                   Result* failure);
  Region* FindOrCreateRegion(uint32_t start, uint32_t length, uint32_t* id, Result* failure);
  // A region's view as a buffer reference valid for this frame's draws.
  bool UseView(uint32_t start, uint32_t length, const Interpretation& how,
               plume::RenderBufferReference* ref, View** out_view, Result* failure);
  void Convert(const Interpretation& how, const uint8_t* src, uint32_t length, uint8_t* dst,
               View& view);
  bool GeneratedIndices(PrimitiveConversion conversion, uint32_t count,
                        plume::RenderBufferReference* ref);
  bool StreamSource(const DrawView& view, const StreamPlan& plan, uint32_t* address,
                    uint32_t* size, Result* failure, std::string* detail);
  bool SynthesizeRects(const DrawView& view, const uint8_t* be, uint32_t vertices,
                       const StreamPlan& plan, uint8_t* out);
  UploadSpan Ring(uint64_t size, uint64_t alignment);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  UploadRing* upload_ = nullptr;
  GuestMemory* memory_ = nullptr;
  GuestMemory::Subscriber subscriber_ = GuestMemory::kNoSubscriber;
  BufferHeap vertex_heap_;
  BufferHeap index_heap_;
  BufferHeap::Allocation zero_buffer_;
  bool zero_buffer_ready_ = false;
  UploadSpan zero_ring_;

  RegionIndex index_;
  std::vector<Region> regions_;
  std::array<Generated, kPrimitiveConversionCount> generated_{};
  std::vector<Promotion> promotions_;
  std::vector<GuestMemory::Range> drained_;
  std::vector<uint32_t> scratch_indices_;
  std::vector<uint32_t> scratch_converted_;
  std::vector<uint8_t> scratch_bytes_;
  std::vector<uint8_t> scratch_fold_;
  std::vector<uint8_t> scratch_convert_;  // a view's bytes before they go to the ring
  std::vector<uint8_t> scratch_rects_;

  uint32_t slot_ = 0;
  uint64_t frame_ = 1;  // frame serial; views stamped with it are in this frame's ring
  uint32_t evict_cursor_ = 0;

  struct Stats {
    uint64_t ring_bytes = 0;         // copied into the ring (regions)
    uint64_t host_made_bytes = 0;    // inline, rects, folds, converted indices
    uint64_t promotions = 0;
    uint64_t promoted_bytes = 0;
    uint64_t new_regions = 0;
    uint64_t invalidations = 0;      // notifications that hit a region
    uint64_t hashes = 0;             // hashed-mode comparisons
    uint64_t to_hashed = 0;
    uint64_t to_watched = 0;
    uint64_t strip_lists = 0;        // indexed strips drawn as lists
    uint64_t rect_clamped = 0;       // UNORM8 corners clamped
    uint64_t rect_int_mismatch = 0;  // integer attributes that differ across a rectangle
    uint64_t folds = 0;
    uint64_t fold_out_of_range = 0;  // source vertices beyond the fetch (zeros)
    uint64_t index_out_of_range = 0; // draws reading past a stream's vertices
    uint64_t positions_checked = 0;
    uint64_t positions_nonfinite = 0;
    uint64_t evicted = 0;
    // Render-thread time in TSC cycles (cycles.h): all of Prepare, and inside
    // it the hashing of hashed regions, the copies of regions into the ring and
    // the folds.
    uint64_t prepare_cycles = 0;
    uint64_t hash_cycles = 0;
    uint64_t copy_cycles = 0;
    uint64_t fold_cycles = 0;
    // mcla_gpu_scene_profile: TSC cycles of the invalidation
    // drain, the stream sources, the region lookups and the rest of UseView.
    uint64_t profile_cycles[4] = {};
    // Region views asked for (vertex, index), and how many repeat the range
    // of the same stream (or the index range) of the draw before.
    uint64_t profile_views[2] = {};
    uint64_t profile_repeats[2] = {};
  };
  Stats stats_;
  bool profile_ = false;
  std::array<uint64_t, guest::kStreamCount> profile_last_stream_{};  // start << 32 | size
  uint64_t profile_last_index_ = 0;
  uint32_t reported_upload_failure_ = 0;
};

}  // namespace mcla::gpu
