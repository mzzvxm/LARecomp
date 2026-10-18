// MCLA GPU renderer -- the guest's vertex and index data on the GPU. See
// geometry.h.
//
// The rectangle-list fourth vertex follows Xenia's kRectangleList geometry
// shader (src/graphics/d3d12/pipeline_cache.cpp in the ReXGlue SDK; Copyright
// Ben Vanik, the Xenia authors, BSD-3-Clause), applied to the vertex inputs on
// the CPU (says why that is exact for MCLA's rectangles).

#include "geometry.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>

#include <rex/hash.h>
#include <rex/logging.h>

#include "cycles.h"
#include "resolve_memory.h"
#include "device.h"
#include "format.h"
#include "frame.h"
#include "native_gfx/vertex_declaration.h"

namespace mcla::gpu {

namespace nfx = mcla::native_gfx;

namespace {


constexpr uint64_t kVertexAlignment = 16;
constexpr uint64_t kIndexAlignment = 16;
constexpr uint32_t kZeroBufferBytes = 64;

// xenos::Endian.
constexpr uint32_t kEndianNone = 0;
constexpr uint32_t kEndian8in16 = 1;
constexpr uint32_t kEndian8in32 = 2;
constexpr uint32_t kEndian16in32 = 3;

inline uint32_t Bswap32(uint32_t v) { return __builtin_bswap32(v); }
inline uint16_t Bswap16(uint16_t v) { return __builtin_bswap16(v); }

// The Xenos endian swap of raw bytes; `length` is a multiple of the unit.
void ApplyEndian(uint8_t* dst, const uint8_t* src, uint32_t length, uint32_t endian) {
  switch (endian) {
    case kEndian8in16:
      for (uint32_t i = 0; i + 2 <= length; i += 2) {
        uint16_t v;
        std::memcpy(&v, src + i, 2);
        v = Bswap16(v);
        std::memcpy(dst + i, &v, 2);
      }
      break;
    case kEndian8in32:
      for (uint32_t i = 0; i + 4 <= length; i += 4) {
        uint32_t v;
        std::memcpy(&v, src + i, 4);
        v = Bswap32(v);
        std::memcpy(dst + i, &v, 4);
      }
      break;
    case kEndian16in32:
      for (uint32_t i = 0; i + 4 <= length; i += 4) {
        uint32_t v;
        std::memcpy(&v, src + i, 4);
        v = (v >> 16) | (v << 16);
        std::memcpy(dst + i, &v, 4);
      }
      break;
    default:
      std::memcpy(dst, src, length);
      break;
  }
}

// A stream under its swap plan (input_layout.h): every element by the width of
// its components, bytes outside elements in 32-bit units; a tail shorter than
// a vertex in 32-bit units.
void SwapVertices(const StreamPlan& plan, const uint8_t* src, uint32_t length, uint8_t* dst) {
  bool all32 = true;
  uint32_t extent = 0;  // end of the last element
  for (const SwapRange& r : plan.swaps) {
    all32 &= r.unit == 4;
    extent = std::max<uint32_t>(extent, uint32_t(r.offset) + r.size);
  }
  if (all32 || extent == 0) {
    ApplyEndian(dst, src, length & ~3u, kEndian8in32);
    std::memcpy(dst + (length & ~3u), src + (length & ~3u), length & 3u);
    return;
  }
  // Per-byte unit map of one vertex. A stride-0 stream (every vertex reads the
  // same bytes) is one vertex as long as its elements reach; the rest of the
  // range goes in 32-bit units like any byte outside an element.
  uint8_t unit[1024];
  const uint32_t stride =
      std::min<uint32_t>(plan.stride != 0 ? plan.stride : std::min(extent, length),
                         sizeof(unit));
  std::memset(unit, 4, stride);
  for (const SwapRange& r : plan.swaps) {
    for (uint32_t b = r.offset; b < uint32_t(r.offset) + r.size && b < stride; ++b) {
      unit[b] = r.unit;
    }
  }
  const uint32_t vertices = length / stride;
  for (uint32_t v = 0; v < vertices; ++v) {
    const uint8_t* s = src + size_t(v) * stride;
    uint8_t* d = dst + size_t(v) * stride;
    uint32_t p = 0;
    while (p < stride) {
      if (unit[p] == 2 && p + 2 <= stride) {
        d[p] = s[p + 1];
        d[p + 1] = s[p];
        p += 2;
      } else if (p + 4 <= stride) {
        d[p] = s[p + 3];
        d[p + 1] = s[p + 2];
        d[p + 2] = s[p + 1];
        d[p + 3] = s[p];
        p += 4;
      } else {
        d[p] = s[p];
        ++p;
      }
    }
  }
  const uint32_t tail = vertices * stride;
  ApplyEndian(dst + tail, src + tail, (length - tail) & ~3u, kEndian8in32);
  const uint32_t rest = tail + ((length - tail) & ~3u);
  std::memcpy(dst + rest, src + rest, length - rest);
}

float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16;
  uint32_t exponent = (h >> 10) & 0x1Fu;
  uint32_t mantissa = h & 0x3FFu;
  uint32_t bits;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      // Subnormal: normalize.
      exponent = 127 - 15 + 1;
      while ((mantissa & 0x400u) == 0) {
        mantissa <<= 1;
        --exponent;
      }
      mantissa &= 0x3FFu;
      bits = sign | (exponent << 23) | (mantissa << 13);
    }
  } else if (exponent == 31) {
    bits = sign | 0x7F800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
  }
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

uint16_t FloatToHalf(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  const uint32_t sign = (bits >> 16) & 0x8000u;
  const int32_t exponent = int32_t((bits >> 23) & 0xFFu) - 127 + 15;
  uint32_t mantissa = bits & 0x7FFFFFu;
  if (((bits >> 23) & 0xFFu) == 0xFFu) {
    return uint16_t(sign | 0x7C00u | (mantissa ? 0x200u : 0u));
  }
  if (exponent >= 31) {
    return uint16_t(sign | 0x7C00u);  // overflow to infinity
  }
  if (exponent <= 0) {
    if (exponent < -10) {
      return uint16_t(sign);
    }
    mantissa |= 0x800000u;
    const uint32_t shift = uint32_t(14 - exponent);
    uint32_t half = mantissa >> shift;
    const uint32_t round = (mantissa >> (shift - 1)) & 1u;
    half += round;  // round half up
    return uint16_t(sign | half);
  }
  uint32_t half = sign | (uint32_t(exponent) << 10) | (mantissa >> 13);
  if (mantissa & 0x1000u) {
    ++half;  // round half up; a carry into the exponent is correct
  }
  return uint16_t(half);
}

}  // namespace

const char* GeometryCache::ResultName(Result result) {
  switch (result) {
    case Result::kOk: return "ok";
    case Result::kEmpty: return "no primitive";
    case Result::kNotVertexFetch: return "stream fetch constant is not a vertex fetch";
    case Result::kStreamEndian: return "stream endian other than 8in32";
    case Result::kUnreadable: return "guest range not readable";
    case Result::kUpload: return "upload memory unavailable";
    case Result::kFoldStart: return "computed fetch start not a multiple of 4";
    case Result::kFoldPrimitive: return "computed fetch on something other than an auto quad list";
    case Result::kRectNoPosition: return "rectangle list without a float POSITION";
    case Result::kIndexedRectList: return "indexed rectangle list";
    case Result::kInlineLayout: return "inline vertices with another stream or stride";
    case Result::kResolveStream: return "stream over resolve memory not built";
    case Result::kCount: break;
  }
  return "?";
}

uint64_t GeometryCache::Interpretation::Key() const {
  if (index) {
    return 0x8000000000000000ull | (uint64_t(index32) << 8) | endian;
  }
  return plan ? (plan->plan_hash & 0x7FFFFFFFFFFFFFFFull) : 0;
}

GeometryCache::~GeometryCache() {
  Shutdown();
}

bool GeometryCache::Initialize(Device& device, FrameRing& frames, UploadRing& upload,
                               GuestMemory& memory) {
  device_ = &device;
  frames_ = &frames;
  upload_ = &upload;
  memory_ = &memory;
  subscriber_ = memory.Subscribe("geometry");
  vertex_heap_.Initialize(device, frames, plume::RenderBufferFlag::VERTEX, "guest vertices");
  index_heap_.Initialize(device, frames, plume::RenderBufferFlag::INDEX, "guest indices");
  zero_buffer_ = vertex_heap_.Allocate(kZeroBufferBytes);
  if (!zero_buffer_) {
    REXLOG_ERROR("[gpu] geometry: the zero stream buffer could not be made");
    return false;
  }
  zero_buffer_ready_ = false;  // filled by the first frame's promotion
  return true;
}

void GeometryCache::Shutdown() {
  regions_.clear();
  index_.Clear();
  promotions_.clear();
  generated_ = {};
  zero_buffer_ = {};
  zero_buffer_ready_ = false;
  vertex_heap_.Shutdown();
  index_heap_.Shutdown();
  device_ = nullptr;
  frames_ = nullptr;
  upload_ = nullptr;
  memory_ = nullptr;
}

void GeometryCache::BeginFrame(uint32_t slot) {
  slot_ = slot;
  ++frame_;
  zero_ring_ = UploadSpan{};
}

UploadSpan GeometryCache::Ring(uint64_t size, uint64_t alignment) {
  UploadSpan span = upload_->Allocate(slot_, size, alignment);
  if (!span && reported_upload_failure_ < 4) {
    ++reported_upload_failure_;
    REXLOG_ERROR("[gpu] geometry: {} bytes of upload memory could not be had", size);
  }
  return span;
}

// ---------------------------------------------------------------------------
// regions
// ---------------------------------------------------------------------------

void GeometryCache::DrainInvalidations() {
  if (!memory_->HasInvalidations(subscriber_)) {
    return;
  }
  drained_.clear();
  memory_->Drain(subscriber_, drained_);
  for (const GuestMemory::Range& range : drained_) {
    index_.ForEachOverlap(range.start, range.length, [&](RegionIndex::Id id) {
      Region& r = regions_[id];
      if (!r.live) {
        return;
      }
      ++r.version;
      // One notification per arm: the touched pages are unwatched now.
      r.armed = false;
      ++stats_.invalidations;
    });
  }
}

GeometryCache::Region* GeometryCache::FindOrCreateRegion(uint32_t start, uint32_t length,
                                                         uint32_t* id, Result* failure) {
  RegionIndex::Id found = index_.Find(start, length);
  if (found != RegionIndex::kNone) {
    *id = found;
    return &regions_[found];
  }
  if (!memory_->Read(start, length)) {
    *failure = Result::kUnreadable;
    return nullptr;
  }
  found = index_.Insert(start, length);
  if (regions_.size() < index_.id_bound()) {
    regions_.resize(index_.id_bound());
  }
  Region& r = regions_[found];
  r = Region{};
  r.live = true;
  r.watchable = memory_->IsWatchable(start, length);
  r.mode = r.watchable ? Mode::kWatched : Mode::kHashed;
  ++stats_.new_regions;
  *id = found;
  return &r;
}

void GeometryCache::Convert(const Interpretation& how, const uint8_t* src, uint32_t length,
                            uint8_t* dst, View& view) {
  // Built in cached memory, read there, written to the ring once (geometry.h).
  view.host_bytes = length;
  scratch_convert_.resize(length);
  uint8_t* tmp = scratch_convert_.data();
  if (how.index) {
    const uint32_t whole = how.index32 ? (length & ~3u) : (length & ~1u);
    ApplyEndian(tmp, src, whole, how.endian);
    std::memcpy(tmp + whole, src + whole, length - whole);
    uint32_t max_index = 0;
    bool has_ffff = false;
    if (how.index32) {
      for (uint32_t i = 0; i + 4 <= length; i += 4) {
        uint32_t v;
        std::memcpy(&v, tmp + i, 4);
        max_index = std::max(max_index, v);
      }
    } else {
      for (uint32_t i = 0; i + 2 <= length; i += 2) {
        uint16_t v;
        std::memcpy(&v, tmp + i, 2);
        has_ffff |= v == 0xFFFFu;
        max_index = std::max<uint32_t>(max_index, v);
      }
    }
    view.max_index = max_index;
    view.has_ffff = has_ffff;
  } else {
    SwapVertices(*how.plan, src, length, tmp);
    if (how.position_offset >= 0 && uint32_t(how.position_offset) + 12 <= length) {
      float xyz[3];
      std::memcpy(xyz, tmp + how.position_offset, 12);
      ++stats_.positions_checked;
      if (!std::isfinite(xyz[0]) || !std::isfinite(xyz[1]) || !std::isfinite(xyz[2])) {
        ++stats_.positions_nonfinite;
      }
    }
  }
  std::memcpy(dst, tmp, length);
}

bool GeometryCache::UseView(uint32_t start, uint32_t length, const Interpretation& how,
                            plume::RenderBufferReference* ref, View** out_view,
                            Result* failure) {
  const uint64_t view_start = profile_ ? cycles::Now() : 0;
  uint32_t id = 0;
  Region* region = FindOrCreateRegion(start, length, &id, failure);
  if (!region) {
    return false;
  }
  const uint64_t region_end = profile_ ? cycles::Now() : 0;
  if (profile_) {
    stats_.profile_cycles[2] += region_end - view_start;
  }
  struct RestTimer {
    GeometryCache* cache;
    uint64_t start;
    ~RestTimer() {
      if (cache->profile_) {
        cache->stats_.profile_cycles[3] += cycles::Now() - start;
      }
    }
  } rest_timer{this, region_end};
  Region& r = *region;
  const uint64_t key = how.Key();
  uint32_t view_index = 0;
  while (view_index < r.views.size() && r.views[view_index].interpretation != key) {
    ++view_index;
  }
  if (view_index == r.views.size()) {
    View v;
    v.interpretation = key;
    v.index = how.index;
    r.views.push_back(v);
  }
  View& v = r.views[view_index];
  *out_view = &v;
  const bool first_use_in_frame = r.last_frame != frame_;
  r.last_frame = frame_;

  if (r.mode == Mode::kWatched) {
    if (v.device && v.device_version == r.version) {
      if (first_use_in_frame) {
        r.changed_streak = 0;  // used, unchanged
      }
      *ref = v.device.reference();
      return true;
    }
    if (v.ring_frame == frame_ && v.ring_version == r.version) {
      *ref = v.ring.reference();
      return true;
    }
    // A copy is needed. It is a change when an earlier copy holds another version.
    const bool changed = r.copied_once && r.copied_version != r.version;
    if (first_use_in_frame || changed) {
      r.changed_streak = changed ? r.changed_streak + 1 : 0;
    }
    if (r.changed_streak >= kDynamicStreak) {
      r.mode = Mode::kHashed;
      r.stable_streak = 0;
      r.last_hash_valid = false;
      if (r.was_hashed) {
        r.stable_required = std::min(r.stable_required * 2, kMaxStableStreak);
      }
      r.was_hashed = true;
      ++stats_.to_hashed;
      // Fall through to the hashed path below.
    } else {
      // Arm BEFORE the copy: a write after it is then always seen.
      if (r.watchable && !r.armed) {
        memory_->Arm(start, length);
        r.armed = true;
      }
      const uint8_t* src = memory_->Read(start, length);
      if (!src) {
        *failure = Result::kUnreadable;
        return false;
      }
      UploadSpan span = Ring(length, how.index ? kIndexAlignment : kVertexAlignment);
      if (!span) {
        *failure = Result::kUpload;
        return false;
      }
      const uint64_t copy_start = cycles::Now();
      Convert(how, src, length, span.cpu, v);
      stats_.copy_cycles += cycles::Now() - copy_start;
      stats_.ring_bytes += length;
      v.ring = span;
      v.ring_frame = frame_;
      v.ring_version = r.version;
      r.copied_once = true;
      r.copied_version = r.version;
      if (!v.promotion_queued) {
        v.promotion_queued = true;
        promotions_.push_back(Promotion{false, id, view_index});
      }
      *ref = span.reference();
      return true;
    }
  }

  // Hashed mode.
  if (v.ring_frame == frame_) {
    *ref = v.ring.reference();
    return true;
  }
  const uint8_t* src = memory_->Read(start, length);
  if (!src) {
    *failure = Result::kUnreadable;
    return false;
  }
  if (r.hash_frame != frame_) {
    r.hash_frame = frame_;
    const uint64_t hash_start = cycles::Now();
    r.frame_hash = XXH3_64bits(src, length);
    stats_.hash_cycles += cycles::Now() - hash_start;
    ++stats_.hashes;
    const bool unchanged = r.last_hash_valid && r.last_hash == r.frame_hash;
    r.stable_streak = unchanged ? r.stable_streak + 1 : 0;
    r.last_hash = r.frame_hash;
    r.last_hash_valid = true;
    if (r.watchable && r.stable_streak >= r.stable_required) {
      // Back to the watch: arm first, then check the bytes once more, so a
      // write between the hash and the arm cannot be lost.
      memory_->Arm(start, length);
      r.armed = true;
      r.frame_hash = XXH3_64bits(src, length);
      r.mode = Mode::kWatched;
      r.changed_streak = 0;
      ++stats_.to_watched;
      for (View& other : r.views) {
        if (other.device && other.device_hash_valid && other.device_hash == r.frame_hash) {
          other.device_version = r.version;  // the device copy is the current bytes
        }
      }
    }
  }
  if (v.device && v.device_hash_valid && v.device_hash == r.frame_hash) {
    if (r.mode == Mode::kWatched) {
      v.device_version = r.version;
    }
    *ref = v.device.reference();
    return true;
  }
  UploadSpan span = Ring(length, how.index ? kIndexAlignment : kVertexAlignment);
  if (!span) {
    *failure = Result::kUpload;
    return false;
  }
  const uint64_t copy_start = cycles::Now();
  Convert(how, src, length, span.cpu, v);
  stats_.copy_cycles += cycles::Now() - copy_start;
  stats_.ring_bytes += length;
  v.ring = span;
  v.ring_frame = frame_;
  v.ring_version = r.version;
  v.ring_hash = r.frame_hash;
  r.copied_once = true;
  r.copied_version = r.version;
  // Promote only bytes that did not change since the last frame of use: data
  // that changes every frame would be copied to the device for nothing.
  if ((r.stable_streak >= 1 || r.mode == Mode::kWatched) && !v.promotion_queued) {
    v.promotion_queued = true;
    promotions_.push_back(Promotion{false, id, view_index});
  }
  *ref = span.reference();
  return true;
}

// ---------------------------------------------------------------------------
// host-made data
// ---------------------------------------------------------------------------

bool GeometryCache::GeneratedIndices(PrimitiveConversion conversion, uint32_t count,
                                     plume::RenderBufferReference* ref) {
  const uint32_t needed = ConvertedIndexCount(conversion, count);
  if (needed == 0) {
    return false;
  }
  if (conversion == PrimitiveConversion::kLineLoop) {
    // The closing index depends on the count: not a prefix pattern.
    UploadSpan span = Ring(uint64_t(needed) * 4, kIndexAlignment);
    if (!span) {
      return false;
    }
    GenerateIndices(conversion, count, reinterpret_cast<uint32_t*>(span.cpu));
    stats_.host_made_bytes += uint64_t(needed) * 4;
    *ref = span.reference();
    return true;
  }
  Generated& g = generated_[size_t(conversion)];
  if (g.device && g.capacity >= needed) {
    *ref = g.device.reference();
    return true;
  }
  if (g.ring_frame == frame_ && g.ring_capacity >= needed) {
    *ref = g.ring.reference();
    return true;
  }
  // Grow: twice the larger of what is held and what is asked, at least 4096
  // guest vertices' worth.
  uint32_t vertices = std::max<uint32_t>({count * 2, 4096u});
  while (ConvertedIndexCount(conversion, vertices) < std::max(needed, g.capacity * 2)) {
    vertices *= 2;
  }
  const uint32_t capacity = ConvertedIndexCount(conversion, vertices);
  UploadSpan span = Ring(uint64_t(capacity) * 4, kIndexAlignment);
  if (!span) {
    return false;
  }
  GenerateIndices(conversion, vertices, reinterpret_cast<uint32_t*>(span.cpu));
  stats_.host_made_bytes += uint64_t(capacity) * 4;
  g.ring = span;
  g.ring_frame = frame_;
  g.ring_capacity = capacity;
  if (!g.promotion_queued) {
    g.promotion_queued = true;
    promotions_.push_back(Promotion{true, uint32_t(conversion), 0});
  }
  *ref = span.reference();
  return true;
}

bool GeometryCache::ReadIndices(uint32_t address, uint32_t count, bool index32,
                                uint32_t endian, Result* failure) {
  const uint32_t size = index32 ? 4u : 2u;
  const uint8_t* src = memory_->Read(address, count * size);
  if (!src) {
    *failure = Result::kUnreadable;
    return false;
  }
  scratch_indices_.resize(count);
  scratch_bytes_.resize(size_t(count) * size);
  ApplyEndian(scratch_bytes_.data(), src, count * size, endian);
  for (uint32_t i = 0; i < count; ++i) {
    if (index32) {
      std::memcpy(&scratch_indices_[i], scratch_bytes_.data() + size_t(i) * 4, 4);
    } else {
      uint16_t v;
      std::memcpy(&v, scratch_bytes_.data() + size_t(i) * 2, 2);
      scratch_indices_[i] = v;
    }
  }
  return true;
}

bool GeometryCache::StreamSource(const DrawView& view, const StreamPlan& plan,
                                 uint32_t* address, uint32_t* size, Result* failure,
                                 std::string* detail) {
  const uint32_t slot = nfx::FetchSlotForStream(plan.stream);
  if (slot >= guest::kFetchGroupCount * 3) {
    *failure = Result::kNotVertexFetch;
    *detail = std::format("stream {}", plan.stream);
    return false;
  }
  uint32_t d0 = 0, d1 = 0;
  view.VertexFetch(slot, &d0, &d1);
  if ((d0 & 3u) != 3u) {
    *failure = Result::kNotVertexFetch;
    *detail = std::format("stream {} fetch {:08X} {:08X}", plan.stream, d0, d1);
    return false;
  }
  if ((d1 & 3u) != kEndian8in32) {
    *failure = Result::kStreamEndian;
    *detail = std::format("stream {} endian {}", plan.stream, d1 & 3u);
    return false;
  }
  *address = d0 & 0xFFFFFFFCu;
  *size = d1 & 0x03FFFFFCu;
  return true;
}

bool GeometryCache::SynthesizeRects(const DrawView& view, const uint8_t* be, uint32_t vertices,
                                    const StreamPlan& plan, uint8_t* out) {
  const DrawRecord& r = view.record();
  const uint32_t stride = plan.stride;
  const uint32_t stream = plan.stream;
  // The stream's elements, by their declaration type (the host formats of the
  // input layout say how the shader reads them, not what the bytes are).
  struct Field {
    uint32_t offset;
    uint32_t kind;  // 0 float32, 1 float16, 2 unorm8, 3 integer
    uint32_t components;
  };
  Field fields[guest::kMaxDeclElements];
  uint32_t field_count = 0;
  int32_t position = -1;
  for (uint32_t d = 0; d < r.decl_count && field_count < guest::kMaxDeclElements; ++d) {
    const nfx::DeclarationElement e = nfx::ReadDeclarationElement(view.decl_records(), d);
    if (e.stream == 0xFF) {
      break;
    }
    if (e.stream != stream) {
      continue;
    }
    const format::Mapping m = format::VertexFormat(e.type);
    Field f{e.offset, 3, 0};
    switch (m.format) {
      case plume::RenderFormat::R32G32B32A32_FLOAT: f = {e.offset, 0, 4}; break;
      case plume::RenderFormat::R32G32B32_FLOAT: f = {e.offset, 0, 3}; break;
      case plume::RenderFormat::R32G32_FLOAT: f = {e.offset, 0, 2}; break;
      case plume::RenderFormat::R16G16B16A16_FLOAT: f = {e.offset, 1, 4}; break;
      case plume::RenderFormat::R16G16_FLOAT: f = {e.offset, 1, 2}; break;
      case plume::RenderFormat::B8G8R8A8_UNORM: f = {e.offset, 2, 4}; break;
      default: f = {e.offset, 3, 1}; break;  // one packed integer dword
    }
    if (e.usage == 0 && e.usage_index == 0 && f.kind == 0 && f.components >= 2) {
      position = int32_t(e.offset);
    }
    fields[field_count++] = f;
  }
  if (position < 0) {
    return false;
  }
  // Swap the three corners of each rectangle into host order (the stream's
  // own plan), pick the diagonal like Xenia (strict comparisons), write a, b,
  // c and d = b + c - a.
  std::vector<uint8_t> corners(size_t(stride) * 3);
  const uint32_t rects = vertices / 3;
  for (uint32_t rect = 0; rect < rects; ++rect) {
    SwapVertices(plan, be + size_t(rect) * 3 * stride, stride * 3, corners.data());
    const uint8_t* v[3] = {corners.data(), corners.data() + stride, corners.data() + 2 * stride};
    float p[3][2];
    for (uint32_t i = 0; i < 3; ++i) {
      std::memcpy(p[i], v[i] + position, 8);
    }
    const auto length2 = [&](uint32_t i, uint32_t j) {
      const float dx = p[j][0] - p[i][0];
      const float dy = p[j][1] - p[i][1];
      return dx * dx + dy * dy;
    };
    const float l12 = length2(1, 2);
    const float l20 = length2(2, 0);
    const float l01 = length2(0, 1);
    uint32_t order[3];
    if (l20 < l12 && l01 < l12) {
      order[0] = 0; order[1] = 1; order[2] = 2;
    } else if (l01 < l20) {
      order[0] = 1; order[1] = 2; order[2] = 0;
    } else {
      order[0] = 2; order[1] = 0; order[2] = 1;
    }
    uint8_t* dst = out + size_t(rect) * 4 * stride;
    const uint8_t* a = v[order[0]];
    const uint8_t* b = v[order[1]];
    const uint8_t* c = v[order[2]];
    std::memcpy(dst, a, stride);
    std::memcpy(dst + stride, b, stride);
    std::memcpy(dst + 2 * stride, c, stride);
    uint8_t* d = dst + 3 * stride;
    std::memcpy(d, b, stride);
    for (uint32_t i = 0; i < field_count; ++i) {
      const Field& f = fields[i];
      if (f.kind == 0) {
        for (uint32_t k = 0; k < f.components; ++k) {
          float fa, fb, fc;
          std::memcpy(&fa, a + f.offset + 4 * k, 4);
          std::memcpy(&fb, b + f.offset + 4 * k, 4);
          std::memcpy(&fc, c + f.offset + 4 * k, 4);
          const float fd = fb + fc - fa;
          std::memcpy(d + f.offset + 4 * k, &fd, 4);
        }
      } else if (f.kind == 1) {
        for (uint32_t k = 0; k < f.components; ++k) {
          uint16_t ha, hb, hc;
          std::memcpy(&ha, a + f.offset + 2 * k, 2);
          std::memcpy(&hb, b + f.offset + 2 * k, 2);
          std::memcpy(&hc, c + f.offset + 2 * k, 2);
          const uint16_t hd = FloatToHalf(HalfToFloat(hb) + HalfToFloat(hc) - HalfToFloat(ha));
          std::memcpy(d + f.offset + 2 * k, &hd, 2);
        }
      } else if (f.kind == 2) {
        for (uint32_t k = 0; k < 4; ++k) {
          const int32_t value = int32_t(b[f.offset + k]) + int32_t(c[f.offset + k]) -
                                int32_t(a[f.offset + k]);
          if (value < 0 || value > 255) {
            ++stats_.rect_clamped;
          }
          d[f.offset + k] = uint8_t(std::clamp(value, 0, 255));
        }
      } else if (std::memcmp(a + f.offset, b + f.offset, 4 * f.components) != 0 ||
                 std::memcmp(b + f.offset, c + f.offset, 4 * f.components) != 0) {
        ++stats_.rect_int_mismatch;  // d keeps b's value
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// a draw
// ---------------------------------------------------------------------------

GeometryCache::Result GeometryCache::Prepare(const DrawView& view, const XenosState& state,
                                             const InputLayout& layout, HostPrimitive primitive,
                                             bool computed_fetch, DrawGeometry& out,
                                             std::string* detail,
                                             plume::RenderCommandList* list) {
  const uint64_t c0 = cycles::Now();
  const Result result =
      PrepareDraw(view, state, layout, primitive, computed_fetch, out, detail, list);
  stats_.prepare_cycles += cycles::Now() - c0;
  return result;
}

void GeometryCache::ReportResolveRange(const char* what, uint32_t address, uint32_t size) {
  ++stats_.resolve_ranges_reported;
  if (reported_resolve_ranges_ < 8) {
    ++reported_resolve_ranges_;
    REXLOG_ERROR("[gpu] geometry: {} over memory a resolve wrote ({:08X}, {} bytes) is read "
                 "from guest memory, which does not hold those bytes (TODO){}",
                 what, address, size,
                 reported_resolve_ranges_ == 8 ? " (further ones counted, not logged)" : "");
  }
}

GeometryCache::Result GeometryCache::ResolveStream(const DrawRecord& r, const StreamPlan& plan,
                                                   uint32_t address, uint32_t size,
                                                   bool computed_fetch,
                                                   plume::RenderCommandList* list,
                                                   plume::RenderBufferReference* ref,
                                                   uint32_t* bytes, bool* handled,
                                                   std::string* detail) {
  *handled = false;
  // The swap plan as 2 bits per dword (resolve_memory_vertex_cs.hlsl): every
  // byte of a dword must swap by the same unit, as SwapVertices does it.
  const uint32_t stride = plan.stride;
  if (stride == 0 || (stride & 3u) != 0 || stride > ResolveMemory::kMaxVertexCopyStride) {
    ReportResolveRange("a stream of this stride", address, size);
    return Result::kOk;
  }
  uint8_t unit[ResolveMemory::kMaxVertexCopyStride];
  std::memset(unit, 4, stride);
  for (const SwapRange& s : plan.swaps) {
    for (uint32_t b = s.offset; b < uint32_t(s.offset) + s.size && b < stride; ++b) {
      unit[b] = s.unit;
    }
  }
  ResolveMemory::VertexCopy copy;
  for (uint32_t d = 0; d < stride / 4; ++d) {
    const uint8_t u = unit[d * 4];
    if (unit[d * 4 + 1] != u || unit[d * 4 + 2] != u || unit[d * 4 + 3] != u ||
        (u != 2 && u != 4)) {
      ReportResolveRange("a stream whose swap units split a dword", address, size);
      return Result::kOk;
    }
    copy.modes[d >> 4] |= (u == 4 ? 2u : 1u) << ((d & 15u) * 2u);
  }
  copy.address = address;
  copy.stride = stride;
  copy.sources = size / stride;
  if (computed_fetch) {
    // The fold's layout (destination vertex v = source (start + v) / 4).
    copy.first = r.start;
    copy.vertices = r.count;
    copy.fold = true;
  } else {
    // The whole fetch range, bound as the CPU copy binds it.
    copy.first = 0;
    copy.vertices = copy.sources;
  }
  *bytes = copy.vertices * stride;
  if (*bytes == 0) {
    return Result::kEmpty;
  }
  std::string why;
  if (!resolve_memory_->CopyVertices(copy, list, ref, &why)) {
    *detail = std::format("stream {} at {:08X}: {}", plan.stream, address, why);
    return Result::kResolveStream;
  }
  *handled = true;
  ++stats_.resolve_streams;
  stats_.resolve_stream_bytes += *bytes;
  return Result::kOk;
}

GeometryCache::Result GeometryCache::PrepareDraw(const DrawView& view, const XenosState& state,
                                                 const InputLayout& layout,
                                                 HostPrimitive primitive, bool computed_fetch,
                                                 DrawGeometry& out, std::string* detail,
                                                 plume::RenderCommandList* list) {
  const uint64_t drain_start = profile_ ? cycles::Now() : 0;
  DrainInvalidations();
  if (profile_) {
    stats_.profile_cycles[0] += cycles::Now() - drain_start;
  }
  const DrawRecord& r = view.record();
  out = DrawGeometry{};
  out.topology = primitive.topology;
  out.slot_count = uint32_t(layout.slots.size());
  const PrimitiveConversion conversion = primitive.conversion;
  const bool rects = conversion == PrimitiveConversion::kRectList;

  if (computed_fetch) {
    if (r.kind != DrawKind::kAuto || conversion != PrimitiveConversion::kQuadList) {
      *detail = std::format("{} {}", uint32_t(r.kind), PrimitiveName(r.primitive));
      return Result::kFoldPrimitive;
    }
    if (r.start % 4 != 0) {
      *detail = std::format("start {}", r.start);
      return Result::kFoldStart;
    }
  }
  if (rects && r.kind == DrawKind::kIndexed) {
    return Result::kIndexedRectList;
  }
  // Inline vertices are one stream whose stride is the record's: the device's
  // stride table holds 0 for them (24.23 item 3), and the Scene builds the
  // input layout with the record's stride -- which a stride table byte (dwords)
  // cannot hold beyond 1020 bytes. A plan with any other stride would draw
  // every vertex from one place and swap 16-bit elements as 32-bit ones.
  if (r.kind == DrawKind::kInline &&
      (layout.streams.size() > 1 || r.inline_stride == 0 || (r.inline_stride & 3u) != 0 ||
       (!layout.streams.empty() && layout.streams[0].stride != r.inline_stride))) {
    *detail = std::format("{} streams, record stride {}, layout stride {}", layout.streams.size(),
                          r.inline_stride, layout.streams.empty() ? 0u : layout.streams[0].stride);
    return Result::kInlineLayout;
  }

  // Vertex streams.
  uint32_t min_vertices = UINT32_MAX;
  Result failure = Result::kOk;
  for (uint32_t i = 0; i < layout.streams.size(); ++i) {
    const StreamPlan& plan = layout.streams[i];
    plume::RenderBufferReference ref;
    uint32_t bytes = 0;
    if (r.kind == DrawKind::kInline) {
      const uint32_t stride = r.inline_stride;
      if (rects) {
        const uint32_t host_vertices = (r.count / 3) * 4;
        bytes = host_vertices * stride;
        if (bytes == 0) {
          return Result::kEmpty;
        }
        UploadSpan span = Ring(bytes, kVertexAlignment);
        if (!span) {
          return Result::kUpload;
        }
        scratch_rects_.resize(bytes);
        if (!SynthesizeRects(view, view.inline_data(), r.count, plan, scratch_rects_.data())) {
          return Result::kRectNoPosition;
        }
        std::memcpy(span.cpu, scratch_rects_.data(), bytes);
        ref = span.reference();
      } else {
        bytes = r.inline_bytes;
        UploadSpan span = Ring(bytes, kVertexAlignment);
        if (!span) {
          return Result::kUpload;
        }
        SwapVertices(plan, view.inline_data(), bytes, span.cpu);
        ref = span.reference();
      }
      stats_.host_made_bytes += bytes;
    } else {
      uint32_t address = 0, size = 0;
      const uint64_t source_start = profile_ ? cycles::Now() : 0;
      if (!StreamSource(view, plan, &address, &size, &failure, detail)) {
        return failure;
      }
      if (profile_) {
        stats_.profile_cycles[1] += cycles::Now() - source_start;
      }
      if (plan.stride != 0) {
        min_vertices = std::min(min_vertices, size / plan.stride);
      }
      // Memory a resolve wrote that guest memory does not hold:
      // the stream is built on the GPU from the resolve memory.
      bool built = false;
      if (resolve_memory_ && size != 0 && resolve_memory_->HoldsRange(address, size)) {
        if (rects) {
          ReportResolveRange("a rectangle list", address, size);
        } else {
          const Result resolved = ResolveStream(r, plan, address, size, computed_fetch, list,
                                                &ref, &bytes, &built, detail);
          if (resolved != Result::kOk) {
            return resolved;
          }
          out.list_state_changed |= built;
        }
      }
      if (built) {
        // Bound below like any other stream.
      } else if (computed_fetch) {
        // Vertex v of the draw reads source vertex (start + v) / 4.
        const uint32_t stride = plan.stride;
        bytes = r.count * stride;
        if (bytes == 0) {
          return Result::kEmpty;
        }
        UploadSpan span = Ring(bytes, kVertexAlignment);
        if (!span) {
          return Result::kUpload;
        }
        const uint64_t fold_start = cycles::Now();
        const uint32_t first_source = r.start / 4;
        const uint32_t last_source = (r.start + r.count - 1) / 4;
        const uint32_t available = stride ? size / stride : 0;
        const uint32_t readable_last = std::min(last_source, available ? available - 1 : 0);
        bool have_sources = false;
        if (available > first_source) {
          // The sources once, swapped; then each four times.
          const uint32_t source_bytes = (readable_last - first_source + 1) * stride;
          const uint8_t* src = memory_->Read(address + first_source * stride, source_bytes);
          if (!src) {
            return Result::kUnreadable;
          }
          scratch_fold_.resize(source_bytes);
          SwapVertices(plan, src, source_bytes, scratch_fold_.data());
          have_sources = true;
        }
        for (uint32_t v = 0; v < r.count; ++v) {
          const uint32_t source = (r.start + v) / 4;
          uint8_t* dst = span.cpu + size_t(v) * stride;
          if (have_sources && source < available) {
            std::memcpy(dst, scratch_fold_.data() + size_t(source - first_source) * stride,
                        stride);
          } else {
            std::memset(dst, 0, stride);
            ++stats_.fold_out_of_range;
          }
        }
        stats_.fold_cycles += cycles::Now() - fold_start;
        ++stats_.folds;
        stats_.host_made_bytes += bytes;
        ref = span.reference();
      } else if (rects) {
        // An auto rectangle list: the vertices come from guest memory.
        const uint32_t stride = plan.stride;
        const uint32_t host_vertices = (r.count / 3) * 4;
        bytes = host_vertices * stride;
        if (bytes == 0) {
          return Result::kEmpty;
        }
        const uint8_t* src = memory_->Read(address + r.start * stride, r.count * stride);
        if (!src) {
          return Result::kUnreadable;
        }
        UploadSpan span = Ring(bytes, kVertexAlignment);
        if (!span) {
          return Result::kUpload;
        }
        scratch_rects_.resize(bytes);
        if (!SynthesizeRects(view, src, r.count, plan, scratch_rects_.data())) {
          return Result::kRectNoPosition;
        }
        std::memcpy(span.cpu, scratch_rects_.data(), bytes);
        stats_.host_made_bytes += bytes;
        ref = span.reference();
      } else {
        Interpretation how;
        how.plan = &plan;
        how.position_offset = plan.position_offset;
        View* v = nullptr;
        if (size == 0) {
          return Result::kEmpty;
        }
        if (profile_ && i < profile_last_stream_.size()) {
          const uint64_t range = (uint64_t(address) << 32) | size;
          ++stats_.profile_views[0];
          stats_.profile_repeats[0] += profile_last_stream_[i] == range;
          profile_last_stream_[i] = range;
        }
        if (!UseView(address, size, how, &ref, &v, &failure)) {
          return failure;
        }
        bytes = size;
      }
    }
    out.vertex_views[i] = plume::RenderVertexBufferView(ref, bytes);
  }
  if (layout.zero_stream) {
    plume::RenderBufferReference zero;
    if (zero_buffer_ready_) {
      zero = zero_buffer_.reference();
    } else {
      // Until the first frame's end fills the device copy: zeros in the ring.
      if (!zero_ring_) {
        zero_ring_ = Ring(kZeroBufferBytes, kVertexAlignment);
        if (!zero_ring_) {
          return Result::kUpload;
        }
        std::memset(zero_ring_.cpu, 0, kZeroBufferBytes);
      }
      zero = zero_ring_.reference();
    }
    out.vertex_views[layout.slots.size() - 1] = plume::RenderVertexBufferView(zero, kZeroBufferBytes);
  }

  // Indices and the call.
  switch (r.kind) {
    case DrawKind::kInline:
    case DrawKind::kAuto: {
      const bool generated = conversion != PrimitiveConversion::kNone || computed_fetch;
      if (!generated) {
        out.indexed = false;
        out.count = r.count;
        out.first = r.kind == DrawKind::kAuto ? r.start : 0;
        if (r.count == 0) {
          return Result::kEmpty;
        }
        if (r.kind == DrawKind::kAuto && uint64_t(r.start) + r.count > min_vertices) {
          ++stats_.index_out_of_range;
        }
        break;
      }
      plume::RenderBufferReference ref;
      const PrimitiveConversion pattern = computed_fetch ? PrimitiveConversion::kQuadList : conversion;
      if (!GeneratedIndices(pattern, r.count, &ref)) {
        return ConvertedIndexCount(pattern, r.count) == 0 ? Result::kEmpty : Result::kUpload;
      }
      out.indexed = true;
      out.count = ConvertedIndexCount(pattern, r.count);
      out.index_view = plume::RenderIndexBufferView(ref, out.count * 4, plume::RenderFormat::R32_UINT);
      out.first = 0;
      // Host-made vertex buffers (inline, rectangles, folds) start at 0; an
      // auto draw's own buffer starts at the guest's start vertex.
      const bool host_made_vertices = r.kind == DrawKind::kInline || rects || computed_fetch;
      out.base_vertex = host_made_vertices ? 0 : int32_t(r.start);
      if (!host_made_vertices && uint64_t(r.start) + r.count > min_vertices) {
        ++stats_.index_out_of_range;
      }
      break;
    }
    case DrawKind::kIndexed: {
      if (r.count == 0) {
        return Result::kEmpty;
      }
      const bool index32 = (r.index_dword0 & 0x80000000u) != 0;
      const uint32_t endian = (r.index_dword0 >> 29) & 3u;
      const uint32_t size = index32 ? 4u : 2u;
      const uint32_t address = GuestMemory::ToPhysical(r.index_address) + r.start * size;
      if ((endian == kEndian8in32 || endian == kEndian16in32) && (address & 3u) != 0) {
        *detail = std::format("index range {:08X} not dword aligned for endian {}", address, endian);
        return Result::kUnreadable;
      }
      out.base_vertex = r.base_vertex;
      const bool strip = primitive.topology == plume::RenderPrimitiveTopology::TRIANGLE_STRIP ||
                         primitive.topology == plume::RenderPrimitiveTopology::LINE_STRIP;
      if (conversion == PrimitiveConversion::kNone) {
        Interpretation how;
        how.index = true;
        how.index32 = index32;
        how.endian = endian;
        plume::RenderBufferReference ref;
        View* v = nullptr;
        if (resolve_memory_ && resolve_memory_->HoldsRange(address, r.count * size)) {
          ReportResolveRange("an index buffer", address, r.count * size);
        }
        if (profile_) {
          const uint64_t range = (uint64_t(address) << 32) | (r.count * size);
          ++stats_.profile_views[1];
          stats_.profile_repeats[1] += profile_last_index_ == range;
          profile_last_index_ = range;
        }
        if (!UseView(address, r.count * size, how, &ref, &v, &failure)) {
          return failure;
        }
        const bool reset = state.sc_mode.multi_prim_ib_ena != 0;
        const uint32_t reset_index = state.reset_index & (index32 ? 0xFFFFFFFFu : 0xFFFFu);
        const bool host_cut_differs =
            strip && (index32 || (reset ? reset_index != 0xFFFFu : v->has_ffff));
        if (!host_cut_differs) {
          out.indexed = true;
          out.count = r.count;
          out.first = 0;
          out.index_view = plume::RenderIndexBufferView(
              ref, r.count * size,
              index32 ? plume::RenderFormat::R32_UINT : plume::RenderFormat::R16_UINT);
          if (uint64_t(v->max_index) + uint64_t(int64_t(r.base_vertex)) >= min_vertices) {
            ++stats_.index_out_of_range;
          }
          break;
        }
        // The strip as a list, with the guest's own reset semantics.
        if (!ReadIndices(address, r.count, index32, endian, &failure)) {
          return failure;
        }
        const bool triangles = primitive.topology == plume::RenderPrimitiveTopology::TRIANGLE_STRIP;
        scratch_converted_.resize(size_t(r.count) * 3);
        const uint32_t n = StripToList(triangles, scratch_indices_.data(), r.count, reset,
                                       reset_index, scratch_converted_.data());
        if (n == 0) {
          return Result::kEmpty;
        }
        UploadSpan span = Ring(uint64_t(n) * 4, kIndexAlignment);
        if (!span) {
          return Result::kUpload;
        }
        std::memcpy(span.cpu, scratch_converted_.data(), size_t(n) * 4);
        stats_.host_made_bytes += uint64_t(n) * 4;
        ++stats_.strip_lists;
        out.topology = triangles ? plume::RenderPrimitiveTopology::TRIANGLE_LIST
                                 : plume::RenderPrimitiveTopology::LINE_LIST;
        out.indexed = true;
        out.count = n;
        out.first = 0;
        out.index_view = plume::RenderIndexBufferView(span.reference(), n * 4,
                                                      plume::RenderFormat::R32_UINT);
        break;
      }
      // Quads, fans, polygons, line loops over the guest's indices.
      const uint32_t n = ConvertedIndexCount(conversion, r.count);
      if (n == 0) {
        return Result::kEmpty;
      }
      if (!ReadIndices(address, r.count, index32, endian, &failure)) {
        return failure;
      }
      UploadSpan span = Ring(uint64_t(n) * 4, kIndexAlignment);
      if (!span) {
        return Result::kUpload;
      }
      // ConvertIndices reads what it writes: never in the ring (geometry.h).
      scratch_converted_.resize(n);
      ConvertIndices(conversion, scratch_indices_.data(), r.count, scratch_converted_.data());
      std::memcpy(span.cpu, scratch_converted_.data(), size_t(n) * 4);
      stats_.host_made_bytes += uint64_t(n) * 4;
      out.indexed = true;
      out.count = n;
      out.first = 0;
      out.index_view = plume::RenderIndexBufferView(span.reference(), n * 4,
                                                    plume::RenderFormat::R32_UINT);
      break;
    }
  }
  return Result::kOk;
}

// ---------------------------------------------------------------------------
// end of frame
// ---------------------------------------------------------------------------

void GeometryCache::EvictSome() {
  constexpr uint32_t kScanPerFrame = 256;
  const uint32_t bound = uint32_t(regions_.size());
  for (uint32_t n = 0; n < kScanPerFrame && bound != 0; ++n) {
    const uint32_t id = evict_cursor_++ % bound;
    Region& r = regions_[id];
    if (!r.live || frame_ - r.last_frame < kEvictFrames) {
      continue;
    }
    bool queued = false;
    for (View& v : r.views) {
      queued |= v.promotion_queued;
    }
    if (queued) {
      continue;
    }
    for (View& v : r.views) {
      if (v.device) {
        (v.index ? index_heap_ : vertex_heap_).Free(v.device);
      }
    }
    index_.Remove(id);
    r = Region{};
    ++stats_.evicted;
  }
}

void GeometryCache::EndFrame(plume::RenderCommandList* list) {
  if (!list) {
    for (const Promotion& p : promotions_) {
      if (p.generated) {
        generated_[p.region].promotion_queued = false;
      } else if (p.region < regions_.size() && p.view < regions_[p.region].views.size()) {
        regions_[p.region].views[p.view].promotion_queued = false;
      }
    }
    promotions_.clear();
    return;
  }
  struct Copy {
    BufferHeap::Allocation dst;
    plume::RenderBufferReference src;
    uint64_t size;
  };
  std::vector<Copy> copies;
  copies.reserve(promotions_.size() + 1);
  std::vector<plume::RenderBuffer*> vertex_pages;
  std::vector<plume::RenderBuffer*> index_pages;
  const auto touch = [](std::vector<plume::RenderBuffer*>& pages, plume::RenderBuffer* page) {
    if (std::find(pages.begin(), pages.end(), page) == pages.end()) {
      pages.push_back(page);
    }
  };
  for (const Promotion& p : promotions_) {
    if (p.generated) {
      Generated& g = generated_[p.region];
      g.promotion_queued = false;
      if (g.ring_frame != frame_) {
        continue;
      }
      BufferHeap::Allocation dst = index_heap_.Allocate(uint64_t(g.ring_capacity) * 4);
      if (!dst) {
        continue;
      }
      if (g.device) {
        index_heap_.Free(g.device);
      }
      g.device = dst;
      g.capacity = g.ring_capacity;
      copies.push_back({dst, g.ring.reference(), uint64_t(g.ring_capacity) * 4});
      touch(index_pages, dst.buffer);
      continue;
    }
    if (p.region >= regions_.size()) {
      continue;
    }
    Region& r = regions_[p.region];
    if (p.view >= r.views.size()) {
      continue;
    }
    View& v = r.views[p.view];
    v.promotion_queued = false;
    if (!r.live || v.ring_frame != frame_) {
      continue;
    }
    BufferHeap& heap = v.index ? index_heap_ : vertex_heap_;
    BufferHeap::Allocation dst = heap.Allocate(v.host_bytes);
    if (!dst) {
      continue;
    }
    if (v.device) {
      heap.Free(v.device);
    }
    v.device = dst;
    v.device_version = v.ring_version;
    v.device_hash = v.ring_hash;
    v.device_hash_valid = r.mode == Mode::kHashed;
    copies.push_back({dst, v.ring.reference(), v.host_bytes});
    touch(v.index ? index_pages : vertex_pages, dst.buffer);
    stats_.promoted_bytes += v.host_bytes;
  }
  promotions_.clear();
  if (!zero_buffer_ready_ && zero_ring_) {
    copies.push_back({zero_buffer_, zero_ring_.reference(), kZeroBufferBytes});
    touch(vertex_pages, zero_buffer_.buffer);
  } else if (!zero_buffer_ready_) {
    // Nothing used the zero stream yet: fill it from this frame's ring anyway,
    // so a draw of the next frame can bind it directly.
    UploadSpan span = Ring(kZeroBufferBytes, kVertexAlignment);
    if (span) {
      std::memset(span.cpu, 0, kZeroBufferBytes);
      copies.push_back({zero_buffer_, span.reference(), kZeroBufferBytes});
      touch(vertex_pages, zero_buffer_.buffer);
    }
  }
  if (!copies.empty()) {
    std::vector<plume::RenderBufferBarrier> barriers;
    barriers.reserve(vertex_pages.size() + index_pages.size());
    for (plume::RenderBuffer* page : vertex_pages) {
      barriers.emplace_back(page, plume::RenderBufferAccess::WRITE);
    }
    for (plume::RenderBuffer* page : index_pages) {
      barriers.emplace_back(page, plume::RenderBufferAccess::WRITE);
    }
    list->barriers(plume::RenderBarrierStage::COPY, barriers.data(), uint32_t(barriers.size()),
                   nullptr, 0);
    for (const Copy& c : copies) {
      list->copyBufferRegion(c.dst.reference(), c.src, c.size);
    }
    for (plume::RenderBufferBarrier& b : barriers) {
      b.accessBits = plume::RenderBufferAccess::READ;
    }
    list->barriers(plume::RenderBarrierStage::GRAPHICS, barriers.data(),
                   uint32_t(barriers.size()), nullptr, 0);
    stats_.promotions += copies.size();
    if (!zero_buffer_ready_) {
      zero_buffer_ready_ = true;
    }
  }
  zero_ring_ = UploadSpan{};
  EvictSome();
}

std::string GeometryCache::TakeSummary(uint64_t frames) {
  uint64_t live = 0, hashed = 0, read_only = 0;
  for (const Region& r : regions_) {
    if (!r.live) {
      continue;
    }
    ++live;
    hashed += r.mode == Mode::kHashed;
    read_only += !r.watchable;
  }
  const double f = double(frames ? frames : 1);
  constexpr double kMb = 1.0 / (1024.0 * 1024.0);
  const BufferHeap::Stats& vs = vertex_heap_.stats();
  const BufferHeap::Stats& is = index_heap_.stats();
  const Stats& s = stats_;
  std::string text = std::format(
      "geometry: {} regions ({} hashed, {} not watchable) | device {:.1f}+{:.1f} MiB used of "
      "{}+{} pages | ring {:.3f} MB/frame, host-made {:.3f} MB/frame | {:.1f} promotions/frame "
      "({:.3f} MB) | per frame: {:.2f} new, {:.1f} invalidations, {:.1f} hashes | modes +{} "
      "hashed +{} watched | {} evicted | strips as lists {}, rect clamps {}, rect int mismatch "
      "{}, folds {:.1f}/frame (out of range {}), index past vertices {}, positions non-finite "
      "{} of {} | {:.2f} ms/frame (hash {:.2f}, copies {:.2f}, folds {:.2f})",
      live, hashed, read_only, double(vs.used_bytes) * kMb, double(is.used_bytes) * kMb,
      vs.pages + vs.dedicated, is.pages + is.dedicated, double(s.ring_bytes) * kMb / f,
      double(s.host_made_bytes) * kMb / f, double(s.promotions) / f,
      double(s.promoted_bytes) * kMb / f, double(s.new_regions) / f,
      double(s.invalidations) / f, double(s.hashes) / f, s.to_hashed, s.to_watched, s.evicted,
      s.strip_lists, s.rect_clamped, s.rect_int_mismatch, double(s.folds) / f,
      s.fold_out_of_range, s.index_out_of_range, s.positions_nonfinite, s.positions_checked,
      cycles::ToNs(s.prepare_cycles) / f / 1e6, cycles::ToNs(s.hash_cycles) / f / 1e6,
      cycles::ToNs(s.copy_cycles) / f / 1e6, cycles::ToNs(s.fold_cycles) / f / 1e6);
  if (s.resolve_streams || s.resolve_ranges_reported) {
    text += std::format(" | streams from the resolve memory {:.1f}/frame ({:.3f} MB/frame), "
                        "ranges over it read from guest memory {}",
                        double(s.resolve_streams) / f, double(s.resolve_stream_bytes) * kMb / f,
                        s.resolve_ranges_reported);
  }
  if (profile_) {
    text += std::format(" | views/frame: vertex {:.0f} ({:.1f}% repeat), index {:.0f} ({:.1f}% repeat)",
                        double(s.profile_views[0]) / f,
                        s.profile_views[0] ? 100.0 * double(s.profile_repeats[0]) / double(s.profile_views[0]) : 0.0,
                        double(s.profile_views[1]) / f,
                        s.profile_views[1] ? 100.0 * double(s.profile_repeats[1]) / double(s.profile_views[1]) : 0.0);
    text += std::format(" | profile ms/frame: drain {:.3f}, sources {:.3f}, regions {:.3f}, views {:.3f}",
                        cycles::ToNs(s.profile_cycles[0]) / f / 1e6,
                        cycles::ToNs(s.profile_cycles[1]) / f / 1e6,
                        cycles::ToNs(s.profile_cycles[2]) / f / 1e6,
                        cycles::ToNs(s.profile_cycles[3]) / f / 1e6);
  }
  stats_ = Stats{};
  return text;
}

}  // namespace mcla::gpu
