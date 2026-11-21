// citypack_tool: the city pack builder from the command line (dev tool; the app and the launcher call the same library).
//
//   citypack_tool build    --game <folder with xarchive_cache.rpf> --out <folder> [--mods <xarchive_mods.rpf>] [--threads N] [--force]
//                          [--positions float|grid] [--keep decal,interior,water,...]
//   citypack_tool census   --game <folder> [--threads N] [--keep ...]      read every sector, write nothing, print the census (golden totals, classes,
//                                                                          every shader, buckets, timings)
//   citypack_tool check    --game <folder> --out <folder>                  stat only: is the pack current? (exit 0 = yes)
//   citypack_tool validate <pack file>                                     decode every blob and compare with the table (exit 0 = whole)
//   citypack_tool info     <pack file> [sector name]                       the header, the totals, or one sector's record and mesh (a city pack, or a hit pack: it tells by the magic)
//   citypack_tool lamps    --game <folder> [--golden]                      read lights.xlm from the archive (the mod archive's first), print the census and the 64 m grid;
//                                                                          --golden: exit 1 unless the counts are the ones the survey and this file's reading found
//                          --simulate-live: the merge rule (lamp_merge.h) on the real file: every light packed the way the game packs it (a model: 16 bit position steps,
//                                                                          half floats), merged in the file's order and in a shuffled one: all 15,174 must match, 1:1
//   citypack_tool lamps    <pack file> [--near x,z,r] [--strongest x,y,z,r,n [--hour h]] [--cell x,z]
//                                                                          the pack's lamps section: its totals, the lamps around a point, the strongest at a point,
//                                                                          one cell's emitter
//
//   the HIT DATA (mcla_city_hit.v1.pack, built over the city pack in --out: hit_builder.h; the same game files):
//   citypack_tool build-hit --game <folder> --out <folder> [--mods <xarchive_mods.rpf>] [--threads N] [--force] [--census] [--rows <table file>]...
//                          builds the hit pack next to the city pack (--out is the city pack's folder); --census reads, parses and encodes everything and writes nothing (the golden counts
//                          and the sizes); --rows adds a GB-F2 per-texture table file (applied to the materials by their texture keys)
//                          the textures: where package H2's texture set is built in (citypack/texture_set.h), build-hit builds it first (the dictionaries of common, lod_0 and the 439 sectors:
//                          mcla_city_tex.v1.pack beside the hit pack, its table in the hit file, the materials' pictures, keys and averages); --no-textures leaves it out
//                          --uv-step N (the texture coordinate step is never finer than 2^-N of a repeat; 0 = the full 16 bits), --ao-bits N, --tint-bits N (8 = lossless): hit_collect.h HitRules
//                          --zstd N (zstd level of the hit file's corner streams and of the texture file's chains, 1..19, 0 = stored as they are; default 9: city_zstd.h)
//   citypack_tool check-hit --game <folder> --out <folder> [--rows <table file>]... [--deep]
//                          stat only: is the hit pack current? (exit 0 = yes); --deep also decodes every blob and compares it with the city pack (exit 0 = whole)
//   citypack_tool info <hit pack file> [sector number | sector name --out <the city pack's folder>]
//   citypack_tool check-tex --out <folder>                                 the texture file against the hit pack beside it: every picture read and unpacked (exit 0 = whole), timed
//   citypack_tool tex-census --game <folder> [--threads N]                 the city texture set (CITY-RT M2 H2): read every dictionary (common.xtd, lod_0.xtd, the sectors' own), decode,
//                                                                          dedupe, print the counts, the formats and the size at each class's cap; writes nothing
//   citypack_tool tex-build  --game <folder> --out <folder> [--threads N]  the same, then the texture file mcla_city_tex.v1.pack (its hit table hash is 0: a development file; the
//                                                                          hit data build writes the real one) and a read back of every texture
//
// build-hit, check-hit and check-tex run the code that larecomp.exe --build_city_hit / --check_city_hit and the game's own background build run (citypack/hit_data.h): the same builders,
// the same stamp, the same checks.
//
// Reads the player's own game files; the pack it writes is game-derived and is never shipped or committed.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "citypack/builder.h"
#include "citypack/hit_builder.h"
#include "citypack/hit_data.h"
#include "citypack/hit_texture_adapter.h"
#include "citypack/lamp_merge.h"
#include "citypack/lamp_parse.h"
#include "citypack/pack_reader.h"
#include "citypack/texture_residency.h"
#include "citypack/texture_set.h"
#include "mc_engine/modloader/rpf3.h"
#include "mc_engine/modloader/xcompress.h"

namespace fs = std::filesystem;
using namespace larecomp::citypack;

namespace {

class PrintSink final : public ProgressSink {
 public:
  void BeginPhase(const std::string& name, int total) override {
    total_ = total;
    done_ = 0;
    std::fprintf(stderr, "[%s] %d steps\n", name.c_str(), total);
  }
  void Step(const std::string& item) override {
    ++done_;
    if (done_ % 40 == 0 || done_ == total_) std::fprintf(stderr, "  %d/%d (%s)\n", done_, total_, item.c_str());
  }
  void Log(LogLevel level, const std::string& text) override {
    std::fprintf(stderr, "%s%s\n", level == LogLevel::kError ? "ERROR: " : level == LogLevel::kWarning ? "warning: " : "", text.c_str());
  }

 private:
  int total_ = 0;
  int done_ = 0;
};

struct Args {
  std::vector<std::string> positional;
  std::string game, out, mods, positions, keep;
  std::vector<std::string> rows;
  std::string near_query, strongest_query, cell_query, hour;
  int uv_step = -1, ao_bits = -1, tint_bits = -1;  // build-hit: the precision rules (hit_collect.h HitRules); -1: the defaults
  int zstd = -1;                                   // build-hit, tex-census, tex-build: the zstd level (city_zstd.h); -1: the default
  uint32_t threads = 0;
  bool force = false;
  bool census = false;
  bool deep = false;
  bool no_textures = false;
  bool golden = false;
  bool simulate_live = false;
};

bool ParseArgs(int argc, char** argv, int first, Args* args) {
  for (int i = first; i < argc; ++i) {
    const std::string a = argv[i];
    const auto value = [&](std::string* into) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", a.c_str());
        return false;
      }
      *into = argv[++i];
      return true;
    };
    if (a == "--game") { if (!value(&args->game)) return false; }
    else if (a == "--out") { if (!value(&args->out)) return false; }
    else if (a == "--mods") { if (!value(&args->mods)) return false; }
    else if (a == "--positions") { if (!value(&args->positions)) return false; }
    else if (a == "--keep") { if (!value(&args->keep)) return false; }
    else if (a == "--rows") { std::string r; if (!value(&r)) return false; args->rows.push_back(r); }
    else if (a == "--threads") { std::string t; if (!value(&t)) return false; args->threads = uint32_t(std::strtoul(t.c_str(), nullptr, 10)); }
    else if (a == "--near") { if (!value(&args->near_query)) return false; }
    else if (a == "--strongest") { if (!value(&args->strongest_query)) return false; }
    else if (a == "--cell") { if (!value(&args->cell_query)) return false; }
    else if (a == "--hour") { if (!value(&args->hour)) return false; }
    else if (a == "--golden") args->golden = true;
    else if (a == "--simulate-live") args->simulate_live = true;
    else if (a == "--force") args->force = true;
    else if (a == "--census") args->census = true;
    else if (a == "--uv-step") { std::string t; if (!value(&t)) return false; args->uv_step = std::atoi(t.c_str()); }
    else if (a == "--ao-bits") { std::string t; if (!value(&t)) return false; args->ao_bits = std::atoi(t.c_str()); }
    else if (a == "--tint-bits") { std::string t; if (!value(&t)) return false; args->tint_bits = std::atoi(t.c_str()); }
    else if (a == "--zstd") { std::string t; if (!value(&t)) return false; args->zstd = std::atoi(t.c_str()); }
    else if (a == "--deep") args->deep = true;
    else if (a == "--no-textures") args->no_textures = true;
    else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return false; }
    else args->positional.push_back(a);
  }
  return true;
}

// --keep decal,interior: classes that stay in the pack although the default drops them.
bool ApplyKeep(const std::string& keep, uint32_t* drop_mask) {
  size_t pos = 0;
  while (pos < keep.size()) {
    size_t end = keep.find(',', pos);
    if (end == std::string::npos) end = keep.size();
    const std::string name = keep.substr(pos, end - pos);
    pos = end + 1;
    bool found = false;
    for (uint32_t c = 0; c < uint32_t(MaterialClass::kCount); ++c) {
      if (ClassName(static_cast<MaterialClass>(c)) == name) {
        *drop_mask &= ~ClassBit(static_cast<MaterialClass>(c));
        found = true;
      }
    }
    if (!found) {
      std::fprintf(stderr, "unknown class %s\n", name.c_str());
      return false;
    }
  }
  return true;
}

bool MakeRequest(const Args& args, BuildRequest* request) {
  if (args.game.empty()) {
    std::fprintf(stderr, "--game <folder with xarchive_cache.rpf> is required\n");
    return false;
  }
  request->source_rpf = fs::path(args.game) / "xarchive_cache.rpf";
  if (!args.mods.empty()) request->mods_rpf = args.mods;
  else if (fs::exists(fs::path(args.game) / "xarchive_mods.rpf")) request->mods_rpf = fs::path(args.game) / "xarchive_mods.rpf";
  request->out_dir = args.out;
  request->threads = args.threads;
  request->force = args.force;
  request->app_version = "citypack_tool";
  request->lzx = [](const uint8_t* src, size_t src_len, std::vector<uint8_t>& dst, size_t dst_len) { return mc::modloader::LzxDecompress(src, src_len, dst, dst_len); };
  if (args.positions == "grid") request->positions = PositionEncoding::kGrid16;
  else if (!args.positions.empty() && args.positions != "float") {
    std::fprintf(stderr, "--positions is float or grid\n");
    return false;
  }
  return ApplyKeep(args.keep, &request->drop_mask);
}

// The hit pack's info: the header, the totals, or one sector.
int InfoHit(const Args& args) {
  HitPackReader hit;
  std::string error;
  if (!hit.Open(args.positional[0], &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  const HitHeader& h = hit.header();
  std::printf("city hit pack: format %u, builder %u, %u sectors, %u materials, %u textures, %llu bytes (tables end at %llu); built over a city pack of %llu bytes, hash %016llx\n",
              h.format_version, h.builder_version, h.sector_count, h.material_count, h.texture_count, static_cast<unsigned long long>(h.file_size),
              static_cast<unsigned long long>(h.blobs_offset), static_cast<unsigned long long>(h.base_file_size), static_cast<unsigned long long>(h.base_table_hash));
  if (args.positional.size() < 2) {
    uint64_t hi_t = 0, lod_t = 0, hi_r = 0, lod_r = 0, hi_b = 0, lod_b = 0;
    for (const HitSectorRecord& s : hit.sectors()) {
      hi_t += s.hi_triangles; lod_t += s.lod_triangles; hi_r += s.hi_runs; lod_r += s.lod_runs; hi_b += s.hi_size; lod_b += s.lod_size;
    }
    std::printf("detail: %llu triangles, %llu runs, %llu bytes; far: %llu triangles, %llu runs, %llu bytes\n", static_cast<unsigned long long>(hi_t), static_cast<unsigned long long>(hi_r),
                static_cast<unsigned long long>(hi_b), static_cast<unsigned long long>(lod_t), static_cast<unsigned long long>(lod_r), static_cast<unsigned long long>(lod_b));
    uint32_t by_class[32] = {};
    for (const HitMaterial& m : hit.materials()) ++by_class[WordClass(m.word) & 31u];
    std::printf("materials by class (the G-buffer's numbers):");
    for (int c = 0; c < 32; ++c) {
      if (by_class[c] != 0) std::printf(" %d=%u", c, by_class[c]);
    }
    std::printf("\n");
    return 0;
  }
  // The hit file keeps no names: the second argument is the sector's number, or its name when --out names the city pack's folder.
  uint32_t found = ~0u;
  if (!args.out.empty()) {
    PackReader base;
    if (base.Open(fs::path(args.out) / kPackFileName, &error)) {
      for (uint32_t i = 0; i < base.sector_count(); ++i) {
        if (base.name(i) == args.positional[1]) found = i;
      }
    }
  }
  if (found == ~0u) found = uint32_t(std::strtoul(args.positional[1].c_str(), nullptr, 10));
  if (found >= hit.sector_count()) {
    std::fprintf(stderr, "no sector %s (give its number, or its name with --out <the city pack's folder>)\n", args.positional[1].c_str());
    return 1;
  }
  const HitSectorRecord& s = hit.sector(found);
  std::printf("sector %u: name hash %08x; detail %u triangles %u runs %u bytes; far %u triangles %u runs %u bytes\n", found, s.name_hash, s.hi_triangles, s.hi_runs, s.hi_size, s.lod_triangles,
              s.lod_runs, s.lod_size);
  HitBlob blob;
  if (hit.ReadHitBlob(found, MeshKind::kHi, &blob, &error)) {
    std::printf("detail blob decodes: %u triangles (%u opaque), %zu runs, %s\n", blob.triangle_count, blob.opaque_triangles, blob.runs.size(), blob.has_corners() ? "with corners" : "no corners");
    for (const HitRun& run : blob.runs) {
      const HitMaterial& m = hit.materials()[run.material];
      std::printf("  run %u +%u material %u (class %u smooth %u lit %u, texture %u) flags %u uv bias (%g %g) scale (%g %g)\n", run.first_triangle, run.triangle_count, run.material, WordClass(m.word),
                  WordSmoothness(m.word), WordLit(m.word), m.texture, run.flags, run.uv_bias[0], run.uv_bias[1], run.uv_scale[0], run.uv_scale[1]);
    }
  }
  return 0;
}

// Is the file a hit pack? (the magic of its first eight bytes)
bool IsHitFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  char magic[8] = {};
  in.read(magic, sizeof(magic));
  return in && std::memcmp(magic, kHitMagic, sizeof(magic)) == 0;
}

int Info(const Args& args) {
  if (args.positional.empty()) {
    std::fprintf(stderr, "info <pack file> [sector name]\n");
    return 2;
  }
  if (IsHitFile(args.positional[0])) return InfoHit(args);
  PackReader reader;
  std::string error;
  if (!reader.Open(args.positional[0], &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  const PackHeader& h = reader.header();
  std::printf("city pack: format %u, builder %u, %u sectors, drop mask 0x%x, positions %u, %llu bytes\n", h.format_version, h.builder_version, h.sector_count, h.drop_mask,
              h.position_encoding, static_cast<unsigned long long>(h.file_size));
  if (h.has_lamps()) std::printf("lamps section: %llu bytes at %llu (citypack_tool lamps <pack> reads it)\n", static_cast<unsigned long long>(h.lamps_size), static_cast<unsigned long long>(h.lamps_offset));
  else std::printf("no lamps section (builder version %u)\n", h.builder_version);
  if (args.positional.size() < 2) {
    uint64_t hi_t = 0, hi_v = 0, lod_t = 0, lod_v = 0, hi_b = 0, lod_b = 0;
    for (const SectorRecord& s : reader.sectors()) {
      hi_t += s.hi_triangles; hi_v += s.hi_vertices; lod_t += s.lod_triangles; lod_v += s.lod_vertices; hi_b += s.hi_size; lod_b += s.lod_size;
    }
    std::printf("hi: %llu triangles, %llu vertices, %llu bytes; lod: %llu triangles, %llu vertices, %llu bytes\n", static_cast<unsigned long long>(hi_t), static_cast<unsigned long long>(hi_v),
                static_cast<unsigned long long>(hi_b), static_cast<unsigned long long>(lod_t), static_cast<unsigned long long>(lod_v), static_cast<unsigned long long>(lod_b));
    return 0;
  }
  for (uint32_t i = 0; i < reader.sector_count(); ++i) {
    if (reader.name(i) != args.positional[1]) continue;
    const SectorRecord& s = reader.sector(i);
    std::printf("%s: box (%.1f %.1f %.1f) - (%.1f %.1f %.1f); hi %u triangles %u vertices (%u opaque, %u dropped) %u bytes; lod %u triangles %u vertices %u bytes\n",
                std::string(reader.name(i)).c_str(), s.aabb_min[0], s.aabb_min[1], s.aabb_min[2], s.aabb_max[0], s.aabb_max[1], s.aabb_max[2], s.hi_triangles, s.hi_vertices,
                s.hi_opaque_triangles, s.dropped_triangles, s.hi_size, s.lod_triangles, s.lod_vertices, s.lod_size);
    MeshData mesh;
    if (reader.ReadMesh(i, MeshKind::kHi, &mesh, &error)) {
      std::printf("hi mesh decodes: %u vertices, %u triangles, %zu runs\n", mesh.vertex_count(), mesh.triangle_count(), mesh.runs.size());
      for (const GeometryRun& run : mesh.runs) std::printf("  run %u +%u %s shader %u\n", run.first_triangle, run.triangle_count, std::string(ClassName(run.material)).c_str(), run.shader_index);
    }
    return 0;
  }
  std::fprintf(stderr, "no sector named %s\n", args.positional[1].c_str());
  return 1;
}

// The texture set: tex-census (nothing written) and tex-build (the file, read back).
int TextureSetCommand(const Args& args, bool write) {
  if (args.game.empty() || (write && args.out.empty())) {
    std::fprintf(stderr, "--game <folder with xarchive_cache.rpf>%s is required\n", write ? " and --out <folder>" : "");
    return 2;
  }
  BuildRequest base;
  if (!MakeRequest(args, &base)) return 2;
  TextureSetRequest request;
  request.source_rpf = base.source_rpf;
  request.mods_rpf = base.mods_rpf;
  request.lzx = base.lzx;
  request.threads = args.threads;
  if (args.zstd >= 0) request.zstd_level = args.zstd;
  PrintSink sink;
  const TextureSetResult result = BuildTextureSet(request, &sink);
  if (!result.ok) {
    std::fprintf(stderr, "failed: %s\n", result.error.c_str());
    return 1;
  }
  const TextureSetStats& s = result.stats;
  const auto mb = [](uint64_t bytes) { return double(bytes) / 1048576.0; };
  std::printf("sources %u (%u failed): common.xtd, lod_0.xtd and %u sectors' own dictionaries\n", s.sources, s.sources_failed, s.sectors);
  std::printf("dictionary entries %llu (bad pointer %u, not 2D %u, unsupported format %u, no base %u, size mismatch %u, not tiled %u, odd swizzle %u, odd endian %u)\n",
              static_cast<unsigned long long>(s.entries), s.dictionary.bad_pointer, s.dictionary.not_supported, s.dictionary.unsupported_format, s.dictionary.no_base_level, s.dictionary.size_mismatch,
              s.dictionary.not_tiled, s.dictionary.odd_swizzle, s.dictionary.odd_endian);
  std::printf("distinct names %u; distinct guest keys %u; distinct pictures %u (the duplicates would have taken %.1f MB); %u names mean different pictures in different dictionaries, %u twice in one\n",
              s.distinct_names, s.key_variants, s.distinct, mb(s.duplicate_bytes), s.cross_conflicts, s.name_conflicts);
  std::printf("formats: BC1 %u, BC2 %u, BC3 %u, RGBA8 %u; single level %u; gamma %u; alpha is coverage %u; damaged %u; decode failed %u\n", s.by_format[0], s.by_format[1], s.by_format[2], s.by_format[3],
              s.single_level, s.gamma, s.with_alpha, s.damaged, s.decode_failed);
  std::printf("texture file %.1f MB (%llu bytes); built in %.1f s\n", mb(result.set.file_bytes()), static_cast<unsigned long long>(result.set.file_bytes()), s.seconds);
  if (s.zstd_level != 0) {
    std::printf("  zstd level %d: the chains %.1f MB -> %.1f MB (%.1f percent), %u of %u pictures stored as frames, compression %.2f s (wall)\n", s.zstd_level, mb(s.plain_bytes), mb(s.data_bytes),
                100.0 * double(s.data_bytes) / double(std::max<uint64_t>(s.plain_bytes, 1)), s.zstd_pictures, s.distinct, s.compress_seconds);
  }
  std::printf("resident (the whole set at a class's cap, block data): ");
  for (const uint32_t cap : {0u, 512u, 256u, 128u}) {
    const ResidentTotals t = TotalResidentBytes(result.set.textures, cap);
    std::printf("%s %.1f MB (%u capped)%s", cap == 0 ? "full" : ("cap " + std::to_string(cap)).c_str(), mb(t.bytes), t.capped, cap == 128 ? "\n" : ", ");
  }
  if (!write) return 0;
  const std::filesystem::path path = std::filesystem::path(args.out) / kTexFileName;
  std::string error;
  if (!WriteTextureFile(path, result.set, 0, &error)) {
    std::fprintf(stderr, "failed: %s\n", error.c_str());
    return 1;
  }
  std::vector<std::string> problems;
  const auto read_started = std::chrono::steady_clock::now();
  const uint32_t bad = ValidateTextureFile(path, result.set.textures, 0, &problems);
  const double read_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - read_started).count();
  for (const std::string& p : problems) std::fprintf(stderr, "  %s\n", p.c_str());
  std::printf("wrote %s; read back (every picture's whole chain, unpacked, one thread): %u problems in %.2f s\n", path.string().c_str(), bad, read_seconds);
  return bad == 0 ? 0 : 1;
}

// "1.5,2,3" -> numbers; false unless there are exactly `count`.
bool ParseNumbers(const std::string& text, size_t count, float* out) {
  size_t at = 0;
  for (size_t i = 0; i < count; ++i) {
    if (at > text.size()) return false;
    char* end = nullptr;
    out[i] = std::strtof(text.c_str() + at, &end);
    if (end == text.c_str() + at) return false;
    at = size_t(end - text.c_str());
    if (i + 1 < count) {
      if (at >= text.size() || text[at] != ',') return false;
      ++at;
    }
  }
  return at == text.size();
}

void PrintLamp(const LampTable& table, uint32_t i, const char* extra = "") {
  const Lamp& l = table[i];
  static const char* kSource[] = {"file", "live fill", "street lamp"};
  std::printf("  #%u file %d %s %s at (%.2f %.2f %.2f) colour (%.3f %.3f %.3f) intensity %.1f cone %.1f spread %.1f dir (%.3f %.3f %.3f)%s\n", i, l.file_index == kNoLampIndex ? -1 : int(l.file_index),
              kSource[size_t(l.source) % 3], l.is_spot() ? "spot" : "omni", l.pos[0], l.pos[1], l.pos[2], l.colour[0], l.colour[1], l.colour[2], l.intensity, l.cone_deg, l.spread_deg, l.dir[0],
              l.dir[1], l.dir[2], extra);
}

// lights.xlm out of the archive the way the builder does it: the mod archive's first.
bool ReadLightsFromArchive(const Args& args, std::vector<Lamp>* lamps, LampParseStats* parse, bool* from_mods, std::string* error) {
  const fs::path source = fs::path(args.game) / "xarchive_cache.rpf";
  fs::path mods = args.mods.empty() ? fs::path(args.game) / "xarchive_mods.rpf" : fs::path(args.mods);
  mc::modloader::Rpf3Reader archive, mod_archive;
  if (!archive.Open(source)) {
    *error = "the game archive " + source.string() + " could not be opened";
    return false;
  }
  const bool has_mods = fs::exists(mods) && mod_archive.Open(mods);
  mc::modloader::Rpf3Entry entry;
  std::vector<uint8_t> file;
  *from_mods = false;
  if (has_mods && mod_archive.Find(kLightsPath, entry) && !entry.is_directory() && mod_archive.ReadFile(entry, file)) {
    *from_mods = true;
  } else if (!(archive.Find(kLightsPath, entry) && !entry.is_directory() && archive.ReadFile(entry, file))) {
    *error = std::string(kLightsPath) + " is not in the archive";
    return false;
  }
  RscImage image;
  const LzxFunction lzx = [](const uint8_t* src, size_t src_len, std::vector<uint8_t>& dst, size_t dst_len) { return mc::modloader::LzxDecompress(src, src_len, dst, dst_len); };
  return DecodeResourceFile(file, lzx, &image, error) && ParseLightsResource(image, lamps, parse, error);
}

int Lamps(const Args& args) {
  LampTable table;
  LampCensus census;
  std::string error;
  bool from_archive = false;
  if (!args.game.empty()) {
    std::vector<Lamp> lamps;
    LampParseStats parse;
    bool from_mods = false;
    if (!ReadLightsFromArchive(args, &lamps, &parse, &from_mods, &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    census = CensusOfLamps(lamps, parse);
    census.from_mods = from_mods;
    table = LampTable::Build(lamps, kDefaultLampCellMetres, from_mods ? kLampTableFromMods : 0);
    from_archive = true;
    std::printf("%s", LampCensusReport(census).c_str());
  } else if (!args.positional.empty()) {
    PackReader reader;
    if (!reader.Open(args.positional[0], &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    if (!reader.has_lamps()) {
      std::fprintf(stderr, "this pack has no lamps section (built by builder version %u)\n", reader.header().builder_version);
      return 1;
    }
    if (!reader.ReadLamps(&table, &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    std::printf("lamps section: %llu bytes at %llu\n", static_cast<unsigned long long>(reader.header().lamps_size), static_cast<unsigned long long>(reader.header().lamps_offset));
  } else {
    std::fprintf(stderr, "lamps --game <folder> [--golden]   or   lamps <pack file> [--near x,z,r] [--strongest x,y,z,r,n [--hour h]] [--cell x,z]\n");
    return 2;
  }
  const LampStats st = table.Stats();
  std::printf("table: %u lamps (%u spot, %u omni; file %u, live fill %u, street %u), grid %u x %u cells of %.0f m from (%.0f, %.0f), hash %016llx, section %zu bytes\n", st.count, st.spots, st.omni,
              st.by_source[0], st.by_source[1], st.by_source[2], table.grid().nx, table.grid().nz, table.grid().cell_m, table.grid().origin_x, table.grid().origin_z,
              static_cast<unsigned long long>(table.ContentHash()), table.Encode().size());
  int exit_code = 0;
  if (args.golden) {
    if (!from_archive) {
      std::fprintf(stderr, "--golden needs --game\n");
      return 2;
    }
    const LampGolden golden = CheckLampGolden(table, census);
    std::printf("golden:\n%s%s\n", golden.report.c_str(), golden.ok ? "the lamps are the golden ones" : "the lamps DIFFER from the golden ones");
    exit_code = golden.ok ? 0 : 1;
  }
  if (args.simulate_live) {
    if (!from_archive) {
      std::fprintf(stderr, "--simulate-live needs --game\n");
      return 2;
    }
    // the file's lights in the file's order, packed the way the game packs them (CHECK: the packer's own box; this one holds every light and the street lamps' extent)
    const PackingBox box = {{-3470.0f, -31.0f, -2360.0f}, {2830.0f, 380.0f, 2110.0f}};
    std::vector<const Lamp*> by_index;
    for (const Lamp& l : table.lamps()) by_index.push_back(&l);
    std::sort(by_index.begin(), by_index.end(), [](const Lamp* a, const Lamp* b) { return a->file_index < b->file_index; });
    std::vector<LiveLight> live;
    for (const Lamp* l : by_index) live.push_back(SimulateGamePacking(*l, false, box));
    MergeReport ordered, shuffled_report;
    const LampTable merged = MergeLive(table, live, MergeOptions{}, &ordered);
    std::mt19937_64 rng(2026);
    std::shuffle(live.begin(), live.end(), rng);
    const LampTable merged_shuffled = MergeLive(table, live, MergeOptions{}, &shuffled_report);
    const uint32_t n = table.size();
    std::printf("simulated live read, file order: %s\nsimulated live read, shuffled:   %s\n", ordered.Summary().c_str(), shuffled_report.Summary().c_str());
    const bool ok = ordered.matched == n && ordered.matched_by_ordinal == n && ordered.live_only_fill == 0 && ordered.file_only == 0 && shuffled_report.matched == n &&
                    shuffled_report.live_only_fill == 0 && shuffled_report.file_only == 0 && merged.Encode() == merged_shuffled.Encode() && merged.size() == n;
    std::printf("%s\n", ok ? "the merge rule matches every file light 1:1, in either order" : "the merge rule did NOT match every file light 1:1");
    if (!ok) exit_code = 1;
  }
  float v[5];
  if (!args.near_query.empty()) {
    if (!ParseNumbers(args.near_query, 3, v)) {
      std::fprintf(stderr, "--near x,z,r\n");
      return 2;
    }
    std::vector<uint32_t> near;
    table.Near(v[0], v[1], v[2], &near);
    std::printf("%zu lamps within %.1f m of (%.1f, %.1f):\n", near.size(), v[2], v[0], v[1]);
    for (const uint32_t i : near) PrintLamp(table, i);
  }
  if (!args.cell_query.empty()) {
    if (!ParseNumbers(args.cell_query, 2, v)) {
      std::fprintf(stderr, "--cell x,z\n");
      return 2;
    }
    uint32_t cx = 0, cz = 0;
    if (!table.CellOf(v[0], v[1], &cx, &cz)) {
      std::printf("(%.1f, %.1f) is outside the grid\n", v[0], v[1]);
    } else {
      const CellEmitter e = table.EmitterOfCell(cx, cz);
      std::printf("cell (%u, %u): %u lamps, centre (%.1f %.1f %.1f), radiance (%.1f %.1f %.1f), reach %.1f m\n", cx, cz, e.count, e.centre[0], e.centre[1], e.centre[2], e.radiance[0], e.radiance[1],
                  e.radiance[2], e.reach_m);
    }
  }
  if (!args.strongest_query.empty()) {
    if (!ParseNumbers(args.strongest_query, 5, v)) {
      std::fprintf(stderr, "--strongest x,y,z,r,n\n");
      return 2;
    }
    LampQuery query;
    if (!args.hour.empty()) query.hour = std::strtof(args.hour.c_str(), nullptr);
    const float p[3] = {v[0], v[1], v[2]};
    const std::vector<LampPick> picks = table.Strongest(p, v[3], uint32_t(std::max(v[4], 0.0f)), query);
    const std::string at_hour = args.hour.empty() ? std::string() : " at hour " + args.hour;
    std::printf("the %zu strongest lamps at (%.1f %.1f %.1f) within %.1f m%s:\n", picks.size(), p[0], p[1], p[2], v[3], at_hour.c_str());
    for (const LampPick& pick : picks) {
      char extra[64];
      std::snprintf(extra, sizeof(extra), "  score %.3f at %.1f m", pick.score, pick.distance);
      PrintLamp(table, pick.index, extra);
    }
  }
  return exit_code;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "citypack_tool build|census|check|validate|info|lamps|build-hit|check-hit|check-tex|tex-census|tex-build (see the top of main.cpp)\n");
    return 2;
  }
  const std::string command = argv[1];
  Args args;
  if (!ParseArgs(argc, argv, 2, &args)) return 2;

  if (command == "info") return Info(args);
  if (command == "check-tex") {
    if (args.out.empty()) {
      std::fprintf(stderr, "--out (the folder with the hit pack and the texture file) is required\n");
      return 2;
    }
    HitDataCheckParts parts;
    parts.pack = false;
    parts.hit = false;
    const HitDataCheck check = DeepCheckHitData(args.out, parts, nullptr);
    if (check.unreadable) {
      std::fprintf(stderr, "%s\n", check.error.c_str());
      return 1;
    }
    for (const std::string& p : check.problems) std::fprintf(stderr, "  %s\n", p.c_str());
    std::printf("%u problems in %.2f s (%u pictures)\n", check.texture_problems, check.texture_seconds, check.pictures);
    return check.ok ? 0 : 1;
  }
  if (command == "tex-census") return TextureSetCommand(args, false);
  if (command == "tex-build") return TextureSetCommand(args, true);
  if (command == "lamps") return Lamps(args);
  if (command == "validate") {
    if (args.positional.empty()) {
      std::fprintf(stderr, "validate <pack file>\n");
      return 2;
    }
    PackReader reader;
    std::string error;
    if (!reader.Open(args.positional[0], &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    const auto t = std::chrono::steady_clock::now();
    std::vector<std::string> problems;
    const uint32_t bad = ValidatePack(reader, &problems);
    for (const std::string& p : problems) std::fprintf(stderr, "  %s\n", p.c_str());
    std::printf("%u problems in %.1f s\n", bad, std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count());
    return bad == 0 ? 0 : 1;
  }

  BuildRequest request;
  if (!MakeRequest(args, &request)) return 2;
  if (command == "build-hit" || command == "check-hit") {
    if (args.out.empty()) {
      std::fprintf(stderr, "--out (the city pack's folder) is required\n");
      return 2;
    }
    HitDataRequest hit;
    hit.base = request;
    hit.base.census_only = command == "build-hit" && args.census;
    for (const std::string& r : args.rows) hit.texture_rows.emplace_back(r);
    if (args.uv_step >= 0) hit.rules.uv_step_log2 = uint32_t(args.uv_step);
    if (args.ao_bits >= 0) hit.rules.ao_bits = uint32_t(args.ao_bits);
    if (args.tint_bits >= 0) hit.rules.tint_bits = uint32_t(args.tint_bits);
    if (args.zstd >= 0) hit.zstd_level = args.zstd;
    hit.textures = !args.no_textures;
    if (command == "check-hit") {
      const StampCheck check = CheckHitData(hit);
      std::printf("%s%s\n", check.current ? "current" : "not current: ", check.current ? "" : check.why.c_str());
      if (!check.current) return 1;
      if (!args.deep) return 0;
      HitDataCheckParts parts;
      parts.pack = false;
      parts.textures = false;
      const HitDataCheck deep = DeepCheckHitData(args.out, parts, nullptr);
      if (deep.unreadable) {
        std::fprintf(stderr, "%s\n", deep.error.c_str());
        return 1;
      }
      for (const std::string& p : deep.problems) std::fprintf(stderr, "  %s\n", p.c_str());
      std::printf("%u problems in %.1f s\n", deep.hit_problems, deep.hit_seconds);
      return deep.ok ? 0 : 1;
    }
    PrintSink sink;
    const HitDataResult result = BuildHitData(hit, &sink);
    if (!result.ok) {
      std::fprintf(stderr, "failed: %s\n", result.error.c_str());
      return 1;
    }
    if (result.textures_built) {
      const TextureSetStats& t = result.texture_stats;
      std::printf("texture set: %u pictures (%llu data bytes, %llu without zstd; zstd level %d took %.2f s) from %llu dictionary entries, %u distinct names, %.1f s\n", t.distinct,
                  static_cast<unsigned long long>(t.data_bytes), static_cast<unsigned long long>(t.plain_bytes), t.zstd_level, t.compress_seconds, static_cast<unsigned long long>(t.entries),
                  t.distinct_names, t.seconds);
    }
    if (result.up_to_date) {
      std::printf("current: nothing built\n");
      return 0;
    }
    std::printf("%s", result.hit.census.Report().c_str());
    std::printf("total %.1f s%s\n", result.hit.seconds, args.census ? " (census: nothing written)" : "");
    if (!args.census) std::printf("hit pack %s: %llu bytes\n", result.hit.hit_path.string().c_str(), static_cast<unsigned long long>(result.hit.hit_bytes));
    return 0;
  }
  if (command == "check") {
    if (args.out.empty()) {
      std::fprintf(stderr, "--out is required\n");
      return 2;
    }
    const StampCheck check = CheckCityPack(request);
    std::printf("%s%s%s\n", check.current ? "current" : "not current: ", check.current ? "" : check.why.c_str(), "");
    return check.current ? 0 : 1;
  }
  if (command == "census") request.census_only = true;
  else if (command != "build") {
    std::fprintf(stderr, "unknown command %s\n", command.c_str());
    return 2;
  }
  if (command == "build" && args.out.empty()) {
    std::fprintf(stderr, "--out is required\n");
    return 2;
  }
  PrintSink sink;
  const BuildResult result = BuildCityPack(request, &sink);
  if (!result.ok) {
    std::fprintf(stderr, "failed: %s\n", result.error.c_str());
    return 1;
  }
  if (result.up_to_date) {
    std::printf("current: nothing built\n");
    return 0;
  }
  std::printf("%s", result.census.Report().c_str());
  std::printf("total %.1f s%s\n", result.seconds, command == "build" ? "" : " (census: nothing written)");
  if (command == "build") std::printf("pack %s: %llu bytes\n", result.pack_path.string().c_str(), static_cast<unsigned long long>(result.pack_bytes));
  return 0;
}
