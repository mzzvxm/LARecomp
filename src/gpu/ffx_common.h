#pragma once
// ===========================================================================
// MCLA GPU renderer -- what the FidelityFX backends share
// ===========================================================================
// The small helpers that load AMD's runtime and describe an image to it: the folder of the exe, a DLL
// opened by its full path, the version names, the image description, the backend description of
// the API in use. The upscaler backend (temporal_ffx.cpp) and the frame generation backend
// (frame_generation_ffx.cpp) both use these.
//
// AMD's D3D12 and Vulkan headers cannot be included in one translation unit (temporal_ffx_glue.h):
// nothing here includes either, only the API-neutral ones.
// ===========================================================================

#include <cstdint>
#include <iterator>
#include <string>

#include <ffx_api.h>
#include <ffx_api_types.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "backend.h"
#include "temporal_backend.h"
#include "temporal_ffx_glue.h"

namespace mcla::gpu::ffx_common {

inline const char* ReturnName(uint32_t code) {
  switch (code) {
    case FFX_API_RETURN_OK: return "ok";
    case FFX_API_RETURN_ERROR: return "error";
    case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: return "unknown description type";
    case FFX_API_RETURN_ERROR_RUNTIME_ERROR: return "runtime error";
    case FFX_API_RETURN_NO_PROVIDER: return "no provider";
    case FFX_API_RETURN_ERROR_MEMORY: return "out of memory";
    case FFX_API_RETURN_ERROR_PARAMETER: return "bad parameter";
    case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE: return "provider too old for this description";
    default: return "unknown";
  }
}

inline std::string Utf8(const wchar_t* text) {
  if (!text) {
    return {};
  }
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (bytes <= 1) {
    return {};
  }
  std::string out(size_t(bytes) - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
  return out;
}

// The folder of the running exe, with a trailing separator; empty when it cannot be had.
inline std::wstring ExeDirectory() {
  wchar_t path[MAX_PATH * 2];
  const DWORD length = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
  if (length == 0 || length >= std::size(path)) {
    return {};
  }
  std::wstring dir(path, length);
  const size_t slash = dir.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash + 1);
}

inline bool FileExists(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// A DLL by its full path: its dependencies are looked for in its own folder and in the system
// folder, never in the working directory or on PATH.
inline HMODULE LoadByFullPath(const std::wstring& path, DWORD* error) {
  HMODULE module = LoadLibraryExW(
      path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  *error = module ? 0 : GetLastError();
  return module;
}

struct Version {
  uint64_t id = 0;
  std::string name;
  int major = 0;
  int minor = 0;
  int patch = 0;
  bool parsed = false;
};

// The first "N.M" or "N.M.P" in a version's name.
inline void ParseVersion(Version* v) {
  const char* p = v->name.c_str();
  while (*p && !(*p >= '0' && *p <= '9')) {
    ++p;
  }
  int parts[3] = {0, 0, 0};
  int count = 0;
  while (*p && count < 3) {
    if (!(*p >= '0' && *p <= '9')) {
      break;
    }
    int n = 0;
    while (*p >= '0' && *p <= '9') {
      n = n * 10 + (*p - '0');
      ++p;
    }
    parts[count++] = n;
    if (*p != '.') {
      break;
    }
    ++p;
  }
  v->major = parts[0];
  v->minor = parts[1];
  v->patch = parts[2];
  v->parsed = count >= 2;
}

inline bool NewerThan(const Version& a, const Version& b) {
  if (a.major != b.major) return a.major > b.major;
  if (a.minor != b.minor) return a.minor > b.minor;
  return a.patch > b.patch;
}

inline uint32_t SurfaceFormat(plume::RenderFormat format) {
  switch (format) {
    case plume::RenderFormat::R16G16B16A16_FLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case plume::RenderFormat::R32_FLOAT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    case plume::RenderFormat::R16G16_FLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case plume::RenderFormat::R8_UNORM: return FFX_API_SURFACE_FORMAT_R8_UNORM;  // the reactive mask
    default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
  }
}

// One image as the runtime describes it. Nothing is read back from the native object: the
// description is the one Plume made the texture with (2D, one level, one layer).
inline bool MakeResource(const TemporalImage& image, uint32_t state, FfxApiResource* out) {
  *out = FfxApiResource{};
  if (!image.texture) {
    return false;
  }
  void* handle = backend::NativeTextureHandle(image.texture);
  const uint32_t format = SurfaceFormat(image.format);
  if (!handle || format == FFX_API_SURFACE_FORMAT_UNKNOWN) {
    return false;
  }
  out->resource = handle;
  out->state = state;
  out->description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
  out->description.format = format;
  out->description.width = image.width;
  out->description.height = image.height;
  out->description.depth = 1;
  out->description.mipCount = 1;
  out->description.flags = FFX_API_RESOURCE_FLAGS_NONE;
  out->description.usage =
      (image.flags & (plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS))
          ? FFX_API_RESOURCE_USAGE_UAV
          : FFX_API_RESOURCE_USAGE_READ_ONLY;
  return true;
}

// The backend description of the API this process runs (a translation unit per API: see
// temporal_ffx_glue.h, and only the ones the build has).
inline bool FillBackend(Backend api, const backend::NativeDevice& native, ffx_glue::BackendDesc* out) {
#if defined(LARECOMP_GPU_HAS_D3D12)
  if (api == Backend::kD3D12) {
    return ffx_glue::dx12::FillBackendDesc(native, out);
  }
#endif
#if defined(LARECOMP_GPU_HAS_VULKAN)
  if (api == Backend::kVulkan) {
    return ffx_glue::vulkan::FillBackendDesc(native, out);
  }
#endif
  (void)api;
  (void)native;
  (void)out;
  return false;
}

}  // namespace mcla::gpu::ffx_common
