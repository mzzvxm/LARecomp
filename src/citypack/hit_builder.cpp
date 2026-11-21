#include "citypack/hit_builder.h"

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

#include "citypack/hit_params.h"
#include "citypack/material_class.h"
#include "citypack/stamp.h"
#include "mc_engine/modloader/rpf3.h"

namespace larecomp::citypack {

namespace gb = mcla::gpu::gbuffer;

namespace {

using Clock = std::chrono::steady_clock;
double SecondsSince(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

constexpr const char* kBodySuffix = ".body";
constexpr uint32_t kAnyOpaque = 0xFFFFFFFFu;  // HitMesh::Finish: the opaque count is not compared (the lod's record has none)

// The archives, searched the way the game does: the mod overlay first, then the shipped one (builder.cpp has the same).
struct Archives {
  mc::modloader::Rpf3Reader source;
  mc::modloader::Rpf3Reader mods;
  bool has_mods = false;

  bool Read(const std::string& path, std::vector<uint8_t>* file) const {
    mc::modloader::Rpf3Entry entry;
    if (has_mods && mods.Find(path, entry) && !entry.is_directory() && mods.ReadFile(entry, *file)) return true;
    return source.Find(path, entry) && !entry.is_directory() && source.ReadFile(entry, *file);
  }
};

// One mesh being read again: the welded mesh (it must come out as the pack's), the corner collector, and the materials the mesh's geometries name, by LOCAL number.
struct HitMesh {
  explicit HitMesh(const HitRules& rules = HitRules()) : hit(rules) {}
  MeshBuilder mesh;
  HitCollector hit;
  std::vector<MaterialDef> locals;
  std::vector<MaterialFacts> facts;
  std::vector<int32_t> local_of_shader;
  int32_t default_local = -1;

  uint32_t LocalOf(const SourceGeometry& g, MaterialClass cls) {
    if (g.shader == nullptr) {
      if (default_local < 0) {
        default_local = int32_t(locals.size());
        locals.emplace_back();
        facts.emplace_back();
      }
      return uint32_t(default_local);
    }
    if (g.shader_index >= local_of_shader.size()) local_of_shader.resize(size_t(g.shader_index) + 1, -1);
    int32_t& slot = local_of_shader[g.shader_index];
    if (slot < 0) {
      MaterialFacts f;
      locals.push_back(MaterialFromShader(g.shader, cls, &f));
      facts.push_back(f);
      slot = int32_t(locals.size() - 1);
    }
    return uint32_t(slot);
  }

  // Adds a source geometry the way the pack's builder does (the same class, the same drop, the same weld), and its corners to the collector.
  void Take(const SourceGeometry& g, uint32_t drop_mask) {
    static const ShaderNames kNoNames;
    const ShaderNames& names = g.shader ? *g.shader : kNoNames;
    const MaterialClass cls = ClassifyShader(names.group, names.preset);
    if ((drop_mask & ClassBit(cls)) != 0) return;
    std::vector<float> positions(size_t(g.vertex_count) * 3);
    for (uint32_t v = 0; v < g.vertex_count; ++v) SourcePosition(g, v, &positions[size_t(v) * 3]);
    std::vector<uint16_t> indices(g.index_count);
    for (uint32_t k = 0; k < g.index_count; ++k) indices[k] = LoadBE16(g.indices + size_t(k) * 2);
    std::vector<uint32_t> kept;
    mesh.AddGeometry(positions.data(), g.vertex_count, indices.data(), g.index_count, cls, static_cast<uint16_t>(g.shader_index), ShaderHash(names.group, names.preset), &kept);
    if (kept.empty()) return;
    hit.AddGeometry(g, !IsOpaqueClass(cls), kept, LocalOf(g, cls));
  }

  // The finished blob against the pack's record. `what` names the mesh in the problem.
  bool Finish(uint32_t expect_triangles, uint32_t expect_opaque, bool has_expected, HitBlob* blob, bool* has_blob, std::string* problem, const char* what) {
    MeshData m = mesh.Finish();
    *has_blob = false;
    if (m.empty()) {
      if (has_expected && expect_triangles != 0) {
        *problem = std::string(what) + ": the base pack has " + std::to_string(expect_triangles) + " triangles here and the hit pass found none";
        return false;
      }
      return true;
    }
    if (!has_expected || m.triangle_count() != expect_triangles || (expect_opaque != kAnyOpaque && m.opaque_triangles != expect_opaque)) {
      *problem = std::string(what) + ": the hit pass has " + std::to_string(m.triangle_count()) + " triangles (" + std::to_string(m.opaque_triangles) + " opaque), the base pack " +
                 std::to_string(has_expected ? expect_triangles : 0) + " (" + std::to_string(has_expected ? expect_opaque : 0) + "): the base pack is not of these files";
      return false;
    }
    std::string error;
    if (!hit.Finish(m.opaque_triangles, m.triangle_count(), blob, &error)) {
      *problem = std::string(what) + ": " + error;
      return false;
    }
    *has_blob = true;
    return true;
  }
};

struct HitSectorResult {
  bool failed = false;
  bool has_blob = false;
  HitBlob blob;                          // the runs (local material numbers); its corners are moved into `corner_stream`
  std::vector<uint8_t> corner_stream;    // EncodeIndexedCorners of the blob's corners (empty: the blob has none)
  uint64_t corner_count = 0;
  std::vector<MaterialDef> locals;
  std::vector<MaterialFacts> facts;
  CollectStats stats;
  uint32_t shaders = 0;
  double read_seconds = 0.0;
  double parse_seconds = 0.0;
  std::string problem;
};

// The blob's corners into the indexed stream; the words are dropped (they are the biggest thing a worker holds).
void CodeCorners(HitBlob* blob, std::vector<uint8_t>* stream, uint64_t* corner_count) {
  *corner_count = blob->corners.size() / 2;
  stream->clear();
  if (!blob->has_corners()) return;
  *stream = EncodeIndexedCorners(blob->corners.data(), blob->corners.size() / 2);
  std::vector<uint32_t>().swap(blob->corners);
}

HitSectorResult ProcessHitSector(const Archives& archives, const SectorInfo& sector, const SectorRecord& base, const BuildRequest& request, uint32_t drop_mask,
                                 const HitRules& rules) {
  HitSectorResult result;
  auto t = Clock::now();
  std::vector<uint8_t> file;
  if (!archives.Read(SectorArchivePath(sector.name), &file)) {
    if (base.has_hi()) {
      result.failed = true;
      result.problem = sector.name + ": the game archive does not have the sector the base pack was built from";
    }
    return result;
  }
  result.read_seconds += SecondsSince(t);
  t = Clock::now();
  RscImage image;
  std::string error;
  if (!DecodeResourceFile(file, request.lzx, &image, &error)) {
    result.failed = true;
    result.problem = sector.name + ": " + error;
    return result;
  }
  file.clear();
  file.shrink_to_fit();
  result.read_seconds += SecondsSince(t);

  t = Clock::now();
  HitMesh mesh(rules);
  std::vector<ShaderNames> shaders;
  ParseStats stats;
  if (!ParseSector(
          image, &shaders, [&](const SourceGeometry& g) { mesh.Take(g, drop_mask); }, &stats, &error, true)) {
    result.failed = true;
    result.problem = sector.name + ": " + error;
    return result;
  }
  result.shaders = uint32_t(shaders.size());
  if (!mesh.Finish(base.hi_triangles, base.hi_opaque_triangles, base.has_hi(), &result.blob, &result.has_blob, &result.problem, sector.name.c_str())) {
    result.failed = true;
    return result;
  }
  result.locals = std::move(mesh.locals);
  result.facts = std::move(mesh.facts);
  result.stats = mesh.hit.stats();
  if (result.has_blob) CodeCorners(&result.blob, &result.corner_stream, &result.corner_count);
  result.parse_seconds += SecondsSince(t);
  return result;
}

// The temporary file and the final one are written through this: a failed write is remembered, never ignored.
bool WriteAll(std::ofstream& out, const void* data, size_t size) {
  out.write(static_cast<const char*>(data), std::streamsize(size));
  return bool(out);
}

// ---------------------------------------------------------------------------
// the stamp
// ---------------------------------------------------------------------------

struct HitStampInputs {
  StampInputs base_inputs;       // the game archive's stat, the mods' city files
  uint64_t base_pack_hash = 0;
  uint64_t base_pack_size = 0;
  uint32_t drop_mask = 0;
  std::string texture_set;
  uint64_t rows_hash = 0;
  uint64_t rules = 0;
  uint64_t hit_size = 0;
  uint64_t hit_hash = 0;
  uint64_t tex_size = 0;
  uint32_t sectors = 0;
  uint32_t materials = 0;
  uint32_t textures = 0;
  uint64_t runs = 0;
  std::string app_version;
};

Stamp MakeHitStamp(const HitStampInputs& in) {
  Stamp stamp;
  stamp.Set("stamp_format", uint64_t(kHitStampFormat));
  stamp.Set("hit_builder_version", uint64_t(kHitBuilderVersion));
  stamp.Set("hit_format_version", uint64_t(kHitFormatVersion));
  stamp.Set("builder_revision", uint64_t(kHitBuilderRevision));
  stamp.Set("corner_stream_version", uint64_t(kIndexedCornersVersion));
  stamp.Set("rules", in.rules);
  stamp.Set("material_rules_version", uint64_t(gb::kMaterialRulesVersion));
  stamp.SetHex("base_pack_hash", in.base_pack_hash);
  stamp.Set("base_pack_size", in.base_pack_size);
  stamp.Set("source_size", in.base_inputs.source.exists ? in.base_inputs.source.size : 0);
  stamp.Set("source_mtime_us", in.base_inputs.source.exists ? std::to_string(in.base_inputs.source.mtime_us) : std::string("0"));
  stamp.SetHex("city_overlay", in.base_inputs.city_overlay);
  stamp.Set("drop_mask", uint64_t(in.drop_mask));
  stamp.Set("texture_set", in.texture_set);
  stamp.SetHex("texture_rows_hash", in.rows_hash);
  stamp.Set("hit_size", in.hit_size);
  stamp.SetHex("hit_hash", in.hit_hash);
  stamp.Set("tex_size", in.tex_size);
  stamp.Set("sectors", uint64_t(in.sectors));
  stamp.Set("materials", uint64_t(in.materials));
  stamp.Set("textures", uint64_t(in.textures));
  stamp.Set("runs", in.runs);
  stamp.Set("app_version", in.app_version);
  return stamp;
}

std::string TextureSetIdentity(const HitTextureSource* set) { return set ? set->Identity() : std::string("none"); }

std::filesystem::path TextureFilePath(const BuildRequest& base) { return base.out_dir / kTexFileName; }

}  // namespace

// ---------------------------------------------------------------------------
// the check
// ---------------------------------------------------------------------------

StampCheck CheckCityHitPack(const HitBuildRequest& request) {
  StampCheck check;
  const auto no = [&](std::string why) {
    check.current = false;
    check.why = std::move(why);
    return check;
  };
  const std::filesystem::path out_dir = request.base.out_dir;
  Stamp stamp;
  std::string error;
  if (!Stamp::Read(out_dir / kHitStampFileName, &stamp, &error)) return no("there is no hit stamp: the hit data was never built here");
  const auto number = [&](const char* key, uint64_t want) {
    uint64_t have = 0;
    return stamp.GetU64(key, &have) && have == want;
  };
  if (!number("stamp_format", kHitStampFormat)) return no("the hit stamp's format is not this build's");
  if (!number("hit_builder_version", kHitBuilderVersion)) return no("the hit data was built by another version of the builder");
  if (!number("hit_format_version", kHitFormatVersion)) return no("the hit data is another format version");
  if (!number("builder_revision", kHitBuilderRevision) || !number("corner_stream_version", kIndexedCornersVersion)) return no("the hit data was built by another revision of the hit builder");
  if (!number("rules", request.rules.Pack())) return no("the hit data was built with other precision rules (texture coordinate step, colour bits)");
  if (!number("material_rules_version", gb::kMaterialRulesVersion)) return no("the hit data was built from other G-buffer material rules");

  PackReader base;
  if (!base.Open(out_dir / kPackFileName, &error)) return no("the city pack is not there or is damaged: " + error);
  uint64_t hash = 0;
  if (!stamp.GetHex("base_pack_hash", &hash) || hash != base.header().table_hash || !number("base_pack_size", base.header().file_size)) return no("the city pack is not the one the hit data was built over");
  if (!number("drop_mask", base.header().drop_mask)) return no("the city pack leaves out other classes than the hit data was built for");

  const StampInputs inputs = StampInputsFor(request.base);
  if (!number("source_size", inputs.source.exists ? inputs.source.size : 0)) return no("the game archive has another size than the hit data was built from");
  if (stamp.Get("source_mtime_us") == nullptr || *stamp.Get("source_mtime_us") != (inputs.source.exists ? std::to_string(inputs.source.mtime_us) : std::string("0")))
    return no("the game archive has another time than the hit data was built from");
  uint64_t overlay = 0;
  if (!stamp.GetHex("city_overlay", &overlay) || overlay != inputs.city_overlay) return no("the mods' own city files have changed since the hit data was built");

  const std::string* texture_set = stamp.Get("texture_set");
  if (texture_set == nullptr || *texture_set != TextureSetIdentity(request.textures)) return no("the hit data was built with another texture set");
  uint64_t rows_hash = 0;
  size_t bad = 0;
  gb::TextureTable table;
  LoadTextureRows(request.texture_rows, &table, &rows_hash, &bad);
  uint64_t stamped_rows = 0;
  if (!stamp.GetHex("texture_rows_hash", &stamped_rows) || stamped_rows != rows_hash) return no("the per-texture material rows have changed since the hit data was built");

  const FileStat hit = StatFile(out_dir / kHitFileName);
  uint64_t hit_size = 0;
  if (!hit.exists || !stamp.GetU64("hit_size", &hit_size) || hit.size != hit_size) return no("the hit file is missing or is not the size the stamp says");
  uint64_t tex_size = 0;
  stamp.GetU64("tex_size", &tex_size);
  const FileStat tex = StatFile(TextureFilePath(request.base));
  if (tex_size != 0 && (!tex.exists || tex.size != tex_size)) return no("the texture file is missing or is not the size the stamp says");
  check.current = true;
  return check;
}

// ---------------------------------------------------------------------------
// the build
// ---------------------------------------------------------------------------

namespace {

// The golden counts of the retail game's archive: what BuildCityHitPack measured on 2026-11-21 (a census, not limits: another game build gives other numbers; the bytes are for the
// default HitRules). The base pack's own goldens (pack_format.h) are 439 sectors, 22,236,322 detail and 382,485 far triangles kept.
constexpr uint64_t kGoldenHiRuns = 83970;
constexpr uint64_t kGoldenLodRuns = 1452;
constexpr uint64_t kGoldenCorners = 67856421;  // 3 a triangle: 66,708,966 detail and 1,147,455 far
constexpr uint64_t kGoldenMaterials = 5261;    // the default included (no texture source: a material is a shader instance, a texture name and its values)
constexpr uint64_t kGoldenMaterialsTextured = 6805;  // with package H2's texture set: the key of the dictionary entry is part of a material, so one is split in two where two sectors name
                                                     // one picture by different keys (4,765 of 4,765 names resolve; 6,235 keys over 4,721 pictures)
constexpr uint64_t kGoldenTextureNames = 4765; // distinct diffuse texture names
constexpr uint64_t kGoldenShaderInstances = 54843;
constexpr uint64_t kGoldenFileBytes = 174174812;
constexpr uint64_t kGoldenFileBytesTextured = 174491164;  // + the texture table (8,342 records) and the longer material table; the texture file beside it is 244,060,992

}  // namespace

HitBuildResult BuildCityHitPack(const HitBuildRequest& request, ProgressSink* sink_in) {
  ProgressSink fallback;
  ProgressSink& sink = sink_in ? *sink_in : fallback;
  const BuildRequest& base_request = request.base;
  HitBuildResult result;
  const auto started = Clock::now();
  result.hit_path = base_request.out_dir / kHitFileName;
  const auto fail = [&](std::string why) {
    result.ok = false;
    result.error = std::move(why);
    result.seconds = SecondsSince(started);
    sink.Log(LogLevel::kError, result.error);
    return result;
  };
  if (!base_request.lzx) return fail("no LZX decoder was given to the city hit builder");
  if (!StatFile(base_request.source_rpf).exists) return fail("the game archive " + base_request.source_rpf.string() + " is not there");

  PackReader base;
  {
    std::string error;
    if (!base.Open(base_request.out_dir / kPackFileName, &error)) return fail("the city pack the hit data is built over is not there or is damaged (" + error + "): build the city pack first");
  }
  if (!base_request.force && !base_request.census_only) {
    const StampCheck check = CheckCityHitPack(request);
    if (check.current) {
      result.ok = true;
      result.up_to_date = true;
      result.seconds = SecondsSince(started);
      sink.Log(LogLevel::kInfo, "the city hit data is current: nothing to build");
      return result;
    }
    sink.Log(LogLevel::kInfo, "the city hit data is built: " + check.why);
  }
  const StampInputs stamp_base = StampInputsFor(base_request);
  const uint32_t drop_mask = base.header().drop_mask;

  gb::TextureTable rows;
  uint64_t rows_hash = 0;
  size_t bad_rows = 0;
  LoadTextureRows(request.texture_rows, &rows, &rows_hash, &bad_rows);
  if (bad_rows != 0) sink.Log(LogLevel::kWarning, std::to_string(bad_rows) + " rows of the per-texture material files could not be read and are skipped");

  Archives archives;
  if (!archives.source.Open(base_request.source_rpf)) return fail("the game archive " + base_request.source_rpf.string() + " could not be opened as an RPF archive");
  if (!base_request.mods_rpf.empty() && StatFile(base_request.mods_rpf).exists) archives.has_mods = archives.mods.Open(base_request.mods_rpf);

  // The table, and the sectors must be the base pack's.
  std::vector<uint8_t> file;
  std::string error;
  RscImage image;
  if (!archives.Read(kCityTablePath, &file)) return fail("city.xct is not in the archive");
  if (!DecodeResourceFile(file, base_request.lzx, &image, &error)) return fail(std::string("city.xct: ") + error);
  std::vector<SectorInfo> sectors;
  if (!ParseCityTable(image, &sectors, &error)) return fail(error);
  const uint32_t count = uint32_t(sectors.size());
  if (count != base.sector_count()) return fail("the game's city table has " + std::to_string(count) + " sectors and the city pack " + std::to_string(base.sector_count()) + ": the pack is not of these files");
  for (uint32_t i = 0; i < count; ++i) {
    if (sectors[i].name_hash != base.sector(i).name_hash) return fail("sector " + std::to_string(i) + " (" + sectors[i].name + ") is not the city pack's: the pack is not of these files");
  }

  HitCensus census;
  census.sectors = count;
  MaterialSet set;
  std::vector<MaterialFacts> set_facts(1);  // material 0: the default
  std::vector<HitBlob> lod_blobs(count);
  std::vector<std::vector<uint8_t>> lod_streams(count);
  std::vector<uint64_t> lod_corner_counts(count, 0);
  std::vector<std::vector<MaterialDef>> lod_locals(count);
  std::vector<std::vector<MaterialFacts>> lod_facts(count);
  std::vector<char> lod_has(count, 0);
  uint64_t lod_shader_instances = 0;
  {
    sink.BeginPhase("far level of detail", 1);
    if (!archives.Read(kLodPath, &file)) return fail("lod.xmd is not in the archive");
    if (!DecodeResourceFile(file, base_request.lzx, &image, &error)) return fail(std::string("lod.xmd: ") + error);
    std::vector<HitMesh> meshes;
    meshes.reserve(count);
    for (uint32_t i = 0; i < count; ++i) meshes.emplace_back(request.rules);
    std::vector<ShaderNames> shaders;
    ParseStats stats;
    if (!ParseLod(
            image, &shaders, [&](const SourceGeometry& g) { if (g.model_index < count) meshes[g.model_index].Take(g, drop_mask); }, &stats, &error, true))
      return fail(error);
    lod_shader_instances = shaders.size();
    for (uint32_t i = 0; i < count; ++i) {
      const SectorRecord& rec = base.sector(i);
      std::string problem;
      bool has_blob = false;
      if (!meshes[i].Finish(rec.lod_triangles, kAnyOpaque, rec.has_lod(), &lod_blobs[i], &has_blob, &problem, ("lod of " + sectors[i].name).c_str())) return fail(problem);
      lod_has[i] = has_blob ? 1 : 0;
      if (has_blob) CodeCorners(&lod_blobs[i], &lod_streams[i], &lod_corner_counts[i]);
      lod_locals[i] = std::move(meshes[i].locals);
      lod_facts[i] = std::move(meshes[i].facts);
      census.collect.Add(meshes[i].hit.stats());
    }
    sink.Step("lod.xmd");
  }
  if (sink.Cancelled()) {
    result.cancelled = true;
    result.seconds = SecondsSince(started);
    return result;
  }

  // The body: the blobs, as they come, in the table's order.
  std::filesystem::path body_path = result.hit_path;
  body_path += kBodySuffix;
  std::ofstream body;
  const bool writing = !base_request.census_only;
  if (writing) {
    std::error_code ec;
    std::filesystem::create_directories(base_request.out_dir, ec);
    body.open(body_path, std::ios::binary | std::ios::trunc);
    if (!body) return fail("the temporary file " + body_path.string() + " cannot be written");
  }
  uint64_t body_pos = 0;
  std::vector<HitSectorRecord> records(count);
  const auto write_blob = [&](const std::vector<uint8_t>& bytes, uint64_t* offset) {
    const uint64_t aligned = (body_pos + kHitSectionAlignment - 1) & ~uint64_t(kHitSectionAlignment - 1);
    if (writing && aligned != body_pos) {
      const char zeros[kHitSectionAlignment] = {};
      if (!WriteAll(body, zeros, size_t(aligned - body_pos))) return false;
    }
    *offset = aligned;
    if (writing && !WriteAll(body, bytes.data(), bytes.size())) return false;
    body_pos = aligned + bytes.size();
    return true;
  };

  // Maps a blob's local material numbers to the table's and puts the blob together: the runs and the indexed corner stream a worker made (a blob without corners is H0's plain one).
  // `dictionary` is what the blob's texture names are seen from (hit_material.h): the sector's own for a detail blob, the far level of detail's for a far one.
  const auto encode = [&](HitBlob* blob, const std::vector<uint8_t>& corner_stream, uint64_t corner_count, const std::vector<MaterialDef>& locals, const std::vector<MaterialFacts>& facts,
                          uint32_t dictionary, uint32_t* run_count) {
    const auto t = Clock::now();
    std::vector<uint32_t> global(locals.size());
    for (size_t l = 0; l < locals.size(); ++l) {
      MaterialDef def = locals[l];
      if (!def.texture.empty() && request.textures != nullptr) {
        const TextureEntry entry = request.textures->Resolve(dictionary, def.texture);
        def.picture = entry.picture;
        def.texture_key = entry.found() ? entry.key : 0;
      }
      global[l] = set.Add(def);
      if (global[l] == set_facts.size()) set_facts.push_back(facts[l]);
    }
    for (HitRun& run : blob->runs) run.material = global[run.material];
    *run_count = uint32_t(blob->runs.size());
    census.raw_equivalent_bytes += kHitBlobHeaderBytes + (corner_count != 0 ? 2u : 1u) * kStreamEntryBytes + uint64_t(blob->runs.size()) * kHitRunBytes + corner_count * 8u;
    std::vector<uint8_t> bytes;
    if (corner_count != 0) {
      bytes = AssembleIndexedHitBlob(*blob, corner_stream);
      census.corner_stream_bytes += corner_stream.size();
    } else {
      bytes = EncodeHitBlob(*blob, CornerEncoding::kRaw);
      ++census.blobs_without_corners;
    }
    census.corners += corner_count;
    census.encode_seconds += SecondsSince(t);
    return bytes;
  };

  // The sectors: workers parse, this thread maps the materials, encodes and writes in the table's order.
  const uint32_t hardware = std::max(1u, std::thread::hardware_concurrency());
  const uint32_t threads = std::clamp(base_request.threads != 0 ? base_request.threads : (hardware > 1 ? hardware - 1 : 1), 1u, 64u);
  sink.BeginPhase("city hit sectors", int(count));
  std::vector<HitSectorResult> results(count);
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
      if (base_request.worker_start) base_request.worker_start();
      for (;;) {
        const uint32_t i = next.fetch_add(1);
        if (i >= count || stop.load()) return;
        {
          std::unique_lock lock(mutex);
          cv_space.wait(lock, [&] { return stop.load() || i < written + window; });
          if (stop.load()) return;
        }
        HitSectorResult r = ProcessHitSector(archives, sectors[i], base.sector(i), base_request, drop_mask, request.rules);
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
  bool failed_sector = false;
  std::string failed_text;
  for (uint32_t i = 0; i < count; ++i) {
    {
      std::unique_lock lock(mutex);
      cv_done.wait(lock, [&] { return done[i] != 0; });
    }
    HitSectorResult r = std::move(results[i]);
    results[i] = HitSectorResult{};
    HitSectorRecord& rec = records[i];
    rec.name_hash = base.sector(i).name_hash;
    rec.hi_triangles = base.sector(i).hi_triangles;
    rec.lod_triangles = base.sector(i).lod_triangles;
    census.read_seconds += r.read_seconds;
    census.parse_seconds += r.parse_seconds;
    census.collect.Add(r.stats);
    census.shader_instances += r.shaders;
    if (r.failed) {
      ++census.sectors_failed;
      if (!failed_sector) failed_text = r.problem;
      failed_sector = true;
      if (census.problems.size() < 40) census.problems.push_back(r.problem);
    } else {
      if (r.has_blob) {
        std::vector<uint8_t> bytes = encode(&r.blob, r.corner_stream, r.corner_count, r.locals, r.facts, HitDictionaryOfSector(i), &rec.hi_runs);
        if (!write_blob(bytes, &rec.hi_offset)) write_failed = true;
        rec.hi_size = uint32_t(bytes.size());
        census.hi_blob_bytes += bytes.size();
        census.hi_blobs += 1;
        census.hi_triangles += r.blob.triangle_count;
        census.hi_runs += rec.hi_runs;
      }
      if (lod_has[i] != 0) {
        std::vector<uint8_t> bytes = encode(&lod_blobs[i], lod_streams[i], lod_corner_counts[i], lod_locals[i], lod_facts[i], kHitDictionaryLod, &rec.lod_runs);
        if (!write_blob(bytes, &rec.lod_offset)) write_failed = true;
        rec.lod_size = uint32_t(bytes.size());
        census.lod_blob_bytes += bytes.size();
        census.lod_blobs += 1;
        census.lod_triangles += lod_blobs[i].triangle_count;
        census.lod_runs += rec.lod_runs;
        std::vector<uint8_t>().swap(lod_streams[i]);
      }
    }
    {
      std::lock_guard lock(mutex);
      written = i + 1;
    }
    cv_space.notify_all();
    sink.Step(sectors[i].name);
    if (write_failed || failed_sector || sink.Cancelled()) {
      stop.store(true);
      cv_space.notify_all();
      result.cancelled = !write_failed && !failed_sector;
      break;
    }
  }
  stop.store(true);
  cv_space.notify_all();
  for (std::thread& t : workers) t.join();
  census.shader_instances += uint32_t(lod_shader_instances);
  const auto discard_body = [&] {
    if (writing) {
      body.close();
      std::error_code ec;
      std::filesystem::remove(body_path, ec);
    }
  };
  if (write_failed) {
    discard_body();
    return fail("the hit file could not be written completely (disk full?)");
  }
  if (failed_sector) {
    discard_body();
    return fail(failed_text);
  }
  if (result.cancelled) {
    discard_body();
    result.seconds = SecondsSince(started);
    return result;
  }
  if (writing) {
    body.flush();
    if (!body) {
      discard_body();
      return fail("the hit file could not be written completely (disk full?)");
    }
    body.close();
  }

  // The materials and the textures, now that every sector has said which it uses.
  std::vector<HitMaterial> materials;
  std::vector<HitTexture> textures;
  if (!FinalizeMaterials(set, request.textures, request.texture_rows.empty() ? nullptr : &rows, &materials, &textures, &census.finalize, &error)) {
    discard_body();
    return fail("the texture set stopped the build: " + error);
  }
  census.materials = uint32_t(materials.size());
  census.textures = uint32_t(textures.size());
  for (size_t m = 0; m < materials.size(); ++m) {
    ++census.materials_by_class[WordClass(materials[m].word)];
    const MaterialFacts& f = set_facts[m];
    if (m != 0) {
      if (!f.group_known) ++census.materials_group_unknown;
      if (f.no_params) ++census.materials_without_params;
      if (!f.has_exponent) ++census.materials_without_exponent;
      if (f.gloss_pixel) ++census.materials_gloss_pixel;
      if (f.gloss_vertex) ++census.materials_gloss_vertex;
    }
    if (set.defs()[m].flat_colour) ++census.materials_flat_colour;
    if (materials[m].emissive > 0.0f) ++census.materials_emissive;
  }

  // The head: header, sector table, materials, textures.
  const HitFileLayout layout = PlanHitFile(count, uint32_t(materials.size()), uint32_t(textures.size()));
  ByteWriter tables;  // [table_offset, blobs_offset)
  for (HitSectorRecord rec : records) {
    if (rec.hi_size != 0) rec.hi_offset += layout.blobs_offset;
    if (rec.lod_size != 0) rec.lod_offset += layout.blobs_offset;
    rec.Write(tables);
  }
  const auto pad_to = [&](uint64_t offset) {
    const size_t at = size_t(offset - layout.table_offset);
    if (tables.size() < at) tables.Zeros(at - tables.size());
  };
  pad_to(layout.materials_offset);
  for (const HitMaterial& m : materials) m.Write(tables);
  pad_to(layout.textures_offset);
  for (const HitTexture& t : textures) t.Write(tables);
  pad_to(layout.blobs_offset);
  HitHeader header;
  header.sector_count = count;
  header.material_count = uint32_t(materials.size());
  header.texture_count = uint32_t(textures.size());
  header.base_table_hash = base.header().table_hash;
  header.base_file_size = base.header().file_size;
  header.table_offset = layout.table_offset;
  header.materials_offset = layout.materials_offset;
  header.textures_offset = layout.textures_offset;
  header.blobs_offset = layout.blobs_offset;
  header.file_size = layout.blobs_offset + body_pos;
  header.table_hash = Fnv1a64(tables.bytes().data(), tables.size());
  ByteWriter head;
  header.Write(head);
  head.Bytes(tables.bytes().data(), tables.size());
  census.head_bytes = head.size();
  census.file_bytes = header.file_size;
  result.census = std::move(census);
  result.table_hash = header.table_hash;
  if (!writing) {
    result.ok = true;
    result.seconds = SecondsSince(started);
    return result;
  }

  // The file: the head, then the body copied behind it, under a temporary name; then moved into place.
  std::filesystem::path temp_path = result.hit_path;
  temp_path += ".tmp";
  {
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    std::ifstream in(body_path, std::ios::binary);
    bool ok = bool(out) && bool(in) && WriteAll(out, head.bytes().data(), head.size());
    std::vector<char> buffer(size_t(4) << 20);
    uint64_t left = body_pos;
    while (ok && left != 0) {
      const size_t chunk = size_t(std::min<uint64_t>(left, buffer.size()));
      in.read(buffer.data(), std::streamsize(chunk));
      ok = size_t(in.gcount()) == chunk && WriteAll(out, buffer.data(), chunk);
      left -= chunk;
    }
    if (ok) {
      out.flush();
      ok = bool(out);
    }
    out.close();
    in.close();
    std::error_code ec;
    std::filesystem::remove(body_path, ec);
    if (!ok) {
      std::filesystem::remove(temp_path, ec);
      return fail("the hit file could not be written completely (disk full?)");
    }
  }
  std::error_code ec;
  std::filesystem::remove(result.hit_path, ec);
  std::filesystem::rename(temp_path, result.hit_path, ec);
  if (ec) return fail("the hit file could not be moved into place: " + ec.message());

  // The texture file (the set's), with the hit table's hash in its header.
  uint64_t tex_size = 0;
  if (request.textures != nullptr) {
    const std::filesystem::path tex_path = TextureFilePath(base_request);
    std::error_code rm;
    std::filesystem::remove(tex_path, rm);
    if (!request.textures->Finish(tex_path, header.table_hash, &error)) return fail("the texture set could not write its file: " + error);
    const FileStat tex = StatFile(tex_path);
    tex_size = tex.exists ? tex.size : 0;
  }

  HitStampInputs stamp_in;
  stamp_in.base_inputs = stamp_base;
  stamp_in.base_pack_hash = base.header().table_hash;
  stamp_in.base_pack_size = base.header().file_size;
  stamp_in.drop_mask = drop_mask;
  stamp_in.texture_set = TextureSetIdentity(request.textures);
  stamp_in.rows_hash = rows_hash;
  stamp_in.rules = request.rules.Pack();
  stamp_in.hit_size = header.file_size;
  stamp_in.hit_hash = header.table_hash;
  stamp_in.tex_size = tex_size;
  stamp_in.sectors = count;
  stamp_in.materials = uint32_t(materials.size());
  stamp_in.textures = uint32_t(textures.size());
  stamp_in.runs = result.census.hi_runs + result.census.lod_runs;
  stamp_in.app_version = base_request.app_version;
  std::string stamp_error;
  if (!MakeHitStamp(stamp_in).Write(base_request.out_dir / kHitStampFileName, &stamp_error)) return fail(stamp_error);

  result.ok = true;
  result.hit_bytes = header.file_size;
  result.seconds = SecondsSince(started);
  sink.Log(LogLevel::kInfo, "city hit data built: " + std::to_string(count) + " sectors, " + std::to_string(result.census.hi_runs + result.census.lod_runs) + " runs, " +
                                std::to_string(materials.size()) + " materials, " + std::to_string(header.file_size) + " bytes in " + std::to_string(result.seconds) + " s");
  return result;
}

// ---------------------------------------------------------------------------
// the reader
// ---------------------------------------------------------------------------

bool HitPackReader::Open(const std::filesystem::path& file, std::string* error) {
  open_ = false;
  sectors_.clear();
  materials_.clear();
  textures_.clear();
  path_ = file;
  const auto fail = [&](std::string why) {
    if (error) *error = std::move(why);
    return false;
  };
  std::ifstream in(file, std::ios::binary);
  if (!in) return fail("the city hit file " + file.string() + " cannot be opened");
  std::vector<uint8_t> head(kHitHeaderBytes);
  in.read(reinterpret_cast<char*>(head.data()), std::streamsize(head.size()));
  if (size_t(in.gcount()) != head.size()) return fail("the city hit file is shorter than its header");
  ByteReader hr(head.data(), head.size());
  std::string why;
  if (!HitHeader::Read(hr, &header_, &why)) return fail(why);
  in.seekg(0, std::ios::end);
  const uint64_t file_size = uint64_t(in.tellg());
  if (header_.file_size != file_size) return fail("the city hit file is not the size its header says (cut short, or still being written)");
  const uint64_t material_bytes = uint64_t(header_.material_count) * kHitMaterialBytes;
  const uint64_t texture_bytes = uint64_t(header_.texture_count) * kHitTextureBytes;
  const uint64_t sector_bytes = uint64_t(header_.sector_count) * kHitSectorRecordBytes;
  if (header_.table_offset + sector_bytes > header_.materials_offset || header_.materials_offset + material_bytes > header_.textures_offset ||
      header_.textures_offset + texture_bytes > header_.blobs_offset || header_.blobs_offset > file_size)
    return fail("the city hit file's tables overlap or run past its end");
  std::vector<uint8_t> tables(size_t(header_.blobs_offset - header_.table_offset));
  in.seekg(std::streamoff(header_.table_offset));
  in.read(reinterpret_cast<char*>(tables.data()), std::streamsize(tables.size()));
  if (!in) return fail("the city hit file's tables could not be read");
  if (Fnv1a64(tables.data(), tables.size()) != header_.table_hash) return fail("the city hit file's tables do not match their hash (the file is damaged)");
  ByteReader sr(tables.data(), tables.size());
  sectors_.resize(header_.sector_count);
  for (HitSectorRecord& s : sectors_) {
    if (!HitSectorRecord::Read(sr, &s)) return fail("the city hit file's sector table is cut short");
    if ((s.hi_size != 0 && s.hi_offset + s.hi_size > file_size) || (s.lod_size != 0 && s.lod_offset + s.lod_size > file_size)) return fail("a sector's blob is outside the file");
  }
  ByteReader mr(tables.data() + (header_.materials_offset - header_.table_offset), size_t(material_bytes));
  materials_.resize(header_.material_count);
  for (HitMaterial& m : materials_) {
    if (!HitMaterial::Read(mr, &m)) return fail("the city hit file's material table is cut short");
  }
  ByteReader tr(tables.data() + (header_.textures_offset - header_.table_offset), size_t(texture_bytes));
  textures_.resize(header_.texture_count);
  for (HitTexture& t : textures_) {
    if (!HitTexture::Read(tr, &t)) return fail("the city hit file's texture table is cut short or has a texture format this build does not know");
  }
  open_ = true;
  return true;
}

bool HitPackReader::ReadBlob(uint32_t sector, MeshKind kind, std::vector<uint8_t>* out, std::string* error) const {
  if (error) error->clear();
  if (!open_ || sector >= sectors_.size()) {
    if (error) *error = "no such sector";
    return false;
  }
  const HitSectorRecord& s = sectors_[sector];
  const bool hi = kind == MeshKind::kHi;
  const uint32_t size = hi ? s.hi_size : s.lod_size;
  if (size == 0) return false;  // not an error: the sector has no such blob
  std::ifstream in(path_, std::ios::binary);
  if (!in) {
    if (error) *error = "the city hit file cannot be opened";
    return false;
  }
  out->resize(size);
  in.seekg(std::streamoff(hi ? s.hi_offset : s.lod_offset));
  in.read(reinterpret_cast<char*>(out->data()), std::streamsize(size));
  if (!in) {
    if (error) *error = "a blob of the city hit file could not be read";
    return false;
  }
  return true;
}

bool HitPackReader::ReadHitBlob(uint32_t sector, MeshKind kind, HitBlob* out, std::string* error) const {
  std::vector<uint8_t> bytes;
  if (!ReadBlob(sector, kind, &bytes, error)) return false;
  return DecodeHitBlobAny(bytes.data(), bytes.size(), out, error);
}

uint32_t ValidateHitPack(const HitPackReader& hit, const PackReader& base, std::vector<std::string>* problems) {
  uint32_t count = 0;
  const auto note = [&](uint32_t sector, const std::string& what) {
    ++count;
    if (problems && problems->size() < 20) problems->push_back((sector < base.sector_count() ? std::string(base.name(sector)) : std::string("(header)")) + ": " + what);
  };
  if (hit.header().sector_count != base.sector_count()) {
    note(0xFFFFFFFFu, "the hit file has " + std::to_string(hit.header().sector_count) + " sectors, the base pack " + std::to_string(base.sector_count()));
    return count;
  }
  if (hit.header().base_table_hash != base.header().table_hash || hit.header().base_file_size != base.header().file_size) note(0xFFFFFFFFu, "the hit file was built over another base pack");
  if (hit.materials().empty() || WordClass(hit.materials()[0].word) != 1) note(0xFFFFFFFFu, "material 0 is not the default world surface");
  for (uint32_t i = 0; i < base.sector_count(); ++i) {
    const SectorRecord& b = base.sector(i);
    const HitSectorRecord& h = hit.sector(i);
    if (h.name_hash != b.name_hash) note(i, "the sector's name hash is not the base pack's");
    if (h.hi_triangles != b.hi_triangles || h.lod_triangles != b.lod_triangles) note(i, "the sector's triangle counts are not the base pack's");
    for (const MeshKind kind : {MeshKind::kHi, MeshKind::kLod}) {
      const bool is_hi = kind == MeshKind::kHi;
      const bool base_has = is_hi ? b.has_hi() : b.has_lod();
      const uint32_t size = is_hi ? h.hi_size : h.lod_size;
      if (base_has != (size != 0)) {
        note(i, std::string("the ") + (is_hi ? "detail" : "far") + " blob is " + (base_has ? "missing" : "there although the base pack has none"));
        continue;
      }
      if (!base_has) continue;
      HitBlob blob;
      std::string error;
      if (!hit.ReadHitBlob(i, kind, &blob, &error)) {
        note(i, error.empty() ? "a blob could not be read" : error);
        continue;
      }
      if (blob.triangle_count != (is_hi ? b.hi_triangles : b.lod_triangles)) note(i, "a blob's triangle count is not the base pack's");
      if (is_hi && blob.opaque_triangles != b.hi_opaque_triangles) note(i, "the opaque triangle count is not the base pack's");
      if (blob.runs.size() != (is_hi ? h.hi_runs : h.lod_runs)) note(i, "a blob's run count is not its record's");
      for (const HitRun& run : blob.runs) {
        if (run.material >= hit.materials().size()) {
          note(i, "a run names a material outside the table");
          break;
        }
      }
      if (blob.has_corners() && blob.corners.size() != size_t(blob.triangle_count) * 6) note(i, "a blob's corners are not three a triangle");
    }
  }
  return count;
}

// ---------------------------------------------------------------------------
// the report
// ---------------------------------------------------------------------------

std::string HitCensus::Report() const {
  std::ostringstream out;
  const auto golden = [&](const char* what, uint64_t have, uint64_t want) {
    out << "  " << what << ": " << have << " (golden " << want << (have == want ? ", equal" : ", DIFFERENT") << ")\n";
  };
  const auto mib = [](uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f MiB", double(bytes) / (1024.0 * 1024.0));
    return std::string(text);
  };
  out << "city hit data census\n";
  out << "sectors: " << sectors << ", " << hi_blobs << " detail blobs and " << lod_blobs << " far blobs, " << sectors_failed << " failed\n";
  out << "what the hit file holds:\n";
  out << "  detail: " << hi_triangles << " triangles in " << hi_runs << " runs; far: " << lod_triangles << " triangles in " << lod_runs << " runs\n";
  golden("detail runs", hi_runs, kGoldenHiRuns);
  golden("far runs", lod_runs, kGoldenLodRuns);
  golden("corners (3 a triangle)", corners, kGoldenCorners);
  golden("materials (the default included)", materials, textures != 0 ? kGoldenMaterialsTextured : kGoldenMaterials);
  golden("shader instances read", shader_instances, kGoldenShaderInstances);
  golden("distinct diffuse texture names", finalize.texture_names, kGoldenTextureNames);
  out << "  runs: " << collect.runs << " in all, " << collect.runs_split << " made by cutting a geometry whose texture coordinates range wider than " << kMaxRunUvRange
      << ", triangles clamped into a transform: " << collect.triangles_uv_clamped << "\n";
  out << "sizes:\n";
  golden("hit file bytes (default rules)", file_bytes, textures != 0 ? kGoldenFileBytesTextured : kGoldenFileBytes);
  out << "  the hit file: " << file_bytes << " bytes (" << mib(file_bytes) << "): head " << head_bytes << ", detail blobs " << hi_blob_bytes << " (" << mib(hi_blob_bytes) << "), far blobs "
      << lod_blob_bytes << "\n";
  out << "  the same blobs with the raw corner encoding: " << raw_equivalent_bytes << " bytes (" << mib(raw_equivalent_bytes) << "); the indexed corner streams are " << corner_stream_bytes
      << " bytes (" << mib(corner_stream_bytes) << "); " << blobs_without_corners << " blobs have no corners\n";
  if (corners != 0) out << "  " << double(hi_blob_bytes + lod_blob_bytes) / double(corners) << " bytes a corner in all (raw 8), " << double(corner_stream_bytes) / double(corners) << " in the corner streams\n";
  out << "the corners' source (CHECK C2 and C3):\n";
  out << "  geometries " << collect.geometries << ", triangles " << collect.triangles << "; without a texture coordinate " << collect.geometries_without_uv << ", without COLOR1 "
      << collect.geometries_without_colour << ", declaration not read " << collect.geometries_layout_unknown << ", texture coordinates not finite (corners) " << collect.uv_not_finite << "\n";
  out << "  a second texture coordinate set (TEXCOORD1) in " << collect.geometries_second_uv << " geometries, " << collect.triangles_second_uv << " triangles; COLOR0 in "
      << collect.geometries_colour0 << " geometries\n";
  out << "the material table (" << materials << "), by class:";
  static const char* kClassNames[16] = {"none", "world", "road", "wetroad", "sidewalk", "terrain", "lod", "foliage", "paint", "chrome", "metal", "glass", "skin", "emissive", "interior", "water"};
  for (size_t c = 0; c < materials_by_class.size(); ++c) {
    if (materials_by_class[c] != 0) out << " " << kClassNames[c] << "=" << materials_by_class[c];
  }
  out << "\n  smoothness from the pixel shader's exponent " << materials_gloss_pixel << ", from the vertex shader's " << materials_gloss_vertex << ", the class default for the rest ("
      << materials_without_exponent << " have no SpecularExponent); flat colours " << materials_flat_colour << ", glowing " << materials_emissive << "; group not in the class table "
      << materials_group_unknown << ", without parameters " << materials_without_params << "\n";
  out << "  textures: " << finalize.texture_names << " distinct names in " << finalize.materials_textured << " textured materials; " << finalize.names_resolved << " resolved by the texture set ("
      << textures << " in its table), " << finalize.materials_unresolved << " materials keep their average colour; per-texture rows changed " << finalize.rows_applied << " materials\n";
  if (!finalize.unresolved_names.empty()) {
    out << "  e.g. unresolved:";
    for (const std::string& n : finalize.unresolved_names) out << " " << n;
    out << "\n";
  }
  out << "time summed over the workers: read and LZX decompress " << read_seconds << " s, parse and weld " << parse_seconds << " s; the writer's encode " << encode_seconds << " s\n";
  if (!problems.empty()) {
    out << "problems (first " << problems.size() << "):\n";
    for (const std::string& p : problems) out << "  " << p << "\n";
  }
  return out.str();
}

}  // namespace larecomp::citypack
