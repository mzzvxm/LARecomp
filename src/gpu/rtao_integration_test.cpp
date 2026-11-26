// Run from any directory with the LARecomp src/gpu directory as an optional argument.
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <regex>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
namespace {
constexpr std::array Settings{
    "mcla_gpu_rtao_rays", "mcla_gpu_rtao_reach", "mcla_gpu_rtao_range",
    "mcla_gpu_rtao_scale", "mcla_gpu_rtao_quarter_at_4k", "mcla_gpu_rtao_alpha", "mcla_gpu_rtao_screen",
    "mcla_gpu_rtao_interior", "mcla_gpu_rtao_check", "mcla_gpu_rtao_view",
    "mcla_gpu_rtao_history", "mcla_gpu_rtao_denoise", "mcla_gpu_rtao_spatial_resolve", "mcla_gpu_rtao_reuse_trace_validity", "mcla_gpu_rtao_raw_scale",
    "mcla_gpu_dlss_rr_rt_ao", "mcla_gpu_rtao_blend_car",
};

std::string Read(const fs::path& file) {
  std::ifstream in(file);
  const std::string src{std::istreambuf_iterator<char>(in), {}};
  std::string code;
  code.reserve(src.size());
  char quote = 0;
  for (size_t i = 0; i < src.size(); ++i) {
    const char c = src[i];
    if (quote) {
      code += c;
      if (c == quote) quote = 0;
      if (c == '\\' && i + 1 < src.size()) code += src[++i];
    } else if (src.compare(i, 2, "//") == 0) {
      i = src.find('\n', i);
      if (i == std::string::npos) break;
      code += '\n';
    } else if (src.compare(i, 2, "/*") == 0) {
      i = src.find("*/", i + 2);
      if (i == std::string::npos) break;
      ++i;
      code += ' ';
    } else {
      const bool digit_separator = c == '\'' && i > 0 &&
                                   std::isdigit(static_cast<unsigned char>(src[i - 1]));
      if (c == '"' || (c == '\'' && !digit_separator)) quote = c;
      code += c;
    }
  }
  return code;
}

size_t count(const std::string& code, std::string_view needle) {
  size_t found = 0;
  for (size_t at = code.find(needle); at != std::string::npos; at = code.find(needle, at + needle.size())) ++found;
  return found;
}

struct Checks {
  int total = 0;
  int failed = 0;
  void Expect(bool ok, std::string_view what) {
    ++total;
    if (ok) return;
    ++failed;
    std::cerr << "FAIL " << what << '\n';
  }
};

void SettingsChecks(Checks& checks, const fs::path& src) {
  const auto definitions = Read(src / "settings/generated/settings_gpu.h");
  std::map<std::string, unsigned> reads;
  const std::regex read(R"rx(\b(mcla_gpu_rtao_\w+|mcla_gpu_dlss_rr_rt_ao)\s*\.\s*Get\s*\()rx");
  for (const auto& file : fs::recursive_directory_iterator(src)) {
    const auto& path = file.path();
    if (!file.is_regular_file() || (path.extension() != ".cpp" && path.extension() != ".h") ||
        path.stem().string().ends_with("_test") || path.parent_path().filename() == "generated")
      continue;
    const auto code = Read(path);
    for (auto it = std::sregex_iterator(code.begin(), code.end(), read); it != std::sregex_iterator(); ++it)
      ++reads[(*it)[1].str()];
  }
  for (const std::string name : Settings) {
    const std::regex definition("\\bREX_SETTING\\s*\\(\\s*\\w+(?:::\\w+)*\\s*,\\s*" + name + "\\s*,");
    std::smatch match;
    const bool defined = std::regex_search(definitions, match, definition);
    const std::string declaration = defined ? match.suffix().str() : std::string{};
    const auto options = declaration.substr(0, declaration.find("REX_SETTING("));
    checks.Expect(defined && options.contains("rex::kDebugOnly") &&
                  !options.contains("rex::kRequiresRestart") && !options.contains("rex::kInitOnly"),
                  name + " is live and debug-only");
    checks.Expect(reads[name] == 1, name + " has exactly one reader");
    reads.erase(name);
  }
  checks.Expect(reads.empty(), "all RTAO setting readers are registered");
  checks.Expect(definitions.contains(std::string("REX_") + "SETTING(bool, mcla_gpu_rtao_spatial_resolve, false,"),
                "the spatial resolve is opt-in");
}

void FrameChecks(Checks& checks, const fs::path& gpu) {
  const auto ao = Read(gpu / "ao.cpp");
  const auto order = Read(gpu / "frame_order.h");
  const auto rules = Read(gpu / "rtao_rules.h");
  const auto trace = Read(gpu / "rtao.cpp");
  const auto denoise = Read(gpu / "rtao_denoise.cpp");
  const auto consumer = Read(gpu / "hlsl/rt_denoise_ao_consumer.hlsli");
  const auto trace_shader = Read(gpu / "hlsl/rtao_trace_cs.hlsl");
  const auto engine = Read(gpu / "hlsl/rt_denoise_engine.hlsli");
  const auto has = [&](const std::string& code, const char* pattern, std::string_view what) {
    checks.Expect(std::regex_search(code, std::regex(pattern)), what);
  };
  checks.Expect(denoise.contains("kSpatialResolve = 27;") &&
                    denoise.contains("push[kSpatialResolve] = larecomp::settings::mcla_gpu_rtao_spatial_resolve.Get() ? 1u : 0u;") &&
                    consumer.contains("RtAoSpatialResolve() { return RtDnW(27) != 0u; }"),
                "host and spatial shader agree on the live resolve word");
  checks.Expect(denoise.contains("kTraceValidity = 28;") &&
                    consumer.contains("RtAoReadSignalValidity(RtDnW(28) != 0u, temporal, RtDnIteration())") &&
                    trace_shader.contains("judged ? 1.0 : 0.0") &&
                    trace_shader.contains("g_signal[pixel] = float4(1.0, 0.0, 0.0, 1.0);") &&
                    trace_shader.contains("g_signal[pixel] = float4(open, 1.0, float(rays) / 4.0, 1.0);") &&
                    engine.contains("float4(lerp(own.x, filtered.x, amount), own.yzw)"),
                "trace validity matches the guide and survives the spatial passes");
  checks.Expect(engine.contains("!(step == 2u && resolve)") &&
                    engine.contains("RtDnAmbientStep(radius, step, c.length, resolve, c.vehicle != 0u)"),
                "the spatial shader runs the extra bilateral step only on eligible world pixels");
  checks.Expect(engine.contains("extra_step = step == 2u && c.length >= kRtDnMatureLength;") &&
                    engine.contains("if (extra_step) w *= RtAoResolveEdgeWeight(c.colour.x, t.colour.x);"),
                "the extra step applies the stronger edge weight");
  const auto trace_call = ao.find("rtao_->RecordTrace(");
  const auto filter_call = ao.find("rtao_->RecordFilter(");
  const auto visibility = ao.find(R"rx("ao visibility")rx", filter_call);
  const auto compose_call = ao.find("rtao_->RecordCompose(");
  const auto contact = ao.find("RecordContact(", compose_call);
  checks.Expect(trace_call < filter_call && filter_call < visibility && visibility < compose_call &&
                    compose_call < contact && contact != std::string::npos,
                "AO calls trace, filter, visibility, compose, contact in frame order");
  // The three spans are real PassTiming marks in rtao.cpp (a name that only a constant or a comment holds puts nothing in the summary), each once, and one closing mark ends the last.
  for (const auto* label : {"rtao trace", "rtao denoise", "rtao compose"}) {
    // CHECK: verify that all three labels survive the full pass-timing summary on the first cost run.
    const std::string literal = "\"" + std::string(label) + "\"";
    const std::string mark = "timing_->Mark(list, " + literal + ");";
    checks.Expect(order.contains(literal), std::string("frame-order label ") + label);
    checks.Expect(count(trace, mark) == 1, std::string("rtao.cpp marks the span ") + label + " once");
  }
  checks.Expect(count(trace, "\"after rtao\"") == 1 && count(trace, "timing_->Mark(list, \"after rtao\");") == 1,
                "one closing mark ends the RTAO span");
  const auto denoise_mark = trace.find("timing_->Mark(list, \"rtao denoise\");");
  const auto denoise_record = trace.find("denoise_->Record(frame)");
  checks.Expect(denoise_mark != std::string::npos && denoise_mark < denoise_record, "the denoise span opens before the denoiser records");
  const auto compose_mark = trace.find("timing_->Mark(list, \"rtao compose\");");
  const auto compose_record = trace.find("compose_->Record(frame, guides)");
  checks.Expect(compose_mark != std::string::npos && compose_mark < compose_record, "the compose span opens before the compose records");
  // The handoffs between the four branches.
  checks.Expect(count(trace, "rtao_setting::NotePassBuilt(available());") == 1 && count(trace, "rtao_setting::NotePassBuilt(false);") == 1,
                "RtAo publishes its availability to the setting's gate and clears it at shutdown");
  checks.Expect(trace.contains("facts.scale = rtao_denoise::RawScale();"), "a raw frame traces at the denoise setting's scale");
  has(trace, R"rx(const\s+RtAoCompose::Guides\s+guides\s*=\s*\{\s*\.raw\s*=\s*&signal_\s*,\s*\.id_share\s*=\s*&id_share_\s*,\s*\.stencil_slot\s*=\s*stencil_slot_)rx",
      "the compose gets the raw signal, the ID guide and the stencil plane");
  has(ao, R"rx(\.car_term_slot\s*=\s*rt\.recorded\s*&&\s*rt\.occlusion\s*&&\s*rtao_->blend_car_)rx", "the car term reaches the compose only in the A/B arm");
  has(ao, R"rx(in\.compute_read\s*=\s*rtao_runs\s*&&\s*in\.occlusion;)rx", "the car image is made compute-readable where the compose reads it");
  has(Read(gpu / "scene.cpp"), R"rx(ao_->SetRtAoRawMode\(\s*ao_->rtao_wanted\(\)\s*&&\s*temporal_\s*&&\s*temporal_->RawModes\(\)\.RtAo\(\)\s*\))rx",
      "the scene hands the AO the Ray Reconstruction raw mode before it plans the frame");
  has(order, R"rx(\bkRtAoSignal\s*=\s*1u\s*<<\s*31\b)rx", "trace signal uses bit 31");
  has(ao, R"rx(car_rt_ao::OcclusionOn\s*\(\s*plan\.occlusion\s*,\s*rtao_runs\s*,\s*blend_car\s*\))rx",
      "car occlusion folds only in an RTAO frame");
  has(ao, R"rx(record_car\s*\(\s*plan\.rtao\.on\s*\)\s*;)rx", "the first car pass of the frame knows whether RTAO runs");
  has(ao, R"rx(record_car\s*\(\s*false\s*\)\s*;)rx", "the Ultra retry records the car pass with its occlusion back");
  has(ao, R"rx(if\s*\(\s*rtao_slot\s*!=\s*(?:RtAo::)?kNoSlot\s*\)\s*\{?\s*ao_slot\s*=\s*rtao_slot)rx",
      "a valid RTAO slot replaces the existing AO term");
  has(rules, R"rx(if\s*\(\s*!in\.wanted\s*\)[^;{}]*Fallback::kNotWanted)rx",
      "the unwanted plan keeps the identity fallback");
  has(ao, R"rx(\.noise_index\s*=\s*scene_noise::NoiseIndex\s*\(\s*frame_\s*\))rx",
      "AO hands RTAO the per-frame scene noise index");
}
}  // namespace

int main(int argc, char** argv) {
  const fs::path gpu = argc == 2 ? fs::path(argv[1]) : fs::path(__FILE__).parent_path();
  Checks checks;
  SettingsChecks(checks, gpu.parent_path());
  FrameChecks(checks, gpu);
  std::cout << "rtao_integration_test: " << checks.total << " checks, " << checks.failed << " failed\n";
  return checks.failed == 0 ? 0 : 1;
}
