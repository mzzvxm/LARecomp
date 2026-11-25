#pragma once
// ===========================================================================
// LARecomp shader pack builder -- a stage's block of constants, declared at the place the chain gives it
// ===========================================================================
// Every variant that has constants of its own reads them from the bytes after the 608 byte SharedConstants block (the push
// constant's SharedConstants address on Vulkan, the DEFINE_SHARED_CONSTANTS() cbuffer on D3D12). Version 2 of the pack had each
// variant write its own declaration at byte 608, which only works when it is the only one. DeclareConstants takes the stage's
// members and the offset the chain gives its block in this permutation (StageContext::constants_offset: 608 alone, after the
// blocks of the stages before it when composed; variant_chain.h LayoutConstants), and adds them to both backends'
// declarations of the shared constants, with exactly the text version 2's variants wrote for themselves when the stage is
// alone. The declarations are written next to the translator's last member (g_InvResolutionScale), so two stages of one text
// each add theirs there, in any order: the registers are named by offset, so their place in the text does not matter.
//
//   SPIR-V  `#define <name> vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + <offset>, 0x10)` per member (a member
//           that is an array: `#define <spirv_name>(INDEX) ... + <offset> + (INDEX) * 16 ...`, the name with its argument)
//   DXIL    `float4 <name> : packoffset(c<offset / 16>);` in the macro's continued lines (an array: `<name>[<count>]`)
//
// A member is 16 bytes (a float4 or a uint4) or `count` of them. The members must add up to the table row's constants_bytes.
// Bindless textures and samplers are members too: a uint in a uint4 is the descriptor index of the texture or sampler.
// ===========================================================================

#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "shaderpack/variant_pack.h"
#include "shaderpack/variants/text_edit.h"

namespace larecomp::shaderpack {

struct ConstantMember {
  std::string_view type;  // "float4" or "uint4"
  std::string_view name;  // the name the stage's text uses (DXIL), and SPIR-V's too unless spirv_name is given
  uint32_t count = 0;     // 0: one value; else an array of this many
  std::string_view spirv_name = {};  // an array's macro with its argument, "g_HlLight(INDEX)"; empty: `name`
};

// Adds the declarations to `text` (it must have the translator's g_InvResolutionScale declaration of each backend once, and none of
// these names yet). False with *error in plain words when it does not, or when the members' size is not the row's.
inline bool DeclareConstants(std::string& text, const StageContext& ctx, std::string_view comment, std::span<const ConstantMember> members,
                             std::string* error) {
  using namespace textedit;
  uint32_t bytes = 0;
  for (const ConstantMember& member : members) bytes += 16u * (member.count == 0 ? 1u : member.count);
  if (bytes != ctx.constants_bytes) {
    *error = std::format("its constants are {} bytes and the chain's table says {}", bytes, ctx.constants_bytes);
    return false;
  }
  if (ctx.constants_offset % chain::kConstantsAlignment != 0 || ctx.constants_offset < chain::kSharedConstantsBytes) {
    *error = std::format("its constants are at byte {}, which is not a 16 byte step from the shared constants' 608", ctx.constants_offset);
    return false;
  }
  std::string spirv_lines;
  std::string dxil_lines;
  std::vector<std::string> spirv_checks;
  std::vector<std::string> dxil_checks;
  uint32_t at = ctx.constants_offset;
  for (const ConstantMember& member : members) {
    const std::string_view spirv_name = member.spirv_name.empty() ? member.name : member.spirv_name;
    const std::string place = member.count == 0 ? std::format("{}", at) : std::format("{} + (INDEX) * 16", at);
    const std::string define = std::format("#define {} vk::RawBufferLoad<{}>(g_PushConstants.SharedConstants + {}, 0x10)", spirv_name, member.type, place);
    spirv_lines += (spirv_lines.empty() ? "" : "\n") + define;
    spirv_checks.push_back(define);
    const std::string declaration =
        member.count == 0 ? std::format("{} {} : packoffset(c{});", member.type, member.name, at / 16)
                          : std::format("{} {}[{}] : packoffset(c{});", member.type, member.name, member.count, at / 16);
    dxil_lines += (dxil_lines.empty() ? "" : " \\\n    ") + declaration;
    dxil_checks.push_back(declaration);
    at += 16u * (member.count == 0 ? 1u : member.count);
  }
  static constexpr Piece kSpirvDeclaration[] = {Lit("#define g_InvResolutionScale"), Ws(1),
                                                Lit("vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 592)")};
  static constexpr Piece kDxilDeclaration[] = {Lit("float2 g_InvResolutionScale : packoffset(c37.x);")};
  if (!ReplaceOnce(text, kSpirvDeclaration, 0, 2, "SPIR-V g_InvResolutionScale",
                   [&](std::string_view group) { return std::string(group) + "\n// " + std::string(comment) + "\n" + spirv_lines; }, error)) {
    return false;
  }
  if (!ReplaceOnce(text, kDxilDeclaration, 0, 0, "DXIL g_InvResolutionScale",
                   [&](std::string_view group) { return std::string(group.substr(0, group.size() - 1)) + "; \\\n    " + dxil_lines; }, error)) {
    return false;
  }
  // Each member is declared once in each backend (a name that was there already would be two declarations of it).
  for (const std::string& needle : spirv_checks) {
    if (const size_t count = CountOccurrences(text, needle); count != 1) {
      *error = std::format("the transformed HLSL has '{}' {} times (need 1)", needle, count);
      return false;
    }
  }
  for (const std::string& needle : dxil_checks) {
    if (const size_t count = CountOccurrences(text, needle); count != 1) {
      *error = std::format("the transformed HLSL has '{}' {} times (need 1)", needle, count);
      return false;
    }
  }
  return true;
}

}  // namespace larecomp::shaderpack
