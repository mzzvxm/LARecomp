// LARecomp shader pack builder -- the v8 pack writer, reader and validator (see pack_writer.h).
// The layout is build_shader_pack_v8.py's (mzzvxm), constants from pack_format.h.

#include "shaderpack/pack_writer.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <system_error>

#include "native_gfx/shader_identity.h"
#include "shaderpack/hash.h"

namespace larecomp::shaderpack {

namespace {

std::string PathText(const std::filesystem::path& path) {
  const auto u8 = path.u8string();  // never throws, unlike path::string() on a non-ANSI path
  return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

void Put32(std::vector<uint8_t>& out, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out.push_back(uint8_t(value >> shift));
}

void Put64(std::vector<uint8_t>& out, uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) out.push_back(uint8_t(value >> shift));
}

void Put16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(uint8_t(value));
  out.push_back(uint8_t(value >> 8));
}

uint32_t Get32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint64_t Get64(const uint8_t* p) {
  return uint64_t(Get32(p)) | (uint64_t(Get32(p + 4)) << 32);
}

uint16_t Get16(const uint8_t* p) {
  return uint16_t(uint32_t(p[0]) | (uint32_t(p[1]) << 8));
}

std::string KeyText(uint64_t key) {
  return std::format("{:016X}", key);
}

// Whether the entry's blobs agree with the formats and spec mask (the writer's contract, which is
// v8's "variant 1 blobs are present exactly when spec_mask != 0").
bool CheckEntryInput(const PackEntryInput& entry, uint32_t formats, std::string* error) {
  if (entry.spec_mask > 0xFF) {
    *error = std::format("entry {}: spec mask {:#x} does not fit the pack's byte", KeyText(entry.key),
                         entry.spec_mask);
    return false;
  }
  if (entry.vertex_elements.size() > 0xFFFF) {
    *error = std::format("entry {}: {} vertex elements do not fit the pack's 16 bits", KeyText(entry.key),
                         entry.vertex_elements.size());
    return false;
  }
  for (const bool spirv : {false, true}) {
    const bool built = (formats & (spirv ? kSpirv : kDxil)) != 0;
    for (int variant = 0; variant < 2; ++variant) {
      const bool present = !entry.blobs[BlobIndexOf(spirv, variant)].empty();
      const bool wanted = built && (variant == 0 || entry.spec_mask != 0);
      if (present != wanted) {
        *error = std::format("entry {}: {} variant {} is {} (spec mask {:#x}, {} {})", KeyText(entry.key),
                             spirv ? "SPIR-V" : "DXIL", variant, present ? "present" : "missing",
                             entry.spec_mask, spirv ? "SPIR-V" : "DXIL", built ? "built" : "not built");
        return false;
      }
    }
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

PackWriteResult WritePackFile(const std::filesystem::path& path, std::span<const PackEntryInput> entries,
                              uint32_t formats) {
  PackWriteResult result;
  const auto fail = [&](std::string message) {
    result.ok = false;
    result.error = std::move(message);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return result;
  };

  if (entries.size() > 0xFFFFFFFFull) return fail("too many entries");
  uint64_t total_velems = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    if (i != 0 && entries[i].key <= entries[i - 1].key) {
      return fail(std::format("entry {} is not after entry {} (keys must strictly increase)",
                              KeyText(entries[i].key), KeyText(entries[i - 1].key)));
    }
    std::string why;
    if (!CheckEntryInput(entries[i], formats, &why)) return fail(why);
    total_velems += entries[i].vertex_elements.size();
  }

  // Layout: header | entries | vertex element words | blobs.
  const uint64_t velem_base = uint64_t(kPackHeaderBytes) + uint64_t(entries.size()) * kPackEntryBytes;
  const uint64_t blob_base = velem_base + total_velems * 4;

  std::vector<uint8_t> head;
  head.reserve(size_t(blob_base));
  // Header: magic | version | entry count | formats | 16 reserved bytes.
  head.insert(head.end(), kPackMagic, kPackMagic + 4);
  Put32(head, kPackVersion);
  Put32(head, uint32_t(entries.size()));
  Put32(head, formats);
  for (int i = 0; i < 4; ++i) Put32(head, 0);

  std::vector<uint8_t> velems;
  velems.reserve(size_t(total_velems) * 4);
  uint64_t blob_position = blob_base;
  for (const PackEntryInput& entry : entries) {
    const uint64_t velem_offset = entry.vertex_elements.empty() ? 0 : velem_base + velems.size();
    for (const uint32_t word : entry.vertex_elements) Put32(velems, word);

    const size_t entry_start = head.size();
    Put64(head, entry.key);
    head.push_back(entry.stage == ShaderStage::kVertex ? kStageVertex : kStagePixel);
    head.push_back(uint8_t(entry.spec_mask));
    Put16(head, uint16_t(entry.vertex_elements.size()));
    Put32(head, uint32_t(velem_offset));
    Put32(head, entry.flags);
    Put32(head, entry.texture_fetch_mask);
    for (int index = 0; index < kBlobSlots; ++index) {
      const std::span<const uint8_t> blob = entry.blobs[index];
      if (blob.empty()) {
        Put32(head, 0);
        Put32(head, 0);
        Put64(head, 0);
        continue;
      }
      // Offsets and sizes are u32: the pack stays under 4 GiB.
      if (blob_position + blob.size() > kMaxPackBytes) {
        return fail("the pack would pass 4 GiB (blob offsets are 32 bit)");
      }
      Put32(head, uint32_t(blob_position));
      Put32(head, uint32_t(blob.size()));
      Put64(head, mcla::native_gfx::Fnv1a64(blob.data(), blob.size()));
      blob_position += blob.size();
      (index < 2 ? result.dxil_blobs : result.spirv_blobs)++;
    }
    if (head.size() - entry_start != kPackEntryBytes) {
      return fail("internal error: an entry is not 88 bytes");  // the layout constants disagree
    }
  }
  head.insert(head.end(), velems.begin(), velems.end());
  result.vertex_element_words = total_velems;
  if (head.size() != blob_base) return fail("internal error: the head of the pack has the wrong size");

  std::error_code ec;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) return fail("cannot create " + PathText(path));

  Xxh3Stream hash;
  const auto write = [&](const uint8_t* data, size_t size) {
    file.write(reinterpret_cast<const char*>(data), std::streamsize(size));
    hash.Update(data, size);
  };
  write(head.data(), head.size());
  for (const PackEntryInput& entry : entries) {
    for (int index = 0; index < kBlobSlots; ++index) {
      const std::span<const uint8_t> blob = entry.blobs[index];
      if (!blob.empty()) write(blob.data(), blob.size());
    }
  }
  file.flush();
  file.close();
  if (file.fail()) return fail("cannot write " + PathText(path) + " (disk full or not writable?)");

  result.ok = true;
  result.bytes = blob_position;
  result.xxh3 = hash.Digest();
  return result;
}

bool CommitFile(const std::filesystem::path& temporary, const std::filesystem::path& final_path,
                std::string* error) {
  std::error_code ec;
  std::filesystem::rename(temporary, final_path, ec);
  if (!ec) return true;
  // A rename that will not replace (another platform's rule, or a reader that has the old file open
  // without sharing delete): take the old file away first.
  std::error_code remove_ec;
  std::filesystem::remove(final_path, remove_ec);
  std::filesystem::rename(temporary, final_path, ec);
  if (!ec) return true;
  *error = std::format("cannot move {} to {}: {}", PathText(temporary), PathText(final_path), ec.message());
  return false;
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

bool PackFile::Open(const std::filesystem::path& path, std::string* error) {
  data_.clear();
  entries_.clear();
  version_ = 0;
  formats_ = 0;

  std::error_code ec;
  const uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) {
    *error = std::format("cannot stat {}: {}", PathText(path), ec.message());
    return false;
  }
  if (size < kPackHeaderBytes || size > 0xFFFFFFFFull * 2) {
    *error = std::format("{} is not a shader pack ({} bytes)", PathText(path), size);
    return false;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    *error = "cannot open " + PathText(path);
    return false;
  }
  data_.resize(size_t(size));
  file.read(reinterpret_cast<char*>(data_.data()), std::streamsize(data_.size()));
  if (size_t(file.gcount()) != data_.size()) {
    *error = "cannot read " + PathText(path);
    data_.clear();
    return false;
  }

  if (std::memcmp(data_.data(), kPackMagic, 4) != 0) {
    *error = PathText(path) + " is not a shader pack (wrong magic)";
    data_.clear();
    return false;
  }
  version_ = Get32(data_.data() + kHeaderVersion);
  const uint32_t count = Get32(data_.data() + kHeaderCount);
  formats_ = Get32(data_.data() + kHeaderFormats);
  if (version_ != kPackVersion) {
    *error = std::format("{} is version {}, expected {}", PathText(path), version_, kPackVersion);
    data_.clear();
    return false;
  }
  if (uint64_t(kPackHeaderBytes) + uint64_t(count) * kPackEntryBytes > data_.size()) {
    *error = PathText(path) + ": the entry table runs past the end of the file";
    data_.clear();
    return false;
  }

  entries_.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* p = data_.data() + kPackHeaderBytes + size_t(i) * kPackEntryBytes;
    Entry& entry = entries_[i];
    entry.key = Get64(p + kEntryKey);
    entry.stage = p[kEntryStage];
    entry.spec_mask = p[kEntrySpecMask];
    entry.velem_count = Get16(p + kEntryVertexElementCount);
    entry.velem_offset = Get32(p + kEntryVertexElementOffset);
    entry.flags = Get32(p + kEntryFlags);
    entry.texture_fetch_mask = Get32(p + kEntryTextureFetchMask);
    for (int index = 0; index < kBlobSlots; ++index) {
      const uint8_t* record = p + kEntryDxil0 + size_t(index) * kBlobRecordBytes;
      entry.blobs[index] = Blob{Get32(record), Get32(record + 4), Get64(record + 8)};
    }
  }
  return true;
}

const PackFile::Entry* PackFile::Find(uint64_t key) const {
  const auto it = std::lower_bound(entries_.begin(), entries_.end(), key,
                                   [](const Entry& entry, uint64_t value) { return entry.key < value; });
  return it != entries_.end() && it->key == key ? &*it : nullptr;
}

std::span<const uint8_t> PackFile::BlobBytes(const Blob& blob) const {
  if (blob.size == 0 || uint64_t(blob.offset) + blob.size > data_.size()) return {};
  return std::span<const uint8_t>(data_.data() + blob.offset, blob.size);
}

// ---------------------------------------------------------------------------
// Validation: ShaderStore::Validate (src/gpu/shaders.cpp) and its VerifyBlob, for every blob.
// ---------------------------------------------------------------------------

ValidateResult ValidatePack(const std::filesystem::path& path, const ValidateOptions& options) {
  ValidateResult result;
  const auto problem = [&](std::string text) {
    ++result.problem_count;
    if (result.problems.size() < options.max_problems) result.problems.push_back(std::move(text));
  };

  PackFile pack;
  std::string error;
  if (!pack.Open(path, &error)) {
    problem(error);
    return result;
  }
  result.version = pack.version();
  result.formats = pack.formats();
  result.entries = uint32_t(pack.entries().size());
  result.bytes = pack.bytes().size();
  result.xxh3 = Xxh3(pack.bytes().data(), pack.bytes().size());

  const uint32_t required = options.formats != 0 ? options.formats : pack.formats();
  if ((pack.formats() & required) != required) {
    problem(std::format("formats {:#x}: the pack lacks {}", pack.formats(), required & ~pack.formats()));
  }

  const uint64_t file_size = pack.bytes().size();
  uint64_t previous_key = 0;
  for (size_t i = 0; i < pack.entries().size(); ++i) {
    const PackFile::Entry& entry = pack.entries()[i];
    if (entry.stage != kStageVertex && entry.stage != kStagePixel) {
      problem(std::format("entry {}: stage {}", i, entry.stage));
      continue;
    }
    (entry.stage == kStageVertex ? result.vertex_shaders : result.pixel_shaders)++;
    if (i != 0 && entry.key <= previous_key) {
      problem(std::format("entry {}: keys are not strictly increasing", i));
    }
    previous_key = entry.key;
    if (entry.velem_count != 0 && uint64_t(entry.velem_offset) + uint64_t(entry.velem_count) * 4 > file_size) {
      problem(std::format("entry {}: vertex elements past the end", i));
    }

    for (int index = 0; index < kBlobSlots; ++index) {
      const PackFile::Blob& blob = entry.blobs[index];
      const bool spirv = index >= 2;
      const int variant = index & 1;
      const char* what = spirv ? "SPIR-V" : "DXIL";
      if (blob.size == 0) {
        // What this backend needs must be there: variant 0 always, variant 1 whenever the shader
        // has spec bits.
        const bool format_required = (required & (spirv ? kSpirv : kDxil)) != 0;
        if (format_required && (variant == 0 || entry.spec_mask != 0)) {
          problem(std::format("entry {}: lacks {} variant {}", i, what, variant));
        }
        continue;
      }
      if (variant == 1 && entry.spec_mask == 0) {
        problem(std::format("entry {}: has a {} variant 1 blob but no spec bits", i, what));
      }
      if (uint64_t(blob.offset) + blob.size > file_size) {
        problem(std::format("entry {}: the {} variant {} blob is past the end", i, what, variant));
        continue;
      }
      ++result.blobs;
      if (!options.verify_blobs) continue;
      const std::span<const uint8_t> bytes = pack.BlobBytes(blob);
      const bool magic = spirv ? bytes.size() >= kSpirvMinBytes && bytes.size() % 4 == 0 &&
                                     Get32(bytes.data()) == kSpirvMagic
                               : bytes.size() >= 4 && std::memcmp(bytes.data(), kDxbcMagic, 4) == 0;
      const bool fnv = mcla::native_gfx::Fnv1a64(bytes.data(), bytes.size()) == blob.fnv;
      if (!magic) {
        ++result.bad_magic;
        problem(std::format("entry {} (key {}): the {} variant {} blob has the wrong magic", i,
                            KeyText(entry.key), what, variant));
      }
      if (!fnv) {
        ++result.bad_fnv;
        problem(std::format("entry {} (key {}): the {} variant {} blob fails its FNV", i,
                            KeyText(entry.key), what, variant));
      }
    }
  }

  result.ok = result.problem_count == 0;
  return result;
}

}  // namespace larecomp::shaderpack
