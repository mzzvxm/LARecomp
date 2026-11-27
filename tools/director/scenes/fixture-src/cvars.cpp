// Setting declarations for the scene linter tests.
REXCVAR_DEFINE_STRING(
    mcla_gpu_ao, "auto", "MCLA/GPU",
    "Ambient occlusion: darkens creases, kerbs, the ground under cars and where objects meet; a ; inside the text "
    "is not the end of the statement. off, high, ultra or auto. Live.")
    .allowed({"auto", "off", "high", "ultra"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_ao_fade_end, 160.0, "MCLA/GPU",
    "The distance where ambient occlusion has faded out entirely. Live.")
    .debug_only()
    .range(20.0, 1000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_ao_view, 0, "MCLA/GPU",
    "Diagnostic: shows a stage of the ambient occlusion. 0: off. Live.")
    .debug_only()
    .range(0, 6)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// The before/after split (src/gpu/split.cpp, trailer toolkit M5b; descriptions shortened).
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_split, -1.0, "MCLA/GPU",
    "A before/after split in one frame: a position from 0 to 1 across the picture's width. -1 is off. Live.")
    .debug_only()
    .range(-1.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(
    mcla_gpu_split_effects, "all", "MCLA/GPU",
    "Which effects mcla_gpu_split cuts: ao, dither or all. Live.")
    .allowed({"all", "ao", "dither"})
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(mcla_fixture_restart, false, "MCLA/Fixture", "A switch the game reads only at start.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

