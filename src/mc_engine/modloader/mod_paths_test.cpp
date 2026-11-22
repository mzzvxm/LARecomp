// A check that needs no game: the folder lookups of the modloader that ignore the case of a name (mod_paths.h) and the two
// functions of modloader.h that use them (ModRootDir, IsReservedModFolder). Build:
//   cmake --build <build dir> --target larecomp_mod_paths_test, then run it; exit 0 = passed.
// On a case-insensitive file system (Windows, macOS) the folder scans cannot tell the spellings apart, so the checks that need a Linux file system
// are made only where a probe finds one; PickNoCase and the other pure rules are checked everywhere.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "modloader.h"

namespace fs = std::filesystem;
using namespace mc::modloader;

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    ++g_checks;                                                         \
    if (!(cond)) {                                                      \
      ++g_failed;                                                       \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
    }                                                                   \
  } while (0)

void MakeDir(const fs::path& p) {
  std::error_code ec;
  fs::create_directories(p, ec);
}

void MakeFile(const fs::path& p) {
  MakeDir(p.parent_path());
  std::ofstream(p) << "x";
}

bool IsDir(const fs::path& p) {
  std::error_code ec;
  return fs::is_directory(p, ec);
}

std::string Leaf(const fs::path& p) { return p.filename().string(); }

void TestPureRules() {
  CHECK(LowerAscii('A') == 'a');
  CHECK(LowerAscii('Z') == 'z');
  CHECK(LowerAscii('a') == 'a');
  CHECK(LowerAscii('_') == '_');
  CHECK(LowerAscii('@') == '@');   // the character before 'A'
  CHECK(LowerAscii('[') == '[');   // the character after 'Z'
  CHECK(LowerAscii(char(0xC4)) == char(0xC4));  // not ASCII: left alone, whatever the locale

  CHECK(EqualsNoCase("rims", "RIMS"));
  CHECK(EqualsNoCase("Textures_Replace", "textures_replace"));
  CHECK(EqualsNoCase("", ""));
  CHECK(!EqualsNoCase("rims", "rim"));
  CHECK(!EqualsNoCase("rims", "rimz"));
  CHECK(!EqualsNoCase("rims", "rims "));

  // The exact spelling wins, whatever sorts first.
  CHECK(PickNoCase({"Rims", "rims"}, "rims") == 1);
  CHECK(PickNoCase({"rims", "Rims"}, "rims") == 0);
  CHECK(PickNoCase({"RIMS", "rims", "Rims"}, "Rims") == 2);
  // No exact spelling: the first in byte order, not in the order the file system listed them.
  CHECK(PickNoCase({"RIMS", "Rims"}, "rims") == 0);   // "RIMS" < "Rims"
  CHECK(PickNoCase({"Rims", "RIMS"}, "rims") == 1);
  CHECK(PickNoCase({"zzz", "Rims", "aaa"}, "rims") == 1);
  CHECK(PickNoCase({"zzz", "aaa"}, "rims") == -1);
  CHECK(PickNoCase({}, "rims") == -1);
  CHECK(PickNoCase({"rimsx", "xrims", "rim"}, "rims") == -1);  // a longer or shorter name is another name
}

void TestFolders(const fs::path& root) {
  // A probe of the file system: does it tell "Probe" from "probe"?
  MakeDir(root / "ProbeCase");
  const bool case_sensitive = !IsDir(root / "probecase");

  const fs::path mod = root / "game" / "Mods" / "Sonic";
  MakeDir(mod / "Rims");
  MakeDir(mod / "FILES" / "Textures_Replace");
  MakeFile(mod / "Vehicles");            // a FILE with the vehicles folder's name is not that folder
  MakeFile(mod / "sonic.obj");

  const fs::path rims = FindDirNoCase(mod, "rims");
  CHECK(IsDir(rims));
  const fs::path files = FindDirNoCase(mod, "files");
  CHECK(IsDir(files));
  CHECK(IsDir(FindDirNoCase(files, "textures_replace")));
  CHECK(IsDir(FindDirNoCase(FindDirNoCase(mod, "files"), "textures_replace")));
  CHECK(!IsDir(FindDirNoCase(mod, "vehicles")));          // only a file of that name: no such folder
  CHECK(!IsDir(FindDirNoCase(mod, "music")));             // nothing of the name
  CHECK(Leaf(FindDirNoCase(mod, "music")) == "music");     // the caller's own spelling comes back for a name nothing matches
  CHECK(!IsDir(FindDirNoCase(root / "no" / "such", "rims")));  // a folder that is not there: no throw, no folder
  // FindChildNoCase without the directory rule finds the file too.
  CHECK(fs::exists(FindChildNoCase(mod, "vehicles", /*want_directory=*/false)));
  if (case_sensitive) {
    CHECK(Leaf(rims) == "Rims");
    CHECK(Leaf(files) == "FILES");
    CHECK(Leaf(FindDirNoCase(files, "textures_replace")) == "Textures_Replace");
    // Two spellings of one name: the exact one wins, else the first in byte order.
    MakeDir(mod / "music");
    MakeDir(mod / "Music");
    CHECK(Leaf(FindDirNoCase(mod, "music")) == "music");
    CHECK(Leaf(FindDirNoCase(mod, "MUSIC")) == "Music");   // "Music" < "music"
    // A file that matches too does not hide the folder, even where the file's name sorts first.
    const fs::path other = root / "game2" / "mod";
    MakeFile(other / "VEHICLES");
    MakeDir(other / "Vehicles");
    CHECK(IsDir(FindDirNoCase(other, "vehicles")));
    CHECK(Leaf(FindDirNoCase(other, "vehicles")) == "Vehicles");
  }

  // ModRootDir: the three names in order, in any case.
  const fs::path exe = root / "game";
  CHECK(IsDir(ModRootDir(exe)));
  CHECK(IsDir(ModRootDir(exe) / "Sonic"));      // found the Mods folder the way it is spelled
  const fs::path empty_exe = root / "bare";
  MakeDir(empty_exe);
  CHECK(Leaf(ModRootDir(empty_exe)) == "modloader");     // none present: the name anything that creates it uses
  CHECK(!IsDir(ModRootDir(empty_exe)));
  MakeDir(empty_exe / "MODELS");
  CHECK(IsDir(ModRootDir(empty_exe)));
  MakeDir(empty_exe / "MoDs");
  CHECK(Leaf(ModRootDir(empty_exe)).size() == 4 || !case_sensitive);   // "mods" before "models"
  if (case_sensitive) {
    CHECK(Leaf(ModRootDir(empty_exe)) == "MoDs");
    MakeDir(empty_exe / "Modloader");
    CHECK(Leaf(ModRootDir(empty_exe)) == "Modloader");    // "modloader" before "mods"
  }
}

void TestReserved() {
  CHECK(IsReservedModFolder(""));
  CHECK(IsReservedModFolder(".cache"));
  CHECK(IsReservedModFolder("textures_dump"));
  CHECK(IsReservedModFolder("Textures_Dump"));
  CHECK(IsReservedModFolder("TEXTURES_REPLACE"));
  CHECK(IsReservedModFolder("music"));
  CHECK(IsReservedModFolder("Music"));
  CHECK(!IsReservedModFolder("musical"));
  CHECK(!IsReservedModFolder("my music"));
  CHECK(!IsReservedModFolder("Sonic"));
  CHECK(!IsReservedModFolder("textures"));
}

}  // namespace

int main() {
  TestPureRules();
  TestReserved();

  const fs::path root = fs::temp_directory_path() /
                        ("larecomp_mod_paths_test_" +
                         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::error_code ec;
  fs::remove_all(root, ec);
  MakeDir(root);
  TestFolders(root);
  fs::remove_all(root, ec);

  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
