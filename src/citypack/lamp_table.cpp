#include "citypack/lamp_table.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <numeric>

#include "citypack/bytes.h"

namespace larecomp::citypack {

namespace {

constexpr float kCellEpsilon = 1e-3f;  // metres a lamp may lie outside its cell's edge when a section is read back (the cell index is a float computation)

// The cell a coordinate falls in, as a plain number that cannot overflow (a query with a huge radius is a legal question): clamped far outside any grid.
int32_t CellIndex(float v, float origin, float cell) {
  const double q = std::floor((double(v) - double(origin)) / double(cell));
  return int32_t(std::clamp(q, -1.0e9, 1.0e9));
}

// The lamp's own numbers as bit patterns: a total order for lights that share a cell, a source and a file index (the live ones have none), so that a build does not depend on the
// order the lights arrive in.
std::array<uint32_t, 18> TieKey(const Lamp& l) {
  std::array<uint32_t, 18> k{};
  size_t n = 0;
  const auto put = [&](float v) { k[n++] = std::bit_cast<uint32_t>(v); };
  for (const float v : l.pos) put(v);
  put(l.intensity);
  for (const float v : l.colour) put(v);
  for (const float v : l.dir) put(v);
  put(l.cone_deg);
  put(l.spread_deg);
  k[n++] = uint32_t(l.spot) | (uint32_t(l.on) << 8) | (uint32_t(l.flags) << 16);
  return k;
}

bool FinitePosition(const float p[3]) {
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(p[i]) || std::fabs(p[i]) > 1.0e6f) return false;
  }
  return true;
}

// The grid over a set of lamps: the lattice is the cell size's own (the origin a multiple of it), coarsened until it is at most kMaxLampCells cells.
LampGrid GridFor(const float bmin[3], const float bmax[3], float cell_m) {
  LampGrid g;
  g.cell_m = std::max(cell_m, 1.0f);
  for (;;) {
    g.origin_x = std::floor(bmin[0] / g.cell_m) * g.cell_m;
    g.origin_z = std::floor(bmin[2] / g.cell_m) * g.cell_m;
    const int64_t nx = int64_t(CellIndex(bmax[0], g.origin_x, g.cell_m)) + 1;
    const int64_t nz = int64_t(CellIndex(bmax[2], g.origin_z, g.cell_m)) + 1;
    if (nx * nz <= int64_t(kMaxLampCells)) {
      g.nx = uint32_t(nx);
      g.nz = uint32_t(nz);
      return g;
    }
    g.cell_m *= 2.0f;
  }
}

}  // namespace

LampTable LampTable::Build(std::vector<Lamp> lamps, float cell_m, uint32_t flags) {
  LampTable t;
  t.flags_ = flags;
  std::vector<Lamp> kept;
  kept.reserve(lamps.size());
  for (const Lamp& l : lamps) {
    if (kept.size() >= kMaxLamps || !FinitePosition(l.pos)) {
      ++t.dropped_;
      continue;
    }
    kept.push_back(l);
  }
  lamps.clear();
  lamps.shrink_to_fit();
  if (kept.empty()) {
    t.grid_.cell_m = std::max(cell_m, 1.0f);
    t.cell_start_.assign(1, 0);
    return t;
  }
  for (int a = 0; a < 3; ++a) {
    t.bounds_min_[a] = t.bounds_max_[a] = kept[0].pos[a];
  }
  for (const Lamp& l : kept) {
    for (int a = 0; a < 3; ++a) {
      t.bounds_min_[a] = std::min(t.bounds_min_[a], l.pos[a]);
      t.bounds_max_[a] = std::max(t.bounds_max_[a], l.pos[a]);
    }
  }
  t.grid_ = GridFor(t.bounds_min_, t.bounds_max_, cell_m);

  // cell by cell, row by row; inside a cell the source, then the file index, then the light's own numbers (a total order: the table is the same whatever order the lights came in)
  std::vector<uint32_t> cell_of(kept.size());
  for (size_t i = 0; i < kept.size(); ++i) {
    const int32_t cx = std::clamp(CellIndex(kept[i].pos[0], t.grid_.origin_x, t.grid_.cell_m), 0, int32_t(t.grid_.nx) - 1);
    const int32_t cz = std::clamp(CellIndex(kept[i].pos[2], t.grid_.origin_z, t.grid_.cell_m), 0, int32_t(t.grid_.nz) - 1);
    cell_of[i] = uint32_t(cz) * t.grid_.nx + uint32_t(cx);
  }
  std::vector<uint32_t> order(kept.size());
  std::iota(order.begin(), order.end(), 0u);
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    if (cell_of[a] != cell_of[b]) return cell_of[a] < cell_of[b];
    if (kept[a].source != kept[b].source) return uint8_t(kept[a].source) < uint8_t(kept[b].source);
    if (kept[a].file_index != kept[b].file_index) return kept[a].file_index < kept[b].file_index;
    const auto ka = TieKey(kept[a]), kb = TieKey(kept[b]);
    if (ka != kb) return ka < kb;
    return a < b;  // identical lights: the order given (they are interchangeable)
  });
  t.lamps_.reserve(kept.size());
  t.cell_start_.assign(size_t(t.grid_.cells()) + 1, 0);
  for (const uint32_t i : order) {
    t.lamps_.push_back(kept[i]);
    ++t.cell_start_[size_t(cell_of[i]) + 1];
  }
  for (size_t c = 1; c < t.cell_start_.size(); ++c) t.cell_start_[c] += t.cell_start_[c - 1];
  t.Index();
  return t;
}

void LampTable::Index() {
  by_file_index_.clear();
  uint32_t top = 0;
  bool any = false;
  for (const Lamp& l : lamps_) {
    if (l.file_index < kMaxLamps) {
      top = std::max(top, l.file_index);
      any = true;
    }
  }
  if (!any) return;
  by_file_index_.assign(size_t(top) + 1, kNoLampIndex);
  for (uint32_t i = 0; i < lamps_.size(); ++i) {
    const uint32_t f = lamps_[i].file_index;
    if (f < kMaxLamps && by_file_index_[f] == kNoLampIndex) by_file_index_[f] = i;  // the first in table order
  }
}

uint32_t LampTable::FindByFileIndex(uint32_t file_index) const {
  return file_index < by_file_index_.size() ? by_file_index_[file_index] : kNoLampIndex;
}

bool LampTable::CellOf(float x, float z, uint32_t* cx, uint32_t* cz) const {
  if (lamps_.empty() || !std::isfinite(x) || !std::isfinite(z)) return false;
  const int32_t ix = CellIndex(x, grid_.origin_x, grid_.cell_m);
  const int32_t iz = CellIndex(z, grid_.origin_z, grid_.cell_m);
  if (ix < 0 || iz < 0 || ix >= int32_t(grid_.nx) || iz >= int32_t(grid_.nz)) return false;
  *cx = uint32_t(ix);
  *cz = uint32_t(iz);
  return true;
}

void LampTable::CellRange(uint32_t cx, uint32_t cz, uint32_t* first, uint32_t* last) const {
  if (cx >= grid_.nx || cz >= grid_.nz) {
    *first = *last = 0;
    return;
  }
  const size_t id = size_t(cz) * grid_.nx + cx;
  *first = cell_start_[id];
  *last = cell_start_[id + 1];
}

void LampTable::InBox(const float box_min[3], const float box_max[3], std::vector<uint32_t>* out) const {
  out->clear();
  if (lamps_.empty()) return;
  for (int a = 0; a < 3; ++a) {
    if (!(box_min[a] <= box_max[a])) return;  // also false for a NaN
  }
  int32_t x0 = CellIndex(box_min[0] - kCellEpsilon, grid_.origin_x, grid_.cell_m), x1 = CellIndex(box_max[0] + kCellEpsilon, grid_.origin_x, grid_.cell_m);
  int32_t z0 = CellIndex(box_min[2] - kCellEpsilon, grid_.origin_z, grid_.cell_m), z1 = CellIndex(box_max[2] + kCellEpsilon, grid_.origin_z, grid_.cell_m);
  x0 = std::max(x0, 0);
  z0 = std::max(z0, 0);
  x1 = std::min(x1, int32_t(grid_.nx) - 1);
  z1 = std::min(z1, int32_t(grid_.nz) - 1);
  for (int32_t cz = z0; cz <= z1; ++cz) {
    for (int32_t cx = x0; cx <= x1; ++cx) {
      uint32_t first = 0, last = 0;
      CellRange(uint32_t(cx), uint32_t(cz), &first, &last);
      for (uint32_t i = first; i < last; ++i) {
        const Lamp& l = lamps_[i];
        if (l.pos[0] >= box_min[0] && l.pos[0] <= box_max[0] && l.pos[1] >= box_min[1] && l.pos[1] <= box_max[1] && l.pos[2] >= box_min[2] && l.pos[2] <= box_max[2]) out->push_back(i);
      }
    }
  }
}

void LampTable::Near(float x, float z, float radius, std::vector<uint32_t>* out) const {
  out->clear();
  if (lamps_.empty() || !(radius >= 0.0f) || !std::isfinite(x) || !std::isfinite(z) || !std::isfinite(radius)) return;
  const float lo[3] = {x - radius, bounds_min_[1], z - radius};
  const float hi[3] = {x + radius, bounds_max_[1], z + radius};
  InBox(lo, hi, out);  // the square, then the circle
  const double r2 = double(radius) * double(radius);
  size_t kept = 0;
  for (const uint32_t i : *out) {
    const double dx = double(lamps_[i].pos[0]) - double(x), dz = double(lamps_[i].pos[2]) - double(z);
    if (dx * dx + dz * dz <= r2) (*out)[kept++] = i;
  }
  out->resize(kept);
}

void LampTable::NearSphere(const float p[3], float radius, std::vector<uint32_t>* out) const {
  out->clear();
  if (lamps_.empty() || !(radius >= 0.0f) || !FinitePosition(p) || !std::isfinite(radius)) return;
  const float lo[3] = {p[0] - radius, p[1] - radius, p[2] - radius};
  const float hi[3] = {p[0] + radius, p[1] + radius, p[2] + radius};
  InBox(lo, hi, out);
  const double r2 = double(radius) * double(radius);
  size_t kept = 0;
  for (const uint32_t i : *out) {
    const double dx = double(lamps_[i].pos[0]) - double(p[0]), dy = double(lamps_[i].pos[1]) - double(p[1]), dz = double(lamps_[i].pos[2]) - double(p[2]);
    if (dx * dx + dy * dy + dz * dz <= r2) (*out)[kept++] = i;
  }
  out->resize(kept);
}

std::vector<LampPick> LampTable::Strongest(const float p[3], float radius, uint32_t n, const LampQuery& query) const {
  std::vector<LampPick> picks;
  if (n == 0) return picks;
  std::vector<uint32_t> near;
  NearSphere(p, radius, &near);
  picks.reserve(near.size());
  for (const uint32_t i : near) {
    const Lamp& l = lamps_[i];
    const float d[3] = {p[0] - l.pos[0], p[1] - l.pos[1], p[2] - l.pos[2]};
    const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float to_point[3] = {0.0f, -1.0f, 0.0f};  // at the lamp itself: straight down, the common axis
    if (dist > 1e-4f) {
      for (int a = 0; a < 3; ++a) to_point[a] = d[a] / dist;
    }
    const float activity = query.hour < 0.0f ? (l.on ? 1.0f : 0.0f) : LampActivity(l, query.hours != nullptr ? *query.hours : hours_, query.hour, query.night_factor);
    const float score = std::max({l.colour[0], l.colour[1], l.colour[2], 0.0f}) * LampTerm(l.intensity, dist) * LampConeFactor(l, to_point) * activity;
    if (score > 0.0f) picks.push_back({i, score, dist});
  }
  const auto better = [](const LampPick& a, const LampPick& b) { return a.score != b.score ? a.score > b.score : a.index < b.index; };
  if (picks.size() > n) {
    std::partial_sort(picks.begin(), picks.begin() + n, picks.end(), better);
    picks.resize(n);
  } else {
    std::sort(picks.begin(), picks.end(), better);
  }
  return picks;
}

CellEmitter LampTable::EmitterOfCell(uint32_t cx, uint32_t cz) const {
  CellEmitter e;
  uint32_t first = 0, last = 0;
  CellRange(cx, cz, &first, &last);
  double centre[3] = {0, 0, 0};
  for (uint32_t i = first; i < last; ++i) {
    const Lamp& l = lamps_[i];
    ++e.count;
    for (int a = 0; a < 3; ++a) {
      centre[a] += double(l.pos[a]);
      e.radiance[a] += std::max(l.colour[a], 0.0f) * std::max(l.intensity, 0.0f);
    }
    e.reach_m = std::max(e.reach_m, LampReach(l.intensity));
  }
  if (e.count != 0) {
    for (int a = 0; a < 3; ++a) e.centre[a] = float(centre[a] / double(e.count));
  }
  return e;
}

LampStats LampTable::Stats() const {
  LampStats s;
  s.count = size();
  for (int a = 0; a < 3; ++a) {
    s.bounds_min[a] = bounds_min_[a];
    s.bounds_max[a] = bounds_max_[a];
  }
  for (const Lamp& l : lamps_) {
    (l.is_spot() ? s.spots : s.omni) += 1;
    if (uint8_t(l.source) < s.by_source.size()) s.by_source[size_t(l.source)] += 1;
    s.intensity_max = std::max(s.intensity_max, l.intensity);
  }
  return s;
}

// ---------------------------------------------------------------------------
// the section
// ---------------------------------------------------------------------------
//   header (kLampHeaderBytes, 96):
//     +0 u32 magic "LAMP"   +4 u32 version   +8 u32 lamp count   +12 u32 record bytes (56)   +16 u32 nx   +20 u32 nz   +24 u32 table flags   +28 u32 dropped (information)
//     +32 f32 grid origin x   +36 f32 origin z   +40 f32 cell metres   +44 u32 cell table bytes   +48 f32 bounds min xyz   +60 f32 bounds max xyz
//     +72 u32 offset of the records from the section's start (16 aligned)   +76 f32 lamps on hour   +80 u64 body hash (FNV-1a over every byte after the header)
//     +88 f32 lamps off hour   +92 f32 the margin c, hours (the defaults of LampHours: the file has none of its own)
//   cell table: (nx * nz + 1) u32, the first lamp of each cell (row by row, x fastest) and the end
//   records, 56 bytes each, in the table's order: f32 position xyz, f32 intensity, f32 colour rgb, f32 cone degrees, f32 direction xyz, f32 spread degrees,
//   u8 type (1 spot), u8 on, u8 source, u8 flags, u32 file index (0xFFFFFFFF none)

std::vector<uint8_t> LampTable::Encode() const {
  const uint32_t cells = grid_.cells();
  const uint32_t table_bytes = uint32_t((size_t(cells) + 1) * 4);
  const uint32_t records_offset = (kLampHeaderBytes + table_bytes + 15u) & ~15u;
  ByteWriter body;
  for (const uint32_t v : cell_start_) body.U32(v);
  if (cell_start_.empty()) body.U32(0);  // an empty table still has the one entry
  body.Zeros(records_offset - kLampHeaderBytes - table_bytes);
  for (const Lamp& l : lamps_) {
    for (const float v : l.pos) body.F32(v);
    body.F32(l.intensity);
    for (const float v : l.colour) body.F32(v);
    body.F32(l.cone_deg);
    for (const float v : l.dir) body.F32(v);
    body.F32(l.spread_deg);
    body.U8(l.spot);
    body.U8(l.on);
    body.U8(uint8_t(l.source));
    body.U8(l.flags);
    body.U32(l.file_index);
  }
  ByteWriter w;
  w.U32(kLampMagic);
  w.U32(kLampSectionVersion);
  w.U32(size());
  w.U32(kLampRecordBytes);
  w.U32(grid_.nx);
  w.U32(grid_.nz);
  w.U32(flags_);
  w.U32(dropped_);
  w.F32(grid_.origin_x);
  w.F32(grid_.origin_z);
  w.F32(grid_.cell_m);
  w.U32(table_bytes);
  for (const float v : bounds_min_) w.F32(v);
  for (const float v : bounds_max_) w.F32(v);
  w.U32(records_offset);
  w.F32(hours_.on_hour);
  w.U64(Fnv1a64(body.bytes().data(), body.size()));
  w.F32(hours_.off_hour);
  w.F32(hours_.margin_hours);
  w.Zeros(kLampHeaderBytes - w.size());
  w.Bytes(body.bytes().data(), body.size());
  return std::move(w.bytes());
}

uint64_t LampTable::ContentHash() const {
  const std::vector<uint8_t> bytes = Encode();
  return Fnv1a64(bytes.data(), bytes.size());
}

bool LampTable::Decode(const uint8_t* data, size_t size, LampTable* out, std::string* error) {
  const auto fail = [&](const std::string& why) {
    if (error) *error = "the lamp section " + why;
    return false;
  };
  if (size < kLampHeaderBytes) return fail("is shorter than its header");
  ByteReader r(data, size);
  if (r.U32() != kLampMagic) return fail("does not start with the lamp magic");
  const uint32_t version = r.U32();
  if (version != kLampSectionVersion) return fail("is version " + std::to_string(version) + ", this build reads " + std::to_string(kLampSectionVersion));
  const uint32_t count = r.U32();
  const uint32_t record_bytes = r.U32();
  LampTable t;
  t.grid_.nx = r.U32();
  t.grid_.nz = r.U32();
  t.flags_ = r.U32();
  t.dropped_ = r.U32();
  t.grid_.origin_x = r.F32();
  t.grid_.origin_z = r.F32();
  t.grid_.cell_m = r.F32();
  const uint32_t table_bytes = r.U32();
  float stored_min[3], stored_max[3];
  for (float& v : stored_min) v = r.F32();
  for (float& v : stored_max) v = r.F32();
  (void)stored_min;
  (void)stored_max;  // informational: the table recomputes its bounds from the lamps it reads
  const uint32_t records_offset = r.U32();
  t.hours_.on_hour = r.F32();
  const uint64_t hash = r.U64();
  t.hours_.off_hour = r.F32();
  t.hours_.margin_hours = r.F32();
  if (count > kMaxLamps) return fail("holds more lamps than a table may");
  const auto hour_ok = [](float h) { return std::isfinite(h) && h >= 0.0f && h <= 24.0f; };
  if (!hour_ok(t.hours_.on_hour) || !hour_ok(t.hours_.off_hour) || !(t.hours_.margin_hours >= 0.0f && t.hours_.margin_hours <= 2.0f)) return fail("has lamp hours that are not hours");
  if (record_bytes != kLampRecordBytes) return fail("has records of another size");
  if (!(t.grid_.cell_m >= 1.0f) || !std::isfinite(t.grid_.cell_m) || !std::isfinite(t.grid_.origin_x) || !std::isfinite(t.grid_.origin_z)) return fail("has a grid that is not one");
  const uint64_t cells = uint64_t(t.grid_.nx) * t.grid_.nz;
  if (cells > kMaxLampCells) return fail("has more grid cells than a table may");
  if (count != 0 && cells == 0) return fail("has lamps and no grid");
  if (table_bytes != (cells + 1) * 4) return fail("has a cell table of the wrong size");
  const uint64_t expect_offset = (uint64_t(kLampHeaderBytes) + table_bytes + 15u) & ~uint64_t(15);
  if (records_offset != expect_offset) return fail("has its records at an unexpected place");
  if (size != expect_offset + uint64_t(count) * kLampRecordBytes) return fail("is not the size its header says (cut short, or too long)");
  if (Fnv1a64(data + kLampHeaderBytes, size - kLampHeaderBytes) != hash) return fail("does not match its hash (the file is damaged)");

  ByteReader tr(data + kLampHeaderBytes, table_bytes);
  t.cell_start_.resize(size_t(cells) + 1);
  for (uint32_t& v : t.cell_start_) v = tr.U32();
  if (t.cell_start_.front() != 0 || t.cell_start_.back() != count) return fail("has a cell table that does not add up to its lamps");
  for (size_t c = 1; c < t.cell_start_.size(); ++c) {
    if (t.cell_start_[c] < t.cell_start_[c - 1]) return fail("has a cell table that goes backwards");
  }

  ByteReader rr(data + records_offset, size_t(count) * kLampRecordBytes);
  t.lamps_.resize(count);
  for (uint32_t c = 0; c < uint32_t(cells); ++c) {
    const uint32_t cx = c % t.grid_.nx, cz = c / t.grid_.nx;
    const float x0 = t.grid_.origin_x + float(cx) * t.grid_.cell_m - kCellEpsilon, x1 = t.grid_.origin_x + float(cx + 1) * t.grid_.cell_m + kCellEpsilon;
    const float z0 = t.grid_.origin_z + float(cz) * t.grid_.cell_m - kCellEpsilon, z1 = t.grid_.origin_z + float(cz + 1) * t.grid_.cell_m + kCellEpsilon;
    for (uint32_t i = t.cell_start_[c]; i < t.cell_start_[c + 1]; ++i) {
      Lamp& l = t.lamps_[i];
      for (float& v : l.pos) v = rr.F32();
      l.intensity = rr.F32();
      for (float& v : l.colour) v = rr.F32();
      l.cone_deg = rr.F32();
      for (float& v : l.dir) v = rr.F32();
      l.spread_deg = rr.F32();
      l.spot = rr.U8();
      l.on = rr.U8();
      const uint8_t source = rr.U8();
      l.flags = rr.U8();
      l.file_index = rr.U32();
      if (!FinitePosition(l.pos)) return fail("holds a lamp whose place is not a number");
      if (l.spot > 1 || l.on > 1 || source > uint8_t(LampSource::kStreetLamp)) return fail("holds a lamp with a type, on byte or source that is not one");
      l.source = LampSource(source);
      if (l.pos[0] < x0 || l.pos[0] >= x1 || l.pos[2] < z0 || l.pos[2] >= z1) return fail("holds a lamp outside the cell it is listed under");
    }
  }
  if (!rr.ok() || !tr.ok()) return fail("is cut short");
  for (int a = 0; a < 3; ++a) {
    t.bounds_min_[a] = t.bounds_max_[a] = count != 0 ? t.lamps_[0].pos[a] : 0.0f;
  }
  for (const Lamp& l : t.lamps_) {
    for (int a = 0; a < 3; ++a) {
      t.bounds_min_[a] = std::min(t.bounds_min_[a], l.pos[a]);
      t.bounds_max_[a] = std::max(t.bounds_max_[a], l.pos[a]);
    }
  }
  t.Index();
  *out = std::move(t);
  return true;
}

}  // namespace larecomp::citypack
