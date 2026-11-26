// Runs the real resolve copy pass (hlsl/resolve_ps.hlsl, the colour, single sample variant, compiled with the host shader flags of
// src/gpu/CMakeLists.txt) on the D3D12 software adapter (WARP) over a row of texels that hold what a 7e3 colour target
// (k_2_10_10_10_FLOAT, the host's R16G16B16A16_FLOAT) can hold when a guest draw has gone past what the console's target holds:
// the console's range ends at 31.875 (stored as written, the colour times 2^exp_bias), is never negative, and never holds an
// infinity or a NaN (the write clamps; Xenia's UnclampedFloat32To7e3); the host's float16 holds -65504 to 65504, an infinity and
// a NaN. The resolve (resolve_common.hlsli ResolveSample, kFlagSourceNanToZero and kFlagSourceCeiling7e3) is where that is made
// the console's again, so this checks, per colour channel and per sample, at the edge of the range and beyond it:
//   * a NaN of any payload and either sign reads as 0, in red, green, blue and alpha alike;
//   * red, green and blue are clamped to 0 .. 31.875 (negative and minus infinity: 0; plus infinity: 31.875), alpha is not clamped;
//   * 31.875 is kept as it is and the next float16 above it is not;
//   * the sum of the samples and the 2^bias scale come after the clamp (the scale times 31.875, never a bigger number);
//   * with the ceiling flag off (the HDR float scene) the colour is the loaded one, bit for bit, an infinity included, and the
//     NaN flag alone still zeroes a NaN;
//   * with neither flag (k_16_16_16_16_FLOAT sources) nothing is touched.
// The expected numbers are worked out here by the twin of the shader's per-sample rule (ResolveTwin), written out again on purpose.
//
// The program needs no renderer and no game files. It needs the D3D12 WARP adapter (Windows 10 and later) and the DXC compiler
// (dxc.exe with dxil.dll beside it): without either it says SKIPPED and exits 0 (a build machine with neither is not a failure of the
// resolve).
//
//   <test exe> [<src/gpu dir>] [<dir of dxc.exe>]       defaults: the folder of this file, $LARECOMP_DXC_DIR, the repo's download
//
// by hand: unit-test.ps1 -Test <this file>  (clang++ -std=c++23; the pragmas below link d3d12 and dxgi)

#include "../os/environment.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace fs = std::filesystem;

// A COM pointer, as little as this program needs (wrl/client.h lives in the winrt include folder, which the unit test script does not name).
template <class T>
class ComPtr {
 public:
  ComPtr() = default;
  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;
  ~ComPtr() { Reset(); }
  void Reset() {
    if (p_) {
      p_->Release();
      p_ = nullptr;
    }
  }
  T* Get() const { return p_; }
  T* operator->() const { return p_; }
  T** operator&() {  // for IID_PPV_ARGS and out parameters: the old pointer goes
    Reset();
    return &p_;
  }
  explicit operator bool() const { return p_ != nullptr; }

 private:
  T* p_ = nullptr;
};

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

// ---- float16 <-> float32 (round to nearest even; overflow to infinity; NaN keeps being a NaN) --------------------------------

float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16;
  const uint32_t exponent = (h >> 10) & 0x1Fu;
  const uint32_t mantissa = h & 0x3FFu;
  uint32_t bits;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {  // a denormal: normalise it
      int e = -1;
      uint32_t m = mantissa;
      do {
        ++e;
        m <<= 1;
      } while ((m & 0x400u) == 0);
      bits = sign | uint32_t(127 - 15 - e) << 23 | (m & 0x3FFu) << 13;
    }
  } else if (exponent == 31) {
    bits = sign | 0x7F800000u | mantissa << 13;  // an infinity or a NaN (its payload kept)
  } else {
    bits = sign | (exponent + 127 - 15) << 23 | mantissa << 13;
  }
  return std::bit_cast<float>(bits);
}

uint16_t FloatToHalf(float f) {
  const uint32_t bits = std::bit_cast<uint32_t>(f);
  const uint32_t sign = (bits >> 16) & 0x8000u;
  const uint32_t abs_bits = bits & 0x7FFFFFFFu;
  if (abs_bits > 0x7F800000u) {
    return uint16_t(sign | 0x7E00u);  // a NaN
  }
  if (abs_bits >= 0x477FF000u) {  // 65520 and up rounds to infinity
    return uint16_t(sign | 0x7C00u);
  }
  if (abs_bits < 0x38800000u) {  // below the smallest normal float16 (2^-14): a denormal, or 0; the unit there is 2^-24
    const double scaled = std::fabs(double(f)) * 16777216.0;
    const uint32_t q = uint32_t(std::nearbyint(scaled));  // round to nearest even; 1024 is the smallest normal, as it should
    return uint16_t(sign | q);
  }
  uint32_t exponent = (abs_bits >> 23) - 127 + 15;
  uint32_t mantissa = abs_bits & 0x7FFFFFu;
  uint32_t half = (exponent << 10) | (mantissa >> 13);
  const uint32_t rem = mantissa & 0x1FFFu;
  if (rem > 0x1000u || (rem == 0x1000u && (half & 1))) {
    ++half;  // may carry into the exponent: that is the right result
  }
  return uint16_t(sign | half);
}

bool IsNan(float f) { return (std::bit_cast<uint32_t>(f) & 0x7FFFFFFFu) > 0x7F800000u; }

// ---- the twin of the shader's rule -------------------------------------------------------------------------------------------

constexpr uint32_t kModeFloat = 3u;
constexpr uint32_t kFlagSourceNanToZero = 1u << 12;
constexpr uint32_t kFlagSourceCeiling7e3 = 1u << 13;
constexpr float kCeiling = 31.875f;

struct F4 {
  float v[4];
};

// resolve_common.hlsli ResolveSample for a non-gamma source: NaN of a component -> 0 (by the bits), then red, green and blue
// clamped to 0 .. 31.875 (max(min(x, 31.875), 0); no NaN is left to order), alpha as it is.
F4 ResolveTwin(F4 c, uint32_t mode) {
  if (mode & kFlagSourceNanToZero) {
    for (float& x : c.v) {
      if (IsNan(x)) {
        x = 0.0f;
      }
    }
  }
  if (mode & kFlagSourceCeiling7e3) {
    for (int i = 0; i < 3; ++i) {
      c.v[i] = std::max(std::min(c.v[i], kCeiling), 0.0f);
    }
  }
  return c;
}

// ---- DXC ---------------------------------------------------------------------------------------------------------------------

bool RunDxc(const fs::path& dxc, const std::wstring& args, const fs::path& log) {
  std::wstring cmd = L"\"" + dxc.wstring() + L"\" " + args;
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE out = CreateFileW(log.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  si.hStdOutput = out;
  si.hStdError = out;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
  mutable_cmd.push_back(0);
  const BOOL started = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  if (!started) {
    CloseHandle(out);
    return false;
  }
  WaitForSingleObject(pi.hProcess, 120000);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  CloseHandle(out);
  return code == 0;
}

std::vector<uint8_t> ReadFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ---- the push block of the pass (resolve_ps.hlsl ResolvePush, the colour variants) -------------------------------------------

struct Push {
  uint32_t source;
  uint32_t stencil;
  int32_t offset_x;
  int32_t offset_y;
  uint32_t samples;  // bits 0-3 count, bits 8 + 3i the host index of sample i
  float scale;
  uint32_t mode;
  uint32_t fill;
};
static_assert(sizeof(Push) == 32);

struct Rig {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3D12PipelineState> pso;
  ComPtr<ID3D12DescriptorHeap> srv_heap;
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  ComPtr<ID3D12Fence> fence;
  uint64_t fence_value = 0;
};

bool WaitIdle(Rig& r) {
  const uint64_t v = ++r.fence_value;
  if (FAILED(r.queue->Signal(r.fence.Get(), v))) {
    return false;
  }
  if (r.fence->GetCompletedValue() < v) {
    HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    r.fence->SetEventOnCompletion(v, e);
    WaitForSingleObject(e, 60000);
    CloseHandle(e);
  }
  return r.fence->GetCompletedValue() >= v;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = res;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  return b;
}

D3D12_HEAP_PROPERTIES Heap(D3D12_HEAP_TYPE t) {
  D3D12_HEAP_PROPERTIES h{};
  h.Type = t;
  return h;
}

D3D12_RESOURCE_DESC Tex2D(uint32_t w, uint32_t h, D3D12_RESOURCE_FLAGS flags) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = w;
  d.Height = h;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  d.SampleDesc.Count = 1;
  d.Flags = flags;
  return d;
}

// Draws the pass over a W x 1 target whose source is the W x 1 float16 texture `texels` (4 halves a texel), and returns the W x 4 halves
// it wrote. nullopt-like: an empty vector when the GPU side failed.
std::vector<uint16_t> RunPass(Rig& r, const std::vector<uint16_t>& texels, uint32_t width, const Push& push) {
  std::vector<uint16_t> result;
  ComPtr<ID3D12Resource> source, target, upload, readback;
  const D3D12_HEAP_PROPERTIES def = Heap(D3D12_HEAP_TYPE_DEFAULT);
  const D3D12_HEAP_PROPERTIES up = Heap(D3D12_HEAP_TYPE_UPLOAD);
  const D3D12_HEAP_PROPERTIES rb = Heap(D3D12_HEAP_TYPE_READBACK);
  D3D12_RESOURCE_DESC src_desc = Tex2D(width, 1, D3D12_RESOURCE_FLAG_NONE);
  D3D12_RESOURCE_DESC dst_desc = Tex2D(width, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  D3D12_CLEAR_VALUE clear{};
  clear.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  if (FAILED(r.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &src_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&source))) ||
      FAILED(r.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &dst_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&target)))) {
    return result;
  }
  const uint32_t row_pitch = (width * 8 + 255) & ~255u;
  D3D12_RESOURCE_DESC buf{};
  buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buf.Width = row_pitch;
  buf.Height = buf.DepthOrArraySize = buf.MipLevels = 1;
  buf.SampleDesc.Count = 1;
  buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(r.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))) ||
      FAILED(r.device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) {
    return result;
  }
  void* mapped = nullptr;
  upload->Map(0, nullptr, &mapped);
  std::memset(mapped, 0, row_pitch);
  std::memcpy(mapped, texels.data(), size_t(width) * 8);
  upload->Unmap(0, nullptr);

  // The descriptors: the source at slot 0 of the SRV heap (the other three tables point at the same heap and are never read), the target's RTV.
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  r.device->CreateShaderResourceView(source.Get(), &srv, r.srv_heap->GetCPUDescriptorHandleForHeapStart());
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = r.rtv_heap->GetCPUDescriptorHandleForHeapStart();
  r.device->CreateRenderTargetView(target.Get(), nullptr, rtv);

  r.allocator->Reset();
  r.list->Reset(r.allocator.Get(), r.pso.Get());
  D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
  dst.pResource = source.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.pResource = upload.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint.Footprint = {DXGI_FORMAT_R16G16B16A16_FLOAT, width, 1, 1, row_pitch};
  r.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  const D3D12_RESOURCE_BARRIER to_read = Transition(source.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  r.list->ResourceBarrier(1, &to_read);

  ID3D12DescriptorHeap* heaps[] = {r.srv_heap.Get()};
  r.list->SetDescriptorHeaps(1, heaps);
  r.list->SetGraphicsRootSignature(r.root.Get());
  const D3D12_GPU_DESCRIPTOR_HANDLE table = r.srv_heap->GetGPUDescriptorHandleForHeapStart();
  for (UINT i = 0; i < 4; ++i) {
    r.list->SetGraphicsRootDescriptorTable(i, table);
  }
  r.list->SetGraphicsRoot32BitConstants(4, 8, &push, 0);
  r.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  const D3D12_VIEWPORT vp{0, 0, float(width), 1, 0, 1};
  const D3D12_RECT sc{0, 0, LONG(width), 1};
  r.list->RSSetViewports(1, &vp);
  r.list->RSSetScissorRects(1, &sc);
  r.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  r.list->DrawInstanced(3, 1, 0, 0);
  const D3D12_RESOURCE_BARRIER to_copy = Transition(target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  r.list->ResourceBarrier(1, &to_copy);
  D3D12_TEXTURE_COPY_LOCATION from{}, to{};
  from.pResource = target.Get();
  from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  to.pResource = readback.Get();
  to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  to.PlacedFootprint.Footprint = {DXGI_FORMAT_R16G16B16A16_FLOAT, width, 1, 1, row_pitch};
  r.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  r.list->Close();
  ID3D12CommandList* lists[] = {r.list.Get()};
  r.queue->ExecuteCommandLists(1, lists);
  if (!WaitIdle(r)) {
    return result;
  }
  void* out = nullptr;
  const D3D12_RANGE range{0, row_pitch};
  readback->Map(0, &range, &out);
  result.resize(size_t(width) * 4);
  std::memcpy(result.data(), out, size_t(width) * 8);
  readback->Unmap(0, nullptr);
  return result;
}

// ---- the texels ------------------------------------------------------------------------------------------------------------------

struct Pattern {
  uint16_t bits;
  const char* name;
};
// What a channel may hold. 31.875 is 0x4FF8 in float16 (1.1111111b x 2^4: mantissa 1111111000b); the next float16 above it is 0x4FF9.
const Pattern kPatterns[] = {
    {0x0000, "+0"},           {0x8000, "-0"},          {0x3C00, "1"},          {0x4C00, "16"},
    {0x4FF8, "31.875"},       {0x4FF9, "31.875+ulp"},  {0x5000, "32"},         {0x5640, "100"},
    {0x7BFF, "65504"},        {0x7C00, "+inf"},        {0xFC00, "-inf"},       {0x7E00, "NaN"},
    {0x7C01, "sNaN"},         {0xFE00, "-NaN"},        {0xBC00, "-1"},         {0xFBFF, "-65504"},
    {0x0001, "denormal"},     {0x8001, "-denormal"},   {0x4FF7, "31.875-ulp"}, {0x3555, "1/3"},
};
constexpr uint32_t kPatternCount = sizeof(kPatterns) / sizeof(kPatterns[0]);

uint32_t Samples(uint32_t count) {
  // count in bits 0-3, the host index of every sample 0 (the source has one sample: a non-multisampled load ignores the index)
  return count;
}

void CheckSweep(Rig& r, uint32_t mode, float bias_scale, uint32_t count, const std::string& label) {
  // every pattern in every channel, the channels rotated so that each texel has four different patterns
  const uint32_t width = kPatternCount;
  std::vector<uint16_t> texels(size_t(width) * 4);
  for (uint32_t t = 0; t < width; ++t) {
    for (uint32_t c = 0; c < 4; ++c) {
      texels[size_t(t) * 4 + c] = kPatterns[(t + c * 5) % kPatternCount].bits;
    }
  }
  Push push{};
  push.source = 0;
  push.samples = Samples(count);
  push.scale = bias_scale / float(count);
  push.mode = mode;
  const std::vector<uint16_t> got = RunPass(r, texels, width, push);
  Check(got.size() == size_t(width) * 4, label + ": the pass ran and returned " + std::to_string(width * 4) + " halves");
  if (got.size() != size_t(width) * 4) {
    return;
  }
  for (uint32_t t = 0; t < width; ++t) {
    F4 in{};
    for (uint32_t c = 0; c < 4; ++c) {
      in.v[c] = HalfToFloat(texels[size_t(t) * 4 + c]);
    }
    const F4 one = ResolveTwin(in, mode);
    for (uint32_t c = 0; c < 4; ++c) {
      float sum = 0.0f;
      for (uint32_t i = 0; i < count; ++i) {
        sum += one.v[c];
      }
      const float want = sum * push.scale;
      const uint16_t want_bits = FloatToHalf(want);
      const uint16_t got_bits = got[size_t(t) * 4 + c];
      const bool want_nan = IsNan(want);
      const bool got_nan = (got_bits & 0x7C00u) == 0x7C00u && (got_bits & 0x3FFu) != 0;
      // A zero of either sign is a zero (the pass's `0 + x` is folded to x, so a loaded -0 comes out as -0). A sum past float16's range is the
      // adapter's to convert: the target's write may give the infinity or the largest finite number (WARP gives the largest finite).
      const bool want_overflow = (want_bits & 0x7FFFu) == 0x7C00u;
      const bool same = want_nan ? got_nan
                        : want_overflow ? (got_bits == want_bits || (got_bits & 0x7FFFu) == 0x7BFFu) && (got_bits & 0x8000u) == (want_bits & 0x8000u)
                                        : ((got_bits & 0x7FFFu) == 0 && (want_bits & 0x7FFFu) == 0) || got_bits == want_bits;
      Check(same, label + ": texel " + std::to_string(t) + " channel " + "RGBA"[c] + " loaded " + kPatterns[(t + c * 5) % kPatternCount].name +
                      ": got 0x" + [&] {
                        char b[8];
                        std::snprintf(b, sizeof(b), "%04X", got_bits);
                        return std::string(b);
                      }() + " (" + std::to_string(HalfToFloat(got_bits)) + "), want 0x" + [&] {
                        char b[8];
                        std::snprintf(b, sizeof(b), "%04X", want_bits);
                        return std::string(b);
                      }() + " (" + std::to_string(want) + ")");
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  fs::path src_dir = argc > 1 ? fs::path(argv[1]) : fs::path(__FILE__).parent_path();
  fs::path dxc_dir;
  if (argc > 2) {
    dxc_dir = argv[2];
  } else if (const auto env = larecomp::os::ReadEnvironment("LARECOMP_DXC_DIR")) {
    dxc_dir = *env;
  } else {
    dxc_dir = "C:\\mcv\\larecomp\\downloads\\dxc-v1.9.2609\\x\\bin\\x64";
  }
  const fs::path dxc = dxc_dir / "dxc.exe";
  if (!fs::exists(dxc) || !fs::exists(src_dir / "hlsl" / "resolve_ps.hlsl")) {
    std::printf("SKIPPED: no dxc.exe at %s or no hlsl folder under %s\n", dxc.string().c_str(), src_dir.string().c_str());
    return 0;
  }

  // The adapter: WARP.
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter> warp;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) {
    std::printf("SKIPPED: no WARP adapter\n");
    return 0;
  }
  Rig r;
  if (FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&r.device)))) {
    std::printf("SKIPPED: no D3D12 device on WARP\n");
    return 0;
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS opts{};
  r.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts));
  if (opts.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3) {
    std::printf("SKIPPED: WARP binding tier %d (the pass's bindless texture sets need tier 3)\n", int(opts.ResourceBindingTier));
    return 0;
  }

  // Compile the pass and the full-screen VS with the flags of src/gpu/CMakeLists.txt.
  const fs::path work = fs::temp_directory_path() / "larecomp_resolve_range_warp_test";
  fs::create_directories(work);
  const std::wstring inc = L"-I \"" + (src_dir / "hlsl").wstring() + L"\" -I \"" + (src_dir / ".." / ".." / "thirdparty").lexically_normal().wstring() + L"\"";
  const fs::path ps_out = work / "resolve_color_ps.dxil", vs_out = work / "fullscreen_vs.dxil";
  const bool ps_ok = RunDxc(dxc, L"-T ps_6_0 -HV 2021 -all-resources-bound -Wno-ignored-attributes -D RESOLVE_DEPTH=0 -D RESOLVE_MSAA=0 " + inc + L" -Fo \"" +
                                     ps_out.wstring() + L"\" \"" + (src_dir / "hlsl" / "resolve_ps.hlsl").wstring() + L"\"",
                            work / "ps.log");
  const bool vs_ok = RunDxc(dxc, L"-T vs_6_0 -HV 2021 -all-resources-bound " + inc + L" -Fo \"" + vs_out.wstring() + L"\" \"" +
                                     (src_dir / "hlsl" / "fullscreen_vs.hlsl").wstring() + L"\"",
                            work / "vs.log");
  Check(ps_ok, "resolve_ps.hlsl compiles (colour, one sample; log: " + (work / "ps.log").string() + ")");
  Check(vs_ok, "fullscreen_vs.hlsl compiles");
  if (!ps_ok || !vs_ok) {
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return 1;
  }
  const std::vector<uint8_t> ps = ReadFile(ps_out), vs = ReadFile(vs_out);

  // The root signature the host passes use (host_pass_common.hlsli): four unbounded SRV tables (spaces 0 to 3) and eight root constants at b0, space4.
  D3D12_DESCRIPTOR_RANGE ranges[4];
  D3D12_ROOT_PARAMETER params[5]{};
  for (UINT i = 0; i < 4; ++i) {
    ranges[i] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, i, 0};
    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[i].DescriptorTable = {1, &ranges[i]};
    params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  }
  params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[4].Constants = {0, 4, 8};
  params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC rs{};
  rs.NumParameters = 5;
  rs.pParameters = params;
  rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  ComPtr<ID3DBlob> rs_blob, rs_err;
  // D3D12SerializeRootSignature lives in d3d12.dll
  if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1_0, &rs_blob, &rs_err)) ||
      FAILED(r.device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&r.root)))) {
    std::printf("FAILED: the root signature: %s\n", rs_err ? static_cast<const char*>(rs_err->GetBufferPointer()) : "");
    return 1;
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = r.root.Get();
  pso.VS = {vs.data(), vs.size()};
  pso.PS = {ps.data(), ps.size()};
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso.RasterizerState.DepthClipEnable = TRUE;
  pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.SampleMask = UINT_MAX;
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
  pso.SampleDesc.Count = 1;
  if (FAILED(r.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&r.pso)))) {
    std::printf("FAILED: the pipeline state of the resolve pass\n");
    return 1;
  }
  D3D12_COMMAND_QUEUE_DESC qd{};
  r.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&r.queue));
  r.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&r.allocator));
  r.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, r.allocator.Get(), nullptr, IID_PPV_ARGS(&r.list));
  r.list->Close();
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 8;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  r.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&r.srv_heap));
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hd.NumDescriptors = 1;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  r.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&r.rtv_heap));
  r.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&r.fence));

  // The same table of numbers under every mode the pass is given for a colour source:
  //   7e3 on an SDR frame (NaN flag + ceiling), 7e3 on a float scene frame (NaN flag only), a 16F source (none),
  // with the game's exponent bias (2^4: the console's 31.875 is a colour of 1.99) and without, with one, two and four samples.
  const uint32_t kOn = kModeFloat | kFlagSourceNanToZero | kFlagSourceCeiling7e3;
  const uint32_t kNanOnly = kModeFloat | kFlagSourceNanToZero;
  const uint32_t kNone = kModeFloat;
  CheckSweep(r, kOn, 1.0f, 1, "7e3 SDR, scale 1, 1 sample");
  CheckSweep(r, kOn, 16.0f, 1, "7e3 SDR, scale 16, 1 sample");
  CheckSweep(r, kOn, 1.0f / 16.0f, 1, "7e3 SDR, scale 1/16, 1 sample");
  CheckSweep(r, kOn, 1.0f, 2, "7e3 SDR, scale 1, 2 samples");
  CheckSweep(r, kOn, 1.0f, 4, "7e3 SDR, scale 1, 4 samples");
  CheckSweep(r, kNanOnly, 1.0f, 1, "7e3 float scene (NaN flag only), 1 sample");
  CheckSweep(r, kNanOnly, 1.0f, 2, "7e3 float scene (NaN flag only), 2 samples");
  CheckSweep(r, kNone, 1.0f, 1, "16F source (no flags), 1 sample");

  // The named edge cases, spelled out once so that a reader sees the claim without the table (each also is a row of the sweeps above).
  {
    // red = +inf, green = 31.875, blue = NaN, alpha = +inf
    const std::vector<uint16_t> t = {0x7C00, 0x4FF8, 0x7E00, 0x7C00};
    Push p{};
    p.scale = 1.0f;
    p.samples = 1;
    p.mode = kOn;
    const std::vector<uint16_t> got = RunPass(r, t, 1, p);
    Check(got.size() == 4 && HalfToFloat(got[0]) == 31.875f, "an infinite red reads as 31.875");
    Check(got.size() == 4 && HalfToFloat(got[1]) == 31.875f, "a green of 31.875 stays 31.875");
    Check(got.size() == 4 && got[2] == 0x0000, "a NaN blue reads as 0");
    Check(got.size() == 4 && got[3] == 0x7C00, "an infinite alpha is not clamped (the 2-bit alpha of the console is not this pass's business)");
    p.mode = kNanOnly;
    const std::vector<uint16_t> got2 = RunPass(r, t, 1, p);
    Check(got2.size() == 4 && got2[0] == 0x7C00 && got2[2] == 0x0000, "the float scene keeps an infinity and still zeroes a NaN");
  }
  {
    // the one that makes pink and green: an overflowed red and blue with a finite green next to a NaN red and blue
    const std::vector<uint16_t> t = {0x7C00, 0x3C00, 0x7C00, 0x3C00, 0x7E00, 0x3C00, 0x7E00, 0x3C00};
    Push p{};
    p.scale = 1.0f;
    p.samples = 1;
    p.mode = kOn;
    const std::vector<uint16_t> got = RunPass(r, t, 2, p);
    const bool shape = got.size() == 8 && HalfToFloat(got[0]) == 31.875f && got[1] == 0x3C00 && HalfToFloat(got[2]) == 31.875f && got[4] == 0 && got[5] == 0x3C00 &&
                       got[6] == 0;
    Check(shape, "an infinite red and blue over a green of 1 become 31.875, 1, 31.875 (pink); a NaN red and blue become 0, 1, 0 (green): the clamp is per channel");
  }

  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
