#include "citypack/sector_parse.h"

#include <algorithm>
#include <unordered_map>

#include "citypack/material_class.h"

namespace larecomp::citypack {

namespace {

constexpr uint32_t kRoot = 0x50000000;
constexpr uint32_t kMaxShaders = 8192;
constexpr uint32_t kMaxModels = 20000;
constexpr uint32_t kMaxGeometriesPerModel = 256;
constexpr uint32_t kMaxStride = 128;
constexpr uint32_t kCityEntryBytes = 304;
constexpr uint32_t kCityModelBytes = 80;

// The shader table: group and preset names, read once.
void ReadShaders(RscCursor& c, uint32_t group_pointer, std::vector<ShaderNames>* out, ParseStats* stats) {
  out->clear();
  if (!c.IsPointer(group_pointer)) return;
  const uint32_t array = c.U32(group_pointer + 8);
  const uint32_t count = std::min<uint32_t>(c.U16(group_pointer + 12), kMaxShaders);
  if (c.bad() || !c.IsPointer(array)) {
    c.ClearBad();
    return;
  }
  out->resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t shader = c.U32(array + 4 * i);
    if (c.bad() || !c.IsPointer(shader)) {
      c.ClearBad();
      continue;
    }
    const uint32_t group = c.U32(shader + 48);
    const uint32_t preset = c.U32(shader + 60);
    if (c.IsPointer(group)) (*out)[i].group = c.CString(group);
    if (c.IsPointer(preset)) (*out)[i].preset = c.CString(preset);
    c.ClearBad();
  }
  stats->shaders = count;
}

// One grmModel's geometries.
void ParseModel(RscCursor& c, uint32_t model, uint32_t model_index, const std::vector<ShaderNames>& shaders, uint32_t bucket, const GeometryCallback& on_geometry,
                ParseStats* stats) {
  const uint32_t geometry_array = c.U32(model + 4);
  const uint32_t geometry_count = c.U16(model + 8);
  const uint32_t shader_indices = c.U32(model + 16);
  if (c.bad() || !c.IsPointer(geometry_array) || geometry_count > kMaxGeometriesPerModel) {
    c.ClearBad();
    ++stats->skipped_bad_range;
    return;
  }
  ++stats->models;
  for (uint32_t j = 0; j < geometry_count; ++j) {
    const uint32_t g = c.U32(geometry_array + 4 * j);
    if (c.bad() || !c.IsPointer(g)) {
      c.ClearBad();
      ++stats->skipped_bad_range;
      continue;
    }
    const uint32_t vb = c.U32(g + 12);
    const uint32_t ib = c.U32(g + 28);
    const uint32_t index_count = c.U32(g + 44);
    const uint32_t primitive_count = c.U32(g + 48);
    const uint16_t vertex_count16 = c.U16(g + 52);
    const uint16_t mode = c.U16(g + 54);
    const uint32_t bones = c.U32(g + 56);
    if (c.bad() || !c.IsPointer(vb) || !c.IsPointer(ib)) {
      c.ClearBad();
      ++stats->skipped_bad_range;
      continue;
    }
    ++stats->geometries;
    stats->triangles += index_count / 3;
    if (bones != 0) ++stats->skinned;
    if (mode != 3) {
      ++stats->skipped_not_triangle_list;
      continue;
    }
    const uint32_t vertex_count = c.U16(vb + 4);
    const uint32_t stride = c.U32(vb + 12);
    const uint32_t declaration = c.U32(vb + 16);
    const uint32_t vertex_data = c.U32(vb + 24);
    const uint32_t index_buffer_count = c.U32(ib + 4);
    const uint32_t index_data = c.U32(ib + 8);
    uint32_t fvf = 1;  // a declaration that cannot be read counts as "position at 0": the range and box checks decide
    if (c.IsPointer(declaration)) fvf = c.U32(declaration);
    if (c.bad() || stride < 12 || stride > kMaxStride) {
      c.ClearBad();
      ++stats->skipped_bad_range;
      continue;
    }
    stats->vertices += vertex_count;
    if (vertex_count16 != vertex_count) ++stats->vertex_count_mismatch;
    if (index_buffer_count != index_count) ++stats->index_count_mismatch;
    if ((fvf & 1u) == 0) {
      ++stats->skipped_no_position;
      continue;
    }
    if (index_count % 3 != 0 || index_count == 0 || vertex_count == 0) {
      ++stats->skipped_bad_range;
      continue;
    }
    const uint8_t* vertices = c.Span(vertex_data, uint64_t(vertex_count) * stride);
    const uint8_t* indices = c.Span(index_data, uint64_t(index_count) * 2);
    if (c.bad() || !vertices || !indices) {
      c.ClearBad();
      ++stats->skipped_bad_range;
      continue;
    }
    (void)primitive_count;
    SourceGeometry geometry;
    geometry.model_index = model_index;
    geometry.geometry_index = j;
    geometry.shader_index = 0xFFFF;
    if (c.IsPointer(shader_indices)) {
      geometry.shader_index = c.U16(shader_indices + 2 * j);
      c.ClearBad();
    }
    if (geometry.shader_index < shaders.size()) {
      geometry.shader = &shaders[geometry.shader_index];
    } else {
      ++stats->unknown_shader_index;
    }
    geometry.vertex_count = vertex_count;
    geometry.stride = stride;
    geometry.index_count = index_count;
    geometry.fvf = fvf;
    geometry.bucket_mask = bucket;
    geometry.vertices = vertices;
    geometry.indices = indices;
    on_geometry(geometry);
  }
}

}  // namespace

bool ParseCityTable(const RscImage& image, std::vector<SectorInfo>* out, std::string* error) {
  RscCursor c(image);
  const uint32_t array = c.U32(kRoot + 20);
  const uint32_t count = c.U16(kRoot + 24);
  if (c.bad() || !c.IsPointer(array) || count == 0 || count > 10000) {
    if (error) *error = "city.xct does not have a sector array where the city table's layout has it";
    return false;
  }
  std::vector<SectorInfo> sectors(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t entry = array + kCityEntryBytes * i;
    SectorInfo& s = sectors[i];
    s.index = i;
    for (int a = 0; a < 3; ++a) {
      s.aabb_min[a] = c.F32(entry + 208 + 4 * uint32_t(a));
      s.aabb_max[a] = c.F32(entry + 224 + 4 * uint32_t(a));
    }
    const uint32_t name = c.U32(entry + 240);
    if (c.bad() || !c.IsPointer(name)) {
      if (error) *error = "a sector entry of city.xct has no name";
      return false;
    }
    s.name = c.CString(name, 64);
    if (s.name.empty() || c.bad()) {
      if (error) *error = "a sector entry of city.xct has an empty name";
      return false;
    }
    s.name_hash = RageHashText(s.name);
  }
  *out = std::move(sectors);
  return true;
}

bool ParseSector(const RscImage& image, std::vector<ShaderNames>* shaders, const GeometryCallback& on_geometry, ParseStats* stats, std::string* error) {
  RscCursor c(image);
  const auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  const uint32_t drawable = c.U32(kRoot + 16);
  if (c.bad() || !c.IsPointer(drawable)) return fail("the sector has no drawable where the layout has it");
  const uint32_t group = c.U32(drawable + 8);
  const uint32_t dictionary = c.U32(drawable + 12);
  if (c.bad() || !c.IsPointer(dictionary)) return fail("the sector has no model dictionary");
  ReadShaders(c, group, shaders, stats);

  const uint32_t hashes = c.U32(dictionary + 16);
  const uint32_t model_count = c.U16(dictionary + 20);
  const uint32_t models = c.U32(dictionary + 24);
  if (c.bad() || !c.IsPointer(models) || model_count == 0 || model_count > kMaxModels) return fail("the sector's model dictionary is empty or out of range");

  // The city model records carry the bucket mask; they are found by the model's name hash.
  std::unordered_map<uint32_t, uint32_t> bucket_by_hash;
  const uint32_t records = c.U32(kRoot + 32);
  const uint32_t record_count = c.U16(kRoot + 36);
  if (!c.bad() && c.IsPointer(records) && record_count <= kMaxModels) {
    bucket_by_hash.reserve(record_count);
    for (uint32_t i = 0; i < record_count; ++i) {
      const uint32_t record = records + kCityModelBytes * i;
      const uint32_t hash = c.U32(record + 76);
      const uint8_t mask = c.U8(record + 74);
      if (c.bad()) {
        c.ClearBad();
        break;
      }
      bucket_by_hash[hash] = mask;
    }
    stats->city_models += record_count;
  }
  c.ClearBad();

  for (uint32_t i = 0; i < model_count; ++i) {
    const uint32_t model = c.U32(models + 4 * i);
    if (c.bad() || !c.IsPointer(model)) {
      c.ClearBad();
      ++stats->null_models;
      continue;
    }
    uint32_t bucket = 0;
    if (c.IsPointer(hashes)) {
      const uint32_t hash = c.U32(hashes + 4 * i);
      const auto it = bucket_by_hash.find(hash);
      if (it != bucket_by_hash.end()) {
        bucket = it->second;
      } else {
        ++stats->models_without_record;
      }
      c.ClearBad();
    }
    ParseModel(c, model, i, *shaders, bucket, on_geometry, stats);
  }
  return true;
}

bool ParseLod(const RscImage& image, std::vector<ShaderNames>* shaders, const GeometryCallback& on_geometry, ParseStats* stats, std::string* error) {
  RscCursor c(image);
  const uint32_t group = c.U32(kRoot + 8);
  const uint32_t models = c.U32(kRoot + 12);
  const uint32_t count = c.U16(kRoot + 16);
  if (c.bad() || !c.IsPointer(models) || count == 0 || count > kMaxModels) {
    if (error) *error = "lod.xmd does not have a model array where the layout has it";
    return false;
  }
  ReadShaders(c, group, shaders, stats);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t model = c.U32(models + 4 * i);
    if (c.bad() || !c.IsPointer(model)) {
      c.ClearBad();
      ++stats->null_models;
      continue;
    }
    ParseModel(c, model, i, *shaders, 0, on_geometry, stats);
  }
  return true;
}

}  // namespace larecomp::citypack
