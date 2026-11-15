#pragma once
// ===========================================================================
// MCLA GPU renderer -- the guest shader store (shader pack v8)
// ===========================================================================
// The translated guest shaders (LAR-XenosRecomp HLSL compiled offline by
// tools/build_shader_pack_v7.py), keyed by the runtime shader identity
// (FNV-1a 64 of the vfetch-normalized microcode, native_gfx/shader_identity.h).
// Layout of the file: the builder's header (for v3, 24.27 for what v6 changes, 24.41 for v7). v7 is the shader
// contract this renderer is written against: v6's plus the host target's
// resolution scale (24.41: g_InvResolutionScale at byte 592, SharedConstants
// of 608 bytes; the pixel position a pixel shader receives is in guest
// pixels). v6 is v4's (-- SharedConstants of 592 bytes
// with g_PosScale / g_PosOffset, structured control flow, conditional exec
// blocks that test their condition) plus alpha to coverage in the pixel shader
// (24.27: g_AlphaToMask at byte 540; SV_Coverage beside oC0 only in the pixel
// shader variant 1, the one whose coverage depends on alpha). The legacy
// runtime's v2 pack keeps the old contract.
//
// The pack is memory-mapped read-only from a pack folder (Attach; the folders
// the renderer looks in are SearchFolders); only the pages of the blobs actually
// used are read. The header, every entry and every range are validated at
// open; a blob's FNV and magic are verified the first time it is used, and a
// bad blob marks its entry unusable, logged with its key.
//
// Pending and attached: the store starts without a pack (Initialize), because
// on a first run the packs are built from the player's game files after the
// renderer is up -- the renderer draws the ISO wizard before those files exist.
// While pending, Find and GetShader return null and count the asks
// (pending_lookups): nothing draws guest content, since a guest draw whose
// shader is not found is skipped. Attach opens and validates the pack and
// publishes it; from then on the store behaves as it always did. A pack that is
// in place at start is attached right in the renderer's Initialize, so nothing
// changes for it.
//
// Shaders: one plume::RenderShader per blob, created on first use on the
// render thread through Device::CreateShader (which rejects a null-backed
// result). Both backends have one blob per spec variant present (DXIL for
// D3D12, SPIR-V for Vulkan): the spec constants are fixed when the pack is
// compiled, because the variants differ in their outputs, which a
// specialization constant cannot change.
//
// Pixel shader variants: a second, optional file next to the pack (in the folder the main pack came
// from, folder()), mcla_shaders_variants.v2.pack, built from the same HLSL tree (the layout is in
// shaderpack/pack_format.h; the builder is src/shaderpack's variant step, variant_pack.h: it works on the
// game's translated shaders, none of it is part of the game's content). It carries, for pixel shaders the
// renderer can draw with another version of their text, a blob per backend of that version, keyed by the
// same identity and a variant id: the HDR scene's tone map (kHdrVariant, hdr_tonemap.h: its curve and its
// encode changed for values above white, with the identity of the vertex shaders it is drawn with) and the
// bloom's Gaussians at a render scale (kBloomVariant, pipelines.cpp), the environment map's level pass and
// reduction at the map's scale (kEnvVariant, pipelines.cpp) and the sun shadow collector's soft lookup
// (kShadowFilterVariant, pipelines.cpp, shadow_filter.h). A pipeline key names one with
// ps_variant. The pack is not the main one's contract: the SharedConstants block stays 608 bytes and a
// variant with constants of its own reads them after it (the HDR tone map: constants.h
// HdrSceneConstants; the shadow filter: shadow_filter.h ShadowFilterConstants; uploaded only by the
// draw that uses it). A missing, malformed or foreign file leaves
// every draw with the game's own shader: the HDR output at stage 2 (the scene at paper white), the bloom
// at the guest's taps. The renderer loads it right after the main pack is attached (LoadVariants), and
// has_variants() / has_hdr_variants() say it is usable: one atomic flag, set last (release) and read
// first (acquire), as attached().
//
// Threading: Find and GetShader from any thread once attached (the pipeline
// precache's workers call GetShader), and Find is safe before: it checks one
// atomic flag, set last by Attach (release) and read first by Find (acquire).
// Attach and Shutdown are called from one thread, with the guest not yet
// running (Attach) or the render thread stopped (Shutdown); the rest from the
// render thread.
// ===========================================================================

#include <atomic>
#include <cstdint>
#include <mutex>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>

#include "shaderpack/pack_format.h"

namespace rex::memory {
class MappedMemory;
}

namespace mcla::gpu {

class Device;

class ShaderStore {
 public:
  // The file layout and its constants are shared with the pack writer: shaderpack/pack_format.h.
  static constexpr const char* kPackFileName = larecomp::shaderpack::kPackFileName;
  static constexpr uint32_t kPackVersion = larecomp::shaderpack::kPackVersion;
  // The variant pack (above): file, format version, and the contract of its constants block (1:
  // SharedConstants, then for the HDR tone map float4 {knee, highlights, peak, 0} at byte 608).
  static constexpr const char* kVariantPackFileName = larecomp::shaderpack::kVariantPackFileName;
  static constexpr uint32_t kVariantPackVersion = larecomp::shaderpack::kVariantPackVersion;
  static constexpr uint32_t kVariantContract = larecomp::shaderpack::kVariantContract;
  // PipelineKey::ps_variant: 0 and 1 are the spec variants of the main pack (the shader entry's blobs),
  // the ids from kFirstPackedVariant are the variant pack's, up to kVariantSlots - 1.
  static constexpr uint32_t kFirstPackedVariant = larecomp::shaderpack::kFirstPackedVariant;
  static constexpr uint32_t kVariantSlots = larecomp::shaderpack::kVariantSlots;
  // A pixel shader's HDR scene variant, the bloom's Gaussians at a render scale, the environment map's mip
  // level pass and 64x32 reduction at the map's scale, and the sun shadow collector's soft lookup
  // (shadow_filter.h: its 16 bytes of constants follow the shared block).
  static constexpr uint32_t kHdrVariant = larecomp::shaderpack::kVariantHdrScene;
  static constexpr uint32_t kBloomVariant = larecomp::shaderpack::kVariantBloomGauss;
  static constexpr uint32_t kEnvVariant = larecomp::shaderpack::kVariantEnvMap;
  static constexpr uint32_t kShadowFilterVariant = larecomp::shaderpack::kVariantShadowFilter;
  // The glow sprites' soft visibility: a share of depth taps in place of the one compare (pipelines.cpp).
  static constexpr uint32_t kGlowVariant = larecomp::shaderpack::kVariantGlowSoft;
  // The player's car paint with its environment reads occluded by, or replaced with, the car's own previous frame
  // (car_selfrefl.h: its 112 bytes of constants follow the shared block; a test prototype).
  static constexpr uint32_t kCarSelfReflVariant = larecomp::shaderpack::kVariantCarSelfRefl;
  // The auto-exposure's sample pass (PSSampleAverageLuminance) with its nine taps spread over the screen area
  // they cover at 1x, at a render scale above 1 (shaderpack/variants/luminance_scale.h).
  static constexpr uint32_t kLuminanceVariant = larecomp::shaderpack::kVariantLuminanceScale;

  enum class Stage : uint8_t { kVertex, kPixel };

  struct Blob {
    uint32_t offset = 0;
    uint32_t size = 0;
    uint64_t fnv = 0;
  };

  enum EntryFlags : uint32_t {
    // a vfetch takes its index from something other than r0.x
    kComputedFetchIndex = larecomp::shaderpack::kFlagComputedFetchIndex,
  };

  // Pixel shader outputs (pack flags bits 8..12, PixelShader::outputs of the container).
  enum PixelOutputs : uint8_t {
    kOutputColor0 = larecomp::shaderpack::kPixelOutputColor0,
    kOutputColor1 = larecomp::shaderpack::kPixelOutputColor1,
    kOutputColor2 = larecomp::shaderpack::kPixelOutputColor2,
    kOutputColor3 = larecomp::shaderpack::kPixelOutputColor3,
    kOutputDepth = larecomp::shaderpack::kPixelOutputDepth,
  };

  struct Entry {
    uint64_t key = 0;
    Stage stage = Stage::kVertex;
    // Spec bits the shader uses: VS 0x1 R11G11B10_NORMAL, PS 0x2 ALPHA_TEST (the
    // variant whose coverage depends on alpha: alpha test and/or alpha to
    // coverage, constants.h).
    uint8_t spec_mask = 0;
    uint16_t velem_count = 0;
    uint32_t velem_offset = 0;
    uint32_t flags = 0;
    uint8_t ps_outputs = 0;           // PixelOutputs; 0 for vertex shaders
    uint32_t texture_fetch_mask = 0;  // bit i: a texture instruction reads fetch constant i
    Blob dxil[2];   // variant 0 (no spec bit) / variant 1 (spec_mask)
    Blob spirv[2];  // the same variants
    // The variant pack's variants this pixel shader has (LoadVariants): bit n is set when the pack has one
    // with id n (kHdrVariant, kBloomVariant...); 0 for the shaders that have none.
    uint32_t variants = 0;
    // Whether it has the HDR scene variant (bit kHdrVariant of `variants`).
    bool hdr_variant = false;
  };

  // What the variant pack says of one shader's variant.
  static constexpr uint32_t kVariantVertexShaders = larecomp::shaderpack::kVariantVertexShaders;
  struct Variant {
    uint64_t ps_key = 0;
    uint32_t variant = 0;
    // The vertex shaders the variant is drawn with (the ones whose outputs it reads: 0 = none).
    uint64_t vs_keys[kVariantVertexShaders] = {};
    uint32_t reserved = 0;
    Blob dxil;
    Blob spirv;
    bool DrawnWith(uint64_t vs_key) const {
      for (uint64_t key : vs_keys) {
        if (key != 0 && key == vs_key) {
          return true;
        }
      }
      return false;
    }
  };
  using HdrVariant = Variant;

  struct VertexElement {
    uint32_t address = 0;  // vfetch instruction address in the microcode
    uint32_t usage = 0;    // D3DDECLUSAGE
    uint32_t usage_index = 0;
  };

  enum class ShaderResult : uint8_t {
    kOk,
    kNoVariant,     // the variant asked for was not compiled for this shader
    kBadBlob,       // FNV or magic mismatch (logged once)
    kCreateFailed,  // the backend refused it (logged once)
    // No pack is attached yet (the store is pending).
    // The store is pending: no pack is attached yet, and the draw is skipped.
    kNoPack,
  };
  static const char* ResultName(ShaderResult result);

  struct Stats {
    uint64_t shaders_created = 0;
    uint64_t blobs_verified = 0;
    uint64_t blob_failures = 0;
    uint64_t create_failures = 0;
  };

  ShaderStore();
  ~ShaderStore();
  ShaderStore(const ShaderStore&) = delete;
  ShaderStore& operator=(const ShaderStore&) = delete;

  // Takes the device. Needs no pack: the store starts pending (see the top of this file) and is
  // attached by Attach/AttachFirst. Always true.
  bool Initialize(Device& device);
  // Opens and validates the main pack in `folder` (kPackFileName) and makes the store usable.
  // False (logged) when it is missing or malformed: the store stays pending and nothing of the
  // file is kept. A store that is attached already is left as it is (true).
  bool Attach(const std::filesystem::path& folder);
  // Attach for the first of `folders` that holds a valid pack; when none does, logs that the
  // store is pending and which folders were looked in.
  bool AttachFirst(const std::vector<std::filesystem::path>& folders);
  // The GPU must be idle: shaders are destroyed directly.
  void Shutdown();

  // The folders a pack is looked for in, in order. The cvar shader_pack_dir, when set, is the
  // only one (a test or a pack kept elsewhere must not be mixed with the others). Otherwise
  // `configured` (the folder the app named: gpu.h SetShaderPackFolder) or, when that is empty,
  // the places the packs are built into by default (<exe>/cache/shaders, then
  // %LOCALAPPDATA%\larecomp\cache\shaders); then <exe>/assets, where the pack lived before it
  // was built on the player's PC. The first folder with a valid pack wins.
  static std::vector<std::filesystem::path> SearchFolders(const std::filesystem::path& configured);
  // Where the packs are built by default: <exe>/cache/shaders when that can be written (tried
  // with a real file, as Controlled Folder Access allows reads and not writes), else
  // %LOCALAPPDATA%\larecomp\cache\shaders (~/.cache/larecomp/shaders off Windows). Makes the
  // folder.
  static std::filesystem::path DefaultPackFolder();
  // Where a build of the packs writes: the cvar shader_pack_dir when it is set, else
  // `configured` when it is not empty, else DefaultPackFolder().
  static std::filesystem::path BuildFolder(const std::filesystem::path& configured);

  // Any thread. False while the store is pending.
  bool attached() const { return attached_.load(std::memory_order_acquire); }
  // Find and GetShader calls answered "no pack yet" so far (Attach logs the count and clears it).
  uint64_t pending_lookups() const { return pending_lookups_.load(std::memory_order_relaxed); }
  uint32_t entry_count() const { return attached() ? uint32_t(entries_.size()) : 0u; }
  const std::filesystem::path& path() const { return path_; }
  // The folder the attached pack came from (the variant pack is read from there too).
  const std::filesystem::path& folder() const { return folder_; }
  const Stats& stats() const { return stats_; }

  // Null for an identity the pack does not have, and while pending (counted, pending_lookups).
  const Entry* Find(uint64_t identity) const;
  // Opens the optional variant pack beside the main one (in folder()). False when there is none or it
  // is not usable, with the reason in `why` (the caller logs it: absent is normal for a build that made
  // no variants). Marks the entries it has variants for (Entry::variants, hdr_variant). Once, right after
  // Attach and before the pipeline records are read; the guest has not drawn yet (the lifecycle thread).
  bool LoadVariants(std::string* why);
  // Any thread: the variants are loaded and usable (set last by LoadVariants, so FindVariant and
  // Entry::variants are in place for whoever sees true).
  bool has_variants() const { return variants_ready_.load(std::memory_order_acquire); }
  // The variants are loaded and the pack has the HDR scene variant.
  bool has_hdr_variants() const { return has_variants() && hdr_variant_count_ != 0; }
  const std::filesystem::path& variants_path() const { return variants_path_; }
  // The variant `variant` (kHdrVariant, kBloomVariant...) of pixel shader `identity`, null when it has none.
  const Variant* FindVariant(uint64_t identity, uint32_t variant) const;
  // The HDR scene variant of pixel shader `identity`, null when it has none.
  const HdrVariant* FindHdrVariant(uint64_t identity) const { return FindVariant(identity, kHdrVariant); }
  // Appends the vertex shader's elements to `out` (cleared first).
  void GetVertexElements(const Entry& entry, std::vector<VertexElement>& out) const;
  // The shader for `entry` with spec `variant` (0, or 1 = spec_mask applied, or a variant pack id,
  // kHdrVariant, kBloomVariant..., for a pixel shader that has that variant), created on first use. Null
  // with the reason in `result` otherwise.
  plume::RenderShader* GetShader(const Entry& entry, uint32_t variant, ShaderResult* result);

 private:
  struct Slot {
    std::unique_ptr<plume::RenderShader> shader;
    ShaderResult result = ShaderResult::kOk;
    bool done = false;
  };

  bool Validate();
  bool VerifyBlob(const Entry& entry, const Blob& blob, bool spirv, const uint8_t* base);

  Device* device_ = nullptr;
  std::mutex mutex_;  // GetShader's slots and stats
  std::atomic<bool> attached_{false};  // set last by Attach; Find checks it first
  mutable std::atomic<uint64_t> pending_lookups_{0};
  std::filesystem::path path_;    // the pack file
  std::filesystem::path folder_;  // its folder
  std::unique_ptr<rex::memory::MappedMemory> file_;
  std::unordered_map<uint64_t, Entry> entries_;
  std::unordered_map<uint64_t, Slot> slots_[kVariantSlots];  // per variant id, by identity
  std::atomic<bool> variants_ready_{false};  // set last by LoadVariants
  uint32_t hdr_variant_count_ = 0;           // the pack's HDR scene variants (written before variants_ready_)
  std::filesystem::path variants_path_;
  std::unique_ptr<rex::memory::MappedMemory> variants_file_;
  std::unordered_map<uint64_t, Variant> variants_[kVariantSlots];  // per variant id, by pixel shader identity
  Stats stats_;
};

}  // namespace mcla::gpu
