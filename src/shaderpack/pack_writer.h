#pragma once
// ===========================================================================
// LARecomp shader pack builder -- the v8 pack writer, reader and validator
// ===========================================================================
// WritePackFile lays out mcla_shaders.v8.pack exactly as build_shader_pack_v8.py does (mzzvxm):
// header | entries sorted by key | the vertex element words | the blobs, per key in key order
// DXIL 0, DXIL 1, SPIR-V 0, SPIR-V 1. The constants are pack_format.h's; the reader is
// src/gpu/shaders.cpp.
//
// Single-format packs (the owner's decision: the first start builds the active backend's format
// only): the header's `formats` says which formats the pack carries, and the blob records of a
// format that was not built are zero (offset 0, size 0, fnv 0). ShaderStore::Validate only asks the
// active backend's blobs to be there.
//
// The file is written under a temporary name; CommitFile moves it into place (replacing what was
// there), so a build that dies never leaves a half written pack under the real name.
//
// PackFile reads a pack back into memory; ValidatePack is the renderer's Validate plus its
// VerifyBlob (magic and FNV of every blob) in one function: it replaces native/tools/check_pack.py.
//
// No logging, no threads.
// ===========================================================================

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "shaderpack/dxc.h"
#include "shaderpack/pack_format.h"

namespace larecomp::shaderpack {

// Index of a blob in an entry: the order of the four blob records and of the blobs in the file.
enum BlobIndex : int { kBlobDxil0 = 0, kBlobDxil1 = 1, kBlobSpirv0 = 2, kBlobSpirv1 = 3, kBlobSlots = 4 };

inline constexpr int BlobIndexOf(bool spirv, int variant) { return (spirv ? 2 : 0) + variant; }

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

struct PackEntryInput {
  uint64_t key = 0;
  ShaderStage stage = ShaderStage::kPixel;
  uint32_t spec_mask = 0;  // at most 255
  uint32_t flags = 0;
  uint32_t texture_fetch_mask = 0;
  std::span<const uint32_t> vertex_elements;  // vertex shaders only; at most 65535
  // By BlobIndex. An empty span means "no such blob": variant 0 is needed for every format in
  // `formats`, variant 1 exactly when spec_mask != 0, and nothing for a format that is not built.
  std::span<const uint8_t> blobs[kBlobSlots];
};

struct PackWriteResult {
  bool ok = false;
  std::string error;  // plain English, names the entry or the path
  uint64_t bytes = 0;
  uint64_t xxh3 = 0;  // XXH3-64 of the whole file (what the renderer logs for the pack)
  uint32_t dxil_blobs = 0;
  uint32_t spirv_blobs = 0;
  uint64_t vertex_element_words = 0;
};

// `entries` must be sorted by strictly increasing key. `formats` is a mask of Format. Writes `path`
// (truncating); on failure the file is removed again.
PackWriteResult WritePackFile(const std::filesystem::path& path, std::span<const PackEntryInput> entries,
                              uint32_t formats);

// Moves `temporary` over `final_path` (which may exist). False with *error on failure.
bool CommitFile(const std::filesystem::path& temporary, const std::filesystem::path& final_path,
                std::string* error);

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

// A pack held in memory. Open checks what is needed to walk the table (magic, version, the table
// inside the file); the rest is ValidatePack's.
class PackFile {
 public:
  struct Blob {
    uint32_t offset = 0;
    uint32_t size = 0;
    uint64_t fnv = 0;
  };
  struct Entry {
    uint64_t key = 0;
    uint8_t stage = 0;  // 'v' or 'p'
    uint8_t spec_mask = 0;
    uint16_t velem_count = 0;
    uint32_t velem_offset = 0;
    uint32_t flags = 0;
    uint32_t texture_fetch_mask = 0;
    Blob blobs[kBlobSlots];
  };

  bool Open(const std::filesystem::path& path, std::string* error);

  uint32_t version() const { return version_; }
  uint32_t formats() const { return formats_; }
  const std::vector<Entry>& entries() const { return entries_; }
  std::span<const uint8_t> bytes() const { return data_; }
  // The entry with this key, by binary search: only meaningful when the keys are sorted, which
  // ValidatePack checks.
  const Entry* Find(uint64_t key) const;
  // The blob's bytes; empty for a zero size or a range outside the file.
  std::span<const uint8_t> BlobBytes(const Blob& blob) const;

 private:
  std::vector<uint8_t> data_;
  uint32_t version_ = 0;
  uint32_t formats_ = 0;
  std::vector<Entry> entries_;
};

struct ValidateOptions {
  // The formats whose blobs must all be there; 0 means every format the header lists.
  uint32_t formats = 0;
  bool verify_blobs = true;  // magic and FNV of every blob
  size_t max_problems = 20;  // lines kept in ValidateResult::problems
};

struct ValidateResult {
  bool ok = false;
  std::vector<std::string> problems;  // the first max_problems of them
  size_t problem_count = 0;
  uint32_t version = 0;
  uint32_t formats = 0;
  uint32_t entries = 0;
  uint32_t vertex_shaders = 0;
  uint32_t pixel_shaders = 0;
  uint32_t blobs = 0;
  uint32_t bad_magic = 0;
  uint32_t bad_fnv = 0;
  uint64_t bytes = 0;
  uint64_t xxh3 = 0;
};

ValidateResult ValidatePack(const std::filesystem::path& path, const ValidateOptions& options = {});

}  // namespace larecomp::shaderpack
