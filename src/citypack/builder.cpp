#include "citypack/builder.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#include "citypack/material_class.h"
#include "mc_engine/modloader/rpf3.h"

namespace larecomp::citypack {

namespace {

using Clock = std::chrono::steady_clock;
double SecondsSince(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

void AddStats(ParseStats& to, const ParseStats& from) {
  to.shaders = std::max(to.shaders, from.shaders);
  to.models += from.models;
  to.null_models += from.null_models;
  to.geometries += from.geometries;
  to.triangles += from.triangles;
  to.vertices += from.vertices;
  to.skipped_not_triangle_list += from.skipped_not_triangle_list;
  to.skipped_no_position += from.skipped_no_position;
  to.skipped_bad_range += from.skipped_bad_range;
  to.unknown_shader_index += from.unknown_shader_index;
  to.vertex_count_mismatch += from.vertex_count_mismatch;
  to.index_count_mismatch += from.index_count_mismatch;
  to.skinned += from.skinned;
  to.city_models += from.city_models;
  to.models_without_record += from.models_without_record;
}

void AddWeld(WeldStats& to, const WeldStats& from) {
  to.source_vertices += from.source_vertices;
  to.source_triangles += from.source_triangles;
  to.dropped_degenerate += from.dropped_degenerate;
  to.dropped_bad_index += from.dropped_bad_index;
  to.dropped_not_finite += from.dropped_not_finite;
}

// The archives, searched the way the game does: the mod overlay first, then the shipped one.
struct Archives {
  mc::modloader::Rpf3Reader source;
  mc::modloader::Rpf3Reader mods;
  bool has_mods = false;

  bool Read(const std::string& path, std::vector<uint8_t>* file, bool* from_mods) const {
    mc::modloader::Rpf3Entry entry;
    if (has_mods && mods.Find(path, entry) && !entry.is_directory() && mods.ReadFile(entry, *file)) {
      if (from_mods) *from_mods = true;
      return true;
    }
    if (from_mods) *from_mods = false;
    return source.Find(path, entry) && !entry.is_directory() && source.ReadFile(entry, *file);
  }
};

// What a worker hands the writer.
struct SectorResult {
  bool missing = false;
  bool failed = false;
  bool from_mods = false;
  std::vector<uint8_t> hi_blob;
  SectorRecord record;
  Census census;
};

bool OutsideBox(const float p[3], const SectorInfo& s) {
  for (int a = 0; a < 3; ++a) {
    if (p[a] < s.aabb_min[a] - kBoxMargin || p[a] > s.aabb_max[a] + kBoxMargin) return true;
  }
  return false;
}

// Adds one source geometry to `builder` and the census; shared by the sectors and the lod.
struct GeometrySink {
  const BuildRequest& request;
  Census& census;
  bool lod = false;

  void Take(const SourceGeometry& g, MeshBuilder* builder, const SectorInfo* box_of) {
    static const ShaderNames kNoNames;
    const ShaderNames& names = g.shader ? *g.shader : kNoNames;
    const MaterialClass cls = ClassifyShader(names.group, names.preset);
    const uint64_t triangles = g.index_count / 3;
    ShaderCensusRow& row = census.shaders[names.group + "/" + names.preset];
    row.material = cls;
    row.geometries += 1;
    row.triangles += triangles;
    if (!lod) {
      census.triangles_by_class[size_t(cls)] += triangles;
      census.by_bucket[g.bucket_mask][size_t(cls)] += triangles;
      census.strides[g.stride] += 1;
    }
    if ((request.drop_mask & ClassBit(cls)) != 0) {
      if (!lod) census.dropped_by_class[size_t(cls)] += triangles;
      return;
    }
    std::vector<float> positions(size_t(g.vertex_count) * 3);
    uint64_t outside = 0;
    for (uint32_t v = 0; v < g.vertex_count; ++v) {
      SourcePosition(g, v, &positions[size_t(v) * 3]);
      if (box_of && OutsideBox(&positions[size_t(v) * 3], *box_of)) ++outside;
    }
    if (outside != 0) {
      census.vertices_outside_box += outside;
      census.geometries_outside_box += 1;
    }
    std::vector<uint16_t> indices(g.index_count);
    for (uint32_t k = 0; k < g.index_count; ++k) indices[k] = LoadBE16(g.indices + size_t(k) * 2);
    builder->AddGeometry(positions.data(), g.vertex_count, indices.data(), g.index_count, cls, static_cast<uint16_t>(g.shader_index),
                         ShaderHash(names.group, names.preset));
  }
};

SectorResult ProcessSector(const Archives& archives, const SectorInfo& sector, const BuildRequest& request) {
  SectorResult result;
  result.census.sectors = 1;
  const std::string path = SectorArchivePath(sector.name);

  auto t = Clock::now();
  std::vector<uint8_t> file;
  if (!archives.Read(path, &file, &result.from_mods)) {
    result.missing = true;
    result.census.sectors_without_file = 1;
    result.census.problems.push_back(sector.name + ": not in the archive (" + path + ")");
    return result;
  }
  result.census.read_seconds += SecondsSince(t);

  t = Clock::now();
  RscImage image;
  std::string error;
  if (!DecodeResourceFile(file, request.lzx, &image, &error)) {
    result.failed = true;
    result.census.sectors_failed = 1;
    result.census.problems.push_back(sector.name + ": " + error);
    return result;
  }
  file.clear();
  file.shrink_to_fit();
  result.census.decode_seconds += SecondsSince(t);

  t = Clock::now();
  MeshBuilder builder;
  GeometrySink sink{request, result.census, false};
  std::vector<ShaderNames> shaders;
  ParseStats stats;
  const bool parsed = ParseSector(
      image, &shaders, [&](const SourceGeometry& g) { sink.Take(g, &builder, &sector); }, &stats, &error);
  if (!parsed) {
    result.failed = true;
    result.census.sectors_failed = 1;
    result.census.problems.push_back(sector.name + ": " + error);
    return result;
  }
  AddStats(result.census.hi, stats);
  AddWeld(result.census.hi_weld, builder.stats());
  MeshData mesh = builder.Finish();
  result.census.parse_seconds += SecondsSince(t);

  t = Clock::now();
  uint64_t dropped = 0;
  for (const uint64_t d : result.census.dropped_by_class) dropped += d;
  result.record.dropped_triangles = static_cast<uint32_t>(dropped);
  if (!mesh.empty()) {
    result.record.flags |= kSectorHasHi;
    result.record.hi_triangles = mesh.triangle_count();
    result.record.hi_vertices = mesh.vertex_count();
    result.record.hi_opaque_triangles = mesh.opaque_triangles;
    std::copy(std::begin(mesh.bounds_min), std::end(mesh.bounds_min), result.record.hi_min);
    std::copy(std::begin(mesh.bounds_max), std::end(mesh.bounds_max), result.record.hi_max);
    result.census.sectors_built = 1;
    result.census.hi_kept_triangles = mesh.triangle_count();
    result.census.hi_kept_vertices = mesh.vertex_count();
    result.census.hi_kept_alpha_triangles = mesh.alpha_triangles();
    if (mesh.triangle_count() > result.census.max_sector_triangles) {
      result.census.max_sector_triangles = mesh.triangle_count();
      result.census.max_sector_name = sector.name;
    }
    if (!request.census_only) {
      result.hi_blob = EncodeMesh(mesh, request.positions, IndexEncoding::kDeltaVarint);
      result.record.hi_size = static_cast<uint32_t>(result.hi_blob.size());
      result.census.hi_blob_bytes = result.hi_blob.size();
    }
  }
  if (result.from_mods) result.record.flags |= kSectorFromMods;
  result.census.encode_seconds += SecondsSince(t);
  return result;
}

}  // namespace

std::string SectorArchivePath(std::string_view sector_name) {
  std::string path = "resources/city/sc/";
  path += sector_name;
  path += ".xcs";
  return path;
}

// ---------------------------------------------------------------------------
// the census
// ---------------------------------------------------------------------------

void Census::Merge(const Census& o) {
  sectors += o.sectors;
  sectors_built += o.sectors_built;
  sectors_without_file += o.sectors_without_file;
  sectors_failed += o.sectors_failed;
  AddStats(hi, o.hi);
  AddStats(lod, o.lod);
  AddWeld(hi_weld, o.hi_weld);
  AddWeld(lod_weld, o.lod_weld);
  hi_kept_triangles += o.hi_kept_triangles;
  hi_kept_vertices += o.hi_kept_vertices;
  hi_kept_alpha_triangles += o.hi_kept_alpha_triangles;
  lod_kept_triangles += o.lod_kept_triangles;
  lod_kept_vertices += o.lod_kept_vertices;
  for (size_t i = 0; i < triangles_by_class.size(); ++i) {
    triangles_by_class[i] += o.triangles_by_class[i];
    dropped_by_class[i] += o.dropped_by_class[i];
  }
  for (const auto& [key, row] : o.shaders) {
    ShaderCensusRow& mine = shaders[key];
    mine.material = row.material;
    mine.geometries += row.geometries;
    mine.triangles += row.triangles;
  }
  for (const auto& [bucket, counts] : o.by_bucket) {
    auto& mine = by_bucket[bucket];
    for (size_t i = 0; i < counts.size(); ++i) mine[i] += counts[i];
  }
  for (const auto& [stride, count] : o.strides) strides[stride] += count;
  vertices_outside_box += o.vertices_outside_box;
  geometries_outside_box += o.geometries_outside_box;
  lod_models_outside_box += o.lod_models_outside_box;
  lod_models_checked += o.lod_models_checked;
  lod_best_is_index += o.lod_best_is_index;
  if (o.max_sector_triangles > max_sector_triangles) {
    max_sector_triangles = o.max_sector_triangles;
    max_sector_name = o.max_sector_name;
  }
  hi_blob_bytes += o.hi_blob_bytes;
  lod_blob_bytes += o.lod_blob_bytes;
  read_seconds += o.read_seconds;
  decode_seconds += o.decode_seconds;
  parse_seconds += o.parse_seconds;
  encode_seconds += o.encode_seconds;
  for (const std::string& p : o.problems) {
    if (problems.size() < 40) problems.push_back(p);
  }
}

std::string Census::Report() const {
  std::ostringstream out;
  const auto golden = [&](const char* what, uint64_t have, uint64_t want) {
    out << "  " << what << ": " << have << " (the survey's " << want << (have == want ? ", equal" : ", DIFFERENT") << ")\n";
  };
  out << "city pack census\n";
  out << "sectors: " << sectors << " in the table, " << sectors_built << " with a hi mesh, " << sectors_without_file << " without a file, " << sectors_failed << " that did not parse\n";
  out << "hi detail sources (all classes, before the drop mask):\n";
  golden("sectors", sectors, kGoldenSectors);
  golden("geometries", hi.geometries, kGoldenHiGeometries);
  golden("triangles", hi.triangles, kGoldenHiTriangles);
  golden("vertices (the buffers', seams repeated)", hi.vertices, kGoldenHiVertices);
  out << "  skipped geometries: not a triangle list " << hi.skipped_not_triangle_list << ", no position at 0 " << hi.skipped_no_position << ", bad range " << hi.skipped_bad_range
      << "; vertex count mismatches " << hi.vertex_count_mismatch << ", index count mismatches " << hi.index_count_mismatch << ", skinned " << hi.skinned
      << ", unknown shader index " << hi.unknown_shader_index << ", models without a record " << hi.models_without_record << ", null models " << hi.null_models << "\n";
  out << "  vertices outside their sector's box by more than " << kBoxMargin << " m: " << vertices_outside_box << " in " << geometries_outside_box << " geometries\n";
  out << "far lod (lod.xmd):\n";
  golden("triangles", lod.triangles, kGoldenLodTriangles);
  golden("vertices", lod.vertices, kGoldenLodVertices);
  out << "  models " << lod.models << ", null " << lod.null_models << "; models whose bounds are outside their sector's box (by the index): " << lod_models_outside_box << " of "
      << lod_models_checked << "; the sector a model overlaps most is its own (by index) for " << lod_best_is_index << " of them\n";
  out << "what the pack keeps:\n";
  out << "  hi: " << hi_kept_triangles << " triangles (" << hi_kept_alpha_triangles << " alpha tested), " << hi_kept_vertices << " welded vertices ("
      << (hi.vertices ? 100.0 * double(hi_kept_vertices) / double(hi.vertices) : 0.0) << "% of the sources'), weld dropped " << hi_weld.dropped_degenerate << " degenerate, "
      << hi_weld.dropped_bad_index << " bad index, " << hi_weld.dropped_not_finite << " not finite triangles\n";
  out << "  lod: " << lod_kept_triangles << " triangles, " << lod_kept_vertices << " vertices; weld dropped " << lod_weld.dropped_degenerate << " degenerate\n";
  out << "  largest sector: " << max_sector_name << " with " << max_sector_triangles << " triangles\n";
  if (hi_blob_bytes != 0) out << "  blobs: hi " << hi_blob_bytes << " bytes, lod " << lod_blob_bytes << " bytes\n";
  out << "classes (hi sources' triangles; dropped = left out by the mask):\n";
  for (size_t c = 0; c < triangles_by_class.size(); ++c) {
    out << "  " << ClassName(static_cast<MaterialClass>(c)) << ": " << triangles_by_class[c] << " (" << (hi.triangles ? 100.0 * double(triangles_by_class[c]) / double(hi.triangles) : 0.0)
        << "%), dropped " << dropped_by_class[c] << "\n";
  }
  out << "bucket masks (the city model record's byte, by class; CHECK C2):\n";
  for (const auto& [bucket, counts] : by_bucket) {
    out << "  mask " << bucket << ":";
    for (size_t c = 0; c < counts.size(); ++c) {
      if (counts[c] != 0) out << " " << ClassName(static_cast<MaterialClass>(c)) << "=" << counts[c];
    }
    out << "\n";
  }
  out << "vertex strides (geometries):";
  for (const auto& [stride, count] : strides) out << " " << stride << "=" << count;
  out << "\n";
  out << "shaders (group/preset: class, geometries, triangles; hi and lod together), " << shaders.size() << " of them:\n";
  std::vector<std::pair<std::string, ShaderCensusRow>> rows(shaders.begin(), shaders.end());
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.triangles > b.second.triangles; });
  for (const auto& [key, row] : rows) out << "  " << key << ": " << ClassName(row.material) << ", " << row.geometries << ", " << row.triangles << "\n";
  out << "time summed over the workers: read " << read_seconds << " s, LZX decompress " << decode_seconds << " s, parse and weld " << parse_seconds << " s, encode " << encode_seconds
      << " s\n";
  if (!problems.empty()) {
    out << "problems (first " << problems.size() << "):\n";
    for (const std::string& p : problems) out << "  " << p << "\n";
  }
  return out.str();
}

// ---------------------------------------------------------------------------
// the build
// ---------------------------------------------------------------------------

uint64_t CityOverlayHash(const std::filesystem::path& mods_rpf) {
  if (mods_rpf.empty() || !StatFile(mods_rpf).exists) return 0;
  constexpr uint64_t kUnreadable = 0x0BADC17A0BADC17Aull;
  mc::modloader::Rpf3Reader mods;
  if (!mods.Open(mods_rpf)) return kUnreadable;
  std::vector<mc::modloader::Rpf3Entry> entries;
  if (!mods.ListDirectory("resources/city/sc", entries) || entries.empty()) return 0;
  uint64_t hash = 0xCBF29CE484222325ull;
  std::vector<uint8_t> bytes;
  bool any = false;
  for (const mc::modloader::Rpf3Entry& entry : entries) {  // table order: ascending hash, the same on every PC
    if (entry.is_directory()) continue;
    any = true;
    hash = Fnv1a64(&entry.hash, sizeof(entry.hash), hash);
    hash = Fnv1a64(&entry.size, sizeof(entry.size), hash);
    hash = Fnv1a64(&entry.flag, sizeof(entry.flag), hash);
    if (mods.ReadFile(entry, bytes)) {
      hash = Fnv1a64(bytes.data(), bytes.size(), hash);
    } else {
      hash = Fnv1a64(&kUnreadable, sizeof(kUnreadable), hash);
    }
  }
  return any ? (hash == 0 ? 1 : hash) : 0;
}

StampInputs StampInputsFor(const BuildRequest& request) {
  StampInputs in;
  in.source = StatFile(request.source_rpf);
  if (!request.mods_rpf.empty()) {
    in.mods = StatFile(request.mods_rpf);
    in.city_overlay = CityOverlayHash(request.mods_rpf);
  }
  in.drop_mask = request.drop_mask;
  in.positions = static_cast<uint32_t>(request.positions);
  in.app_version = request.app_version;
  return in;
}

StampCheck CheckCityPack(const BuildRequest& request) { return CheckStamp(request.out_dir, StampInputsFor(request)); }

BuildResult BuildCityPack(const BuildRequest& request, ProgressSink* sink_in) {
  ProgressSink fallback;
  ProgressSink& sink = sink_in ? *sink_in : fallback;
  BuildResult result;
  const auto started = Clock::now();
  result.pack_path = request.out_dir / kPackFileName;
  const auto fail = [&](std::string why) {
    result.ok = false;
    result.error = std::move(why);
    result.seconds = SecondsSince(started);
    sink.Log(LogLevel::kError, result.error);
    return result;
  };
  if (!request.lzx) return fail("no LZX decoder was given to the city pack builder");
  if (!StatFile(request.source_rpf).exists) return fail("the game archive " + request.source_rpf.string() + " is not there");

  if (!request.force && !request.census_only) {
    const StampCheck check = CheckCityPack(request);
    if (check.current) {
      result.ok = true;
      result.up_to_date = true;
      result.seconds = SecondsSince(started);
      sink.Log(LogLevel::kInfo, "the city pack is current: nothing to build");
      return result;
    }
    sink.Log(LogLevel::kInfo, "the city pack is built: " + check.why);
  }

  // What the stamp will say, taken BEFORE the archives are read: a mod archive that changes during the build leaves a stamp that is not current (the pack is built again)
  // and never one that claims inputs the pack was not made from.
  const StampInputs stamp_base = StampInputsFor(request);

  Archives archives;
  if (!archives.source.Open(request.source_rpf)) return fail("the game archive " + request.source_rpf.string() + " could not be opened as an RPF archive");
  if (!request.mods_rpf.empty() && StatFile(request.mods_rpf).exists) archives.has_mods = archives.mods.Open(request.mods_rpf);

  // The table, and the far lod.
  std::vector<uint8_t> file;
  std::string error;
  RscImage image;
  if (!archives.Read(kCityTablePath, &file, nullptr)) return fail("city.xct is not in the archive");
  if (!DecodeResourceFile(file, request.lzx, &image, &error)) return fail(std::string("city.xct: ") + error);
  std::vector<SectorInfo> sectors;
  if (!ParseCityTable(image, &sectors, &error)) return fail(error);
  const uint32_t count = static_cast<uint32_t>(sectors.size());

  Census census;
  std::vector<std::vector<uint8_t>> lod_blobs(count);
  std::vector<uint32_t> lod_triangles(count, 0), lod_vertices(count, 0);
  std::vector<std::array<float, 6>> lod_bounds(count);
  {
    sink.BeginPhase("far level of detail", 1);
    if (!archives.Read(kLodPath, &file, nullptr)) return fail("lod.xmd is not in the archive");
    if (!DecodeResourceFile(file, request.lzx, &image, &error)) return fail(std::string("lod.xmd: ") + error);
    std::vector<MeshBuilder> builders(count);
    std::vector<ShaderNames> shaders;
    ParseStats stats;
    GeometrySink sink_lod{request, census, true};
    if (!ParseLod(
            image, &shaders,
            [&](const SourceGeometry& g) {
              if (g.model_index < count) sink_lod.Take(g, &builders[g.model_index], nullptr);
            },
            &stats, &error))
      return fail(error);
    AddStats(census.lod, stats);
    for (uint32_t i = 0; i < count; ++i) {
      AddWeld(census.lod_weld, builders[i].stats());
      MeshData mesh = builders[i].Finish();
      if (mesh.empty()) continue;
      ++census.lod_models_checked;
      for (int a = 0; a < 3; ++a) {
        if (mesh.bounds_min[a] < sectors[i].aabb_min[a] - kBoxMargin || mesh.bounds_max[a] > sectors[i].aabb_max[a] + kBoxMargin) {
          ++census.lod_models_outside_box;
          break;
        }
      }
      {
        // C5: is sector i the sector whose box this model overlaps most (volume of the overlap over the model's own, so a model larger than its box still counts)?
        double best = -1.0;
        uint32_t best_sector = i;
        for (uint32_t j = 0; j < count; ++j) {
          double volume = 1.0;
          for (int a = 0; a < 3; ++a) {
            const double lo = std::max<double>(mesh.bounds_min[a], sectors[j].aabb_min[a]);
            const double hi = std::min<double>(mesh.bounds_max[a], sectors[j].aabb_max[a]);
            volume *= std::max(0.0, hi - lo + 1e-3);
          }
          if (volume > best) {
            best = volume;
            best_sector = j;
          }
        }
        if (best_sector == i) ++census.lod_best_is_index;
      }
      lod_triangles[i] = mesh.triangle_count();
      lod_vertices[i] = mesh.vertex_count();
      for (int a = 0; a < 3; ++a) {
        lod_bounds[i][size_t(a)] = mesh.bounds_min[a];
        lod_bounds[i][size_t(a) + 3] = mesh.bounds_max[a];
      }
      census.lod_kept_triangles += mesh.triangle_count();
      census.lod_kept_vertices += mesh.vertex_count();
      if (!request.census_only) {
        lod_blobs[i] = EncodeMesh(mesh, request.positions, IndexEncoding::kDeltaVarint);
        census.lod_blob_bytes += lod_blobs[i].size();
      }
    }
    sink.Step("lod.xmd");
  }
  if (sink.Cancelled()) {
    result.cancelled = true;
    result.seconds = SecondsSince(started);
    return result;
  }

  // The names, the table's size, and so where the blobs start.
  std::string names;
  std::vector<SectorRecord> records(count);
  for (uint32_t i = 0; i < count; ++i) {
    records[i].name_hash = sectors[i].name_hash;
    records[i].name_offset = static_cast<uint32_t>(names.size());
    records[i].name_length = static_cast<uint32_t>(sectors[i].name.size());
    names += sectors[i].name;
    std::copy(std::begin(sectors[i].aabb_min), std::end(sectors[i].aabb_min), records[i].aabb_min);
    std::copy(std::begin(sectors[i].aabb_max), std::end(sectors[i].aabb_max), records[i].aabb_max);
  }
  PackHeader header;
  header.sector_count = count;
  header.names_bytes = static_cast<uint32_t>(names.size());
  header.drop_mask = request.drop_mask;
  header.position_encoding = static_cast<uint32_t>(request.positions);
  header.table_offset = kHeaderBytes;
  header.names_offset = header.table_offset + uint64_t(count) * kSectorRecordBytes;
  header.blobs_offset = (header.names_offset + names.size() + 15) & ~uint64_t(15);

  std::filesystem::path temp_path = result.pack_path;
  temp_path += ".tmp";
  std::ofstream out;
  if (!request.census_only) {
    std::error_code ec;
    std::filesystem::create_directories(request.out_dir, ec);
    out.open(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) return fail("the pack file " + temp_path.string() + " cannot be written");
    std::vector<char> zeros(size_t(header.blobs_offset), 0);
    out.write(zeros.data(), std::streamsize(zeros.size()));
  }
  uint64_t position = header.blobs_offset;
  const auto write_blob = [&](const std::vector<uint8_t>& blob, uint64_t* offset) {
    const uint64_t aligned = (position + 15) & ~uint64_t(15);
    if (aligned != position) {
      const char zeros[16] = {};
      out.write(zeros, std::streamsize(aligned - position));
    }
    *offset = aligned;
    out.write(reinterpret_cast<const char*>(blob.data()), std::streamsize(blob.size()));
    position = aligned + blob.size();
  };

  // The sectors: workers parse, this thread writes in the table's order.
  const uint32_t hardware = std::max(1u, std::thread::hardware_concurrency());
  const uint32_t threads = std::clamp(request.threads != 0 ? request.threads : (hardware > 1 ? hardware - 1 : 1), 1u, 64u);
  sink.BeginPhase("city sectors", int(count));
  std::vector<SectorResult> results(count);
  std::vector<char> done(count, 0);
  std::mutex mutex;
  std::condition_variable cv_done, cv_space;
  std::atomic<uint32_t> next{0};
  std::atomic<bool> stop{false};
  uint32_t written = 0;
  const uint32_t window = threads * 2 + 4;
  std::vector<std::thread> workers;
  for (uint32_t w = 0; w < threads; ++w) {
    workers.emplace_back([&] {
      if (request.worker_start) request.worker_start();
      for (;;) {
        const uint32_t i = next.fetch_add(1);
        if (i >= count || stop.load()) return;
        {
          std::unique_lock lock(mutex);
          cv_space.wait(lock, [&] { return stop.load() || i < written + window; });
          if (stop.load()) return;
        }
        SectorResult r = ProcessSector(archives, sectors[i], request);
        {
          std::lock_guard lock(mutex);
          results[i] = std::move(r);
          done[i] = 1;
        }
        cv_done.notify_all();
      }
    });
  }
  bool write_failed = false;
  for (uint32_t i = 0; i < count; ++i) {
    {
      std::unique_lock lock(mutex);
      cv_done.wait(lock, [&] { return done[i] != 0; });
    }
    SectorResult r = std::move(results[i]);
    results[i] = SectorResult{};
    SectorRecord& rec = records[i];
    // the worker's record carries the hi fields; the table's own (name, box) stay
    rec.flags = r.record.flags;
    std::copy(std::begin(r.record.hi_min), std::end(r.record.hi_min), rec.hi_min);
    std::copy(std::begin(r.record.hi_max), std::end(r.record.hi_max), rec.hi_max);
    rec.hi_size = r.record.hi_size;
    rec.hi_triangles = r.record.hi_triangles;
    rec.hi_vertices = r.record.hi_vertices;
    rec.hi_opaque_triangles = r.record.hi_opaque_triangles;
    rec.dropped_triangles = r.record.dropped_triangles;
    census.Merge(r.census);
    if (!request.census_only) {
      if (!r.hi_blob.empty()) write_blob(r.hi_blob, &rec.hi_offset);
      if (!lod_blobs[i].empty()) {
        rec.flags |= kSectorHasLod;
        write_blob(lod_blobs[i], &rec.lod_offset);
        rec.lod_size = static_cast<uint32_t>(lod_blobs[i].size());
        rec.lod_triangles = lod_triangles[i];
        rec.lod_vertices = lod_vertices[i];
        for (int a = 0; a < 3; ++a) {
          rec.lod_min[a] = lod_bounds[i][size_t(a)];
          rec.lod_max[a] = lod_bounds[i][size_t(a) + 3];
        }
        std::vector<uint8_t>().swap(lod_blobs[i]);
      }
      if (!out) write_failed = true;
    } else if (lod_triangles[i] != 0) {
      rec.flags |= kSectorHasLod;
    }
    {
      std::lock_guard lock(mutex);
      written = i + 1;
    }
    cv_space.notify_all();
    sink.Step(sectors[i].name);
    if (write_failed || sink.Cancelled()) {
      stop.store(true);
      cv_space.notify_all();
      result.cancelled = !write_failed;
      break;
    }
  }
  stop.store(true);
  cv_space.notify_all();
  for (std::thread& t : workers) t.join();
  if (write_failed) {
    out.close();
    std::error_code ec;
    std::filesystem::remove(temp_path, ec);
    return fail("the pack file could not be written completely (disk full?)");
  }
  if (result.cancelled) {
    out.close();
    std::error_code ec;
    std::filesystem::remove(temp_path, ec);
    result.seconds = SecondsSince(started);
    return result;
  }

  result.census = std::move(census);
  if (request.census_only) {
    result.ok = true;
    result.seconds = SecondsSince(started);
    return result;
  }

  // The head: the header, the table, the names.
  header.file_size = position;
  ByteWriter table;
  for (const SectorRecord& r : records) r.Write(table);
  header.table_hash = Fnv1a64(table.bytes().data(), table.size());
  header.table_hash = Fnv1a64(names.data(), names.size(), header.table_hash);
  ByteWriter head;
  header.Write(head);
  head.Bytes(table.bytes().data(), table.size());
  head.Bytes(names.data(), names.size());
  out.seekp(0);
  out.write(reinterpret_cast<const char*>(head.bytes().data()), std::streamsize(head.size()));
  out.flush();
  if (!out) {
    out.close();
    std::error_code ec;
    std::filesystem::remove(temp_path, ec);
    return fail("the pack's table could not be written");
  }
  out.close();

  std::error_code ec;
  std::filesystem::remove(result.pack_path, ec);
  std::filesystem::rename(temp_path, result.pack_path, ec);
  if (ec) return fail("the pack could not be moved into place: " + ec.message());

  StampInputs stamp_in = stamp_base;
  stamp_in.pack_size = position;
  stamp_in.pack_hash = header.table_hash;
  stamp_in.sectors = count;
  stamp_in.hi_triangles = result.census.hi_kept_triangles;
  stamp_in.lod_triangles = result.census.lod_kept_triangles;
  std::string stamp_error;
  if (!MakeStamp(stamp_in).Write(request.out_dir / kStampFileName, &stamp_error)) return fail(stamp_error);

  result.ok = true;
  result.pack_bytes = position;
  result.pack_hash = header.table_hash;
  result.seconds = SecondsSince(started);
  sink.Log(LogLevel::kInfo, "city pack built: " + std::to_string(count) + " sectors, " + std::to_string(position) + " bytes in " + std::to_string(result.seconds) + " s");
  return result;
}

}  // namespace larecomp::citypack
