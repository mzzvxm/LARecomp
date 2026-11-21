#include "citypack/city_hit_format.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace larecomp::citypack {

namespace {

uint64_t AlignUp(uint64_t v, uint64_t alignment) { return (v + alignment - 1) & ~(alignment - 1); }

uint32_t ZigZag(int32_t v) { return (static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31); }
int32_t UnZigZag(uint32_t z) { return static_cast<int32_t>(z >> 1) ^ -static_cast<int32_t>(z & 1u); }

void PutU32(uint8_t* at, uint32_t v) {
  at[0] = uint8_t(v);
  at[1] = uint8_t(v >> 8);
  at[2] = uint8_t(v >> 16);
  at[3] = uint8_t(v >> 24);
}
void PutU16(uint8_t* at, uint16_t v) {
  at[0] = uint8_t(v);
  at[1] = uint8_t(v >> 8);
}

}  // namespace

// ---------------------------------------------------------------------------
// R10G10B10
// ---------------------------------------------------------------------------

uint32_t PackRgb10(const float rgb[3]) {
  uint32_t out = 0;
  for (uint32_t c = 0; c < 3; ++c) {
    const float v = std::clamp(std::isfinite(rgb[c]) ? rgb[c] : 0.0f, 0.0f, kHitRgb10Max) / kHitRgb10Max;
    out |= uint32_t(std::lround(v * 1023.0f)) << (10u * c);
  }
  return out;
}

void UnpackRgb10(uint32_t packed, float rgb[3]) {
  for (uint32_t c = 0; c < 3; ++c) rgb[c] = float((packed >> (10u * c)) & 1023u) / 1023.0f * kHitRgb10Max;
}

// ---------------------------------------------------------------------------
// float16
// ---------------------------------------------------------------------------

float HalfToFloat(uint16_t half) {
  const uint32_t sign = uint32_t(half & 0x8000u) << 16;
  const uint32_t exponent = (half >> 10) & 0x1Fu;
  const uint32_t mantissa = half & 0x3FFu;
  if (exponent == 0) {
    if (mantissa == 0) return std::bit_cast<float>(sign);
    // a subnormal: mantissa x 2^-24
    const float v = float(mantissa) * (1.0f / 16777216.0f);
    return (sign != 0) ? -v : v;
  }
  if (exponent == 31) return std::bit_cast<float>(sign | 0x7F800000u | (mantissa << 13));
  return std::bit_cast<float>(sign | ((exponent + 112u) << 23) | (mantissa << 13));
}

uint16_t HalfFromFloat(float value) {
  if (std::isnan(value)) return 0;
  const uint32_t bits = std::bit_cast<uint32_t>(value);
  const uint32_t sign = (bits >> 16) & 0x8000u;
  const uint32_t raw_exponent = (bits >> 23) & 0xFFu;
  if (raw_exponent == 0xFFu) return uint16_t(sign | 0x7BFFu);  // infinity: saturate
  const int32_t exponent = int32_t(raw_exponent) - 127 + 15;
  uint32_t mantissa = bits & 0x7FFFFFu;
  if (exponent >= 31) return uint16_t(sign | 0x7BFFu);
  if (exponent <= 0) {
    if (exponent < -10) return uint16_t(sign);
    mantissa |= 0x800000u;
    const uint32_t shift = uint32_t(14 - exponent);
    uint32_t half = mantissa >> shift;
    const uint32_t remainder = mantissa & ((1u << shift) - 1u);
    const uint32_t halfway = 1u << (shift - 1u);
    if (remainder > halfway || (remainder == halfway && (half & 1u) != 0)) ++half;
    return uint16_t(sign | half);
  }
  uint32_t half = (uint32_t(exponent) << 10) | (mantissa >> 13);
  const uint32_t remainder = mantissa & 0x1FFFu;
  if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1u) != 0)) ++half;
  if (half >= 0x7C00u) half = 0x7BFFu;
  return uint16_t(sign | half);
}

namespace {
// The half that is the next larger value (towards +65504) and the next smaller one, by value, not by bits; the ends stay put.
uint16_t HalfNextUp(uint16_t h) {
  if ((h & 0x7FFFu) == 0) return 0x0001u;  // +-0 -> the smallest positive subnormal
  if ((h & 0x8000u) == 0) return h == 0x7BFFu ? h : uint16_t(h + 1);
  return uint16_t(h - 1);  // a negative: its magnitude goes down (a result of 0x8000 is -0, which is 0)
}
uint16_t HalfNextDown(uint16_t h) {
  if ((h & 0x7FFFu) == 0) return 0x8001u;
  if ((h & 0x8000u) == 0) return uint16_t(h - 1);
  return h == 0xFBFFu ? h : uint16_t(h + 1);
}
}  // namespace

uint16_t HalfFromFloatDown(float value) {
  if (std::isnan(value)) return 0;
  uint16_t h = HalfFromFloat(value);
  if (HalfToFloat(h) > value) h = HalfNextDown(h);
  return h;
}

uint16_t HalfFromFloatUp(float value) {
  if (std::isnan(value)) return 0;
  uint16_t h = HalfFromFloat(value);
  if (HalfToFloat(h) < value) h = HalfNextUp(h);
  return h;
}

// ---------------------------------------------------------------------------
// the material record
// ---------------------------------------------------------------------------

void HitMaterial::Pack(uint32_t words[8]) const {
  words[0] = word;
  words[1] = uint32_t(texture) | (uint32_t(layer) << 16);
  words[2] = PackRgb10(tint);
  words[3] = PackRgb10(average);
  words[4] = std::bit_cast<uint32_t>(std::isfinite(emissive) && emissive > 0.0f ? emissive : 0.0f);
  words[5] = uint32_t(key);
  words[6] = uint32_t(key >> 32);
  words[7] = shader_hash;
}

HitMaterial HitMaterial::Unpack(const uint32_t words[8]) {
  HitMaterial m;
  m.word = words[0];
  m.texture = uint16_t(words[1] & 0xFFFFu);
  m.layer = uint16_t(words[1] >> 16);
  UnpackRgb10(words[2], m.tint);
  UnpackRgb10(words[3], m.average);
  m.emissive = std::bit_cast<float>(words[4]);
  m.key = uint64_t(words[5]) | (uint64_t(words[6]) << 32);
  m.shader_hash = words[7];
  return m;
}

void HitMaterial::Write(ByteWriter& w) const {
  uint32_t words[8];
  Pack(words);
  for (uint32_t v : words) w.U32(v);
}

bool HitMaterial::Read(ByteReader& r, HitMaterial* out) {
  uint32_t words[8];
  for (uint32_t& v : words) v = r.U32();
  if (!r.ok()) return false;
  *out = Unpack(words);
  return true;
}

// ---------------------------------------------------------------------------
// the texture record
// ---------------------------------------------------------------------------

uint64_t HitMipBytes(HitTextureFormat format, uint32_t width, uint32_t height) {
  width = std::max(width, 1u);
  height = std::max(height, 1u);
  if (format == HitTextureFormat::kRgba8) return uint64_t(width) * height * 4u;
  const uint64_t blocks = uint64_t((width + 3u) / 4u) * ((height + 3u) / 4u);
  return blocks * (format == HitTextureFormat::kBc1 ? 8u : 16u);
}

uint64_t HitTexture::ChainBytes(uint32_t first_mip) const {
  uint64_t total = 0;
  for (uint32_t m = first_mip; m < mip_count; ++m) total += HitMipBytes(format, uint32_t(width) >> m, uint32_t(height) >> m);
  return total;
}

uint64_t HitTexture::MipOffset(uint32_t mip) const {
  uint64_t offset = 0;
  for (uint32_t m = 0; m < mip && m < mip_count; ++m) offset += HitMipBytes(format, uint32_t(width) >> m, uint32_t(height) >> m);
  return offset;
}

void HitTexture::Pack(uint32_t words[8]) const {
  words[0] = uint32_t(key);
  words[1] = uint32_t(key >> 32);
  words[2] = uint32_t(width) | (uint32_t(height) << 16);
  words[3] = uint32_t(format) | (uint32_t(mip_count) << 8) | (uint32_t(flags) << 16);
  words[4] = uint32_t(data_offset);
  words[5] = uint32_t(data_offset >> 32);
  words[6] = data_size;
  words[7] = stored_size;
}

HitTexture HitTexture::Unpack(const uint32_t words[8]) {
  HitTexture t;
  t.key = uint64_t(words[0]) | (uint64_t(words[1]) << 32);
  t.width = uint16_t(words[2] & 0xFFFFu);
  t.height = uint16_t(words[2] >> 16);
  t.format = static_cast<HitTextureFormat>(words[3] & 0xFFu);
  t.mip_count = uint8_t((words[3] >> 8) & 0xFFu);
  t.flags = uint8_t((words[3] >> 16) & 0xFFu);
  t.data_offset = uint64_t(words[4]) | (uint64_t(words[5]) << 32);
  t.data_size = words[6];
  t.stored_size = words[7];
  return t;
}

void HitTexture::Write(ByteWriter& w) const {
  uint32_t words[8];
  Pack(words);
  for (uint32_t v : words) w.U32(v);
}

bool HitTexture::Read(ByteReader& r, HitTexture* out) {
  uint32_t words[8];
  for (uint32_t& v : words) v = r.U32();
  if (!r.ok()) return false;
  HitTexture t = Unpack(words);
  if (static_cast<uint32_t>(t.format) >= static_cast<uint32_t>(HitTextureFormat::kCount)) return false;
  *out = t;
  return true;
}

GpuTextureRow MakeTextureRow(const HitTexture& texture, uint32_t slot, uint32_t first_resident_mip) {
  GpuTextureRow row;
  row.slot = slot == kNoSlot ? kNoSlot : ((slot & ~kGammaBit) | ((texture.flags & kTextureGamma) != 0 ? kGammaBit : 0u));
  row.size = uint32_t(texture.width) | (uint32_t(texture.height) << 16);
  const uint32_t first = texture.mip_count == 0 ? 0u : std::min<uint32_t>(first_resident_mip, texture.mip_count - 1u);
  const uint32_t resident = texture.mip_count > first ? texture.mip_count - first : 0u;
  row.mips = (resident & 0xFFu) | ((first & 0xFFu) << 8) | (uint32_t(texture.flags) << 16);
  return row;
}

// ---------------------------------------------------------------------------
// runs and the UV transform
// ---------------------------------------------------------------------------

void HitRun::Pack(uint32_t words[8]) const {
  words[0] = material;
  words[1] = flags;
  words[2] = uint32_t(HalfFromFloat(uv_bias[0])) | (uint32_t(HalfFromFloat(uv_bias[1])) << 16);
  words[3] = uint32_t(HalfFromFloat(uv_scale[0])) | (uint32_t(HalfFromFloat(uv_scale[1])) << 16);
  words[4] = first_triangle;
  words[5] = triangle_count;
  words[6] = 0;
  words[7] = 0;
}

HitRun HitRun::Unpack(const uint32_t words[8]) {
  HitRun r;
  r.material = words[0];
  r.flags = words[1];
  r.uv_bias[0] = HalfToFloat(uint16_t(words[2] & 0xFFFFu));
  r.uv_bias[1] = HalfToFloat(uint16_t(words[2] >> 16));
  r.uv_scale[0] = HalfToFloat(uint16_t(words[3] & 0xFFFFu));
  r.uv_scale[1] = HalfToFloat(uint16_t(words[3] >> 16));
  r.first_triangle = words[4];
  r.triangle_count = words[5];
  return r;
}

bool ChooseUvTransform(float min_u, float max_u, float min_v, float max_v, HitRun* run) {
  const float lo[2] = {min_u, min_v};
  const float hi[2] = {max_u, max_v};
  float bias[2], scale[2];
  for (int a = 0; a < 2; ++a) {
    if (!std::isfinite(lo[a]) || !std::isfinite(hi[a]) || lo[a] > hi[a]) return false;
    if (std::fabs(lo[a]) > 65504.0f || std::fabs(hi[a]) > 65504.0f) return false;
    bias[a] = HalfToFloat(HalfFromFloatDown(lo[a]));
    const double range = double(hi[a]) - double(bias[a]);
    if (range > 65504.0) return false;
    scale[a] = 1.0f;
    if (range > 0.0) {
      // the range as a float that is not below it (the rounding of the double may have fallen short), then the next float16 at or above
      float wanted = float(range);
      if (double(wanted) < range) wanted = std::nextafter(wanted, INFINITY);
      scale[a] = HalfToFloat(HalfFromFloatUp(wanted));
      if (!(double(scale[a]) >= range)) return false;  // the range is at the very top of the float16 numbers
    }
  }
  for (int a = 0; a < 2; ++a) {
    run->uv_bias[a] = bias[a];
    run->uv_scale[a] = scale[a];
  }
  return true;
}

uint32_t EncodeCornerUv(const HitRun& run, float u, float v) {
  const float in[2] = {u, v};
  uint32_t q[2];
  for (int a = 0; a < 2; ++a) {
    double t = (double(in[a]) - double(run.uv_bias[a])) / double(run.uv_scale[a]);
    if (!(t > 0.0)) t = 0.0;  // also a NaN
    if (t > 1.0) t = 1.0;
    q[a] = uint32_t(std::lround(t * 65535.0));
  }
  return q[0] | (q[1] << 16);
}

void DecodeCornerUv(const HitRun& run, uint32_t word, float uv[2]) {
  const float qu = float(word & 0xFFFFu) * (1.0f / 65535.0f);
  const float qv = float(word >> 16) * (1.0f / 65535.0f);
  uv[0] = run.uv_bias[0] + run.uv_scale[0] * qu;
  uv[1] = run.uv_bias[1] + run.uv_scale[1] * qv;
}

// ---------------------------------------------------------------------------
// the corner stream
// ---------------------------------------------------------------------------

void EncodeCorners(const uint32_t* words, size_t corners, CornerEncoding encoding, ByteWriter& w) {
  if (encoding == CornerEncoding::kRaw) {
    for (size_t i = 0; i < corners * 2; ++i) w.U32(words[i]);
    return;
  }
  int32_t previous_u = 0, previous_v = 0;
  uint32_t previous_colour = 0;
  for (size_t i = 0; i < corners; ++i) {
    const int32_t u = int32_t(words[2 * i] & 0xFFFFu);
    const int32_t v = int32_t(words[2 * i] >> 16);
    const uint32_t colour = words[2 * i + 1];
    const bool changed = colour != previous_colour;
    w.Varint((ZigZag(u - previous_u) << 1) | (changed ? 1u : 0u));
    w.Varint(ZigZag(v - previous_v));
    if (changed) w.U32(colour);
    previous_u = u;
    previous_v = v;
    previous_colour = colour;
  }
}

bool DecodeCorners(ByteReader& r, size_t corners, CornerEncoding encoding, std::vector<uint32_t>* out) {
  out->clear();
  out->reserve(corners * 2);
  if (encoding == CornerEncoding::kRaw) {
    for (size_t i = 0; i < corners * 2; ++i) out->push_back(r.U32());
    return r.ok();
  }
  if (encoding != CornerEncoding::kDelta) return false;
  int64_t previous_u = 0, previous_v = 0;
  uint32_t previous_colour = 0;
  for (size_t i = 0; i < corners; ++i) {
    const uint32_t head = r.Varint();
    const uint32_t zv = r.Varint();
    if (!r.ok()) return false;
    previous_u += UnZigZag(head >> 1);
    previous_v += UnZigZag(zv);
    if (head & 1u) previous_colour = r.U32();
    if (!r.ok() || previous_u < 0 || previous_u > 0xFFFF || previous_v < 0 || previous_v > 0xFFFF) return false;
    out->push_back(uint32_t(previous_u) | (uint32_t(previous_v) << 16));
    out->push_back(previous_colour);
  }
  return true;
}

// ---------------------------------------------------------------------------
// the hit blob
// ---------------------------------------------------------------------------

std::string CheckRuns(const std::vector<HitRun>& runs, uint32_t triangle_count, uint32_t opaque_triangles) {
  if (runs.size() > kMaxRunsPerBlob) return "a blob has more runs than a 16 bit run id holds";
  if (opaque_triangles > triangle_count) return "a blob has more opaque triangles than triangles";
  uint64_t next = 0;
  for (size_t i = 0; i < runs.size(); ++i) {
    const HitRun& run = runs[i];
    if (run.first_triangle != next) return "a run does not start where the one before it ended";
    if (run.triangle_count == 0) return "a run has no triangles";
    const uint64_t end = uint64_t(run.first_triangle) + run.triangle_count;
    if (end > triangle_count) return "a run goes past the triangles";
    if (run.first_triangle < opaque_triangles && end > opaque_triangles) return "a run holds both opaque and alpha tested triangles";
    const bool alpha = run.first_triangle >= opaque_triangles;
    if (((run.flags & kRunAlphaTested) != 0) != alpha) return "a run's alpha tested flag does not match its side of the opaque boundary";
    next = end;
  }
  if (next != triangle_count) return "the runs do not cover every triangle";
  return std::string();
}

std::vector<uint16_t> ExpandRunIds(const std::vector<HitRun>& runs, uint32_t triangle_count) {
  std::vector<uint16_t> ids;
  uint64_t next = 0;
  for (const HitRun& run : runs) {
    if (run.first_triangle != next || uint64_t(run.first_triangle) + run.triangle_count > triangle_count) return {};
    next += run.triangle_count;
  }
  if (next != triangle_count || runs.size() > kMaxRunsPerBlob) return {};
  ids.resize(triangle_count);
  for (size_t i = 0; i < runs.size(); ++i) {
    std::fill_n(ids.begin() + runs[i].first_triangle, runs[i].triangle_count, uint16_t(i));
  }
  return ids;
}

std::vector<uint8_t> EncodeHitBlob(const HitBlob& blob, CornerEncoding corners) {
  const bool has_corners = blob.has_corners();
  const uint32_t stream_count = has_corners ? 2u : 1u;
  const uint32_t body_start = kHitBlobHeaderBytes + stream_count * kStreamEntryBytes;

  ByteWriter body;
  std::vector<StreamEntry> table;
  {  // runs
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
  if (has_corners) {
    body.Align(4);
    StreamEntry e;
    e.id = kHitStreamCorners;
    e.encoding = static_cast<uint32_t>(corners);
    e.offset = body_start + uint32_t(body.size());
    e.count = blob.triangle_count * 3u;
    e.element_bytes = corners == CornerEncoding::kRaw ? 8u : 0u;
    EncodeCorners(blob.corners.data(), blob.corners.size() / 2, corners, body);
    e.size = body_start + uint32_t(body.size()) - e.offset;
    table.push_back(e);
  }
  body.Align(4);

  ByteWriter tail;  // stream table + streams: what the hash covers
  for (const StreamEntry& e : table) e.Write(tail);
  tail.Bytes(body.bytes().data(), body.size());

  ByteWriter blob_bytes;
  const size_t start = blob_bytes.size();
  blob_bytes.U32(kHitBlobMagic);
  blob_bytes.U32(blob.flags);
  blob_bytes.U32(blob.triangle_count);
  blob_bytes.U32(blob.opaque_triangles);
  blob_bytes.U32(uint32_t(blob.runs.size()));
  blob_bytes.U32(stream_count);
  blob_bytes.U64(Fnv1a64(tail.bytes().data(), tail.size()));
  blob_bytes.Zeros(kHitBlobHeaderBytes - (blob_bytes.size() - start));
  blob_bytes.Bytes(tail.bytes().data(), tail.size());
  return std::move(blob_bytes.bytes());
}

bool DecodeHitBlob(const uint8_t* data, size_t size, HitBlob* out, std::string* error) {
  const auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  ByteReader r(data, size);
  if (r.U32() != kHitBlobMagic) return fail("a hit blob's magic is wrong");
  HitBlob blob;
  blob.flags = r.U32();
  blob.triangle_count = r.U32();
  blob.opaque_triangles = r.U32();
  const uint32_t run_count = r.U32();
  const uint32_t stream_count = r.U32();
  const uint64_t body_hash = r.U64();
  r.Skip(kHitBlobHeaderBytes - r.pos());
  if (!r.ok()) return fail("a hit blob's header is cut short");
  if (stream_count > 64 || blob.triangle_count > (1u << 28) || blob.opaque_triangles > blob.triangle_count || run_count > kMaxRunsPerBlob) return fail("a hit blob's counts are out of range");
  if (Fnv1a64(data + kHitBlobHeaderBytes, size - kHitBlobHeaderBytes) != body_hash) return fail("a hit blob's hash does not match (the file is damaged)");
  std::vector<StreamEntry> table(stream_count);
  for (StreamEntry& e : table) {
    if (!StreamEntry::Read(r, &e)) return fail("a hit blob's stream table is cut short");
  }
  bool have_runs = false, have_corners = false;
  for (const StreamEntry& e : table) {
    if (uint64_t(e.offset) + e.size > size) return fail("a hit blob's stream runs past its end");
    ByteReader s(data + e.offset, e.size);
    if (e.id == kHitStreamRuns) {
      if (e.count != run_count || uint64_t(e.count) * kHitRunBytes > e.size) return fail("a hit blob's runs are cut short");
      blob.runs.resize(e.count);
      for (HitRun& run : blob.runs) {
        uint32_t words[8];
        for (uint32_t& v : words) v = s.U32();
        run = HitRun::Unpack(words);
      }
      have_runs = true;
    } else if (e.id == kHitStreamCorners) {
      if (e.count != blob.triangle_count * 3u) return fail("a hit blob's corner count is not three a triangle");
      if (e.encoding != static_cast<uint32_t>(CornerEncoding::kRaw) && e.encoding != static_cast<uint32_t>(CornerEncoding::kDelta)) {
        return fail("a hit blob's corners have an encoding this build does not know");
      }
      if (!DecodeCorners(s, e.count, static_cast<CornerEncoding>(e.encoding), &blob.corners)) return fail("a hit blob's corners are cut short or out of range");
      have_corners = true;
    }
    // any other id (a stream of a later version) is skipped
  }
  if (!have_runs) return fail("a hit blob has no runs");
  if (blob.has_corners() != have_corners) return fail("a hit blob's flags do not match its corner stream");
  const std::string runs = CheckRuns(blob.runs, blob.triangle_count, blob.opaque_triangles);
  if (!runs.empty()) return fail("a hit blob's runs are not a partition of its triangles");
  *out = std::move(blob);
  return true;
}

// ---------------------------------------------------------------------------
// the file
// ---------------------------------------------------------------------------

void HitHeader::Write(ByteWriter& w) const {
  const size_t start = w.size();
  w.Bytes(kHitMagic, sizeof(kHitMagic));
  w.U32(format_version);
  w.U32(builder_version);
  w.U32(flags);
  w.U32(sector_count);
  w.U32(sector_record_bytes);
  w.U32(material_count);
  w.U32(material_record_bytes);
  w.U32(texture_count);
  w.U32(texture_record_bytes);
  w.U64(base_table_hash);
  w.U64(base_file_size);
  w.U64(table_offset);
  w.U64(materials_offset);
  w.U64(textures_offset);
  w.U64(blobs_offset);
  w.U64(file_size);
  w.U64(table_hash);
  w.Zeros(kHitHeaderBytes - (w.size() - start));
}

bool HitHeader::Read(ByteReader& r, HitHeader* out, std::string* error) {
  const size_t start = r.pos();
  char magic[8] = {};
  if (!r.Read(magic, sizeof(magic)) || std::memcmp(magic, kHitMagic, sizeof(magic)) != 0) {
    if (error) *error = "not a city hit file (the magic is wrong)";
    return false;
  }
  HitHeader h;
  h.format_version = r.U32();
  h.builder_version = r.U32();
  h.flags = r.U32();
  h.sector_count = r.U32();
  h.sector_record_bytes = r.U32();
  h.material_count = r.U32();
  h.material_record_bytes = r.U32();
  h.texture_count = r.U32();
  h.texture_record_bytes = r.U32();
  h.base_table_hash = r.U64();
  h.base_file_size = r.U64();
  h.table_offset = r.U64();
  h.materials_offset = r.U64();
  h.textures_offset = r.U64();
  h.blobs_offset = r.U64();
  h.file_size = r.U64();
  h.table_hash = r.U64();
  r.Skip(kHitHeaderBytes - (r.pos() - start));
  if (!r.ok()) {
    if (error) *error = "the city hit file's header is cut short";
    return false;
  }
  if (h.format_version != kHitFormatVersion) {
    if (error) *error = "the city hit file is format version " + std::to_string(h.format_version) + ", this build reads " + std::to_string(kHitFormatVersion);
    return false;
  }
  if (h.sector_record_bytes != kHitSectorRecordBytes || h.material_record_bytes != kHitMaterialBytes || h.texture_record_bytes != kHitTextureBytes || h.sector_count > 100000 ||
      h.material_count == 0 || h.material_count > kMaxMaterials || h.texture_count > kMaxTextures) {
    if (error) *error = "the city hit file's tables do not have the shape this build reads";
    return false;
  }
  *out = h;
  return true;
}

void HitSectorRecord::Write(ByteWriter& w) const {
  w.U32(name_hash);
  w.U32(flags);
  w.U64(hi_offset);
  w.U64(lod_offset);
  w.U32(hi_size);
  w.U32(lod_size);
  w.U32(hi_triangles);
  w.U32(lod_triangles);
  w.U32(hi_runs);
  w.U32(lod_runs);
}

bool HitSectorRecord::Read(ByteReader& r, HitSectorRecord* out) {
  HitSectorRecord s;
  s.name_hash = r.U32();
  s.flags = r.U32();
  s.hi_offset = r.U64();
  s.lod_offset = r.U64();
  s.hi_size = r.U32();
  s.lod_size = r.U32();
  s.hi_triangles = r.U32();
  s.lod_triangles = r.U32();
  s.hi_runs = r.U32();
  s.lod_runs = r.U32();
  if (!r.ok()) return false;
  *out = s;
  return true;
}

HitFileLayout PlanHitFile(uint32_t sector_count, uint32_t material_count, uint32_t texture_count) {
  HitFileLayout l;
  l.table_offset = kHitHeaderBytes;
  l.materials_offset = AlignUp(l.table_offset + uint64_t(sector_count) * kHitSectorRecordBytes, kHitSectionAlignment);
  l.textures_offset = AlignUp(l.materials_offset + uint64_t(material_count) * kHitMaterialBytes, kHitSectionAlignment);
  l.blobs_offset = AlignUp(l.textures_offset + uint64_t(texture_count) * kHitTextureBytes, kHitSectionAlignment);
  return l;
}

void TexHeader::Write(ByteWriter& w) const {
  const size_t start = w.size();
  w.Bytes(kTexMagic, sizeof(kTexMagic));
  w.U32(format_version);
  w.U32(builder_version);
  w.U32(texture_count);
  w.U32(flags);
  w.U64(hit_table_hash);
  w.U64(data_offset);
  w.U64(file_size);
  w.Zeros(kTexHeaderBytes - (w.size() - start));
}

bool TexHeader::Read(ByteReader& r, TexHeader* out, std::string* error) {
  const size_t start = r.pos();
  char magic[8] = {};
  if (!r.Read(magic, sizeof(magic)) || std::memcmp(magic, kTexMagic, sizeof(magic)) != 0) {
    if (error) *error = "not a city texture file (the magic is wrong)";
    return false;
  }
  TexHeader h;
  h.format_version = r.U32();
  h.builder_version = r.U32();
  h.texture_count = r.U32();
  h.flags = r.U32();
  h.hit_table_hash = r.U64();
  h.data_offset = r.U64();
  h.file_size = r.U64();
  r.Skip(kTexHeaderBytes - (r.pos() - start));
  if (!r.ok()) {
    if (error) *error = "the city texture file's header is cut short";
    return false;
  }
  if (h.format_version != kHitFormatVersion || h.texture_count > kMaxTextures || h.data_offset < kTexHeaderBytes) {
    if (error) *error = "the city texture file is of another format version or has the wrong shape";
    return false;
  }
  *out = h;
  return true;
}

// ---------------------------------------------------------------------------
// the pool
// ---------------------------------------------------------------------------

PoolBlockPlan PlanPoolBlock(uint32_t vertex_count, uint32_t triangle_count, uint32_t run_count, bool has_runs, bool has_corners) {
  PoolBlockPlan p;
  p.has_runs = has_runs;
  p.has_corners = has_corners && has_runs;  // a corner is decoded with its run's transform
  uint64_t at = 0;
  p.index_offset = at;
  at = AlignUp(at + uint64_t(triangle_count) * 12u, kPoolAlignment);
  p.position_offset = at;
  at = AlignUp(at + uint64_t(vertex_count) * 12u, kPoolAlignment);
  if (p.has_runs) {
    p.run_id_offset = at;
    at = AlignUp(at + uint64_t((uint64_t(triangle_count) + 1u) / 2u) * 4u, kPoolAlignment);
    p.run_table_offset = at;
    at = AlignUp(at + uint64_t(run_count) * kHitRunBytes, kPoolAlignment);
  }
  if (p.has_corners) {
    p.attribute_offset = at;
    at = AlignUp(at + uint64_t(triangle_count) * 24u, kPoolAlignment);
  }
  p.total_bytes = at;
  return p;
}

bool MakeSectorRow(const PoolBlockPlan& plan, uint64_t base, uint32_t triangle_count, uint32_t opaque_triangles, uint32_t extra_flags, GpuSectorRow* out) {
  if (base % kPoolAlignment != 0 || base + plan.total_bytes > kMaxPoolBytes || opaque_triangles > triangle_count) return false;
  if (plan.has_corners && !plan.has_runs) return false;
  GpuSectorRow row;
  row.index_offset = uint32_t(base + plan.index_offset);
  row.position_offset = uint32_t(base + plan.position_offset);
  row.attribute_offset = plan.has_corners ? uint32_t(base + plan.attribute_offset) : 0u;
  row.run_id_offset = plan.has_runs ? uint32_t(base + plan.run_id_offset) : 0u;
  row.run_table_offset = plan.has_runs ? uint32_t(base + plan.run_table_offset) : 0u;
  row.triangle_count = triangle_count;
  row.opaque_triangles = opaque_triangles;
  row.flags = kSectorRowValid | (plan.has_runs ? kSectorRowRuns : 0u) | (plan.has_corners ? kSectorRowCorners : 0u) | (extra_flags & kSectorRowFoliage);
  *out = row;
  return true;
}

bool FillPoolBlock(const PoolBlockPlan& plan, const MeshData& mesh, const HitBlob* blob, uint8_t* dest, size_t dest_size, std::string* error) {
  const auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  const uint32_t triangles = mesh.triangle_count();
  const uint32_t vertices = mesh.vertex_count();
  if (plan.has_runs && !blob) return fail("a block with runs needs the hit blob");
  if (plan.has_corners && !plan.has_runs) return fail("corners need runs");
  const PoolBlockPlan expected = PlanPoolBlock(vertices, triangles, blob ? uint32_t(blob->runs.size()) : 0u, plan.has_runs, plan.has_corners);
  if (expected.total_bytes != plan.total_bytes || expected.index_offset != plan.index_offset || expected.position_offset != plan.position_offset ||
      expected.run_id_offset != plan.run_id_offset || expected.run_table_offset != plan.run_table_offset || expected.attribute_offset != plan.attribute_offset) {
    return fail("the block's plan is not the plan of this mesh and blob");
  }
  if (dest_size < plan.total_bytes) return fail("the destination is smaller than the block");
  if (mesh.indices.size() != size_t(triangles) * 3 || mesh.positions.size() != size_t(vertices) * 3) return fail("the mesh's arrays do not match its counts");
  if (blob) {
    if (blob->triangle_count != triangles || blob->opaque_triangles != mesh.opaque_triangles) return fail("the hit blob's triangles are not the mesh's");
    if (!CheckRuns(blob->runs, triangles, mesh.opaque_triangles).empty()) return fail("the hit blob's runs are not a partition");
    if (plan.has_corners && (!blob->has_corners() || blob->corners.size() != size_t(triangles) * 6)) return fail("the hit blob has no corners for every triangle");
  }
  std::memset(dest, 0, size_t(plan.total_bytes));
  for (size_t i = 0; i < mesh.indices.size(); ++i) PutU32(dest + plan.index_offset + i * 4, mesh.indices[i]);
  for (size_t i = 0; i < mesh.positions.size(); ++i) PutU32(dest + plan.position_offset + i * 4, std::bit_cast<uint32_t>(mesh.positions[i]));
  if (plan.has_runs) {
    const std::vector<uint16_t> ids = ExpandRunIds(blob->runs, triangles);
    for (size_t t = 0; t < ids.size(); ++t) PutU16(dest + plan.run_id_offset + t * 2, ids[t]);
    for (size_t i = 0; i < blob->runs.size(); ++i) {
      uint32_t words[8];
      blob->runs[i].Pack(words);
      for (size_t k = 0; k < 8; ++k) PutU32(dest + plan.run_table_offset + i * kHitRunBytes + k * 4, words[k]);
    }
  }
  if (plan.has_corners) {
    for (size_t i = 0; i < blob->corners.size(); ++i) PutU32(dest + plan.attribute_offset + i * 4, blob->corners[i]);
  }
  return true;
}

// ---------------------------------------------------------------------------
// the decode (the twin of hlsl/city_hit_common.hlsli)
// ---------------------------------------------------------------------------

namespace {

struct Reader {
  const HitView& view;
  bool ok = true;
  uint32_t U32(uint64_t offset) {
    if (offset + 4 > view.pool_size) {
      ok = false;
      return 0;
    }
    const uint8_t* p = view.pool + offset;
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
  }
  float F32(uint64_t offset) { return std::bit_cast<float>(U32(offset)); }
};

}  // namespace

DecodedHit DecodeHit(const HitView& view, uint32_t instance_id, uint32_t geometry_index, uint32_t primitive_index, float b1, float b2) {
  DecodedHit h;
  const uint32_t slot = SectorRowOfInstance(instance_id);
  if (!view.sectors || slot >= view.sector_slots) return h;
  const uint32_t* row = view.sectors + size_t(slot) * 8;
  GpuSectorRow s;
  s.index_offset = row[0];
  s.position_offset = row[1];
  s.attribute_offset = row[2];
  s.run_id_offset = row[3];
  s.run_table_offset = row[4];
  s.triangle_count = row[5];
  s.opaque_triangles = row[6];
  s.flags = row[7];
  h.sector_flags = s.flags;
  if ((s.flags & kSectorRowValid) == 0) return h;
  const uint32_t tri = TriangleOfHit(geometry_index, primitive_index, s.opaque_triangles);
  h.triangle = tri;
  if (tri >= s.triangle_count) return h;
  Reader rd{view};
  uint32_t index[3];
  for (uint32_t k = 0; k < 3; ++k) index[k] = rd.U32(uint64_t(s.index_offset) + uint64_t(tri) * 12 + k * 4);
  for (uint32_t k = 0; k < 3; ++k) {
    for (uint32_t c = 0; c < 3; ++c) h.p[k][c] = rd.F32(uint64_t(s.position_offset) + uint64_t(index[k]) * 12 + c * 4);
  }
  if (!rd.ok) {
    h = DecodedHit();
    h.sector_flags = s.flags;
    h.triangle = tri;
    return h;
  }
  const float e1[3] = {h.p[1][0] - h.p[0][0], h.p[1][1] - h.p[0][1], h.p[1][2] - h.p[0][2]};
  const float e2[3] = {h.p[2][0] - h.p[0][0], h.p[2][1] - h.p[0][1], h.p[2][2] - h.p[0][2]};
  const float c[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
  const float l2 = c[0] * c[0] + c[1] * c[1] + c[2] * c[2];
  h.world_area = 0.5f * std::sqrt(l2);
  if (l2 > 1e-24f) {
    const float inv = 1.0f / std::sqrt(l2);
    for (int a = 0; a < 3; ++a) h.normal[a] = c[a] * inv;
  } else {
    h.normal[0] = 0.0f;
    h.normal[1] = 1.0f;
    h.normal[2] = 0.0f;
  }
  if ((s.flags & kSectorRowRuns) != 0) {
    const uint32_t word = rd.U32(uint64_t(s.run_id_offset) + uint64_t(tri >> 1) * 4);
    const uint32_t run_id = (word >> ((tri & 1u) * 16u)) & 0xFFFFu;
    uint32_t words[8];
    for (uint32_t k = 0; k < 8; ++k) words[k] = rd.U32(uint64_t(s.run_table_offset) + uint64_t(run_id) * kHitRunBytes + k * 4);
    if (rd.ok) {
      const HitRun run = HitRun::Unpack(words);
      h.material = run.material;
      h.run_flags = run.flags;
      if ((s.flags & kSectorRowCorners) != 0) {
        float uv[3][2];
        float colour[3][4];
        for (uint32_t k = 0; k < 3; ++k) {
          const uint32_t uv_word = rd.U32(uint64_t(s.attribute_offset) + uint64_t(tri) * 24 + k * 8);
          const uint32_t colour_word = rd.U32(uint64_t(s.attribute_offset) + uint64_t(tri) * 24 + k * 8 + 4);
          DecodeCornerUv(run, uv_word, uv[k]);
          for (uint32_t a = 0; a < 4; ++a) colour[k][a] = float((colour_word >> (8 * a)) & 255u) * (1.0f / 255.0f);
        }
        const float w0 = 1.0f - b1 - b2;
        for (int a = 0; a < 2; ++a) h.uv[a] = uv[0][a] * w0 + uv[1][a] * b1 + uv[2][a] * b2;
        for (int a = 0; a < 4; ++a) h.colour[a] = colour[0][a] * w0 + colour[1][a] * b1 + colour[2][a] * b2;
        h.uv_area = 0.5f * std::fabs((uv[1][0] - uv[0][0]) * (uv[2][1] - uv[0][1]) - (uv[2][0] - uv[0][0]) * (uv[1][1] - uv[0][1]));
      }
    }
  }
  h.valid = rd.ok;
  return h;
}

}  // namespace larecomp::citypack
