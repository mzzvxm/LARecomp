// citypack_tool: the city pack builder from the command line (dev tool; the app and the launcher call the same library).
//
//   citypack_tool build    --game <folder with xarchive_cache.rpf> --out <folder> [--mods <xarchive_mods.rpf>] [--threads N] [--force]
//                          [--positions float|grid] [--keep decal,interior,water,...]
//   citypack_tool census   --game <folder> [--threads N] [--keep ...]      read every sector, write nothing, print the census (golden totals, classes,
//                                                                          every shader, buckets, timings)
//   citypack_tool check    --game <folder> --out <folder>                  stat only: is the pack current? (exit 0 = yes)
//   citypack_tool validate <pack file>                                     decode every blob and compare with the table (exit 0 = whole)
//   citypack_tool info     <pack file> [sector name]                       the header, the totals, or one sector's record and mesh
//   citypack_tool lamps    --game <folder> [--golden]                      read lights.xlm from the archive (the mod archive's first), print the census and the 64 m grid;
//                                                                          --golden: exit 1 unless the counts are the ones the survey and this file's reading found
//                          --simulate-live: the merge rule (lamp_merge.h) on the real file: every light packed the way the game packs it (a model: 16 bit position steps,
//                                                                          half floats), merged in the file's order and in a shuffled one: all 15,174 must match, 1:1
//   citypack_tool lamps    <pack file> [--near x,z,r] [--strongest x,y,z,r,n [--hour h]] [--cell x,z]
//                                                                          the pack's lamps section: its totals, the lamps around a point, the strongest at a point,
//                                                                          one cell's emitter
//
// Reads the player's own game files; the pack it writes is game-derived and is never shipped or committed.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "citypack/builder.h"
#include "citypack/lamp_merge.h"
#include "citypack/lamp_parse.h"
#include "citypack/pack_reader.h"
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
  std::string near_query, strongest_query, cell_query, hour;
  uint32_t threads = 0;
  bool force = false;
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
    else if (a == "--threads") { std::string t; if (!value(&t)) return false; args->threads = uint32_t(std::strtoul(t.c_str(), nullptr, 10)); }
    else if (a == "--near") { if (!value(&args->near_query)) return false; }
    else if (a == "--strongest") { if (!value(&args->strongest_query)) return false; }
    else if (a == "--cell") { if (!value(&args->cell_query)) return false; }
    else if (a == "--hour") { if (!value(&args->hour)) return false; }
    else if (a == "--golden") args->golden = true;
    else if (a == "--simulate-live") args->simulate_live = true;
    else if (a == "--force") args->force = true;
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

int Info(const Args& args) {
  if (args.positional.empty()) {
    std::fprintf(stderr, "info <pack file> [sector name]\n");
    return 2;
  }
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
    std::fprintf(stderr, "citypack_tool build|census|check|validate|info (see the top of main.cpp)\n");
    return 2;
  }
  const std::string command = argv[1];
  Args args;
  if (!ParseArgs(argc, argv, 2, &args)) return 2;

  if (command == "info") return Info(args);
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
