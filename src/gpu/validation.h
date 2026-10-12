#pragma once
// ===========================================================================
// MCLA GPU renderer -- validation of the captured stream (milestone M3)
// ===========================================================================
// Decodes every record the render thread receives and checks what M4 will
// need from it: shader identities in
// the pack, shader objects created, spec variants, input layouts against the
// pack's vertex elements, render target / depth / texture formats, the
// constant banks rebuilt from the deltas against the guest's own hashes,
// clears, resolves, tiling, the front buffer. Nothing is drawn.
//
// Output: a log line every kReportInterval guest presents and at shutdown,
// and three CSV files next to the executable, rewritten at the same times:
//   mcla_gpu_validation_shaders.csv  identity,stage,ucode_bytes,draws,in_pack,blob_bytes
//                                    (the columns of the legacy runtime's
//                                    mcla_native_gfx_resources_shaders.csv, for
//                                    a direct comparison of the same scene)
//   mcla_gpu_validation_summary.csv  every counter
//   mcla_gpu_validation_formats.csv  kind,guest_value,guest_name,plume,status,uses
//
// Threading: render thread only.
// ===========================================================================

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "constants.h"
#include "format.h"
#include "records.h"
#include "shaders.h"

namespace mcla::gpu {

class FrameValidator {
 public:
  static constexpr uint64_t kReportInterval = 600;

  enum Counter : uint32_t {
    kDraws, kDrawsIndexed, kDrawsAuto, kDrawsInline,
    kVsUnreadable, kPsUnreadable, kNoPixelShader, kDeclUnreadable, kDeclTooLong,
    kIndexUnreadable, kInlineUnreadable, kInlineUnterminated, kIndex32Bit,
    kVsInPack, kVsNotInPack, kPsInPack, kPsNotInPack, kStageMismatch,
    kVsVariant1, kPsVariant1, kVariantMissing,
    kShaderOk, kShaderBadBlob, kShaderCreateFailed,
    kInputOk, kInputNoTable, kInputUnsupportedUsage, kInputUnknownDecltype,
    kInputMissingInPlume, kInputUnsupplied, kInputNoVertexFetch, kInputNoDecl,
    kComputedFetchDraws, kComputedFetchMismatch,
    kColorTargets, kColorFormatMissing, kColorFormatUnknown, kDepthTargets,
    kDepthFormatUnknown,
    kTexturesBound, kTextureFetchInvalid, kTextureFormatMissing, kTextureFormatUnknown,
    kBankChecks, kBankMismatchVs, kBankMismatchPs,
    kPolyOffsetDraws, kOtherDeviceDraws, kVizQueryDraws,
    kPredicatedDraws, kZPassDraws,
    kClears, kClearColor, kClearDepth, kClearStencil, kClearWholeSurface, kClearRect,
    kClearNothingBound, kClearUnreadable,
    kResolves, kResolvesNested, kResolveSourceUnbound, kResolveDestUnreadable,
    kResolveDestFormatUnknown, kResolveDestFormatMissing, kResolveWithClear,
    kResolveStackUnreadable,
    kTilingBegins, kTilingEnds, kTiles, kTilingNestedResolves,
    kPresents, kFrontBufferUnreadable, kFrontBufferFormatUnknown,
    kGuestUnreadable, kIdentityNew, kIdentityRenormalized, kConstantBlocksMasked,
    kConstantBlocksUnmasked, kFramesDrawCountMismatch, kFramesRecordCountMismatch,
    kCounterCount
  };

  explicit FrameValidator(ShaderStore& shaders) : shaders_(shaders) {}
  FrameValidator(const FrameValidator&) = delete;
  FrameValidator& operator=(const FrameValidator&) = delete;

  void OnDraw(const uint8_t* payload);
  void OnClear(const ClearRecord& record);
  void OnResolve(const ResolveRecord& record);
  void OnTilingBegin(const TilingBeginRecord& record);
  void OnTilingEnd(const TilingEndRecord& record);
  // Frame boundary: folds in the guest's capture statistics and reports every
  // kReportInterval presents.
  void OnPresent(const PresentRecord& record);
  // Final report and CSVs (at renderer shutdown).
  void Finish();

 private:
  struct ShaderInfo {
    const ShaderStore::Entry* entry = nullptr;  // null when not in the pack
    bool pixel = false;
    bool stage_mismatch = false;
    uint32_t ucode_bytes = 0;
    uint64_t draws = 0;
    uint32_t blob_bytes = 0;
  };
  struct FormatUse {
    format::Mapping mapping;
    uint64_t uses = 0;
  };

  void Count(Counter counter, uint64_t n = 1) {
    window_[counter] += n;
    total_[counter] += n;
  }
  void NoteFormat(const char* kind, uint32_t value, const format::Mapping& mapping);
  const ShaderStore::Entry* NoteShader(uint64_t identity, bool pixel, uint32_t ucode_bytes,
                                       bool readable);
  void CheckShaderObject(const ShaderStore::Entry& entry, uint32_t variant);
  void CheckInputLayout(const uint8_t* payload, const ShaderStore::Entry& vs,
                        bool* packed_normal);
  void ApplyConstants(const uint8_t* payload);
  void Report(const char* why);
  void WriteCsv() const;

  ShaderStore& shaders_;
  std::array<uint64_t, kCounterCount> total_{};
  std::array<uint64_t, kCounterCount> window_{};
  std::unordered_map<uint64_t, ShaderInfo> shader_table_;
  std::map<std::pair<std::string, uint32_t>, FormatUse> formats_;
  std::map<uint32_t, uint64_t> clear_callers_;
  std::map<uint32_t, uint64_t> resolve_callers_;
  ConstantBanks banks_;  // its own copy: the replay keeps another (constants.h)
  std::unordered_map<uint64_t, std::vector<ShaderStore::VertexElement>> velems_;
  std::vector<std::array<float, 4>> poly_offsets_;  // distinct front/back scale/offset, first 16
  // RB_SAMPLE_COUNT_CTL as the draws left it: distinct values (first 16) and
  // their draws. Whether occlusion counting is ever armed.
  std::map<uint32_t, uint64_t> sample_count_ctl_;
  uint32_t present_device_ = 0;
  // Records of the frame in progress, against the guest's own count in the
  // present (stream integrity: nothing lost, nothing duplicated).
  uint64_t frame_draws_ = 0;
  uint64_t frame_records_ = 0;

  // Costs, per window and in total.
  uint64_t window_frames_ = 0;
  uint64_t window_decode_ns_ = 0;
  uint64_t window_capture_ns_ = 0;
  uint64_t window_bytes_ = 0;
  uint64_t total_frames_ = 0;
  uint64_t total_decode_ns_ = 0;
  uint64_t total_capture_ns_ = 0;
  uint64_t total_bytes_ = 0;
  std::vector<ShaderStore::VertexElement> scratch_velems_;
};

}  // namespace mcla::gpu
