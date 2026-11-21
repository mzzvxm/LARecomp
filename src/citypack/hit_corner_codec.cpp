#include "citypack/hit_corner_codec.h"

#include <algorithm>
#include <utility>

#include "citypack/city_zstd.h"

namespace larecomp::citypack {

namespace {

constexpr uint32_t kEmpty = 0xFFFFFFFFu;
constexpr uint32_t kNearMax = 12;          // tokens 1..12 are the distance itself
constexpr uint32_t kByteBase = 13;         // token 13: 13 + the next extra byte (13..268)
constexpr uint32_t kVarintBase = 13 + 256;  // token 14: 269 + the next extra varint

uint32_t ZigZag(int32_t v) { return (static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31); }
int32_t UnZigZag(uint32_t z) { return static_cast<int32_t>(z >> 1) ^ -static_cast<int32_t>(z & 1u); }

}  // namespace

std::vector<uint8_t> EncodeIndexedCorners(const uint32_t* words, size_t n) {
  // The last position of every distinct pair, in an open addressing table (load at most one half).
  size_t capacity = 16;
  uint32_t shift = 60;
  while (capacity < n * 2) {
    capacity <<= 1;
    --shift;
  }
  std::vector<uint64_t> keys(capacity);
  std::vector<uint32_t> last(capacity, kEmpty);
  const size_t mask = capacity - 1;

  std::vector<uint64_t> entries;
  entries.reserve(n / 3 + 16);
  std::vector<uint8_t> nibbles((n + 1) / 2, 0);
  ByteWriter extra;
  for (size_t i = 0; i < n; ++i) {
    const uint64_t key = uint64_t(words[2 * i]) | (uint64_t(words[2 * i + 1]) << 32);
    size_t h = size_t((key * 0x9E3779B97F4A7C15ull) >> shift) & mask;
    while (last[h] != kEmpty && keys[h] != key) h = (h + 1) & mask;
    uint32_t token = 0;
    if (last[h] == kEmpty) {
      keys[h] = key;
      entries.push_back(key);
    } else {
      const uint32_t d = uint32_t(i) - last[h];
      if (d <= kNearMax) {
        token = d;
      } else if (d < kVarintBase) {
        token = 13;
        extra.U8(uint8_t(d - kByteBase));
      } else {
        token = 14;
        extra.Varint(d - kVarintBase);
      }
    }
    last[h] = uint32_t(i);
    nibbles[i >> 1] |= uint8_t(token << ((i & 1u) * 4u));
  }

  // The palette: the entries' colours, most used first.
  std::vector<uint32_t> colours(entries.size());
  for (size_t e = 0; e < entries.size(); ++e) colours[e] = uint32_t(entries[e] >> 32);
  std::vector<uint32_t> sorted = colours;
  std::sort(sorted.begin(), sorted.end());
  std::vector<std::pair<uint32_t, uint32_t>> counted;  // (count, colour)
  for (size_t i = 0; i < sorted.size();) {
    size_t j = i;
    while (j < sorted.size() && sorted[j] == sorted[i]) ++j;
    counted.emplace_back(uint32_t(j - i), sorted[i]);
    i = j;
  }
  std::sort(counted.begin(), counted.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
  std::vector<std::pair<uint32_t, uint32_t>> by_colour;  // (colour, palette index)
  by_colour.reserve(counted.size());
  for (size_t p = 0; p < counted.size(); ++p) by_colour.emplace_back(counted[p].second, uint32_t(p));
  std::sort(by_colour.begin(), by_colour.end());

  ByteWriter uv;
  ByteWriter colour_index;
  int32_t pu = 0, pv = 0;
  for (size_t e = 0; e < entries.size(); ++e) {
    const int32_t u = int32_t(entries[e] & 0xFFFFu);
    const int32_t v = int32_t((entries[e] >> 16) & 0xFFFFu);
    uv.Varint(ZigZag(u - pu));
    uv.Varint(ZigZag(v - pv));
    pu = u;
    pv = v;
    const auto it = std::lower_bound(by_colour.begin(), by_colour.end(), std::make_pair(colours[e], 0u));
    colour_index.Varint(it->second);
  }

  ByteWriter out;
  out.U8(uint8_t(kIndexedCornersVersion));
  out.U8(0);
  out.U16(0);
  out.U32(uint32_t(entries.size()));
  out.U32(uint32_t(counted.size()));
  out.U32(uint32_t(nibbles.size()));
  out.U32(uint32_t(extra.size()));
  out.U32(uint32_t(uv.size()));
  out.U32(uint32_t(colour_index.size()));
  for (const auto& c : counted) out.U32(c.second);
  out.Bytes(nibbles.data(), nibbles.size());
  out.Bytes(extra.bytes().data(), extra.size());
  out.Bytes(uv.bytes().data(), uv.size());
  out.Bytes(colour_index.bytes().data(), colour_index.size());
  return std::move(out.bytes());
}

std::vector<uint8_t> CompressIndexedCornerStream(std::vector<uint8_t> stream, int zstd_level, uint32_t* encoding) {
  *encoding = kIndexedCornersVersion;
  if (zstd_level > 0) {
    std::vector<uint8_t> frame;
    if (ZstdCompress(stream.data(), stream.size(), zstd_level, &frame) && frame.size() < stream.size()) {
      *encoding = kIndexedCornersZstd;
      return frame;
    }
  }
  return stream;
}

std::vector<uint8_t> EncodeIndexedCornersZ(const uint32_t* words, size_t n, int zstd_level, uint32_t* encoding) {
  return CompressIndexedCornerStream(EncodeIndexedCorners(words, n), zstd_level, encoding);
}

bool DecodeIndexedCorners(const uint8_t* data, size_t size, size_t n, std::vector<uint32_t>* out, std::string* error) {
  const auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  out->clear();
  ByteReader r(data, size);
  const uint32_t version = r.U8();
  r.Skip(3);
  const uint32_t entry_count = r.U32();
  const uint32_t palette_count = r.U32();
  const uint32_t nibble_bytes = r.U32();
  const uint32_t extra_bytes = r.U32();
  const uint32_t uv_bytes = r.U32();
  const uint32_t index_bytes = r.U32();
  if (!r.ok()) return fail("an indexed corner stream is cut short");
  if (version != kIndexedCornersVersion) return fail("an indexed corner stream is of another version");
  if (nibble_bytes != (n + 1) / 2 || entry_count > n || palette_count > entry_count) return fail("an indexed corner stream's counts do not fit the blob");
  const uint64_t body = uint64_t(palette_count) * 4u + nibble_bytes + extra_bytes + uv_bytes + index_bytes;
  if (kIndexedCornerHeaderBytes + body != size) return fail("an indexed corner stream's sections do not add up to its size");

  std::vector<uint32_t> palette(palette_count);
  for (uint32_t& c : palette) c = r.U32();
  const uint8_t* nibbles = data + r.pos();
  r.Skip(nibble_bytes);
  ByteReader extra(data + r.pos(), extra_bytes);
  r.Skip(extra_bytes);
  ByteReader uv(data + r.pos(), uv_bytes);
  r.Skip(uv_bytes);
  ByteReader index(data + r.pos(), index_bytes);
  if (!r.ok()) return fail("an indexed corner stream is cut short");

  std::vector<uint64_t> entries(entry_count);
  int64_t u = 0, v = 0;
  for (uint32_t e = 0; e < entry_count; ++e) {
    u += UnZigZag(uv.Varint());
    v += UnZigZag(uv.Varint());
    const uint32_t colour_slot = index.Varint();
    if (!uv.ok() || !index.ok()) return fail("an indexed corner stream's entries are cut short");
    if (u < 0 || u > 0xFFFF || v < 0 || v > 0xFFFF || colour_slot >= palette_count) return fail("an indexed corner stream's entry is out of range");
    entries[e] = uint64_t(uint32_t(u) | (uint32_t(v) << 16)) | (uint64_t(palette[colour_slot]) << 32);
  }
  if (uv.left() != 0 || index.left() != 0) return fail("an indexed corner stream has bytes left over in its entries");

  out->resize(n * 2);
  uint32_t next_entry = 0;
  for (size_t i = 0; i < n; ++i) {
    const uint32_t token = (nibbles[i >> 1] >> ((i & 1u) * 4u)) & 15u;
    if (token == 0) {
      if (next_entry >= entry_count) return fail("an indexed corner stream asks for more new entries than it has");
      const uint64_t e = entries[next_entry++];
      (*out)[2 * i] = uint32_t(e);
      (*out)[2 * i + 1] = uint32_t(e >> 32);
      continue;
    }
    uint32_t d = 0;
    if (token <= kNearMax) {
      d = token;
    } else if (token == 13) {
      d = kByteBase + extra.U8();
    } else if (token == 14) {
      d = kVarintBase + extra.Varint();
    } else {
      return fail("an indexed corner stream has a token that is not one");
    }
    if (!extra.ok() || d > i) return fail("an indexed corner stream refers to a corner before the first");
    (*out)[2 * i] = (*out)[2 * (i - d)];
    (*out)[2 * i + 1] = (*out)[2 * (i - d) + 1];
  }
  if (next_entry != entry_count || extra.left() != 0) return fail("an indexed corner stream has entries or escapes it does not use");
  return true;
}

std::vector<uint8_t> AssembleIndexedHitBlob(const HitBlob& blob, const std::vector<uint8_t>& corner_stream, uint32_t encoding) {
  constexpr uint32_t kStreams = 2;
  const uint32_t body_start = kHitBlobHeaderBytes + kStreams * kStreamEntryBytes;
  ByteWriter body;
  std::vector<StreamEntry> table;
  {  // the runs, as city_hit_format.cpp EncodeHitBlob writes them
    body.Align(4);
    StreamEntry e;
    e.id = kHitStreamRuns;
    e.encoding = 0;
    e.offset = body_start + uint32_t(body.size());
    e.count = uint32_t(blob.runs.size());
    e.element_bytes = kHitRunBytes;
    for (const HitRun& run : blob.runs) {
      uint32_t words[8];
      run.Pack(words);
      for (uint32_t v : words) body.U32(v);
    }
    e.size = body_start + uint32_t(body.size()) - e.offset;
    table.push_back(e);
  }
  {
    body.Align(4);
    StreamEntry e;
    e.id = kHitStreamCornersIndexed;
    e.encoding = encoding;
    e.offset = body_start + uint32_t(body.size());
    e.count = blob.triangle_count * 3u;
    e.element_bytes = 0;
    body.Bytes(corner_stream.data(), corner_stream.size());
    e.size = uint32_t(corner_stream.size());
    table.push_back(e);
  }
  body.Align(4);

  ByteWriter tail;  // stream table + streams: what the hash covers
  for (const StreamEntry& e : table) e.Write(tail);
  tail.Bytes(body.bytes().data(), body.size());

  ByteWriter bytes;
  bytes.U32(kHitBlobMagic);
  bytes.U32((blob.flags & ~kHitBlobHasCorners) | kHitBlobHasIndexedCorners);
  bytes.U32(blob.triangle_count);
  bytes.U32(blob.opaque_triangles);
  bytes.U32(uint32_t(blob.runs.size()));
  bytes.U32(kStreams);
  bytes.U64(Fnv1a64(tail.bytes().data(), tail.size()));
  bytes.Zeros(kHitBlobHeaderBytes - bytes.size());
  bytes.Bytes(tail.bytes().data(), tail.size());
  return std::move(bytes.bytes());
}

bool DecodeHitBlobAny(const uint8_t* data, size_t size, HitBlob* out, std::string* error) {
  HitBlob blob;
  if (!DecodeHitBlob(data, size, &blob, error)) return false;
  if ((blob.flags & kHitBlobHasIndexedCorners) == 0) {
    *out = std::move(blob);
    return true;
  }
  const auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  if (blob.has_corners()) return fail("a hit blob has both kinds of corner stream");
  ByteReader r(data, size);
  r.Skip(20);
  const uint32_t stream_count = r.U32();
  r.Skip(kHitBlobHeaderBytes - r.pos());
  for (uint32_t k = 0; k < stream_count && k < 64; ++k) {
    StreamEntry e;
    if (!StreamEntry::Read(r, &e)) return fail("a hit blob's stream table is cut short");
    if (e.id != kHitStreamCornersIndexed) continue;
    if (e.count != blob.triangle_count * 3u || uint64_t(e.offset) + e.size > size) return fail("a hit blob's indexed corner stream does not fit the blob");
    if (e.encoding == kIndexedCornersVersion) {
      if (!DecodeIndexedCorners(data + e.offset, e.size, e.count, &blob.corners, error)) return false;
    } else if (e.encoding == kIndexedCornersZstd) {
      std::vector<uint8_t> stream;
      if (!ZstdDecompressSelfSized(data + e.offset, e.size, kMaxIndexedCornerStreamBytes, &stream, error)) return false;
      if (!DecodeIndexedCorners(stream.data(), stream.size(), e.count, &blob.corners, error)) return false;
    } else {
      return fail("a hit blob's indexed corner stream has an encoding this build does not know");
    }
    blob.flags = (blob.flags & ~kHitBlobHasIndexedCorners) | kHitBlobHasCorners;
    *out = std::move(blob);
    return true;
  }
  return fail("a hit blob says it has indexed corners and has none");
}

}  // namespace larecomp::citypack
