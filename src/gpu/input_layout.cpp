// MCLA GPU renderer -- vertex input layouts. See input_layout.h.

#include "input_layout.h"

#include <algorithm>
#include <cstring>

#include <rex/hash.h>
#include <rex/logging.h>

#include "format.h"
#include "guest_device.h"
#include "native_gfx/vertex_declaration.h"

namespace mcla::gpu {

namespace nfx = mcla::native_gfx;
using plume::RenderFormat;

namespace {

// D3DDECLUSAGE values (shader.h DeclUsage).
constexpr uint32_t kUsagePosition = 0;
constexpr uint32_t kUsageNormal = 3;
constexpr uint32_t kUsageTexCoord = 5;
constexpr uint32_t kUsageTangent = 6;
constexpr uint32_t kUsageBinormal = 7;
constexpr uint32_t kUsageColor = 10;

// The usages the translated shaders declare as uint4 (USAGE_TYPES).
bool IsUintUsage(uint32_t usage) {
  return usage == kUsageNormal || usage == kUsageTangent || usage == kUsageBinormal;
}

// LAR-XenosRecomp's USAGE_LOCATIONS (shader_recompiler.cpp), the Vulkan
// location of each vertex input. UINT32_MAX when the generator gives none.
uint32_t VulkanLocation(uint32_t usage, uint32_t index) {
  struct Entry {
    uint32_t usage, index, location;
  };
  static constexpr Entry kTable[] = {
      {kUsagePosition, 0, 0}, {kUsageNormal, 0, 1},   {kUsageTangent, 0, 2},
      {kUsageBinormal, 0, 3}, {kUsageTexCoord, 0, 4}, {kUsageTexCoord, 1, 5},
      {kUsageTexCoord, 2, 6}, {kUsageTexCoord, 3, 7}, {kUsageColor, 0, 8},
      {2 /* BlendIndices */, 0, 9}, {1 /* BlendWeight */, 0, 10}, {kUsageColor, 1, 11},
      {kUsageTexCoord, 4, 12}, {kUsageTexCoord, 5, 13}, {kUsageTexCoord, 6, 14},
      {kUsageTexCoord, 7, 15}, {kUsagePosition, 1, 16},
  };
  for (const Entry& e : kTable) {
    if (e.usage == usage && e.index == index) {
      return e.location;
    }
  }
  return UINT32_MAX;
}

// Bytes a vertex format occupies and the unit its components swap in.
bool FormatLayout(RenderFormat format, uint32_t* bytes, uint32_t* unit) {
  switch (format) {
    case RenderFormat::R16G16_FLOAT: *bytes = 4; *unit = 2; return true;
    case RenderFormat::R16G16B16A16_FLOAT: *bytes = 8; *unit = 2; return true;
    case RenderFormat::R32G32_FLOAT: *bytes = 8; *unit = 4; return true;
    case RenderFormat::R32G32B32_FLOAT: *bytes = 12; *unit = 4; return true;
    case RenderFormat::R32G32B32A32_FLOAT: *bytes = 16; *unit = 4; return true;
    case RenderFormat::R8G8B8A8_UINT:
    case RenderFormat::B8G8R8A8_UNORM:
    case RenderFormat::R32_UINT: *bytes = 4; *unit = 4; return true;
    default: return false;
  }
}

// The integer format with the same bits, for the uint4 inputs.
RenderFormat UintFormatOf(RenderFormat format) {
  switch (format) {
    case RenderFormat::R32_UINT: return RenderFormat::R32_UINT;  // the packed 2_10_10_10 dword
    case RenderFormat::R32G32_FLOAT: return RenderFormat::R32G32_UINT;
    case RenderFormat::R32G32B32_FLOAT: return RenderFormat::R32G32B32_UINT;
    case RenderFormat::R32G32B32A32_FLOAT: return RenderFormat::R32G32B32A32_UINT;
    default: return RenderFormat::UNKNOWN;
  }
}

bool IsUintFormat(RenderFormat format) {
  switch (format) {
    case RenderFormat::R32_UINT:
    case RenderFormat::R32G32_UINT:
    case RenderFormat::R32G32B32_UINT:
    case RenderFormat::R32G32B32A32_UINT:
    case RenderFormat::R8G8B8A8_UINT: return true;
    default: return false;
  }
}

}  // namespace

size_t InputLayouts::KeyHash::operator()(const Key& k) const {
  return size_t(XXH3_64bits(k.bytes.data(), k.bytes.size()) ^ (k.vs * 0x9E3779B97F4A7C15ull));
}

const InputLayout* InputLayouts::Get(const ShaderStore::Entry& vs, const uint8_t* decl,
                                     uint32_t decl_count, const uint8_t* stream_strides) {
  scratch_key_.vs = vs.key;
  scratch_key_.bytes.assign(decl, decl + size_t(decl_count) * guest::kDeclElementBytes);
  // The strides of the streams the declaration names.
  uint32_t named = 0;
  for (uint32_t d = 0; d < decl_count; ++d) {
    const nfx::DeclarationElement e = nfx::ReadDeclarationElement(decl, d);
    if (e.stream == 0xFF) {
      break;
    }
    if (e.stream < guest::kStreamCount) {
      named |= 1u << e.stream;
    }
  }
  for (uint32_t s = 0; s < guest::kStreamCount; ++s) {
    if (named & (1u << s)) {
      scratch_key_.bytes.push_back(uint8_t(s));
      scratch_key_.bytes.push_back(stream_strides[s]);
    }
  }
  auto it = layouts_.find(scratch_key_);
  if (it != layouts_.end()) {
    return it->second.get();
  }
  std::unique_ptr<InputLayout> layout = Build(vs, decl, decl_count, stream_strides);
  const InputLayout* raw = layout.get();
  layouts_.emplace(scratch_key_, std::move(layout));
  return raw;
}

std::unique_ptr<InputLayout> InputLayouts::Build(const ShaderStore::Entry& vs,
                                                 const uint8_t* decl, uint32_t decl_count,
                                                 const uint8_t* stream_strides) {
  auto layout = std::make_unique<InputLayout>();
  layout->id = next_id_++;

  std::vector<nfx::DeclarationElement> elements;
  for (uint32_t d = 0; d < decl_count; ++d) {
    const nfx::DeclarationElement e = nfx::ReadDeclarationElement(decl, d);
    if (e.stream == 0xFF) {
      break;  // D3DDECL_END
    }
    elements.push_back(e);
  }

  const auto report = [&](const char* what, uint32_t usage, uint32_t index, uint32_t type) {
    ++layout->problems;
    if (reported_ < 32) {
      ++reported_;
      REXLOG_ERROR("[gpu] input layout of vs {:016X}: {} (usage {} index {}, decltype {:08X})",
                   vs.key, what, usage, index, type);
    }
  };

  // Slot of a guest stream, creating it on first use.
  const auto slot_of = [&](uint32_t stream) -> uint32_t {
    for (uint32_t i = 0; i < layout->streams.size(); ++i) {
      if (layout->streams[i].stream == stream) {
        return i;
      }
    }
    StreamPlan plan;
    plan.stream = stream;
    plan.stride = stream < guest::kStreamCount ? uint32_t(stream_strides[stream]) * 4u : 0u;
    layout->streams.push_back(plan);
    return uint32_t(layout->streams.size() - 1);
  };

  shaders_.GetVertexElements(vs, scratch_);
  bool any_zero = false;
  struct Pending {
    plume::RenderInputElement element;
    bool zero = false;
  };
  std::vector<Pending> pending;
  for (const ShaderStore::VertexElement& ve : scratch_) {
    const char* semantic = format::UsageSemantic(ve.usage);
    if (!semantic) {
      report("usage without a semantic", ve.usage, ve.usage_index, 0);
      continue;
    }
    const uint32_t location = VulkanLocation(ve.usage, ve.usage_index);
    if (location == UINT32_MAX) {
      report("usage without a Vulkan location", ve.usage, ve.usage_index, 0);
    }
    const bool uint_input = IsUintUsage(ve.usage);
    const nfx::DeclarationElement* match = nullptr;
    for (const nfx::DeclarationElement& e : elements) {
      if (e.usage == ve.usage && e.usage_index == ve.usage_index) {
        match = &e;
        break;
      }
    }
    Pending p;
    p.element.semanticName = semantic;
    p.element.semanticIndex = ve.usage_index;
    p.element.location = location == UINT32_MAX ? 0 : location;
    if (!match) {
      // Unpatched vfetch: the hardware reads zeros.
      p.zero = true;
      any_zero = true;
      p.element.format =
          uint_input ? RenderFormat::R32G32B32A32_UINT : RenderFormat::R32G32B32A32_FLOAT;
      p.element.alignedByteOffset = 0;
      pending.push_back(p);
      continue;
    }
    const format::Mapping mapping = format::VertexFormat(match->type);
    if (mapping.status != format::Status::kOk) {
      report("declaration type without a host format", ve.usage, ve.usage_index, match->type);
      continue;
    }
    RenderFormat host = mapping.format;
    if (uint_input) {
      if (format::IsPackedNormalDeclType(match->type)) {
        layout->packed_normal = true;
      }
      host = UintFormatOf(mapping.format);
      if (host == RenderFormat::UNKNOWN) {
        report("uint4 input over a type that is not 32-bit floats or the packed dword", ve.usage,
               ve.usage_index, match->type);
        continue;
      }
    } else if (IsUintFormat(host)) {
      // The hardware fetch converts these to floats (UBYTE4: each byte as
      // 0..255). The host has no USCALED vertex format on D3D12, and an integer
      // format feeding a float input reads the raw bits.
      report("float4 input over an integer type (no host format converts it as the hardware does)",
             ve.usage, ve.usage_index, match->type);
      continue;
    }
    p.element.format = host;
    p.element.slotIndex = slot_of(match->stream);
    p.element.alignedByteOffset = match->offset;
    pending.push_back(p);
  }

  // Swap plans over every element each bound stream holds.
  for (StreamPlan& plan : layout->streams) {
    for (const nfx::DeclarationElement& e : elements) {
      if (e.stream != plan.stream) {
        continue;
      }
      const format::Mapping mapping = format::VertexFormat(e.type);
      uint32_t bytes = 0;
      uint32_t unit = 4;
      if (mapping.status != format::Status::kOk || !FormatLayout(mapping.format, &bytes, &unit)) {
        report("declaration element of unknown size in a bound stream", e.usage, e.usage_index,
               e.type);
        continue;
      }
      plan.swaps.push_back(SwapRange{uint16_t(e.offset), uint16_t(bytes), uint8_t(unit)});
    }
    std::sort(plan.swaps.begin(), plan.swaps.end(),
              [](const SwapRange& a, const SwapRange& b) { return a.offset < b.offset; });
    for (size_t i = 1; i < plan.swaps.size(); ++i) {
      const SwapRange& prev = plan.swaps[i - 1];
      if (plan.swaps[i].offset < prev.offset + prev.size) {
        report("overlapping declaration elements", 0, 0, 0);
        break;
      }
    }
    uint8_t bytes[4 + 5 * 64] = {};
    size_t n = 0;
    std::memcpy(bytes, &plan.stride, 4);
    n = 4;
    for (const SwapRange& r : plan.swaps) {
      if (n + 5 > sizeof(bytes)) {
        break;
      }
      std::memcpy(bytes + n, &r.offset, 2);
      std::memcpy(bytes + n + 2, &r.size, 2);
      bytes[n + 4] = r.unit;
      n += 5;
    }
    plan.plan_hash = XXH3_64bits(bytes, n);
  }

  for (uint32_t i = 0; i < layout->streams.size(); ++i) {
    layout->slots.emplace_back(i, layout->streams[i].stride,
                               plume::RenderInputSlotClassification::PER_VERTEX_DATA);
  }
  if (any_zero) {
    layout->zero_stream = true;
    const uint32_t zero_slot = uint32_t(layout->slots.size());
    layout->slots.emplace_back(zero_slot, 0, plume::RenderInputSlotClassification::PER_VERTEX_DATA);
    for (Pending& p : pending) {
      if (p.zero) {
        p.element.slotIndex = zero_slot;
      }
    }
  }
  for (const Pending& p : pending) {
    layout->elements.push_back(p.element);
  }
  return layout;
}

}  // namespace mcla::gpu
