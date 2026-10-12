#pragma once
// ===========================================================================
// MCLA GPU renderer -- the records guest threads write into the command stream
// ===========================================================================
// One record per guest event that produces GPU work: draws, clears, resolves, the two ends of tiled
// rendering, and the present. Each carries the device state its event
// consumed, copied at the moment it was valid. Plus the guest's GPU fences
// (24.20), which the render thread retires as it executes.
//
// Conventions:
//  - Blocks copied out of guest memory (register and fetch windows, constant
//    blocks, declaration records, object headers, rectangles, inline vertices)
//    stay RAW, big-endian, exactly as the guest had them. Interpreting them is
//    the render thread's job, in one place (draw_decode.cpp).
//  - Scalars the capture had to read in order to follow them (pointers,
//    counts, sizes) and the capture's own results (identities, flags) are in
//    host order.
//  - A field the capture could not read is zero, with a flag saying so. Never
//    invented data.
//
// Fixed parts are plain structs, written in place in the stream. Variable
// parts follow the fixed part at offsets DrawRecordLayout computes, so writer
// and reader cannot disagree.
//
// Threading: types only.
// ===========================================================================

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "guest_device.h"

namespace mcla::gpu {

enum class RecordType : uint8_t {
  kPad,  // filler to the end of the buffer; never handed to an executor
  kPresent,
  kDraw,
  kClear,
  kResolve,
  kTilingBegin,
  kTilingEnd,
  kFence,
  kCount,  // number of types; not a record
};

const char* RecordTypeName(RecordType type);

enum RecordHeaderFlags : uint8_t {
  // The producer waits for this record to be executed (the present): the
  // consumer reports its sequence as executed right after running it.
  kRecordSync = 1u << 0,
};

// When a committed record becomes visible to the render thread
// (CommandStream::Writer::Commit).
enum class Publication : uint8_t {
  kBatched,  // with the batch (stream.h): most records
  kNow,      // at once, nobody waits for its execution (a fence: the guest waits
             // for the retired value instead)
  kWaited,   // at once and flagged kRecordSync: the producer waits (the present)
};

struct RecordHeader {
  uint32_t size = 0;  // whole record, header included, multiple of kRecordAlignment
  RecordType type = RecordType::kPad;
  uint8_t flags = 0;  // RecordHeaderFlags
  uint8_t reserved[2] = {};
  uint64_t sequence = 0;
};
static_assert(sizeof(RecordHeader) == 16);

inline constexpr uint32_t kRecordAlignment = 16;

constexpr uint32_t AlignRecord(uint32_t bytes) {
  return (bytes + kRecordAlignment - 1) & ~(kRecordAlignment - 1);
}

// --- present -----------------------------------------------------------------

enum PresentFlags : uint32_t {
  kPresentFrontBufferUnreadable = 1u << 0,
};

// The guest capture's own measurements for the frame this present ends, so the
// render thread reports guest and render costs side by side.
struct GuestFrameStats {
  uint64_t capture_ns = 0;          // time spent inside the capture functions
  uint64_t bytes = 0;               // stream bytes the frame wrote
  uint32_t draws = 0;
  uint32_t records = 0;
  uint32_t identity_new = 0;        // microcode seen for the first time (hashed)
  uint32_t identity_renormalized = 0;  // known (address, size), new raw bytes
  uint32_t constant_blocks_masked = 0;  // blocks sent because the dirty mask named them
  uint32_t constant_blocks_unmasked = 0;  // changed with their dirty bit clear (verify mode)
  uint32_t inline_unterminated = 0;
  uint32_t unreadable = 0;          // any guest pointer the capture could not follow
  uint32_t fences = 0;              // kFence records
};

struct PresentRecord {
  uint32_t device = 0;
  uint32_t front_buffer = 0;  // D3DDevice_Swap r4: the D3DTexture EndFrame resolved RT0 into
  uint32_t flags = 0;
  uint32_t reserved = 0;
  GuestFrameStats stats;
  uint8_t front_buffer_header[guest::kTextureHeaderBytes] = {};
};

// --- draw ----------------------------------------------------------------------

enum class DrawKind : uint8_t { kIndexed, kAuto, kInline };

enum DrawFlags : uint32_t {
  kDrawVsUnreadable = 1u << 0,         // VS object or microcode not readable
  kDrawPsUnreadable = 1u << 1,         // PS bound but object or microcode not readable
  kDrawNoPixelShader = 1u << 2,        // no PS bound (depth-only)
  kDrawDeclUnreadable = 1u << 3,
  kDrawDeclTooLong = 1u << 4,          // more than kMaxDeclElements
  kDrawIndexUnreadable = 1u << 5,
  kDrawInlineUnreadable = 1u << 6,     // the vertices EndVertices committed
  kDrawInlineUnterminated = 1u << 7,   // closed by another event, not by EndVertices
  kDrawVsComputedFetchIndex = 1u << 8, // from the VS microcode (ProbeComputedVertexFetchIndex)
  kDrawBankCheck = 1u << 9,            // bank_check_vs/ps are valid
};

struct DrawRecord {
  uint32_t device = 0;
  DrawKind kind = DrawKind::kIndexed;
  uint8_t vs_variant = 0;
  uint16_t decl_count = 0;      // declaration records that follow
  uint32_t primitive = 0;       // xenos primitive type (the call's r4)
  uint32_t count = 0;           // indices or vertices
  uint32_t start = 0;           // start index (indexed) / start vertex (auto)
  int32_t base_vertex = 0;      // indexed only
  uint32_t inline_stride = 0;   // inline only
  uint32_t inline_bytes = 0;    // inline only: count * stride, follows the deltas
  uint32_t flags = 0;
  uint32_t index_object = 0;
  uint32_t index_dword0 = 0;    // +0: bit 31 32-bit indices, bits 30..29 endian
  uint32_t index_address = 0;   // +24, as stored (windowed)
  uint32_t vs_object = 0;
  uint32_t ps_object = 0;
  uint32_t vs_ucode_address = 0;
  uint32_t vs_ucode_size = 0;
  uint32_t ps_ucode_address = 0;
  uint32_t ps_ucode_size = 0;
  uint64_t vs_identity = 0;
  uint64_t ps_identity = 0;
  uint32_t decl_object = 0;
  uint16_t vs_delta_count = 0;  // 64-byte VS constant blocks that follow
  uint16_t ps_delta_count = 0;
  uint8_t viz_query_index = 0;  // byte dev+12187
  uint8_t viz_query_id = 0;     // byte dev+12122[index]
  uint8_t reserved0[2] = {};
  uint32_t device_flags = 0;    // dword dev+10940, raw
  uint32_t surfaces[guest::kSurfaceCount] = {};
  uint32_t sampler_textures[guest::kSamplerCount] = {};
  uint64_t bank_check_vs = 0;   // XXH3 of the device's VS bank, raw (kDrawBankCheck)
  uint64_t bank_check_ps = 0;
  uint8_t stream_strides[guest::kStreamCount] = {};
  uint8_t fetch[guest::kFetchShadowBytes] = {};          // window F, raw
  uint8_t registers[guest::kRegisterWindowBytes] = {};   // window R, raw
};
static_assert(std::is_trivially_copyable_v<DrawRecord>);

// Offsets of a draw's variable parts, relative to the start of the payload
// (the DrawRecord). Order: declaration records, VS block indices, PS block
// indices, VS blocks, PS blocks, inline vertices. Every part starts 16-aligned.
struct DrawRecordLayout {
  uint32_t decl = 0;
  uint32_t vs_indices = 0;
  uint32_t ps_indices = 0;
  uint32_t vs_blocks = 0;
  uint32_t ps_blocks = 0;
  uint32_t inline_data = 0;
  uint32_t payload_bytes = 0;
};

constexpr DrawRecordLayout ComputeDrawLayout(uint32_t decl_count, uint32_t vs_deltas,
                                             uint32_t ps_deltas, uint32_t inline_bytes) {
  DrawRecordLayout l;
  uint32_t at = AlignRecord(sizeof(DrawRecord));
  l.decl = at;
  at = AlignRecord(at + decl_count * guest::kDeclElementBytes);
  l.vs_indices = at;
  at += vs_deltas;
  l.ps_indices = at;
  at = AlignRecord(at + ps_deltas);
  l.vs_blocks = at;
  at += vs_deltas * guest::kConstantBlockBytes;
  l.ps_blocks = at;
  at += ps_deltas * guest::kConstantBlockBytes;
  l.inline_data = at;
  at = AlignRecord(at + inline_bytes);
  l.payload_bytes = at;
  return l;
}

// --- events with a before/after state: clear, resolve, tiling ---------------------

// Window P before the call and window R after it, plus the five bound surfaces'
// headers read before the call. Shared by every event record below.
enum EventFlags : uint32_t {
  kEventSurfaceUnreadable = 1u << 0,  // a bound surface header could not be read
  kEventRectUnreadable = 1u << 1,
  kEventColorUnreadable = 1u << 2,
  kEventPointUnreadable = 1u << 3,
  kEventDestUnreadable = 1u << 4,     // destination texture object
  kEventStackUnreadable = 1u << 5,    // Resolve's stack arguments
  kEventInsideEndTiling = 1u << 6,    // a resolve EndTiling issued for one tile
  kEventHasRect = 1u << 7,
  kEventHasColor = 1u << 8,
  kEventHasPoint = 1u << 9,
  kEventHasDest = 1u << 10,
  kEventHasRects = 1u << 11,          // tiling: the rectangle array was given
};

struct EventState {
  uint8_t pre[guest::kEventWindowBytes] = {};      // window P, raw, before the call
  uint8_t post[guest::kRegisterWindowBytes] = {};  // window R, raw, after the call
  uint8_t surfaces[guest::kSurfaceCount][guest::kSurfaceHeaderBytes] = {};
};

// ClearF (sub_824194C0): every clear funnels here (Clear per rectangle,
// BeginTiling, the Z pass).
struct ClearRecord {
  uint32_t device = 0;
  uint32_t caller = 0;   // return address: which of the three callers
  uint32_t clear_flags = 0;  // r4: 0xF colour targets, 0x10 depth, 0x20 stencil
  float z = 0.0f;        // f1
  uint32_t stencil = 0;  // r8
  uint32_t rect_ptr = 0;   // r5, 0 = whole surface
  uint32_t color_ptr = 0;  // r6, four floats
  uint32_t flags = 0;      // EventFlags
  uint8_t rect[16] = {};   // x0, y0, x1, y1 (kEventHasRect)
  uint8_t color[16] = {};  // (kEventHasColor)
  EventState state;
};

// D3DDevice_Resolve (sub_82420BA8).
struct ResolveRecord {
  uint32_t device = 0;
  uint32_t caller = 0;
  uint32_t resolve_flags = 0;  // r4: & 7 = source (0..3 colour, 4 depth), clear bits 0x100/0x200
  uint32_t src_rect_ptr = 0;   // r5
  uint32_t dest_ptr = 0;       // r6
  uint32_t dest_point_ptr = 0; // r7
  uint32_t dest_level = 0;     // r8
  uint32_t dest_slice = 0;     // r9
  uint32_t clear_color_ptr = 0;  // r10
  float clear_z = 0.0f;          // f1
  uint32_t clear_stencil = 0;    // [sp+0x5C]
  uint32_t params_ptr = 0;       // [sp+0x64]
  uint32_t flags = 0;            // EventFlags
  uint32_t reserved = 0;
  uint8_t src_rect[16] = {};
  uint8_t dest_point[8] = {};
  uint8_t reserved1[8] = {};
  uint8_t clear_color[16] = {};
  uint8_t dest_header[guest::kTextureHeaderBytes] = {};
  uint8_t reserved2[12] = {};
  EventState state;
};

// D3DDevice_BeginTiling (sub_8241BE78). The clear it may issue through ClearF
// (caller 0x8241C160) is its own ClearRecord, written before this one.
struct TilingBeginRecord {
  uint32_t device = 0;
  uint32_t caller = 0;
  uint32_t tiling_flags = 0;  // r4
  uint32_t tile_count = 0;    // r5
  uint32_t rects_ptr = 0;     // r6
  uint32_t clear_color_ptr = 0;  // r7
  float clear_z = 0.0f;          // f1
  uint32_t clear_stencil = 0;    // r9
  uint32_t flags = 0;            // EventFlags
  uint32_t reserved = 0;
  uint8_t rects[guest::kMaxTiles][16] = {};
  uint8_t clear_color[16] = {};
  EventState state;
};

// D3DDevice_EndTiling (sub_8241C308): one Resolve per tile, each its own
// ResolveRecord flagged kEventInsideEndTiling, written before this one.
struct TilingEndRecord {
  uint32_t device = 0;
  uint32_t caller = 0;
  uint32_t resolve_flags = 0;  // r4
  uint32_t rects_ptr = 0;      // r5, 0 = the tiles' own rectangles
  uint32_t dest_ptr = 0;       // r6
  uint32_t clear_color_ptr = 0;  // r7
  float clear_z = 0.0f;          // f1
  uint32_t clear_stencil = 0;    // r9
  uint32_t params_ptr = 0;       // r10
  uint32_t nested_resolves = 0;  // resolve records the call produced
  uint32_t flags = 0;            // EventFlags
  uint32_t reserved = 0;
  uint8_t rects[guest::kMaxTiles][16] = {};
  uint8_t clear_color[16] = {};
  uint8_t dest_header[guest::kTextureHeaderBytes] = {};
  uint8_t reserved1[12] = {};
  EventState state;
};

// --- fence -----------------------------------------------------------------------

// A GPU fence the guest inserted (sub_82411CD0, from Submit and from the end
// of tiled rendering): the EVENT_WRITE that makes the GPU write `value` into
// the guest's retired dword once everything submitted before it is done. The
// render thread plays that GPU and publishes `value` when it executes this
// record, i.e. after every record captured before the insertion. Also written once by the hand-over at startup, to cover the fences
// inserted before the capture was on.
struct FenceRecord {
  uint32_t device = 0;
  uint32_t value = 0;  // the device's issued counter before the insertion
};

static_assert(std::is_trivially_copyable_v<ClearRecord>);
static_assert(std::is_trivially_copyable_v<ResolveRecord>);
static_assert(std::is_trivially_copyable_v<TilingBeginRecord>);
static_assert(std::is_trivially_copyable_v<TilingEndRecord>);
static_assert(std::is_trivially_copyable_v<PresentRecord>);
static_assert(std::is_trivially_copyable_v<FenceRecord>);

// Raw guest reads, for decoders.
inline uint32_t LoadBe32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}
inline uint16_t LoadBe16(const uint8_t* p) { return uint16_t(uint16_t(p[0]) << 8 | p[1]); }

}  // namespace mcla::gpu
