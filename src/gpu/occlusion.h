#pragma once
// ===========================================================================
// MCLA GPU renderer -- the guest's occlusion queries
// ===========================================================================
// The game scales the sun's glare by an occlusion query: each frame it asks how
// many samples of the sun's quad pass the depth test (one query object of two
// used in turn, read two frames after it was issued; sub_822DD088) and draws the
// glare with the count divided by the 1,250 samples it expects. The console
// answers by writing the GPU's sample counters to the query's record. Nothing
// executes the guest's command buffer here, so nothing wrote them: the record
// kept the sentinel the game puts there and every read said "not finished".
//
// The guest's side (D3DQuery::Issue and GetData, hooks.cpp). A record is two
// 32-byte sample count structs, the END snapshot at +0x00 and the BEGIN snapshot
// at +0x20, one record per tile of predicated tiling (the pointers are an array
// in the query object, the last slot holds the sentinel); the game subtracts the
// begin from the end and adds the tiles up, and takes the query as finished when
// the end snapshot's ZPass words no longer hold 0xFFFFFEED (sub_8241E088). The
// events arrive through sub_824302F0(device, query, is_begin), the one function
// that emits an EVENT_WRITE_ZPD, called only by Issue:
//  - begin: the begin snapshots are zeroed (guest thread, at once) and the use is
//    numbered; the end snapshots are written at the end;
//  - end: what the console's memory writes would have done and nothing executes
//    -- the end snapshots zeroed except the last tile's ZPass pair, which holds the
//    sentinel -- also at once. The game stores the sentinel with the CPU only when
//    the query's last result was read; with a result pending it queues two PM4
//    memory writes instead, which nobody runs, so the hook stores it either way;
//  - GetData: before the original, a count the host has delivered for this very
//    use is written into the last tile's end snapshot (Total and ZPass, the way
//    the console's GPU would have), so the original finds the query finished.
//    Every write to guest memory is the guest thread's, in the order the game
//    asks: the render thread never touches the records, so there is no race with
//    the game's own sentinel stores and a late count cannot land on a record the
//    game has armed again (a use's count is dropped once another use began).
//
// The host's side (the render thread, in stream order with the draws). The
// begin and end are records (RecordType::kOcclusion). While a query is open,
// every draw the Scene records is wrapped in an occlusion query of its own, slot
// by slot of the frame's pool (Vulkan wants a query to begin and end inside one
// render pass; a draw is the unit that always does), and the guest query's count
// is the sum of its draws', in guest samples: the host samples per guest sample
// (a host target with fewer samples than the guest's) and the resolution scale
// squared divided out. A guest query that spans a present has draws in two
// frames' pools and is complete when both were read.
//
// Latency. Nothing waits for the GPU. The counts of a frame's pool are read
// when the ring has waited that frame's fence (Advance, at the end of the next
// frame's present): a query of frame N is delivered at the end of frame N+1's
// present on the render thread, which is the best the frame ring allows without
// polling a fence (Plume has none that does not consume it). The game reads a
// query two frames after issuing it, so the count is normally in time; when it
// is not, the read says "not finished" and the game keeps its last value for
// that frame, as it does on the console.
//
// A guest query is not delivered (the sentinel stays: the game keeps its value)
// when a draw in it was left out by the replay (a pipeline still being built, an
// unreadable object: the count would be too low), when the frame's pool was full,
// when its frame was never submitted, or when a guest query began inside it (one
// counter: the game does not nest queries). Without exact host occlusion queries
// nothing here runs and the guest sees what it saw before: a query that never
// finishes.
//
// Threading: Active and the guest-side functions on any guest thread; the
// GuestOcclusion class on the render thread.
// ===========================================================================

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>

#include "frame.h"
#include "records.h"

namespace mcla::gpu {

class Device;

namespace occlusion {

// True while host occlusion queries are in use (GuestOcclusion::Initialize made
// its pools): the guest hooks do nothing otherwise. Any thread.
bool Active();

// D3DQuery::Issue's sample count event for the query object `query`. Guest
// thread, before the original. Returns whether a record is to be written, with
// the number of the query's use for it.
bool OnGuestIssue(uint8_t* base, uint32_t query, bool is_begin, uint32_t* use);
// D3DQuery::GetData, before the original. Guest thread.
void OnGuestGetData(uint8_t* base, uint32_t query);
// The host's count for use `use` of `query`, in guest samples; dropped if the game began
// the query again since. Render thread.
void Publish(uint32_t query, uint32_t use, uint32_t samples);

}  // namespace occlusion

class GuestOcclusion {
 public:
  // Draws that can be counted in one frame (the sun's query is four draws).
  static constexpr uint32_t kQueriesPerSlot = 256;
  static constexpr uint32_t kNoQuery = UINT32_MAX;

  GuestOcclusion() = default;
  ~GuestOcclusion();
  GuestOcclusion(const GuestOcclusion&) = delete;
  GuestOcclusion& operator=(const GuestOcclusion&) = delete;

  // Never fails the renderer: without exact occlusion queries (or pools) the
  // guest's queries stay as they were, and the log says so.
  void Initialize(Device& device);
  void Shutdown();

  // A begin or an end, at its place in the stream.
  void OnRecord(const OcclusionRecord& record);
  // A guest query is open: the draws are counted.
  bool query_open() const { return open_ != 0; }

  // Outside any render pass, on the first list of the frame in `slot`: resets the
  // queries the slot's previous round used.
  void BeginFrame(plume::RenderCommandList* list, uint32_t slot);
  // Around a recorded draw while a query is open. BeginDraw returns the query's
  // index, kNoQuery when the draw cannot be counted (the guest query is then
  // lost). `to_guest`: the factor from host samples to guest samples for the
  // draw's targets. `allow` is false when the draw already sits inside another
  // occlusion query of the host (Vulkan has no nesting).
  uint32_t BeginDraw(plume::RenderCommandList* list, uint32_t slot, double to_guest, bool allow);
  void EndDraw(plume::RenderCommandList* list, uint32_t index);
  // A draw of the open query was left out (skipped, not "no effect"): its count
  // would be too low.
  void NoteLostDraw();

  // After FrameRing::Submit executed the slot's last list.
  void OnSubmitted(uint32_t slot);
  // After FrameRing::Advance: the slot about to be recorded had its fence
  // waited, so what its previous round counted can be read.
  void OnSlotReady(uint32_t slot);

 private:
  struct Use {
    uint32_t query = 0;
    uint32_t use = 0;
    bool ended = false;
    bool lost = false;
    uint32_t draws = 0;
    uint32_t outstanding = 0;  // draws whose slot has not been read yet
    double samples = 0;        // guest samples so far
    double raw = 0;            // host samples so far
    std::chrono::steady_clock::time_point ended_at{};
  };
  struct Item {
    uint64_t serial = 0;  // the Use it counts for
    double to_guest = 1.0;
  };
  struct Slot {
    std::unique_ptr<plume::RenderQueryPool> pool;
    std::vector<Item> items;
    uint32_t reset_count = 0;  // queries to reset at the slot's next frame
    bool reset_done = false;   // the pool was reset once, whole
    bool submitted = false;
  };

  void Finish(uint64_t serial);
  void Abandon(uint64_t serial);

  Device* device_ = nullptr;
  bool usable_ = false;
  Slot slots_[kFramesInFlight];
  std::unordered_map<uint64_t, Use> uses_;
  uint64_t open_ = 0;  // serial of the open use, 0: none
  uint64_t next_serial_ = 1;
  uint32_t draw_slot_ = 0;  // the slot BeginDraw used last, for EndDraw

  // Statistics: the whole run.
  uint64_t begins_ = 0;
  uint64_t delivered_ = 0;
  uint64_t lost_draw_ = 0;      // a draw of the query was left out
  uint64_t lost_overflow_ = 0;  // the frame's pool was full
  uint64_t lost_nested_ = 0;    // a begin inside an open query (or a host query around the draw)
  uint64_t lost_frame_ = 0;     // the frame was never submitted
  uint64_t stray_ends_ = 0;     // an end with no matching begin
  uint64_t logged_ = 0;
  double latency_sum_ms_ = 0;
  double latency_max_ms_ = 0;
};

}  // namespace mcla::gpu
