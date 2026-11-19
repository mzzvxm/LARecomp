#pragma once
// ===========================================================================
// MCLA GPU renderer -- sharper UI art where the game's bitmaps are magnified (prototype A, round 2)
// ===========================================================================
// The game's HUD and menus are Flash movies drawn from bitmaps made for the console's 1280x720 screen:
// the dictionary's buttons and icons are 32x32 .. 128x128 DXT5 pictures (perf/UI-SHARP.md). The renderer
// draws the UI phase into targets at the render scale, so at 1080p (scale 1.5) a texel of that art covers
// 1.5 pixels and at 4K (scale 3) three, with the bilinear filter the game asked for: edges that were one
// texel soft are three pixels soft. This is the "a little low resolution" of the owner's report. This
// prototype changes only how a magnified fetch is reconstructed, in the UI phase:
//
//   colour   Catmull-Rom instead of bilinear (the bicubic that keeps edges: 5 bilinear taps), mixed with
//            the bilinear result by the setting's strength (at most 1: full Catmull-Rom).
//   alpha    where the four texels around the sample differ in alpha by a lot (a glyph's or an icon's edge:
//            the range is above about 0.3), the alpha is pushed away from the middle of its local range by a
//            gain that follows the magnification: the ramp that was one texel wide and is now m pixels wide
//            becomes about one pixel wide again, like the edge of a distance-field glyph. The ends of the
//            range are kept (a thin stroke keeps its peak alpha), and a smooth gradient (a glow, a fade: the
//            range across one texel is small) is left as the game has it.
//
// Where it runs. Per pixel shader: the variant "ui_sharp" of the variant pack (shaderpack/variants/ui_sharp.h,
// ids kVariantUiSharp and kVariantUiSharpAlphaTest) replaces the fetch of the shader's colour bitmap by a helper
// that is given the texture's fetch slot, works out the magnification from the screen-space derivatives of the
// texel coordinate (texels a pixel spans, quad uniform) and calls the game's own tfetch2D when it is below
// kMinMagnification or when the draw did not name the slot, so a draw that does not magnify, the 1:1 copy of the
// scene the pause view starts with, and every draw at a render scale of 1 are the game's. Per draw: the Scene
// chooses the variant for the draws that are in the UI phase (after the composite marker), go into an 8-bit
// colour target drawn at a scale above kMinMagnification, whose pixel shader has the variant in the pack and
// that read at least one linearly filtered 2D colour bitmap (Decide). The slots that are such bitmaps go to the
// shader as a bit mask (UiSharpConstants::slots): a shader's depth fetch (point filtered), its environment map
// (16 bit float) or its bump map are not sharpened even though the same draw is. A draw whose key is the
// alpha test spec variant takes the twin id (the same text compiled with the alpha test and coverage). It does
// that to the key only, as the UI layer split does: the memo keeps the game's. The constants reach the shader as
// 16 bytes after the shared constants (UiSharpConstants, constants.h kUiSharpConstantsOffset).
//
// Round 1 (feat/ui-sharp, perf/out/UISHARP.txt) never took the variant for any draw: 1.3 million texture reads in 12
// windows, 0 with the variant, also for xrage_im::PS_Textured, which had the variant. Round 2 therefore (1) covers
// the "ui" category shaders the census saw, (2) drops two gates that were not needed (the colour target count: the
// variant writes oC0 as the game's shader does, and "every fetch is linear": a depth fetch is point filtered), (3) gives
// the alpha test draws the twin and (4) makes the census say, for every read, WHY the draw did not take the variant
// (Why: the first condition that failed), so that a run with the variant at 0 is not a mystery again.
//
// Settings (debug-only, live, DEBUG > FIXES): mcla_ui_sharp, 0 = off (the default: nothing changes, no draw
// picks the variant), 1 = on, up to 2 for more; mcla_ui_sharp_census logs what the UI phase samples (below).
//
// The census (mcla_ui_sharp_census). For every texture a UI-phase draw reads, per window of kCensusFrames
// guest frames: the pixel shader's identity, the texture's format, size and mips, its name and dictionary
// when the game's hooks know them, the sampler's filters and clamp modes, the target's scale, the fetch slot
// and whether it is a bitmap the variant sharpens, the key's spec variant, and the number of draws, how many of
// them magnify and how many took the variant, and the outcomes by reason.
//
// CHECK (nothing here has run in the game): (1) that the draws now take the variant (the census's "with the variant" above 0 once
// the tour turns the setting on; if not, the outcomes name the gate); (2) the pictures at 1080p and 4K: no dark or coloured fringe from
// the Catmull-Rom lobe on bitmaps with black transparent texels, no mismatch between a steepened alpha and unsteepened premultiplied colour,
// thin strokes not thinner, soft fades not hardened, 720p at --scale 1 identical; (3) the first use of the variant builds pipelines on the
// render thread (a stutter; in the multisampled pause view a draw may be left out for a frame while a builder thread works,
// Scene::MaySkipForPipeline); (4) D3D12 against Vulkan for the coarse derivatives and GatherAlpha through a swizzled view; (5) the cost
// (about 46 UI draws a frame, five taps and a gather only where magnified: expected well under 0.1 ms at 4K, not measured).
//
// This file is the pure half (ui_sharp_test.cpp): the strength's clamp and constants, the per-draw rule and
// the census's bookkeeping. ui_sharp.cpp has the two cvars.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace mcla::gpu::ui_sharp {

// mcla_ui_sharp's range. 1 is full Catmull-Rom on colour and an edge ramp of about one pixel; 2 doubles the
// edge gain (a harder edge, more aliasing).
inline constexpr double kMaxStrength = 2.0;
// Texels a pixel must stand for, as a magnification (pixels a texel covers), before the helper does
// anything but the game's own fetch. The host scales are on a grid of 0.05, so 1.25 is "at least a quarter
// bigger than the console's picture"; the helper tests the real footprint again per pixel.
inline constexpr float kMinMagnification = 1.25f;
// Guest frames in a census window (10 s at 60 fps).
inline constexpr uint32_t kCensusFrames = 600;
// The fetch slots the mask has bits for. A shader has 16 pixel fetch constants; the mask is held in a float (exact below 2^24).
inline constexpr uint32_t kMaskSlots = 16;

// The cvar's value as a strength: 0 for anything that is not a number or is not above 0, at most kMaxStrength.
inline double ClampStrength(double asked) {
  if (!(asked > 0.0)) {
    return 0.0;
  }
  return std::min(asked, kMaxStrength);
}

// The 16 bytes the variant reads after the shared constants (`float4 g_UiSharp`, byte 608).
struct UiSharpConstants {
  float colour;             // x: the share of Catmull-Rom in the colour, 0..1
  float edge_gain;          // y: times the magnification gives the alpha edge's gain; 0 leaves alpha alone
  float min_magnification;  // z: below it the game's own fetch
  float slots;              // w: the fetch slots to sharpen, bit i for slot i (an integer below 2^16 as a float)
};
static_assert(sizeof(UiSharpConstants) == 16);

inline UiSharpConstants ConstantsFor(double strength, uint32_t slots) {
  const double s = ClampStrength(strength);
  UiSharpConstants c;
  c.colour = float(std::min(s, 1.0));
  c.edge_gain = float(s);
  c.min_magnification = kMinMagnification;
  c.slots = float(slots & ((1u << kMaskSlots) - 1u));
  return c;
}

inline bool operator==(const UiSharpConstants& a, const UiSharpConstants& b) {
  return a.colour == b.colour && a.edge_gain == b.edge_gain && a.min_magnification == b.min_magnification &&
         a.slots == b.slots;
}

// Whether a fetch is a colour bitmap the helper may reconstruct: a 2D texture (not stacked, not a cube or a volume) the game filters linearly when
// it is magnified, in a format that holds colours as normalised 8 bit (or smaller) channels or blocks of them. Not a depth, not a 16 bit float
// environment map, not a normal map (DXN): sharpening those is meaningless or wrong, and the same draw may have both.
inline bool IsBitmapFormat(uint32_t xenos_texture_format) {
  switch (xenos_texture_format) {
    case 2:   // k_8
    case 3:   // k_1_5_5_5
    case 4:   // k_5_6_5
    case 6:   // k_8_8_8_8
    case 8:   // k_8_A
    case 9:   // k_8_B
    case 10:  // k_8_8
    case 14:  // k_8_8_8_8_A
    case 15:  // k_4_4_4_4
    case 18:  // k_DXT1
    case 19:  // k_DXT2_3
    case 20:  // k_DXT4_5
      return true;
    default:
      return false;
  }
}
inline bool IsBitmapFetch(bool is_texture, bool two_d, bool linear_mag, uint32_t xenos_texture_format) {
  return is_texture && two_d && linear_mag && IsBitmapFormat(xenos_texture_format);
}

// What the Scene knows about a draw when it chooses (all from the prepared key and the draw's record).
struct DrawFacts {
  bool ui_phase = false;           // after the composite marker (Scene::ui_phase_)
  bool eight_bit_colour = false;   // colour 0 is k_8_8_8_8 (or its gamma variant): the UI's and the menus' targets
  float target_scale = 1.0f;       // host pixels per guest pixel of the draw's targets
  bool has_variant = false;        // the pixel shader has the variant in the variant pack (spec variant 0)
  bool has_alpha_variant = false;  // ... and its alpha test twin
  uint32_t ps_variant = 0;         // the variant the key has so far: 0 = the game's spec variant 0, 1 = its alpha test one, 2 and up another packed variant
  uint32_t bitmap_slots = 0;       // the fetch slots that are colour bitmaps the helper may sharpen (IsBitmapFetch)
  bool hidden = false;             // the draw puts nothing on the screen (an empty scissor)
};

// Why a draw did or did not take the variant: the first condition that failed, in this order (the census prints the names).
enum class Why : uint8_t {
  kTaken,
  kNotUiPhase,
  kHidden,
  kColourNot8Bit,
  kTargetScale,       // the target is below kMinMagnification: nothing is magnified (render scale 1)
  kOtherVariant,      // the key has another packed variant (the HDR tone map, a shadow filter...)
  kNoVariant,         // the pixel shader has no ui_sharp variant in the pack
  kNoBitmap,          // no slot is a linear 2D colour bitmap
  kAlphaTestNoTwin,   // the key is the alpha test spec variant and the shader has no twin in the pack (an old pack)
  kCount,
};
inline const char* WhyText(Why why) {
  switch (why) {
    case Why::kTaken: return "taken";
    case Why::kNotUiPhase: return "not the UI phase";
    case Why::kHidden: return "hidden";
    case Why::kColourNot8Bit: return "colour not 8 bit";
    case Why::kTargetScale: return "target below the minimum scale";
    case Why::kOtherVariant: return "another variant";
    case Why::kNoVariant: return "no variant in the pack for the shader";
    case Why::kNoBitmap: return "no linear colour bitmap";
    case Why::kAlphaTestNoTwin: return "alpha test and no twin";
    default: return "?";
  }
}

struct Decision {
  Why why = Why::kNotUiPhase;
  bool alpha_test = false;  // taken: with the alpha test twin (ps_variant kVariantUiSharpAlphaTest) rather than id 10
  bool taken() const { return why == Why::kTaken; }
};

// Whether the draw takes the variant, and why not. The strength is not asked here: the caller takes the draw only when it is above 0 (the census
// evaluates the same rule with the setting off, to say what would happen).
inline Decision Decide(const DrawFacts& f) {
  Decision d;
  if (!f.ui_phase) { d.why = Why::kNotUiPhase; return d; }
  if (f.hidden) { d.why = Why::kHidden; return d; }
  if (!f.eight_bit_colour) { d.why = Why::kColourNot8Bit; return d; }
  if (!(f.target_scale >= kMinMagnification)) { d.why = Why::kTargetScale; return d; }
  if (f.ps_variant >= 2) { d.why = Why::kOtherVariant; return d; }
  if (!f.has_variant) { d.why = Why::kNoVariant; return d; }
  if (f.bitmap_slots == 0) { d.why = Why::kNoBitmap; return d; }
  if (f.ps_variant == 1) {
    if (!f.has_alpha_variant) { d.why = Why::kAlphaTestNoTwin; return d; }
    d.alpha_test = true;
  }
  d.why = Why::kTaken;
  return d;
}

// ---- the census ----------------------------------------------------------------------------------------------

// One texture a UI-phase draw reads, as the Scene sees it.
struct CensusSample {
  uint64_t ps = 0;                // the pixel shader's identity
  std::string format;             // the texture's format ("k_DXT4_5")
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t levels = 1;
  uint32_t base_page = 0;         // physical page of the base level, to tell two textures of one size apart in a log
  std::string name;               // the game's name for it ("shared_24.dds"), empty when no hook saw it
  std::string dictionary;         // ... and its dictionary's file stem ("shared_latin")
  uint8_t mag_filter = 0;         // xenos::TextureFilter of the fetch constant: 0 point, 1 linear
  uint8_t min_filter = 0;
  uint8_t mip_filter = 0;
  uint8_t clamp_x = 0;            // xenos::ClampMode
  uint8_t clamp_y = 0;
  uint8_t aniso = 0;
  int32_t lod_bias = 0;           // 1/32 levels
  float target_scale = 1.0f;      // the draw's target's scale
  uint32_t target_format = 0;     // the colour 0 format number of the draw's target
  uint32_t slot = 0;              // the fetch slot of the read
  bool bitmap = false;            // the slot is a colour bitmap the helper sharpens (IsBitmapFetch)
  uint32_t ps_variant = 0;        // the key's spec variant before the choice (0, 1; 2 and up another variant)
  Why why = Why::kNotUiPhase;     // the draw's outcome
  bool took_variant = false;      // the draw drew with the UI sharpening variant
};

class Census {
 public:
  void Note(const CensusSample& s) {
    char buffer[200];
    std::snprintf(buffer, sizeof(buffer), "%016llX|%s|%ux%u|%u|%u%u%u|%u%u|%u|%d|%u|%u",
                  static_cast<unsigned long long>(s.ps), s.format.c_str(), s.width, s.height, s.levels,
                  s.mag_filter, s.min_filter, s.mip_filter, s.clamp_x, s.clamp_y, s.aniso, s.lod_bias, s.slot,
                  s.ps_variant);
    Entry& e = entries_[buffer];
    if (e.draws == 0) {
      e.first = s;
    }
    ++e.draws;
    if (s.target_scale >= kMinMagnification) {
      ++e.magnified;
    }
    if (s.took_variant) {
      ++e.variant_draws;
    }
    ++e.by_why[size_t(s.why)];
    ++why_total_[size_t(s.why)];
    e.last_scale = s.target_scale;
  }

  // A guest frame ended.
  void OnFrame() { ++frames_; }
  uint32_t frames() const { return frames_; }
  bool Due() const { return frames_ >= kCensusFrames; }
  bool empty() const { return entries_.empty(); }

  // "taken 12, no linear colour bitmap 400": the non-zero counts, the most first.
  static std::string WhyList(const std::array<uint64_t, size_t(Why::kCount)>& counts) {
    std::vector<std::pair<uint64_t, size_t>> rows;
    for (size_t i = 0; i < counts.size(); ++i) {
      if (counts[i] != 0) {
        rows.push_back({counts[i], i});
      }
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
    std::string text;
    for (const auto& [count, index] : rows) {
      if (!text.empty()) {
        text += ", ";
      }
      text += WhyText(Why(index));
      text += " " + std::to_string(count);
    }
    return text.empty() ? std::string("none") : text;
  }

  // The window's lines, the most drawn first (at most `limit`), then the counters reset. The first line
  // says what the window held, the second the outcomes of all its reads; one more per texture and pixel shader.
  std::vector<std::string> TakeReport(size_t limit = 48) {
    struct Row {
      const std::string* key;
      const Entry* entry;
    };
    std::vector<Row> rows;
    uint64_t draws = 0;
    uint64_t magnified = 0;
    uint64_t taken = 0;
    for (const auto& [key, entry] : entries_) {
      rows.push_back({&key, &entry});
      draws += entry.draws;
      magnified += entry.magnified;
      taken += entry.variant_draws;
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
      return a.entry->draws != b.entry->draws ? a.entry->draws > b.entry->draws : *a.key < *b.key;
    });
    std::vector<std::string> lines;
    char head[200];
    std::snprintf(head, sizeof(head),
                  "ui sharp census: window of %u frames, %zu (shader, texture) pairs, %llu texture reads in the UI phase, "
                  "%llu at a target scale of at least %.2f, %llu of them took the variant",
                  frames_, rows.size(), static_cast<unsigned long long>(draws),
                  static_cast<unsigned long long>(magnified), double(kMinMagnification),
                  static_cast<unsigned long long>(taken));
    lines.emplace_back(head);
    lines.emplace_back("ui sharp census:   outcomes of the reads (the first condition that failed; 'taken' is what the rule says, the variant is drawn only while mcla_ui_sharp is above 0): " +
                       WhyList(why_total_));
    for (size_t i = 0; i < rows.size() && i < limit; ++i) {
      const Entry& e = *rows[i].entry;
      const CensusSample& s = e.first;
      char line[700];
      std::snprintf(line, sizeof(line),
                    "ui sharp census:   PS %016llX reads %s %ux%u %u level(s) @%08x %s%s%s%s, mag/min/mip filter %u/%u/%u, "
                    "clamp %u/%u, aniso %u, lod bias %d/32; %llu reads x%u frames, %llu magnified, %llu with the variant, "
                    "target scale %.2f, target format %u, slot %u %s, spec variant %u",
                    static_cast<unsigned long long>(s.ps), s.format.c_str(), s.width, s.height, s.levels,
                    s.base_page << 12, s.name.empty() ? "unnamed" : "named ", s.name.c_str(),
                    s.dictionary.empty() ? "" : " in ", s.dictionary.c_str(), s.mag_filter, s.min_filter,
                    s.mip_filter, s.clamp_x, s.clamp_y, s.aniso, s.lod_bias,
                    static_cast<unsigned long long>(e.draws), frames_,
                    static_cast<unsigned long long>(e.magnified), static_cast<unsigned long long>(e.variant_draws),
                    double(e.last_scale), s.target_format, s.slot, s.bitmap ? "bitmap" : "not a bitmap", s.ps_variant);
      std::string text = line;
      text += "; outcomes: " + WhyList(e.by_why);
      lines.push_back(std::move(text));
    }
    if (rows.size() > limit) {
      lines.emplace_back("ui sharp census:   (" + std::to_string(rows.size() - limit) + " more pairs, fewer draws each)");
    }
    entries_.clear();
    why_total_.fill(0);
    frames_ = 0;
    return lines;
  }

 private:
  struct Entry {
    CensusSample first;
    uint64_t draws = 0;
    uint64_t magnified = 0;
    uint64_t variant_draws = 0;
    float last_scale = 1.0f;
    std::array<uint64_t, size_t(Why::kCount)> by_why{};
  };
  std::map<std::string, Entry> entries_;
  std::array<uint64_t, size_t(Why::kCount)> why_total_{};
  uint32_t frames_ = 0;
};

// The cvars (ui_sharp.cpp): the strength, clamped, and whether the census runs. Any thread.
double Strength();
bool CensusOn();

}  // namespace mcla::gpu::ui_sharp
