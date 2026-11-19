#pragma once
// ===========================================================================
// MCLA GPU renderer -- sharper UI art where the game's bitmaps are magnified (prototype A)
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
// id kVariantUiSharp) replaces every tfetch2D of the shader by a helper that works out the magnification
// from the screen-space derivatives of the texel coordinate (texels a pixel spans, quad uniform) and calls the
// game's own tfetch2D when it is below kMinMagnification, so a draw that does not magnify, the 1:1 copy of the
// scene the pause view starts with, and every draw at a render scale of 1 are the game's. Per draw: the
// Scene chooses the variant for the draws that are in the UI phase (after the composite marker), go into an
// 8-bit colour target drawn at a scale above kMinMagnification, read only linearly filtered textures, whose
// pixel shader has the variant in the pack and would otherwise draw with the game's own spec variant 0
// (Scene::UiSharpDraw). It does that to the key only, as the UI layer split does: the memo keeps the game's.
// The strength reaches the shader as 16 bytes after the shared constants (UiSharpConstants, constants.h
// kUiSharpConstantsOffset).
//
// Settings (debug-only, live, DEBUG > FIXES): mcla_ui_sharp, 0 = off (the default: nothing changes, no draw
// picks the variant), 1 = on, up to 2 for more; mcla_ui_sharp_census logs what the UI phase samples (below).
//
// The census (mcla_ui_sharp_census). For every texture a UI-phase draw reads, per window of kCensusFrames
// guest frames: the pixel shader's identity, the texture's format, size and mips, its name and dictionary
// when the game's hooks know them, the sampler's filters and clamp modes, the target's scale, and the
// number of draws, how many of them magnify and how many took the variant. It is what the survey in
// perf/UI-SHARP.md could not read from the existing logs: which shaders the Flash player draws with (the
// table kUiSharpShaders in shaderpack/variants/ui_sharp.h lists what is guessed), which textures are the
// glyph atlases, and how large they are.
//
// CHECK (nothing here has run in the game): (1) that the Flash player's textured draws really use the xrage_im shaders the variant lists (the census's
// PS column says); (2) the pictures at 1080p and 4K: no dark or coloured fringe from the Catmull-Rom lobe on bitmaps with black transparent texels, no
// mismatch between a steepened alpha and unsteepened premultiplied colour, thin strokes not thinner, soft fades not hardened, 720p identical; (3) the first
// use of the variant builds pipelines on the render thread (a stutter; in the multisampled pause view a draw may be left out for a frame while a builder
// thread works, Scene::MaySkipForPipeline); (4) D3D12 against Vulkan for the coarse derivatives and GatherAlpha through a swizzled view; (5) the cost
// (about 46 UI draws a frame, five taps and a gather only where magnified: expected well under 0.1 ms at 4K, not measured).
//
// This file is the pure half (ui_sharp_test.cpp): the strength's clamp and constants, the per-draw rule and
// the census's bookkeeping. ui_sharp.cpp has the two cvars.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
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
  float reserved;
};
static_assert(sizeof(UiSharpConstants) == 16);

inline UiSharpConstants ConstantsFor(double strength) {
  const double s = ClampStrength(strength);
  UiSharpConstants c;
  c.colour = float(std::min(s, 1.0));
  c.edge_gain = float(s);
  c.min_magnification = kMinMagnification;
  c.reserved = 0.0f;
  return c;
}

inline bool operator==(const UiSharpConstants& a, const UiSharpConstants& b) {
  return a.colour == b.colour && a.edge_gain == b.edge_gain && a.min_magnification == b.min_magnification &&
         a.reserved == b.reserved;
}

// What the Scene knows about a draw when it chooses (all from the prepared key and the draw's record).
struct DrawFacts {
  bool ui_phase = false;           // after the composite marker (Scene::ui_phase_)
  uint32_t color_count = 0;        // colour targets of the draw
  bool eight_bit_colour = false;   // colour 0 is k_8_8_8_8 (or its gamma variant): the UI's and the menus' targets
  float target_scale = 1.0f;       // host pixels per guest pixel of the draw's targets
  bool has_variant = false;        // the pixel shader has the variant in the variant pack
  uint32_t ps_variant = 0;         // the variant the key has so far: 0 = the game's own spec variant 0
  bool reads_textures = false;     // the pixel shader fetches at least one 2D texture
  bool all_linear = false;         // ... and every one of them is filtered linearly when magnified
  bool hidden = false;             // the draw puts nothing on the screen (an empty scissor)
};

// Whether the draw takes the variant. The strength is the clamped cvar.
inline bool Wants(double strength, const DrawFacts& f) {
  return strength > 0.0 && f.ui_phase && f.color_count == 1 && f.eight_bit_colour &&
         f.target_scale >= kMinMagnification && f.has_variant && f.ps_variant == 0 && f.reads_textures &&
         f.all_linear && !f.hidden;
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
  bool took_variant = false;      // the draw drew with the UI sharpening variant
};

class Census {
 public:
  void Note(const CensusSample& s) {
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "%016llX|%s|%ux%u|%u|%u%u%u|%u%u|%u|%d",
                  static_cast<unsigned long long>(s.ps), s.format.c_str(), s.width, s.height, s.levels,
                  s.mag_filter, s.min_filter, s.mip_filter, s.clamp_x, s.clamp_y, s.aniso, s.lod_bias);
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
    e.last_scale = s.target_scale;
  }

  // A guest frame ended.
  void OnFrame() { ++frames_; }
  uint32_t frames() const { return frames_; }
  bool Due() const { return frames_ >= kCensusFrames; }
  bool empty() const { return entries_.empty(); }

  // The window's lines, the most drawn first (at most `limit`), then the counters reset. The first line
  // says what the window held; one more per texture and pixel shader.
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
    for (size_t i = 0; i < rows.size() && i < limit; ++i) {
      const Entry& e = *rows[i].entry;
      const CensusSample& s = e.first;
      char line[600];
      std::snprintf(line, sizeof(line),
                    "ui sharp census:   PS %016llX reads %s %ux%u %u level(s) @%08x %s%s%s%s, mag/min/mip filter %u/%u/%u, "
                    "clamp %u/%u, aniso %u, lod bias %d/32; %llu reads x%u frames, %llu magnified, %llu with the variant, "
                    "target scale %.2f, target format %u",
                    static_cast<unsigned long long>(s.ps), s.format.c_str(), s.width, s.height, s.levels,
                    s.base_page << 12, s.name.empty() ? "unnamed" : "named ", s.name.c_str(),
                    s.dictionary.empty() ? "" : " in ", s.dictionary.c_str(), s.mag_filter, s.min_filter,
                    s.mip_filter, s.clamp_x, s.clamp_y, s.aniso, s.lod_bias,
                    static_cast<unsigned long long>(e.draws), frames_,
                    static_cast<unsigned long long>(e.magnified), static_cast<unsigned long long>(e.variant_draws),
                    double(e.last_scale), s.target_format);
      lines.emplace_back(line);
    }
    if (rows.size() > limit) {
      lines.emplace_back("ui sharp census:   (" + std::to_string(rows.size() - limit) + " more pairs, fewer draws each)");
    }
    entries_.clear();
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
  };
  std::map<std::string, Entry> entries_;
  uint32_t frames_ = 0;
};

// The cvars (ui_sharp.cpp): the strength, clamped, and whether the census runs. Any thread.
double Strength();
bool CensusOn();

}  // namespace mcla::gpu::ui_sharp
