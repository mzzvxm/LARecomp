#include "mod_data.h"

#include "mc_engine/modloader/modloader.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <system_error>

namespace {

std::filesystem::path ModelsDir() {
    std::error_code ec;
#if defined(_WIN32)
    wchar_t exe[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH))
        return mc::modloader::ModRootDir(std::filesystem::path(exe).parent_path());
#endif
    return mc::modloader::ModRootDir(std::filesystem::current_path(ec));
}

}  // namespace

std::vector<ModDataFile> FindModDataFiles(const char* name) {
    std::vector<ModDataFile> out;
    std::error_code ec;
    const std::filesystem::path models = ModelsDir();
    if (!std::filesystem::is_directory(models, ec)) return out;
    std::vector<std::filesystem::path> mods;
    for (const auto& entry : std::filesystem::directory_iterator(models, ec)) {
        if (entry.is_directory(ec) &&
            !mc::modloader::IsReservedModFolder(entry.path().filename().string()))
            mods.push_back(entry.path());
    }
    std::sort(mods.begin(), mods.end());
    for (size_t i = 0; i < mods.size(); ++i) {
        const std::filesystem::path file = mods[i] / name;
        if (std::filesystem::is_regular_file(file, ec))
            out.push_back({int(i), mods[i].filename().string(), file});
    }
    return out;
}
