#pragma once
// ===========================================================================
// MCLA GPU renderer -- the Windows 10 builds the D3D12 runtime needs (pure rules)
// ===========================================================================
// The game ships its own Direct3D 12 runtime next to the exe (the Agility SDK: main.cpp exports
// D3D12SDKVersion, D3D12\D3D12Core.dll) and the Windows that loads it needs an update that teaches it
// the redistributable. Microsoft's page for it (DirectX-Specs, d3d/D3D12Redistributable) says the
// redistributable is "serviced to every retail build of Windows Version 1909 (19H2) and more recent",
// with the servicing patches that began on 2021-02-09 (KB4601315 for 1909, KB4601319 for 2004 and 20H2:
// the builds 18363.1377, 19041.804 and 19042.804), and that "a D3D12 SDK version declares the oldest
// version of Windows that it is supported with" (it was 1909 for every version read: whether 1.619
// raised it is a CHECK for a tester, see device.cpp). On an older Windows 10 the device creation fails,
// and what the player saw was "no graphics card here has a driver for it". This file says which Windows
// builds are below that line, so that the refusal can name the cause. It decides nothing about whether
// the game starts: the device creation is the judge, and this is only the explanation when it fails.
//
// No Windows API here, so windows_floor_test.cpp checks it alone; the build number and the update
// revision are read by device.cpp.
// ===========================================================================

#include <cstdint>
#include <format>
#include <string>

namespace mcla::gpu::windows_floor {

// Windows' own numbers: the version (10.0 for both Windows 10 and 11), the build, and the update
// revision of that build (the registry's UBR, the number after the dot of "19045.5247"; 0 when it could
// not be read).
struct WindowsBuild {
  uint32_t major = 0;
  uint32_t minor = 0;
  uint32_t build = 0;
  uint32_t revision = 0;
};

enum class Verdict : uint8_t {
  kUnknown,      // not read, or not Windows 10 or 11: nothing is said
  kOk,           // at or above the line
  kNeedsUpdate,  // a version the redistributable is serviced for, before the update that does it
  kTooOld,       // older than Windows 10 1909
};

struct Floor {
  Verdict verdict = Verdict::kUnknown;
  uint32_t needs_revision = 0;  // kNeedsUpdate: the revision of this build the update makes
  const char* update = "";      // kNeedsUpdate: the update's name
};

inline constexpr uint32_t kBuild1909 = 18363;
inline constexpr uint32_t kBuild2004 = 19041;
inline constexpr uint32_t kBuild20H2 = 19042;
inline constexpr uint32_t kBuild21H1 = 19043;
inline constexpr uint32_t kBuildWindows11 = 22000;

constexpr Floor Classify(const WindowsBuild& windows) {
  if (windows.major != 10 || windows.minor != 0 || windows.build == 0) {
    return {};
  }
  if (windows.build >= kBuild21H1) {
    return {Verdict::kOk, 0, ""};  // released after the servicing patches (Windows 11 included)
  }
  if (windows.build < kBuild1909) {
    return {Verdict::kTooOld, 0, ""};
  }
  // 1909, 2004 and 20H2 need their patch; without the revision nothing can be said.
  const bool is_1909 = windows.build == kBuild1909;
  const uint32_t need = is_1909 ? 1377u : 804u;
  if (windows.revision == 0) {
    return {};
  }
  if (windows.revision < need) {
    return {Verdict::kNeedsUpdate, need, is_1909 ? "KB4601315" : "KB4601319"};
  }
  return {Verdict::kOk, 0, ""};
}

// "Windows 10 version 22H2 (build 19045.5247)", "Windows 11 (build 26100.1)", "Windows (build 17763)": the
// names of the builds that matter here, else the number alone.
inline std::string Describe(const WindowsBuild& windows) {
  const char* name = "";
  switch (windows.build) {
    case 17134: name = " version 1803"; break;
    case 17763: name = " version 1809"; break;
    case 18362: name = " version 1903"; break;
    case 18363: name = " version 1909"; break;
    case 19041: name = " version 2004"; break;
    case 19042: name = " version 20H2"; break;
    case 19043: name = " version 21H1"; break;
    case 19044: name = " version 21H2"; break;
    case 19045: name = " version 22H2"; break;
    default: break;
  }
  const char* product = windows.build >= kBuildWindows11 ? "Windows 11"
                        : windows.major == 10             ? "Windows 10"
                                                          : "Windows";
  if (windows.build >= kBuildWindows11) {
    name = "";
  }
  const std::string number = windows.revision != 0
                                 ? std::format("{}.{}", windows.build, windows.revision)
                                 : std::format("{}", windows.build);
  return std::format("{}{} (build {})", product, name, number);
}

// What a refused Direct3D 12 start says about this Windows, or "" when it is no cause the player can fix.
inline std::string Explain(const WindowsBuild& windows) {
  const Floor floor = Classify(windows);
  switch (floor.verdict) {
    case Verdict::kNeedsUpdate:
      return std::format(
          "this is {}, which has not had the update ({}, build {}.{} or newer) that Windows 10 needs "
          "to run the Direct3D 12 files that come with the game; Windows Update installs it",
          Describe(windows), floor.update, windows.build, floor.needs_revision);
    case Verdict::kTooOld:
      return std::format(
          "this is {}, which is older than the Windows 10 version 1909 that the Direct3D 12 files "
          "that come with the game need; Windows Update (or a newer Windows 10 version) fixes it",
          Describe(windows));
    default:
      return {};
  }
}

}  // namespace mcla::gpu::windows_floor
