// citypack_tool: the city pack builder from the command line (dev tool; the app and the launcher call the same library).
//
//   citypack_tool build    --game <folder with xarchive_cache.rpf> --out <folder> [--mods <xarchive_mods.rpf>] [--threads N] [--force]
//                          [--positions float|grid] [--keep decal,interior,water,...]
//   citypack_tool census   --game <folder> [--threads N] [--keep ...]      read every sector, write nothing, print the census (golden totals, classes,
//                                                                          every shader, buckets, timings)
//   citypack_tool check    --game <folder> --out <folder>                  stat only: is the pack current? (exit 0 = yes)
//   citypack_tool validate <pack file>                                     decode every blob and compare with the table (exit 0 = whole)
//   citypack_tool info     <pack file> [sector name]                       the header, the totals, or one sector's record and mesh
//
// Reads the player's own game files; the pack it writes is game-derived and is never shipped or committed.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "citypack/builder.h"
#include "citypack/pack_reader.h"
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
  uint32_t threads = 0;
  bool force = false;
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
