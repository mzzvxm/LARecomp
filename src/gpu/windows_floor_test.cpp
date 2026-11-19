// Check of the Windows builds the D3D12 runtime needs (windows_floor.h). A program of its own: no Windows
// API, no renderer; exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_windows_floor_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_windows_floor_test

#include <cstdint>
#include <cstdio>
#include <string>

#include "windows_floor.h"

using namespace mcla::gpu::windows_floor;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

bool Has(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

void CheckText(const std::string& text, const std::string& part, const std::string& what) {
  ++g_checks;
  if (!Has(text, part)) {
    ++g_failed;
    std::printf("FAILED: %s: '%s' is not in '%s'\n", what.c_str(), part.c_str(), text.c_str());
  }
}

WindowsBuild W(uint32_t build, uint32_t revision, uint32_t major = 10, uint32_t minor = 0) {
  return {major, minor, build, revision};
}

void TestVerdicts() {
  // Windows 11 and the Windows 10 versions after the servicing patches.
  Check(Classify(W(26100, 1)).verdict == Verdict::kOk, "Windows 11 24H2");
  Check(Classify(W(22000, 0)).verdict == Verdict::kOk, "Windows 11 21H2 with no revision read");
  Check(Classify(W(19045, 5247)).verdict == Verdict::kOk, "Windows 10 22H2");
  Check(Classify(W(19045, 0)).verdict == Verdict::kOk, "22H2 with no revision read: still ok");
  Check(Classify(W(19044, 1288)).verdict == Verdict::kOk, "Windows 10 21H2");
  Check(Classify(W(19043, 928)).verdict == Verdict::kOk, "Windows 10 21H1");
  // 2004 and 20H2: 804.
  Check(Classify(W(19041, 804)).verdict == Verdict::kOk, "2004 at 804");
  Check(Classify(W(19041, 1288)).verdict == Verdict::kOk, "2004 later");
  Check(Classify(W(19042, 804)).verdict == Verdict::kOk, "20H2 at 804");
  Floor f = Classify(W(19041, 746));
  Check(f.verdict == Verdict::kNeedsUpdate && f.needs_revision == 804, "2004 at 746 needs 804");
  Check(std::string(f.update) == "KB4601319", "2004's update is KB4601319");
  f = Classify(W(19042, 685));
  Check(f.verdict == Verdict::kNeedsUpdate && f.needs_revision == 804 && std::string(f.update) == "KB4601319",
        "20H2 at 685 needs 804");
  Check(Classify(W(19042, 803)).verdict == Verdict::kNeedsUpdate, "20H2 at 803 is one short");
  // 1909: 1377.
  Check(Classify(W(18363, 1377)).verdict == Verdict::kOk, "1909 at 1377");
  f = Classify(W(18363, 1198));
  Check(f.verdict == Verdict::kNeedsUpdate && f.needs_revision == 1377 && std::string(f.update) == "KB4601315",
        "1909 at 1198 needs 1377");
  // Revision not read: nothing is claimed for the builds that depend on it.
  Check(Classify(W(19041, 0)).verdict == Verdict::kUnknown, "2004, revision not read");
  Check(Classify(W(18363, 0)).verdict == Verdict::kUnknown, "1909, revision not read");
  // Older than 1909.
  Check(Classify(W(18362, 1139)).verdict == Verdict::kTooOld, "1903");
  Check(Classify(W(17763, 5936)).verdict == Verdict::kTooOld, "1809");
  Check(Classify(W(10240, 0)).verdict == Verdict::kTooOld, "Windows 10 1507");
  // Not Windows 10 or 11, or nothing read.
  Check(Classify(W(9600, 0, 6, 3)).verdict == Verdict::kUnknown, "Windows 8.1 (6.3)");
  Check(Classify(W(0, 0, 0, 0)).verdict == Verdict::kUnknown, "nothing read");
  Check(Classify(W(19045, 5247, 10, 1)).verdict == Verdict::kUnknown, "a made-up 10.1");
}

void TestText() {
  CheckText(Describe(W(19045, 5247)), "Windows 10 version 22H2 (build 19045.5247)", "describe 22H2");
  CheckText(Describe(W(19041, 746)), "version 2004", "describe 2004");
  CheckText(Describe(W(26100, 1)), "Windows 11 (build 26100.1)", "describe Windows 11 has no version word");
  CheckText(Describe(W(22631, 0)), "Windows 11 (build 22631)", "describe without revision");
  CheckText(Describe(W(18363, 1198)), "version 1909", "describe 1909");
  Check(!Has(Describe(W(26100, 1)), "version"), "Windows 11 names no Windows 10 version");
  // What the refusal says.
  const std::string need = Explain(W(19041, 746));
  CheckText(need, "KB4601319", "explain names the update");
  CheckText(need, "19041.804", "explain names the build it needs");
  CheckText(need, "Windows Update", "explain says what to do");
  CheckText(Explain(W(18363, 1198)), "KB4601315", "explain 1909");
  const std::string old = Explain(W(17763, 5936));
  CheckText(old, "version 1809", "explain names the old version");
  CheckText(old, "1909", "explain names the floor");
  Check(Explain(W(19045, 5247)).empty(), "22H2: nothing to explain");
  Check(Explain(W(26100, 1)).empty(), "Windows 11: nothing to explain");
  Check(Explain(W(19041, 0)).empty(), "revision unknown: nothing to explain");
  Check(Explain(W(0, 0, 0, 0)).empty(), "nothing read: nothing to explain");
}

}  // namespace

int main() {
  TestVerdicts();
  TestText();
  std::printf("windows_floor_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
