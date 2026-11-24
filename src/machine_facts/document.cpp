#include "document.h"

#include "citypack/file_replace.h"
#include "mc_engine/online/online_tool/json_writer.h"

#include <bit>
#include <fstream>

namespace larecomp::machine {
namespace {

using online_tool::JsonWriter;

constexpr uint32_t kRound[] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

const char* Vendor(uint32_t id) {
  switch (id) {
    case 0x10de: return "nvidia";
    case 0x1002: return "amd";
    case 0x8086: return "intel";
    case 0x106b: return "apple";
    case 0x5143: return "qualcomm";
    case 0x1414: return "microsoft";
    default: return "other";
  }
}

template <typename T>
void Field(JsonWriter& out, std::string_view name, const std::optional<T>& value) {
  if (value) out.Field(name, *value);
  else out.Key(name).Null();
}

void Text(JsonWriter& out, std::string_view name, std::string_view value) {
  if (value.empty()) out.Key(name).Null();
  else out.Field(name, value);
}

void WriteDisplay(JsonWriter& out, const Display& display) {
  out.BeginObject().Field("name", display.name)
      .Field("width", display.width).Field("height", display.height)
      .Field("size", std::to_string(display.width) + "X" + std::to_string(display.height));
  if (display.hz > 0) out.Key("refresh_hz").Double(display.hz);
  Field(out, "hdr", display.hdr);
  out.EndObject();
}

template <size_t Count>
void Sizes(JsonWriter& out, const std::vector<std::array<uint32_t, Count>>& sizes) {
  out.BeginArray();
  for (const auto& size : sizes) {
    out.BeginArray();
    for (uint32_t value : size) out.Uint(value);
    out.EndArray();
  }
  out.EndArray();
}

void WriteGpu(JsonWriter& out, const GpuFacts& gpu, const Adapter* selected) {
  out.Key("gpu").BeginObject();
  if (selected) {
    out.Field("name", selected->name).Field("vendor", Vendor(selected->vendor_id))
        .Field("vendor_id", selected->vendor_id).Field("device_id", selected->device_id)
        .Field("vram_mb", selected->vram_mb).Field("software", selected->software);
    if (!selected->driver_version.empty()) out.Field("driver_version", selected->driver_version);
    Field(out, "shared_memory", selected->shared_memory);
    Field(out, "class", gpu.class_forced ? gpu.class_forced : selected->gpu_class);
    Text(out, "class_why", gpu.class_forced ? "forced by mcla_gpu_class" : selected->class_why);
    Field(out, "handheld", gpu.handheld);
  }
  Field(out, "rtx", gpu.rtx);
  Field(out, "rx9000", gpu.rx9000);
  Field(out, "class_forced", gpu.class_forced);
  Field(out, "rt_tier", gpu.rt_tier);
  Field(out, "rt_rung", gpu.rt_rung);
  Text(out, "rt_tier_why", gpu.rt_tier_why);
  Field(out, "rt_benchmarked", gpu.rt_benchmarked);
  out.Key("adapters").BeginArray();
  // The contract requires one selected entry whenever the list has entries. With no selected
  // device the adapter generator falls back to its offline values.
  if (selected) {
    for (const Adapter& adapter : gpu.adapters) {
      out.BeginObject().Field("name", adapter.name).Field("vendor", Vendor(adapter.vendor_id))
          .Field("vram_mb", adapter.vram_mb).Field("software", adapter.software)
          .Field("selected", &adapter == selected);
      Field(out, "class", adapter.gpu_class);
      out.EndObject();
    }
  }
  out.EndArray().EndObject();

  out.Key("api").BeginObject();
  Text(out, "name", gpu.api);
  Text(out, "version", gpu.api_version);
  Text(out, "shader_model", gpu.shader_model);
  Field(out, "dxr_1_1", gpu.dxr_1_1);
  Field(out, "ray_query", gpu.ray_query);
  Field(out, "car_rt", gpu.car_rt);
  Field(out, "city_rt_shadows", gpu.city_rt_shadows);
  out.EndObject();
}

void WriteFeatures(JsonWriter& out, const Facts& facts) {
  const GpuFacts& gpu = facts.gpu;
  out.Key("features").BeginObject().Key("upscalers").BeginObject();
  Field(out, "fsr1", gpu.fsr1);
  Field(out, "fsr31", gpu.fsr31);
  Field(out, "fsr4", gpu.fsr4);
  Field(out, "dlss", gpu.dlss);
  out.Key("dlss_modes_refused");
  if (gpu.refused_modes) {
    out.BeginArray();
    for (const auto& mode : *gpu.refused_modes) out.String(mode);
    out.EndArray();
  } else {
    out.Null();
  }
  out.EndObject().Key("frame_generation").BeginObject();
  Field(out, "fsr31", gpu.fsr31_fg);
  Field(out, "dlss", gpu.dlss_fg);
  Field(out, "dlss_multiplier_max", gpu.dlss_multiplier);
  Field(out, "dlss_after_restart", gpu.dlss_after_restart);
  out.EndObject().Key("latency").BeginObject();
  Field(out, "kind", gpu.latency);
  Field(out, "auto_on", gpu.latency_auto);
  out.EndObject().Key("denoiser").BeginObject();
  Field(out, "vendor", gpu.denoiser);
  Field(out, "dlss_rr", gpu.dlss_rr);
  Field(out, "ray_regen", gpu.ray_regen);
  out.EndObject().Key("hdr").BeginObject();
  Field(out, "display_capable", facts.hdr_capable);
  Field(out, "windows_hdr_on", facts.os == "windows" && !facts.displays.empty()
                                  ? facts.displays.front().hdr : std::nullopt);
  Field(out, "auto_paper_white_nits", gpu.paper_white);
  Field(out, "auto_peak_nits", gpu.peak_nits);
  out.EndObject();
  Field(out, "pcss", gpu.pcss);
  out.EndObject();
}

void WritePaths(JsonWriter& out, const Facts& facts) {
  out.Key("paths").BeginObject();
  const auto path = [&](const char* key, const std::filesystem::path& value) {
    if (!value.empty()) out.Field(key, PathText(value));
  };
  path("game_folder", facts.game_folder);
  path("config_file", facts.config_file);
  path("user_root", facts.user_root);
  path("saves_root", facts.saves_root);
  path("cache_root", facts.cache_root);
  path("logs", facts.logs);
  path("game_data_root", facts.game_data_root);
  path("mod_root", facts.mod_root);
  path("strings_replace", facts.strings_replace);
  out.Key("mod_roots_ignored").BeginArray();
  for (const auto& value : facts.mod_roots_ignored) out.String(PathText(value));
  out.EndArray().Key("music").BeginArray();
  for (const auto& value : facts.music) out.String(PathText(value));
  out.EndArray().Field("title_id", facts.title_id).Field("profile_id", facts.profile_id).EndObject();
}

}  // namespace

std::string Sha256(std::string_view bytes) {
  // FIPS 180-4, SHA-256 padding, message schedule and compression.
  std::string padded(bytes);
  const uint64_t bits = uint64_t(bytes.size()) * 8;
  padded.push_back(char(0x80));
  while (padded.size() % 64 != 56) padded.push_back('\0');
  for (int shift = 56; shift >= 0; shift -= 8) padded.push_back(char(bits >> shift));

  std::array<uint32_t, 8> hash = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  for (size_t offset = 0; offset < padded.size(); offset += 64) {
    uint32_t words[64] = {};
    for (size_t i = 0; i < 64; ++i) {
      const auto byte = static_cast<unsigned char>(padded[offset + i]);
      words[i / 4] = (words[i / 4] << 8) | byte;
    }
    for (size_t i = 16; i < 64; ++i) {
      const uint32_t x = words[i - 15], y = words[i - 2];
      const uint32_t s0 = std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3);
      const uint32_t s1 = std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10);
      words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }
    auto [a, b, c, d, e, f, g, h] = hash;
    for (size_t i = 0; i < 64; ++i) {
      const uint32_t choose = (e & f) ^ (~e & g);
      const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
      const uint32_t sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
      const uint32_t t1 = h + sum1 + choose + kRound[i] + words[i];
      const uint32_t t2 = sum0 + majority;
      h = g; g = f; f = e; e = d + t1;
      d = c; c = b; b = a; a = t1 + t2;
    }
    const uint32_t state[] = {a, b, c, d, e, f, g, h};
    for (size_t i = 0; i < hash.size(); ++i) hash[i] += state[i];
  }
  constexpr char kHex[] = "0123456789abcdef";
  std::string hex;
  for (uint32_t word : hash) {
    for (int shift = 28; shift >= 0; shift -= 4) hex += kHex[(word >> shift) & 15];
  }
  return hex;
}

std::string HardwareId(const Adapter& adapter, uint32_t os_build) {
  return Sha256(adapter.name + '\0' + std::to_string(adapter.vendor_id) + '\0' +
                std::to_string(adapter.device_id) + '\0' + adapter.driver_version + '\0' +
                std::to_string(os_build));
}

std::string PathText(const std::filesystem::path& path) {
  const auto utf8 = path.u8string();
  return {utf8.begin(), utf8.end()};
}

bool IsFinal(bool probe, const GpuFacts& gpu) {
  return !probe && !gpu.pending;
}

std::string Serialize(const Facts& facts, bool probe, std::string_view written_utc) {
  const GpuFacts& gpu = facts.gpu;
  const Adapter* selected = gpu.selected && *gpu.selected < gpu.adapters.size()
                                ? &gpu.adapters[*gpu.selected] : nullptr;
  JsonWriter out(true);
  out.BeginObject().Field("schema", "larecomp.machine").Field("schema_version", 1)
      .Field("min_reader", 1).Field("source", probe ? "probe" : "game")
      .Field("final", IsFinal(probe, gpu)).Key("written_by").BeginObject()
      .Field("game_version", facts.game_version);
  if (!facts.commit.empty()) out.Field("commit", facts.commit);
  out.EndObject();
  if (!written_utc.empty()) out.Field("written_utc", written_utc);
  if (selected) out.Field("hardware_id", HardwareId(*selected, facts.os_build));
  WriteGpu(out, gpu, selected);
  WriteFeatures(out, facts);

  out.Key("display").BeginObject();
  if (!facts.displays.empty()) WriteDisplay(out.Key("primary"), facts.displays.front());
  out.Key("list").BeginArray();
  for (const auto& display : facts.displays) WriteDisplay(out, display);
  out.EndArray();
  Sizes(out.Key("windowed_sizes"), facts.windowed_sizes);
  Sizes(out.Key("exclusive_sizes"), facts.exclusive_sizes);
  out.EndObject().Key("os").BeginObject().Field("name", facts.os).Field("build", facts.os_build)
      .Field("exclusive_fullscreen", facts.exclusive_fullscreen)
      .Field("live_window_resize", facts.live_window_resize)
      .Field("window_size_fixed", facts.window_size_fixed).Field("hdr_output", facts.hdr_output)
      .Field("real_weather", facts.real_weather).Field("mouse_look", facts.mouse_look)
      .Field("wheel_ffb", facts.wheel_ffb).Field("frame_pacing_wake", facts.frame_pacing_wake)
      .EndObject().Key("cpu").BeginObject().Field("threads", facts.cpu_threads)
      .Field("level", facts.cpu_level).EndObject().Key("build").BeginObject();
  Field(out, "rt_gi_update_built", gpu.gi_update);
  out.EndObject().Key("install").BeginObject();
  Field(out, "portuguese_text", facts.portuguese_text);
  Field(out, "fidelityfx_files", gpu.fidelityfx_files);
  Field(out, "streamline_files", gpu.streamline_files);
  Field(out, "city_pack", facts.city_pack);
  Field(out, "city_hit", facts.city_hit);
  Field(out, "shader_pack", facts.shader_pack);
  out.EndObject();
  WritePaths(out, facts);
  return out.EndObject().str() + '\n';
}

bool WriteDocument(const std::filesystem::path& path, std::string_view bytes, std::string& error) {
  const auto temporary = citypack::TempPathFor(path);
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();
  if (!output) {
    error = "could not write the temporary file";
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return false;
  }
  return citypack::MoveIntoPlace(temporary, path, &error, {.max_attempts = 1}) !=
         citypack::MoveResult::kFailed;
}

}  // namespace larecomp::machine
