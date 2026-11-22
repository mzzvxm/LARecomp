#include "citypack/hit_collect.h"

#include <algorithm>
#include <cmath>

namespace larecomp::citypack {

namespace {

// The size in bytes of a channel of format code `code` (4 bits of the table); 0 for a code this does not know.
uint32_t ChannelBytes(uint32_t code) {
  switch (code) {
    case kChannelPosition:
      return 12;
    case kChannelHalf2:
    case kChannelColour:
    case kChannelPacked:
      return 4;
    default:
      return 0;
  }
}

// A value of `bits` bits (1..8) of a byte, rounded, and back.
uint32_t QuantiseLinear(uint32_t v, uint32_t bits) {
  const uint32_t top = (1u << bits) - 1u;
  const uint32_t q = (v * top + 127u) / 255u;
  return (q * 255u + top / 2u) / top;
}
// ... on a square root scale (a small value keeps its place).
uint32_t QuantiseSqrt(uint32_t v, uint32_t bits) {
  const double top = double((1u << bits) - 1u);
  const double q = std::floor(std::sqrt(double(v) / 255.0) * top + 0.5);
  return uint32_t(std::floor(q * q / (top * top) * 255.0 + 0.5));
}

}  // namespace

uint32_t HitRules::QuantiseColour(uint32_t c) const {
  uint32_t r = c & 0xFFu, g = (c >> 8) & 0xFFu, b = (c >> 16) & 0xFFu, a = c >> 24;
  if (ao_bits >= 1 && ao_bits < 8) a = QuantiseLinear(a, ao_bits);
  if (tint_bits >= 1 && tint_bits < 8) {
    r = QuantiseSqrt(r, tint_bits);
    g = QuantiseSqrt(g, tint_bits);
    b = QuantiseSqrt(b, tint_bits);
  }
  return PackCornerColour(r, g, b, a);
}

VertexLayout LayoutOfDeclaration(uint32_t fvf, uint64_t channel_sizes, uint32_t stride) {
  VertexLayout layout;
  layout.stride = stride;
  if ((fvf & kFvfPosition) == 0 || (channel_sizes & 15u) != kChannelPosition) return layout;
  uint32_t offset = 0;
  for (uint32_t channel = 0; channel < 16; ++channel) {
    if ((fvf & (1u << channel)) == 0) continue;
    const uint32_t code = uint32_t((channel_sizes >> (4 * channel)) & 15u);
    const uint32_t bytes = ChannelBytes(code);
    if (bytes == 0) return layout;  // a channel format this does not know: the offsets after it are not
    if (channel == 4 && code == kChannelColour) layout.colour0 = int32_t(offset);
    if (channel == 5 && code == kChannelColour) layout.colour1 = int32_t(offset);
    if (channel == 6 && code == kChannelHalf2) layout.uv0 = int32_t(offset);
    if (channel == 7 && code == kChannelHalf2) layout.uv1 = int32_t(offset);
    offset += bytes;
  }
  if ((fvf >> 16) != 0 || offset != stride) {
    layout.colour0 = layout.colour1 = layout.uv0 = layout.uv1 = -1;
    return layout;
  }
  layout.valid = true;
  return layout;
}

void CollectStats::Add(const CollectStats& o) {
  geometries += o.geometries;
  triangles += o.triangles;
  corners += o.corners;
  geometries_without_uv += o.geometries_without_uv;
  geometries_without_colour += o.geometries_without_colour;
  geometries_layout_unknown += o.geometries_layout_unknown;
  geometries_second_uv += o.geometries_second_uv;
  triangles_second_uv += o.triangles_second_uv;
  geometries_colour0 += o.geometries_colour0;
  uv_not_finite += o.uv_not_finite;
  runs += o.runs;
  runs_split += o.runs_split;
  triangles_uv_clamped += o.triangles_uv_clamped;
}

void HitCollector::AddGeometry(const SourceGeometry& g, bool alpha, const std::vector<uint32_t>& kept, uint32_t material) {
  if (kept.empty()) return;
  const VertexLayout layout = LayoutOfDeclaration(g.fvf, g.fvf_sizes, g.stride);
  List& list = lists_[alpha ? 1 : 0];
  Rec rec;
  rec.first = list.triangles;
  rec.count = uint32_t(kept.size());
  rec.material = material;
  rec.has_uv = layout.uv0 >= 0;
  rec.has_colour = layout.colour1 >= 0;
  ++stats_.geometries;
  stats_.triangles += kept.size();
  stats_.corners += kept.size() * 3;
  if (!layout.valid) ++stats_.geometries_layout_unknown;
  if (!rec.has_uv) ++stats_.geometries_without_uv;
  if (!rec.has_colour) ++stats_.geometries_without_colour;
  if (layout.uv1 >= 0) {
    ++stats_.geometries_second_uv;
    stats_.triangles_second_uv += kept.size();
  }
  if (layout.colour0 >= 0) ++stats_.geometries_colour0;

  list.corners.reserve(list.corners.size() + kept.size() * 3);
  for (const uint32_t t : kept) {
    for (uint32_t k = 0; k < 3; ++k) {
      const uint32_t index = LoadBE16(g.indices + size_t(t * 3 + k) * 2);
      const uint8_t* vertex = g.vertices + size_t(index) * g.stride;
      Corner corner;
      if (rec.has_uv) {
        const float u = HalfToFloat(LoadBE16(vertex + layout.uv0));
        const float v = HalfToFloat(LoadBE16(vertex + layout.uv0 + 2));
        if (std::isfinite(u) && std::isfinite(v)) {
          corner.u = u;
          corner.v = v;
        } else {
          ++stats_.uv_not_finite;
        }
      }
      if (rec.has_colour) corner.colour = rules_.QuantiseColour(CornerColourOfD3dColor(LoadBE32(vertex + layout.colour1)));
      list.corners.push_back(corner);
    }
  }
  list.triangles += rec.count;
  list.recs.push_back(rec);
}

bool HitCollector::Build(bool split, uint32_t opaque_triangles, HitBlob* out, CollectStats* stats) const {
  HitBlob blob;
  blob.triangle_count = lists_[0].triangles + lists_[1].triangles;
  blob.opaque_triangles = opaque_triangles;
  bool any_attributes = false;
  for (const List& list : lists_) {
    for (const Rec& rec : list.recs) {
      if (rec.has_uv || rec.has_colour) any_attributes = true;
    }
  }
  blob.flags = any_attributes ? kHitBlobHasCorners : 0u;
  if (any_attributes) blob.corners.reserve(size_t(blob.triangle_count) * 6);

  for (uint32_t li = 0; li < 2; ++li) {
    const List& list = lists_[li];
    const uint32_t list_base = li == 0 ? 0u : opaque_triangles;
    for (const Rec& rec : list.recs) {
      // The pieces of this geometry: one, or several where its texture coordinates range wider than a run's transform can hold (kMaxRunUvRange).
      uint32_t piece_first = 0;
      while (piece_first < rec.count) {
        uint32_t piece_end = rec.count;
        float lo[2] = {0, 0}, hi[2] = {0, 0};
        if (rec.has_uv) {
          bool started = false;
          for (uint32_t t = piece_first; t < rec.count; ++t) {
            float tlo[2], thi[2];
            for (int a = 0; a < 2; ++a) tlo[a] = thi[a] = a == 0 ? list.corners[size_t(rec.first + t) * 3].u : list.corners[size_t(rec.first + t) * 3].v;
            for (uint32_t k = 1; k < 3; ++k) {
              const Corner& c = list.corners[size_t(rec.first + t) * 3 + k];
              tlo[0] = std::min(tlo[0], c.u);
              thi[0] = std::max(thi[0], c.u);
              tlo[1] = std::min(tlo[1], c.v);
              thi[1] = std::max(thi[1], c.v);
            }
            const float nlo[2] = {started ? std::min(lo[0], tlo[0]) : tlo[0], started ? std::min(lo[1], tlo[1]) : tlo[1]};
            const float nhi[2] = {started ? std::max(hi[0], thi[0]) : thi[0], started ? std::max(hi[1], thi[1]) : thi[1]};
            if (started && split && (nhi[0] - nlo[0] > kMaxRunUvRange || nhi[1] - nlo[1] > kMaxRunUvRange)) {
              piece_end = t;  // cut before this triangle
              break;
            }
            lo[0] = nlo[0];
            lo[1] = nlo[1];
            hi[0] = nhi[0];
            hi[1] = nhi[1];
            started = true;
          }
        }
        HitRun run;
        run.first_triangle = list_base + rec.first + piece_first;
        run.triangle_count = piece_end - piece_first;
        run.material = rec.material;
        run.flags = (li == 1 ? kRunAlphaTested : 0u) | (rec.has_uv ? kRunHasUv : 0u) | (rec.has_colour ? kRunHasColour : 0u);
        bool transform_ok = true;
        if (rec.has_uv) {
          // a run whose range is narrower than the step allows is given a wider transform: its coordinates quantise to small numbers
          if (rules_.uv_step_log2 != 0) {
            const float min_range = 65535.0f / float(1u << rules_.uv_step_log2);
            for (int a = 0; a < 2; ++a) hi[a] = std::max(hi[a], lo[a] + min_range);
          }
          transform_ok = ChooseUvTransform(lo[0], hi[0], lo[1], hi[1], &run);
          if (!transform_ok) stats->triangles_uv_clamped += run.triangle_count;  // the corners clamp into the default transform (0 to 1)
        }
        if (any_attributes) {
          for (uint32_t t = piece_first; t < piece_end; ++t) {
            for (uint32_t k = 0; k < 3; ++k) {
              const Corner& c = list.corners[size_t(rec.first + t) * 3 + k];
              blob.corners.push_back(rec.has_uv ? EncodeCornerUv(run, c.u, c.v) : 0u);
              blob.corners.push_back(c.colour);
            }
          }
        }
        blob.runs.push_back(run);
        ++stats->runs;
        if (piece_first != 0) ++stats->runs_split;
        piece_first = piece_end;
      }
    }
  }
  if (blob.runs.size() > kMaxRunsPerBlob) return false;
  *out = std::move(blob);
  return true;
}

bool HitCollector::Finish(uint32_t opaque_triangles, uint32_t triangle_count, HitBlob* out, std::string* error) {
  if (lists_[0].triangles != opaque_triangles || lists_[0].triangles + lists_[1].triangles != triangle_count) {
    if (error) *error = "the hit pass kept other triangles than the pack's mesh (the two builds disagree)";
    return false;
  }
  CollectStats built;
  if (!Build(true, opaque_triangles, out, &built)) {
    // More runs than a 16 bit id holds (a geometry whose texture coordinates jump about): no cutting, the geometry's own runs, the corners clamp into their ranges.
    built = CollectStats();
    if (!Build(false, opaque_triangles, out, &built)) {
      if (error) *error = "a mesh has more geometries than a hit blob has runs";
      return false;
    }
  }
  stats_.runs += built.runs;
  stats_.runs_split += built.runs_split;
  stats_.triangles_uv_clamped += built.triangles_uv_clamped;
  const std::string runs = CheckRuns(out->runs, out->triangle_count, out->opaque_triangles);
  if (!runs.empty()) {
    if (error) *error = "the hit runs are not a partition: " + runs;
    return false;
  }
  return true;
}

}  // namespace larecomp::citypack
