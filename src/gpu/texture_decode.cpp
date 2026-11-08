// MCLA GPU renderer -- guest texture levels decoded on the CPU. See
// texture_decode.h.
//
// The DXT decompression is Xenia's arithmetic (src/xenia/gpu/shaders/
// pixel_formats.xesli: XeDXTColorEndpointsToBGR8In10, XeDXTHighColorWeights,
// XeDXTOpaqueRowToRGB8, XeDXT1TransRowToRGBA8, XeDXT58StepRowToA8,
// XeDXT56StepRowToA8, the 4-bit DXT3 alpha replication), written per texel.
// Copyright 2022 Ben Vanik and the Xenia authors, BSD-3-Clause.

#include "texture_decode.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <vector>

// SSSE3 (pshufb) for the byte swaps. The renderer's objects are built with -msse4.1 on x86-64
// (rexglue_apply_target_settings in src/gpu/CMakeLists.txt), which includes SSSE3, and the SDK
// the game runs on needs the same: no run-time dispatch. Without it (ARM, an x86 build without the
// flag) the same loops run on 64-bit integer operations.
#if defined(__SSSE3__) || (defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64))
#include <tmmintrin.h>
#define MCLA_TEXTURE_DECODE_SSSE3 1
#else
#define MCLA_TEXTURE_DECODE_SSSE3 0
#endif

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

namespace mcla::gpu::texture_decode {

namespace xe = rex::graphics::xenos;
namespace tu = rex::graphics::texture_util;

namespace {

uint32_t Log2(uint32_t bytes_per_block) {
  uint32_t log2 = 0;
  while ((1u << (log2 + 1)) <= bytes_per_block) {
    ++log2;
  }
  return log2;
}

uint32_t SwapUnit(uint32_t endian) {
  switch (xe::Endian(endian)) {
    case xe::Endian::k8in16: return 2;
    case xe::Endian::k8in32:
    case xe::Endian::k16in32: return 4;
    default: return 1;
  }
}

// Undoes the fetch's endian swap over `length` bytes (a multiple of the unit).
void Swap(uint8_t* data, uint32_t length, uint32_t endian) {
  switch (xe::Endian(endian)) {
    case xe::Endian::k8in16:
      for (uint32_t i = 0; i + 1 < length; i += 2) {
        std::swap(data[i], data[i + 1]);
      }
      break;
    case xe::Endian::k8in32:
      for (uint32_t i = 0; i + 3 < length; i += 4) {
        std::swap(data[i], data[i + 3]);
        std::swap(data[i + 1], data[i + 2]);
      }
      break;
    case xe::Endian::k16in32:
      for (uint32_t i = 0; i + 3 < length; i += 4) {
        std::swap(data[i], data[i + 2]);
        std::swap(data[i + 1], data[i + 3]);
      }
      break;
    default:
      break;
  }
}

uint16_t LoadLe16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t LoadLe32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// Reads the block at `offset` into `out` (bytes_per_block bytes) with the
// endian swap undone. A block smaller than the swap unit is cut out of its
// swapped unit, as the GPU sees memory in aligned units. False (zeros) when
// any byte needed lies beyond `readable`.
bool ReadBlock(const Source& s, uint32_t offset, uint8_t* out) {
  const uint32_t bpb = s.bytes_per_block;
  const uint32_t unit = SwapUnit(s.endian);
  if (bpb >= unit) {
    if (uint64_t(offset) + bpb > s.readable) {
      std::memset(out, 0, bpb);
      return false;
    }
    std::memcpy(out, s.data + offset, bpb);
    Swap(out, bpb, s.endian);
    return true;
  }
  const uint32_t aligned = offset & ~(unit - 1);
  if (uint64_t(aligned) + unit > s.readable) {
    std::memset(out, 0, bpb);
    return false;
  }
  uint8_t word[4];
  std::memcpy(word, s.data + aligned, unit);
  Swap(word, unit, s.endian);
  std::memcpy(out, word + (offset - aligned), bpb);
  return true;
}

// --- DXT (Xenia's arithmetic, see the top of the file) ----------------------

// One endpoint of a 5:6:5 color as 8-bit components with bit replication.
struct Rgb8 {
  uint32_t r, g, b;
};
Rgb8 Endpoint(uint16_t c) {
  const uint32_t r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
  return Rgb8{(r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2)};
}

// 16 RGBA8 texels (row-major, R in the low byte) of a DXT color block (8
// bytes, PC layout after the endian swap). `punchthrough`: the DXT1 rule that
// color0 <= color1 selects three colors and transparent black; DXT3/DXT5 color
// blocks are always four-color.
void DecodeColorBlock(const uint8_t* block, bool punchthrough, uint32_t texels[16]) {
  const uint16_t c0 = LoadLe16(block);
  const uint16_t c1 = LoadLe16(block + 2);
  const uint32_t codes = LoadLe32(block + 4);
  const Rgb8 e0 = Endpoint(c0), e1 = Endpoint(c1);
  const bool three_color = punchthrough && c0 <= c1;
  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t code = (codes >> (2 * i)) & 3;
    uint32_t r, g, b, a = 0xFF;
    if (!three_color) {
      // Weights of (color0, color1): 00 = 3:0, 01 = 0:3, 10 = 2:1, 11 = 1:2,
      // integer division by 3 (XeDXTOpaqueRowToRGB8).
      static constexpr uint32_t kW0[4] = {3, 0, 2, 1};
      const uint32_t w0 = kW0[code], w1 = 3 - w0;
      r = (w0 * e0.r + w1 * e1.r) / 3;
      g = (w0 * e0.g + w1 * e1.g) / 3;
      b = (w0 * e0.b + w1 * e1.b) / 3;
    } else {
      // 00 = color0, 01 = color1, 10 = (color0 + color1) >> 1, 11 = transparent
      // black (XeDXT1TransRowToRGBA8).
      switch (code) {
        case 0: r = e0.r; g = e0.g; b = e0.b; break;
        case 1: r = e1.r; g = e1.g; b = e1.b; break;
        case 2:
          r = (e0.r + e1.r) >> 1;
          g = (e0.g + e1.g) >> 1;
          b = (e0.b + e1.b) >> 1;
          break;
        default: r = g = b = 0; a = 0; break;
      }
    }
    texels[i] = r | g << 8 | b << 16 | a << 24;
  }
}

// DXT3 explicit alpha: 4 bits per texel, replicated to 8 (a4 * 17).
void ApplyExplicitAlpha(const uint8_t* block, uint32_t texels[16]) {
  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t a4 = (block[i >> 1] >> ((i & 1) * 4)) & 0xF;
    texels[i] = (texels[i] & 0x00FFFFFFu) | ((a4 * 17u) << 24);
  }
}

// DXT5 interpolated alpha: alpha0 > alpha1 interpolates 8 steps by integer
// division by 7; otherwise 6 steps by 5, with 110 = 0 and 111 = 255
// (XeDXT58StepRowToA8, XeDXT56StepRowToA8).
void ApplyInterpolatedAlpha(const uint8_t* block, uint32_t texels[16]) {
  const uint32_t a0 = block[0], a1 = block[1];
  uint64_t codes = 0;
  for (uint32_t i = 0; i < 6; ++i) {
    codes |= uint64_t(block[2 + i]) << (8 * i);
  }
  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t code = uint32_t(codes >> (3 * i)) & 7;
    uint32_t a;
    if (code == 0) {
      a = a0;
    } else if (code == 1) {
      a = a1;
    } else if (a0 > a1) {
      const uint32_t w1 = code - 1, w0 = 7 - w1;
      a = (w0 * a0 + w1 * a1) / 7;
    } else if (code < 6) {
      const uint32_t w1 = code - 1, w0 = 5 - w1;
      a = (w0 * a0 + w1 * a1) / 5;
    } else {
      a = code == 6 ? 0 : 255;
    }
    texels[i] = (texels[i] & 0x00FFFFFFu) | (a << 24);
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// The fast decoder
// ---------------------------------------------------------------------------
// Decode() below gives the bytes DecodeReference() gives (the per-block decoder this file started
// with, kept as the reference and as the fallback). What it does differently:
//
// - The Xenos 2D tiling address of a block is the origin of its 32x32-block macro tile plus an
//   offset inside the tile that depends on neither the pitch nor the tile:
//   inner(x, y) = X[x & 31] ^ Y[y & 31] (tables per block size, built once from the SDK's own
//   GetTiledOffset2D and checked against it there), and the origin of tile (tx, ty) is
//   GetTiledOffset2D(32 tx, 32 ty). Both are verified for every block size and several pitches by
//   the self-test; the origin is one SDK call per tile instead of one per block.
// - In a tile, blocks that are neighbours in a row are neighbours in memory for a run of 8 blocks
//   (1 and 2 byte blocks), 4 (4 byte), 2 (8 byte) or 1 (16 byte): 8 or 16 bytes. A run is one
//   load, one byte swap and one store (SSSE3 pshufb, or two 64-bit swaps without it). Runs are at
//   most 16 bytes, so wider vectors would need gathers and buy nothing here.
// - The fetch's endian swap is a permutation inside aligned 2 or 4 byte units, so it commutes
//   with taking a block out of memory: swapping the bytes of every unit of the source and then
//   reading blocks is what ReadBlock does, also for blocks smaller than the unit.
// - Everything the reference reads is checked once for the whole level (the highest tile origin
//   plus the largest inner offset against `readable`). A level that does not pass, which is the
//   rare one at the end of committed memory, and any case outside the tables (a block size that is
//   not 1, 2, 4, 8 or 16; a load that does not match the block size; a linear pitch that is not a
//   multiple of the block) is decoded by the reference, so incomplete levels, zero fill and the
//   return value stay what they were.
// - The loads that change the texels (1:5:5:5, depth, DXT decompression) run over the rows the
//   copy made, in place or from a temporary buffer.
//
// Threading: pure functions; the tables are built once (a function-local static); the temporary
// row buffer is per thread.

namespace {

// The self-test sets it to false to run the scalar loops as well.
std::atomic<bool> g_simd{true};
// Decodes the fast path made, and the ones it handed to DecodeReference (the self-test reports
// both: a pass that was all fallbacks would prove nothing).
std::atomic<uint64_t> g_fast_decodes{0};
std::atomic<uint64_t> g_fallback_decodes{0};

enum SwapMode : uint32_t { kSwapNone = 0, kSwap8in16 = 1, kSwap8in32 = 2, kSwap16in32 = 3 };

// xenos::Endian: 0 none, 1 8in16, 2 8in32, 3 16in32; anything else is no swap (SwapUnit).
constexpr uint32_t ModeOf(uint32_t endian) { return endian <= 3 ? endian : 0u; }

// Blocks that are neighbours in memory inside a tile row, by log2 of the block size.
constexpr uint32_t RunBlocks(uint32_t bpb_log2) { return bpb_log2 <= 1 ? 8u : (16u >> bpb_log2); }

constexpr uint64_t AlignUp64(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

struct TileTables {
  uint32_t x[5][32];      // GetTiledOffset2D(x, 0, 32, b)
  uint32_t y[5][32];      // GetTiledOffset2D(0, y, 32, b)
  uint32_t max_inner[5];  // the largest x ^ y: how far from its origin a tile's block can lie
  bool ok;                // the tables reproduce the SDK's function; false: use the reference
};

TileTables BuildTileTables() {
  TileTables t{};
  t.ok = true;
  for (uint32_t b = 0; b < 5; ++b) {
    const uint32_t bpb = 1u << b;
    const uint32_t run = RunBlocks(b);
    for (uint32_t i = 0; i < 32; ++i) {
      t.x[b][i] = uint32_t(tu::GetTiledOffset2D(int32_t(i), 0, 32, b));
      t.y[b][i] = uint32_t(tu::GetTiledOffset2D(0, int32_t(i), 32, b));
    }
    uint32_t max_inner = 0;
    for (uint32_t yl = 0; yl < 32; ++yl) {
      for (uint32_t xl = 0; xl < 32; ++xl) {
        const uint32_t inner = t.x[b][xl] ^ t.y[b][yl];
        if (inner != uint32_t(tu::GetTiledOffset2D(int32_t(xl), int32_t(yl), 32, b))) {
          t.ok = false;
        }
        // Inside a run each block follows the one before it.
        if ((xl & (run - 1)) != 0 && inner != (t.x[b][xl - 1] ^ t.y[b][yl]) + bpb) {
          t.ok = false;
        }
        max_inner = std::max(max_inner, inner);
      }
    }
    t.max_inner[b] = max_inner;
    // Origin + inner holds at another pitch too, in the neighbouring tiles.
    for (uint32_t y = 0; y < 64; ++y) {
      for (uint32_t x = 0; x < 64; ++x) {
        const uint32_t origin =
            uint32_t(tu::GetTiledOffset2D(int32_t(x & ~31u), int32_t(y & ~31u), 96, b));
        if (uint32_t(tu::GetTiledOffset2D(int32_t(x), int32_t(y), 96, b)) !=
            origin + (t.x[b][x & 31] ^ t.y[b][y & 31])) {
          t.ok = false;
        }
      }
    }
  }
  return t;
}

const TileTables& Tables() {
  static const TileTables tables = BuildTileTables();
  return tables;
}

// --- byte swaps ------------------------------------------------------------

template <uint32_t kMode>
inline uint32_t Swap32(uint32_t v) {
  if constexpr (kMode == kSwap8in16) {
    return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
  } else if constexpr (kMode == kSwap8in32) {
    return __builtin_bswap32(v);
  } else if constexpr (kMode == kSwap16in32) {
    return (v << 16) | (v >> 16);
  } else {
    return v;
  }
}

template <uint32_t kMode>
inline uint64_t Swap64(uint64_t v) {
  if constexpr (kMode == kSwap8in16) {
    return ((v & 0x00FF00FF00FF00FFull) << 8) | ((v >> 8) & 0x00FF00FF00FF00FFull);
  } else if constexpr (kMode == kSwap8in32) {
    const uint64_t r = __builtin_bswap64(v);  // all eight bytes reversed, then the halves back
    return (r << 32) | (r >> 32);
  } else if constexpr (kMode == kSwap16in32) {
    return ((v & 0x0000FFFF0000FFFFull) << 16) | ((v >> 16) & 0x0000FFFF0000FFFFull);
  } else {
    return v;
  }
}

#if MCLA_TEXTURE_DECODE_SSSE3
// pshufb control: result byte i is source byte mask[i].
template <uint32_t kMode>
inline __m128i SwapMask() {
  if constexpr (kMode == kSwap8in16) {
    return _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
  } else if constexpr (kMode == kSwap8in32) {
    return _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
  } else {
    return _mm_setr_epi8(2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9, 14, 15, 12, 13);
  }
}
#endif

// One run of 8 or 16 bytes at a source offset that is a multiple of the run's size (so whole
// swap units).
template <uint32_t kRunBytes, uint32_t kMode, bool kSimd>
inline void CopyRun(uint8_t* dst, const uint8_t* src) {
#if MCLA_TEXTURE_DECODE_SSSE3
  if constexpr (kSimd) {
    __m128i v;
    if constexpr (kRunBytes == 16) {
      v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
    } else {
      v = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(src));
    }
    if constexpr (kMode != kSwapNone) {
      v = _mm_shuffle_epi8(v, SwapMask<kMode>());
    }
    if constexpr (kRunBytes == 16) {
      _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), v);
    } else {
      _mm_storel_epi64(reinterpret_cast<__m128i*>(dst), v);
    }
    return;
  }
#endif
  uint64_t a;
  std::memcpy(&a, src, 8);
  a = Swap64<kMode>(a);
  std::memcpy(dst, &a, 8);
  if constexpr (kRunBytes == 16) {
    uint64_t b;
    std::memcpy(&b, src + 8, 8);
    b = Swap64<kMode>(b);
    std::memcpy(dst + 8, &b, 8);
  }
}

// `bytes` bytes at `offset` from the origin `data`, swapped by unit as memory is: the byte at
// offset o comes from (o & ~(unit - 1)) + ((o & (unit - 1)) ^ flip). Slow and exact; for the edges
// of a level (the blocks of a partial run) and for the odd cases.
template <uint32_t kMode>
inline void CopyBytes(uint8_t* dst, const uint8_t* data, uint32_t offset, uint32_t bytes) {
  constexpr uint32_t kUnitMask = kMode == kSwapNone ? 0u : (kMode == kSwap8in16 ? 1u : 3u);
  constexpr uint32_t kFlip =
      kMode == kSwapNone ? 0u : (kMode == kSwap8in16 ? 1u : (kMode == kSwap8in32 ? 3u : 2u));
  for (uint32_t i = 0; i < bytes; ++i) {
    const uint32_t o = offset + i;
    dst[i] = data[(o & ~kUnitMask) + ((o & kUnitMask) ^ kFlip)];
  }
}

// --- tiled -----------------------------------------------------------------

struct UntileArgs {
  const uint8_t* data;
  uint32_t pitch_blocks;  // row pitch in blocks (GetTiledOffset2D aligns it to 32)
  uint32_t offset_x;      // blocks: where the level starts in its storage
  uint32_t offset_y;
  uint32_t width;         // blocks of the level
  uint32_t height;
  uint8_t* out;
  uint32_t out_pitch;
};

// Tile by tile (a tile's 1-16 KB are read in one go, the rows written 32 at a time), row by row
// in the tile, run by run in the row. Partial runs (the first or last blocks of a row when the
// level does not start or end on a run) go block by block.
template <uint32_t kB, uint32_t kMode, bool kSimd>
void UntileKernel(const UntileArgs& a) {
  constexpr uint32_t kBpb = 1u << kB;
  constexpr uint32_t kRun = RunBlocks(kB);
  constexpr uint32_t kRunBytes = kRun * kBpb;
  const TileTables& t = Tables();
  const uint32_t* xin = t.x[kB];
  const uint32_t* yin = t.y[kB];
  const uint32_t x_end = a.offset_x + a.width;
  const uint32_t y_end = a.offset_y + a.height;
  const uint32_t ty_last = (y_end - 1) >> 5;
  const uint32_t tx_last = (x_end - 1) >> 5;
  for (uint32_t ty = a.offset_y >> 5; ty <= ty_last; ++ty) {
    const uint32_t y_lo = std::max(a.offset_y, ty << 5);
    const uint32_t y_hi = std::min(y_end, (ty + 1) << 5);
    for (uint32_t tx = a.offset_x >> 5; tx <= tx_last; ++tx) {
      const uint32_t x_lo = std::max(a.offset_x, tx << 5);
      const uint32_t x_hi = std::min(x_end, (tx + 1) << 5);
      const uint8_t* tile =
          a.data + uint32_t(tu::GetTiledOffset2D(int32_t(tx << 5), int32_t(ty << 5), a.pitch_blocks,
                                                 kB));
      for (uint32_t y = y_lo; y < y_hi; ++y) {
        const uint32_t row_inner = yin[y & 31];
        uint8_t* dst = a.out + uint64_t(y - a.offset_y) * a.out_pitch +
                       uint64_t(x_lo - a.offset_x) * kBpb;
        uint32_t x = x_lo;
        if constexpr (kRun > 1) {
          for (; x < x_hi && (x & (kRun - 1)) != 0; ++x, dst += kBpb) {
            CopyBytes<kMode>(dst, tile, xin[x & 31] ^ row_inner, kBpb);
          }
        }
        for (; x + kRun <= x_hi; x += kRun, dst += kRunBytes) {
          CopyRun<kRunBytes, kMode, kSimd>(dst, tile + (xin[x & 31] ^ row_inner));
        }
        for (; x < x_hi; ++x, dst += kBpb) {
          CopyBytes<kMode>(dst, tile, xin[x & 31] ^ row_inner, kBpb);
        }
      }
    }
  }
}

using UntileFn = void (*)(const UntileArgs&);

template <uint32_t kB, uint32_t kMode>
UntileFn PickUntileSimd(bool simd) {
  return simd ? &UntileKernel<kB, kMode, true> : &UntileKernel<kB, kMode, false>;
}

template <uint32_t kB>
UntileFn PickUntileMode(uint32_t mode, bool simd) {
  switch (mode) {
    case kSwap8in16: return PickUntileSimd<kB, kSwap8in16>(simd);
    case kSwap8in32: return PickUntileSimd<kB, kSwap8in32>(simd);
    case kSwap16in32: return PickUntileSimd<kB, kSwap16in32>(simd);
    default: return PickUntileSimd<kB, kSwapNone>(simd);
  }
}

UntileFn PickUntile(uint32_t bpb_log2, uint32_t mode, bool simd) {
  switch (bpb_log2) {
    case 0: return PickUntileMode<0>(mode, simd);
    case 1: return PickUntileMode<1>(mode, simd);
    case 2: return PickUntileMode<2>(mode, simd);
    case 3: return PickUntileMode<3>(mode, simd);
    case 4: return PickUntileMode<4>(mode, simd);
    default: return nullptr;
  }
}

// --- linear ----------------------------------------------------------------

// `bytes` at `offset` from the origin, the swap done by unit. The vector and word forms swap
// whole units, so they need the offset on a unit (4 bytes cover every mode).
template <uint32_t kMode, bool kSimd>
inline void SwapCopyRow(uint8_t* dst, const uint8_t* data, uint32_t offset, uint32_t bytes) {
  const uint8_t* src = data + offset;
  if constexpr (kMode == kSwapNone) {
    std::memcpy(dst, src, bytes);
  } else {
    uint32_t i = 0;
    if ((offset & 3u) == 0) {
#if MCLA_TEXTURE_DECODE_SSSE3
      if constexpr (kSimd) {
        const __m128i mask = SwapMask<kMode>();
        for (; i + 16 <= bytes; i += 16) {
          const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
          _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), _mm_shuffle_epi8(v, mask));
        }
      }
#endif
      for (; i + 8 <= bytes; i += 8) {
        uint64_t v;
        std::memcpy(&v, src + i, 8);
        v = Swap64<kMode>(v);
        std::memcpy(dst + i, &v, 8);
      }
      for (; i + 4 <= bytes; i += 4) {
        uint32_t v;
        std::memcpy(&v, src + i, 4);
        v = Swap32<kMode>(v);
        std::memcpy(dst + i, &v, 4);
      }
    }
    if (i < bytes) {
      CopyBytes<kMode>(dst + i, data, offset + i, bytes - i);
    }
  }
}

struct LinearArgs {
  const uint8_t* data;
  uint32_t row_pitch_bytes;
  uint32_t bytes_per_block;
  uint32_t offset_x;
  uint32_t offset_y;
  uint32_t width;  // blocks
  uint32_t height;
  uint8_t* out;
  uint32_t out_pitch;
};

template <uint32_t kMode, bool kSimd>
void LinearKernel(const LinearArgs& a) {
  const uint32_t row_bytes = a.width * a.bytes_per_block;
  for (uint32_t y = 0; y < a.height; ++y) {
    const uint32_t offset = (a.offset_y + y) * a.row_pitch_bytes + a.offset_x * a.bytes_per_block;
    SwapCopyRow<kMode, kSimd>(a.out + uint64_t(y) * a.out_pitch, a.data, offset, row_bytes);
  }
}

using LinearFn = void (*)(const LinearArgs&);

template <uint32_t kMode>
LinearFn PickLinearSimd(bool simd) {
  return simd ? &LinearKernel<kMode, true> : &LinearKernel<kMode, false>;
}

LinearFn PickLinear(uint32_t mode, bool simd) {
  switch (mode) {
    case kSwap8in16: return PickLinearSimd<kSwap8in16>(simd);
    case kSwap8in32: return PickLinearSimd<kSwap8in32>(simd);
    case kSwap16in32: return PickLinearSimd<kSwap16in32>(simd);
    default: return PickLinearSimd<kSwapNone>(simd);
  }
}

// Writes the blocks [0, width_blocks) x [0, height_blocks) of the level into `out` (rows
// out_pitch apart, bytes_per_block each) with the endian swap undone: what ReadBlock gives each of
// them. False when the reference has to do it (see the top of this section); nothing is written
// then.
bool CopyBlocks(const Source& s, uint32_t width_blocks, uint32_t height_blocks, uint8_t* out,
                uint32_t out_pitch) {
  const uint32_t bpb = s.bytes_per_block;
  if (bpb == 0 || bpb > 16 || (bpb & (bpb - 1)) != 0 || s.data == nullptr || width_blocks == 0 ||
      height_blocks == 0) {
    return false;
  }
  const uint32_t log2 = Log2(bpb);
  const uint32_t mode = ModeOf(s.endian);
  const uint32_t unit = SwapUnit(s.endian);
  const bool simd = g_simd.load(std::memory_order_relaxed);
  if (s.tiled) {
    const TileTables& t = Tables();
    if (!t.ok) {
      return false;
    }
    const uint32_t pitch_blocks = s.row_pitch_bytes / bpb;
    // The origin of tile (tx, ty) grows with tx + ty * the pitch in tiles, so the last tile's
    // origin plus the largest inner offset (and a whole swap unit) bounds every byte the level
    // reads.
    const uint32_t last_tx = (s.offset_x + width_blocks - 1) >> 5;
    const uint32_t last_ty = (s.offset_y + height_blocks - 1) >> 5;
    const uint64_t last_origin = uint32_t(
        tu::GetTiledOffset2D(int32_t(last_tx << 5), int32_t(last_ty << 5), pitch_blocks, log2));
    if (last_origin + t.max_inner[log2] + std::max(bpb, unit) > s.readable) {
      return false;
    }
    const UntileFn fn = PickUntile(log2, mode, simd);
    if (!fn) {
      return false;
    }
    fn(UntileArgs{s.data, pitch_blocks, s.offset_x, s.offset_y, width_blocks, height_blocks, out,
                  out_pitch});
    return true;
  }
  if (s.row_pitch_bytes % bpb != 0) {
    return false;
  }
  const uint64_t end = uint64_t(s.offset_y + height_blocks - 1) * s.row_pitch_bytes +
                       uint64_t(s.offset_x + width_blocks) * bpb;
  if (AlignUp64(end, unit) > s.readable) {
    return false;
  }
  PickLinear(mode, simd)(LinearArgs{s.data, s.row_pitch_bytes, bpb, s.offset_x, s.offset_y,
                                    width_blocks, height_blocks, out, out_pitch});
  return true;
}

// The load of a request that is not a copy, over the rows CopyBlocks wrote.
void ConvertLoad(const Request& r, uint8_t* out, uint32_t out_pitch) {
  for (uint32_t y = 0; y < r.height_blocks; ++y) {
    uint8_t* row = out + uint64_t(y) * out_pitch;
    switch (r.load) {
      case format::TextureLoad::kCopy:
        return;
      case format::TextureLoad::kR5G5B5A1ToB5G5R5A1:
        for (uint32_t x = 0; x < r.width_blocks; ++x) {
          const uint16_t v = LoadLe16(row + x * 2);
          const uint16_t swapped =
              uint16_t((v & 0x83E0u) | ((v & 0x1Fu) << 10) | ((v >> 10) & 0x1Fu));
          std::memcpy(row + x * 2, &swapped, 2);
        }
        break;
      case format::TextureLoad::kDepthUnorm24:
        for (uint32_t x = 0; x < r.width_blocks; ++x) {
          const float f = xe::UNorm24To32(LoadLe32(row + x * 4) >> 8);
          std::memcpy(row + x * 4, &f, 4);
        }
        break;
      case format::TextureLoad::kDepthFloat20e4:
        for (uint32_t x = 0; x < r.width_blocks; ++x) {
          const float f = xe::Float20e4To32(LoadLe32(row + x * 4) >> 8);
          std::memcpy(row + x * 4, &f, 4);
        }
        break;
    }
  }
}

}  // namespace

uint32_t BlockOffset(const Source& s, uint32_t x, uint32_t y) {
  if (s.tiled) {
    const uint32_t pitch_blocks = s.row_pitch_bytes / s.bytes_per_block;
    return uint32_t(tu::GetTiledOffset2D(int32_t(x), int32_t(y), pitch_blocks,
                                         Log2(s.bytes_per_block)));
  }
  return y * s.row_pitch_bytes + x * s.bytes_per_block;
}

uint64_t Reach(const Source& s, uint32_t width_blocks, uint32_t height_blocks) {
  if (width_blocks == 0 || height_blocks == 0 || s.bytes_per_block == 0) {
    return 0;
  }
  const uint32_t unit = std::max(SwapUnit(s.endian), s.bytes_per_block);
  uint64_t reach = 0;
  if (!s.tiled) {
    const uint32_t last = BlockOffset(s, s.offset_x + width_blocks - 1, s.offset_y + height_blocks - 1);
    return ((uint64_t(last) & ~uint64_t(unit - 1)) + unit);
  }
  for (uint32_t y = 0; y < height_blocks; ++y) {
    for (uint32_t x = 0; x < width_blocks; ++x) {
      const uint64_t end =
          (uint64_t(BlockOffset(s, s.offset_x + x, s.offset_y + y)) & ~uint64_t(unit - 1)) + unit;
      reach = std::max(reach, end);
    }
  }
  return reach;
}

uint32_t HostRowBytes(const Request& r) {
  if (r.decompress != format::TextureDecompress::kNone) {
    return r.width * 4;
  }
  return r.width_blocks * r.source.bytes_per_block;
}

uint32_t HostRows(const Request& r) {
  return r.decompress != format::TextureDecompress::kNone ? r.height : r.height_blocks;
}

bool DecodeReference(const Request& r, uint8_t* out, uint32_t out_pitch) {
  const Source& s = r.source;
  const uint32_t bpb = s.bytes_per_block;
  bool complete = true;
  uint8_t block[16];
  if (r.decompress == format::TextureDecompress::kNone) {
    for (uint32_t y = 0; y < r.height_blocks; ++y) {
      uint8_t* row = out + uint64_t(y) * out_pitch;
      for (uint32_t x = 0; x < r.width_blocks; ++x) {
        const uint32_t offset = BlockOffset(s, s.offset_x + x, s.offset_y + y);
        complete &= ReadBlock(s, offset, block);
        uint8_t* dst = row + x * bpb;
        switch (r.load) {
          case format::TextureLoad::kCopy:
            std::memcpy(dst, block, bpb);
            break;
          case format::TextureLoad::kR5G5B5A1ToB5G5R5A1: {
            const uint16_t v = LoadLe16(block);
            const uint16_t swapped =
                uint16_t((v & 0x83E0u) | ((v & 0x1Fu) << 10) | ((v >> 10) & 0x1Fu));
            std::memcpy(dst, &swapped, 2);
            break;
          }
          case format::TextureLoad::kDepthUnorm24: {
            const float f = xe::UNorm24To32(LoadLe32(block) >> 8);
            std::memcpy(dst, &f, 4);
            break;
          }
          case format::TextureLoad::kDepthFloat20e4: {
            const float f = xe::Float20e4To32(LoadLe32(block) >> 8);
            std::memcpy(dst, &f, 4);
            break;
          }
        }
      }
    }
    return complete;
  }
  // Decompression: 4x4 texels per block, cut at the level's edge.
  uint32_t texels[16];
  for (uint32_t by = 0; by < r.height_blocks; ++by) {
    for (uint32_t bx = 0; bx < r.width_blocks; ++bx) {
      const uint32_t offset = BlockOffset(s, s.offset_x + bx, s.offset_y + by);
      complete &= ReadBlock(s, offset, block);
      switch (r.decompress) {
        case format::TextureDecompress::kDXT1:
          DecodeColorBlock(block, /*punchthrough=*/true, texels);
          break;
        case format::TextureDecompress::kDXT2_3:
          DecodeColorBlock(block + 8, false, texels);
          ApplyExplicitAlpha(block, texels);
          break;
        case format::TextureDecompress::kDXT4_5:
          DecodeColorBlock(block + 8, false, texels);
          ApplyInterpolatedAlpha(block, texels);
          break;
        case format::TextureDecompress::kNone:
          break;
      }
      for (uint32_t ty = 0; ty < 4; ++ty) {
        const uint32_t y = by * 4 + ty;
        if (y >= r.height) {
          break;
        }
        uint8_t* row = out + uint64_t(y) * out_pitch;
        for (uint32_t tx = 0; tx < 4; ++tx) {
          const uint32_t x = bx * 4 + tx;
          if (x >= r.width) {
            break;
          }
          std::memcpy(row + x * 4, &texels[ty * 4 + tx], 4);
        }
      }
    }
  }
  return complete;
}

namespace {

bool Fallback(const Request& r, uint8_t* out, uint32_t out_pitch) {
  g_fallback_decodes.fetch_add(1, std::memory_order_relaxed);
  return DecodeReference(r, out, out_pitch);
}

}  // namespace

bool Decode(const Request& r, uint8_t* out, uint32_t out_pitch) {
  const Source& s = r.source;
  const uint32_t bpb = s.bytes_per_block;
  if (r.width_blocks == 0 || r.height_blocks == 0 || bpb == 0) {
    return Fallback(r, out, out_pitch);
  }
  if (r.decompress == format::TextureDecompress::kNone) {
    // A load other than the copy reads the first bytes of a block as a 16 or 32 bit value: the
    // block size has to be the value's (the reference does what it does with any other).
    if ((r.load == format::TextureLoad::kR5G5B5A1ToB5G5R5A1 && bpb != 2) ||
        ((r.load == format::TextureLoad::kDepthUnorm24 ||
          r.load == format::TextureLoad::kDepthFloat20e4) &&
         bpb != 4)) {
      return Fallback(r, out, out_pitch);
    }
    if (!CopyBlocks(s, r.width_blocks, r.height_blocks, out, out_pitch)) {
      return Fallback(r, out, out_pitch);
    }
    ConvertLoad(r, out, out_pitch);
    g_fast_decodes.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  // Decompression: the guest blocks go to a row buffer first (untiled, swapped), then each is
  // expanded as the reference does.
  const bool dxt1 = r.decompress == format::TextureDecompress::kDXT1;
  if ((dxt1 && bpb != 8) || (!dxt1 && bpb != 16)) {
    return Fallback(r, out, out_pitch);
  }
  thread_local std::vector<uint8_t> raw;
  const uint32_t raw_pitch = r.width_blocks * bpb;
  raw.resize(size_t(raw_pitch) * r.height_blocks);
  if (!CopyBlocks(s, r.width_blocks, r.height_blocks, raw.data(), raw_pitch)) {
    return Fallback(r, out, out_pitch);
  }
  uint32_t texels[16];
  for (uint32_t by = 0; by < r.height_blocks; ++by) {
    for (uint32_t bx = 0; bx < r.width_blocks; ++bx) {
      const uint8_t* block = raw.data() + size_t(by) * raw_pitch + size_t(bx) * bpb;
      switch (r.decompress) {
        case format::TextureDecompress::kDXT1:
          DecodeColorBlock(block, /*punchthrough=*/true, texels);
          break;
        case format::TextureDecompress::kDXT2_3:
          DecodeColorBlock(block + 8, false, texels);
          ApplyExplicitAlpha(block, texels);
          break;
        case format::TextureDecompress::kDXT4_5:
          DecodeColorBlock(block + 8, false, texels);
          ApplyInterpolatedAlpha(block, texels);
          break;
        case format::TextureDecompress::kNone:
          break;
      }
      for (uint32_t ty = 0; ty < 4; ++ty) {
        const uint32_t y = by * 4 + ty;
        if (y >= r.height) {
          break;
        }
        uint8_t* row = out + uint64_t(y) * out_pitch;
        for (uint32_t tx = 0; tx < 4; ++tx) {
          const uint32_t x = bx * 4 + tx;
          if (x >= r.width) {
            break;
          }
          std::memcpy(row + x * 4, &texels[ty * 4 + tx], 4);
        }
      }
    }
  }
  g_fast_decodes.fetch_add(1, std::memory_order_relaxed);
  return true;
}

namespace {

// True when the rows (HostRowBytes each, out_pitch apart; the padding is not looked at) differ.
bool RowsDiffer(const Request& r, const uint8_t* a, const uint8_t* b, uint32_t out_pitch,
                uint64_t* first_byte) {
  const uint32_t row_bytes = HostRowBytes(r);
  const uint32_t rows = HostRows(r);
  for (uint32_t y = 0; y < rows; ++y) {
    const uint8_t* ra = a + uint64_t(y) * out_pitch;
    const uint8_t* rb = b + uint64_t(y) * out_pitch;
    if (std::memcmp(ra, rb, row_bytes) != 0) {
      if (first_byte) {
        uint32_t x = 0;
        while (ra[x] == rb[x]) {
          ++x;
        }
        *first_byte = uint64_t(y) * out_pitch + x;
      }
      return true;
    }
  }
  return false;
}

}  // namespace

bool DecodeVerified(const Request& r, uint8_t* out, uint32_t out_pitch, bool* mismatch) {
  const bool complete = Decode(r, out, out_pitch);
  thread_local std::vector<uint8_t> reference;
  reference.assign(size_t(out_pitch) * HostRows(r), 0);
  const bool reference_complete = DecodeReference(r, reference.data(), out_pitch);
  if (mismatch) {
    *mismatch = complete != reference_complete || RowsDiffer(r, out, reference.data(), out_pitch, nullptr);
  }
  return complete;
}

SelfTestResult SelfTest(uint32_t case_count) {
  SelfTestResult result;
  uint64_t rng = 0x243F6A8885A308D3ull;
  const auto next = [&rng]() {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
  };
  const auto pick = [&next](uint32_t n) { return uint32_t(next() % n); };
  static constexpr uint32_t kBlockSizes[] = {1, 2, 4, 8, 16};
  static constexpr uint32_t kExtents[] = {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 47, 64, 65, 100};
  static constexpr uint32_t kOffsets[] = {1, 2, 3, 5, 8, 16, 31};
  std::vector<uint8_t> guest, fast, slow;
  const bool simd_was = g_simd.load();
  const uint64_t fast_before = g_fast_decodes.load();
  const uint64_t fallback_before = g_fallback_decodes.load();
  result.tables_ok = Tables().ok;
  for (uint32_t c = 0; c < case_count; ++c) {
    Request r;
    Source& s = r.source;
    s.bytes_per_block = kBlockSizes[pick(5)];
    s.endian = pick(4);
    s.tiled = pick(3) != 0;
    if (pick(4) == 0) {  // a level inside a packed tail
      s.offset_x = kOffsets[pick(7)];
      s.offset_y = kOffsets[pick(7)];
    }
    r.width_blocks = kExtents[pick(18)];
    r.height_blocks = kExtents[pick(18)];
    r.width = r.width_blocks;
    r.height = r.height_blocks;
    if (s.bytes_per_block == 2 && pick(3) == 0) {
      r.load = format::TextureLoad::kR5G5B5A1ToB5G5R5A1;
    } else if (s.bytes_per_block == 4 && pick(3) == 0) {
      r.load = pick(2) ? format::TextureLoad::kDepthUnorm24 : format::TextureLoad::kDepthFloat20e4;
    } else if ((s.bytes_per_block == 8 || s.bytes_per_block == 16) && pick(2) == 0) {
      r.decompress = s.bytes_per_block == 8
                         ? format::TextureDecompress::kDXT1
                         : (pick(2) ? format::TextureDecompress::kDXT2_3
                                    : format::TextureDecompress::kDXT4_5);
      r.width = r.width_blocks * 4 - pick(4);  // cut at the level's edge
      r.height = r.height_blocks * 4 - pick(4);
    }
    uint32_t pitch_blocks;
    if (s.tiled) {
      pitch_blocks = uint32_t(AlignUp64(s.offset_x + r.width_blocks, 32)) + (pick(4) == 0 ? 32 : 0);
      if (pick(8) == 0) {
        pitch_blocks += 5;  // GetTiledOffset2D aligns it
      }
    } else {
      pitch_blocks = s.offset_x + r.width_blocks + pick(8);
    }
    s.row_pitch_bytes = pitch_blocks * s.bytes_per_block;
    const uint64_t reach = Reach(s, r.width_blocks, r.height_blocks);
    // 0: all of it readable, with room behind; 1: exactly the reach; 2: short of it (zero fill).
    const uint32_t variant = pick(4) == 0 ? 1 + pick(2) : 0;
    const uint64_t size = variant == 0 ? reach + 0x4000 + 64 : reach;
    guest.resize(size_t(size));
    for (uint64_t i = 0; i + 8 <= size; i += 8) {
      const uint64_t v = next();
      std::memcpy(guest.data() + i, &v, 8);
    }
    for (uint64_t i = size & ~7ull; i < size; ++i) {
      guest[size_t(i)] = uint8_t(next());
    }
    s.data = guest.data();
    s.readable = variant == 2 ? (reach > 20 ? reach - 1 - pick(16) : reach / 2) : size;
    const uint32_t row_bytes = HostRowBytes(r);
    const uint32_t rows = HostRows(r);
    const uint32_t out_pitch = pick(2) ? uint32_t(AlignUp64(row_bytes, 256)) : row_bytes;
    const size_t out_size = size_t(out_pitch) * rows;
    slow.assign(out_size, 0xCD);
    const bool slow_complete = DecodeReference(r, slow.data(), out_pitch);
    for (uint32_t pass = 0; pass < 2; ++pass) {
      g_simd.store(pass == 0, std::memory_order_relaxed);
      fast.assign(out_size, 0xCD);
      const bool fast_complete = Decode(r, fast.data(), out_pitch);
      uint64_t at = 0;
      const bool differ = fast_complete != slow_complete || std::memcmp(fast.data(), slow.data(), out_size) != 0;
      if (differ) {
        ++result.mismatches;
        if (result.first.empty()) {
          for (at = 0; at < out_size && fast[size_t(at)] == slow[size_t(at)]; ++at) {
          }
          result.first = std::format(
              "bpb {} endian {} {} offset {},{} size {}x{} blocks pitch {} load {} decompress {} "
              "readable {}/{} ({}) {} out pitch {}: complete {} vs {}, first byte {}",
              s.bytes_per_block, s.endian, s.tiled ? "tiled" : "linear", s.offset_x, s.offset_y,
              r.width_blocks, r.height_blocks, pitch_blocks, uint32_t(r.load),
              uint32_t(r.decompress), s.readable, reach, variant, pass == 0 ? "simd" : "scalar",
              out_pitch, fast_complete, slow_complete, at);
        }
      }
      ++result.runs;
    }
  }
  g_simd.store(simd_was, std::memory_order_relaxed);
  result.cases = case_count;
  result.fast = uint32_t(g_fast_decodes.load() - fast_before);
  result.fallback = uint32_t(g_fallback_decodes.load() - fallback_before);
  return result;
}

}  // namespace mcla::gpu::texture_decode
