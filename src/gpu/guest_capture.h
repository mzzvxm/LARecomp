#pragma once
// ===========================================================================
// MCLA GPU renderer -- capture of the guest's D3D calls
// ===========================================================================
// Runs on the guest threads, inside the hooks (gpu.h), and turns each event
// into a record in the command stream with the device state it consumed:
//
//  - Draws: the registers the draw builders flush (window R), the fetch
//    constants (window F), the bound objects and their pointers, the
//    declaration records, and only the 64-byte constant blocks that changed.
//    Inline draws (BeginVertices/EndVertices) are captured at Begin -- the
//    state the packet was built from -- and emitted at End with the vertices
//    the caller wrote.
//  - Clears (ClearF, the funnel), resolves, BeginTiling, EndTiling: the device
//    window P before the call, window R after it, the bound surfaces' headers
//    and every block an argument points at.
//  - The present: the front buffer and the frame's capture statistics.
//  - The guest's GPU fences, which the render thread retires as
//    it executes them.
//
// Constants. A guest-side shadow of each device's two banks holds what the
// render thread has been sent. A draw sends the blocks whose dirty bit was set
// and whose bytes differ from the shadow; with verification on
// (mcla_gpu_verify_constants) every block is compared, and one that changed
// with its dirty bit clear is sent too and counted -- correctness never rests
// on the mask. Every 32 draws, and on a device's first draw after a present,
// the record carries an XXH3 of both banks as the device holds them, which the
// render thread compares with its reconstruction.
//
// Identity. Exact, with no hash per draw: (address, size) -> the raw bytes
// seen there (up to kMaxRawVariants, since the D3D runtime repatches a VS's
// vfetches per declaration), the normalized bytes, the identity. Raw bytes that
// match a known variant are a memcmp; new raw bytes are normalized and compared
// with the normalized copy; only genuinely new microcode is hashed.
//
// Every guest pointer is checked with IsGuestRangeReadable before it is read.
// What cannot be read becomes a zero field and a flag, never a crash.
//
// Threading: any guest thread. One mutex serializes the capture state and the
// writes into the stream; the pre-state of clear/resolve/tiling events lives
// on the calling thread (the original runs between Before and After, and an
// EndTiling nests its resolves).
// ===========================================================================

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "gpu.h"
#include "records.h"
#include "stream.h"

namespace mcla::gpu {

class GuestCapture {
 public:
  static constexpr uint32_t kMaxRawVariants = 8;
  static constexpr uint32_t kBankCheckInterval = 32;
  // The game ships ~2000 shaders; the bound only matters if microcode keeps
  // arriving at new addresses for a whole session.
  static constexpr size_t kMaxIdentityEntries = 16384;

  explicit GuestCapture(CommandStream& stream, bool verify_constants);
  GuestCapture(const GuestCapture&) = delete;
  GuestCapture& operator=(const GuestCapture&) = delete;

  // After this every entry point is a no-op; a thread already inside finishes
  // its record or fails it on the stopped stream.
  void Disable() { enabled_.store(false, std::memory_order_release); }
  bool enabled() const { return enabled_.load(std::memory_order_acquire); }

  GuestCallToken BeforeDraw(const uint8_t* base, uint32_t device);
  void AfterDraw(const GuestCallToken& token, const uint8_t* base, uint32_t device, DrawKind kind,
                 uint32_t primitive, uint32_t count, uint32_t start, int32_t base_vertex);
  void AfterBeginVertices(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                          uint32_t primitive, uint32_t count, uint32_t stride, uint32_t buffer);
  void AfterEndVertices(const uint8_t* base, uint32_t device);

  GuestCallToken BeforeEvent(const uint8_t* base, uint32_t device, GuestEvent event,
                             uint32_t stack_pointer);
  void AfterClear(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                  uint32_t caller, uint32_t flags, uint32_t rect, uint32_t color, float z,
                  uint32_t stencil);
  void AfterResolve(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                    uint32_t caller, uint32_t flags, uint32_t source_rect, uint32_t dest_texture,
                    uint32_t dest_point, uint32_t dest_level, uint32_t dest_slice,
                    uint32_t clear_color, float clear_z);
  void AfterBeginTiling(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                        uint32_t caller, uint32_t flags, uint32_t tile_count, uint32_t rects,
                        uint32_t clear_color, float clear_z, uint32_t clear_stencil);
  void AfterEndTiling(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                      uint32_t caller, uint32_t flags, uint32_t rects, uint32_t dest_texture,
                      uint32_t clear_color, float clear_z, uint32_t clear_stencil,
                      uint32_t params);

  // Writes the present and waits for the render thread to run it.
  void Present(const uint8_t* base, uint32_t device, uint32_t front_buffer);

  // A fence the guest inserted (sub_82411CD0), called AFTER the original:
  // whether the capture is on is decided here, after the insertion, which is
  // what makes the startup hand-over exact. `value` is the issued
  // counter read before the original. Published at once.
  void Fence(uint32_t device, uint32_t value);
  // The startup hand-over: one fence record that covers every fence the guest
  // inserted before the capture was on, with the issued counter read under the
  // stream's producer lock. False when there is nothing to cover (no device,
  // ring not set up). Published at once.
  bool FenceCatchUp(const uint8_t* base, uint32_t device);

 private:
  struct DeviceState {
    uint32_t device = 0;
    bool shadows_valid = false;
    bool check_next = true;  // send a bank check with the next draw
    uint32_t draws_since_check = 0;
    std::array<uint8_t, guest::kConstantBankBytes> vs_shadow{};
    std::array<uint8_t, guest::kConstantBankBytes> ps_shadow{};
  };

  struct IdentityEntry {
    std::vector<std::vector<uint8_t>> raw;  // most recent last
    std::vector<uint8_t> normalized;
    uint64_t identity = 0;
    bool computed_fetch_index = false;
  };

  struct ShaderRead {
    uint32_t object = 0;
    uint32_t address = 0;
    uint32_t size = 0;
    uint64_t identity = 0;
    bool computed_fetch_index = false;
    bool readable = false;
  };

  // A draw's state, gathered before the stream record is reserved so the
  // record can be sized exactly.
  struct DrawCapture {
    DrawKind kind = DrawKind::kIndexed;
    uint32_t primitive = 0;
    uint32_t count = 0;
    uint32_t start = 0;
    int32_t base_vertex = 0;
    uint32_t inline_stride = 0;
    uint32_t inline_bytes = 0;
    uint32_t flags = 0;
    uint32_t vs_variant = 0;
    ShaderRead vs;
    ShaderRead ps;
    uint32_t decl_object = 0;
    uint32_t decl_count = 0;  // records to copy (0 when unreadable or too long)
    uint32_t index_object = 0;
    uint32_t index_dword0 = 0;
    uint32_t index_address = 0;
    std::array<uint8_t, guest::kConstantBlocksPerBank> vs_blocks{};
    std::array<uint8_t, guest::kConstantBlocksPerBank> ps_blocks{};
    uint32_t vs_block_count = 0;
    uint32_t ps_block_count = 0;
    bool bank_check = false;
  };

  // The inline draw between BeginVertices and EndVertices.
  struct PendingInline {
    bool active = false;
    uint32_t device = 0;
    uint32_t buffer = 0;
    std::vector<uint8_t> payload;  // the whole record payload; vertices filled at End
  };

  DeviceState& StateFor(uint32_t device);
  ShaderRead ReadVertexShader(const uint8_t* base, uint32_t device, uint32_t ps_object,
                              uint32_t* variant);
  ShaderRead ReadPixelShader(const uint8_t* base, uint32_t ps_object);
  void ResolveIdentity(const uint8_t* base, ShaderRead& shader, bool vertex);
  void GatherDraw(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                  DrawCapture& capture);
  // Fills a draw payload of capture's exact layout and updates the shadows.
  void WriteDraw(const uint8_t* base, uint32_t device, const DrawCapture& capture,
                 uint8_t* payload);
  void FlushPendingInline(const uint8_t* base, uint32_t device, bool terminated);
  bool CopyGuest(const uint8_t* base, uint32_t address, void* out, uint32_t bytes);
  void CountRecord(uint32_t payload_bytes);

  CommandStream& stream_;
  const bool verify_constants_;
  std::atomic<bool> enabled_{true};

  std::mutex mutex_;
  std::vector<std::unique_ptr<DeviceState>> devices_;
  std::unordered_map<uint64_t, IdentityEntry> identities_;
  std::vector<uint8_t> scratch_;  // normalization buffer
  PendingInline pending_;
  GuestFrameStats frame_;
  bool reported_device_count_ = false;
};

}  // namespace mcla::gpu
