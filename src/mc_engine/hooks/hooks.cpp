// Entry points of the hook layer. InitHooks() runs once at startup, before any
// guest code, and Patch_DeltaTimePre() is the per-frame tick the features hang
// off. The hooks themselves live in the other files of this folder, one per
// category; what they share is in hooks_internal.h.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <atomic>
#include <string>
#include <string_view>

#include "hooks.h"
#include "hooks_internal.h"
#include "discord_rpc/discord_rpc.h"
#include "../camera_look.h"
#include "../cutscene_gallery.h"
#include "../hud_units.h"
#include "../map_mouse.h"
#include "../map_warp.h"
#include "../online/server_browser/browser_join.h"
#include "../modloader/features/mod_breakables.h"
#include "../modloader/features/mod_glows.h"
#include "../modloader/modloader.h"
#include "../mp3custom/mp3custom.h"
#include "../online/race_props.h"

REXCVAR_DECLARE(std::string, aspect_ratio);  // graphics.cpp

void InitHooks() {
    // Builds xarchive_mods.rpf from models/*.obj. Must run before guest code
    // reaches sub_822C4630 and mounts the archives.
    mc::modloader::Init();

    // Scans <exe>/music and the User Music folder. The tracks are handed to
    // mcMusicManager later, from the ctor hook MCLA_CustomMusic_Install.
    InitCustomMusic();

    ApplyAspectRatioPatch(REXCVAR_GET(aspect_ratio));

    rex::cvar::RegisterChangeCallback("aspect_ratio",
        [](std::string_view name, std::string_view new_value) {
            ApplyAspectRatioPatch(new_value);
        }
    );

    ApplyVinylLayerCaps();

    // export_vinyl acts as a button: toggling it ON runs the export, then it
    // flips back OFF so it can be triggered again.
    rex::cvar::RegisterChangeCallback("export_vinyl",
        [](std::string_view name, std::string_view new_value) {
            if (new_value == "true" || new_value == "1") {
                ExportVinyl();
                rex::cvar::SetFlagByName("export_vinyl", "false");
            }
        }
    );

    // cam_probe_mark / cam_probe_dump: buttons — snapshot the gameplay camera
    // and diff it, to find what the look-around actually moves. Same
    // flip-back-off shape as the dumps below.
    rex::cvar::RegisterChangeCallback("cam_probe_mark",
        [](std::string_view name, std::string_view new_value) {
            if (new_value == "true" || new_value == "1") {
                CameraProbeMark();
                rex::cvar::SetFlagByName("cam_probe_mark", "false");
            }
        }
    );

    rex::cvar::RegisterChangeCallback("cam_probe_dump",
        [](std::string_view name, std::string_view new_value) {
            if (new_value == "true" || new_value == "1") {
                CameraProbeDump();
                rex::cvar::SetFlagByName("cam_probe_dump", "false");
            }
        }
    );

    // The one that is actually usable: arms both samples on a timer, because
    // opening this overlay to press a button is itself enough to take the game
    // out of the camera state being measured.
    rex::cvar::RegisterChangeCallback("cam_probe_run",
        [](std::string_view name, std::string_view new_value) {
            if (new_value == "true" || new_value == "1") {
                CameraProbeRun();
                rex::cvar::SetFlagByName("cam_probe_run", "false");
            }
        }
    );

    // rubberband_dump: button — writes the 11 parsed tune entries out and flips
    // itself back off so it can be triggered again.
    rex::cvar::RegisterChangeCallback("rubberband_dump",
        [](std::string_view name, std::string_view new_value) {
            if (new_value == "true" || new_value == "1") {
                DumpRubberBandTuning();
                rex::cvar::SetFlagByName("rubberband_dump", "false");
            }
        }
    );

    // import_vinyl: type a .vgp file name + Enter to apply it, then the field
    // clears itself. The empty write re-fires this callback, hence the guard.
    rex::cvar::RegisterChangeCallback("import_vinyl",
        [](std::string_view name, std::string_view new_value) {
            if (new_value.empty()) return;
            ImportVinyl(std::string(new_value));
            rex::cvar::SetFlagByName("import_vinyl", "");
        }
    );

    // dump_vinyl_shapes: button — toggling ON writes the shape catalog manifest,
    // then flips back OFF so it can be triggered again.
    rex::cvar::RegisterChangeCallback("dump_vinyl_shapes",
        [](std::string_view name, std::string_view new_value) {
            if (new_value == "true" || new_value == "1") {
                DumpVinylShapes();
                rex::cvar::SetFlagByName("dump_vinyl_shapes", "false");
            }
        }
    );

    // capture_vinyl_shapes: button — starts the hands-free sweep, which runs on
    // the per-frame tick (Patch_DeltaTimePre) and finalizes itself.
    rex::cvar::RegisterChangeCallback("capture_vinyl_shapes",
        [](std::string_view name, std::string_view new_value) {
            if (new_value == "true" || new_value == "1") {
                RequestVinylShapeCapture();
                rex::cvar::SetFlagByName("capture_vinyl_shapes", "false");
            }
        }
    );

    // Reactivate the game's dev command-line options from <exe>/debug_options.txt.
    // Must run before the game's option consumers (all init/level-load reads).
    ApplyDebugOptions();

    // The MCLA/Performance cvars that map onto those same dev switches. Runs
    // after the file, so a cvar that is on overrides the same name coming from
    // debug_options.txt; a cvar that is off leaves the file's value alone.
    ApplyPerfDebugOptions();

    StartFreezeWatchdog();
}

// Fires at 0x822C22EC, right after the clock update (sub_821BDA90). The delta
// time itself is delivered at the clock source (MCLAFrameDelta /
// MCLAUseRealDelta / MCLAFixedStepPath) rather than overwritten late in the
// frame, which is what caused traffic jitter and physics stutter. What is left
// here is the per-frame housekeeping.
void Patch_DeltaTimePre() {
    TickVinylReadbackWindow();  // runs every frame regardless of real_frame_delta
    TickGlobalReadback();       // closes the vinyl / photo readback window once both are done
    TickVinylShapeCapture();    // hands-free shape-catalog sweep, if requested
    TickButtonPrompts();        // picks up a live button_prompts change
    TickCustomMusic();          // custom radio: volume + end-of-track advance
    TickHudUnits();             // hud_speed_units: mph -> km/h, live
    TickCutsceneGallery();      // cutscene replay: close the menu, start the script
    TickMapMouse();             // full map: notices the screen closed, frees the cursor
    TickMapWarp();              // warp console command / F9: a mod's warp point <-> back
    larecomp::browser::TickServerBrowserJoin();  // server browser: drive the picked join
    TickModGlows();             // guest scratch for the mods' light glows
    TickModBreakables();        // mods' breakable sector props: knock-over on a hard hit
    TickRaceProps();            // online race props: the watched owner word, if any
    TickCameraLook();           // cam_freelook: mouse -> gameplay camera lookaround
    TickRearView();             // rearview: the mirror's visible bit + placement, live
    RpcOnRaceTick();            // Discord RPC: race name + series/tournament standings
    ApplyAmbientDensityTuning();  // no-op unless an ambient cvar moved
    ApplyFragTuneOverrides();     // re-asserts the fragment tune overrides
    ApplyRenderPhaseMask();       // perf_no_shadows, live
    ApplyLoadTimeDevOptions();    // keeps node+4 in sync for the load-time switches
    ApplyRubberBandLevel();       // forced AI difficulty tune index
    ApplyRubberBandScales();      // AI catch-up / hold-back limits
    LogRenderPhaseMaskOnce();     // a few samples of the real per-frame masks
    g_frame_heartbeat.fetch_add(1, std::memory_order_relaxed);
}
#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

void InitHooks() {}
void Patch_DeltaTimePre() {}
#endif // REXGLUE_HAS_XEO3_TARGET
