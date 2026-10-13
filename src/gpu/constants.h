#pragma once
// ===========================================================================
// MCLA GPU renderer -- the guest constants of a draw
// ===========================================================================.
//
// Banks. Each device's 256-register VS and PS banks, rebuilt on the render
// thread from the 64-byte blocks each draw record carries (only the blocks
// that changed, guest_capture.h), and compared with the guest's own XXH3 of
// the banks whenever a record carries one (every 32 draws and after a
// present): the caller reports a mismatch, it is never drawn with silently.
// The raw bytes stay big-endian; the upload swaps every dword to a host float.
// The replay and the optional validator (validation.h) each keep their own
// ConstantBanks.
//
// SharedConstants. The block of shader contract v5 (LAR-XenosRecomp
// shader_common.h, 592 bytes) the translated shaders read besides the banks:
// the bindless texture indices per fetch constant slot (bit 31 = PWL gamma on
// read) and sampler indices, the VS bools, the alpha test threshold, the
// alpha to coverage parameters, the MIN/MAX source factor and RT0's exponent
// bias (both through the premult modes), and the vertex position transform
// g_PosScale/g_PosOffset.
//
// Threading: render thread only.
// ===========================================================================

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "draw_decode.h"
#include "draw_state.h"
#include "guest_device.h"
#include "viewport.h"

namespace mcla::gpu {

inline constexpr uint32_t kFetchSlotCount = 32;
// TFETCH_GAMMA_BIT of shader_common.h: the texture index asks the shader to
// apply the Xenos piecewise-linear gamma on read.
inline constexpr uint32_t kTextureGammaBit = 1u << 31;

// shader_common.h, byte for byte (DEFINE_SHARED_CONSTANTS and the SPIR-V offsets).
struct SharedConstants {
  uint32_t texture_2d[kFetchSlotCount];    // 0
  uint32_t texture_3d[kFetchSlotCount];    // 128
  uint32_t texture_cube[kFetchSlotCount];  // 256
  uint32_t sampler[kFetchSlotCount];       // 384
  uint32_t booleans;                       // 512
  uint32_t swapped_texcoords;              // 516
  float half_pixel_offset[2];              // 520, reserved: not read in v4
  float alpha_threshold;                   // 528
  uint32_t blend_premult_rgb;              // 532
  uint32_t blend_premult_a;                // 536
  uint32_t alpha_to_mask;                  // 540, v5: AlphaToMaskParameters
  float blend_premult_constant[4];         // 544
  float pos_scale[4];                      // 560
  float pos_offset[4];                     // 576
};
static_assert(sizeof(SharedConstants) == 592, "SharedConstants is shader contract v4");
static_assert(offsetof(SharedConstants, sampler) == 384);
static_assert(offsetof(SharedConstants, booleans) == 512);
static_assert(offsetof(SharedConstants, alpha_threshold) == 528);
static_assert(offsetof(SharedConstants, alpha_to_mask) == 540);
static_assert(offsetof(SharedConstants, blend_premult_constant) == 544);
static_assert(offsetof(SharedConstants, pos_scale) == 560);
static_assert(offsetof(SharedConstants, pos_offset) == 576);

class ConstantBanks {
 public:
  struct Banks {
    std::array<uint8_t, guest::kConstantBankBytes> vs{};
    std::array<uint8_t, guest::kConstantBankBytes> ps{};
    uint64_t vs_version = 1;  // bumped by every draw that changed the bank
    uint64_t ps_version = 1;
  };
  // The comparison with the guest's hashes, when the record carried them.
  struct Check {
    bool done = false;
    bool vs_matches = true;
    bool ps_matches = true;
  };

  // Applies the draw's blocks to its device's banks. Returns the banks.
  const Banks& Apply(const DrawView& view, Check* check = nullptr);

 private:
  std::unordered_map<uint32_t, Banks> devices_;
};

// Swaps a big-endian bank into host floats (4096 bytes in, 4096 out).
void SwapBankToHost(const uint8_t* guest_bank, uint8_t* host_out);

// The alpha test as the translated pixel shader runs it -- clip(alpha -
// threshold), "keep alpha >= threshold", behind the ALPHA_TEST spec variant --
// for the guest's function and reference (the legacy runtime's rule,
// guest/render_state.cpp). ALWAYS needs no variant (a negative alpha would be
// discarded by clip), and a function that is not a lower bound (LESS, EQUAL,
// LESS_EQUAL, NOT_EQUAL with a reference other than 0) cannot be run: nothing
// is discarded and the caller reports it.
struct AlphaTest {
  bool discard = false;  // the ALPHA_TEST variant is needed
  float threshold = 0.0f;
  bool unsupported = false;
};
AlphaTest ComputeAlphaTest(const XenosState& state);

// g_AlphaToMask of contract v5 (the generated computeAlphaToMask, Xenia's
// rule): bits 0-7 RB_COLORCONTROL's alpha_to_mask_offset0..3,
// bit 8 enabled, bits 9-10 the sample count log2. 0 = off.
uint32_t AlphaToMaskParameters(const XenosState& state);

// What FillSharedConstants could not express; the caller reports each kind.
enum SharedConstantsIssue : uint32_t {
  kIssueMinMaxWithExpBias = 1u << 0,  // one premult mode cannot carry both
  kIssueExpBiasBeyondRt0 = 1u << 1,   // only RT0's bias reaches the shader
};

// Fills everything but the texture and sampler tables. Returns the issues
// (SharedConstantsIssue bits).
uint32_t FillSharedConstants(const XenosState& state, const ViewportInfo& viewport,
                             SharedConstants& out);

}  // namespace mcla::gpu
