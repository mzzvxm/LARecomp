// LARecomp shader pack builder -- dxcompiler loaded at run time (see dxc.h).

#include "dxc.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#else
#include <dlfcn.h>
#endif

// dxcapi.h declares DxcCreateInstance as a dllimport. It is never called by
// name here (the function pointer comes from GetProcAddress / dlsym), so the
// declaration must not ask for an import library.
#ifndef DXC_API_IMPORT
#define DXC_API_IMPORT
#endif
// CHECK: header search path. thirdparty/dxc-bin/inc (dxcapi.h, WinAdapter.h) is
// expected on the include path of the shaderpack target; the SDK's copy under
// thirdparty/dxc/include works too if it is the same dxcapi.h generation
// (IDxcCompiler3, IDxcResult, IDxcVersionInfo3 are needed).
#include <dxcapi.h>

namespace larecomp::shaderpack {

namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Minimal COM smart pointer (no WRL / ATL: the file also builds off Windows).
template <class T>
class ComPtr {
 public:
  ComPtr() = default;
  ~ComPtr() { Reset(); }
  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;
  ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
  ComPtr& operator=(ComPtr&& other) noexcept {
    if (this != &other) {
      Reset();
      ptr_ = other.ptr_;
      other.ptr_ = nullptr;
    }
    return *this;
  }
  T* Get() const { return ptr_; }
  T* operator->() const { return ptr_; }
  T** Put() {  // for out parameters; releases what it held
    Reset();
    return &ptr_;
  }
  void Attach(T* ptr) {
    Reset();
    ptr_ = ptr;
  }
  void Reset() {
    if (ptr_) {
      ptr_->Release();
      ptr_ = nullptr;
    }
  }

 private:
  T* ptr_ = nullptr;
};

std::string PathText(const std::filesystem::path& path) {
  const auto u8 = path.u8string();  // never throws, unlike path::string() on a non-ANSI path
  return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

DxcStatus Failure(DxcError error, std::string message) {
  DxcStatus status;
  status.error = error;
  status.message = std::move(message);
  return status;
}

uint32_t ReadLe32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

constexpr uint32_t kSpirvMagic = 0x07230203;

void AppendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

// A text output of an IDxcResult (errors, disassembly) as UTF-8. dxc hands it
// out as IDxcBlobUtf8 when asked; the fallback decodes whatever encoding the
// blob says it has, so a build never loses the message.
std::string GetText(IDxcResult* result, DXC_OUT_KIND kind) {
  if (!result->HasOutput(kind)) return {};
  ComPtr<IDxcBlobUtf8> utf8;
  if (SUCCEEDED(result->GetOutput(kind, IID_PPV_ARGS(utf8.Put()), nullptr)) && utf8.Get()) {
    const char* text = utf8->GetStringPointer();
    return text ? std::string(text, size_t(utf8->GetStringLength())) : std::string();
  }
  ComPtr<IDxcBlob> blob;
  if (FAILED(result->GetOutput(kind, IID_PPV_ARGS(blob.Put()), nullptr)) || !blob.Get()) return {};
  UINT32 code_page = 0;
  BOOL known = FALSE;
  ComPtr<IDxcBlobEncoding> encoding;
  if (SUCCEEDED(blob->QueryInterface(IID_PPV_ARGS(encoding.Put()))) && encoding.Get()) {
    encoding->GetEncoding(&known, &code_page);
  }
  const auto* bytes = static_cast<const uint8_t*>(blob->GetBufferPointer());
  const size_t size = size_t(blob->GetBufferSize());
  std::string text;
  if (known && code_page == DXC_CP_UTF16) {
    for (size_t i = 0; i + 1 < size; i += 2) {
      uint32_t unit = uint32_t(bytes[i]) | (uint32_t(bytes[i + 1]) << 8);
      if (unit >= 0xD800 && unit < 0xDC00 && i + 3 < size) {
        const uint32_t low = uint32_t(bytes[i + 2]) | (uint32_t(bytes[i + 3]) << 8);
        if (low >= 0xDC00 && low < 0xE000) {
          unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
          i += 2;
        }
      }
      AppendUtf8(text, unit);
    }
  } else if (known && code_page == DXC_CP_UTF32) {
    for (size_t i = 0; i + 3 < size; i += 4) AppendUtf8(text, ReadLe32(bytes + i));
  } else if (bytes != nullptr) {
    text.assign(reinterpret_cast<const char*>(bytes), size);
  }
  while (!text.empty() && text.back() == '\0') text.pop_back();
  return text;
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

#ifdef _WIN32
using LibHandle = HMODULE;
const char* const kCompilerLibraryName = "dxcompiler.dll";
#else
using LibHandle = void*;
#if defined(__APPLE__)
const char* const kCompilerLibraryName = "libdxcompiler.dylib";
#else
const char* const kCompilerLibraryName = "libdxcompiler.so";
#endif
#endif

// By full path. On Windows with the altered search path, so that anything the
// DLL imports is looked for in its own folder first (needs an absolute path with
// backslashes).
LibHandle OpenLibrary(const std::filesystem::path& path, std::string* why) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    *why = "file not found";
    return nullptr;
  }
#ifdef _WIN32
  std::filesystem::path full = std::filesystem::absolute(path, ec);
  if (ec) full = path;
  full.make_preferred();
  HMODULE handle = LoadLibraryExW(full.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!handle) {
    const DWORD code = GetLastError();
    *why = std::format("LoadLibrary error {} ({})", uint32_t(code),
                       std::system_category().message(int(code)));
  }
  return handle;
#else
  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    const char* error = dlerror();
    *why = error ? error : "dlopen failed";
  }
  return handle;
#endif
}

void* FindSymbol(LibHandle handle, const char* name) {
#ifdef _WIN32
  return reinterpret_cast<void*>(GetProcAddress(handle, name));
#else
  return dlsym(handle, name);
#endif
}

void CloseLibrary(LibHandle handle) {
  if (!handle) return;
#ifdef _WIN32
  FreeLibrary(handle);
#else
  dlclose(handle);
#endif
}

// dxc hands out version strings allocated with CoTaskMemAlloc (malloc off
// Windows). ole32 is always loaded by then (dxcompiler imports it), so it is
// looked up instead of linked; if it cannot be found the few bytes are leaked.
void FreeCoString(char* text) {
  if (!text) return;
#ifdef _WIN32
  using FreeFn = void(WINAPI*)(LPVOID);
  if (HMODULE ole = GetModuleHandleW(L"ole32.dll")) {
    if (auto free_fn = reinterpret_cast<FreeFn>(GetProcAddress(ole, "CoTaskMemFree"))) free_fn(text);
  }
#else
  CoTaskMemFree(text);  // WinAdapter.h
#endif
}

// CHECK: whether the compiler object answers QueryInterface for the three
// IDxcVersionInfo flavours is from memory of dxcompiler's sources, not seen; every
// step is optional and the result is only logged.
std::string ReadVersion(IDxcCompiler3* compiler) {
  std::string version;
  ComPtr<IDxcVersionInfo> info;
  if (SUCCEEDED(compiler->QueryInterface(IID_PPV_ARGS(info.Put()))) && info.Get()) {
    UINT32 major = 0, minor = 0;
    if (SUCCEEDED(info->GetVersion(&major, &minor))) version = std::format("{}.{}", major, minor);
  }
  ComPtr<IDxcVersionInfo2> info2;
  if (SUCCEEDED(compiler->QueryInterface(IID_PPV_ARGS(info2.Put()))) && info2.Get()) {
    UINT32 count = 0;
    char* hash = nullptr;
    if (SUCCEEDED(info2->GetCommitInfo(&count, &hash))) {
      version += std::format(" commit-count {} {}", count, hash ? hash : "");
      FreeCoString(hash);
    }
  }
  ComPtr<IDxcVersionInfo3> info3;
  if (SUCCEEDED(compiler->QueryInterface(IID_PPV_ARGS(info3.Put()))) && info3.Get()) {
    char* custom = nullptr;
    if (SUCCEEDED(info3->GetCustomVersionString(&custom)) && custom) {
      version += std::format(" [{}]", custom);
      FreeCoString(custom);
    }
  }
  return version;
}

// The v8 builder's arguments, one token each (compile_dxil / compile_spirv in
// native/tools/build_shader_pack_v8.py; "-Fo <file>" and the file path are the
// parts a memory buffer does not need).
std::vector<std::wstring> BuildArguments(const CompileRequest& request, bool spirv) {
  std::vector<std::wstring> args;
  args.emplace_back(L"-T");
  args.emplace_back(request.stage == ShaderStage::kVertex ? L"vs_6_0" : L"ps_6_0");
  args.emplace_back(L"-E");
  args.emplace_back(L"main");
  if (spirv) {
    args.emplace_back(L"-HV");
    args.emplace_back(L"2021");
    args.emplace_back(L"-all-resources-bound");
    args.emplace_back(L"-spirv");
    args.emplace_back(L"-fvk-use-dx-layout");
    if (request.stage == ShaderStage::kVertex) args.emplace_back(L"-fvk-invert-y");
    args.emplace_back(L"-Qstrip_debug");
  }
  args.push_back(L"-DXENOS_SPEC_CONSTANTS=" + std::to_wstring(request.spec_constants));
  return args;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public helpers
// ---------------------------------------------------------------------------

std::string_view StageProfile(ShaderStage stage) {
  return stage == ShaderStage::kVertex ? "vs_6_0" : "ps_6_0";
}

bool DxilIsSigned(std::span<const uint8_t> container) {
  if (container.size() < 20) return false;
  for (size_t i = 4; i < 20; ++i) {
    if (container[i] != 0) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// DxcRuntime
// ---------------------------------------------------------------------------

struct DxcRuntime::Impl {
  LibHandle compiler_library = nullptr;
  LibHandle dxil_library = nullptr;  // our own reference to the signing library (Windows)
  DxcCreateInstanceProc create_instance = nullptr;
};

struct DxcWorker::Impl {
  std::shared_ptr<DxcRuntime> runtime;  // keeps the DLLs mapped while the compiler lives
  ComPtr<IDxcCompiler3> compiler;

  DxcStatus Compile(const CompileRequest& request, bool spirv, std::vector<uint8_t>* blob);
};

DxcRuntime::DxcRuntime() : impl_(std::make_unique<Impl>()) {}

DxcRuntime::~DxcRuntime() {
  // Workers hold a reference to this object, so none is alive by now.
  if (impl_) {
    CloseLibrary(impl_->compiler_library);
    CloseLibrary(impl_->dxil_library);
  }
}

std::shared_ptr<DxcRuntime> DxcRuntime::Load(const std::filesystem::path& dxc_dir,
                                             const LoadOptions& options, DxcStatus* status) {
  DxcStatus local_status;
  if (!status) status = &local_status;
  *status = DxcStatus{};

  std::shared_ptr<DxcRuntime> runtime(new DxcRuntime());
  runtime->compiler_path_ = dxc_dir / kCompilerLibraryName;

#ifdef _WIN32
  if (options.need_dxil) {
    const std::filesystem::path dxil_path = dxc_dir / "dxil.dll";
    // A dxil.dll that is already in the process (from another folder) is what
    // dxcompiler's own LoadLibrary("dxil.dll") would get, and its version goes
    // into the DXIL (dx.valver) and its signature: say so.
    if (HMODULE existing = GetModuleHandleW(L"dxil.dll")) {
      std::wstring existing_path(32768, L'\0');
      const DWORD n = GetModuleFileNameW(existing, existing_path.data(), DWORD(existing_path.size()));
      existing_path.resize(n);
      std::error_code ec;
      if (n == 0 || !std::filesystem::equivalent(std::filesystem::path(existing_path), dxil_path, ec)) {
        runtime->notes_ += std::format(
            "a dxil.dll from {} was already loaded in this process; DXIL is signed by that one, "
            "not by {}\n",
            n ? PathText(std::filesystem::path(existing_path)) : std::string("an unknown path"),
            PathText(dxil_path));
      }
    }
    std::string why;
    runtime->impl_->dxil_library = OpenLibrary(dxil_path, &why);
    if (!runtime->impl_->dxil_library) {
      *status = Failure(DxcError::kLoadFailed,
                        std::format("cannot load {} ({}): without it the DXIL would not be signed",
                                    PathText(dxil_path), why));
      return nullptr;
    }
  }
#else
  (void)options;
#endif

  std::string why;
  runtime->impl_->compiler_library = OpenLibrary(runtime->compiler_path_, &why);
  if (!runtime->impl_->compiler_library) {
    *status = Failure(DxcError::kLoadFailed, std::format("cannot load {} ({})",
                                                         PathText(runtime->compiler_path_), why));
    return nullptr;
  }
  runtime->impl_->create_instance = reinterpret_cast<DxcCreateInstanceProc>(
      FindSymbol(runtime->impl_->compiler_library, "DxcCreateInstance"));
  if (!runtime->impl_->create_instance) {
    *status = Failure(DxcError::kLoadFailed,
                      std::format("{} has no DxcCreateInstance", PathText(runtime->compiler_path_)));
    return nullptr;
  }

  // One instance, made here: dxc's one-time start-up work runs on this thread
  // before the workers start in parallel.
  DxcStatus worker_status;
  std::unique_ptr<DxcWorker> probe = runtime->CreateWorker(&worker_status);
  if (!probe) {
    *status = worker_status;
    return nullptr;
  }
  runtime->version_ = ReadVersion(probe->impl_->compiler.Get());
  probe.reset();
  return runtime;
}

std::unique_ptr<DxcWorker> DxcRuntime::CreateWorker(DxcStatus* status) {
  DxcStatus local_status;
  if (!status) status = &local_status;
  *status = DxcStatus{};

  IDxcCompiler3* raw = nullptr;
  const HRESULT hr = impl_->create_instance(CLSID_DxcCompiler, IID_PPV_ARGS(&raw));
  if (FAILED(hr) || raw == nullptr) {
    *status = Failure(DxcError::kLoadFailed,
                      std::format("DxcCreateInstance(CLSID_DxcCompiler) failed with HRESULT 0x{:08X}",
                                  uint32_t(hr)));
    return nullptr;
  }
  std::unique_ptr<DxcWorker> worker(new DxcWorker());
  worker->impl_ = std::make_unique<DxcWorker::Impl>();
  worker->impl_->runtime = shared_from_this();
  worker->impl_->compiler.Attach(raw);
  return worker;
}

// ---------------------------------------------------------------------------
// DxcWorker
// ---------------------------------------------------------------------------

DxcWorker::DxcWorker() = default;

DxcWorker::~DxcWorker() {
  // The compiler first, then the runtime reference (which may unload the DLL).
  if (impl_) {
    impl_->compiler.Reset();
    impl_->runtime.reset();
  }
}

DxcStatus DxcWorker::Impl::Compile(const CompileRequest& request, bool spirv,
                                   std::vector<uint8_t>* blob) {
  const std::string label =
      std::format("{} ({}, spec={}, {})", request.name.empty() ? std::string_view("<unnamed shader>") : request.name,
                  StageProfile(request.stage), request.spec_constants, spirv ? "SPIR-V" : "DXIL");

  const std::vector<std::wstring> arguments = BuildArguments(request, spirv);
  std::vector<LPCWSTR> argv;
  argv.reserve(arguments.size());
  for (const std::wstring& argument : arguments) argv.push_back(argument.c_str());

  // UTF-8 stated outright: with 0 dxc would read non-ASCII bytes in the player's
  // ANSI code page, and the pack must not depend on it. (The HLSL the generator
  // writes is plain ASCII; all 2333 files of the v8 tree were checked.)
  DxcBuffer source{};
  source.Ptr = request.hlsl.data();
  source.Size = request.hlsl.size();
  source.Encoding = DXC_CP_UTF8;

  ComPtr<IDxcResult> result;
  const HRESULT hr = compiler->Compile(&source, argv.data(), UINT32(argv.size()), nullptr,
                                       IID_PPV_ARGS(result.Put()));
  if (FAILED(hr) || !result.Get()) {
    return Failure(DxcError::kCompileFailed,
                   std::format("dxc failed for {}:\nIDxcCompiler3::Compile failed with HRESULT 0x{:08X}",
                               label, uint32_t(hr)));
  }

  HRESULT status = S_OK;
  const HRESULT status_hr = result->GetStatus(&status);
  const std::string diagnostics = GetText(result.Get(), DXC_OUT_ERRORS);
  if (FAILED(status_hr) || FAILED(status)) {
    return Failure(DxcError::kCompileFailed,
                   std::format("dxc failed for {}:\n{}", label,
                               diagnostics.empty() ? std::string("DXC failed without emitting diagnostics.")
                                                   : diagnostics));
  }

  ComPtr<IDxcBlob> object;
  if (!result->HasOutput(DXC_OUT_OBJECT) ||
      FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(object.Put()), nullptr)) || !object.Get() ||
      object->GetBufferSize() == 0) {
    return Failure(DxcError::kCompileFailed,
                   std::format("dxc produced no object for {}:\n{}", label, diagnostics));
  }
  const auto* data = static_cast<const uint8_t*>(object->GetBufferPointer());
  const size_t size = size_t(object->GetBufferSize());
  blob->assign(data, data + size);

  if (!spirv) {
    if (size < 4 || std::memcmp(data, "DXBC", 4) != 0) {
      blob->clear();
      return Failure(DxcError::kBadBlob, std::format("not DXBC: {}", label));
    }
    if (!DxilIsSigned(*blob)) {
      blob->clear();
      return Failure(DxcError::kNotSigned,
                     std::format("DXIL is not signed for {}: dxcompiler did not find dxil.dll "
                                 "(dxc said: {})",
                                 label, diagnostics.empty() ? std::string("nothing") : diagnostics));
    }
  } else if (size < 20 || size % 4 != 0 || ReadLe32(data) != kSpirvMagic) {
    blob->clear();
    return Failure(DxcError::kBadBlob, std::format("not SPIR-V: {}", label));
  }
  return DxcStatus{};
}

DxcStatus DxcWorker::CompileDxil(const CompileRequest& request, std::vector<uint8_t>* blob) {
#ifdef _WIN32
  return impl_->Compile(request, false, blob);
#else
  (void)request;
  (void)blob;
  return Failure(DxcError::kUnavailable,
                 "DXIL is signed by dxil.dll, which only the Windows build loads; only SPIR-V is built here");
#endif
}

DxcStatus DxcWorker::CompileSpirv(const CompileRequest& request, std::vector<uint8_t>* blob) {
  return impl_->Compile(request, true, blob);
}

DxcStatus DxcWorker::DisassembleDxil(std::span<const uint8_t> container, std::string* text) {
  text->clear();
  DxcBuffer object{};
  object.Ptr = container.data();
  object.Size = container.size();
  object.Encoding = 0;  // binary

  ComPtr<IDxcResult> result;
  const HRESULT hr = impl_->compiler->Disassemble(&object, IID_PPV_ARGS(result.Put()));
  if (FAILED(hr) || !result.Get()) {
    return Failure(DxcError::kDisassembleFailed,
                   std::format("IDxcCompiler3::Disassemble failed with HRESULT 0x{:08X}", uint32_t(hr)));
  }
  HRESULT status = S_OK;
  const HRESULT status_hr = result->GetStatus(&status);
  if (FAILED(status_hr) || FAILED(status)) {
    const std::string errors = GetText(result.Get(), DXC_OUT_ERRORS);
    return Failure(DxcError::kDisassembleFailed,
                   std::format("dxc could not disassemble the DXIL: {}",
                               errors.empty() ? std::string("no diagnostics") : errors));
  }
  *text = GetText(result.Get(), DXC_OUT_DISASSEMBLY);
  if (text->empty()) {
    return Failure(DxcError::kDisassembleFailed, "dxc returned an empty disassembly");
  }
  return DxcStatus{};
}

}  // namespace larecomp::shaderpack
