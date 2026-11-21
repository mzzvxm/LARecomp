#include "citypack/hit_data.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "citypack/pack_reader.h"
#include "citypack/stamp.h"

namespace larecomp::citypack {

namespace {

using Clock = std::chrono::steady_clock;

double SecondsSince(Clock::time_point started) { return std::chrono::duration<double>(Clock::now() - started).count(); }

// The first few problems of a part go to the list, each with the part's name in front.
void AddProblems(HitDataCheck* check, const char* part, const std::vector<std::string>& found) {
  constexpr size_t kMaxListed = 12;
  for (const std::string& p : found) {
    if (check->problems.size() >= kMaxListed) break;
    check->problems.push_back(std::string(part) + ": " + p);
  }
}

}  // namespace

HitBuildRequest MakeHitBuildRequest(const HitDataRequest& request, HitTextureSource* source) {
  HitBuildRequest hit;
  hit.base = request.base;
  hit.texture_rows = request.texture_rows;
  hit.rules = request.rules;
  hit.zstd_level = request.zstd_level;
  hit.textures = request.textures ? source : nullptr;
  return hit;
}

StampCheck CheckHitData(const HitDataRequest& request) {
  CityTextureSource identity;  // no set: its identity is all the currency check needs
  return CheckCityHitPack(MakeHitBuildRequest(request, &identity));
}

HitDataResult BuildHitData(const HitDataRequest& request, ProgressSink* sink_in) {
  ProgressSink fallback;
  ProgressSink& sink = sink_in ? *sink_in : fallback;
  HitDataResult result;
  const auto started = Clock::now();
  const auto finish = [&] {
    result.seconds = SecondsSince(started);
    return result;
  };
  const auto fail = [&](std::string why) {
    result.ok = false;
    result.error = std::move(why);
    sink.Log(LogLevel::kError, result.error);
    return finish();
  };
  const BuildRequest& base = request.base;
  if (!base.lzx) return fail("no LZX decoder was given to the city hit data build");
  if (!StatFile(base.source_rpf).exists) {
    result.no_archive = true;
    return fail("the game archive " + base.source_rpf.string() + " is not there");
  }
  {
    // Before the texture set is built (seconds of work): the hit data goes over the city pack, and without one there is nothing to do.
    PackReader pack;
    std::string error;
    if (!pack.Open(base.out_dir / kPackFileName, &error)) {
      result.no_pack = true;
      return fail("the city pack the hit data is built over is not there or is damaged (" + error + "): build the city pack first");
    }
  }

  CityTextureSource source;
  TextureSetResult texture_result;
  HitBuildRequest hit = MakeHitBuildRequest(request, &source);
  // The texture set is built when the files are not current (or a census/force asks): it is the long part, so a current stamp costs none of it.
  if (hit.textures != nullptr && (base.census_only || base.force || !CheckCityHitPack(hit).current)) {
    TextureSetRequest texture_request;
    texture_request.source_rpf = base.source_rpf;
    texture_request.mods_rpf = base.mods_rpf;
    texture_request.lzx = base.lzx;
    texture_request.threads = base.threads;
    texture_request.worker_start = base.worker_start;
    texture_request.zstd_level = request.zstd_level;
    texture_result = BuildTextureSet(texture_request, &sink);
    if (!texture_result.ok) {
      if (texture_result.cancelled) {
        result.cancelled = true;
        result.error = "cancelled";
        return finish();
      }
      return fail("the texture set: " + texture_result.error);
    }
    result.textures_built = true;
    result.texture_stats = texture_result.stats;
    source.SetTextureSet(&texture_result.set);
  }

  result.hit = BuildCityHitPack(hit, &sink);
  if (!result.hit.ok) {
    if (result.hit.cancelled) {
      result.cancelled = true;
      result.error = "cancelled";
      return finish();
    }
    // BuildCityHitPack has told the sink already.
    result.ok = false;
    result.error = result.hit.error;
    return finish();
  }
  result.ok = true;
  result.up_to_date = result.hit.up_to_date;
  if (!result.up_to_date && !base.census_only && request.textures) {
    const FileStat tex = StatFile(base.out_dir / kTexFileName);
    result.texture_bytes = tex.exists ? tex.size : 0;
  }
  return finish();
}

HitDataCheck DeepCheckHitData(const std::filesystem::path& folder, const HitDataCheckParts& parts, ProgressSink* sink_in) {
  ProgressSink fallback;
  ProgressSink& sink = sink_in ? *sink_in : fallback;
  HitDataCheck check;
  const auto unreadable = [&](const std::string& what) {
    check.ok = false;
    check.unreadable = true;
    check.error = what;
    sink.Log(LogLevel::kError, what);
    return check;
  };
  std::string error;

  // The city pack alone (the base of everything else).
  PackReader base;
  if (parts.pack || parts.hit) {
    if (!base.Open(folder / kPackFileName, &error)) return unreadable("the city pack cannot be read: " + error);
  }
  if (parts.pack) {
    sink.BeginPhase("checking the city pack", 1);
    const auto t = Clock::now();
    std::vector<std::string> found;
    check.pack_problems = ValidatePack(base, &found);
    check.pack_seconds = SecondsSince(t);
    AddProblems(&check, "city pack", found);
    sink.Step("city pack");
  }

  HitPackReader hit;
  if (parts.hit || parts.textures) {
    if (!hit.Open(folder / kHitFileName, &error)) return unreadable("the city hit file cannot be read: " + error);
  }
  if (parts.hit) {
    sink.BeginPhase("checking the city hit file", 1);
    const auto t = Clock::now();
    std::vector<std::string> found;
    check.hit_problems = ValidateHitPack(hit, base, &found);
    check.hit_seconds = SecondsSince(t);
    AddProblems(&check, "hit file", found);
    sink.Step("city hit file");
  }
  if (parts.textures) {
    check.pictures = uint32_t(hit.textures().size());
    const std::filesystem::path texture_file = folder / kTexFileName;
    if (hit.textures().empty() && !StatFile(texture_file).exists) {
      check.textures_skipped = true;  // a hit file built without textures
    } else {
      sink.BeginPhase("checking the city textures", 1);
      const auto t = Clock::now();
      std::vector<std::string> found;
      check.texture_problems = ValidateTextureFile(texture_file, hit.textures(), hit.header().table_hash, &found);
      check.texture_seconds = SecondsSince(t);
      AddProblems(&check, "texture file", found);
      sink.Step("city textures");
    }
  }
  check.ok = check.pack_problems == 0 && check.hit_problems == 0 && check.texture_problems == 0;
  return check;
}

}  // namespace larecomp::citypack
