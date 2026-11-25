// Standalone check of the AppImage variable rule (os/appimage_env.h): which program counts as "inside" an AppImage mount, and that a
// game the launcher started loses the launcher's variables while a game that is an AppImage itself keeps its own. No game, no SDK.
//
//   cmake --build <dir> --target larecomp_appimage_env_test, then run it (exit 0 = passed).
//   Or by hand:  clang++ -std=c++23 -I src src/os/appimage_env_test.cpp src/os/appimage_env.cpp

#include "os/appimage_env.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#if defined(__linux__)
#include <unistd.h>

#include <filesystem>
#include <system_error>
#endif

namespace os = larecomp::os;

namespace {

int g_failed = 0;
int g_checked = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    ++g_checked;                                                   \
    if (!(cond)) {                                                 \
      ++g_failed;                                                  \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
    }                                                              \
  } while (0)

void TestRule() {
  const char* const mount = "/tmp/.mount_larecoAbCdEf";
  CHECK(os::ProgramIsInsideMount(mount, "/tmp/.mount_larecoAbCdEf/usr/lib/larecomp/larecomp"));
  CHECK(os::ProgramIsInsideMount("/tmp/.mount_larecoAbCdEf/", "/tmp/.mount_larecoAbCdEf/usr/bin/larecomp"));
  CHECK(os::ProgramIsInsideMount("/tmp/.mount_larecoAbCdEf//", "/tmp/.mount_larecoAbCdEf/larecomp"));
  // The game the launcher started lives in the install folder, not in the launcher's mount.
  CHECK(!os::ProgramIsInsideMount(mount, "/home/deck/LARecomp/game/larecomp"));
  // A sibling whose name starts the same is another folder.
  CHECK(!os::ProgramIsInsideMount(mount, "/tmp/.mount_larecoAbCdEf2/larecomp"));
  CHECK(!os::ProgramIsInsideMount(mount, "/tmp/.mount_larecoAbCdEf"));  // the folder itself is not a file in it
  CHECK(!os::ProgramIsInsideMount(mount, ""));
  // A mount that says nothing, or everything, never counts.
  CHECK(!os::ProgramIsInsideMount("", "/home/deck/larecomp"));
  CHECK(!os::ProgramIsInsideMount("/", "/home/deck/larecomp"));
  CHECK(!os::ProgramIsInsideMount("//", "/home/deck/larecomp"));
  CHECK(!os::ProgramIsInsideMount("tmp/.mount_x", "tmp/.mount_x/larecomp"));
}

#if defined(__linux__)

bool Set(const char* name, const std::string& value) { return setenv(name, value.c_str(), 1) == 0; }

void ClearAll() {
  for (const char* name : {"APPIMAGE", "APPDIR", "ARGV0", "OWD"}) {
    unsetenv(name);
  }
}

bool AllGone() {
  for (const char* name : {"APPIMAGE", "APPDIR", "ARGV0", "OWD"}) {
    if (std::getenv(name) != nullptr) {
      return false;
    }
  }
  return true;
}

void TestLive() {
  std::error_code error;
  const std::filesystem::path program = std::filesystem::read_symlink("/proc/self/exe", error);
  CHECK(!error);
  if (error) {
    return;
  }
  const std::string program_folder = program.parent_path().string();

  // 1. A game the launcher started: the launcher's four variables, its mount is somewhere else.
  ClearAll();
  Set("APPIMAGE", "/home/deck/Applications/LARecomp-launcher.AppImage");
  Set("APPDIR", "/tmp/.mount_larecoAbCdEf");
  Set("ARGV0", "LARecomp-launcher.AppImage");
  Set("OWD", "/home/deck");
  CHECK(!os::RunsFromAppImage());
  CHECK(os::OwnAppImageFile().empty());
  const std::string removed = os::DropInheritedAppImageVariables();
  CHECK(AllGone());
  CHECK(removed == "APPIMAGE, APPDIR, ARGV0, OWD");
  CHECK(os::DropInheritedAppImageVariables().empty());  // nothing left to remove

  // 2. Only some of them (a runtime that sets two): those are removed, the rest were never there.
  ClearAll();
  Set("APPIMAGE", "/home/deck/Applications/LARecomp-launcher.AppImage");
  Set("OWD", "/home/deck");
  CHECK(os::DropInheritedAppImageVariables() == "APPIMAGE, OWD");
  CHECK(AllGone());

  // 3. The game is an AppImage itself: its own file is inside its own APPDIR, so its variables stay and restart uses its file.
  ClearAll();
  Set("APPIMAGE", "/home/deck/Applications/LARecomp-game.AppImage");
  Set("APPDIR", program_folder);
  Set("ARGV0", "LARecomp-game.AppImage");
  CHECK(os::RunsFromAppImage());
  CHECK(os::OwnAppImageFile() == "/home/deck/Applications/LARecomp-game.AppImage");
  CHECK(os::DropInheritedAppImageVariables().empty());
  CHECK(std::getenv("APPIMAGE") != nullptr && std::getenv("APPDIR") != nullptr);

  // 4. The same, with APPDIR reached through a link (the kernel resolves /proc/self/exe, APPDIR may not be resolved).
  const std::filesystem::path link =
      std::filesystem::temp_directory_path(error) / ("larecomp_appimage_env_test_" + std::to_string(getpid()));
  std::filesystem::remove(link, error);
  std::filesystem::create_directory_symlink(program_folder, link, error);
  if (!error) {
    ClearAll();
    Set("APPIMAGE", "/home/deck/Applications/LARecomp-game.AppImage");
    Set("APPDIR", link.string());
    CHECK(os::RunsFromAppImage());
    std::filesystem::remove(link, error);
  } else {
    std::printf("  info: no symbolic link could be made here, the linked mount was not tried\n");
  }

  // 5. APPIMAGE without APPDIR (a shell that kept one of them): not an AppImage run.
  ClearAll();
  Set("APPIMAGE", "/home/deck/Applications/LARecomp-game.AppImage");
  CHECK(!os::RunsFromAppImage());
  CHECK(os::OwnAppImageFile().empty());
  CHECK(os::DropInheritedAppImageVariables() == "APPIMAGE");

  ClearAll();
}

#else

void TestLive() {
  // Nothing sets these variables off Linux: the helpers say no and remove nothing.
  CHECK(!os::RunsFromAppImage());
  CHECK(os::OwnAppImageFile().empty());
  CHECK(os::DropInheritedAppImageVariables().empty());
}

#endif

}  // namespace

int main() {
  TestRule();
  TestLive();
  std::printf("%s: %d checks, %d failed\n", g_failed == 0 ? "PASS" : "FAIL", g_checked, g_failed);
  return g_failed == 0 ? 0 : 1;
}
