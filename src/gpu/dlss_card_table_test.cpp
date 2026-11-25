// Check that the Streamline groups' `gpu_name` patterns in cmake/deps.json say what the game's own name tests say (LAUNCHER-FOLLOWS-GAME: the launcher's first setup
// decides which Streamline files to fetch before the game can run, so the game hands it the rule as data; this keeps the data and the code from drifting apart).
//
// Against documentation/contract/adapter-name-vectors.json (adapter names with the answers of the game's tests), for every vector:
//   1. the code: gpu.rtx (an NVIDIA adapter, dlss_rr_rules.h NameLooksLikeRtx) and features.frame_generation.dlss_card (an NVIDIA adapter, dlss_fg_card.h NameLooksLikeFrameGeneration)
//      are what the vector says;
//   2. the data: the streamline kit's `core` group (its `gpu_vendor` and its `gpu_name`, searched in the name, ignoring case) applies exactly where the vector says rtx; the
//      `frame_generation` group exactly where it says dlss_card; and each group's `when` (evaluated with the contract's own reference evaluator, for that adapter on Windows) says the
//      same about the vendor as `gpu_vendor` does, so the two statements of the vendor cannot drift apart.
// And the patterns use only the subset of regular expression syntax the schema allows (every reader, C++, C# and Python, then reads them the same way).
//
//   cmake --build <build dir> --target larecomp_dlss_card_table_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_dlss_card_table_test [<repository root>]     (exit 0 = passed)
// or one file: clang++ -std=c++20 -I src/gpu src/gpu/dlss_card_table_test.cpp -o dlss_card_table_test

#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <string>

#include "../../documentation/contract/cpp/mini_json.h"
#include "../../documentation/contract/cpp/ui_expr_ref.h"
#include "dlss_fg_card.h"
#include "dlss_rr_rules.h"

namespace {

namespace json = larecomp::minijson;
namespace uiexpr = larecomp::uiexpr;

int checks = 0;
int failures = 0;

void Check(bool ok, const std::string& what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
  }
}

std::string ReadFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::string DefaultRoot() {
  std::string here = __FILE__;
  for (int i = 0; i < 3; ++i) {  // src/gpu/dlss_card_table_test.cpp -> the repository root
    const size_t slash = here.find_last_of("/\\");
    if (slash == std::string::npos) return ".";
    here.resize(slash);
  }
  return here;
}

// The facts of one adapter alone, as the launcher's first setup has them (os.* and the adapter's own gpu.*).
class AdapterFacts final : public uiexpr::Env {
 public:
  AdapterFacts(std::string vendor, std::string name) : vendor_(std::move(vendor)), name_(std::move(name)) {}
  uiexpr::Value Setting(std::string_view) const override { return uiexpr::Value::Null(); }
  uiexpr::Value Fact(std::string_view path) const override {
    if (path == "os.name") return uiexpr::Value::String("windows");
    if (path == "gpu.vendor") return uiexpr::Value::String(vendor_);
    if (path == "gpu.name") return uiexpr::Value::String(name_);
    return uiexpr::Value::Null();
  }
  uiexpr::Value Renderer() const override { return uiexpr::Value::String("native"); }
  const uiexpr::AutoDescriptor* Auto(std::string_view) const override { return nullptr; }
  const uiexpr::Def* DefOf(std::string_view) const override { return nullptr; }

 private:
  std::string vendor_;
  std::string name_;
};

// The pattern subset of deps.schema.json: letters, digits, spaces and | * + ? [ ] - only.
bool InSubset(const std::string& pattern) {
  if (pattern.empty() || pattern.size() > 200) return false;
  for (const char c : pattern) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' ||
                    c == '|' || c == '*' || c == '+' || c == '?' || c == '[' || c == ']' || c == '-';
    if (!ok) return false;
  }
  return true;
}

struct Group {
  uiexpr::Program when;
  std::regex pattern;
  std::string pattern_text;
  std::string vendor;
  bool found = false;
};

Group ReadGroup(const json::Json& deps, const std::string& name) {
  Group group;
  const json::Json* node = deps.Path("kits.streamline.groups." + name);
  Check(node != nullptr, "deps.json: the streamline kit has a " + name + " group");
  if (!node) return group;
  const json::Json* when = node->Find("when");
  const json::Json* text = node->Find("gpu_name");
  const json::Json* vendor = node->Find("gpu_vendor");
  Check(when && when->IsString(), "deps.json: streamline " + name + " has a `when`");
  Check(text && text->IsString(), "deps.json: streamline " + name + " has a `gpu_name`");
  Check(vendor && vendor->IsString(), "deps.json: streamline " + name + " has a `gpu_vendor`");
  if (!when || !when->IsString() || !text || !text->IsString() || !vendor || !vendor->IsString()) return group;
  group.pattern_text = text->s;
  group.vendor = vendor->s;
  Check(InSubset(group.pattern_text), "deps.json: streamline " + name + " `gpu_name` '" + group.pattern_text + "' is in the allowed subset");
  group.when = uiexpr::Parse(when->s);
  Check(group.when.ok, "deps.json: streamline " + name + " `when` parses (" + group.when.error + ")");
  try {
    group.pattern = std::regex(group.pattern_text, std::regex::ECMAScript | std::regex::icase);
  } catch (const std::regex_error& error) {
    Check(false, "deps.json: streamline " + name + " `gpu_name` compiles (" + error.what() + ")");
    return group;
  }
  group.found = group.when.ok;
  return group;
}

bool Applies(const Group& group, const std::string& vendor, const std::string& name) {
  return vendor == group.vendor && std::regex_search(name, group.pattern);
}

// What the group's `when` says about the adapter's vendor (the OS is Windows here): true, false, or null when it says nothing.
uiexpr::Value When(const Group& group, const std::string& vendor, const std::string& name) {
  return uiexpr::Evaluate(group.when, AdapterFacts(vendor, name));
}

}  // namespace

int main(int argc, char** argv) {
  const std::string root = argc > 1 ? argv[1] : DefaultRoot();
  json::Json vectors;
  json::Json deps;
  std::string error;
  Check(json::Parse(ReadFile(root + "/documentation/contract/adapter-name-vectors.json"), &vectors, &error), "adapter-name-vectors.json parses: " + error);
  Check(json::Parse(ReadFile(root + "/cmake/deps.json"), &deps, &error), "cmake/deps.json parses: " + error);
  const json::Json* list = vectors.Find("vectors");
  Check(list && list->IsArray() && list->a.size() >= 30, "the vector list is there and not trivial");
  if (!list || !list->IsArray()) return 1;

  const Group core = ReadGroup(deps, "core");
  const Group frame_generation = ReadGroup(deps, "frame_generation");

  int rtx_true = 0, rtx_false = 0, card_true = 0, card_false = 0;
  for (const json::Json& vector : list->a) {
    const std::string name = vector.StringOr("name", "");
    const std::string vendor = vector.StringOr("vendor", "");
    const json::Json* rtx_field = vector.Find("rtx");
    const json::Json* card_field = vector.Find("dlss_card");
    Check(!name.empty() && !vendor.empty() && rtx_field && rtx_field->IsBool() && card_field && card_field->IsBool(), "a vector is complete: " + name);
    if (!rtx_field || !card_field) continue;
    const bool rtx = rtx_field->b;
    const bool card = card_field->b;
    (rtx ? rtx_true : rtx_false)++;
    (card ? card_true : card_false)++;

    // 1. the code (the game's machine.json says both only for an NVIDIA adapter)
    const bool nvidia = vendor == "nvidia";
    Check((nvidia && mcla::gpu::rr::NameLooksLikeRtx(name)) == rtx, "code: gpu.rtx for '" + name + "' (" + vendor + ")");
    Check((nvidia && mcla::gpu::dlss::NameLooksLikeFrameGeneration(name)) == card, "code: dlss_card for '" + name + "' (" + vendor + ")");

    // 2. the data
    if (core.found) {
      Check(Applies(core, vendor, name) == rtx, "data: the core group's `gpu_vendor` and `gpu_name` for '" + name + "' (" + vendor + ")");
      Check(When(core, vendor, name).IsTrue() == (vendor == core.vendor), "data: the core group's `when` and `gpu_vendor` agree about '" + name + "' (" + vendor + ")");
    }
    if (frame_generation.found) {
      Check(Applies(frame_generation, vendor, name) == card, "data: the frame_generation group's `gpu_vendor` and `gpu_name` for '" + name + "' (" + vendor + ")");
      Check(When(frame_generation, vendor, name).IsTrue() == (vendor == frame_generation.vendor), "data: the frame_generation group's `when` and `gpu_vendor` agree about '" + name + "' (" + vendor + ")");
    }
  }
  // The vectors must tell the cases apart: both answers true and false for both questions.
  Check(rtx_true >= 10 && rtx_false >= 10 && card_true >= 10 && card_false >= 10, "the vectors have both answers for both questions");

  std::printf("%d checks, %d failed\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
