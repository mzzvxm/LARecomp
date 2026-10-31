// Drop-in model replacement, modloader style.
//
//   <exe>/modloader/<any mod name>/<asset name>.obj
//   e.g. modloader/sonic/drv_mp_01_set.obj
//
// (mods/ and models/ work as well; see ModRootDir.)
//
// Wheels go in a "rims" folder inside the mod, since a wheel and a character are
// different assets under different archive paths:
//
//   <exe>/modloader/<any mod name>/rims/<wheel name>.glb
//   e.g. modloader/vw/rims/whl_am_volk_te37.glb
//
// In either folder a mesh named "all" stands in for every asset of that kind.
//
// At startup each .obj is baked into a drawable resource and packed into
// <game data>/xarchive_mods.rpf, which is appended to the archive list the
// engine mounts. Every archive mounts at the same point ("a:/archive/"), and
// fiDevice::GetDevice (sub_821CB488) walks that mount's device list from the
// last registered backwards, falling through when a device does not hold the
// file -- so the mod archive wins per file and everything it does not contain
// still comes from the shipped archives. Original game files are never touched.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace mc::modloader {

// The mod folder beside the executable. Three names are accepted, the first
// one present wins: modloader/, mods/, models/. With none of them present it
// is modloader/, which is what anything creating the folder makes.
inline std::filesystem::path ModRootDir(const std::filesystem::path& exe_dir) {
    std::error_code ec;
    for (const char* name : {"modloader", "mods", "models"}) {
        std::filesystem::path dir = exe_dir / name;
        if (std::filesystem::is_directory(dir, ec)) return dir;
    }
    return exe_dir / "modloader";
}

// Folders in the mod folder that are not mods: the texture dumps
// (textures_dump/), loose replacements (textures_replace/), music and caches
// (any name starting with '.').
inline bool IsReservedModFolder(std::string_view name) {
    return name.empty() || name.front() == '.' || name == "textures_dump" ||
           name == "textures_replace" || name == "music";
}

// Scans the mod folder, builds xarchive_mods.rpf and prepares the archive list the
// engine will mount. Call once at startup, before guest code runs.
void Init();

// Appends the mod archive to the ';'-separated list the engine just copied into
// its parse buffer. `buffer` is the guest address of that buffer and `capacity`
// its size in bytes, terminator included. Does nothing when the modloader is
// inactive, when the archive is already listed, or when it would not fit.
void AppendModArchiveTo(uint32_t buffer, size_t capacity);

// Renames a new car's driver animation name, in place, to the one it borrows
// (its donor's unless parts.txt says `driver = <car>`). `buffer` is a guest
// string the engine just formatted: a pack path, a dictionary path or an
// animation name. Does nothing when no new car is named in it or the result
// would not fit in `capacity` bytes, terminator included.
void AliasDriverAnimName(uint32_t buffer, size_t capacity);

}  // namespace mc::modloader
