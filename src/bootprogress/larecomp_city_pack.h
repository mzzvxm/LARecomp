#pragma once

// The city pack (src/citypack: the city's geometry for ray traced shadows, built from the player's own game files) as the app builds it: the launcher's setup step and the
// game's own background build (CITY-RT M1). The decisions and the words are in city_pack_rules.h (checked on their own); this is the part that runs.
//
//   OnInit                  RunCommandLine        larecomp.exe --build_city_pack: the whole build on every core with no window and no runtime (the launcher's setup runs it after
//                                                 --build_shader_packs, where the PC's graphics card can ray trace), then the program ends
//   SetupPresentation       InstallRendererHooks  before the renderer starts: tells it how the pack stands (stat only) and lets it ask for a build
//   (render thread)         the request           the renderer wants the pack, it is missing or stale: ONE background thread builds it (a quarter of the PC's threads at most
//                                                 four, at a lower priority than the game's own threads); the renderer asks again every 2 s and the shadows come on when it ends
//   OnShutdown              StopBackgroundBuild   a build that still runs is cancelled (it polls between sectors) and joined
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

}  // namespace larecomp::citypackbuild
