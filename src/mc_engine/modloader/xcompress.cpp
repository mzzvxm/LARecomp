#include "xcompress.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace mc::modloader {

namespace {

using LzxDecompressFn = int(__cdecl*)(const void* src, int src_len, void* dst, int* dst_len);

LzxDecompressFn g_lzx_decompress = nullptr;
bool g_tried_load = false;

}  // namespace

bool XCompressAvailable(const std::filesystem::path& exe_dir) {
#if defined(_WIN32)
    if (g_tried_load) return g_lzx_decompress != nullptr;
    g_tried_load = true;

    const std::filesystem::path dll = exe_dir / "xcompress32.dll";
    std::error_code ec;
    if (!std::filesystem::exists(dll, ec)) return false;

    HMODULE module = LoadLibraryW(dll.wstring().c_str());
    if (!module) return false;

    g_lzx_decompress =
        reinterpret_cast<LzxDecompressFn>(GetProcAddress(module, "LZXDecompress"));
    return g_lzx_decompress != nullptr;
#else
    (void)exe_dir;
    return false;
#endif
}

bool LzxDecompress(const uint8_t* src, size_t src_len, std::vector<uint8_t>& dst,
                   size_t dst_len) {
    if (!g_lzx_decompress || !src || !src_len || !dst_len) return false;

    dst.assign(dst_len, 0);
    int out_len = static_cast<int>(dst_len);
    const int result =
        g_lzx_decompress(src, static_cast<int>(src_len), dst.data(), &out_len);
    return result == 0;
}

}  // namespace mc::modloader
