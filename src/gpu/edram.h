#pragma once
// ===========================================================================
// MCLA GPU renderer -- the guest's EDRAM render targets on the host
// ===========================================================================
// A draw names its targets
// only through registers: a range of EDRAM tiles seen with a pitch, a format
// and a sample count. One host texture stands for each such identity --
// (colour or depth, base tile, guest format with the _AS_ variants folded,
// pitch, samples) -- for the life of the renderer: the EDRAM keeps its
// contents across frames and so does the host (the photo mode reads what an
// earlier frame left, memory project_native_gfx_photo_capture). Identities
// never exchange contents (measured, 24.3).
//
// Size: width = the pitch; height = the largest any use asked for (rounded
// up to kHeightAlignment, at most kMaxExtent). A use that needs more rows
// recreates the texture and copies the old one into it whole -- colour as one
// subresource, MSAA included; depth as its depth and stencil planes (Plume's
// SubresourcePlane, a fork addition) -- so rows drawn earlier in the frame
// survive. The old texture and every framebuffer naming it are retired by the
// FrameRing's serial rule.
//
// A new texture is cleared whole before anything else touches it: D3D12
// requires a render target or depth buffer from a heap created without
// zeroing to be cleared, discarded or wholly copied first (debug layer id
// 1422); the EDRAM's own initial contents are undefined, and zeros stand for
// them. The clear binds a framebuffer: Get says so through
// *framebuffer_unset, and leaves none bound.
//
// Layout: each target remembers its last layout; Transition records a
// barrier only when it changes.
//
// Shader views (M4.6): a resolve reads a target through bindless
// slots -- the colour or depth plane, and the stencil plane of a depth target
// (Plume's planeSlice, a fork addition) -- made on the first resolve that asks
// (SampleSlots) and remade when the texture is replaced by growth. Clears
// (ClearColor/ClearDepthStencil) are shared by the Scene's clear records and
// the clears a resolve does after its copy, with one value rule
// (EncodeColorClear).
//
// Ownership (M5.2d): the identities never exchange contents, and
// the census (24.3) found the guest never needs them to -- every change of
// configuration over another's tiles starts with a clear or an opaque full
// rect. That is watched, not assumed: per EDRAM tile (80x16 samples, the
// SDK's geometry) and per aspect of its 32-bit words -- the main bits (a
// colour word whole, or a depth word's depth24) and the stencil byte (the low
// 8 bits; a colour write writes it too) -- the identity that wrote it last.
// An access that replaces every texel of an aspect it touches takes it; one
// that may keep, blend or read what an aspect held, over tiles another
// identity wrote last, is counted and reported -- on the console it would
// see that identity's bytes, which the host does not carry over (TODO
// M5.2d). A detector may report too much, never too little. It runs only
// with mcla_gpu_edram_aliasing, read at start: off, none of its work is done
// (no owners, no tile map, no notes, no query pool); its coverage checks
// only with mcla_gpu_coverage_queries as well.
//
// Reach (the M5.2d closure): whether inherited bytes leave the
// EDRAM. A write that keeps or blends another identity's tiles marks them
// (taint, per tile and aspect) until something takes them; a resolve whose
// source had another identity's tiles or marked ones is an inherited resolve,
// handed to the ResolveCache by its number (TakeInheritedResolves), which
// marks what it wrote and reports the readers of it through NoteReach.
//
// Threading: render thread only, after Initialize.
// ===========================================================================

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>

namespace mcla::gpu {

class BindlessHeap;
class Device;
class FrameRing;

struct EdramKey {
  uint16_t base_tiles = 0;
  uint16_t pitch = 0;     // pixels
  uint8_t depth = 0;      // 1: depth/stencil target
  uint8_t format = 0;     // xenos Color/DepthRenderTargetFormat, _AS_ folded
  uint8_t samples = 1;
  uint8_t reserved = 0;
  bool operator==(const EdramKey& o) const {
    return base_tiles == o.base_tiles && pitch == o.pitch && depth == o.depth &&
           format == o.format && samples == o.samples;
  }
};

// The colour format a target is stored as: the _AS_ variants share the
// storage of their base format (24.12).
uint32_t FoldColorFormat(uint32_t color_format);

// "colour k_8_8_8_8@224 pitch 160 4x", for logs.
std::string DescribeEdramKey(const EdramKey& key);

// What a colour clear stores in a target of (folded) `color_format` with
// RB_COLOR_INFO's `exp_bias` (IDB sub_824206A8): the colour times
// 2^bias; the host's SNORM 16-bit targets hold value/32; the _GAMMA targets,
// kept linear, saturated.
void EncodeColorClear(uint32_t color_format, int32_t exp_bias, const float color[4],
                      float out[4]);

// What an EDRAM access was, for the reports: plain values, kept
// while a tiled section waits for its tiles.
struct EdramEvent {
  enum class Kind : uint8_t { kDraw, kClear, kResolve };
  Kind kind = Kind::kDraw;
  // kDraw
  const char* draw_kind = "";  // static names
  const char* primitive = "";
  uint32_t count = 0;
  uint32_t blend = 0;       // colour target 0's RB_BLENDCONTROL
  uint32_t write_mask = 0;  // and its write mask
  uint32_t depth_control = 0;
  uint32_t color_control = 0;
  uint32_t stencil = 0;
  uint32_t stencil_bf = 0;
  uint32_t aa_mask = 0;
  plume::RenderRect scissor;
  uint64_t vs = 0;
  uint64_t ps = 0;
  // kResolve: the destination, and the ResolveCache's number for the copy
  // (0: none), handed back when the copy turns out inherited.
  uint32_t dest_page = 0;
  uint32_t dest_format = 0;
  uint32_t dest_width = 0;
  uint32_t dest_height = 0;
  uint32_t resolve = 0;
  // The coverage check of a draw whose takes rest on its extent
  // (EdramTargets::ReserveCoverageCheck), or 0xFFFFFFFF.
  uint32_t check = 0xFFFFFFFFu;
};

// One tile of a tiled section (BeginTiling sub_8241BE78): its
// rectangle in surface pixels (dev+12752) and the origin the GPU moves to the
// EDRAM's (dev+12992: x1 & ~31, y1 & ~31).
struct EdramTile {
  plume::RenderRect rect;
  int32_t origin_x = 0;
  int32_t origin_y = 0;
};

class EdramTargets {
 public:
  static constexpr uint32_t kHeightAlignment = 32;
  static constexpr uint32_t kMaxExtent = 8192;

  struct Target {
    EdramKey key;
    std::unique_ptr<plume::RenderTexture> texture;
    plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t generation = 0;  // bumped when the texture is replaced
    plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
    uint64_t last_frame = 0;
    // Shader views (SampleSlots), valid for texture generation `views_generation`.
    std::unique_ptr<plume::RenderTextureView> sample_view;
    std::unique_ptr<plume::RenderTextureView> stencil_view;
    uint32_t sample_slot = 0xFFFFFFFFu;
    uint32_t stencil_slot = 0xFFFFFFFFu;
    uint32_t views_generation = 0;
    // Ownership: this identity's index in the tile map
    // (kNoOwner: none left), the tile columns of one row of its tiles (64 bpp
    // texels count two), and per aspect what the last accesses found at a map
    // epoch -- the pixels whose tiles were all its own, and the pixels a read
    // already counted.
    uint16_t owner = 0xFFFF;
    uint32_t pitch_tiles = 0;
    uint32_t columns_per_texel = 1;
    uint32_t own_epoch[2] = {};
    plume::RenderRect own_rect[2];
    uint32_t read_epoch[2] = {};
    plume::RenderRect read_rect[2];
  };

  // The aspects of an EDRAM word: kMain, the colour word whole or
  // a depth word's depth24; kStencil, the low byte -- a depth word's stencil,
  // part of a colour word.
  enum Aspect : uint32_t { kMain = 0, kStencil = 1, kAspectCount = 2 };
  // How an access treats what an aspect held: kTake replaces it in every texel
  // it touches (a clear, an opaque rect that nothing can reject); kDepend
  // writes but may keep or blend it; kRead only reads it (a resolve's source,
  // a depth or stencil test). A colour access treats both aspects alike.
  enum class Access : uint8_t { kNone, kTake, kDepend, kRead };
  struct Accesses {
    Access aspect[kAspectCount] = {Access::kNone, Access::kNone};
    bool operator==(const Accesses& o) const {
      return aspect[kMain] == o.aspect[kMain] && aspect[kStencil] == o.aspect[kStencil];
    }
  };

  EdramTargets() = default;
  ~EdramTargets();
  EdramTargets(const EdramTargets&) = delete;
  EdramTargets& operator=(const EdramTargets&) = delete;

  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap);
  // The GPU must be idle and the FrameRing drained.
  void Shutdown();

  // `slot`'s previous frame is complete: its coverage checks are read, then
  // its query pool is reset in `list` (outside any render pass).
  void BeginFrame(plume::RenderCommandList* list, uint32_t slot);

  // The detector (mcla_gpu_edram_aliasing) and its coverage checks
  // (mcla_gpu_coverage_queries, on a device that counts samples exactly).
  // Callers do no ownership work at all while aliasing() is false.
  bool aliasing() const { return aliasing_; }
  bool coverage_queries() const { return coverage_; }

  // The target of `key`, at least `height` rows tall: created (and cleared),
  // or grown with its contents copied -- recorded into `list`, which must not
  // be inside a render pass the caller still needs: when anything is recorded
  // *framebuffer_unset is set and no framebuffer is left bound. Null when the
  // host cannot make it (logged once per identity).
  Target* Get(const EdramKey& key, uint32_t height, plume::RenderCommandList* list,
              bool* framebuffer_unset);

  // The target of `key` if it exists (nothing is created).
  const Target* Find(const EdramKey& key) const {
    const auto it = targets_.find(key);
    return it == targets_.end() ? nullptr : it->second.get();
  }

  // Records a barrier moving `target` to `layout` when it is elsewhere.
  void Transition(plume::RenderCommandList* list, Target& target,
                  plume::RenderTextureLayout layout);

  // The bindless slots of `target`'s shader views: the colour or depth plane,
  // and (depth targets) the stencil plane; made on first use. False when a
  // view or a slot cannot be had (logged).
  bool SampleSlots(Target& target, uint32_t* sample_slot, uint32_t* stencil_slot);

  // Clears `rect` (clipped to the target) of a colour target to `value`
  // (already encoded, EncodeColorClear), or of a depth target's depth and/or
  // stencil; moves it to its write layout and binds a framebuffer of it alone,
  // which it returns (the caller's binding state); null on failure.
  plume::RenderFramebuffer* ClearColor(plume::RenderCommandList* list, Target& target,
                                       plume::RenderRect rect, const float value[4]);
  plume::RenderFramebuffer* ClearDepthStencil(plume::RenderCommandList* list, Target& target,
                                              plume::RenderRect rect, bool depth, bool stencil,
                                              float z, uint32_t stencil_value);

  // The framebuffer of `color_count` colour targets (slots 0..n-1) and an
  // optional depth target, created on first use. Null on failure (logged).
  plume::RenderFramebuffer* Framebuffer(Target* const* colors, uint32_t color_count,
                                        Target* depth);

  // True when `access` of `target` to the tiles of `pixels` is known to
  // change and report nothing at this map epoch, aspect by aspect (cheap: the
  // caller then skips NoteAccess). `reads_only`: every aspect accessed is
  // kRead (what is known to be read already counts).
  bool Settled(const Target& target, const plume::RenderRect& pixels, Aspect aspect,
               bool reads_only) const {
    if (tiling_) {
      return false;  // the section's accesses wait for their tiles
    }
    if (target.own_epoch[aspect] == epoch_ && Inside(pixels, target.own_rect[aspect])) {
      return true;
    }
    return reads_only && target.read_epoch[aspect] == epoch_ &&
           Inside(pixels, target.read_rect[aspect]);
  }
  bool Settled(const Target& target, const plume::RenderRect& pixels,
               const Accesses& access) const {
    for (uint32_t a = 0; a < kAspectCount; ++a) {
      if (access.aspect[a] != Access::kNone &&
          !Settled(target, pixels, Aspect(a), access.aspect[a] == Access::kRead)) {
        return false;
      }
    }
    return true;
  }
  // True when `aspect` (every aspect `access` touches) is `target`'s own,
  // unmarked, in every tile of `pixels` at this map epoch. A resolve, and the
  // depth target of a draw with colour outputs, ask this instead of Settled:
  // a read already counted is still a copy or a test of inherited bytes, whose
  // result has to be marked again (reach).
  bool SettledClean(const Target& target, const plume::RenderRect& pixels,
                    Aspect aspect) const {
    return !tiling_ && target.own_epoch[aspect] == epoch_ &&
           Inside(pixels, target.own_rect[aspect]);
  }
  bool SettledClean(const Target& target, const plume::RenderRect& pixels,
                    const Accesses& access) const {
    for (uint32_t a = 0; a < kAspectCount; ++a) {
      if (access.aspect[a] != Access::kNone && !SettledClean(target, pixels, Aspect(a))) {
        return false;
      }
    }
    return true;
  }
  // Notes an access in the tile map -- inside a tiled section,
  // keeps it for EndTiling. kTake and kDepend make the aspect `target`'s; a
  // kDepend or kRead that met an aspect another identity wrote last is counted
  // and, the first time for a pair (16 at most), reported with `event`.
  // `outputs` (a draw's colour targets, noted before its depth target): when
  // this access met another identity's bytes or marked ones -- a depth or
  // stencil test over them -- what the draw writes depends on them, and the
  // outputs' tiles inside `pixels` are marked (reach).
  void Note(Target& target, const plume::RenderRect& pixels, const Accesses& access,
            const EdramEvent& event, Target* const* outputs = nullptr,
            uint32_t output_count = 0);
  // Predicated tiling: the guest emits each draw once and
  // the GPU runs the section once per tile, the tile's rectangle moved to the
  // EDRAM's origin -- the host draws it once at full size. Inside a section
  // the accesses wait; EndTiling notes them tile by tile, in the console's
  // order, each cut by the tile and moved by its origin.
  void BeginTiling();
  void EndTiling(const EdramTile* tiles, uint32_t count);
  // Changes whenever a Note could find something else (the map's epoch, or
  // what a tiled section keeps): an access equal to the last one, with the
  // serial unchanged, changes and reports nothing.
  uint64_t serial() const {
    return tiling_ ? (uint64_t(1) << 63) | uint64_t(pending_.size()) : uint64_t(epoch_);
  }

  // Reach. The numbers (EdramEvent::resolve) of the resolves
  // found inherited since the last call: right after their Note, or at the
  // EndTiling of their section. The ResolveCache takes them.
  bool has_inherited_resolves() const { return !inherited_resolves_.empty(); }
  void TakeInheritedResolves(std::vector<uint32_t>* out) {
    out->clear();
    out->swap(inherited_resolves_);
  }
  // A reader of what an inherited resolve wrote: a fetch served by its
  // product, a texture decoded from its pages of the resolve memory, a lock
  // that hands them to the CPU. Counted; logged once per (kind, id), the
  // first 16, with describe()'s text.
  enum class Reach : uint8_t { kFetch, kDecode, kLock, kCount };
  void NoteReach(Reach kind, uint64_t id, const std::function<std::string()>& describe);
  // mcla_gpu_edram_capture: `request` asks for a RenderDoc capture of the next
  // frame (FrameCapture::Request). Called, for the kinds the cvar's bits pick,
  // on the first report of a pair and on the first reach of each kind, 4
  // times at most.
  static constexpr uint32_t kCaptureWritePair = 1;  // a write kept another identity's bytes
  static constexpr uint32_t kCaptureReadPair = 2;   // a read met them
  static constexpr uint32_t kCaptureReach = 4;      // a reader of inherited bytes
  void SetCaptureRequest(std::function<void()> request) { request_capture_ = std::move(request); }

  // Coverage checks. An access noted kTake because a draw is
  // taken to reach every sample of its rectangle -- one rect, or a quad of two
  // triangles -- is checked: an occlusion query around the draw counts the
  // samples it wrote, and once the frame's slot comes back a count under the
  // rectangle's, for a take that took another identity's tiles, is reported.
  // With no check to be had (no exact occlusion queries, or none left this
  // frame) the caller notes the draw kDepend: it may report too much, never
  // too little. With the checks off (mcla_gpu_coverage_queries) such a take
  // goes unchecked, listed once per pair. The check goes in the draw's
  // EdramEvent; Begin and End wrap its recording, the framebuffer bound.
  static constexpr uint32_t kNoCheck = 0xFFFFFFFFu;
  uint32_t ReserveCoverageCheck();
  // What the checked draw was, for the report.
  void DescribeCoverageCheck(uint32_t check, const EdramEvent& event);
  void BeginCoverageCheck(plume::RenderCommandList* list, uint32_t check,
                          uint64_t expected_samples);
  void EndCoverageCheck(plume::RenderCommandList* list, uint32_t check);

  size_t size() const { return targets_.size(); }
  std::string TakeSummary();

 private:
  struct KeyHash {
    size_t operator()(const EdramKey& k) const;
  };
  struct FramebufferKey {
    const Target* targets[5] = {};  // colour 0..3, depth
    uint32_t generations[5] = {};
    uint32_t color_count = 0;
    bool operator==(const FramebufferKey& o) const;
  };
  struct FramebufferKeyHash {
    size_t operator()(const FramebufferKey& k) const;
  };

  static constexpr uint16_t kNoOwner = 0xFFFF;
  static constexpr uint32_t kTileCount = 2048;  // xenos::kEdramTileCount
  static constexpr uint32_t kReportedPairs = 16;
  static bool Inside(const plume::RenderRect& a, const plume::RenderRect& b) {
    return a.left >= b.left && a.top >= b.top && a.right <= b.right && a.bottom <= b.bottom;
  }

  struct Inheritance {
    uint32_t tiles = 0;              // aspect-tiles another identity wrote last that the access met
    const EdramKey* from = nullptr;  // that identity, when the pair is reported for the first time
    Aspect aspect = kMain;           // where `from` was met
    uint32_t tainted = 0;            // aspect-tiles of its own, marked, that it read or kept
    bool tainted_new = false;        // ... read, by a target not reported for that yet
    uint32_t taken = 0;                    // aspect-tiles of another identity a kTake took
    uint16_t taken_owner = 0xFFFF;         // the first of them
    const EdramKey* taken_from = nullptr;  // that identity, when the take pair is new (32 at most)
  };
  struct CoverageCheck {
    uint64_t expected = 0;      // samples of the rectangle, set when the draw is recorded
    bool recorded = false;
    uint32_t taken = 0;         // aspect-tiles of other identities its takes took
    uint16_t taken_owner = 0xFFFF;
    uint16_t target = 0xFFFF;   // the identity that took them
    EdramEvent event;
  };
  struct Pending {
    Target* target = nullptr;
    plume::RenderRect rect;
    Accesses access;
    EdramEvent event;
    Target* outputs[4] = {};
    uint32_t output_count = 0;
  };

  bool Create(Target& target, uint32_t height);
  void AssignOwner(Target& target);
  // The tiles of `pixels` in `target`'s rows: columns [col0, col1), rows
  // [row0, row1); the tile is base + row * pitch_tiles + col, wrapped.
  static void TileSpan(const Target& target, const plume::RenderRect& pixels, uint32_t* col0,
                       uint32_t* col1, uint32_t* row0, uint32_t* row1);
  Inheritance Apply(Target& target, const plume::RenderRect& pixels, const Accesses& access);
  // Marks both aspects of `target`'s own tiles inside `pixels` (reach).
  void Taint(const Target& target, const plume::RenderRect& pixels);
  // Apply, the outputs' marks when it met inherited bytes, and Report.
  void Account(Target& target, const plume::RenderRect& pixels, const Accesses& access,
               const EdramEvent& event, Target* const* outputs, uint32_t output_count,
               int32_t tile);
  // Whether a kept access needs no walk at EndTiling (SettledClean for a
  // resolve or a test with outputs, Settled otherwise).
  bool SettledFor(const Pending& pending, const plume::RenderRect& pixels) const;
  // Logs what Apply found that is to be reported; `tile` < 0 outside tiling.
  // A take that took another identity's tiles is kept for its coverage check.
  void Report(const Target& target, const Accesses& access, const Inheritance& found,
              const EdramEvent& event, int32_t tile);
  void HarvestCoverage(uint32_t slot);
  // mcla_gpu_edram_capture: a capture of the next frame when the cvar's bits
  // hold `why` (kCapture*), 4 at most.
  void RequestCapture(uint32_t why);
  void Clear(Target& target, plume::RenderCommandList* list);
  void RetireFramebuffersOf(const Target* target);
  // Gives the shader views' slots back (deferred) and retires the views.
  void ReleaseViews(Target& target);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  std::unordered_map<EdramKey, std::unique_ptr<Target>, KeyHash> targets_;
  std::unordered_map<FramebufferKey, std::unique_ptr<plume::RenderFramebuffer>,
                     FramebufferKeyHash>
      framebuffers_;
  std::vector<EdramKey> failed_;  // identities reported as impossible
  uint64_t frame_ = 0;

  // Ownership: per aspect and EDRAM tile, the index of the
  // identity that wrote it last; the identities by index; the epoch, bumped
  // whenever an aspect of a tile changes hands; the (from << 16 | to) pairs
  // reported.
  std::array<std::array<uint16_t, kTileCount>, kAspectCount> tile_owner_{};
  std::vector<EdramKey> owner_keys_;
  uint32_t epoch_ = 1;
  std::vector<uint32_t> reported_pairs_;
  std::vector<uint32_t> listed_takes_;  // (from << 16 | to) of kTake over another identity
  bool owners_exhausted_logged_ = false;
  // Reach: per aspect and tile, 1 when the bytes there are, on
  // the console, partly another identity's (a write kept or blended them);
  // the inherited resolves not taken yet; the readers reported (kind << 62 |
  // id), the identities whose marked tiles a read was reported for.
  std::array<std::array<uint8_t, kTileCount>, kAspectCount> tile_taint_{};
  std::vector<uint32_t> inherited_resolves_;
  std::vector<uint64_t> reach_reported_;
  std::vector<uint16_t> tainted_reported_;
  std::array<bool, size_t(Reach::kCount)> reach_seen_{};
  std::function<void()> request_capture_;
  uint32_t capture_ = 0;  // mcla_gpu_edram_capture's bits
  uint32_t captures_ = 0;
  // Coverage checks: per frame slot, an occlusion query pool and what each
  // query checks; the capacity of the pools and what a frame wanted.
  bool aliasing_ = false;
  bool coverage_ = false;
  uint32_t slot_ = 0;
  std::vector<std::unique_ptr<plume::RenderQueryPool>> coverage_pools_;
  std::vector<std::vector<CoverageCheck>> coverage_checks_;
  uint32_t coverage_capacity_ = 0;
  uint32_t coverage_wanted_ = 0;
  std::vector<uint32_t> coverage_reported_;  // (from << 16 | to) of partial takes reported
  bool coverage_over_logged_ = false;
  // The tiled section open, and the accesses it keeps.
  bool tiling_ = false;
  std::vector<Pending> pending_;
  bool tiling_mismatch_logged_ = false;

  struct Stats {
    uint64_t created = 0;
    uint64_t grown = 0;
    uint64_t grown_bytes = 0;
    uint64_t framebuffers_created = 0;
    uint64_t barriers = 0;
    uint64_t cleared = 0;  // new textures initialized
    uint64_t walks = 0;          // NoteAccess calls that walked the tile map
    uint64_t tiles_taken = 0;    // aspect-tiles another identity wrote last, taken over (kTake)
    uint64_t depend_events = 0;  // accesses with a kDepend aspect that met another identity's
    uint64_t depend_tiles = 0;
    uint64_t read_events = 0;    // accesses whose kRead aspects did (and none kDepend did)
    uint64_t read_tiles = 0;
    uint64_t tiled_sections = 0;
    uint64_t tiled_kept = 0;     // accesses a tiled section kept (after merging repeats)
    uint64_t tiled_noted = 0;    // ... noted per tile at its end
    uint64_t checks = 0;          // coverage checks read
    uint64_t checks_full = 0;     // ... whose draw wrote every sample of its rectangle
    uint64_t checks_partial = 0;  // ... that did not
    uint64_t checks_partial_taken = 0;  // ... and had taken another identity's tiles (reported)
    uint64_t checks_over = 0;     // ... that counted more (overlapping triangles)
    uint64_t checks_unrecorded = 0;  // reserved, the draw not recorded
    uint64_t checks_wanting = 0;  // candidates with no check left (noted kDepend)
    uint64_t takes_unchecked = 0;  // draws that took another identity's tiles, unchecked
    uint64_t tainted = 0;          // aspect-tiles marked (a write kept another identity's bytes)
    uint64_t untainted = 0;        // ... taken again
    uint64_t tainted_reads = 0;    // marked aspect-tiles of its own an access read
    uint64_t inherited_resolves = 0;
    std::array<uint64_t, size_t(Reach::kCount)> reach{};
  };
  Stats stats_;
};

}  // namespace mcla::gpu
