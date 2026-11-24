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
    "mcla_gpu_rtao_scale", "mcla_gpu_rtao_alpha", "mcla_gpu_rtao_screen",
    "mcla_gpu_rtao_interior", "mcla_gpu_rtao_check", "mcla_gpu_rtao_view",
    "mcla_gpu_rtao_history", "mcla_gpu_rtao_denoise", "mcla_gpu_rtao_raw_scale",
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
}

void FrameChecks(Checks& checks, const fs::path& gpu) {
  const auto ao = Read(gpu / "ao.cpp");
  const auto order = Read(gpu / "frame_order.h");
  const auto rules = Read(gpu / "rtao_rules.h");
  const auto trace = Read(gpu / "rtao.cpp");
  const auto filter = Read(gpu / "rtao_denoise.cpp");
  const auto compose = Read(gpu / "rtao_compose.cpp");
  const auto has = [&](const std::string& code, const char* pattern, std::string_view what) {
    checks.Expect(std::regex_search(code, std::regex(pattern)), what);
  };
  const auto trace_call = ao.find("rtao_->RecordTrace(");
  const auto filter_call = ao.find("rtao_->RecordFilter(");
  const auto visibility = ao.find(R"rx("ao visibility")rx", filter_call);
  const auto compose_call = ao.find("rtao_->RecordCompose(");
  const auto contact = ao.find("RecordContact(", compose_call);
  checks.Expect(trace_call < filter_call && filter_call < visibility && visibility < compose_call &&
                    compose_call < contact && contact != std::string::npos,
                "AO calls trace, filter, visibility, compose, contact in frame order");
  for (const auto* label : {"rtao trace", "rtao denoise", "rtao compose"}) {
    // CHECK: verify that all three labels survive the full pass-timing summary on the first cost run.
    const std::string literal = "\"" + std::string(label) + "\"";
    checks.Expect(order.contains(literal), std::string("frame-order label ") + label);
    checks.Expect(ao.contains(literal) || trace.contains(literal) || filter.contains(literal) || compose.contains(literal),
                  std::string("renderer label ") + label);
  }
  has(order, R"rx(\bkRtAoSignal\s*=\s*1u\s*<<\s*31\b)rx", "trace signal uses bit 31");
  has(ao, R"rx(car_rt_ao::OcclusionOn\s*\(\s*plan\.occlusion\s*,\s*plan\.rtao\.on\s*,)rx",
      "car occlusion folds only in an RTAO frame");
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
