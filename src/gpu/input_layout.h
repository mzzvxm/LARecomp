#pragma once
// ===========================================================================
// MCLA GPU renderer -- vertex input layouts
// ===========================================================================
// Joins a vertex shader's inputs (the
// pack's vertex elements: usage and usage index per vfetch) with the draw's
// D3D vertex declaration: each input takes the FIRST declaration element with
// the same usage and index -- the rule of the D3D vfetch patcher sub_82423A38,
// which the legacy geometry follows -- and an input the declaration does not
// supply reads a zero stream, as the hardware does with a vfetch the patcher
// left alone.
//
// Formats. The table of gpu/format, with one adjustment: the translated shaders
// declare NORMAL, TANGENT and BINORMAL as uint4 and decode them themselves
// (tfetchR11G11B10: the packed 2_10_10_10 form with spec constant
// R11G11B10_NORMAL, asfloat of the raw dwords otherwise), so those inputs get
// the integer format of the same bits -- R32G32B32_FLOAT becomes
// R32G32B32_UINT. Vulkan requires the numeric types to match; on D3D12 the
// bits are the same either way. A uint4 input over a declaration type whose
// bits are not 32-bit floats (or the packed dword) cannot be decoded by the
// shader, and a float4 input over an integer type (UBYTE4) has no host format
// that converts it the way the hardware fetch does: both are reported, and a
// layout with a problem is not drawn with (the caller skips the draw).
//
// Swap plans. Guest vertex data is big-endian, and the hardware swaps each
// vfetch by the width of its components (the patcher derives it from the
// format), so a stream is swapped element by element: 16-bit components in
// 16-bit units, everything else in 32-bit units; bytes outside any element in
// 32-bit units. The plan covers every element the declaration puts in the
// stream (not only the ones this shader reads), so every shader drawing from
// the same buffer with the same declaration shares one swapped copy. Swapping
// FLOAT16 pairs in 16-bit units is also what puts them back in the order the
// hardware delivers, so g_SwappedTexcoords stays 0.
//
// Vulkan locations: the table LAR-XenosRecomp assigns them from
// (shader_recompiler.cpp USAGE_LOCATIONS); the pack does not carry them.
//
// Threading: render thread only.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

#include <plume_render_interface_types.h>

#include "draw_memo.h"
#include "guest_device.h"
#include "shaders.h"

namespace mcla::gpu {

// One byte range of a stream and the unit it is swapped in.
struct SwapRange {
  uint16_t offset = 0;
  uint16_t size = 0;
  uint8_t unit = 4;  // 2 or 4 bytes
};

struct StreamPlan {
  uint32_t stream = 0;        // guest stream number (fetch constant 95 - stream)
  uint32_t stride = 0;        // bytes (dev+12528 byte x 4)
  std::vector<SwapRange> swaps;  // sorted, disjoint; for the 8in32 endian (24.3: always)
  uint64_t plan_hash = 0;     // of stride + swaps, part of the buffer cache key
  // The byte offset of a float3/float4 POSITION (semantic index 0) in the
  // stream, or -1: the geometry checks its first vertex when it copies the
  // stream. Not in plan_hash -- it changes no byte of the copy.
  int32_t position_offset = -1;
};

struct InputLayout {
  uint32_t id = 0;  // stable per distinct layout, part of the pipeline key
  std::vector<plume::RenderInputElement> elements;
  std::vector<plume::RenderInputSlot> slots;  // slot i = streams[i]; the zero stream last
  std::vector<StreamPlan> streams;
  bool zero_stream = false;    // the last slot reads the zero buffer
  bool packed_normal = false;  // a NORMAL/TANGENT/BINORMAL uses the packed 2_10_10_10 type
  uint32_t problems = 0;       // inputs that cannot be supplied correctly (logged)
};

struct InputLayoutMemoView {
  uint64_t vs;
  std::span<const uint8_t> declaration;
  std::span<const uint8_t, guest::kStreamCount> strides;
};

// Own the bytes: draw records in the command stream are recycled.
struct InputLayoutMemoKey {
  uint64_t vs = 0;
  std::vector<uint8_t> declaration;
  std::array<uint8_t, guest::kStreamCount> strides{};
  bool operator==(const InputLayoutMemoView& view) const {
    return vs == view.vs && declaration.size() == view.declaration.size() &&
           (declaration.empty() ||
            std::memcmp(declaration.data(), view.declaration.data(), declaration.size()) == 0) &&
           std::memcmp(strides.data(), view.strides.data(), strides.size()) == 0;
  }
  InputLayoutMemoKey& operator=(const InputLayoutMemoView& view) {
    vs = view.vs;
    declaration.assign(view.declaration.begin(), view.declaration.end());
    std::copy(view.strides.begin(), view.strides.end(), strides.begin());
    return *this;
  }
};

class InputLayouts {
 public:
  explicit InputLayouts(const ShaderStore& shaders) : shaders_(shaders) {}

  // The layout for `vs` reading the declaration `decl` (decl_count raw 12-byte
  // big-endian D3DVERTEXELEMENT9) with the device's stream strides (dwords).
  const InputLayout* Get(const ShaderStore::Entry& vs, const uint8_t* decl, uint32_t decl_count,
                         const uint8_t* stream_strides, bool two_way);

  size_t size() const { return layouts_.size(); }

  // The second way is probed only when the first way misses.
  struct MemoStats {
    DrawMemoStats first;
    DrawMemoStats second;
  };
  MemoStats TakeMemoStats() {
    const MemoStats s = memo_stats_;
    memo_stats_ = MemoStats{};
    return s;
  }

 private:
  struct Key {
    uint64_t vs = 0;
    std::vector<uint8_t> bytes;  // decl records + the strides of the streams they name
    bool operator==(const Key& o) const { return vs == o.vs && bytes == o.bytes; }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const;
  };

  std::unique_ptr<InputLayout> Build(const ShaderStore::Entry& vs, const uint8_t* decl,
                                     uint32_t decl_count, const uint8_t* stream_strides);
  // Get without the repeat check: the key, the map, or Build.
  const InputLayout* Lookup(const ShaderStore::Entry& vs, const uint8_t* decl,
                            uint32_t decl_count, const uint8_t* stream_strides);

  const ShaderStore& shaders_;
  std::unordered_map<Key, std::unique_ptr<InputLayout>, KeyHash> layouts_;
  std::vector<ShaderStore::VertexElement> scratch_;
  Key scratch_key_;
  uint32_t next_id_ = 1;
  uint32_t reported_ = 0;
  // A repeat skips the key build and map lookup. Layouts live for the session.
  TwoWayDrawMemo<InputLayoutMemoKey, const InputLayout*> memo_;
  MemoStats memo_stats_;
};

}  // namespace mcla::gpu
