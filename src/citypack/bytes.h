#pragma once
// ===========================================================================
// LARecomp city pack -- byte helpers (little endian files, big endian game memory)
// ===========================================================================
// The pack is written and read field by field through these, never by copying a struct: the layout is the
// format's, not the compiler's. A reader never reads past its span; a failed read sets `ok()` to false and
// every later read returns 0, so a parser checks once at the end.
// ===========================================================================

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace larecomp::citypack {

class ByteWriter {
 public:
  void U8(uint8_t v) { bytes_.push_back(v); }
  void U16(uint16_t v) { Put(v, 2); }
  void U32(uint32_t v) { Put(v, 4); }
  void U64(uint64_t v) { Put(v, 8); }
  void I32(int32_t v) { U32(static_cast<uint32_t>(v)); }
  void F32(float v) { U32(std::bit_cast<uint32_t>(v)); }
  void Bytes(const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    bytes_.insert(bytes_.end(), p, p + size);
  }
  void Zeros(size_t count) { bytes_.insert(bytes_.end(), count, 0); }
  // LEB128: seven bits a byte, the low group first.
  void Varint(uint32_t v) {
    while (v >= 0x80u) {
      bytes_.push_back(static_cast<uint8_t>(v | 0x80u));
      v >>= 7;
    }
    bytes_.push_back(static_cast<uint8_t>(v));
  }
  // Pads with zeros to a multiple of `alignment` (a power of two) from the start of this writer.
  void Align(size_t alignment) {
    const size_t rest = bytes_.size() & (alignment - 1);
    if (rest != 0) Zeros(alignment - rest);
  }
  // Overwrites four bytes already written (a size known after the data).
  void PatchU32(size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) bytes_[at + size_t(i)] = static_cast<uint8_t>(v >> (8 * i));
  }
  size_t size() const { return bytes_.size(); }
  const std::vector<uint8_t>& bytes() const { return bytes_; }
  std::vector<uint8_t>& bytes() { return bytes_; }

 private:
  void Put(uint64_t v, int count) {
    for (int i = 0; i < count; ++i) bytes_.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
  std::vector<uint8_t> bytes_;
};

class ByteReader {
 public:
  ByteReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
  uint8_t U8() { return static_cast<uint8_t>(Get(1)); }
  uint16_t U16() { return static_cast<uint16_t>(Get(2)); }
  uint32_t U32() { return static_cast<uint32_t>(Get(4)); }
  uint64_t U64() { return Get(8); }
  int32_t I32() { return static_cast<int32_t>(U32()); }
  float F32() { return std::bit_cast<float>(U32()); }
  uint32_t Varint() {
    uint32_t v = 0;
    for (int shift = 0; shift < 35; shift += 7) {
      const uint8_t b = U8();
      v |= uint32_t(b & 0x7Fu) << shift;
      if ((b & 0x80u) == 0 || !ok_) return v;
    }
    ok_ = false;  // more than five bytes: not a 32 bit value
    return 0;
  }
  void Skip(size_t count) {
    if (count > size_ - pos_) {
      ok_ = false;
      pos_ = size_;
      return;
    }
    pos_ += count;
  }
  bool Seek(size_t at) {
    if (at > size_) {
      ok_ = false;
      return false;
    }
    pos_ = at;
    return true;
  }
  bool Read(void* out, size_t count) {
    if (count > size_ - pos_) {
      ok_ = false;
      pos_ = size_;
      return false;
    }
    std::memcpy(out, data_ + pos_, count);
    pos_ += count;
    return true;
  }
  size_t pos() const { return pos_; }
  size_t size() const { return size_; }
  size_t left() const { return size_ - pos_; }
  bool ok() const { return ok_; }
  const uint8_t* data() const { return data_; }

 private:
  uint64_t Get(int count) {
    if (size_t(count) > size_ - pos_) {
      ok_ = false;
      pos_ = size_;
      return 0;
    }
    uint64_t v = 0;
    for (int i = 0; i < count; ++i) v |= uint64_t(data_[pos_ + size_t(i)]) << (8 * i);
    pos_ += size_t(count);
    return v;
  }
  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
  bool ok_ = true;
};

// FNV-1a, 64 bit: the pack's integrity hash (not a security measure; xxh3 is the shader pack's, this library stands alone).
inline uint64_t Fnv1a64(const void* data, size_t size, uint64_t seed = 0xCBF29CE484222325ull) {
  const auto* p = static_cast<const uint8_t*>(data);
  uint64_t h = seed;
  for (size_t i = 0; i < size; ++i) {
    h ^= p[i];
    h *= 0x100000001B3ull;
  }
  return h;
}

// Big endian loads from the game's memory (the Xbox 360 is big endian). The caller has checked the range.
inline uint16_t LoadBE16(const uint8_t* p) { return static_cast<uint16_t>((uint16_t(p[0]) << 8) | p[1]); }
inline uint32_t LoadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline float LoadBEFloat(const uint8_t* p) { return std::bit_cast<float>(LoadBE32(p)); }

}  // namespace larecomp::citypack
