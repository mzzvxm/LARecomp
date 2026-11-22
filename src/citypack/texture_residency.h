#pragma once
// ===========================================================================
// LARecomp city pack -- which city textures are resident, and what a graphics class keeps of them (CITY-RT M2 package H2)
// ===========================================================================
// The texture file holds every picture at full detail (texture_set.h). What a class makes RESIDENT on the GPU is decided here, without any GPU in sight (the renderer's loader,
// gpu/city_texture_set.h, does what this says: it reads, creates, uploads and publishes the rows):
//
//   CAP        a class keeps the levels of a texture whose longer side is at most the cap (HitBudget::texture_cap_px: 128 Mid, 256 High, 0 = all for Ultra): FirstResidentMip. A texture
//              with no level that small (a picture stored with a single level, which the game itself samples at its one size) keeps what it has.
//   BY SECTOR  the textures a sector's hits can show are known from its runs' materials (CollectTextures); a sector that is resident in the streaming ring references them, a sector that
//              leaves gives them back. A texture is wanted while any resident sector references it, with the priority of the nearest of them (the ring's distance: smaller = sooner).
//   BUDGET     the bytes the resident set may take (HitBudget::texture_bytes; 0 = no limit), counting what is being read and what is waiting to be uploaded. When a wanted texture
//              does not fit, textures no sector references are given back first (the longest unused first), then the wanted ones of a worse priority than the one that is waiting.
//   UPLOADS    at most HitBudget::texture_upload_bytes a frame (8 MB: 30 to 67 MB of the old estimate was 4 to 9 frames; the whole set at full detail is a few seconds), nearest sector
//              first, and always at least one texture so that a big one cannot wait forever.
//   FALLBACK   a texture that is not resident has no slot: its row reads kNoSlot and the hit takes the material's average colour (city_hit_format.h). Nothing waits for a load.
//
// Pure host code (no GPU, no file): the loader drives it each frame, and the tests drive it with a fake loader. One thread (the render thread) calls it.
// ===========================================================================

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "citypack/city_hit_format.h"

namespace larecomp::citypack {

// The first level a class cap keeps (0 = the full size): the smallest level whose longer side is at most `cap_px`; the last level when none is (a single level texture bigger than the
// cap); 0 when cap_px is 0 (no cap). A block compressed texture is never made smaller than one block on a side at its first resident level (a level of 2 x 128 cannot be the top of a BC
// texture on D3D12): an elongated texture that would need it keeps the last level that has both sides at 4 texels or more, which may be above the cap.
uint32_t FirstResidentMip(const HitTexture& texture, uint32_t cap_px);
// The bytes of the levels from `first_mip`: the block data, no padding (what the file read and the upload move).
uint64_t ResidentBytes(const HitTexture& texture, uint32_t first_mip);
// What the budget counts for it: the same, rounded up to the allocator's granularity (a texture is its own allocation: 4 KiB is what D3D12's small-texture placement needs; the real
// figure is read from the renderer's memory line, CHECK). With `large_alignment` set a texture whose first resident level takes more than `small_base_limit` bytes is rounded to that
// instead (D3D12MA places such a texture on 64 KiB: city_texture_rules.h AllocationBytes is the rule the renderer uses).
uint64_t AccountedBytes(const HitTexture& texture, uint32_t first_mip, uint64_t alignment, uint64_t large_alignment = 0, uint64_t small_base_limit = 0);

// How a texture's levels from `first_mip` are laid out for the upload: each level's rows copied into the staging memory on `pitch_alignment` bytes (D3D12's placement rule, which both
// backends follow: 256), its place in the tightly packed bytes the file gives (ReadMips). A level smaller than a block counts as one block.
struct UploadLevel {
  uint32_t width = 0;          // texels
  uint32_t height = 0;
  uint32_t rows = 0;           // block rows
  uint32_t row_bytes = 0;      // one row of blocks, tight
  uint32_t pitch = 0;          // ... in the staging memory
  uint64_t source_offset = 0;  // of the level in the file's bytes
  uint64_t staging_bytes() const { return uint64_t(pitch) * rows; }
};
inline constexpr uint32_t kUploadPitchAlignment = 256;
std::vector<UploadLevel> PlanUploadLevels(const HitTexture& texture, uint32_t first_mip, uint32_t pitch_alignment = kUploadPitchAlignment);

struct ResidentTotals {
  uint64_t bytes = 0;
  uint32_t textures = 0;       // all of them
  uint32_t capped = 0;         // textures the cap shortened
};
// Every texture of the table resident under `cap_px`.
ResidentTotals TotalResidentBytes(const std::vector<HitTexture>& table, uint32_t cap_px);

// The distinct texture indices the runs' materials name: the textures a sector's hits can show (HitMaterial::texture, and ::layer where it is not kNoTexture).
std::vector<uint16_t> CollectTextures(const std::vector<HitRun>& runs, const std::vector<HitMaterial>& materials);
// The same from the material table as the GPU holds it (eight words a material, HitMaterial::Pack: word 1 is the diffuse in its low half and the second layer in its high half), the way the
// renderer has it (HitReader::material_words), and optionally of the cut-out runs alone (HitRun::flags kRunAlphaTested): what the ray traced shadows read. A run whose material is not in
// the table is skipped. Sorted, each index once.
std::vector<uint16_t> CollectTexturesOfWords(const std::vector<HitRun>& runs, const uint32_t* material_words, size_t material_count, bool alpha_tested_only);

struct ResidencyConfig {
  uint32_t cap_px = 0;                       // HitBudget::texture_cap_px
  uint64_t budget_bytes = 0;                 // HitBudget::texture_bytes; 0: no limit
  uint64_t upload_bytes_per_frame = 8u << 20;
  uint32_t max_reads_in_flight = 6;          // file reads the worker holds at once
  uint32_t max_uploads_per_frame = 16;       // textures made in one frame (the bytes limit alone lets a few hundred small ones through at the first ring: CHECK 4 of city_texture_set.h)
  uint32_t max_textures = 0xFFFFFFFFu;       // textures held at once (reading, waiting or resident); no limit by default, 0 holds none. The loader sets it from the bindless slots that are free (city_texture_rules.h)
  uint64_t max_ready_bytes = 32u << 20;      // read but not yet uploaded
  uint64_t allocation_alignment = 4096;      // each texture counts against the budget rounded up to this (AccountedBytes) ...
  uint64_t large_alignment = 0;              // ... or to this when its first resident level takes more than small_base_limit bytes (0: never)
  uint64_t small_base_limit = 0;
  uint64_t idle_frames = 1800;               // a texture no sector references is kept this long (30 s at 60 FPS) when memory is not needed
  uint64_t retry_frames = 600;               // after a failed read
};

enum class TextureState : uint8_t { kAbsent = 0, kReading, kReady, kResident };

class TextureResidency {
 public:
  // `table` must outlive the object.
  explicit TextureResidency(const std::vector<HitTexture>& table);

  // A changed cap re-plans every resident texture (those whose first level changes are given back and wanted again); a changed budget is applied by the next Step.
  void Configure(const ResidencyConfig& config);
  const ResidencyConfig& config() const { return config_; }

  // A sector is resident (its structure is built): it references these textures. Calling again for a row replaces its list and priority.
  void AddSector(uint32_t row, const uint16_t* textures, size_t count, float priority);
  void SetSectorPriority(uint32_t row, float priority);
  void RemoveSector(uint32_t row);

  struct Read {
    uint32_t index = 0;
    uint32_t first_mip = 0;
    uint64_t bytes = 0;
  };
  struct Plan {
    std::vector<Read> reads;           // start these reads now
    std::vector<uint32_t> uploads;     // upload these textures this frame (their bytes are read); they count as resident from now on
    std::vector<uint32_t> evictions;   // release these (slot, texture); their rows read kNoSlot from now on
  };
  // One call a frame. Evictions come first in the loader's order of work (the room they free is counted at once).
  Plan Step(uint64_t frame);

  // The loader's answers. ReadFinished(ok): true: the bytes are in memory and the texture is Ready; false: the read failed (Absent, tried again after retry_frames) or the texture was
  // given back while it was being read (Absent): the loader drops the bytes. UploadFailed: no upload memory this frame (Ready again). CreationFailed: the device would not make the
  // texture (a full video memory): it is Absent again, its bytes are dropped (the loader frees them), and it is tried again after retry_frames.
  bool ReadFinished(uint32_t index, bool ok);
  void UploadFailed(uint32_t index);
  void CreationFailed(uint32_t index);

  TextureState state(uint32_t index) const { return states_[index].state; }
  uint32_t first_mip(uint32_t index) const { return states_[index].first_mip; }
  uint32_t referenced_by(uint32_t index) const { return states_[index].refs; }
  uint64_t committed_bytes() const { return committed_; }   // resident + reading + ready
  uint32_t committed_count() const { return committed_count_; }
  uint64_t resident_bytes() const { return resident_; }
  uint32_t resident_count() const { return resident_count_; }
  uint32_t texture_count() const { return uint32_t(states_.size()); }
  uint64_t evicted_total() const { return evicted_total_; }
  uint64_t failed_reads() const { return failed_reads_; }
  uint64_t failed_creations() const { return failed_creations_; }
  // The bytes the budget counts for texture `index` at the cap in force (what a read of it would add).
  uint64_t accounted_bytes_if_loaded(uint32_t index) const { return PlannedBytes(index); }

 private:
  struct Tex {
    TextureState state = TextureState::kAbsent;
    uint32_t first_mip = 0;
    uint64_t bytes = 0;            // of the planned levels, while Reading, Ready or Resident
    uint32_t refs = 0;
    float priority = 0.0f;         // the best (smallest) of the referencing sectors'
    uint64_t last_wanted = 0;      // the last frame a sector referenced it
    uint64_t retry_at = 0;
    bool stale = false;            // a read in flight that a changed cap made useless
  };
  struct Sector {
    std::vector<uint16_t> textures;
    float priority = 0.0f;
  };

  void Recompute();
  uint64_t PlannedBytes(uint32_t index) const;
  void Drop(uint32_t index, Plan* plan);
  bool MakeRoom(uint64_t need, float priority, Plan* plan);
  bool Over(uint64_t need) const;

  const std::vector<HitTexture>& table_;
  ResidencyConfig config_;
  std::vector<Tex> states_;
  std::unordered_map<uint32_t, Sector> sectors_;
  bool dirty_ = true;
  bool config_changed_ = true;
  uint64_t frame_ = 0;
  uint64_t committed_ = 0;
  uint64_t resident_ = 0;
  uint64_t reading_ = 0;
  uint64_t ready_ = 0;
  uint32_t reads_in_flight_ = 0;
  uint32_t resident_count_ = 0;
  uint32_t committed_count_ = 0;
  uint64_t evicted_total_ = 0;
  uint64_t failed_reads_ = 0;
  uint64_t failed_creations_ = 0;
};

}  // namespace larecomp::citypack
