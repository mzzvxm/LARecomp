// Host-side decoder for the game's XCompress LZX streams.
//
// Character resources are stored LZX-compressed (XCompress). The recompiled
// game decompresses them itself through XMemDecompressStream, but the modloader
// has to crack one open on the host to use it as a template.
//
// This used to go through xcompress32.dll's LZXDecompress. The copy that was
// being shipped was a Debug build importing VCRUNTIME140D.dll and
// ucrtbased.dll, so LoadLibrary failed on any machine without Visual Studio
// and every mesh mod was skipped with "xcompress32.dll missing". libmspack's
// LZX decoder (the one the SDK uses for XEX images), once the XCompress chunk
// headers are peeled off, takes these streams as they are: all 14,928
// resources in xarchive_cache.rpf decode byte for byte the same as the DLL did.
// Up to rexglue 0.10 it was reached as rex/system/lzx.h; 0.11 keeps it private,
// so LARecomp builds libmspack's lzxd.c itself (the submodule
// thirdparty/libmspack, LGPL-2.1).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mc::modloader {

// `src` is the XCompress stream as it follows the RSC5 header (no RSC5 header,
// no 0x0FF512EF marker, no length word): a run of chunks, each a big-endian
// 16-bit compressed length -- or 0xFF, a 16-bit output length and a 16-bit
// compressed length for a short one -- followed by that many LZX bytes.
// `dst_len` is the exact decompressed size.
bool LzxDecompress(const uint8_t* src, size_t src_len, std::vector<uint8_t>& dst,
                   size_t dst_len);

}  // namespace mc::modloader
