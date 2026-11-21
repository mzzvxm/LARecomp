#pragma once
// ===========================================================================
// LARecomp city pack -- the lamps: the static table, its grid, the game's light laws and the on hours (CITY-RT M3 package G2b, shared with M1's S1)
// ===========================================================================
// WHAT THIS IS. The city's lights as a table that needs no running game: the pack's "lamps" section is built from `resources/city/sc/lights.xlm` (lamp_parse.h)
// and read back here. Every light keeps the game's own numbers and units (position in world metres, colour 0..1, intensity in the game's units, the cone's full
// angle and its spread in degrees, the axis the light points along). The table is sorted into a uniform grid of 64 m cells over x and z, so a query (the lamps
// around a point, in a box, the strongest few at a point, a cell's summed emission) reads a handful of cells and never the 15,174 lights.
//
// WHICH LIST HOLDS WHAT (the 943 against 6,298 of CITY-RT-SURVEY C13; the evidence is below, every number was read from the player's own files or from a logged
// run, nothing is a guess except what is marked [I]). The game keeps TWO populations of lights and unions them in one grid:
//   FILE LIGHTS   lights.xlm's own list (root +0x50, 15,174 lights of 96 bytes): 14,231 omni and 943 spot, all with the on byte set; intensities 20 (6,913), 10 (1,307),
//                 15 (1,093), 0 (987), 12 (978) ... up to 470; the spots have cones of 40 to 145 degrees (100 and 80 and 130 the most) and spreads 0 to 60. These are the
//                 FILL lights (shop fronts, signs, ground washes). The offline atlas decodes exactly this list.
//   STREET LAMPS  the game's own packed lamp list (the manager object's +24 list, 24-byte records, unpacked by sub_822F1B08): 6,298 lights, ALL spots, every cone 120 degrees or
//                 more (125 in each per-frame record the probe kept; the lamp logged at (885.32, 25.68, -756.95) has intensity 70, cone 125, spread -50.5), intensity 0 to 100
//                 (the probe's histogram: under 1: 375, 20 to 30: 96, 30 to 40: 41, 40 to 60: 1,969, 60 to 100: 2,409, 100: 1,408), pointing nearly straight down. NOT in
//                 lights.xlm: of the file's 943 spots none has the lamp signature (spread -50.5: 0 of 943; cone 125: 6; intensity 55: 1), and the three street lamps whose
//                 places were logged ((885.32, 25.68, -756.95), (884.68, 25.64, -736.46), (-2932.67, -13.32, 377.97)) have no file light within 13.2, 20.6 and 8.6 m (the
//                 nearest are omni fill lights of intensity 13 to 20). The night probe counted these 6,298 in the live list and the Director's atlas the 15,174 in the file:
//                 they are different lists, not two counts of one. [V: perf/out/NIGHT-PROBE.txt C4, perf/out/LAMP-BSPIKE.txt, feature-parity/DIRECTOR-ATLAS-NOTES.md 1.3b,
//                 lights.xlm read byte by byte for this file.]
//   GRID POINTERS the grid builder (sub_82303A58) is given one pointer list: the null light, then the file lights, then the street lamps: 21,537 pointers in the night
//                 probe = 15,174 + 6,298 + 65 [I: the null light and the lamp list's 64 spare slots, capacity 6,362 against 6,298]. This is why "the city has 21 thousand
//                 lights" and "lights.xlm has 15 thousand" are both true.
//   PER-FRAME LIST the street lamps (and only them: AddLight keeps type 1) that lie within 300 m of the eye and in the lamp hours: mean 64, at most 83 a night frame, none by
//                 day (vol_lamp_list.h, kListHeaderAddress 0x8288B8D8). It is a view of the street lamps, never a third population.
// So the offline table (this file) holds the FILE lights; the STREET LAMPS exist only in the running game (their offline source, the packed list the manager is given,
// is not in lights.xlm and was not found in the archive's city folder: CHECK: where it is loaded from). lamp_merge.h is the merge: the game's live lists (Director's
// ReadRuntimeLights: the object's +8 list = the file lights again, packed, and its +24 list = the street lamps) are matched against this table, a live fill light that
// is a file light keeps the file's exact record, and the street lamps are added. The merged table is what the GI's far cascades, the lamp shadows and the Director read.
//
// THE GAME'S LAWS (read from the lit shaders and sub_822FD2C8, as volumetrics_lamps.h already states; the two headers are held to each other by lamp_table_test.cpp):
//   term = colour x saturate(I / d^2) x cone(angle) x N.L x night factor      a lamp lights with its full colour out to d = sqrt(I) and falls as I / d^2 beyond; the reach
//   the mod loader quotes, sqrt(100 I), is where the term is 1 per cent. The cone is a ramp in the cosine: half angles a = cone / 2 and s = spread / 2 (0 counts as 1),
//   full at cos(a) and zero at cos(a + s) for a spread of 0 or more, full at cos(a + s) and zero at cos(a) for a negative one.
//
// THE ON HOURS. The file has no hours (its on byte is 1 for every light); two rules decide when a light shines [V: mod_glows.cpp's RE map, sub_82729438; LAMP3X.md]:
//   street lamps  the prop lamp pass lights a lamp when hour > on - c - j  or  hour < off + c + j, with on/off the lighting manager's floats (+14568 / +14564: 18 and 6 in
//                 TODSunNice), c = 0.1 h and j = (the prop's address & 0xF0) x 0.00104167 h (0 to 0.25 h: a dither so that the lamps do not all switch at once). The prop's
//                 address is not known offline: LampJitterHours is a stand-in of the same range from the lamp's place. The section carries the hours (LampTable::hours():
//                 18, 6 and 0.1 h, the defaults of LampHours, since the file has none); CHECK: the live on/off floats read from the lighting manager override them (set_hours).
//   fill lights   the lit shaders scale every grid light by ShaderGlobals.w (sm_fGlobalNightLightIntensity), the game's smooth night factor: the caller passes it; EstimateNightFactor
//                 is a stand-in ramp for tools that have no frame. CHECK: its shape against ShaderGlobals.w over a dusk.
//
// Header only for the types and the laws; lamp_table.cpp has the table. Pure: no guest, no renderer, no SDK.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace larecomp::citypack {

inline constexpr uint32_t kLampMagic = 0x504D414Cu;  // "LAMP"
inline constexpr uint32_t kLampSectionVersion = 1;
inline constexpr uint32_t kLampHeaderBytes = 96;
inline constexpr uint32_t kLampRecordBytes = 56;
inline constexpr float kDefaultLampCellMetres = 64.0f;
inline constexpr uint32_t kNoLampIndex = 0xFFFFFFFFu;
inline constexpr uint32_t kMaxLampCells = 1u << 20;      // the grid is coarsened until it has at most this many cells
inline constexpr uint32_t kMaxLamps = 1u << 20;          // a table or a section holds at most this many (the game's own limit is 32,767 in its light textures)

// The counts of the game's own lights.xlm (CITY-RT-SURVEY C13, DIRECTOR-ATLAS-NOTES 1.3, read again for this file): expectations of a census, not limits (another game build, a
// mod's lights.xlm, gives other numbers).
inline constexpr uint32_t kGoldenLamps = 15174;
inline constexpr uint32_t kGoldenLampSpots = 943;
inline constexpr uint32_t kGoldenLampOmni = 14231;
// The street lamp list of the running game (night probe, perf/out/NIGHT-PROBE.txt C4): not in the file, here for the merge's report to compare with.
inline constexpr uint32_t kGoldenStreetLamps = 6298;

enum class LampSource : uint8_t {
  kFile = 0,        // a light of lights.xlm
  kLiveFill = 1,    // a fill light the running game has and the file does not (a mod's, a different file)
  kStreetLamp = 2,  // the game's street lamp list: spots, only live
};
inline constexpr uint8_t kLampSeenLive = 1;  // Lamp::flags: a live light of the running game matched this one

// One light, in the game's units.
struct Lamp {
  float pos[3] = {0, 0, 0};
  float colour[3] = {1, 1, 1};            // rgb, 0..1 (the intensity is separate)
  float dir[3] = {0.0f, -1.0f, 0.0f};     // the axis the light points along (down, for a street lamp)
  float intensity = 0.0f;                 // the game's units: 20 a fill light, 55 to 70 a Los Angeles street lamp
  float cone_deg = 360.0f;                // the full angle of the lit cone, degrees (360: omni)
  float spread_deg = 0.0f;                // the soft edge's width, degrees (0 counts as 1; negative: the edge is inside the angle)
  uint8_t spot = 0;                       // 0 omni, 1 spot (the file's type byte)
  uint8_t on = 1;                         // the file's own on byte
  LampSource source = LampSource::kFile;
  uint8_t flags = 0;                      // kLampSeenLive
  uint32_t file_index = kNoLampIndex;     // the light's index in lights.xlm: what the Director's atlas calls `i`; kNoLampIndex for a live-only light

  bool is_spot() const { return spot != 0; }
};

// ---------------------------------------------------------------------------
// the game's laws
// ---------------------------------------------------------------------------

// The term at distance `dist` on the axis: 1 out to sqrt(I), I / d^2 beyond (the lit shaders' saturate(I / d^2)). Zero for a light without intensity.
inline float LampTerm(float intensity, float dist) {
  if (!(intensity > 0.0f)) return 0.0f;
  const float d2 = dist * dist;
  return d2 <= intensity ? 1.0f : intensity / d2;
}
// Where the term is 1 per cent (mod_lights.cpp: "20 reaches sqrt(100 x 20) = 44.7 m") and where it stops being 1.
inline float LampReach(float intensity) { return intensity > 0.0f ? std::sqrt(100.0f * intensity) : 0.0f; }
inline float LampPlateau(float intensity) { return intensity > 0.0f ? std::sqrt(intensity) : 0.0f; }

// The cone as the cosines of its two edges: `inner` where it is full, `outer` where it is gone (sub_822FD2C8). An omni light (or 360 degrees) has none (both -1).
inline void LampConeCosines(float cone_deg, float spread_deg, float* cos_outer, float* cos_inner) {
  const float kPi = 3.14159265358979323846f;
  const float half = 0.5f * std::clamp(cone_deg, 0.0f, 360.0f);
  const float s = 0.5f * (spread_deg == 0.0f ? 1.0f : spread_deg);
  float inner = half;
  float outer = half;
  if (s >= 0.0f) {
    outer = half + s;
  } else {
    inner = half + s;
  }
  inner = std::clamp(inner, 0.0f, 179.0f);
  outer = std::clamp(outer, inner + 0.1f, 179.9f);
  *cos_inner = std::cos(inner * (kPi / 180.0f));
  *cos_outer = std::cos(outer * (kPi / 180.0f));
}

// The cone's factor, 0..1, for the unit vector `to_point` from the light to the lit point. Omni lights (type 0 or 360 degrees) and a light with no axis give 1.
inline float LampConeFactor(const Lamp& l, const float to_point[3]) {
  if (!l.is_spot() || l.cone_deg >= 359.0f) return 1.0f;
  const float len = std::sqrt(l.dir[0] * l.dir[0] + l.dir[1] * l.dir[1] + l.dir[2] * l.dir[2]);
  if (len <= 1e-3f) return 1.0f;
  float cos_outer = -1.0f, cos_inner = -1.0f;
  LampConeCosines(l.cone_deg, l.spread_deg, &cos_outer, &cos_inner);
  const float c = (l.dir[0] * to_point[0] + l.dir[1] * to_point[1] + l.dir[2] * to_point[2]) / len;
  return std::clamp((c - cos_outer) / (cos_inner - cos_outer), 0.0f, 1.0f);
}

// A lamp's identity from frame to frame: its place to an eighth of a metre (street lamps do not move). The SAME number as the volumetric light's LampKey
// (volumetrics_lamps.h) and the lamp shadows' lamp_shadow::KeyOf: the three must agree, lamp_table_test.cpp holds this one to the first.
inline uint64_t LampKeyOf(const float pos[3]) {
  uint64_t h = 0x9E3779B97F4A7C15ull;
  for (int i = 0; i < 3; ++i) {
    const int64_t q = int64_t(std::floor(double(pos[i]) * 8.0));
    h ^= uint64_t(q) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  }
  return h;
}
inline uint64_t LampKeyOf(const Lamp& l) { return LampKeyOf(l.pos); }

// ---------------------------------------------------------------------------
// the on hours
// ---------------------------------------------------------------------------

struct LampHours {
  float on_hour = 18.0f;        // the lighting manager's +14568 (TODSunNice; CHECK: the live float is the truth)
  float off_hour = 6.0f;        // +14564
  float margin_hours = 0.1f;    // the prop pass's c (image 0x82004288)
};
inline constexpr float kLampJitterHoursPerStep = 0.00104167f;  // the prop pass's constant: j = (address & 0xF0) x this

// The stand-in for the prop's per-instance jitter: 0 to 0.25 h in sixteen steps, the same for a lamp every time (the game's is the prop's address).
inline float LampJitterHours(uint64_t key) {
  uint64_t z = key + 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  return float((z & 0xFu) * 16u) * kLampJitterHoursPerStep;
}

// The prop lamp pass's rule: on when the hour is after on - c - j or before off + c + j. `hour` is the time of day in hours (0..24).
inline bool StreetLampOn(const LampHours& h, float hour, float jitter_hours) {
  return hour > h.on_hour - h.margin_hours - jitter_hours || hour < h.off_hour + h.margin_hours + jitter_hours;
}

// A stand-in for the game's night factor (ShaderGlobals.w) where no frame supplies it: 1 inside the lamp hours, ramping over `ramp_hours` on either side. CHECK: shape.
inline float EstimateNightFactor(const LampHours& h, float hour, float ramp_hours = 0.5f) {
  const float r = std::max(ramp_hours, 1e-3f);
  const float evening = std::clamp((hour - (h.on_hour - r)) / r, 0.0f, 1.0f);   // rises into the on hour
  const float morning = std::clamp(((h.off_hour + r) - hour) / r, 0.0f, 1.0f);  // falls away after the off hour
  return std::max(evening, morning);
}

// How much a light shines at `hour`: street lamps by the prop pass's window (0 or 1; the game's own fade by distance is the live list's scale), every other light by the night
// factor (`night_factor` < 0: EstimateNightFactor). A light whose own on byte is clear gives 0.
inline float LampActivity(const Lamp& l, const LampHours& hours, float hour, float night_factor = -1.0f) {
  if (!l.on) return 0.0f;
  if (l.source == LampSource::kStreetLamp) return StreetLampOn(hours, hour, LampJitterHours(LampKeyOf(l))) ? 1.0f : 0.0f;
  return night_factor >= 0.0f ? std::min(night_factor, 1.0f) : EstimateNightFactor(hours, hour);
}

// ---------------------------------------------------------------------------
// the table
// ---------------------------------------------------------------------------

struct LampGrid {
  float origin_x = 0.0f;   // the lattice's own: a multiple of cell_m, so that a cell means the same place in every table
  float origin_z = 0.0f;
  float cell_m = kDefaultLampCellMetres;
  uint32_t nx = 0;
  uint32_t nz = 0;
  uint32_t cells() const { return nx * nz; }
};

// What `Strongest` asks of the lamps: when (a negative hour ignores the hours), how the lit shaders' night factor stands, and which lamp hours (null: the table's own, which the section
// carries: the defaults the builder wrote, or what a reader set from the game's lighting manager).
struct LampQuery {
  float hour = -1.0f;
  float night_factor = -1.0f;
  const LampHours* hours = nullptr;
};
struct LampPick {
  uint32_t index = 0;   // into the table
  float score = 0.0f;   // brightest colour channel x the term at the point x the cone x the activity
  float distance = 0.0f;
};

// The lamps of one grid cell as one emitter (the far cascades' coarse light): their mean position, the summed colour x intensity, and the longest reach.
struct CellEmitter {
  uint32_t count = 0;
  float centre[3] = {0, 0, 0};
  float radiance[3] = {0, 0, 0};  // sum of colour x intensity
  float reach_m = 0.0f;
};

struct LampStats {
  uint32_t count = 0;
  uint32_t spots = 0;
  uint32_t omni = 0;
  std::array<uint32_t, 3> by_source{};  // LampSource
  float intensity_max = 0.0f;
  float bounds_min[3] = {0, 0, 0};
  float bounds_max[3] = {0, 0, 0};
};

class LampTable {
 public:
  // Sorts `lamps` into the grid (cell by cell, row by row; within a cell by source, then file index, then the order given). A lamp with a position that is not finite is left
  // out (dropped() counts them). `cell_m` is coarsened (doubled) until the grid has at most kMaxLampCells cells.
  static LampTable Build(std::vector<Lamp> lamps, float cell_m = kDefaultLampCellMetres, uint32_t flags = 0);

  uint32_t size() const { return uint32_t(lamps_.size()); }
  bool empty() const { return lamps_.empty(); }
  const Lamp& operator[](uint32_t i) const { return lamps_[i]; }
  const std::vector<Lamp>& lamps() const { return lamps_; }
  const LampGrid& grid() const { return grid_; }
  const float* bounds_min() const { return bounds_min_; }
  const float* bounds_max() const { return bounds_max_; }
  uint32_t flags() const { return flags_; }
  uint32_t dropped() const { return dropped_; }

  // The lamp hours the section carries (18 and 6, the TODSunNice values, with the prop pass's margin of 0.1 h, unless a reader set the game's own: the file has no hours). Set them once
  // on the table a session owns, before it is shared; a query that has other hours passes them in LampQuery / LampSelection.
  const LampHours& hours() const { return hours_; }
  void set_hours(const LampHours& hours) { hours_ = hours; }

  // The cell of a point (false outside the grid) and the table indices [first, last) of a cell's lamps.
  bool CellOf(float x, float z, uint32_t* cx, uint32_t* cz) const;
  void CellRange(uint32_t cx, uint32_t cz, uint32_t* first, uint32_t* last) const;

  // The lamps within `radius` of (x, z) on the ground plane, of a box, or of a sphere; indices ascending. `out` is cleared first.
  void Near(float x, float z, float radius, std::vector<uint32_t>* out) const;
  void NearSphere(const float p[3], float radius, std::vector<uint32_t>* out) const;
  void InBox(const float box_min[3], const float box_max[3], std::vector<uint32_t>* out) const;

  // The `n` lamps that light the point `p` most within `radius` (3-D distance), best first (ties: the lower index). Lamps that give nothing there are left out.
  std::vector<LampPick> Strongest(const float p[3], float radius, uint32_t n, const LampQuery& query = LampQuery{}) const;

  // The emitter of one cell (a cell without lamps: count 0).
  CellEmitter EmitterOfCell(uint32_t cx, uint32_t cz) const;

  // The table index of the lamp whose file_index this is (kNoLampIndex: none).
  uint32_t FindByFileIndex(uint32_t file_index) const;

  LampStats Stats() const;

  // The section's bytes (lamp_table.cpp has the layout) and the way back. Decode is strict: the magic, the version, the sizes, the hash, a cell table that adds up and a lamp in the
  // cell it is listed under; false (with `error`) for anything else, and `out` is left alone.
  std::vector<uint8_t> Encode() const;
  static bool Decode(const uint8_t* data, size_t size, LampTable* out, std::string* error);
  uint64_t ContentHash() const;  // FNV-1a over Encode()'s bytes

 private:
  void Index();  // the cell table's helpers and the file index map, after the lamps are in place
  std::vector<Lamp> lamps_;
  std::vector<uint32_t> cell_start_;  // grid_.cells() + 1 entries
  std::vector<uint32_t> by_file_index_;
  LampGrid grid_;
  float bounds_min_[3] = {0, 0, 0};
  float bounds_max_[3] = {0, 0, 0};
  uint32_t flags_ = 0;
  uint32_t dropped_ = 0;
  LampHours hours_;
};

// Table flags (the section header's flags word).
inline constexpr uint32_t kLampTableFromMods = 1;     // the lights came from the mod archive's lights.xlm
inline constexpr uint32_t kLampTableHasLive = 2;      // the table was merged with the running game's lists (lamp_merge.h)

}  // namespace larecomp::citypack
