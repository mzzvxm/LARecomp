// MCLA GPU renderer -- per-frame draw census by pixel shader. See draw_census.h.

#include "draw_census.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <map>
#include <string_view>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(
    mcla_gpu_census_ps, "", "MCLA/GPU",
    "Diagnostic: up to 4 pixel shader identities, hex, comma separated. Per "
    "guest frame, the draws and vertices recorded with each; per report window, their "
    "distribution. Empty: off.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(
    mcla_gpu_census_targets, false, "MCLA/GPU",
    "Diagnostic: per report window, every pass the frames drew into (its EDRAM "
    "targets) with its draws per frame and the pixel shaders that drew there.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace mcla::gpu {

void TargetCensus::Configure() {
  active_ = REXCVAR_GET(mcla_gpu_census_targets);
  if (active_) {
    REXLOG_WARN("[gpu] census ON (diagnostic): passes by EDRAM targets, with their pixel shaders");
  }
}

std::string TargetCensus::TakeReport() {
  if (!active_ || passes_.empty()) {
    return {};
  }
  const double frames = frames_ ? double(frames_) : 1.0;
  std::vector<std::pair<const std::string*, const Pass*>> order;
  order.reserve(passes_.size());
  for (const auto& [label, pass] : passes_) {
    order.emplace_back(&label, &pass);
  }
  std::sort(order.begin(), order.end(),
            [](const auto& a, const auto& b) { return a.second->draws > b.second->draws; });
  std::string text = std::format("census of passes over {} frames, {} passes:", frames_,
                                 order.size());
  for (const auto& [label, pass] : order) {
    std::vector<std::pair<uint64_t, uint64_t>> shaders(pass->shaders.begin(), pass->shaders.end());
    std::sort(shaders.begin(), shaders.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string list;
    for (size_t i = 0; i < shaders.size() && i < kShadersListed; ++i) {
      list += std::format("{}{:016x} {:.1f}", i ? ", " : "", shaders[i].first,
                          double(shaders[i].second) / frames);
    }
    if (shaders.size() > kShadersListed) {
      list += std::format(" (+{} more)", shaders.size() - kShadersListed);
    }
    text += std::format("\n  [pass] {} | {:.1f} draws/frame | PS {}", *label,
                        double(pass->draws) / frames, list);
  }
  passes_.clear();
  frames_ = 0;
  return text;
}

void DrawCensus::Configure() {
  shader_count_ = 0;
  const std::string text = REXCVAR_GET(mcla_gpu_census_ps);
  std::string_view rest(text);
  while (!rest.empty()) {
    const size_t comma = rest.find(',');
    std::string_view item = rest.substr(0, comma);
    rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
    while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
    while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
    if (item.starts_with("0x") || item.starts_with("0X")) item.remove_prefix(2);
    if (item.empty()) {
      continue;
    }
    uint64_t id = 0;
    const auto [end, ec] = std::from_chars(item.data(), item.data() + item.size(), id, 16);
    if (ec != std::errc() || end != item.data() + item.size() || id == 0) {
      REXLOG_ERROR("[gpu] census: '{}' in mcla_gpu_census_ps is not a shader identity; left out",
                   item);
      continue;
    }
    if (shader_count_ == kMaxShaders) {
      REXLOG_ERROR("[gpu] census: more than {} shaders in mcla_gpu_census_ps; {:016X} and the "
                   "rest left out",
                   kMaxShaders, id);
      break;
    }
    ids_[shader_count_++] = id;
  }
  if (shader_count_) {
    std::string list;
    for (uint32_t i = 0; i < shader_count_; ++i) {
      list += std::format("{}{:016X}", i ? ", " : "", ids_[i]);
    }
    REXLOG_WARN("[gpu] census ON (diagnostic): vertices per guest frame of PS {}", list);
  }
}

void DrawCensus::OnPresent() {
  for (uint32_t i = 0; i < shader_count_; ++i) {
    window_vertices_[i].push_back(frame_vertices_[i]);
    window_draws_[i] += frame_draws_[i];
    frame_vertices_[i] = 0;
    frame_draws_[i] = 0;
  }
}

std::string DrawCensus::TakeReport() {
  std::string text;
  for (uint32_t i = 0; i < shader_count_; ++i) {
    std::vector<uint32_t>& v = window_vertices_[i];
    if (v.empty()) {
      continue;
    }
    uint64_t changed = 0;
    for (size_t f = 1; f < v.size(); ++f) {
      changed += v[f] != v[f - 1];
    }
    std::map<uint32_t, uint32_t> values;
    for (uint32_t x : v) {
      ++values[x];
    }
    std::vector<std::pair<uint32_t, uint32_t>> top(values.begin(), values.end());
    std::sort(top.begin(), top.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string common;
    for (size_t k = 0; k < top.size() && k < 5; ++k) {
      common += std::format("{}{} x{}", k ? ", " : "", top[k].first, top[k].second);
    }
    std::sort(v.begin(), v.end());
    const auto at = [&](double q) { return v[std::min(v.size() - 1, size_t(q * double(v.size())))]; };
    text += std::format(
        "{}census PS {:016X}: {} frames, {:.1f} draws/frame; vertices per frame min {}, median "
        "{}, p90 {}, max {}; changed in {} frames; most common: {}",
        text.empty() ? "" : "\n", ids_[i], v.size(), double(window_draws_[i]) / double(v.size()),
        v.front(), at(0.5), at(0.9), v.back(), changed, common);
    v.clear();
    window_draws_[i] = 0;
  }
  return text;
}

}  // namespace mcla::gpu
