// TEST ONLY: byte comparison of the in-process dxc against the existing v8 pack (see dxc_selftest.h).

#include "dxc_selftest.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>

#include "contract_checks.h"
#include "dxc.h"

namespace larecomp::shaderpack {

namespace {

// CHECK: the v8 layout is copied here from build_shader_pack_v8.py so this file
// stands alone. Use shaderpack/pack_format.h (W2) once it is merged.
constexpr uint32_t kPackVersion = 8;
constexpr size_t kHeaderSize = 32;
constexpr size_t kEntrySize = 88;
constexpr size_t kBlobsPerEntry = 4;  // DXIL 0, DXIL 1, SPIR-V 0, SPIR-V 1
constexpr uint32_t kFlagPsOutputsShift = 8;

uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t Le64(const uint8_t* p) { return uint64_t(Le32(p)) | (uint64_t(Le32(p + 4)) << 32); }

// FNV-1a 64, the pack's per-blob hash (v2.fnv1a64 / native_gfx::Fnv1a64).
uint64_t Fnv1a64(std::span<const uint8_t> bytes) {
  uint64_t hash = 0xCBF29CE484222325ull;
  for (const uint8_t byte : bytes) {
    hash ^= byte;
    hash *= 0x100000001B3ull;
  }
  return hash;
}

std::string PathText(const std::filesystem::path& path) {
  const auto u8 = path.u8string();
  return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

struct BlobRef {
  uint32_t offset = 0;
  uint32_t size = 0;
  uint64_t fnv = 0;
};

struct PackEntry {
  uint64_t key = 0;
  uint8_t stage = 0;  // 'v' or 'p'
  uint8_t spec_mask = 0;
  uint32_t flags = 0;
  BlobRef blobs[kBlobsPerEntry];
};

class PackFile {
 public:
  bool Open(const std::filesystem::path& path, std::string* error) {
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
      *error = std::format("cannot stat the pack {}: {}", PathText(path), ec.message());
      return false;
    }
    file_size_ = size;
    file_.open(path, std::ios::binary);
    if (!file_) {
      *error = std::format("cannot open the pack {}", PathText(path));
      return false;
    }
    uint8_t header[kHeaderSize];
    file_.read(reinterpret_cast<char*>(header), kHeaderSize);
    if (!file_ || std::memcmp(header, "MSPK", 4) != 0 || Le32(header + 4) != kPackVersion) {
      *error = std::format("{} is not a v{} shader pack", PathText(path), kPackVersion);
      return false;
    }
    const uint32_t count = Le32(header + 8);
    const uint64_t table_bytes = uint64_t(count) * kEntrySize;
    if (kHeaderSize + table_bytes > file_size_) {
      *error = "the pack's entry table runs past the end of the file";
      return false;
    }
    std::vector<uint8_t> table(size_t(table_bytes));
    file_.read(reinterpret_cast<char*>(table.data()), std::streamsize(table.size()));
    if (!file_) {
      *error = "cannot read the pack's entry table";
      return false;
    }
    entries_.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* p = table.data() + size_t(i) * kEntrySize;
      PackEntry& entry = entries_[i];
      entry.key = Le64(p);
      entry.stage = p[8];
      entry.spec_mask = p[9];
      entry.flags = Le32(p + 16);
      for (size_t b = 0; b < kBlobsPerEntry; ++b) {
        entry.blobs[b] = {Le32(p + 24 + 16 * b), Le32(p + 28 + 16 * b), Le64(p + 32 + 16 * b)};
        if (uint64_t(entry.blobs[b].offset) + entry.blobs[b].size > file_size_) {
          *error = std::format("entry {}: a blob runs past the end of the pack", i);
          return false;
        }
      }
    }
    return true;
  }

  const std::vector<PackEntry>& entries() const { return entries_; }

  bool ReadBlob(const BlobRef& ref, std::vector<uint8_t>* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    out->resize(ref.size);
    file_.clear();
    file_.seekg(std::streamoff(ref.offset));
    file_.read(reinterpret_cast<char*>(out->data()), std::streamsize(ref.size));
    return bool(file_);
  }

 private:
  std::ifstream file_;
  uintmax_t file_size_ = 0;
  std::vector<PackEntry> entries_;
  std::mutex mutex_;
};

struct Indexes {
  std::unordered_map<uint64_t, uint32_t> by_key;
  std::unordered_map<uint64_t, uint32_t> by_dxil0;   // blob hash -> entry
  std::unordered_map<uint64_t, uint32_t> by_spirv0;
};

struct FileReport {
  bool ok = false;
  bool found = false;
  uint32_t compared = 0;
  uint32_t equal = 0;
  uint32_t problems = 0;
  std::string line;
};

bool ReadWholeFile(const std::filesystem::path& path, std::string* out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  file.seekg(0, std::ios::end);
  const std::streamoff size = file.tellg();
  if (size < 0) return false;
  file.seekg(0, std::ios::beg);
  out->resize(size_t(size));
  file.read(out->data(), std::streamsize(size));
  return bool(file);
}

void WriteBytes(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (file) file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

std::string DescribeDifference(std::span<const uint8_t> mine, std::span<const uint8_t> theirs) {
  const size_t common = std::min(mine.size(), theirs.size());
  size_t at = 0;
  while (at < common && mine[at] == theirs[at]) ++at;
  if (at == common) {
    return std::format("size {} vs {} in the pack (one is a prefix of the other)", mine.size(),
                       theirs.size());
  }
  return std::format("size {} vs {} in the pack, first difference at byte {} ({:02x} vs {:02x})",
                     mine.size(), theirs.size(), at, mine[at], theirs[at]);
}

FileReport RunOne(const SelfTestFile& file, DxcWorker& worker, PackFile& pack, const Indexes& index,
                  const SelfTestOptions& options) {
  FileReport report;
  const std::string name = PathText(file.hlsl.stem());
  const auto fail = [&](const std::string& why) {
    report.ok = false;
    report.line = std::format("FAIL {}: {}", name, why);
    return report;
  };

  std::string hlsl;
  if (!ReadWholeFile(file.hlsl, &hlsl)) return fail("cannot read the HLSL file");

  // Only vertex shaders have an oPos output (pixel shaders read SV_Position as iPos).
  ShaderStage stage = hlsl.find("oPos : SV_Position") != std::string::npos ? ShaderStage::kVertex
                                                                              : ShaderStage::kPixel;
  const PackEntry* entry = nullptr;
  if (file.key) {
    const auto it = index.by_key.find(*file.key);
    if (it == index.by_key.end()) return fail(std::format("key {:016X} is not in the pack", *file.key));
    entry = &pack.entries()[it->second];
    stage = entry->stage == 'v' ? ShaderStage::kVertex : ShaderStage::kPixel;
  }

  // The blobs of this file, compiled the first time they are asked for.
  // [format: 0 DXIL, 1 SPIR-V][variant]
  std::vector<uint8_t> blobs[2][2];
  bool compiled[2][2] = {};
  uint32_t spec_mask = 0;
  const auto get = [&](int format, int variant, std::string* error) -> const std::vector<uint8_t>* {
    if (!compiled[format][variant]) {
      CompileRequest request;
      request.hlsl = hlsl;
      request.stage = stage;
      request.spec_constants = variant ? spec_mask : 0;
      request.name = name;
      const DxcStatus status = format ? worker.CompileSpirv(request, &blobs[format][variant])
                                      : worker.CompileDxil(request, &blobs[format][variant]);
      if (!status.ok()) {
        *error = status.message;
        return nullptr;
      }
      compiled[format][variant] = true;
    }
    return &blobs[format][variant];
  };

  std::string error;
  if (!entry) {
    // Identify the entry by the bytes: DXIL 0, then SPIR-V 0.
    if (const std::vector<uint8_t>* dxil0 = get(0, 0, &error)) {
      const auto it = index.by_dxil0.find(Fnv1a64(*dxil0));
      if (it != index.by_dxil0.end()) entry = &pack.entries()[it->second];
    } else {
      return fail(error);
    }
    if (!entry) {
      if (const std::vector<uint8_t>* spirv0 = get(1, 0, &error)) {
        const auto it = index.by_spirv0.find(Fnv1a64(*spirv0));
        if (it != index.by_spirv0.end()) entry = &pack.entries()[it->second];
      }
    }
    if (!entry) {
      return fail("no pack entry has these DXIL or SPIR-V bytes for variant 0: the blobs differ from "
                  "the pack's, or the shader is not in it (pass its key to compare against one entry)");
    }
    const ShaderStage entry_stage = entry->stage == 'v' ? ShaderStage::kVertex : ShaderStage::kPixel;
    if (entry_stage != stage) return fail("the stage guessed from the text disagrees with the pack entry");
  }
  report.found = true;

  const std::string key_text = std::format("{:016X}", entry->key);
  spec_mask = entry->spec_mask;
  const int variants = spec_mask != 0 ? 2 : 1;
  static constexpr const char* kFormatNames[2] = {"dxil", "spirv"};
  std::string details;

  for (int variant = 0; variant < variants; ++variant) {
    for (int format = 0; format < 2; ++format) {
      const BlobRef& ref = entry->blobs[format * 2 + variant];
      const std::string what = std::format("{} variant {}", kFormatNames[format], variant);
      if (ref.size == 0) {
        details += std::format("\n  {}: the pack has no such blob", what);
        continue;
      }
      const std::vector<uint8_t>* mine = get(format, variant, &error);
      if (!mine) {
        details += std::format("\n  {}: {}", what, error);
        continue;
      }
      std::vector<uint8_t> theirs;
      if (!pack.ReadBlob(ref, &theirs)) {
        details += std::format("\n  {}: cannot read the pack's blob", what);
        continue;
      }
      ++report.compared;
      if (*mine == theirs) {
        ++report.equal;
        continue;
      }
      details += std::format("\n  {}: {}", what, DescribeDifference(*mine, theirs));
      if (!options.dump_dir.empty()) {
        const std::string base = std::format("{}.v{}.{}", name, variant, format ? "spv" : "dxil");
        WriteBytes(options.dump_dir / (base + ".new"), *mine);
        WriteBytes(options.dump_dir / (base + ".old"), theirs);
      }
    }
  }

  if (options.check_contract) {
    Problems problems;
    const uint32_t outputs = (entry->flags >> kFlagPsOutputsShift) & 0x1F;
    HlslCheckInput text_input;
    text_input.name = name;
    text_input.hlsl = hlsl;
    text_input.stage = stage;
    text_input.spec_mask = spec_mask;
    text_input.ps_outputs = outputs;
    CheckHlslText(text_input, &problems);
    for (int variant = 0; variant < variants; ++variant) {
      std::string ignored;
      const std::vector<uint8_t>* dxil = get(0, variant, &ignored);
      const std::vector<uint8_t>* spirv = get(1, variant, &ignored);
      if (!dxil || !spirv) continue;  // the compile failure is already in `details`
      BlobContractInput blob_input;
      blob_input.name = name;
      blob_input.stage = stage;
      blob_input.variant = variant;
      blob_input.writes_oc0 = stage == ShaderStage::kPixel && (outputs & 1u) != 0;
      blob_input.dxil = *dxil;
      blob_input.spirv = *spirv;
      CheckBlobContract(blob_input, &problems);
      if (stage == ShaderStage::kVertex) {
        CheckSpirvPosition(name, variant, *spirv, &problems);
        CheckDxilPosition(worker, name, variant, *dxil, &problems);
      }
    }
    report.problems = uint32_t(problems.size());
    for (const std::string& problem : problems) details += "\n  contract: " + problem;
  }

  report.ok = details.empty();
  report.line = report.ok ? std::format("ok   {} key {} {} spec {:#x}: {} blobs identical", name, key_text,
                                        stage == ShaderStage::kVertex ? "vs" : "ps", spec_mask,
                                        report.compared)
                          : std::format("FAIL {} key {} {} spec {:#x}:{}", name, key_text,
                                        stage == ShaderStage::kVertex ? "vs" : "ps", spec_mask, details);
  return report;
}

}  // namespace

SelfTestResult RunDxcSelfTest(const SelfTestOptions& options) {
  SelfTestResult result;
  const auto say = [&](const std::string& line) {
    if (options.log) options.log(line);
  };

  DxcStatus status;
  const std::shared_ptr<DxcRuntime> runtime = DxcRuntime::Load(options.dxc_dir, {}, &status);
  if (!runtime) {
    result.error = status.message;
    return result;
  }
  say(std::format("dxc: {} version {}", PathText(runtime->compiler_path()),
                  runtime->version().empty() ? std::string("unknown") : runtime->version()));
  if (!runtime->notes().empty()) say("dxc note: " + runtime->notes());

  PackFile pack;
  if (!pack.Open(options.pack, &result.error)) return result;
  Indexes index;
  for (uint32_t i = 0; i < pack.entries().size(); ++i) {
    const PackEntry& entry = pack.entries()[i];
    index.by_key.emplace(entry.key, i);
    if (entry.blobs[0].size) index.by_dxil0.emplace(entry.blobs[0].fnv, i);
    if (entry.blobs[2].size) index.by_spirv0.emplace(entry.blobs[2].fnv, i);
  }
  say(std::format("pack: {} entries", pack.entries().size()));
  if (!options.dump_dir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(options.dump_dir, ec);
  }

  const size_t count = options.files.size();
  const size_t thread_count =
      std::max<size_t>(1, std::min<size_t>(options.threads == 0 ? 1 : options.threads, std::max<size_t>(count, 1)));
  std::vector<std::unique_ptr<DxcWorker>> workers;
  for (size_t t = 0; t < thread_count; ++t) {
    std::unique_ptr<DxcWorker> worker = runtime->CreateWorker(&status);
    if (!worker) {
      result.error = status.message;
      return result;
    }
    workers.push_back(std::move(worker));
  }

  std::vector<FileReport> reports(count);
  std::atomic<size_t> next{0};
  const auto work = [&](DxcWorker& worker) {
    for (;;) {
      const size_t i = next.fetch_add(1);
      if (i >= count) break;
      reports[i] = RunOne(options.files[i], worker, pack, index, options);
    }
  };
  if (thread_count == 1) {
    work(*workers[0]);
  } else {
    std::vector<std::thread> pool;
    for (size_t t = 1; t < thread_count; ++t) pool.emplace_back(work, std::ref(*workers[t]));
    work(*workers[0]);
    for (std::thread& thread : pool) thread.join();
  }

  for (const FileReport& report : reports) {
    ++result.files;
    if (report.ok) ++result.files_ok;
    if (!report.found) ++result.not_found;
    result.blobs_compared += report.compared;
    result.blobs_equal += report.equal;
    result.contract_problems += report.problems;
    say(report.line);
    if (!report.ok) result.failures.push_back(report.line);
  }
  result.ok = result.files > 0 && result.files_ok == result.files;
  say(std::format("{} files, {} ok, {} not found in the pack, {} of {} blobs identical, {} contract problems",
                  result.files, result.files_ok, result.not_found, result.blobs_equal,
                  result.blobs_compared, result.contract_problems));
  return result;
}

}  // namespace larecomp::shaderpack
