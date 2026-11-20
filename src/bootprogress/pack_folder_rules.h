#pragma once
//
// Which folder the shader packs live in, decided from the cache folder setting. Pure (no SDK, no OS): the check
// bootprogress/pack_folder_rules_test.cpp compiles this header alone. The app side is larecomp::shaderbuild::ConfigurePackFolder
// (larecomp_shader_build.cpp), which LarecompApp::OnInit calls BEFORE the command-line modes (--build_shader_packs,
// --precache_pipelines) return, so those modes and a normal start take the same folder.
//
// WHY. README: the packs go in `cache\shaders` next to the executable, and the launcher's shader step (ShadersStep.cs) says the same:
// "shader_pack_dir, a cache folder the player named, else game\cache\shaders". Only a cache folder the player NAMED moves them
// (larecomp.toml, the environment, --cache_root). LARecomp itself gives cache_root a value on every start (LarecompApp::ChooseUserFolder:
// "{app.user_root}/cache" at the title layer, the SDK's folder for its own shader storage), so the value being there says nothing: on
// 0.10 the test was "the cvar has text", and the cvar was empty unless the player set it; on 0.11 the same test (GetFlagByName
// ("cache_root").empty()) is never true, which sent a normal start to <Documents>\larecomp\cache\shaders while the batch mode, which
// returned before the call, built into <program>\cache\shaders: the launcher's packs were never found (2026-11-19).
//
#include <filesystem>

namespace larecomp::shaderbuild {

// What SetShaderPackFolder is given: <cache_root>/shaders when the player named the cache folder; an empty path (leave the renderer's
// own choice: next to the program, else the local application data folder) when they did not, and when there is no cache folder.
inline std::filesystem::path PackFolderFor(const std::filesystem::path& cache_root, bool named_by_player) {
  if (cache_root.empty() || !named_by_player) return {};
  return cache_root / "shaders";
}

}  // namespace larecomp::shaderbuild
