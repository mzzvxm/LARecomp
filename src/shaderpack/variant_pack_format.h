#pragma once
// ===========================================================================
// LARecomp shader pack builder -- the variant pack's file layout, written and read
// ===========================================================================
// The layout itself is documented in pack_format.h (mcla_shaders_variants.v3.pack, MSHV version 3). This header is its two
// directions as pure functions on bytes: the builder (variant_pack.cpp) lays the file out with LayOutVariantPack, the renderer
// (src/gpu/shaders.cpp ShaderStore::LoadVariants) reads it with ParseVariantPack, and the unit test round-trips them and feeds
// the reader the files of other versions. Header only: no I/O, no compiler, no renderer.
//
// A file of another version is refused as a whole, with a sentence that says which version it was and which this build reads
// ("built by another version of the variant pack: ... the shader pack build writes the current one"): nothing of it is used.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "shaderpack/pack_format.h"
#include "shaderpack/variant_chain.h"

namespace larecomp::shaderpack {

struct VariantBlobRecord {
  uint32_t offset = 0;
  uint32_t size = 0;
  uint64_t fnv = 0;  // FNV-1a 64 of the blob
};

// One entry as the file holds it (what the reader hands over).
struct VariantPackEntry {
  uint64_t key = 0;  // the pixel shader's identity, or the vertex shader's (kVariantEntryVertex)
  std::array<uint64_t, kVariantVertexShaders> vs_keys{};
  uint32_t variant = 0;      // the word
  uint32_t flags = 0;
  uint32_t vs_features = 0;  // the vertex features the draw's vertex shader must have
  uint32_t reserved = 0;
  VariantBlobRecord dxil;
  VariantBlobRecord spirv;

  bool vertex() const { return (flags & kVariantEntryVertex) != 0; }
};

// One entry to write: the blobs' bytes (empty: that format was not built).
struct VariantPackNewEntry {
  uint64_t key = 0;
  std::array<uint64_t, kVariantVertexShaders> vs_keys{};
  uint32_t variant = 0;
  uint32_t flags = 0;
  uint32_t vs_features = 0;
  std::vector<uint8_t> dxil;
  std::vector<uint8_t> spirv;

  bool vertex() const { return (flags & kVariantEntryVertex) != 0; }
};

namespace variant_pack_detail {
inline void Put32(std::vector<uint8_t>& out, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out.push_back(uint8_t(value >> shift));
}
inline void Put64(std::vector<uint8_t>& out, uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) out.push_back(uint8_t(value >> shift));
}
inline uint32_t Get32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline uint64_t Get64(const uint8_t* p) { return uint64_t(Get32(p)) | (uint64_t(Get32(p + 4)) << 32); }
// The order of the entries in the file: pixel shaders first, then by word, then by key.
inline std::pair<uint32_t, std::pair<uint32_t, uint64_t>> SortKey(bool vertex, uint32_t variant, uint64_t key) {
  return {vertex ? 1u : 0u, {variant, key}};
}
}  // namespace variant_pack_detail

// header | entries | blobs. `entries` must be in the file's order (SortKey, strictly increasing) and valid words; `fnv` is the
// blob checksum (FNV-1a 64, mcla::native_gfx::Fnv1a64, which the reader checks blobs with). Blobs that are byte for byte
// the same are written once (the shadow collector's shaders share one text per body shape). False with *error when a size does
// not fit or the entries are out of order.
inline bool LayOutVariantPack(std::span<const VariantPackNewEntry> entries, uint32_t formats, uint32_t main_pack_version,
                              uint64_t (*fnv)(const uint8_t*, size_t), std::vector<uint8_t>* out, std::string* error) {
  using namespace variant_pack_detail;
  out->clear();
  out->insert(out->end(), kVariantPackMagic, kVariantPackMagic + 4);
  Put32(*out, kVariantPackVersion);
  Put32(*out, uint32_t(entries.size()));
  Put32(*out, formats);
  Put32(*out, kVariantContract);
  Put32(*out, main_pack_version);  // the main pack the variants were built beside
  Put32(*out, 0);
  Put32(*out, 0);

  const uint64_t blob_base = uint64_t(kVariantPackHeaderBytes) + uint64_t(entries.size()) * kVariantPackEntryBytes;
  std::vector<uint8_t> blobs;
  std::map<std::pair<uint64_t, size_t>, std::vector<size_t>> laid_out;  // (fnv, size) -> where in `blobs`
  for (size_t i = 0; i < entries.size(); ++i) {
    const VariantPackNewEntry& entry = entries[i];
    if (i > 0) {
      const VariantPackNewEntry& before = entries[i - 1];
      if (!(SortKey(before.vertex(), before.variant, before.key) < SortKey(entry.vertex(), entry.variant, entry.key))) {
        *error = "internal error: the variant pack's entries are not in increasing order";
        return false;
      }
    }
    const size_t entry_start = out->size();
    Put64(*out, entry.key);
    for (const uint64_t vs_key : entry.vs_keys) Put64(*out, vs_key);
    Put32(*out, entry.variant);
    Put32(*out, entry.flags);
    Put32(*out, entry.vs_features);
    Put32(*out, 0);  // reserved
    for (const std::vector<uint8_t>* blob : {&entry.dxil, &entry.spirv}) {
      if (blob->empty()) {
        Put32(*out, 0);
        Put32(*out, 0);
        Put64(*out, 0);
        continue;
      }
      const uint64_t checksum = fnv(blob->data(), blob->size());
      uint64_t offset = 0;
      bool shared = false;
      std::vector<size_t>& same = laid_out[{checksum, blob->size()}];
      for (const size_t placed : same) {
        if (std::equal(blob->begin(), blob->end(), blobs.begin() + std::ptrdiff_t(placed))) {
          offset = blob_base + placed;
          shared = true;
          break;
        }
      }
      if (!shared) {
        offset = blob_base + blobs.size();
        if (offset + blob->size() > kMaxPackBytes) {
          *error = "the variant pack would pass 4 GiB (blob offsets are 32 bit)";
          return false;
        }
        same.push_back(blobs.size());
        blobs.insert(blobs.end(), blob->begin(), blob->end());
      }
      Put32(*out, uint32_t(offset));
      Put32(*out, uint32_t(blob->size()));
      Put64(*out, checksum);
    }
    if (out->size() - entry_start != kVariantPackEntryBytes) {
      *error = "internal error: a variant pack entry is not the layout's size";
      return false;
    }
  }
  out->insert(out->end(), blobs.begin(), blobs.end());
  return true;
}

struct ParsedVariantPack {
  uint32_t formats = 0;
  uint32_t main_pack_version = 0;
  std::vector<VariantPackEntry> entries;
};

struct VariantPackExpect {
  uint32_t main_pack_version = kPackVersion;  // the main pack this build reads
  uint32_t formats_needed = 0;                // kDxil or kSpirv: the blobs this backend uses must be there
  std::span<const chain::FeatureInfo> table = chain::kFeatures;
};

// Reads and checks the whole file: the header (magic, version, contract, the main pack it was built beside, the formats), every
// entry (a word the table knows, the flags, the vertex needs the features say, order, at most kMaxWordsPerShader words a shader) and
// every blob range. False with *why in plain words; nothing is kept then.
inline bool ParseVariantPack(std::span<const uint8_t> bytes, const VariantPackExpect& expect, ParsedVariantPack* out,
                             std::string* why) {
  using namespace variant_pack_detail;
  const auto fail = [&](std::string text) {
    *out = ParsedVariantPack{};
    *why = std::move(text);
    return false;
  };
  if (bytes.size() < kVariantPackHeaderBytes || std::memcmp(bytes.data(), kVariantPackMagic, 4) != 0) {
    return fail("not a shader variant pack");
  }
  const uint32_t version = Get32(bytes.data() + 4);
  const uint32_t count = Get32(bytes.data() + 8);
  const uint32_t formats = Get32(bytes.data() + 12);
  const uint32_t contract = Get32(bytes.data() + 16);
  const uint32_t main_version = Get32(bytes.data() + 20);
  if (version != kVariantPackVersion) {
    return fail("built by another version of the variant pack (file version " + std::to_string(version) +
                ", this build reads version " + std::to_string(kVariantPackVersion) +
                "): none of it is used; the shader pack build writes the current one");
  }
  if (contract != kVariantContract || main_version != expect.main_pack_version) {
    return fail("version " + std::to_string(version) + ", constants contract " + std::to_string(contract) +
                ", built for shader pack " + std::to_string(main_version) + "; this build reads version " +
                std::to_string(kVariantPackVersion) + ", contract " + std::to_string(kVariantContract) + ", pack " +
                std::to_string(expect.main_pack_version));
  }
  if ((formats & expect.formats_needed) != expect.formats_needed) {
    return fail("it does not carry a format this backend reads (formats " + std::to_string(formats) + ")");
  }
  if (uint64_t(kVariantPackHeaderBytes) + uint64_t(count) * kVariantPackEntryBytes > bytes.size()) {
    return fail("entry table past the end of the file");
  }
  const auto in_file = [&](const VariantBlobRecord& blob) { return uint64_t(blob.offset) + blob.size <= bytes.size(); };
  ParsedVariantPack parsed;
  parsed.formats = formats;
  parsed.main_pack_version = main_version;
  parsed.entries.reserve(count);
  std::map<std::pair<bool, uint64_t>, uint32_t> words_of_shader;  // (vertex?, key) -> its entries
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* e = bytes.data() + kVariantPackHeaderBytes + uint64_t(i) * kVariantPackEntryBytes;
    VariantPackEntry entry;
    entry.key = Get64(e);
    for (uint32_t k = 0; k < kVariantVertexShaders; ++k) entry.vs_keys[k] = Get64(e + 8 + 8 * k);
    entry.variant = Get32(e + 40);
    entry.flags = Get32(e + 44);
    entry.vs_features = Get32(e + 48);
    entry.reserved = Get32(e + 52);
    entry.dxil = {Get32(e + 56), Get32(e + 60), Get64(e + 64)};
    entry.spirv = {Get32(e + 72), Get32(e + 76), Get64(e + 80)};
    const std::string at = "entry " + std::to_string(i);
    if (entry.variant < kFirstPackedVariant) return fail(at + " has the variant word " + std::to_string(entry.variant) + ", which is the main pack's");
    if ((entry.flags & ~kVariantEntryVertex) != 0 || entry.reserved != 0) return fail(at + " has flags this build does not know");
    const uint32_t features = chain::FeaturesOf(entry.variant);
    for (uint32_t feature = 0; feature < kMaxFeatures; ++feature) {
      if ((features & chain::FeatureBit(feature)) != 0 && !chain::Find(feature, expect.table)) {
        return fail(at + " has the feature " + std::to_string(feature) + ", which this build does not know");
      }
    }
    if (entry.vertex()) {
      if (entry.vs_features != 0 || entry.vs_keys != std::array<uint64_t, kVariantVertexShaders>{}) {
        return fail(at + " is a vertex shader with the fields of a pixel shader");
      }
      if (chain::FeaturesOfStage(features, chain::Stage::kPixel, expect.table) != 0) return fail(at + " is a vertex shader with a pixel feature");
    } else {
      if (chain::FeaturesOfStage(features, chain::Stage::kVertex, expect.table) != 0) return fail(at + " is a pixel shader with a vertex feature");
      if (entry.vs_features != chain::VertexNeedsOf(features, expect.table)) return fail(at + " names vertex features that its features do not need");
    }
    const VariantBlobRecord& needed = (expect.formats_needed & kDxil) != 0 ? entry.dxil : entry.spirv;
    if ((expect.formats_needed & (kDxil | kSpirv)) != 0 && needed.size == 0) return fail(at + " lacks a blob this backend needs");
    if (!in_file(entry.dxil) || !in_file(entry.spirv)) return fail(at + " has a blob past the end");
    if (!parsed.entries.empty()) {
      const VariantPackEntry& before = parsed.entries.back();
      const auto previous = SortKey(before.vertex(), before.variant, before.key);
      const auto current = SortKey(entry.vertex(), entry.variant, entry.key);
      if (!(previous < current)) return fail(at + " is out of order or repeats the variant " + std::to_string(entry.variant) + " of a shader");
    }
    if (++words_of_shader[{entry.vertex(), entry.key}] > chain::kMaxWordsPerShader) {
      return fail(at + ": a shader has more than " + std::to_string(chain::kMaxWordsPerShader) + " variants");
    }
    parsed.entries.push_back(entry);
  }
  *out = std::move(parsed);
  return true;
}

}  // namespace larecomp::shaderpack
