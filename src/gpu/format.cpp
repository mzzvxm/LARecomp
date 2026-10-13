// MCLA GPU renderer -- guest formats to Plume formats. See format.h.

#include "format.h"

namespace mcla::gpu::format {

using plume::RenderFormat;

namespace {

constexpr Mapping Ok(RenderFormat format, const char* name) {
  return Mapping{format, Status::kOk, name};
}
constexpr Mapping Missing(const char* name) {
  return Mapping{RenderFormat::UNKNOWN, Status::kMissingInPlume, name};
}

}  // namespace

const char* StatusName(Status status) {
  switch (status) {
    case Status::kOk: return "ok";
    case Status::kMissingInPlume: return "missing in Plume";
    case Status::kUnknown: return "unknown";
  }
  return "?";
}

const char* PlumeFormatName(RenderFormat format) {
  switch (format) {
    case RenderFormat::UNKNOWN: return "UNKNOWN";
    case RenderFormat::R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
    case RenderFormat::R32G32B32_FLOAT: return "R32G32B32_FLOAT";
    case RenderFormat::R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case RenderFormat::R16G16B16A16_SNORM: return "R16G16B16A16_SNORM";
    case RenderFormat::R32G32_FLOAT: return "R32G32_FLOAT";
    case RenderFormat::R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case RenderFormat::R8G8B8A8_UINT: return "R8G8B8A8_UINT";
    case RenderFormat::B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case RenderFormat::R16G16_FLOAT: return "R16G16_FLOAT";
    case RenderFormat::R16G16_SNORM: return "R16G16_SNORM";
    case RenderFormat::D32_FLOAT_S8_UINT: return "D32_FLOAT_S8_UINT";
    case RenderFormat::R32_FLOAT: return "R32_FLOAT";
    case RenderFormat::R32_UINT: return "R32_UINT";
    case RenderFormat::R8_UNORM: return "R8_UNORM";
    case RenderFormat::BC1_UNORM: return "BC1_UNORM";
    case RenderFormat::BC2_UNORM: return "BC2_UNORM";
    case RenderFormat::BC3_UNORM: return "BC3_UNORM";
    case RenderFormat::D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
    case RenderFormat::B5G5R5A1_UNORM: return "B5G5R5A1_UNORM";
    default: return "other";
  }
}

Mapping VertexFormat(uint32_t packed) {
  switch (packed) {
    case 0x002C235F: return Ok(RenderFormat::R16G16_FLOAT, "k_16_16_FLOAT");
    case 0x001A2360: return Ok(RenderFormat::R16G16B16A16_FLOAT, "k_16_16_16_16_FLOAT");
    // Measured, not derived (legacy DECLGAP, 7170 samples): this one spans 8
    // bytes and the next 16, which is also what the type's own vertex format
    // field says (0xA5 & 0x3F = 37 k_32_32_FLOAT, 0xA6 & 0x3F = 38
    // k_32_32_32_32_FLOAT).
    case 0x002C23A5: return Ok(RenderFormat::R32G32_FLOAT, "k_32_32_FLOAT");
    case 0x001A23A6: return Ok(RenderFormat::R32G32B32A32_FLOAT, "k_32_32_32_32_FLOAT");
    case 0x002A23B9: return Ok(RenderFormat::R32G32B32_FLOAT, "k_32_32_32_FLOAT");
    // The translated shaders declare packed attributes as uint4 and decode them.
    case 0x001A2286: return Ok(RenderFormat::R8G8B8A8_UINT, "k_8_8_8_8 (uint)");
    case 0x00182886: return Ok(RenderFormat::B8G8R8A8_UNORM, "k_8_8_8_8 (unorm, BGRA)");
    // One whole dword: R10G10B10A2 would split it (legacy: every normal read 0).
    case 0x001A2187: return Ok(RenderFormat::R32_UINT, "k_2_10_10_10 (packed)");
    default: return Mapping{};
  }
}

bool IsPackedNormalDeclType(uint32_t packed) { return packed == 0x001A2187; }

Mapping ColorTargetFormat(uint32_t color_format) {
  switch (color_format) {
    case 0: return Ok(RenderFormat::R8G8B8A8_UNORM, "k_8_8_8_8");
    // Stored linear in a wider float target; the resolve encodes the Xenos
    // piecewise-linear gamma when it copies out. sRGB would not be
    // the console's curve even where the host has it.
    case 1: return Ok(RenderFormat::R16G16B16A16_FLOAT, "k_8_8_8_8_GAMMA");
    case 2: return Missing("k_2_10_10_10");     // DXGI R10G10B10A2_UNORM
    case 10: return Missing("k_2_10_10_10_AS_10_10_10_10");
    // No host 7e3 float; the wider float target is the lossless substitute
    // (legacy and Xenia).
    case 3: return Ok(RenderFormat::R16G16B16A16_FLOAT, "k_2_10_10_10_FLOAT");
    case 12: return Ok(RenderFormat::R16G16B16A16_FLOAT, "k_2_10_10_10_FLOAT_AS_16_16_16_16");
    case 4: return Ok(RenderFormat::R16G16_SNORM, "k_16_16");
    case 5: return Ok(RenderFormat::R16G16B16A16_SNORM, "k_16_16_16_16");
    case 6: return Ok(RenderFormat::R16G16_FLOAT, "k_16_16_FLOAT");
    case 7: return Ok(RenderFormat::R16G16B16A16_FLOAT, "k_16_16_16_16_FLOAT");
    case 14: return Ok(RenderFormat::R32_FLOAT, "k_32_FLOAT");
    case 15: return Ok(RenderFormat::R32G32_FLOAT, "k_32_32_FLOAT");
    default: return Mapping{};
  }
}

Mapping DepthTargetFormat(uint32_t depth_format) {
  switch (depth_format) {
    case 0: return Ok(RenderFormat::D24_UNORM_S8_UINT, "kD24S8");
    // No 24-bit float depth on the host; D32 float keeps the range with stencil.
    case 1: return Ok(RenderFormat::D32_FLOAT_S8_UINT, "kD24FS8");
    default: return Mapping{};
  }
}

TextureHostFormat TextureHost(uint32_t texture_format) {
  const auto ok = [](RenderFormat format, const char* name, uint16_t swizzle,
                     TextureLoad load = TextureLoad::kCopy,
                     TextureDecompress decompress = TextureDecompress::kNone) {
    TextureHostFormat f;
    f.format = format;
    f.status = Status::kOk;
    f.guest_name = name;
    f.host_swizzle = swizzle;
    f.load = load;
    f.decompress = decompress;
    return f;
  };
  switch (texture_format) {
    case 2: return ok(RenderFormat::R8_UNORM, "k_8", kHostSwizzleRRRR);
    // Xenia: "Red and blue swapped in the load shader for simplicity" -- the
    // Xenos keeps X in bits 0-4, B5G5R5A1 (DXGI) / A1R5G5B5_PACK16 (Vulkan) keep
    // blue there. Sampling the 5-bit UNORM gives v/31 exactly, which no 8-bit
    // expansion can (corrects 24.14).
    case 3:
      return ok(RenderFormat::B5G5R5A1_UNORM, "k_1_5_5_5", kHostSwizzleRGBA,
                TextureLoad::kR5G5B5A1ToB5G5R5A1);
    case 6: return ok(RenderFormat::R8G8B8A8_UNORM, "k_8_8_8_8", kHostSwizzleRGBA);
    case 18:
      return ok(RenderFormat::BC1_UNORM, "k_DXT1", kHostSwizzleRGBA, TextureLoad::kCopy,
                TextureDecompress::kDXT1);
    case 19:
      return ok(RenderFormat::BC2_UNORM, "k_DXT2_3", kHostSwizzleRGBA, TextureLoad::kCopy,
                TextureDecompress::kDXT2_3);
    case 20:
      return ok(RenderFormat::BC3_UNORM, "k_DXT4_5", kHostSwizzleRGBA, TextureLoad::kCopy,
                TextureDecompress::kDXT4_5);
    // Depth read as a texture: one float channel (Xenia: "R32_FLOAT for depth
    // because shaders would require an additional SRV to sample stencil"). A
    // resolve product serves it normally (resolve.h); decoded from memory only
    // when the guest wrote one itself.
    case 22:
      return ok(RenderFormat::R32_FLOAT, "k_24_8", kHostSwizzleRRRR, TextureLoad::kDepthUnorm24);
    case 23:
      return ok(RenderFormat::R32_FLOAT, "k_24_8_FLOAT", kHostSwizzleRRRR,
                TextureLoad::kDepthFloat20e4);
    // EXPAND is float16 per channel (the SDK's format table, and what the
    // guest's own resolves write into it); M3 had SNORM, which was wrong.
    case 29:
      return ok(RenderFormat::R16G16B16A16_FLOAT, "k_16_16_16_16_EXPAND", kHostSwizzleRGBA);
    case 32:
      return ok(RenderFormat::R16G16B16A16_FLOAT, "k_16_16_16_16_FLOAT", kHostSwizzleRGBA);
    case 36: return ok(RenderFormat::R32_FLOAT, "k_32_FLOAT", kHostSwizzleRRRR);
    default: return TextureHostFormat{};
  }
}

Mapping TextureFormat(uint32_t texture_format) {
  const TextureHostFormat f = TextureHost(texture_format);
  return Mapping{f.format, f.status, f.guest_name};
}

const char* UsageSemantic(uint32_t usage) {
  switch (usage) {
    case 0: return "POSITION";
    case 1: return "BLENDWEIGHT";
    case 2: return "BLENDINDICES";
    case 3: return "NORMAL";
    case 4: return "PSIZE";
    case 5: return "TEXCOORD";
    case 6: return "TANGENT";
    case 7: return "BINORMAL";
    case 10: return "COLOR";
    default: return nullptr;
  }
}

}  // namespace mcla::gpu::format
