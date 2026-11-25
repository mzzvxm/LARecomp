#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime -- the Direct3D 12 pipeline library's file
// ===========================================================================
// PipelineCache keeps the driver's pipeline library (ID3D12PipelineLibrary::Serialize) in cache/d3d12_pso_v2.cache, and every game that starts with the setting on loads it
// into CreatePipelineLibrary, which wants the exact bytes Serialize made. The file is shared by the games that run at once (a test batch's parallel runs) and is replaced in
// one step when a game ends (os/file_replace.h), so a reader never sees a half-written file; but a file that IS damaged (a disk that filled up, a bad copy) used to go to
// the driver as it was, and a driver is not obliged to answer a cut-off library with an error.
//
// So a file written by this build starts with a header of its own: 8 bytes of magic, the size of the library and a hash of its bytes (24 bytes in all), then the library.
// Open says what a file is: SEALED (the header is there and the size and the hash match: the library is the part after the header), LEGACY (no header: a file an older
// build wrote, handed to the driver as it always was) or DAMAGED (the header is there and the size or the hash do not match: left out, and the cache is written again at
// the end of the session). The hash is meant to notice damage, not tampering. Pure, so that a test (src/os/file_replace_test.cpp) needs no device.
// ===========================================================================

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mcla::native_gfx::library_file {

inline constexpr char kMagic[8] = {'L', 'A', 'R', 'P', 'S', 'O', 'L', '1'};
inline constexpr size_t kHeaderBytes = 24;

// A hash of `size` bytes, eight at a time (the library is tens of megabytes: a byte at a time would cost a visible part of a second at every start and every save).
inline uint64_t Hash(const uint8_t* data, size_t size) {
  uint64_t h = 0x9E3779B97F4A7C15ull ^ uint64_t(size);
  size_t at = 0;
  for (; at + 8 <= size; at += 8) {
    uint64_t word;
    std::memcpy(&word, data + at, 8);
    h = (h ^ word) * 0x100000001B3ull;
    h ^= h >> 29;
  }
  uint64_t tail = 0;
  if (at < size) std::memcpy(&tail, data + at, size - at);
  h = (h ^ tail ^ uint64_t(size - at)) * 0x100000001B3ull;
  h ^= h >> 32;
  return h;
}

// Writes the header of a library of `blob_size` bytes, which is at `blob` (it is hashed), into the first kHeaderBytes of `header`.
inline void FillHeader(uint8_t* header, const uint8_t* blob, size_t blob_size) {
  const uint64_t size = blob_size;
  const uint64_t hash = Hash(blob, blob_size);
  std::memcpy(header, kMagic, 8);
  std::memcpy(header + 8, &size, 8);
  std::memcpy(header + 16, &hash, 8);
}

enum class Kind {
  kSealed,   // header, size and hash match: the library is `blob`
  kLegacy,   // no header: the whole file is the library, as older builds wrote it
  kDamaged,  // a header that the rest of the file does not match
};

struct Opened {
  Kind kind = Kind::kLegacy;
  const uint8_t* blob = nullptr;  // the library (kSealed: after the header; kLegacy: the file itself)
  size_t blob_size = 0;
};

inline Opened Open(const uint8_t* file, size_t size) {
  Opened out;
  out.blob = file;
  out.blob_size = size;
  if (size < 8 || std::memcmp(file, kMagic, 8) != 0) return out;  // legacy
  out.kind = Kind::kDamaged;
  out.blob = nullptr;
  out.blob_size = 0;
  if (size < kHeaderBytes) return out;
  uint64_t blob_size = 0;
  uint64_t hash = 0;
  std::memcpy(&blob_size, file + 8, 8);
  std::memcpy(&hash, file + 16, 8);
  if (blob_size == 0 || blob_size != uint64_t(size - kHeaderBytes)) return out;
  if (Hash(file + kHeaderBytes, size_t(blob_size)) != hash) return out;
  out.kind = Kind::kSealed;
  out.blob = file + kHeaderBytes;
  out.blob_size = size_t(blob_size);
  return out;
}

}  // namespace mcla::native_gfx::library_file
