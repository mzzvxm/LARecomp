#pragma once

// The city pack (src/citypack: the city's geometry for ray traced shadows, built from the player's own game files) as the app builds it: the launcher's setup step and the
// game's own background build (CITY-RT M1). The decisions and the words are in city_pack_rules.h (checked on their own); this is the part that runs.
//
//   OnInit                  RunCommandLine        larecomp.exe --build_city_pack: the whole build on every core with no window and no runtime (the launcher's setup runs it after
//                                                 --build_shader_packs, where the PC's graphics card can ray trace), then the program ends
//   SetupPresentation       InstallRendererHooks  before the renderer starts: tells it how the pack stands (stat only) and lets it ask for a build
//   (render thread)         the request           the renderer wants the pack, it is missing or stale: ONE background thread builds it (a quarter of the PC's threads at most
//                                                 four, in background mode: a lower CPU, disk and memory priority than the game's own threads); the renderer asks again every 2 s and the shadows come on when it ends
//   OnShutdown              StopBackgroundBuild   a build that still runs is cancelled (it polls between sectors) and joined
//
// THE HIT DATA (CITY-HIT-ACTION): the hit file and the texture file beside the pack, which the effects that read the city's surfaces need, are built, checked and stamped by the same library as
// citypack_tool's build-hit / check-hit / check-tex (src/citypack/hit_data.h), in the same three ways:
//
//   RunBuildHitCommandLine  larecomp.exe --build_city_hit   the textures, then the hit file, over the pack that is in place (every core but one), no window and no runtime
//   RunCheckHitCommandLine  larecomp.exe --check_city_hit   stat only, or with --check_city_hit_deep=true every file read back whole
//   the request             the hit scene's probe           the renderer wants the hit data, it is missing or stale: ONE background thread builds it (the same budget as the pack's); the
//                                                           renderer asks again every 2 s and the effects switch on when it ends
// The lines the command line prints and the exit codes are city_pack_rules.h's (the same @citypack lines); the logs are logs/city_hit_build.txt and logs/city_hit_check.txt, the game's own
// log has the [city hit] lines.
//
// The pack goes in <shader pack folder>/city (mcla::gpu::CityPackFolder: the cache folder the player named, or next to the program, or the local application data folder), the
// log in logs/city_pack_build.txt (every build; the game's own log has the [city pack] lines). A failure never stops the game: the renderer keeps the game's own shadows.
// Nothing here applies when the XENIA renderer runs (gpu.driver any / d3d12 / vulkan): it never reads the pack.

#include <filesystem>

namespace larecomp::citypackbuild {

// UI thread, from SetupPresentation, before mcla::gpu::Startup (with shaderbuild::InstallStartupGate). `game_root` is the folder the game files are in.
void InstallRendererHooks(const std::filesystem::path& game_root);

// UI thread, from OnShutdown, before logging stops: cancels a background build and waits for it (within about a second). Does nothing when none runs.
void StopBackgroundBuild();

// Builds the pack the way the game would at its first use (the same folder, the same builder and stamp, so the game finds it current and builds nothing), from `game_root`/
// xarchive_cache.rpf and the mod archive's city files, on every core but one. Prints progress for the program that started it (the line format is in city_pack_rules.h). Returns
// the exit code: 0 when the pack is in place (built, or already current) or not needed, 1 otherwise (the game then builds it itself in the background when ray traced shadows are
// on). A pack that is current is left alone (build_city_pack_force builds anyway).
int RunCommandLine(const std::filesystem::path& game_root);

// larecomp.exe --build_city_hit: builds the hit file and the texture file the way the game would (the same folder, builders and stamp), over the city pack in place, from `game_root`/
// xarchive_cache.rpf and the mod archive's city files, on every core but one (shader_pack_threads). Prints progress (city_pack_rules.h). Returns 0 when the hit data is in place (built, or
// already current) and 1 otherwise: no archive, no city pack ("no pack": run --build_city_pack first), a failed or cancelled build. Current hit data is left alone (build_city_hit_force builds
// anyway).
int RunBuildHitCommandLine(const std::filesystem::path& game_root);

// larecomp.exe --check_city_hit: is the hit data in place and current? Stat only: "up to date" (0) or "not current" with the reason (1). `deep` (check_city_hit_deep) reads the city pack, the hit
// file and every picture of the texture file back instead and answers "up to date" when all are whole, "failed" with the counts when not.
int RunCheckHitCommandLine(const std::filesystem::path& game_root, bool deep);

}  // namespace larecomp::citypackbuild
