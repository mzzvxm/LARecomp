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
// HDR scene variants (stage 3 of the HDR output, hdr_tonemap.h): a second, optional file next to
// the pack (in the folder the main pack came from, folder()), mcla_shaders_hdr.v1.pack, built from
// the same HLSL tree (the layout is in shaderpack/pack_format.h; the builder is src/shaderpack's
// hdr_variant step, tools/build_hdr_variants.py for development: neither is part of the game's
// content, they work on the game's translated shaders). It carries, for the pixel shaders the HDR output draws again into a float image (the
// game's tone map), a third blob per backend -- the shader with its curve and its encode changed
// for values above white -- keyed by the same identity, together with the identity of the vertex
// shaders it is drawn with. A pipeline key names it with ps_variant kHdrVariant. The pack is not
// the main one's contract: the SharedConstants block stays 608 bytes and the variant reads 16
// bytes after it that only the variant draws upload (constants.h HdrSceneConstants). A missing,
// malformed or foreign file leaves the HDR output at stage 2 (the scene at paper white). The
// renderer loads it right after the main pack is attached (LoadHdrVariants), and has_hdr_variants()
// says it is usable: one atomic flag, set last (release) and read first (acquire), as attached().
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
  // The HDR scene variants (above): file, format version, and the contract of its constants block
  // (1: SharedConstants, then float4 {knee, highlights, peak, 0} at byte 608).
  static constexpr const char* kHdrPackFileName = larecomp::shaderpack::kHdrPackFileName;
  static constexpr uint32_t kHdrPackVersion = larecomp::shaderpack::kHdrPackVersion;
  static constexpr uint32_t kHdrContract = larecomp::shaderpack::kHdrContract;
  // PipelineKey::ps_variant of a pixel shader's HDR scene variant (0 and 1 are the spec variants).
  static constexpr uint32_t kHdrVariant = 2;

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
    // A pixel shader that has an HDR scene variant (LoadHdrVariants), else false.
    bool hdr_variant = false;
  };

  // What the HDR pack says of one shader.
  static constexpr uint32_t kHdrVertexShaders = larecomp::shaderpack::kHdrVertexShaders;
  struct HdrVariant {
    uint64_t ps_key = 0;
    // The vertex shaders the variant is drawn with (the ones whose outputs it reads: 0 = none).
    uint64_t vs_keys[kHdrVertexShaders] = {};
    uint32_t flags = 0;
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
  // The folder the attached pack came from (the HDR variant pack is read from there too).
  const std::filesystem::path& folder() const { return folder_; }
  const Stats& stats() const { return stats_; }

  // Null for an identity the pack does not have, and while pending (counted, pending_lookups).
  const Entry* Find(uint64_t identity) const;
  // Opens the optional HDR pack beside the main one (in folder()). False when there is none or it
  // is not usable, with the reason in `why` (the caller logs it: absent is normal without HDR).
  // Marks the entries it has a variant for (Entry::hdr_variant). Once, right after Attach and
  // before the pipeline records are read; the guest has not drawn yet (the lifecycle thread).
  bool LoadHdrVariants(std::string* why);
  // Any thread: the variants are loaded and usable (set last by LoadHdrVariants, so FindHdrVariant
  // and Entry::hdr_variant are in place for whoever sees true).
  bool has_hdr_variants() const { return hdr_ready_.load(std::memory_order_acquire); }
  const std::filesystem::path& hdr_path() const { return hdr_path_; }
  // The variant of pixel shader `identity`, null when it has none.
  const HdrVariant* FindHdrVariant(uint64_t identity) const;
  // Appends the vertex shader's elements to `out` (cleared first).
  void GetVertexElements(const Entry& entry, std::vector<VertexElement>& out) const;
  // The shader for `entry` with spec `variant` (0, or 1 = spec_mask applied, or kHdrVariant for
  // the HDR scene variant of a pixel shader that has one), created on first use. Null with the
  // reason in `result` otherwise.
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
  std::unordered_map<uint64_t, Slot> slots_[3];  // per variant (2: HDR scene), by identity
  std::atomic<bool> hdr_ready_{false};  // set last by LoadHdrVariants
  std::filesystem::path hdr_path_;
  std::unique_ptr<rex::memory::MappedMemory> hdr_file_;
  std::unordered_map<uint64_t, HdrVariant> hdr_variants_;
  Stats stats_;
};

}  // namespace mcla::gpu
