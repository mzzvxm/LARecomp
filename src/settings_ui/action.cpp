#include "action.h"
#include "export.h"
#include "settings_ui_data.h"

#include "larecomp_version.h"
#include "settings/larecomp_settings.h"
#include "../../documentation/contract/cpp/mini_json.h"

#include <rex/app_info.h>
#include <rex/driver_context.h>
#include <rex/driver_module.h>
#include <rex/init.h>
#include <rex/platform/file.h>
#include <rex/settings_registry.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace larecomp::settings_ui {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kTypeNames[] = {"bool", "int", "int", "int", "int", "number", "string", "path"};
constexpr std::string_view kLifeNames[] = {"live", "restart", "startup", "until_launch"};

constexpr struct Alias {
    std::string_view flat;
    std::string_view full;
} kAliases[] = {
#include "settings/sdk_names.inc"
};

// The system modules need app information during Connect even for metadata collection.
class CatalogAppInfo final : public rex::IModule, private rex::IAppInfo {
public:
    std::string_view GetName() const override { return "settings-ui"; }
    rex::Result<void> Connect(rex::IInterfaceFactory&) override { return rex::Ok(); }
    void Disconnect() override {}
    rex::Result<void> Init() override { return rex::Ok(); }
    void Shutdown() override {}
    rex::IInterface* QueryInterface(std::string_view name, rex::u32 version) override {
        return rex::FindModuleInterface<rex::IAppInfo>(static_cast<rex::IAppInfo&>(*this), name, version);
    }

private:
    const rex::AppSysRequirements& GetRequirements() const override { return requirements_; }
    std::string_view GetAppName() const override { return "larecomp"; }
    const rex::AppSysRequirements requirements_{rex::GraphicsFamily::kXenos};
};

// CHECK: these are the title defaults that don't require the host adapter. Share the
// startup defaults with LarecompApp once its startup code can be changed alongside this exporter.
void ApplyHeadlessDefaults() {
    constexpr std::pair<std::string_view, std::string_view> defaults[] = {
        {"apu.driver", "larecomp"},
        {"gpu.driver", "larecomp"},
        {"gpu.vsync", "false"},
        {"ui.video_mode_refresh_rate", "60"},
        {"ui.title", "LARecomp"},
        {"gpu.anisotropic_override", "5"},
        {"gpu.xenos.texture_cache_memory_limit_soft", "1536"},
        {"gpu.xenos.texture_cache_memory_limit_hard", "2048"},
        {"gpu.xenos.texture_cache_memory_limit_render_to_texture", "64"},
        {"gpu.xenos.async_shader_compilation", "true"},
        {"gpu.d3d12.bindless", "true"},
        {"gpu.d3d12.readback_resolve", "false"},
        {"gpu.vulkan.readback_resolve", "false"},
        {"gpu.xenos.readback_memexport_fast", "true"},
        {"gpu.d3d12.allow_tearing", "true"},
        {"gpu.xenos.allow_invalid_fetch_constants", "true"},
        {"gpu.xenos.render_target_max_height", "1024"},
        {"gpu.xenos.render_target_trim_seconds", "60"},
        {"gpu.xenos.memory_pressure_percent", "90"},
        {"gpu.xenos.shared_memory_hot_page_faults", "4"},
        {"gpu.xenos.shared_memory_hot_page_ms", "10000"},
        {"gpu.d3d12.hoist_shared_memory_uploads", "true"},
        {"gpu.xenos.readback_resolve_rules", "len=1048576,pitch=512,height=512,fmt=6;addr=0x051A0000-0x051F7FFF"},
        {"gpu.xenos.resolve_unscaled_copy_rules", "len=131072,pitch=128,height=128,fmt=37"},
        {"hid.mnk_mode", "false"},
        {"hid.mnk_mouse", "false"},
    };
    for (const auto& [name, value] : defaults) {
        settings::SetDefault(name, value);
    }
    settings::SetDefault("gpu.d3d12.submit_on_primary_buffer_end",
                         settings::submit_on_primary_buffer_end.Get() ? "true" : "false");
}

void CollectSettings(toml::table& catalog, rex::ISettingsRegistry& registry) {
    for (const auto* setting : registry.Enumerate()) {
        const auto& full = setting->GetName();
        std::string flat = settings::FlatName(*setting);
        if (full == "gpu.resolution_scale_x") {
            // ApplyPlayerConfig already handles this legacy key as a write to both axes.
            flat = "resolution_scale";
        } else if (flat == full) {
            // SDK settings without a historical flat alias still have a stable catalog key.
            // CHECK: the launcher must use full for these SDK settings when editing the config.
            std::replace(flat.begin(), flat.end(), '.', '_');
            flat.insert(0, "sdk_");
        }
        if (catalog.contains(flat)) continue;
        const auto& limits = setting->GetConstraints();
        toml::table entry{
            {"full", full},
            {"type", kTypeNames[static_cast<size_t>(setting->GetType())]},
            {"default", setting->GetLayer() == rex::SettingLayer::kTitle ? setting->GetText() : setting->GetDefaultText()},
            {"declared_default", setting->GetDefaultText()},
            {"life", kLifeNames[static_cast<size_t>(limits.mode)]},
            {"no_save", limits.no_save},
            {"debug", limits.debug_only},
        };
        // CHECK: v1 cannot yet represent the SSR history default (-1 for automatic)
        // outside its editable range. Keep the default and leave its range absent.
        if (limits.min && limits.max && flat != "mcla_gpu_ssr_history") {
            entry.insert("range", toml::array{*limits.min, *limits.max});
        }
        if (!limits.allowed.empty()) {
            toml::array allowed;
            for (const auto& value : limits.allowed) allowed.push_back(value);
            entry.insert("allowed", std::move(allowed));
        }
        toml::array aliases;
        for (const auto& alias : kAliases) {
            if (alias.full == full && alias.flat != flat && registry.IsAlias(alias.flat)) aliases.push_back(alias.flat);
        }
        entry.insert("aliases", std::move(aliases));
        catalog.insert(flat, std::move(entry));
    }
}

toml::table ReadRegistry() {
    toml::table catalog;
    CatalogAppInfo app_info;
    rex::Config config;
    config.code_modules.push_back(&app_info);
    config.modules = {"rexfilesystem", "rexhid", "rexapu", "rexui", "rexgpu", "rexxam", "rexxbdm", "rexkernel",
                      "rexgpu-xenos-vulkan"};
#if defined(_WIN32)
    config.modules.push_back("rexgpu-xenos-d3d12");
#endif
    bool complete = false;
    config.select_plugins = [&](rex::IInterfaceFactory& factory, rex::IInterfaceFactory&)
        -> rex::Result<std::vector<std::string>> {
        auto& registry = rex::GetSettingsRegistry();
        const rex::ScopeGuard unregister{[] { settings::UnregisterAll(); }};
        if (auto registered = settings::RegisterAll(registry); !registered) {
            return std::unexpected(registered.error());
        }
        ApplyHeadlessDefaults();
        CollectSettings(catalog, registry);

        auto* context = factory.Get<rex::IDriverContext>();
        if (!context) return rex::Err<std::vector<std::string>>(rex::ErrorCategory::kModule, "no driver context");
        // CHECK: verify shipped SDK modules register settings in Initialize and defer device
        // creation to SetSurface/Start. Never start the modules or attach a surface here.
        for (const auto driver : {
                 "vulkan",
#if defined(_WIN32)
                 "d3d12",
#endif
             }) {
            auto drivers = rex::SelectDrivers(factory, rex::DriverType::kGraphics, driver, *context);
            if (!drivers) return std::unexpected(drivers.error());
            auto& selected = drivers->front();
            const rex::ScopeGuard stop{[&] { selected->Shutdown(); }};
            CollectSettings(catalog, registry);
        }
        // The SDK unwinds connected modules on a select_plugins error, before any Init.
        complete = true;
        return rex::Err<std::vector<std::string>>(rex::ErrorCategory::kRuntime, "settings collected");
    };
    const rex::ScopeGuard shutdown{[] { rex::Shutdown(); }};
    const auto initialized = rex::Init(config);
    if (!complete) {
        throw std::runtime_error(initialized ? "settings registration was skipped" : initialized.error().GetDescription());
    }
    return catalog;
}

std::string ExecutableId(const fs::path& executable) {
    return std::to_string(fs::file_size(executable)) + ":" +
           std::to_string(fs::last_write_time(executable).time_since_epoch().count());
}

void Export(const fs::path& output, std::string_view exe_id) {
    const Inputs inputs{LARECOMP_VERSION_LABEL, kSettingsUiCommit, exe_id, kUiText, kLauncherRows, kUiRules, kPhaseA};
    WriteDocument(output, Json(BuildDocument(ReadRegistry(), AllSettingsMenus(), inputs)));
}

bool IsCurrent(const fs::path& path, std::string_view id) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    // The writer puts generated fields near the front, but a valid reader accepts any property order.
    minijson::Json doc;
    std::string error;
    if (!minijson::Parse(bytes, &doc, &error)) return false;
    const auto* exe_id = doc.Path("generated.exe_id");
    return exe_id && exe_id->IsString() && exe_id->s == id;
}

void PrintResult(const char* status, std::string detail) {
    std::replace(detail.begin(), detail.end(), '\r', ' ');
    std::replace(detail.begin(), detail.end(), '\n', ' ');
    std::printf("@settingsui|result|%s|%s\n", status, detail.c_str());
    std::fflush(stdout);
}

}  // namespace

int DumpSettingsUi(std::optional<std::string_view> output) {
    try {
        if (output && output->empty()) throw std::runtime_error("the output path is empty");
        const auto exe = rex::platform::GetExecutablePath();
        const auto path = output ? fs::absolute(fs::path(std::u8string(output->begin(), output->end())))
                                 : exe.parent_path() / "settings-ui.json";
        Export(path, ExecutableId(exe));
        const auto utf8 = path.u8string();
        PrintResult("ok", std::string(utf8.begin(), utf8.end()));
        return 0;
    } catch (const std::exception& err) {
        PrintResult("fail", err.what());
        return 1;
    }
}

void RefreshSettingsUi() {
    try {
        const auto exe = rex::platform::GetExecutablePath();
        const auto output = exe.parent_path() / "settings-ui.json";
        const auto id = ExecutableId(exe);
        // CHECK: verify the next title app init after a stale refresh on Windows and Linux.
        if (!IsCurrent(output, id)) Export(output, id);
    } catch (const std::exception& err) {
        // A catalog failure is reported on stdout and normal game startup continues.
        PrintResult("fail", err.what());
    }
}

}  // namespace larecomp::settings_ui
