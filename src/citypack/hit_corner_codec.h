#pragma once
// ===========================================================================
// LARecomp city pack -- the INDEXED corner stream of a hit blob (CITY-RT M2, package H1)
// ===========================================================================
// WHY. city_hit_format.h writes the corners of a blob raw (8 bytes a corner) or delta coded against the corner before (the plan's estimate was 2.5 bytes a corner). Measured on
// the retail game's 22.2 M triangles (67.9 M corners) the delta coding gives 7.2 bytes a corner, 457 MiB: the vertex colour (the baked ambient occlusion) differs from corner to
// corner, and a triangle list's corner has the previous corner's attributes only by chance. The owner's budget is 200 to 300 MB for the hit pack and the textures together. What the
// data does have is REPETITION: a source vertex is used by about 3.3 corners of the triangle list (20.4 M distinct (uv, colour) pairs for 67.9 M corners), and the repeats are
// close: 82 percent of them within 8 corners of the last equal one.
//
// WHAT. The corners are coded as: one 4 bit TOKEN a corner (a new pair, or "the same as the corner d before", d = the distance to the last equal corner), then the distinct pairs in
// first-use order (the texture coordinate as two zigzag varint deltas from the pair before, the colour as an index into a PALETTE of the blob's colours, most frequent first).
// Lossless: the decoder gives back exactly the words that went in (the lossy step, a coarser texture coordinate step and a quantised colour, is the collector's: hit_collect.h
// HitRules, before the words are made). It is read and written by this file alone; city_hit_format.h is not touched:
//
//   * the stream has a NEW STREAM ID (kHitStreamCornersIndexed = 3). city_hit_format.h's DecodeHitBlob skips a stream id it does not know ("a stream of a later version") and a blob
//     whose flags say kHitBlobHasIndexedCorners (bit 1) and not kHitBlobHasCorners (bit 0) passes its consistency check: it decodes as the runs without corners. Everything that
//     needs the corners reads the blob through DecodeHitBlobAny (HitPackReader::ReadHitBlob does), which gives back an ordinary HitBlob (corners as two words each, the flags with
//     kHitBlobHasCorners): the pool filler, the validator and the tests do not know the difference.
//   * H0's own encodings stay readable by DecodeHitBlobAny (a blob with stream id 2).
//
// THE STREAM (stream id 3, element count = 3 a triangle, encoding 1; all integers little endian):
//   u8 version (1) | u8 0 | u16 0 | u32 entries M | u32 palette P | u32 nibble bytes | u32 extra bytes | u32 uv bytes | u32 colour index bytes        (28 bytes)
//   palette           P x u32: the colour words (the corner's RGBA8), most used first (ties by value)
//   nibbles           ceil(N / 2) bytes, the token of corner 2 k in the low nibble of byte k, corner 2 k + 1 in the high one:
//                       0 = a new entry (the next of the entries below);  1..12 = the same as the corner that many before;  13 = the distance is 13 + the next byte of `extra`;
//                       14 = the distance is 269 + the next varint of `extra`;  15 = invalid
//   extra             the escapes' bytes, in the order the tokens come
//   uv                M x (varint zigzag(du), varint zigzag(dv)): the entry's u and v (the 16 bit words of the corner's first word) against the entry before (the first against 0)
//   colour index      M x varint: the entry's colour as an index into the palette
// An entry is the PAIR (uv word, colour word): two corners are "equal" when both words are. The decoder checks every number against the stream's sizes and never reads past them.
//
// COMPRESSED (CITY-HIT-SIZE; city_zstd.h): the stream above is tokens, varints and a palette, and still compresses to 62 percent (the colour indices and the escapes are the part that
// does not). The builders therefore keep the stream as ONE ZSTD FRAME, marked by the stream table's `encoding` = kIndexedCornersZstd (2) instead of kIndexedCornersVersion (1); the frame
// holds the version 1 stream byte for byte and states its size. Which of the two a blob has is the stream entry's business alone: the container, the flags, the blob hash (over the
// compressed bytes) and DecodeHitBlobAny's result are the same, so the format version does not change and a blob of either encoding reads.
// ===========================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "citypack/city_hit_format.h"

namespace larecomp::citypack {

inline constexpr uint32_t kHitStreamCornersIndexed = 3;     // a stream id of a hit blob (city_hit_format.h has 1 and 2)
inline constexpr uint32_t kHitBlobHasIndexedCorners = 2;    // HitBlob::flags bit 1, in the file only: the corner stream is stream 3
inline constexpr uint32_t kIndexedCornersVersion = 1;       // StreamEntry::encoding: the stream as described above
inline constexpr uint32_t kIndexedCornersZstd = 2;          // StreamEntry::encoding: one zstd frame of that stream
inline constexpr uint32_t kIndexedCornerHeaderBytes = 28;
inline constexpr size_t kMaxIndexedCornerStreamBytes = size_t(1) << 30;  // what a compressed stream may state for its size (a blob has at most 2^28 triangles: far less in fact)

// `words`: two uint32 a corner (the layout of city_hit_format.h), `corners` corners. The stream's bytes.
std::vector<uint8_t> EncodeIndexedCorners(const uint32_t* words, size_t corners);
// A zstd frame of `stream` (an EncodeIndexedCorners result) at `zstd_level` when that is smaller, else the stream itself (level 0: always the stream). `*encoding` says what came back:
// kIndexedCornersVersion or kIndexedCornersZstd; pass it to AssembleIndexedHitBlob.
std::vector<uint8_t> CompressIndexedCornerStream(std::vector<uint8_t> stream, int zstd_level, uint32_t* encoding);
// EncodeIndexedCorners and CompressIndexedCornerStream in one.
std::vector<uint8_t> EncodeIndexedCornersZ(const uint32_t* words, size_t corners, int zstd_level, uint32_t* encoding);
// False (with `error`) for a stream that is cut, inconsistent or damaged. `out` gets two words a corner.
bool DecodeIndexedCorners(const uint8_t* data, size_t size, size_t corners, std::vector<uint32_t>* out, std::string* error);

// A hit blob whose corner stream is the indexed one: the container of city_hit_format.h (header, stream table, the runs, then stream 3 = `corner_stream`, an EncodeIndexedCorners
// result for 3 corners a triangle, or an EncodeIndexedCornersZ result with its `encoding`). `blob.corners` is ignored; `blob.flags` gets kHitBlobHasIndexedCorners instead of
// kHitBlobHasCorners.
std::vector<uint8_t> AssembleIndexedHitBlob(const HitBlob& blob, const std::vector<uint8_t>& corner_stream, uint32_t encoding = kIndexedCornersVersion);

// DecodeHitBlob, and the indexed corner stream when the blob has one: the HitBlob has its corners (two words each) and kHitBlobHasCorners.
bool DecodeHitBlobAny(const uint8_t* data, size_t size, HitBlob* out, std::string* error);

}  // namespace larecomp::citypack
