// MCLA GPU renderer -- the guest's occlusion queries. See occlusion.h.
//
// What the guest sees and does is read from the recompiled code: D3DQuery::Issue is
// sub_8241DE08, GetData sub_8241E088, the sample count event sub_824302F0; the
// record layout and the sentinel are the SDK's (xe_gpu_depth_sample_counts) and the
// fork's notes on rexglue-occlusion-queries.patch.

#include "occlusion.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include "device.h"
#include "native_gfx/guest/guest_resources.h"

namespace mcla::gpu {

namespace nfx = mcla::native_gfx;

namespace occlusion {
namespace {

// D3DQuery, as sub_8241DE08, sub_824302F0 and sub_8241E088 read it.
constexpr uint32_t kQueryTypeOffset = 4;
constexpr uint32_t kQueryTypeOcclusion = 9;
// The tiles' record pointers: an array, one slot per tile, from here.
constexpr uint32_t kQueryRecordsOffset = 28;
constexpr uint32_t kQueryTileCountOffset = 148;
constexpr uint32_t kQueryObjectBytes = 152;
// The guest's tiles (predicated tiling splits the scene in two or three).
constexpr uint32_t kMaxTiles = 8;

// A record: the END snapshot at +0x00 and the BEGIN snapshot at +0x20, each an
// xe_gpu_depth_sample_counts of eight dwords (Total, ZFail, ZPass, StencilFail, each
// as an A and a B word). The game adds the ZPass pair up.
constexpr uint32_t kSnapshotBytes = 32;
constexpr uint32_t kRecordBytes = 2 * kSnapshotBytes;
constexpr uint32_t kBeginOffset = kSnapshotBytes;
constexpr uint32_t kZPassOffset = 16;

// 0xFFFFFEED in the guest's byte order, in both ZPass words: "the GPU has not
// written this snapshot yet".
constexpr uint8_t kSentinel[8] = {0xFF, 0xFF, 0xFE, 0xED, 0xFF, 0xFF, 0xFE, 0xED};

// Queries with a use or a result the ledger remembers (one per query object the game
// keeps; the game has a handful). A longer table is forgotten whole: a count on its
// way is lost, nothing else.
constexpr size_t kMaxLedgerEntries = 4096;

std::atomic<bool> g_active{false};

// Between the guest threads and the render thread: which use of a query object is the
// current one, and the count the host delivered for it. The render thread writes
// nothing else of the guest's.
struct Entry {
  uint32_t use = 0;  // the use the guest began last; 0 none
  bool ready = false;
  uint32_t samples = 0;  // the count of that use, when ready
};
std::mutex g_mutex;
std::unordered_map<uint32_t, Entry> g_entries;
uint32_t g_next_use = 0;

uint32_t BeginUse(uint32_t query) {
  std::lock_guard lock(g_mutex);
  if (g_entries.size() >= kMaxLedgerEntries) {
    g_entries.clear();
  }
  if (++g_next_use == 0) {
    ++g_next_use;  // 0 means "no use"
  }
  Entry& e = g_entries[query];
  e.use = g_next_use;
  e.ready = false;
  return e.use;
}

uint32_t CurrentUse(uint32_t query) {
  std::lock_guard lock(g_mutex);
  const auto it = g_entries.find(query);
  return it == g_entries.end() ? 0 : it->second.use;
}

bool Take(uint32_t query, uint32_t* samples) {
  std::lock_guard lock(g_mutex);
  const auto it = g_entries.find(query);
  if (it == g_entries.end() || !it->second.ready) {
    return false;
  }
  it->second.ready = false;
  *samples = it->second.samples;
  return true;
}

// The query object's tiles and where their records are in guest memory.
struct Tiles {
  uint32_t count = 0;
  uint8_t* record[kMaxTiles] = {};
};

// False for anything that is not an occlusion query with readable records: such an
// event is left alone (the original's own stores stand).
// CHECK: the layout (type at +4, the tile count at +148, one record pointer per tile from
// +28) is read from the recompiled sub_8241DE08 / sub_824302F0 / sub_8241E088, not seen
// at run time; the first logged results ("draws", "host samples") are the proof.
bool ReadTiles(uint8_t* base, uint32_t query, Tiles& tiles) {
  if (query < 0x1000u || !nfx::IsGuestRangeReadable(query, kQueryObjectBytes)) {
    return false;
  }
  const uint8_t* q = rex::memory::GuestPtr<const uint8_t*>(base, query);
  if (LoadBe32(q + kQueryTypeOffset) != kQueryTypeOcclusion) {
    return false;
  }
  const uint32_t count = LoadBe32(q + kQueryTileCountOffset);
  if (count == 0 || count > kMaxTiles) {
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    // The pointer the game's own stores and the GPU's register use; the records are
    // 64 bytes (two snapshots).
    const uint32_t record = LoadBe32(q + kQueryRecordsOffset + 4 * i);
    if (record < 0x1000u || !nfx::IsGuestRangeReadable(record, kRecordBytes)) {
      return false;
    }
    tiles.record[i] = rex::memory::GuestPtr<uint8_t*>(base, record);
  }
  tiles.count = count;
  return true;
}

}  // namespace

bool Active() {
  return g_active.load(std::memory_order_acquire);
}

bool OnGuestIssue(uint8_t* base, uint32_t query, bool is_begin, uint32_t* use) {
  if (!Active() || !base) {
    return false;
  }
  Tiles tiles;
  if (!ReadTiles(base, query, tiles)) {
    return false;
  }
  if (is_begin) {
    // The begin snapshots start from zero, so the end snapshots then hold the count of
    // the query alone (the game subtracts the begin from the end).
    for (uint32_t i = 0; i < tiles.count; ++i) {
      std::memset(tiles.record[i] + kBeginOffset, 0, kSnapshotBytes);
    }
    *use = BeginUse(query);
    return true;
  }
  const uint32_t current = CurrentUse(query);
  if (current == 0) {
    return false;  // an end of a use the capture did not see begin
  }
  // The end snapshots as the GPU would have left them with nothing counted, except
  // the last tile's, which holds the sentinel until a count is written (the slot the
  // game's own sentinel stores name, and the one its GetData tests). Whether the game
  // stored the sentinel with the CPU or queued two memory writes nobody runs, the
  // record has it from here on.
  for (uint32_t i = 0; i + 1 < tiles.count; ++i) {
    std::memset(tiles.record[i], 0, kSnapshotBytes);
  }
  uint8_t* last = tiles.record[tiles.count - 1];
  std::memset(last, 0, kZPassOffset);
  std::memset(last + kZPassOffset + 8, 0, kSnapshotBytes - kZPassOffset - 8);
  std::memcpy(last + kZPassOffset, kSentinel, sizeof(kSentinel));
  *use = current;
  return true;
}

void OnGuestGetData(uint8_t* base, uint32_t query) {
  if (!Active() || !base) {
    return;
  }
  uint32_t samples = 0;
  if (!Take(query, &samples)) {
    return;
  }
  Tiles tiles;
  if (!ReadTiles(base, query, tiles)) {
    return;
  }
  // What the GPU's event writes: the counters, in the host's byte order (the struct is
  // little-endian; the game byte-swaps what it reads). The ZPass pair is the one the
  // game tests for the sentinel and adds up.
  // CHECK: the byte order (the SDK's own WriteGuestOcclusionResult stores the count the
  // same way and the fork's runs read it back right); a count read as 4,000,000,000 or
  // as a byte-swapped value would show in the "told to the game" log lines.
  uint8_t* last = tiles.record[tiles.count - 1];
  std::memcpy(last, &samples, sizeof(samples));
  std::memset(last + 4, 0, 4);
  std::memcpy(last + kZPassOffset, &samples, sizeof(samples));
  std::memset(last + kZPassOffset + 4, 0, 4);
}

void Publish(uint32_t query, uint32_t use, uint32_t samples) {
  std::lock_guard lock(g_mutex);
  const auto it = g_entries.find(query);
  if (it == g_entries.end() || it->second.use != use) {
    return;  // the game began the query again: this count belongs to an earlier use
  }
  it->second.ready = true;
  it->second.samples = samples;
}

}  // namespace occlusion

// ---------------------------------------------------------------------------
// the host's side
// ---------------------------------------------------------------------------

GuestOcclusion::~GuestOcclusion() {
  Shutdown();
}

void GuestOcclusion::Initialize(Device& device) {
  // CHECK: Plume's occlusion queries are the only host counter used (no shader counter, no
  // layout change); the edram coverage detector (mcla_gpu_coverage_queries, off by
  // default) has its own pools and a draw it wraps is not counted here (Vulkan has no
  // nesting).
  if (usable_) {
    return;
  }
  device_ = &device;
  if (!device.capabilities().occlusionQueries) {
    REXLOG_WARN("[gpu] occlusion: {} cannot count the samples of an occlusion query exactly: the "
                "guest's queries (the sun's glare) never finish and the game keeps their last "
                "value",
                device.backend_info());
    return;
  }
  for (Slot& slot : slots_) {
    slot.pool = device.CreateQueryPool(kQueriesPerSlot, "guest occlusion queries",
                                       plume::RenderQueryType::OCCLUSION);
    if (!slot.pool) {
      REXLOG_ERROR("[gpu] occlusion: the query pools could not be made: the guest's queries (the "
                   "sun's glare) never finish and the game keeps their last value");
      Shutdown();
      return;
    }
    slot.items.reserve(kQueriesPerSlot);
  }
  usable_ = true;
  occlusion::g_active.store(true, std::memory_order_release);
  REXLOG_INFO("[gpu] occlusion: the guest's occlusion queries are counted on the host ({} draws a "
              "frame; a frame's counts are read once its fence was waited, one frame late)",
              kQueriesPerSlot);
}

void GuestOcclusion::Shutdown() {
  occlusion::g_active.store(false, std::memory_order_release);
  if (begins_ != 0) {
    REXLOG_INFO(
        "[gpu] occlusion: {} guest queries begun, {} delivered (latency from the end to the "
        "count: mean {:.1f} ms, max {:.1f} ms); not delivered: {} a draw was left out, {} pool "
        "full or draw not countable, {} nested, {} frame never submitted; {} stray ends",
        begins_, delivered_, delivered_ ? latency_sum_ms_ / double(delivered_) : 0.0,
        latency_max_ms_, lost_draw_, lost_overflow_, lost_nested_, lost_frame_, stray_ends_);
    begins_ = 0;
  }
  usable_ = false;
  for (Slot& slot : slots_) {
    slot.pool.reset();
    slot.items.clear();
    slot.reset_count = 0;
    slot.reset_done = false;
    slot.submitted = false;
  }
  uses_.clear();
  open_ = 0;
  {
    std::lock_guard lock(occlusion::g_mutex);
    occlusion::g_entries.clear();
  }
  device_ = nullptr;
}

void GuestOcclusion::OnRecord(const OcclusionRecord& record) {
  if (!usable_) {
    return;
  }
  if (record.end == 0) {
    ++begins_;
    if (open_ != 0) {
      // One counter: Direct3D 9 does not nest occlusion queries. The earlier one is lost.
      ++lost_nested_;
      Abandon(open_);
    }
    Use use;
    use.query = record.query;
    use.use = record.use;
    open_ = next_serial_++;
    uses_.emplace(open_, use);
    return;
  }
  const auto it = open_ != 0 ? uses_.find(open_) : uses_.end();
  if (it == uses_.end() || it->second.query != record.query || it->second.use != record.use) {
    if (stray_ends_++ < 4) {
      REXLOG_WARN("[gpu] occlusion: an end of query {:#010x} (use {}) with no matching begin; "
                  "ignored",
                  record.query, record.use);
    }
    return;
  }
  const uint64_t serial = open_;
  open_ = 0;
  it->second.ended = true;
  it->second.ended_at = std::chrono::steady_clock::now();
  if (it->second.outstanding == 0) {
    Finish(serial);
  }
}

void GuestOcclusion::BeginFrame(plume::RenderCommandList* list, uint32_t slot_index) {
  if (!usable_ || slot_index >= kFramesInFlight) {
    return;
  }
  Slot& slot = slots_[slot_index];
  if (!slot.items.empty()) {
    // Nothing read this slot after its last round (a present that never got to the
    // end): the frame ring has waited its fence by now.
    OnSlotReady(slot_index);
  }
  // Vulkan wants a query reset before it is used again; the whole pool once, then
  // the queries the last round used.
  const uint32_t count = slot.reset_done ? slot.reset_count : kQueriesPerSlot;
  if (count != 0 && list) {
    list->resetQueryPool(slot.pool.get(), 0, count);
  }
  slot.reset_done = true;
  slot.reset_count = 0;
}

uint32_t GuestOcclusion::BeginDraw(plume::RenderCommandList* list, uint32_t slot_index,
                                   double to_guest, bool allow) {
  if (!usable_ || open_ == 0 || slot_index >= kFramesInFlight || !list) {
    return kNoQuery;
  }
  const auto it = uses_.find(open_);
  if (it == uses_.end() || it->second.lost) {
    return kNoQuery;
  }
  Use& use = it->second;
  Slot& slot = slots_[slot_index];
  if (!allow) {
    use.lost = true;
    ++lost_nested_;
    return kNoQuery;
  }
  if (slot.items.size() >= kQueriesPerSlot) {
    use.lost = true;
    ++lost_overflow_;
    return kNoQuery;
  }
  const uint32_t index = uint32_t(slot.items.size());
  slot.items.push_back(Item{open_, to_guest});
  ++use.outstanding;
  ++use.draws;
  draw_slot_ = slot_index;
  list->beginOcclusionQuery(slot.pool.get(), index);
  return index;
}

void GuestOcclusion::EndDraw(plume::RenderCommandList* list, uint32_t index) {
  if (index == kNoQuery || !list) {
    return;
  }
  list->endOcclusionQuery(slots_[draw_slot_].pool.get(), index);
}

void GuestOcclusion::NoteLostDraw() {
  if (!usable_ || open_ == 0) {
    return;
  }
  const auto it = uses_.find(open_);
  if (it != uses_.end() && !it->second.lost) {
    it->second.lost = true;
    ++lost_draw_;
  }
}

void GuestOcclusion::OnSubmitted(uint32_t slot_index) {
  if (usable_ && slot_index < kFramesInFlight) {
    slots_[slot_index].submitted = true;
  }
}

void GuestOcclusion::OnSlotReady(uint32_t slot_index) {
  if (!usable_ || slot_index >= kFramesInFlight) {
    return;
  }
  Slot& slot = slots_[slot_index];
  if (slot.items.empty()) {
    slot.submitted = false;
    return;
  }
  const uint64_t* results = nullptr;
  if (slot.submitted && !(device_ && device_->IsLost())) {
    slot.pool->queryResults();
    results = slot.pool->getResults();
  }
  for (size_t i = 0; i < slot.items.size(); ++i) {
    const auto it = uses_.find(slot.items[i].serial);
    if (it == uses_.end()) {
      continue;
    }
    if (results) {
      it->second.raw += double(results[i]);
      it->second.samples += double(results[i]) * slot.items[i].to_guest;
    } else if (!it->second.lost) {
      it->second.lost = true;
      ++lost_frame_;
    }
  }
  // Every draw of the slot has been read (or given up): the uses it completes are done.
  for (size_t i = 0; i < slot.items.size(); ++i) {
    const uint64_t serial = slot.items[i].serial;
    const auto it = uses_.find(serial);
    if (it == uses_.end()) {
      continue;
    }
    if (it->second.outstanding > 0) {
      --it->second.outstanding;
    }
    if (it->second.ended && it->second.outstanding == 0) {
      Finish(serial);
    }
  }
  slot.reset_count = uint32_t(slot.items.size());
  slot.items.clear();
  slot.submitted = false;
}

void GuestOcclusion::Finish(uint64_t serial) {
  const auto it = uses_.find(serial);
  if (it == uses_.end()) {
    return;
  }
  const Use use = it->second;
  uses_.erase(it);
  if (use.lost) {
    return;
  }
  const double rounded = std::round(use.samples);
  const uint32_t samples = rounded >= double(UINT32_MAX) ? UINT32_MAX : uint32_t(rounded);
  occlusion::Publish(use.query, use.use, samples);
  ++delivered_;
  const double latency_ms =
      std::max(0.0, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                              use.ended_at)
                        .count());
  latency_sum_ms_ += latency_ms;
  latency_max_ms_ = std::max(latency_max_ms_, latency_ms);
  if (logged_ < 16 || delivered_ % 600 == 0) {
    ++logged_;
    REXLOG_INFO("[gpu] occlusion: query {:#010x}, use {}: {} draws, {} host samples, {} guest "
                "samples told to the game, {:.1f} ms after its end ({} delivered so far)",
                use.query, use.use, use.draws, uint64_t(use.raw), samples, latency_ms, delivered_);
  }
}

void GuestOcclusion::Abandon(uint64_t serial) {
  const auto it = uses_.find(serial);
  if (it != uses_.end()) {
    if (it->second.outstanding == 0) {
      uses_.erase(it);
    } else {
      // Draws of it are still in a pool: they are read and dropped.
      it->second.lost = true;
      it->second.ended = true;
    }
  }
  if (open_ == serial) {
    open_ = 0;
  }
}

}  // namespace mcla::gpu
