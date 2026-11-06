#include "shaderpack/hash.h"

// Header only, as the SDK's rex/hash.h uses it: no xxhash library to link.
// CHECK: xxhash.h comes from the SDK's include folder in larecomp and from LAR-XenosRecomp's
// thirdparty/xxHash in the standalone tool (src/shaderpack/CMakeLists.txt); both are 0.8.x.
#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
#include <xxhash.h>

#include <format>

namespace larecomp::shaderpack {

uint64_t Xxh3(const void* data, size_t size) {
  return XXH3_64bits(data, size);
}

struct Xxh3Stream::Impl {
  XXH3_state_t* state = nullptr;
};

Xxh3Stream::Xxh3Stream() : impl_(std::make_unique<Impl>()) {
  impl_->state = XXH3_createState();
  if (impl_->state) XXH3_64bits_reset(impl_->state);
}

Xxh3Stream::~Xxh3Stream() {
  if (impl_ && impl_->state) XXH3_freeState(impl_->state);
}

void Xxh3Stream::Update(const void* data, size_t size) {
  if (impl_->state && size != 0) XXH3_64bits_update(impl_->state, data, size);
}

uint64_t Xxh3Stream::Digest() const {
  return impl_->state ? XXH3_64bits_digest(impl_->state) : 0;
}

std::string Hex64(uint64_t value) {
  return std::format("{:016x}", value);
}

bool ParseHex64(std::string_view text, uint64_t* value) {
  if (text.size() != 16) return false;
  uint64_t result = 0;
  for (const char c : text) {
    uint64_t digit;
    if (c >= '0' && c <= '9') {
      digit = uint64_t(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = uint64_t(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      digit = uint64_t(c - 'A' + 10);
    } else {
      return false;
    }
    result = (result << 4) | digit;
  }
  *value = result;
  return true;
}

}  // namespace larecomp::shaderpack
