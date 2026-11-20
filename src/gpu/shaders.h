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
// Shader variants: a second, optional file next to the pack (in the folder the main pack came
// from, folder()), mcla_shaders_variants.v3.pack, built from the same HLSL tree (the layout is in
// shaderpack/pack_format.h; the builder is src/shaderpack's variant step, variant_pack.h: it works on the
// game's translated shaders, none of it is part of the game's content). It carries, for shaders the
// renderer can draw with another version of their text, a blob per backend of that version, keyed by the
// same identity and a VARIANT WORD (shaderpack/variant_chain.h): bit 0 the main pack's spec variant, bits 1..31 the
// set of features whose edits the text has. A shader can have the edits of several features at once (a
// composed shader: the stages of the transform chain applied one after the other), vertex shaders can have
// them too, and the word of a pixel shader that needs a vertex stage's export names the vertex features its
// draw's vertex shader must have (Variant::vs_features). The features: the HDR scene's tone map (kHdrVariant,
// hdr_tonemap.h: its curve and its encode changed for values above white, with the identity of the vertex
// shaders it is drawn with), the bloom's Gaussians at a render scale (kBloomVariant, pipelines.cpp), the
// environment map's level pass and reduction at the map's scale (kEnvVariant, pipelines.cpp), the sun shadow
// collector's soft lookup (kShadowFilterVariant, pipelines.cpp, shadow_filter.h) ... A pipeline key names the
// word with ps_variant (and vs_variant). The renderer picks per draw: pipelines.cpp turns the settings into the set of
// WANTED features and ChooseVariant takes the best word of the shader that has only wanted features (the most of
// them), else the game's own shader; a composition that the pack does not have costs a feature, never a draw. The
// pack is not the main one's contract: the SharedConstants block stays 608 bytes and a
// feature with constants of its own reads them after it, at the place the chain's layout gives
// (chain::LayoutConstants: dense in feature order, a feature alone at byte 608; the HDR tone map: constants.h
// HdrSceneConstants; the shadow filter: shadow_filter.h ShadowFilterConstants; uploaded only by the
// draw that uses it). A missing, malformed or foreign file (another version of the variant pack is refused
// whole, with its version in the log) leaves
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
#include "shaderpack/variant_chain.h"

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
  // PipelineKey::ps_variant (and vs_variant): the variant word. 0 and 1 are the spec variants of the main pack (the shader entry's
  // blobs); from kFirstPackedVariant the variant pack's: bit 0 the spec variant, the other bits the features (kFeature* in
  // shaderpack/pack_format.h, kFeatureBit below), a composed shader has several. Version 2 of the pack had one id per change (2..15,
  // kVariantSlots): the names below are the WORD of each feature alone (spec variant 0), so a key that has only that one still
  // compares equal to them; for a key that may have several use chain::HasFeature(word, feature).
  static constexpr uint32_t kFirstPackedVariant = larecomp::shaderpack::kFirstPackedVariant;
  static constexpr uint32_t kMaxFeatures = larecomp::shaderpack::kMaxFeatures;
  static constexpr uint32_t kMaxWordsPerShader = larecomp::shaderpack::chain::kMaxWordsPerShader;
  // The bit of a feature in Entry::features and in the wanted set.
  static constexpr uint32_t FeatureBit(uint32_t feature) { return larecomp::shaderpack::chain::FeatureBit(feature); }
  // A pixel shader's HDR scene variant, the bloom's Gaussians at a render scale, the environment map's mip
  // level pass and 64x32 reduction at the map's scale, and the sun shadow collector's soft lookup
  // (shadow_filter.h: its 16 bytes of constants follow the shared block).
  static constexpr uint32_t kHdrVariant = larecomp::shaderpack::chain::SoloWord(larecomp::shaderpack::kFeatureHdrScene);
  static constexpr uint32_t kBloomVariant = larecomp::shaderpack::chain::SoloWord(larecomp::shaderpack::kFeatureBloomGauss);
  static constexpr uint32_t kEnvVariant = larecomp::shaderpack::chain::SoloWord(larecomp::shaderpack::kFeatureEnvMap);
  static constexpr uint32_t kShadowFilterVariant = larecomp::shaderpack::chain::SoloWord(larecomp::shaderpack::kFeatureShadowFilter);
  // The glow sprites' soft visibility: a share of depth taps in place of the one compare (pipelines.cpp).
  static constexpr uint32_t kGlowVariant = larecomp::shaderpack::chain::SoloWord(larecomp::shaderpack::kFeatureGlowSoft);
  // The player's car paint with its environment reads occluded by, or replaced with, the car's own previous frame
  // (car_selfrefl.h: its 112 bytes of constants follow the shared block; a test prototype).
  static constexpr uint32_t kCarSelfReflVariant = larecomp::shaderpack::chain::SoloWord(larecomp::shaderpack::kFeatureCarSelfRefl);
  // The car's chrome (xChrome::PS_Chrome: the tailpipes, rims, trim) with its reflection and sky ambient occluded by the car's own
  // parts: the same 112 bytes of constants as the paint's after the shared block (shaderpack/variants/car_chrome.h).
  static constexpr uint32_t kCarChromeVariant = larecomp::shaderpack::chain::SoloWord(larecomp::shaderpack::kFeatureCarChrome);
  // The auto-exposure's sample pass (PSSampleAverageLuminance) with its nine taps spread over the screen area
  // they cover at 1x, at a render scale above 1 (shaderpack/variants/luminance_scale.h).
  static constexpr uint32_t kLuminanceVariant = larecomp::shaderpack::chain::SoloWord(larecomp::shaderpack::kFeatureLuminanceScale);
  // The Flash player's textured pixel shaders with every 2D fetch reconstructed sharper where the console's bitmaps are
  // magnified (ui_sharp.h, shaderpack/variants/ui_sharp.h): chosen by the Scene for the UI phase's draws, not by pipelines.cpp;
  // 16 bytes of constants follow the shared block. Its alpha test twin (the main pack's spec variant 1) is the same feature with the
  // word's bit 0 set: kUiSharpAlphaVariant.
  static constexpr uint32_t kUiSharpVariant = larecomp::shaderpack::chain::SoloWord(larecomp::shaderpack::kFeatureUiSharp);
  static constexpr uint32_t kUiSharpAlphaVariant = larecomp::shaderpack::chain::Word(larecomp::shaderpack::chain::FeatureBit(larecomp::shaderpack::kFeatureUiSharp), 1);

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
    // The variant pack's variants of this shader (LoadVariants), a pixel shader's or a vertex shader's: `features` is the union of the
    // features of its words (bit n for the feature n: FeatureBit; 0 for the shaders that have none), `words` the variant words
    // themselves, composed ones and spec variant 1 twins included (at most kMaxWordsPerShader), `needs[i]` the vertex features
    // the pixel shader's word i wants its draw's vertex shader to have.
    uint32_t features = 0;
    uint8_t word_count = 0;
    uint32_t words[larecomp::shaderpack::chain::kMaxWordsPerShader] = {};
    uint32_t needs[larecomp::shaderpack::chain::kMaxWordsPerShader] = {};
    // Whether it has the HDR scene variant (the feature kFeatureHdrScene alone, spec variant 0).
    bool hdr_variant = false;
    // Whether the pack has the word `word` (a variant word from kFirstPackedVariant) of this shader.
    bool HasWord(uint32_t word) const {
      for (uint32_t i = 0; i < word_count; ++i) {
        if (words[i] == word) {
          return true;
        }
      }
      return false;
    }
  };

  // What the variant pack says of one shader's variant.
  static constexpr uint32_t kVariantVertexShaders = larecomp::shaderpack::kVariantVertexShaders;
  struct Variant {
    uint64_t ps_key = 0;  // the shader's identity: a pixel shader's, or a vertex shader's when `vertex`
    uint32_t variant = 0;  // the word
    // The vertex shaders the variant is drawn with (the ones whose outputs it reads: 0 = none).
    uint64_t vs_keys[kVariantVertexShaders] = {};
    uint32_t vs_features = 0;  // a pixel shader's: the vertex features its draw's vertex shader must have
    bool vertex = false;
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
  // The variant word `variant` (kHdrVariant, kBloomVariant, or a composed one) of shader `identity`, null when it has none.
  const Variant* FindVariant(uint64_t identity, uint32_t variant) const;
  // The HDR scene variant of pixel shader `identity`, null when it has none.
  const HdrVariant* FindHdrVariant(uint64_t identity) const { return FindVariant(identity, kHdrVariant); }
  // The variant word a draw of pixel shader `ps` takes when the settings want the features in `wanted` (a mask of FeatureBit): the
  // word of the pack with the most features, all wanted (shaderpack/variant_chain.h Pick), whose vertex needs the draw's vertex
  // shader `vs` meets; `spec` is the draw's spec variant (0, or 1 for the alpha test: only a word with that bit and twin is taken).
  // The game's own word (`spec`) when none fits. *vs_word is the vertex shader's word for the draw: its own spec variant `vs_spec`
  // and, when the chosen word needs vertex features, those. Render thread (the draw path); no lock, no allocation.
  uint32_t ChooseVariant(const Entry& ps, uint32_t wanted, uint32_t spec, const Entry* vs, uint32_t vs_spec,
                         uint32_t* vs_word) const;
  // Appends the vertex shader's elements to `out` (cleared first).
  void GetVertexElements(const Entry& entry, std::vector<VertexElement>& out) const;
  // The shader for `entry` with `variant` (0, or 1 = spec_mask applied, or a variant pack word, kHdrVariant, kBloomVariant... or a
  // composed one, for a shader that has that variant), created on first use. Null with the reason in `result` otherwise.
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
  // Created shaders: the main pack's two spec variants by identity, the variant pack's by (identity, word).
  struct WordKey {
    uint64_t key = 0;
    uint32_t word = 0;
    bool operator==(const WordKey& o) const { return key == o.key && word == o.word; }
  };
  struct WordKeyHash {
    size_t operator()(const WordKey& k) const { return size_t(k.key * 0x9E3779B97F4A7C15ull) ^ (size_t(k.word) * 0xC2B2AE3D27D4EB4Full); }
  };
  std::unordered_map<uint64_t, Slot> slots_[2];
  std::unordered_map<WordKey, Slot, WordKeyHash> packed_slots_;
  std::atomic<bool> variants_ready_{false};  // set last by LoadVariants
  uint32_t hdr_variant_count_ = 0;           // the pack's HDR scene variants (written before variants_ready_)
  std::filesystem::path variants_path_;
  std::unique_ptr<rex::memory::MappedMemory> variants_file_;
  std::unordered_map<WordKey, Variant, WordKeyHash> variants_;  // by (identity, word)
  Stats stats_;
};

}  // namespace mcla::gpu
