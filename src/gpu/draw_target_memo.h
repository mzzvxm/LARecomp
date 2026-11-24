#pragma once

#include "draw_state.h"
#include "edram.h"

namespace mcla::gpu {

inline EdramKey ColorEdramKey(const XenosState& state, uint32_t i) {
  return {uint16_t(state.color[i].base_tiles), uint16_t(state.surface_pitch), 0,
          uint8_t(FoldColorFormat(uint32_t(state.color[i].format))),
          uint8_t(MsaaSampleCount(state.msaa))};
}

inline EdramKey DepthEdramKey(const XenosState& state) {
  return {uint16_t(state.depth_base_tiles), uint16_t(state.surface_pitch), 1,
          uint8_t(state.depth_format), uint8_t(MsaaSampleCount(state.msaa))};
}

template <typename Targets>
EdramKey DrawDepthKey(Targets& edram, const XenosState& state, uint32_t color_count,
                      uint32_t size_bits) {
  EdramKey depth = DepthEdramKey(state);
  if (color_count != 0) {
    const EdramKey color0 = ColorEdramKey(state, 0);
    depth.reserved = edram.DepthLayer(depth, &color0, size_bits);
  } else {
    depth.reserved = edram.DepthLayer(depth, nullptr, size_bits);
  }
  return depth;
}

struct DrawTargetScale {
  Scale scale;
  uint32_t host_samples = 1;
  uint32_t msaa_guest = 0;  // nonzero when the first target follows the host MSAA level
  bool scene = false;      // the first target takes the scene's separate scale
  bool operator==(const DrawTargetScale&) const = default;
};

// The draw uses the first colour target's scale and samples, or the depth
// target's if there is no colour. Before target creation, use its declared size.
template <typename Targets>
DrawTargetScale DrawScale(Targets& edram, const XenosState& state, uint32_t color_count,
                          bool has_depth, uint32_t size_bits) {
  EdramKey first;
  if (color_count != 0) {
    first = ColorEdramKey(state, 0);
  } else if (has_depth) {
    first = DrawDepthKey(edram, state, 0, size_bits);
  } else {
    return {.host_samples = MsaaSampleCount(state.msaa)};
  }
  DrawTargetScale result;
  result.host_samples = edram.HostSamplesOf(first, size_bits);
  if (edram.HostSamplesFollowLevel(first, size_bits)) {
    result.msaa_guest = first.samples;
  }
  result.scene = Targets::IsSceneKey(first) && edram.SceneApart();
  result.scale = edram.ScaleOf(first, size_bits);
  return result;
}

struct DrawTargetSignature {
  uint64_t serial = 0;
  uint32_t size_bits = 0;
  uint16_t pitch = 0;
  uint16_t color_base = 0;
  uint16_t depth_base = 0;
  uint8_t samples = 0;
  uint8_t color_format = 0;
  uint8_t depth_format = 0;
  uint8_t color_count = 0;
  bool has_depth = false;
  bool scene_phase = false;
  bool operator==(const DrawTargetSignature&) const = default;

  static DrawTargetSignature From(const XenosState& state, uint32_t colors, bool depth,
                                  uint32_t size, uint64_t serial, bool scene_phase) {
    return {serial, size, uint16_t(state.surface_pitch),
            colors ? uint16_t(state.color[0].base_tiles) : uint16_t(0),
            depth ? uint16_t(state.depth_base_tiles) : uint16_t(0),
            uint8_t(MsaaSampleCount(state.msaa)),
            colors ? uint8_t(state.color[0].format) : uint8_t(0),
            depth ? uint8_t(state.depth_format) : uint8_t(0),
            uint8_t(colors), depth, scene_phase};
  }
};

class DrawTargetMemo {
 public:
  struct Result {
    DrawTargetScale scale;
    EdramKey depth;
    bool operator==(const Result&) const = default;
  };

  template <typename Targets>
  const Result& Get(Targets& edram, const XenosState& state, uint32_t colors, bool depth,
                   uint32_t size_bits, bool& hit) {
    // The caller has completed EDRAM's lazy screen and scene discovery.
    const auto key = DrawTargetSignature::From(state, colors, depth, size_bits,
                                               edram.change_serial(), edram.scene_phase());
    hit = key_.serial != 0 && key_ == key;
    if (!hit) {
      key_ = key;
      result_.scale = DrawScale(edram, state, colors, depth, size_bits);
      result_.depth = depth ? DrawDepthKey(edram, state, colors, size_bits) : EdramKey{};
    }
    return result_;
  }

 private:
  DrawTargetSignature key_;
  Result result_;
};

}  // namespace mcla::gpu
