#pragma once
// ===========================================================================
// MCLA GPU renderer -- reading the captured records
// ===========================================================================
// Views over the records of records.h, for every consumer on the render thread
// (the M3 validator, the M4 replay): the register windows by register number
// through the packed block map (guest_device.h), the fetch constants, the
// variable parts of a draw. Raw guest bytes are swapped here and nowhere else.
//
// Threading: plain views; valid while the record is (until it is released).
// ===========================================================================

#include <cstdint>
#include <cstring>

#include "guest_device.h"
#include "records.h"

namespace mcla::gpu {

// A captured device window: raw big-endian bytes that start at device offset
// `base`.
class RegisterWindow {
 public:
  RegisterWindow(const uint8_t* bytes, uint32_t base, uint32_t size)
      : bytes_(bytes), base_(base), size_(size) {}

  // A device dword inside the window; 0 (and *inside false) outside it.
  uint32_t DeviceDword(uint32_t device_offset, bool* inside = nullptr) const {
    const bool ok = device_offset >= base_ && device_offset + 4 <= base_ + size_;
    if (inside) {
      *inside = ok;
    }
    return ok ? LoadBe32(bytes_ + (device_offset - base_)) : 0;
  }
  uint8_t DeviceByte(uint32_t device_offset) const {
    return device_offset >= base_ && device_offset < base_ + size_
               ? bytes_[device_offset - base_]
               : 0;
  }
  // A register by number. *present is false when the device keeps no shadow of
  // it, or the window does not reach it.
  uint32_t Register(uint32_t reg, bool* present = nullptr) const {
    const uint32_t offset = guest::RegisterDeviceOffset(reg);
    if (offset == 0) {
      if (present) {
        *present = false;
      }
      return 0;
    }
    return DeviceDword(offset, present);
  }
  float RegisterFloat(uint32_t reg) const {
    const uint32_t bits = Register(reg);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
  }

 private:
  const uint8_t* bytes_;
  uint32_t base_;
  uint32_t size_;
};

class DrawView {
 public:
  explicit DrawView(const uint8_t* payload)
      : payload_(payload),
        record_(*reinterpret_cast<const DrawRecord*>(payload)),
        layout_(ComputeDrawLayout(record_.decl_count, record_.vs_delta_count,
                                  record_.ps_delta_count, record_.inline_bytes)) {}

  const DrawRecord& record() const { return record_; }
  const DrawRecordLayout& layout() const { return layout_; }

  RegisterWindow registers() const {
    return RegisterWindow(record_.registers, guest::kRegisterWindowBegin,
                          guest::kRegisterWindowBytes);
  }
  // Fetch group `group` (0..31) as six host-order dwords.
  void FetchGroup(uint32_t group, uint32_t out[6]) const {
    const uint8_t* p = record_.fetch + group * guest::kFetchGroupBytes;
    for (uint32_t i = 0; i < 6; ++i) {
      out[i] = LoadBe32(p + 4 * i);
    }
  }
  // Vertex fetch constant `slot` (0..95, three per group) as two host-order dwords.
  void VertexFetch(uint32_t slot, uint32_t* d0, uint32_t* d1) const {
    const uint8_t* p = record_.fetch + (slot / 3) * guest::kFetchGroupBytes + (slot % 3) * 8;
    *d0 = LoadBe32(p);
    *d1 = LoadBe32(p + 4);
  }

  const uint8_t* decl_records() const { return payload_ + layout_.decl; }
  const uint8_t* vs_block_indices() const { return payload_ + layout_.vs_indices; }
  const uint8_t* ps_block_indices() const { return payload_ + layout_.ps_indices; }
  const uint8_t* vs_blocks() const { return payload_ + layout_.vs_blocks; }
  const uint8_t* ps_blocks() const { return payload_ + layout_.ps_blocks; }
  const uint8_t* inline_data() const { return payload_ + layout_.inline_data; }

 private:
  const uint8_t* payload_;
  const DrawRecord& record_;
  DrawRecordLayout layout_;
};

// The six dwords of a texture fetch constant out of a 52-byte D3DBaseTexture
// header (fetch constant at +28), host order.
inline void TextureHeaderFetch(const uint8_t* header, uint32_t out[6]) {
  for (uint32_t i = 0; i < 6; ++i) {
    out[i] = LoadBe32(header + guest::kTextureFetchOffset + 4 * i);
  }
}

}  // namespace mcla::gpu
