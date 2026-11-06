#pragma once
// ===========================================================================
// LARecomp shader pack builder -- dxcompiler loaded at run time
// ===========================================================================
// The shader packs are built on the player's PC from the translated HLSL. The
// blobs have to be the ones the Python v8 builder (native/tools/
// build_shader_pack_v8.py, written by mzzvxm) gets from dxc.exe 1.8.2407.7, so
// the arguments below are copied from its compile_dxil / compile_spirv
// one for one, as separate tokens:
//
//   DXIL   -T vs_6_0|ps_6_0 -E main -DXENOS_SPEC_CONSTANTS=<v>
//   SPIR-V -T vs_6_0|ps_6_0 -E main -HV 2021 -all-resources-bound -spirv
//          -fvk-use-dx-layout [-fvk-invert-y: vertex shaders] -Qstrip_debug
//          -DXENOS_SPEC_CONSTANTS=<v>
//
// (The generator's own dxc_compiler.cpp joins "-T ps_6_0" into one token and
// adds -Qstrip_reflect / -Wno-ignored-attributes to its DXIL; the pack never
// did, so neither does this.)
//
// Loading: dxcompiler.dll comes from the folder the builder is given, by full
// path, and is released again when the last DxcRuntime / DxcWorker reference
// goes away (the memory goes back after the build). dxil.dll is loaded first,
// also by full path: dxcompiler finds it later with a plain LoadLibrary("dxil.dll"),
// and a module of that name that is already in the process satisfies that, so
// the DXIL gets signed by the dxil.dll next to the pinned dxcompiler and not by
// whatever the search order would find. Every DXIL blob is checked for a
// signature before it is returned. Off Windows only SPIR-V is built
// (libdxcompiler.so through dlopen); that path has never been run.
//
// Threading: DxcRuntime is shared (thread safe once loaded). A DxcWorker owns
// one IDxcCompiler3 and must be used by one thread at a time: create one per
// worker thread (the generator's thread_local DxcCompiler does the same).
//
// No Plume, no rex runtime, no logging: errors come back as text.
// ===========================================================================

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace larecomp::shaderpack {

enum class ShaderStage : uint8_t { kVertex, kPixel };

// "vs_6_0" / "ps_6_0": the profile both formats compile with.
std::string_view StageProfile(ShaderStage stage);

enum class DxcError : uint8_t {
  kNone,
  kUnavailable,       // not possible on this platform (DXIL off Windows)
  kLoadFailed,        // dxcompiler / dxil.dll missing or not loadable, or no DxcCreateInstance
  kCompileFailed,     // dxc rejected the HLSL (message has its text) or returned no object
  kBadBlob,           // the object is not a DXBC container / a SPIR-V module
  kNotSigned,         // DXIL container hash is all zero: dxil.dll was not found by dxcompiler
  kDisassembleFailed,
};

struct DxcStatus {
  DxcError error = DxcError::kNone;
  std::string message;  // empty on success; names the shader and carries dxc's own text
  bool ok() const { return error == DxcError::kNone; }
};

// One shader variant. `hlsl` is the translated source exactly as the generator
// wrote it (a memory buffer, never a file: dxc is given no file name). `name`
// only decorates messages ("group__entry"); it is not passed to dxc.
struct CompileRequest {
  std::string_view hlsl;
  ShaderStage stage = ShaderStage::kPixel;
  uint32_t spec_constants = 0;  // -DXENOS_SPEC_CONSTANTS=<value>: 0 (variant 0) or the shader's whole mask (variant 1)
  std::string_view name;
};

// What a DXIL container's hash dwords (bytes 4..19) say: a signed container has
// a non-zero digest. The generator asserts the same on the first dword; here a
// blob counts as unsigned only when all four are zero (same answer on every
// real unsigned blob, no false alarm on the one digest in 2^32 whose first
// dword is 0).
bool DxilIsSigned(std::span<const uint8_t> container);

class DxcWorker;

class DxcRuntime : public std::enable_shared_from_this<DxcRuntime> {
 public:
  struct LoadOptions {
    // Windows: load dxil.dll from the same folder before any compile so that
    // DXIL is signed. A SPIR-V only build can turn it off (and then does not
    // need dxil.dll to exist).
    bool need_dxil = true;
  };

  // Loads <dxc_dir>/dxcompiler.dll (libdxcompiler.so off Windows), resolves
  // DxcCreateInstance and makes (and drops) one compiler instance, so dxc's
  // one-time start-up work happens here on the calling thread, before workers
  // run in parallel. Returns null with *status filled in on failure.
  static std::shared_ptr<DxcRuntime> Load(const std::filesystem::path& dxc_dir,
                                          const LoadOptions& options, DxcStatus* status);

  ~DxcRuntime();
  DxcRuntime(const DxcRuntime&) = delete;
  DxcRuntime& operator=(const DxcRuntime&) = delete;

  // A compiler for one thread. It keeps this runtime (and so the DLLs) alive.
  std::unique_ptr<DxcWorker> CreateWorker(DxcStatus* status);

  const std::filesystem::path& compiler_path() const { return compiler_path_; }
  // Best effort, may be empty: "1.8", plus commit count / hash and dxc's own
  // version string when the compiler reports them. For logs and the stamp.
  const std::string& version() const { return version_; }
  // Things worth a log line that are not errors (for example a dxil.dll from
  // another folder was already in the process).
  const std::string& notes() const { return notes_; }

 private:
  struct Impl;
  DxcRuntime();
  std::unique_ptr<Impl> impl_;
  std::filesystem::path compiler_path_;
  std::string version_;
  std::string notes_;
  friend class DxcWorker;
};

class DxcWorker {
 public:
  ~DxcWorker();
  DxcWorker(const DxcWorker&) = delete;
  DxcWorker& operator=(const DxcWorker&) = delete;

  // On success *blob holds the container / module and the status is ok. Magic
  // checks as the Python: DXIL starts "DXBC" and must be signed; SPIR-V is at
  // least 20 bytes, a multiple of 4, with the magic 0x07230203.
  DxcStatus CompileDxil(const CompileRequest& request, std::vector<uint8_t>* blob);
  DxcStatus CompileSpirv(const CompileRequest& request, std::vector<uint8_t>* blob);

  // IDxcCompiler3::Disassemble of a DXIL container (the text `dxc -dumpbin` prints).
  DxcStatus DisassembleDxil(std::span<const uint8_t> container, std::string* text);

 private:
  friend class DxcRuntime;
  struct Impl;
  DxcWorker();
  std::unique_ptr<Impl> impl_;
};

}  // namespace larecomp::shaderpack
