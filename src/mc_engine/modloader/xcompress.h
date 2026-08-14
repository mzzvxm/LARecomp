// Thin binding to xcompress32.dll's LZXDecompress.
//
// Character resources are stored LZX-compressed (XCompress). The recompiled
// game decompresses them itself through XMemDecompressStream, but the modloader
// has to crack one open on the host to use it as a template, and there is no
// host-side decoder in the SDK for this framing. xcompress32.dll ships with the
// usual Xbox 360 tooling and exports exactly what is needed.
//
// The DLL is optional: without it the modloader stays off and says so.
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace mc::modloader {

// Looks for xcompress32.dll next to the executable. Safe to call repeatedly.
bool XCompressAvailable(const std::filesystem::path& exe_dir);

// `src` is the raw LZX stream (no RSC5 header, no XCompress 8-byte prefix).
bool LzxDecompress(const uint8_t* src, size_t src_len, std::vector<uint8_t>& dst,
                   size_t dst_len);

}  // namespace mc::modloader
