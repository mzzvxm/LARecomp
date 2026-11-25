#include "xcompress.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>

// libmspack's LZX decoder (thirdparty/libmspack, LGPL-2.1; lzxd.c is built as larecomp_mspack and this file alone sees its
// headers: CMakeLists.txt). mspack.h first: lzx.h uses its types. Both are C headers with their own extern "C".
#include <mspack.h>
#include <lzx.h>

namespace mc::modloader {

namespace {

// libmspack reads and writes through a table of callbacks; this one is a pair of memory buffers. The same job as the
// SDK's rex::filesystem::detail::LZXDecompress (src/filesystem/lzx.cpp, BSD 3-Clause, Xenia Project / ReXGlue) and the
// rex/system/lzx.h of rexglue 0.10 that this file used to call: neither is reachable from a program in 0.11.
struct MemoryFile {
    uint8_t* data = nullptr;
    off_t size = 0;
    off_t offset = 0;
};

int ReadMemory(mspack_file* file, void* buffer, int bytes) {
    auto* mem = reinterpret_cast<MemoryFile*>(file);
    const off_t total = std::min<off_t>(static_cast<off_t>(bytes), mem->size - mem->offset);
    std::memcpy(buffer, mem->data + mem->offset, static_cast<size_t>(total));
    mem->offset += total;
    return static_cast<int>(total);
}

int WriteMemory(mspack_file* file, void* buffer, int bytes) {
    auto* mem = reinterpret_cast<MemoryFile*>(file);
    const off_t total = std::min<off_t>(static_cast<off_t>(bytes), mem->size - mem->offset);
    std::memcpy(mem->data + mem->offset, buffer, static_cast<size_t>(total));
    mem->offset += total;
    return static_cast<int>(total);
}

void* AllocMemory(mspack_system*, size_t bytes) { return std::calloc(bytes, 1); }
void FreeMemory(void* ptr) { std::free(ptr); }
void CopyBytes(void* src, void* dst, size_t bytes) { std::memcpy(dst, src, bytes); }

// One LZX bitstream (the chunk headers already removed) of one resource, decoded in one call with a fresh decoder.
// `window_size` is a power of two. 0 on success, else libmspack's error code (or 1 for a call that cannot start).
int DecodeBitstream(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_len, uint32_t window_size) {
    if (window_size == 0 || (window_size & (window_size - 1)) != 0) return 1;
    if (src_len >= INT_MAX || dst_len >= INT_MAX) return 1;  // libmspack's off_t is 32 bits on Windows
    int window_bits = 0;
    while ((1u << window_bits) < window_size) ++window_bits;

    mspack_system sys{};
    sys.read = ReadMemory;
    sys.write = WriteMemory;
    sys.alloc = AllocMemory;
    sys.free = FreeMemory;
    sys.copy = CopyBytes;

    // The handles are the MemoryFile themselves (libmspack never looks inside one).
    MemoryFile input{const_cast<uint8_t*>(src), static_cast<off_t>(src_len), 0};
    MemoryFile output{dst, static_cast<off_t>(dst_len), 0};
    lzxd_stream* lzxd = lzxd_init(&sys, reinterpret_cast<mspack_file*>(&input), reinterpret_cast<mspack_file*>(&output),
                                  window_bits, 0, 0x8000, static_cast<off_t>(dst_len), 0);
    if (!lzxd) return 1;
    const int result = lzxd_decompress(lzxd, static_cast<off_t>(dst_len));
    lzxd_free(lzxd);
    return result;
}

// XMemCreateDecompressionContext in the game (sub_8244FF98) asks for a
// 128 KiB window; every chunk but the last decodes to one 32 KiB LZX frame.
constexpr uint32_t kWindowSize = 0x20000;
constexpr size_t kFrameOutput = 0x8000;

// Drops the chunk headers and leaves the bare LZX bitstream libmspack reads.
// Each chunk ends on a frame boundary, where the decoder realigns itself, so
// the payloads can simply be laid end to end. Returns false on a stream that
// is cut short or does not add up to `dst_len`.
bool StripChunkHeaders(const uint8_t* src, size_t src_len, size_t dst_len,
                       std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(src_len);

    size_t offset = 0;
    size_t produced = 0;
    while (offset < src_len) {
        size_t output = kFrameOutput;
        size_t length = 0;
        if (src[offset] == 0xFF) {
            if (offset + 5 > src_len) return false;
            output = (size_t(src[offset + 1]) << 8) | src[offset + 2];
            length = (size_t(src[offset + 3]) << 8) | src[offset + 4];
            offset += 5;
        } else {
            if (offset + 2 > src_len) return false;
            length = (size_t(src[offset]) << 8) | src[offset + 1];
            offset += 2;
        }
        if (length == 0) break;
        if (offset + length > src_len) return false;

        out.insert(out.end(), src + offset, src + offset + length);
        offset += length;
        produced += output;
    }
    return !out.empty() && produced >= dst_len;
}

}  // namespace

bool LzxDecompress(const uint8_t* src, size_t src_len, std::vector<uint8_t>& dst,
                   size_t dst_len) {
    if (!src || !src_len || !dst_len) return false;

    std::vector<uint8_t> bitstream;
    if (!StripChunkHeaders(src, src_len, dst_len, bitstream)) return false;

    dst.assign(dst_len, 0);
    return DecodeBitstream(bitstream.data(), bitstream.size(), dst.data(), dst_len, kWindowSize) == 0;
}

}  // namespace mc::modloader
