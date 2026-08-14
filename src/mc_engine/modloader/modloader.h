// Drop-in model replacement, modloader style.
//
//   <exe>/models/<any mod name>/<asset name>.obj
//   e.g. models/sonic/drv_mp_01_set.obj
//
// Wheels go in a "rims" folder inside the mod, since a wheel and a character are
// different assets under different archive paths:
//
//   <exe>/models/<any mod name>/rims/<wheel name>.glb
//   e.g. models/vw/rims/whl_am_volk_te37.glb
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

namespace mc::modloader {

// Scans models/, builds xarchive_mods.rpf and prepares the archive list the
// engine will mount. Call once at startup, before guest code runs.
void Init();

// Appends the mod archive to the ';'-separated list the engine just copied into
// its parse buffer. `buffer` is the guest address of that buffer and `capacity`
// its size in bytes, terminator included. Does nothing when the modloader is
// inactive, when the archive is already listed, or when it would not fit.
void AppendModArchiveTo(uint32_t buffer, size_t capacity);

}  // namespace mc::modloader
