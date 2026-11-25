#include "citypack/pack_format.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace larecomp::citypack {

std::string_view ClassName(MaterialClass c) {
  switch (c) {
    case MaterialClass::kSolid: return "solid";
    case MaterialClass::kGround: return "ground";
    case MaterialClass::kGlass: return "glass";
    case MaterialClass::kEmissive: return "emissive";
    case MaterialClass::kBillboard: return "billboard";
    case MaterialClass::kAlphaTested: return "alpha";
    case MaterialClass::kDecal: return "decal";
    case MaterialClass::kInterior: return "interior";
    case MaterialClass::kWater: return "water";
    default: return "?";
  }
}

// ---------------------------------------------------------------------------
// the header, the sector table, the mesh header
// ---------------------------------------------------------------------------

void PackHeader::Write(ByteWriter& w) const {
  const size_t start = w.size();
  w.Bytes(kPackMagic, sizeof(kPackMagic));
  w.U32(format_version);
  w.U32(builder_version);
  w.U32(flags);
  w.U32(sector_count);
  w.U32(sector_record_bytes);
  w.U32(names_bytes);
  w.U32(drop_mask);
  w.U32(position_encoding);
  w.U64(table_offset);
  w.U64(names_offset);
  w.U64(blobs_offset);
  w.U64(file_size);
  w.U64(table_hash);
  w.U64(lamps_offset);
  w.U64(lamps_size);
  w.U64(lamps_hash);
  const size_t written = w.size() - start;
  w.Zeros(kHeaderBytes - written);
}

bool PackHeader::Read(ByteReader& r, PackHeader* out, std::string* error) {
  char magic[8] = {};
  if (!r.Read(magic, sizeof(magic)) || std::memcmp(magic, kPackMagic, sizeof(magic)) != 0) {
    if (error) *error = "not a city pack (the magic is wrong)";
    return false;
  }
  PackHeader h;
  h.format_version = r.U32();
  h.builder_version = r.U32();
  h.flags = r.U32();
  h.sector_count = r.U32();
  h.sector_record_bytes = r.U32();
  h.names_bytes = r.U32();
  h.drop_mask = r.U32();
  h.position_encoding = r.U32();
  h.table_offset = r.U64();
  h.names_offset = r.U64();
  h.blobs_offset = r.U64();
  h.file_size = r.U64();
  h.table_hash = r.U64();
  h.lamps_offset = r.U64();  // zero in a pack of builder version 1
  h.lamps_size = r.U64();
  h.lamps_hash = r.U64();
  if (!r.ok()) {
    if (error) *error = "the city pack's header is cut short";
    return false;
  }
  if (h.format_version != kPackFormatVersion) {
    if (error) *error = "the city pack is format version " + std::to_string(h.format_version) + ", this build reads " + std::to_string(kPackFormatVersion);
    return false;
  }
  if (h.sector_record_bytes != kSectorRecordBytes || h.sector_count > 100000) {
    if (error) *error = "the city pack's sector table does not have the shape this build reads";
    return false;
  }
  *out = h;
  return true;
}

void SectorRecord::Write(ByteWriter& w) const {
  const size_t start = w.size();
  w.U32(name_hash);
  w.U32(name_offset);
  w.U32(name_length);
  w.U32(flags);
  for (float v : aabb_min) w.F32(v);
  for (float v : aabb_max) w.F32(v);
  for (float v : hi_min) w.F32(v);
  for (float v : hi_max) w.F32(v);
  w.U64(hi_offset);
  w.U32(hi_size);
  w.U32(hi_triangles);
  w.U32(hi_vertices);
  w.U32(hi_opaque_triangles);
  w.U64(lod_offset);
  w.U32(lod_size);
  w.U32(lod_triangles);
  w.U32(lod_vertices);
  w.U32(dropped_triangles);
  for (float v : lod_min) w.F32(v);
  for (float v : lod_max) w.F32(v);
  w.Zeros(kSectorRecordBytes - (w.size() - start));
}

bool SectorRecord::Read(ByteReader& r, SectorRecord* out) {
  SectorRecord s;
  const size_t start = r.pos();
  s.name_hash = r.U32();
  s.name_offset = r.U32();
  s.name_length = r.U32();
  s.flags = r.U32();
  for (float& v : s.aabb_min) v = r.F32();
  for (float& v : s.aabb_max) v = r.F32();
  for (float& v : s.hi_min) v = r.F32();
  for (float& v : s.hi_max) v = r.F32();
  s.hi_offset = r.U64();
  s.hi_size = r.U32();
  s.hi_triangles = r.U32();
  s.hi_vertices = r.U32();
  s.hi_opaque_triangles = r.U32();
  s.lod_offset = r.U64();
  s.lod_size = r.U32();
  s.lod_triangles = r.U32();
  s.lod_vertices = r.U32();
  s.dropped_triangles = r.U32();
  for (float& v : s.lod_min) v = r.F32();
  for (float& v : s.lod_max) v = r.F32();
  r.Skip(kSectorRecordBytes - (r.pos() - start));
  if (!r.ok()) return false;
  *out = s;
  return true;
}

void MeshHeader::Write(ByteWriter& w) const {
  const size_t start = w.size();
  w.U32(kMeshMagic);
  w.U32(flags);
  w.U32(vertex_count);
  w.U32(triangle_count);
  w.U32(opaque_triangles);
  w.U32(run_count);
  w.U32(stream_count);
  for (float v : bounds_min) w.F32(v);
  for (float v : bounds_max) w.F32(v);
  w.U64(body_hash);
  w.Zeros(kMeshHeaderBytes - (w.size() - start));
}

bool MeshHeader::Read(ByteReader& r, MeshHeader* out) {
  MeshHeader h;
  const size_t start = r.pos();
  if (r.U32() != kMeshMagic) return false;
  h.flags = r.U32();
  h.vertex_count = r.U32();
  h.triangle_count = r.U32();
  h.opaque_triangles = r.U32();
  h.run_count = r.U32();
  h.stream_count = r.U32();
  for (float& v : h.bounds_min) v = r.F32();
  for (float& v : h.bounds_max) v = r.F32();
  h.body_hash = r.U64();
  r.Skip(kMeshHeaderBytes - (r.pos() - start));
  if (!r.ok()) return false;
  *out = h;
  return true;
}

void StreamEntry::Write(ByteWriter& w) const {
  w.U32(id);
  w.U32(encoding);
  w.U32(offset);
  w.U32(size);
  w.U32(count);
  w.U32(element_bytes);
}

bool StreamEntry::Read(ByteReader& r, StreamEntry* out) {
  StreamEntry e;
  e.id = r.U32();
  e.encoding = r.U32();
  e.offset = r.U32();
  e.size = r.U32();
  e.count = r.U32();
  e.element_bytes = r.U32();
  if (!r.ok()) return false;
  *out = e;
  return true;
}

// ---------------------------------------------------------------------------
// the index codec
// ---------------------------------------------------------------------------

void EncodeIndicesDelta(const uint32_t* indices, size_t count, ByteWriter& w) {
  int64_t previous = 0;
  for (size_t i = 0; i < count; ++i) {
    const int64_t delta = int64_t(indices[i]) - previous;
    previous = int64_t(indices[i]);
    // zigzag: 0, -1, 1, -2, 2 ... -> 0, 1, 2, 3, 4 (the deltas are within +-2^32, the result within 2^33: it is written in two parts when it needs more than 32 bits)
    const uint64_t zigzag = (static_cast<uint64_t>(delta) << 1) ^ static_cast<uint64_t>(delta >> 63);
    if (zigzag > 0xFFFFFFFFull) {
      // Not reachable for a mesh (indices stay far below 2^31); written as a marker the reader rejects rather than a wrong number.
      w.Varint(0xFFFFFFFFu);
      continue;
    }
    w.Varint(static_cast<uint32_t>(zigzag));
  }
}

bool DecodeIndicesDelta(ByteReader& r, size_t count, uint32_t vertex_count, std::vector<uint32_t>* out) {
  out->clear();
  out->reserve(count);
  int64_t previous = 0;
  for (size_t i = 0; i < count; ++i) {
    const uint32_t zigzag = r.Varint();
    if (!r.ok() || zigzag == 0xFFFFFFFFu) return false;
    const int64_t delta = static_cast<int64_t>(zigzag >> 1) ^ -static_cast<int64_t>(zigzag & 1u);
    previous += delta;
    if (previous < 0 || previous >= int64_t(vertex_count)) return false;
    out->push_back(static_cast<uint32_t>(previous));
  }
  return true;
}

// ---------------------------------------------------------------------------
// the mesh blob
// ---------------------------------------------------------------------------

namespace {

void EncodePositionsGrid16(const MeshData& mesh, ByteWriter& w) {
  float lo[3], step[3];
  for (int a = 0; a < 3; ++a) {
    lo[a] = mesh.bounds_min[a];
    const float extent = mesh.bounds_max[a] - mesh.bounds_min[a];
    step[a] = extent > 0.0f ? extent / 65535.0f : 1.0f;
    w.F32(lo[a]);
  }
  for (int a = 0; a < 3; ++a) w.F32(step[a]);
  const size_t vertices = mesh.positions.size() / 3;
  for (size_t v = 0; v < vertices; ++v) {
    for (int a = 0; a < 3; ++a) {
      const double q = std::round((double(mesh.positions[v * 3 + size_t(a)]) - double(lo[a])) / double(step[a]));
      w.U16(static_cast<uint16_t>(std::clamp(q, 0.0, 65535.0)));
    }
  }
}

bool DecodePositionsGrid16(ByteReader& r, size_t vertices, std::vector<float>* out) {
  float lo[3], step[3];
  for (float& v : lo) v = r.F32();
  for (float& v : step) v = r.F32();
  if (!r.ok()) return false;
  out->resize(vertices * 3);
  for (size_t v = 0; v < vertices; ++v) {
    for (int a = 0; a < 3; ++a) {
      (*out)[v * 3 + size_t(a)] = lo[a] + float(r.U16()) * step[a];
    }
  }
  return r.ok();
}

}  // namespace

std::vector<uint8_t> EncodeMesh(const MeshData& mesh, PositionEncoding positions, IndexEncoding indices) {
  constexpr uint32_t kStreams = 3;
  const uint32_t vertices = mesh.vertex_count();
  const uint32_t triangles = mesh.triangle_count();
  const uint32_t body_start = kMeshHeaderBytes + kStreams * kStreamEntryBytes;

  ByteWriter body;  // the streams, each aligned to four bytes; offsets are from the blob's start
  std::vector<StreamEntry> table;

  {  // positions
    body.Align(4);
    StreamEntry e;
    e.id = kStreamPositions;
    e.encoding = static_cast<uint32_t>(positions);
    e.offset = body_start + uint32_t(body.size());
    e.count = vertices;
    if (positions == PositionEncoding::kGrid16) {
      EncodePositionsGrid16(mesh, body);
      e.element_bytes = 6;
    } else {
      for (float v : mesh.positions) body.F32(v);
      e.element_bytes = 12;
    }
    e.size = body_start + uint32_t(body.size()) - e.offset;
    table.push_back(e);
  }
  {  // indices
    body.Align(4);
    StreamEntry e;
    e.id = kStreamIndices;
    e.encoding = static_cast<uint32_t>(indices);
    e.offset = body_start + uint32_t(body.size());
    e.count = triangles * 3;
    if (indices == IndexEncoding::kDeltaVarint) {
      EncodeIndicesDelta(mesh.indices.data(), mesh.indices.size(), body);
      e.element_bytes = 0;
    } else {
      for (uint32_t i : mesh.indices) body.U32(i);
      e.element_bytes = 4;
    }
    e.size = body_start + uint32_t(body.size()) - e.offset;
    table.push_back(e);
  }
  {  // runs
    body.Align(4);
    StreamEntry e;
    e.id = kStreamRuns;
    e.encoding = 0;
    e.offset = body_start + uint32_t(body.size());
    e.count = uint32_t(mesh.runs.size());
    e.element_bytes = kRunBytes;
    for (const GeometryRun& run : mesh.runs) {
      body.U32(run.first_triangle);
      body.U32(run.triangle_count);
      body.U8(static_cast<uint8_t>(run.material));
      body.U8(run.flags);
      body.U16(run.shader_index);
      body.U32(run.shader_hash);
    }
    e.size = body_start + uint32_t(body.size()) - e.offset;
    table.push_back(e);
  }
  body.Align(4);

  ByteWriter tail;  // stream table + streams: what the hash covers
  for (const StreamEntry& e : table) e.Write(tail);
  tail.Bytes(body.bytes().data(), body.size());

  MeshHeader header;
  header.flags = mesh.alpha_triangles() != 0 ? 1u : 0u;
  header.vertex_count = vertices;
  header.triangle_count = triangles;
  header.opaque_triangles = mesh.opaque_triangles;
  header.run_count = uint32_t(mesh.runs.size());
  header.stream_count = kStreams;
  std::memcpy(header.bounds_min, mesh.bounds_min, sizeof(header.bounds_min));
  std::memcpy(header.bounds_max, mesh.bounds_max, sizeof(header.bounds_max));
  header.body_hash = Fnv1a64(tail.bytes().data(), tail.size());

  ByteWriter blob;
  header.Write(blob);
  blob.Bytes(tail.bytes().data(), tail.size());
  return std::move(blob.bytes());
}

bool DecodeMesh(const uint8_t* data, size_t size, MeshData* out, std::string* error) {
  const auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  ByteReader r(data, size);
  MeshHeader header;
  if (!MeshHeader::Read(r, &header)) return fail("a mesh blob's header is cut short or has the wrong magic");
  if (header.stream_count > 64 || header.vertex_count > (1u << 28) || header.triangle_count > (1u << 28) || header.opaque_triangles > header.triangle_count ||
      header.run_count > (1u << 24)) {
    return fail("a mesh blob's counts are out of range");
  }
  if (Fnv1a64(data + kMeshHeaderBytes, size - kMeshHeaderBytes) != header.body_hash) return fail("a mesh blob's hash does not match (the file is damaged)");
  std::vector<StreamEntry> table(header.stream_count);
  for (StreamEntry& e : table) {
    if (!StreamEntry::Read(r, &e)) return fail("a mesh blob's stream table is cut short");
  }
  MeshData mesh;
  mesh.opaque_triangles = header.opaque_triangles;
  std::memcpy(mesh.bounds_min, header.bounds_min, sizeof(mesh.bounds_min));
  std::memcpy(mesh.bounds_max, header.bounds_max, sizeof(mesh.bounds_max));
  bool have_positions = false, have_indices = false;
  for (const StreamEntry& e : table) {
    if (uint64_t(e.offset) + e.size > size) return fail("a mesh blob's stream runs past its end");
    ByteReader s(data + e.offset, e.size);
    if (e.id == kStreamPositions) {
      if (e.count != header.vertex_count) return fail("a mesh blob's position count is not its vertex count");
      if (e.encoding == static_cast<uint32_t>(PositionEncoding::kFloat32)) {
        if (uint64_t(e.count) * 12 > e.size) return fail("a mesh blob's positions are cut short");
        mesh.positions.resize(size_t(e.count) * 3);
        for (float& v : mesh.positions) v = s.F32();
      } else if (e.encoding == static_cast<uint32_t>(PositionEncoding::kGrid16)) {
        if (!DecodePositionsGrid16(s, e.count, &mesh.positions)) return fail("a mesh blob's grid positions are cut short");
      } else {
        return fail("a mesh blob's positions have an encoding this build does not know");
      }
      have_positions = true;
    } else if (e.id == kStreamIndices) {
      if (e.count != header.triangle_count * 3u) return fail("a mesh blob's index count is not three a triangle");
      if (e.encoding == static_cast<uint32_t>(IndexEncoding::kRaw32)) {
        if (uint64_t(e.count) * 4 > e.size) return fail("a mesh blob's indices are cut short");
        mesh.indices.resize(e.count);
        for (uint32_t& v : mesh.indices) {
          v = s.U32();
          if (v >= header.vertex_count) return fail("a mesh blob has an index past its vertices");
        }
      } else if (e.encoding == static_cast<uint32_t>(IndexEncoding::kDeltaVarint)) {
        if (!DecodeIndicesDelta(s, e.count, header.vertex_count, &mesh.indices)) return fail("a mesh blob's delta indices are cut short or past its vertices");
      } else {
        return fail("a mesh blob's indices have an encoding this build does not know");
      }
      have_indices = true;
    } else if (e.id == kStreamRuns) {
      if (e.count != header.run_count || uint64_t(e.count) * kRunBytes > e.size) return fail("a mesh blob's runs are cut short");
      mesh.runs.resize(e.count);
      for (GeometryRun& run : mesh.runs) {
        run.first_triangle = s.U32();
        run.triangle_count = s.U32();
        run.material = static_cast<MaterialClass>(s.U8());
        run.flags = s.U8();
        run.shader_index = s.U16();
        run.shader_hash = s.U32();
        if (uint64_t(run.first_triangle) + run.triangle_count > header.triangle_count) return fail("a mesh blob has a run past its triangles");
      }
    }
    // any other id (the attribute streams of a later version) is skipped
  }
  if (header.triangle_count != 0 && (!have_positions || !have_indices)) return fail("a mesh blob has no positions or no indices");
  if (header.triangle_count == 0) {
    mesh.positions.clear();
    mesh.indices.clear();
  }
  *out = std::move(mesh);
  return true;
}

}  // namespace larecomp::citypack
