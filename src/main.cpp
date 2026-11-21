#ifndef REXGLUE_HAS_XEO3_TARGET
// larecomp - ReXGlue Recompiled Project, on rexglue 0.11: a rex::app::TitleApp (larecomp_app.h).
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

// kPPCImageConfig, the generated image of the game. The codegen output folder is `generated` (rexglue_project's
// OUTPUT_DIR in CMakeLists.txt) and the file prefix is the manifest's project name.
#include "generated/larecomp_init.h"

#include "larecomp_app.h"

#include "settings/larecomp_settings.h"

#include <cmath>
#include <cstdint>

#if defined(_WIN32)
// Direct3D 12 Agility SDK exports.
// The D3D12 runtime checks these exported symbols in the main executable.
// 619 corresponds to D3D12Core.dll version 1.619.3.0 shipped in assets/D3D12.
extern "C" {
__declspec(dllexport) extern const uint32_t D3D12SDKVersion = 619;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}
#endif

uint8_t* g_guest_mem = nullptr;

extern "C" float roundevenf(float x) {
    return std::nearbyint(x);
}

extern "C" double roundeven(double x) {
    return std::nearbyint(x);
}

// Init, the first-launch wizards (save picker, ISO installer, save import: each draws in the window and is pumped by
// RunSetupFrame), LaunchTitle (the boot build behind the progress popup happens inside it, in OnTitleLaunch), Run,
// Shutdown. REX_TITLE_APP would do Init, LaunchTitle, Run, Shutdown; the wizards need the gap.
static int LarecompMain(LarecompApp& app, const rex::CLIArgList& args) {
    // The actions end the process before any window or runtime exists, so the app has to know before Init (it decides
    // what systems to ask for). The launcher's setup passes both on one command line: the packs first, then the pipelines.
    // --build_city_pack builds the city pack for the ray traced shadows (the launcher's setup, one process of its own); given with another action it runs after it.
    const bool city_pack = args.Has("build_city_pack");
    if (args.Has("build_shader_packs")) {
        app.SetBatchMode(LarecompApp::BatchMode::kBuildShaderPacks);
        app.SetAlsoBuildCityPack(city_pack);
    } else if (args.Has("precache_pipelines")) {
        app.SetBatchMode(LarecompApp::BatchMode::kPrecachePipelines);
        app.SetAlsoBuildCityPack(city_pack);
    } else if (city_pack) {
        app.SetBatchMode(LarecompApp::BatchMode::kBuildCityPack);
    }
    // OnConfigure reads larecomp.toml before the command line is applied: tell it where, when the player says (--config_file).
    if (const auto config = args.Get("config_file"); config && !config->empty()) {
        app.SetConfigFileHint(std::filesystem::path(std::u8string(config->begin(), config->end())));
    }
    // CHECK: both given at once (0.10 ran the packs and then the pipelines in one process): BatchMode needs a third value,
    // or RunBatchMode runs both when both options are present.
    // The SDK stops the start on an option it cannot use, so it gets only its own (the actions, its path flags, the positionals);
    // the setting options wait for OnConfigure, where one that cannot be used becomes a log line (settings/cli_rules.h).
    if (app.Init(app.PrepareCommandLine(args))) {
        if (app.GetBatchMode() == LarecompApp::BatchMode::kNone) {
            while (app.SetupPending() && app.RunSetupFrame()) {
            }
            if (!rex::app::IsShutdownRequested() && app.LaunchTitle()) {
                app.Run();
            }
        }
    }
    return app.Shutdown();
}

// wWinMain and main (the exe is a WIN32 program: wWinMain), a default-constructed LarecompApp.
REX_TITLE_APP_MAIN(LarecompApp, LarecompMain)
#endif // REXGLUE_HAS_XEO3_TARGET
