#include "citypack/lamp_merge.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace larecomp::citypack {

namespace {

float Distance(const float a[3], const float b[3]) {
  const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
  return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

// Is the live light the (unpacked) copy of this file light?
bool SameLight(const Lamp& file, const LiveLight& live, const MergeOptions& o, float* distance) {
  if (file.spot != live.spot) return false;
  const float d = Distance(file.pos, live.pos);
  if (!(d <= o.match_metres)) return false;
  if (!(std::fabs(file.intensity - live.intensity) <= o.intensity_tolerance * std::max(file.intensity, 0.0f) + 0.01f)) return false;
  for (int a = 0; a < 3; ++a) {
    if (!(std::fabs(file.colour[a] - live.colour[a]) <= o.colour_tolerance)) return false;
  }
  if (!(std::fabs(file.cone_deg - live.cone_deg) <= o.angle_tolerance_deg)) return false;
  if (!(std::fabs(file.spread_deg - live.spread_deg) <= o.angle_tolerance_deg)) return false;
  *distance = d;
  return true;
}

Lamp LampOf(const LiveLight& l, LampSource source) {
  Lamp lamp;
  for (int a = 0; a < 3; ++a) {
    lamp.pos[a] = l.pos[a];
    lamp.colour[a] = l.colour[a];
    lamp.dir[a] = l.dir[a];
  }
  lamp.intensity = l.intensity;
  lamp.cone_deg = l.cone_deg;
  lamp.spread_deg = l.spread_deg;
  lamp.spot = l.spot;
  lamp.on = l.on;
  lamp.source = source;
  lamp.flags = kLampSeenLive;
  lamp.file_index = kNoLampIndex;
  return lamp;
}

}  // namespace

std::string MergeReport::Summary() const {
  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "lamps merged: %u in the file, live %u fill + %u street; %u fill lights are file lights (%u at the same ordinal, worst %.3f m, mean %.3f m), %u are not in the file, "
                "%u file lights were not seen live, %u street lamps added; %u in all",
                file_lights, live_fill, live_street, matched, matched_by_ordinal, worst_match_metres, mean_match_metres, live_only_fill, file_only, street_added, merged);
  return buf;
}

LampTable MergeLive(const LampTable& file, const std::vector<LiveLight>& live, const MergeOptions& options, MergeReport* report) {
  MergeReport local;
  MergeReport& rep = report ? *report : local;
  rep = MergeReport{};
  rep.file_lights = file.size();
  if (live.empty()) {
    rep.file_only = file.size();
    rep.merged = file.size();
    return file;
  }

  std::vector<Lamp> lamps = file.lamps();
  std::vector<uint8_t> taken(lamps.size(), 0);
  std::vector<Lamp> extra;
  std::vector<uint32_t> near;
  uint32_t ordinal = 0;
  double sum_match = 0.0;
  for (const LiveLight& l : live) {
    if (l.street) {
      ++rep.live_street;
      extra.push_back(LampOf(l, LampSource::kStreetLamp));
      ++rep.street_added;
      continue;
    }
    ++rep.live_fill;
    const uint32_t k = ordinal++;
    uint32_t chosen = kNoLampIndex;
    float chosen_distance = 0.0f;
    bool by_ordinal = false;
    // the file light at the same ordinal first
    const uint32_t hint = file.FindByFileIndex(k);
    float d = 0.0f;
    if (hint != kNoLampIndex && !taken[hint] && lamps[hint].source == LampSource::kFile && SameLight(lamps[hint], l, options, &d)) {
      chosen = hint;
      chosen_distance = d;
      by_ordinal = true;
    } else {
      file.NearSphere(l.pos, options.match_metres, &near);
      for (const uint32_t i : near) {
        if (taken[i] || lamps[i].source != LampSource::kFile || !SameLight(lamps[i], l, options, &d)) continue;
        // the nearest wins; equal distances: the lower file index (the table's order is the cell's, not the file's)
        if (chosen == kNoLampIndex || d < chosen_distance || (d == chosen_distance && lamps[i].file_index < lamps[chosen].file_index)) {
          chosen = i;
          chosen_distance = d;
        }
      }
    }
    if (chosen != kNoLampIndex) {
      taken[chosen] = 1;
      lamps[chosen].flags |= kLampSeenLive;
      ++rep.matched;
      if (by_ordinal) ++rep.matched_by_ordinal;
      rep.worst_match_metres = std::max<double>(rep.worst_match_metres, chosen_distance);
      sum_match += chosen_distance;
    } else {
      extra.push_back(LampOf(l, LampSource::kLiveFill));
      ++rep.live_only_fill;
    }
  }
  rep.file_only = rep.file_lights - rep.matched;
  rep.mean_match_metres = rep.matched != 0 ? sum_match / double(rep.matched) : 0.0;
  lamps.insert(lamps.end(), extra.begin(), extra.end());
  LampTable merged = LampTable::Build(std::move(lamps), file.grid().cell_m > 0.0f ? file.grid().cell_m : kDefaultLampCellMetres, file.flags() | kLampTableHasLive);
  merged.set_hours(file.hours());
  rep.merged = merged.size();
  return merged;
}

float RoundThroughHalf(float v) {
  if (v == 0.0f || !std::isfinite(v)) return v;
  int e = 0;
  const float m = std::frexp(std::fabs(v), &e);
  if (e < -13) return v;
  return std::copysign(std::ldexp(std::round(m * 2048.0f) / 2048.0f, e), v);
}

LiveLight SimulateGamePacking(const Lamp& l, bool street, const PackingBox& box) {
  LiveLight v;
  for (int a = 0; a < 3; ++a) {
    const float span = box.hi[a] - box.lo[a];
    const float clamped = std::clamp(l.pos[a], box.lo[a], box.hi[a]);
    v.pos[a] = box.lo[a] + std::round((clamped - box.lo[a]) / span * 65535.0f) * (span / 65535.0f);
    v.colour[a] = RoundThroughHalf(l.colour[a]);
    v.dir[a] = RoundThroughHalf(l.dir[a]);
  }
  v.intensity = RoundThroughHalf(l.intensity);
  v.cone_deg = RoundThroughHalf(l.cone_deg);
  v.spread_deg = RoundThroughHalf(l.spread_deg);
  v.spot = l.spot;
  v.on = l.on;
  v.street = street;
  return v;
}

std::vector<LiveLight> LiveViewOf(const LampTable& table) {
  std::vector<LiveLight> out;
  out.reserve(table.size());
  // the file lights in the file's order (the ordinals), then the rest in the table's
  std::vector<uint32_t> files;
  for (uint32_t i = 0; i < table.size(); ++i) {
    const Lamp& l = table[i];
    if (l.source == LampSource::kFile && (l.flags & kLampSeenLive) != 0) files.push_back(i);
  }
  std::sort(files.begin(), files.end(), [&](uint32_t a, uint32_t b) { return table[a].file_index < table[b].file_index; });
  const auto convert = [&](const Lamp& l, bool street) {
    LiveLight v;
    for (int a = 0; a < 3; ++a) {
      v.pos[a] = l.pos[a];
      v.colour[a] = l.colour[a];
      v.dir[a] = l.dir[a];
    }
    v.intensity = l.intensity;
    v.cone_deg = l.cone_deg;
    v.spread_deg = l.spread_deg;
    v.spot = l.spot;
    v.on = l.on;
    v.street = street;
    return v;
  };
  for (const uint32_t i : files) out.push_back(convert(table[i], false));
  for (uint32_t i = 0; i < table.size(); ++i) {
    const Lamp& l = table[i];
    if (l.source == LampSource::kLiveFill) out.push_back(convert(l, false));
  }
  for (uint32_t i = 0; i < table.size(); ++i) {
    const Lamp& l = table[i];
    if (l.source == LampSource::kStreetLamp) out.push_back(convert(l, true));
  }
  return out;
}

void SelectLamps(const LampTable& table, const LampSelection& s, std::vector<uint32_t>* out) {
  table.NearSphere(s.eye, s.radius_m, out);
  size_t kept = 0;
  for (const uint32_t i : *out) {
    const Lamp& l = table[i];
    if (s.live_list_fresh && l.source == LampSource::kStreetLamp && Distance(l.pos, s.eye) <= s.live_radius_m) continue;  // the game's list has it, with its own fade
    if (s.hour >= 0.0f && !(LampActivity(l, s.hours != nullptr ? *s.hours : table.hours(), s.hour, s.night_factor) > 0.0f)) continue;
    (*out)[kept++] = i;
  }
  out->resize(kept);
}

}  // namespace larecomp::citypack
