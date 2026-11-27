#pragma once
// ===========================================================================
// MCLA GPU renderer -- the graphics presets at run time (GFX-PRESETS; the table and the rules are gfx_preset.h)
// ===========================================================================
// What a named preset does to the game, and when:
//   * Start(), once the renderer has started (larecomp_app.cpp OnInit: the GPU class and the ray tracing tier are known, the file, the environment and the command line are applied):
//     the recommendation supplies the first-start default. A named preset is HELD over the rows its table covers (a value for every covered row through larecomp::SetCvarForPreset, below authored scene settings, so a save never writes the
//     preset's values to larecomp.toml, and the player's own values underneath are kept); CUSTOM takes its base (mcla_gpu_preset_from) for the rows the player's file does not name.
//     A row that the command line or the environment sets is left to them: those are temporary overrides on top of a preset, never saved, never a change to CUSTOM.
//   * mcla_gpu_preset changing at run time (the pause row, the launcher's file, the console): picking a named preset holds its values over the rows at once (no restart);
//     picking CUSTOM again gives the player's own values back (gfx_preset::OnPicked says which of restore or keep).
//   * NoteRowWrite(), called by the pause menu before it writes a row: a covered row changed by the player while a named preset is held turns the preset into CUSTOM. The rows keep the
//     values they have (they become the player's own: written to larecomp.toml at the next save) and mcla_gpu_preset_from names the preset they came from.
// A change made by anything else (the DEBUG menu's panel, the console, a trailer scene, the harness) is not the player's choice and does not turn the preset into CUSTOM.
// Everything that decides is a plain function of gfx_preset.h (gfx_preset_test.cpp); this file is the part that touches the settings.
// ===========================================================================

#include <filesystem>
#include <string_view>
#include <atomic>

#include "gfx_preset.h"

namespace mcla::gpu::gfx_preset {

// Once, after the renderer has started. Safe to call again (the second call does nothing).
void Start(const FileFacts& file, const std::filesystem::path& config_path);

// Effective preset, including the Director's session override.
Preset Current();
bool Available(Preset preset);
bool SetSessionPreset(std::optional<Preset> preset);
bool Persist(Preset preset);

// A frame latches this generation before reading its effect settings.
uint64_t Generation();

// Keep the idle allocations used by the scene's warmed presets until hand-back.
inline std::atomic<bool> retain_scene_resources{false};
inline std::atomic<bool> retain_scene_rt_resources{false};
inline std::atomic<bool> warming_scene{false};
inline std::atomic<uint64_t> warm_frame_generation{0};

// The pause menu is about to write `cvar` for the player. True when that turned a named preset into CUSTOM (the menu may log it); false for every other row and for every other write.
bool NoteRowWrite(std::string_view cvar);

}  // namespace mcla::gpu::gfx_preset
