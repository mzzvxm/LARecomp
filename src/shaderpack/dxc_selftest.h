#pragma once
// ===========================================================================
// TEST ONLY -- not used by the game or by the pack build.
// ===========================================================================
// Proves the in-process dxc path (dxc.h) gives the same bytes as the Python
// v8 builder's dxc.exe: for each translated .hlsl file of the existing tree
// (for example native/hlsl/mcla_shaders_hlsl_v8) it compiles every variant,
// finds the file's entry in the existing v8 pack, and compares the blobs byte
// for byte. It also runs the contract checks (contract_checks.h) on the HLSL
// and on the blobs it just built, which exercises them on real data: the whole
// v8 pack passes them, so any problem reported here is a port bug.
//
// How the entry is found: the pack is keyed by microcode, which an .hlsl file
// does not carry. So the DXIL variant 0 blob is compiled first and its FNV-1a
// 64 is looked up among the pack's DXIL 0 blob hashes (SPIR-V 0 as a second
// try). When the bytes differ that lookup finds nothing: pass the key of the
// entry (from compare_packs.py or the generator's report) and the file is
// compared against exactly that entry, naming the first differing byte; with a
// dump folder the two blobs are written side by side for `dxc -dumpbin` /
// spirv-dis.
//
// Intended use (tools/shaderpack_tool dxc-selftest): 20 sampled files from
// the v8 tree with the pack in native/work/pack, `ok` must be true.
// ===========================================================================

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace larecomp::shaderpack {

struct SelfTestFile {
  std::filesystem::path hlsl;  // one translated shader, as the generator wrote it (the file name is the shader's name)
  std::optional<uint64_t> key;  // the pack key when known; otherwise found through the DXIL blob's hash
};

struct SelfTestOptions {
  std::filesystem::path dxc_dir;  // dxcompiler.dll + dxil.dll of the pinned dxc (1.8.2407.7)
  std::filesystem::path pack;     // the existing mcla_shaders.v8.pack
  std::vector<SelfTestFile> files;
  unsigned threads = 1;           // files in parallel, one DxcWorker each (0 or 1: the calling thread only)
  bool check_contract = true;
  std::filesystem::path dump_dir;  // optional: the new and the old bytes of every blob that differs
  std::function<void(std::string_view)> log;  // optional; called from the calling thread, in file order
};

struct SelfTestResult {
  bool ok = false;     // every file found in the pack, every blob equal, no contract problem
  std::string error;   // set when the test could not run at all (dxc or the pack did not load)
  uint32_t files = 0;
  uint32_t files_ok = 0;
  uint32_t not_found = 0;       // no pack entry for the file
  uint32_t blobs_compared = 0;
  uint32_t blobs_equal = 0;
  uint32_t contract_problems = 0;
  std::vector<std::string> failures;  // the report of every failing file
};

SelfTestResult RunDxcSelfTest(const SelfTestOptions& options);

}  // namespace larecomp::shaderpack
