// A check that needs no game: which folder the shader packs go in (bootprogress/pack_folder_rules.h). Build:
// cmake --build <build dir> --target larecomp_pack_folder_test, then run it; exit 0 = passed.
//
// The case of 2026-11-19: LARecomp's own default for cache_root ({app.user_root}/cache, set at the title layer on every start) must not
// move the packs; a cache folder the player named must.

#include <cstdio>
#include <filesystem>

#include "bootprogress/pack_folder_rules.h"

using larecomp::shaderbuild::PackFolderFor;

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failed;                                                            \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
    }                                                                        \
  } while (0)

}  // namespace

int main() {
  namespace fs = std::filesystem;
  const fs::path documents_cache = fs::path("C:/Users/p/Documents/larecomp") / "cache";
  const fs::path named = fs::path("D:/games/lar") / "cache";

  // The default: LARecomp's own cache_root value, which the player did not name. No folder is asked for, so the renderer's choice
  // (<program>\cache\shaders, else the local application data folder) stands, for the game and for --build_shader_packs alike.
  CHECK(PackFolderFor(documents_cache, false).empty());
  CHECK(PackFolderFor(fs::path(), false).empty());

  // A cache folder the player named (larecomp.toml, the environment, --cache_root): the packs follow it.
  CHECK(PackFolderFor(named, true) == named / "shaders");
  CHECK(PackFolderFor(named, true) == fs::path("D:/games/lar/cache/shaders"));
  CHECK(PackFolderFor(documents_cache, true) == documents_cache / "shaders");  // named to the same place: the player's word

  // Named but empty (cache_root = "" turns the SDK's shader storage off): nothing to follow.
  CHECK(PackFolderFor(fs::path(), true).empty());

  if (g_failed == 0) {
    std::printf("pack_folder_rules: %d checks passed\n", g_checks);
    return 0;
  }
  std::printf("pack_folder_rules: %d of %d checks FAILED\n", g_failed, g_checks);
  return 1;
}
