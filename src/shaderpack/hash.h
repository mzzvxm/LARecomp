#pragma once
// XXH3-64 for the stamp and the pack's fingerprint (the same hash the renderer logs for the pack:
// "[gpu] shader pack: ... xxh3 <16 hex digits>"). The xxhash header is only included by hash.cpp.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace larecomp::shaderpack {

uint64_t Xxh3(const void* data, size_t size);
inline uint64_t Xxh3(std::string_view text) { return Xxh3(text.data(), text.size()); }

// The same value as Xxh3 over the concatenation of everything given to Update.
class Xxh3Stream {
 public:
  Xxh3Stream();
  ~Xxh3Stream();
  Xxh3Stream(const Xxh3Stream&) = delete;
  Xxh3Stream& operator=(const Xxh3Stream&) = delete;

  void Update(const void* data, size_t size);
  uint64_t Digest() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// 16 lower case hex digits.
std::string Hex64(uint64_t value);
// False unless `text` is exactly 16 hex digits.
bool ParseHex64(std::string_view text, uint64_t* value);

}  // namespace larecomp::shaderpack
