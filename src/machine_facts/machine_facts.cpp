#include "machine_facts.h"

#if defined(LARECOMP_HAS_GPU)
#include "gpu_facts.h"
#include "bootprogress/larecomp_shader_build.h"
#include "citypack/builder.h"
#include "citypack/hit_data.h"
#include "gpu/gpu.h"
#include "shaderpack/pack_format.h"
#endif
#include "cpu_level.h"
#include "larecomp_log.h"
#include "larecomp_version.h"
#include "mc_engine/core/string_dumper.h"
#include "mc_engine/display/display_sizes.h"
#include "mc_engine/modloader/modloader.h"
#include "mc_engine/online/identity/identity_core.h"
#include "mc_engine/online/identity/identity_store.h"
#include "spdlog_console.h"

#include <rex/platform/file.h>
#include <rex/xam/user_profile.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <format>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/utsname.h>
#endif

namespace larecomp::machine {
namespace {

using mcla::display::Size;
namespace fs = std::filesystem;

uint32_t OsBuild() {
#if defined(_WIN32)
  OSVERSIONINFOW version{};
  version.dwOSVersionInfoSize = sizeof(version);
  using GetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
  if (const auto ntdll = GetModuleHandleW(L"ntdll.dll")) {
    const auto get_version = reinterpret_cast<GetVersion>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (get_version && get_version(&version) == 0) return version.dwBuildNumber;
  }
#else
  // The numeric patch component of uname's kernel release (the first three components on Linux).
  utsname os{};
  if (uname(&os) == 0) {
    std::string_view release(os.release);
    for (int n = 0; n < 2; ++n) {
      const size_t dot = release.find('.');
      if (dot == release.npos) return 0;
      release.remove_prefix(dot + 1);
    }
    uint32_t build = 0;
    std::from_chars(release.data(), release.data() + release.size(), build);
    return build;
  }
#endif
  return 0;
}

std::string WriteTime() {
  const auto now = std::time(nullptr);
  std::tm utc{};
#if defined(_WIN32)
  if (gmtime_s(&utc, &now) != 0) return {};
#else
  if (!gmtime_r(&now, &utc)) return {};
#endif
  char text[32]{};
  std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return text;
}

void ReadDisplayFacts(Facts& out, bool probe) {
  // Batch uses SDL's video subsystem only long enough to enumerate displays. No window is created.
  const bool own_video = (SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0;
  if (own_video && !SDL_InitSubSystem(SDL_INIT_VIDEO)) return;
  const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
  int count = 0;
  SDL_DisplayID* ids = SDL_GetDisplays(&count);
  const auto append = [&](SDL_DisplayID id) {
    const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(id);
    if (!mode || mode->w <= 0 || mode->h <= 0) return;
    const char* name = SDL_GetDisplayName(id);
    Display display{.name = name ? name : "", .width = uint32_t(mode->w),
                    .height = uint32_t(mode->h), .hz = mode->refresh_rate};
#if defined(SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN)
    const auto properties = SDL_GetDisplayProperties(id);
    if (SDL_HasProperty(properties, SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN)) {
      display.hdr = SDL_GetBooleanProperty(properties, SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false);
    }
#endif
    out.displays.push_back(std::move(display));
  };
  if (primary) append(primary);
  if (!out.displays.empty()) {
    for (int i = 0; ids && i < count; ++i) {
      if (ids[i] != primary) append(ids[i]);
    }
    if (out.displays.front().hdr == true) out.hdr_capable = true;

    // Keep the menu's live list where it exists. In the batch, use its 16:9 steps against the
    // primary work area. CHECK: verify the headless work-area and fullscreen lists on both OSes.
    if (!probe) {
      for (Size size : mcla::display::WindowedSizes()) {
        out.windowed_sizes.push_back({size.width, size.height});
      }
    }
    if (out.windowed_sizes.empty()) {
      SDL_Rect usable{};
      if (SDL_GetDisplayUsableBounds(primary, &usable) && usable.w > 0 && usable.h > 0) {
        constexpr Size steps[] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440},
                                  {3200, 1800}, {3840, 2160}, {5120, 2880}, {7680, 4320}};
        for (const Size size : steps) {
          if (size.width <= uint32_t(usable.w) && size.height <= uint32_t(usable.h)) {
            out.windowed_sizes.push_back({size.width, size.height});
          }
        }
        if (out.windowed_sizes.empty()) out.windowed_sizes.push_back({1280, 720});
      }
    }
    if (out.exclusive_fullscreen) {
      mcla::display::sizes::Look look;
      const Display& display = out.displays.front();
      look.desktop = {display.width, display.height};
      look.desktop_hz = display.hz > 0 ? uint32_t(display.hz + 0.5f) : 0;
      SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(primary, &count);
      for (int i = 0; modes && i < count; ++i) {
        const auto& mode = *modes[i];
        if (mode.w <= 0 || mode.h <= 0) continue;
        look.modes.push_back({{uint32_t(mode.w), uint32_t(mode.h)},
                              mode.refresh_rate > 0 ? uint32_t(mode.refresh_rate + 0.5f) : 0});
      }
      SDL_free(modes);
      mcla::display::sizes::Book book;
      book.Add(look, false, {1280, 720});
      const auto sizes = probe ? book.Sizes() : mcla::display::ExclusiveSizes();
      for (const Size size : sizes) {
        out.exclusive_sizes.push_back({size.width, size.height, book.RefreshFor(size)});
      }
    }
  }
  SDL_free(ids);
  if (own_video) SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

template <typename Profile>
std::string SaveProfileFolder() {
  if constexpr (requires { Profile::ConfiguredName(); }) {
    if (auto name = Profile::ConfiguredName(); !name.empty()) return name;
  }
  return "B13EBABEBABEBABE";
}

void ReadPaths(Facts& out, const Paths& paths) {
  out.game_folder = rex::platform::GetExecutableFolder();
  out.config_file = paths.config_file;
  out.user_root = paths.user_root;
  out.cache_root = paths.cache_root;
  out.game_data_root = paths.game_root;
  out.logs = LarecompLogFolder();
  out.title_id = "545407F8";
  const auto profile_folder = SaveProfileFolder<rex::xam::UserProfile>();
  const auto xuid = identity::Xuid();
  out.profile_id = std::format("{:016X}", xuid ? xuid : identity::core::kLegacyXuid);
  if (!out.user_root.empty()) {
    out.saves_root = out.user_root / fs::u8path(profile_folder) / out.title_id / "00000001";
  }
  out.mod_root = mc::modloader::ModRootDir(out.game_folder);
  out.mod_roots_ignored.clear();
  for (std::string_view alias : {"modloader", "mods", "models"}) {
    const auto dir = mc::modloader::FindDirNoCase(out.game_folder, alias);
    std::error_code error;
    if (!mc::modloader::EqualsNoCase(PathText(dir.filename()), PathText(out.mod_root.filename())) &&
        fs::is_directory(dir, error)) out.mod_roots_ignored.push_back(dir);
  }
  out.music = {mc::modloader::FindDirNoCase(out.game_folder, "music")};
#if defined(_WIN32)
  if (const wchar_t* profile = _wgetenv(L"USERPROFILE")) {
#else
  if (const char* profile = std::getenv("HOME")) {
#endif
    out.music.push_back(fs::path(profile) / "Documents" / "Rockstar Games" / "LARecomp" / "User Music");
  }
  out.strings_replace = mc::strings::DefaultStringsDir(paths.game_root) / "replace";
}

void ReadPackFacts(Facts& out, bool probe) {
#if defined(LARECOMP_HAS_GPU)
  const auto present = [](const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error);
  };
  const auto shader_folders = mcla::gpu::ShaderPackSearchFolders();
  const auto shader_exists = std::ranges::any_of(shader_folders, [&](const auto& folder) {
    return present(folder / shaderpack::kPackFileName);
  });
  const auto city = mcla::gpu::CityPackFolder();
  const bool city_exists = present(city / citypack::kPackFileName);
  const bool hit_exists = present(city / citypack::kHitFileName) && present(city / citypack::kTexFileName);
  out.shader_pack = shader_exists ? std::nullopt : std::optional<std::string>("missing");
  out.city_pack = city_exists ? std::nullopt : std::optional<std::string>("missing");
  out.city_hit = hit_exists ? std::nullopt : std::optional<std::string>("missing");
  if (probe || out.game_data_root.empty()) return;

  if (shader_exists) {
    // The game's startup gate also accepts existing packs from before it wrote stamps.
    out.shader_pack = shaderbuild::PacksCurrent(out.game_data_root) ? "current" : "stale";
  }
  if (city_exists || hit_exists) {
    citypack::BuildRequest request;
    request.source_rpf = out.game_data_root / "xarchive_cache.rpf";
    request.mods_rpf = out.game_data_root / "xarchive_mods.rpf";
    request.out_dir = city;
    request.app_version = out.game_version;
    if (city_exists) out.city_pack = citypack::CheckCityPack(request).current ? "current" : "stale";
    if (hit_exists) {
      citypack::HitDataRequest hit;
      hit.base = request;
      out.city_hit = citypack::CheckHitData(hit).current ? "current" : "stale";
    }
  }
#else
  (void)out;
  (void)probe;
#endif
}

Facts ReadFacts(const Paths& paths, bool probe) {
  Facts out;
#if defined(_WIN32)
  out.os = "windows";
  out.hdr_output = true;
  out.wheel_ffb = true;
#else
  out.os = "linux";
#endif
  out.os_build = OsBuild();
  out.cpu_threads = std::max(1u, std::thread::hardware_concurrency());
  out.cpu_level = cpu::BuiltLevel();
  out.game_version = LARECOMP_VERSION_LABEL;
#if defined(LARECOMP_MACHINE_COMMIT)
  out.commit = LARECOMP_MACHINE_COMMIT;
#endif
#if defined(LARECOMP_MIN_LAUNCHER)
  out.min_launcher = LARECOMP_MIN_LAUNCHER;  // CMake: min_launcher of launcher-recipe.json (the one place it is edited)
#endif
  out.exclusive_fullscreen = mcla::display::ExclusiveSupported();
  out.live_window_resize = mcla::display::LiveResize();
  out.window_size_fixed = mcla::display::WindowSizeFixed();
  ReadPaths(out, paths);
  ReadDisplayFacts(out, probe);
  ReadPackFacts(out, /*probe=*/true);
#if defined(LARECOMP_HAS_GPU)
  out.gpu = ReadGpuFacts(probe, out.game_folder);
#else
  out.gpu.gi_update = false;
#endif
  // CHECK: the language probe belongs to the pause-menu migration. Its result is private to that
  // module today, and the headless probe does not run the mods or load the title.
  return out;
}

void Emit(const char* result, std::string text) {
  // Pipe is forbidden in Windows filenames, but is allowed on Linux. Escape record delimiters on
  // either OS and keep stdout to one line. A literal percent is escaped with them.
  std::string field;
  for (char c : text) {
    if (c == '%') field += "%25";
    else if (c == '|') field += "%7C";
    else if (c == '\r') field += "%0D";
    else if (c == '\n') field += "%0A";
    else field += c;
  }
  std::printf("@machine|result|%s|%s\n", result, field.c_str());
  std::fflush(stdout);
}

}  // namespace

int Probe(const Paths& paths, const fs::path& output) {
  std::error_code ec;
  const fs::path path = fs::absolute(output.empty()
      ? rex::platform::GetExecutableFolder() / "machine.json" : output, ec);
  if (ec) {
    Emit("failed", ec.message());
    return 1;
  }
  try {
    const Facts facts = ReadFacts(paths, true);
    std::string error;
    if (!WriteDocument(path, Serialize(facts, true, WriteTime()), error)) {
      Emit("failed", error);
      return 1;
    }
    Emit("ok", PathText(path));
    return 0;
  } catch (const std::exception& error) {
    Emit("failed", error.what());
    return 1;
  }
}

void GamePublisher::Tick(const Paths* paths) {
  const auto now = std::chrono::steady_clock::now();
  if (!paths && now < next_) return;
  next_ = now + std::chrono::seconds(1);
  try {
    if (facts_.game_folder.empty()) {
      if (!paths) return;
      facts_ = ReadFacts(*paths, false);
    } else {
      if (paths) {
        ReadPaths(facts_, *paths);
        ReadPackFacts(facts_, false);
      }
#if defined(LARECOMP_HAS_GPU)
      RefreshGpuFacts(facts_.gpu);
#endif
    }
    std::string snapshot = Serialize(facts_, false);
    if (!publication_.Changed(snapshot)) return;
    std::string error;
    if (WriteDocument(facts_.game_folder / "machine.json", Serialize(facts_, false, WriteTime()), error)) {
      publication_.NoteSuccess(std::move(snapshot));
      warned_ = false;
      return;
    }
    if (!warned_) LARECOMP_APP_WARN("[machine] could not write machine.json: {}", error);
    warned_ = true;
  } catch (const std::exception& error) {
    if (!warned_) LARECOMP_APP_WARN("[machine] could not collect machine facts: {}", error.what());
    warned_ = true;
  }
}

}  // namespace larecomp::machine
