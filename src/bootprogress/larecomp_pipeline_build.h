#pragma once

// larecomp.exe --precache_pipelines: the app's side of the pipeline prebuild
// (src/gpu/pipeline_prebuild.h). The launcher runs it after the shader packs are built, so that the
// first start finds the driver's pipeline cache filled and has nothing to compile while the title
// runs. Nothing is shown: no window, no graphics output, no guest.
//
//   SetupEnvironment   RunCommandLine     creates the device the game would, reads the pack and the
//                                         pipeline lists, makes every listed pipeline on all
//                                         processors but one, writes the cache file, prints
//                                         progress; then the program ends

#include <filesystem>

namespace larecomp::pipelinebuild {

// From SetupEnvironment (the paths and the config are final, logging is up, nothing else has
// started): whether the program was asked to prepare the pipelines and close.
bool CommandLineRequested();

// Prepares the pipelines for the graphics API the game would run on (precache_pipelines_backend
// picks another), from the shader packs the game would use. Returns the exit code: 0 when the cache
// is prepared or needs nothing, 1 otherwise (the game then compiles the pipelines itself while it
// runs, as it always did). A stamp keeps a second run quick (see pipeline_prebuild.h).
//
// What a program that runs it reads on stdout, one line each, every line starting with
// "@pipelines|" (the console also carries the engine's own log lines, which are not for it).
// Fields are separated by '|'; the last one is free text and may contain more of them.
//   @pipelines|device|<D3D12|Vulkan>|<adapter>|<driver version, hex>   once, when the device exists
//   @pipelines|phase|<steps, 0 when unknown>|<name>                    the build begins
//   @pipelines|step|<done>|<steps>|                                    progress, at most ten a second
//   @pipelines|log|<info|warn|error>|<text>                            why it builds or skips
//   @pipelines|result|<status>|<text>                                  once, last
// <status> is ok, up to date, not needed, no list, or what failed (no shader pack, no device, bad
// option, stalled, failed, not ready, packs not current). Exit 0 for the first four and ok. There
// is no cancel message: ending the process is the cancel, and it is safe at any moment (the cache
// file and the stamp are written at the end, under temporary names). With --build_shader_packs on
// the same command line the packs are built first (their @shaderpack lines come before these) and
// this step runs only when they succeeded.
int RunCommandLine(const std::filesystem::path& game_root);

}  // namespace larecomp::pipelinebuild
