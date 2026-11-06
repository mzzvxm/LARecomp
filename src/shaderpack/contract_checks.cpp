// LARecomp shader pack builder -- the shader contract, as hard errors (see contract_checks.h).
// Ported check for check from native/tools/build_shader_pack_v8.py (mzzvxm).

#include "contract_checks.h"

#include <format>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace larecomp::shaderpack {

namespace {

// ---------------------------------------------------------------------------
// The text markers, in one place (V*_MARK ... ALPHA_TO_MASK_ROUNDING of the Python).
// The bytes are the ones the Python tests; the generator writes its own part
// of the file with \n, the pasted shader_common.h part may carry \r\n, and none
// of these crosses a line except the coverage block, which is generated text.
// ---------------------------------------------------------------------------
constexpr std::string_view kV4Mark = "float4 g_PosScale : packoffset(c35);";
constexpr std::string_view kV5Mark = "uint g_AlphaToMask : packoffset(c33.w);";
constexpr std::string_view kV6Mark =
    "#define XENOS_ALPHA_COVERAGE (((XENOS_SPEC_CONSTANTS) & SPEC_CONSTANT_ALPHA_TEST) != 0)";
constexpr std::string_view kV6CoverageOutput =
    "#if XENOS_ALPHA_COVERAGE\n\t, out uint oCoverage : SV_Coverage\n#endif";
constexpr std::string_view kV7Mark = "float2 g_InvResolutionScale : packoffset(c37.x);";
// v8: the 8-sample branch of computeAlphaToMask, in every pixel shader.
constexpr std::string_view kV8Mark = "if (samplesLog2 == 3u)";
// v6's pixel position prologue, in host pixels: none may be left.
constexpr std::string_view kV6PositionPrologue = "= float4((iPos.xy - 0.5)";
// The depth-test outputs, precise (and the position Invariant on SPIR-V).
constexpr std::string_view kPrecisePosition =
    "\t[[vk::ext_decorate(18)]] out precise float4 oPos : SV_Position,";
constexpr std::string_view kPreciseDepth = "\tout precise float oDepth : SV_Depth";
// Alpha to coverage compares the alpha rounded to 1/65536, not the host float.
constexpr std::string_view kAlphaToMaskFunction =
    "uint computeAlphaToMask(float alpha, float2 pixelPosition)";
constexpr std::string_view kAlphaToMaskRounding =
    "    alpha = round(alpha * 65536.0) * (1.0 / 65536.0);";

// SPIR-V 1.6 specification, 3.32 / 3.20 / 3.21.
constexpr uint32_t kOpDecorate = 71;
constexpr uint32_t kDecorationSpecId = 1;
constexpr uint32_t kDecorationBuiltIn = 11;
constexpr uint32_t kBuiltInSampleMask = 20;
constexpr uint32_t kDecorationInvariant = 18;
constexpr uint32_t kDecorationNoContraction = 42;
constexpr uint32_t kBuiltInPosition = 0;
bool IsSpecConstantOp(uint32_t opcode) { return opcode >= 48 && opcode <= 52; }  // {48, 49, 50, 51, 52}

bool Has(std::string_view text, std::string_view needle) {
  return text.find(needle) != std::string_view::npos;
}

void AddProblem(Problems* problems, std::string_view prefix, std::string_view what) {
  std::string line;
  line.reserve(prefix.size() + 2 + what.size());
  line.append(prefix);
  line.append(": ");
  line.append(what);
  problems->push_back(std::move(line));
}

std::string VariantPrefix(std::string_view name, int variant) {
  return std::format("{}/{}", name, variant);
}

// The words of a SPIR-V module, little endian (the magic the compile check
// already tested says so).
class SpirvWords {
 public:
  explicit SpirvWords(std::span<const uint8_t> bytes) : bytes_(bytes) {}
  bool Valid(std::string* error) const {
    if (bytes_.size() < 20 || bytes_.size() % 4 != 0) {
      *error = std::format("not a SPIR-V module ({} bytes)", bytes_.size());
      return false;
    }
    return true;
  }
  size_t size() const { return bytes_.size() / 4; }
  uint32_t operator[](size_t i) const {
    const uint8_t* p = bytes_.data() + i * 4;
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
  }

 private:
  std::span<const uint8_t> bytes_;
};

// Walks the instruction stream past the 5-word header, calling visit(index,
// word count, opcode). False (with *error) when an instruction runs off the end.
template <class Visitor>
bool WalkSpirv(const SpirvWords& words, std::string* error, Visitor&& visit) {
  size_t i = 5;  // past the header
  while (i < words.size()) {
    const uint32_t count = words[i] >> 16;
    const uint32_t opcode = words[i] & 0xFFFF;
    if (count == 0 || i + count > words.size()) {
      *error = std::format("malformed instruction at word {}", i);
      return false;
    }
    visit(i, count, opcode);
    i += count;
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// HLSL text
// ---------------------------------------------------------------------------

bool CheckHlslText(const HlslCheckInput& input, Problems* problems, bool* rounds_alpha) {
  const size_t before = problems->size();
  const std::string_view text = input.hlsl;
  const bool pixel = input.stage == ShaderStage::kPixel;
  if (rounds_alpha) *rounds_alpha = false;

  if (!Has(text, kV4Mark) || !Has(text, kV5Mark) || !Has(text, kV6Mark) || !Has(text, kV7Mark)) {
    AddProblem(problems, input.name, "lacks the v7 header");
  }
  if (pixel && !Has(text, kV8Mark)) {
    AddProblem(problems, input.name, "has no 8-sample alpha to coverage (v8)");
  }
  if (Has(text, kV6PositionPrologue)) {
    AddProblem(problems, input.name, "gives the pixel position in host pixels (v6)");
  }
  if (!pixel && !Has(text, kPrecisePosition)) {
    AddProblem(problems, input.name, "declares oPos without precise and Invariant");
  }
  if (pixel && Has(text, "SV_Depth") && !Has(text, kPreciseDepth)) {
    AddProblem(problems, input.name, "declares oDepth without precise");
  }
  if (pixel || Has(text, kAlphaToMaskFunction)) {
    if (!Has(text, kAlphaToMaskFunction) || !Has(text, kAlphaToMaskRounding)) {
      AddProblem(problems, input.name, "alpha to coverage compares the unrounded alpha");
    } else if (rounds_alpha) {
      *rounds_alpha = true;
    }
  }
  // The generator declares exactly the outputs the container lists.
  if (pixel) {
    const uint32_t outputs = input.ps_outputs & 0x1F;
    struct Output {
      uint32_t bit;
      std::string_view semantic;
    };
    static constexpr Output kOutputs[] = {{0, "SV_Target0"}, {1, "SV_Target1"}, {2, "SV_Target2"},
                                          {3, "SV_Target3"}, {4, "SV_Depth"}};
    for (const Output& output : kOutputs) {
      if (bool(outputs & (1u << output.bit)) != Has(text, output.semantic)) {
        AddProblem(problems, input.name,
                   std::format("output {} disagrees with the container", output.semantic));
      }
    }
    // v6: the coverage output beside oC0, in the alpha coverage variant only.
    if (bool(outputs & 1u) != Has(text, kV6CoverageOutput)) {
      AddProblem(problems, input.name, "conditional SV_Coverage disagrees with its oC0");
    }
    if ((outputs & 1u) && input.spec_mask != 0x2) {
      AddProblem(problems, input.name,
                 std::format("writes oC0 with spec mask {:#x}, not ALPHA_TEST alone (the alpha "
                             "coverage variant)",
                             input.spec_mask));
    }
  }
  return problems->size() == before;
}

bool CheckControlFlow(std::string_view name, ControlFlowForm form, Problems* problems) {
  if (form == ControlFlowForm::kPcMachine) {
    AddProblem(problems, name, "pc machine control flow (crashes vkd3d-proton)");
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Blob contract
// ---------------------------------------------------------------------------

bool ReadSpirvTraits(std::span<const uint8_t> module, SpirvTraits* traits, std::string* error) {
  *traits = SpirvTraits{};
  const SpirvWords words(module);
  if (!words.Valid(error)) return false;
  return WalkSpirv(words, error, [&](size_t i, uint32_t count, uint32_t opcode) {
    if (opcode == kOpDecorate && count >= 3) {
      const uint32_t decoration = words[i + 2];
      if (decoration == kDecorationBuiltIn && count >= 4 && words[i + 3] == kBuiltInSampleMask) {
        traits->writes_sample_mask = true;
      } else if (decoration == kDecorationSpecId) {
        traits->has_spec_constants = true;
      }
    } else if (IsSpecConstantOp(opcode)) {
      traits->has_spec_constants = true;
    }
  });
}

bool CheckBlobContract(const BlobContractInput& input, Problems* problems) {
  const size_t before = problems->size();
  const std::string prefix = VariantPrefix(input.name, input.variant);
  // SV_Coverage (DXIL) / BuiltIn SampleMask (SPIR-V) exactly in the pixel shader
  // variant 1 of shaders that write oC0.
  const bool want = input.stage == ShaderStage::kPixel && input.writes_oc0 && input.variant == 1;

  const bool have_dxil = !input.dxil.empty();
  const bool have_spirv = !input.spirv.empty();
  bool dxil_coverage = false;
  if (have_dxil) {
    const std::string_view bytes(reinterpret_cast<const char*>(input.dxil.data()), input.dxil.size());
    dxil_coverage = Has(bytes, "SV_Coverage");
  }
  SpirvTraits spirv;
  if (have_spirv) {
    std::string error;
    if (!ReadSpirvTraits(input.spirv, &spirv, &error)) {
      AddProblem(problems, prefix, "SPIR-V " + error);
      return false;  // the Python stops checking this variant here too
    }
  }
  if ((have_dxil && dxil_coverage != want) || (have_spirv && spirv.writes_sample_mask != want)) {
    AddProblem(problems, prefix,
               std::format("coverage output DXIL {} SPIR-V {}, contract says {}",
                           have_dxil ? (dxil_coverage ? "true" : "false") : "not built",
                           have_spirv ? (spirv.writes_sample_mask ? "true" : "false") : "not built",
                           want ? "true" : "false"));
  }
  if (have_spirv && spirv.has_spec_constants) {
    AddProblem(problems, prefix, "SPIR-V keeps a specialization constant");
  }
  return problems->size() == before;
}

// ---------------------------------------------------------------------------
// Position: SPIR-V
// ---------------------------------------------------------------------------

bool ReadSpirvPositionTraits(std::span<const uint8_t> module, SpirvPositionTraits* traits,
                             std::string* error) {
  *traits = SpirvPositionTraits{};
  const SpirvWords words(module);
  if (!words.Valid(error)) return false;
  std::unordered_set<uint32_t> position, invariant;
  uint32_t no_contraction = 0;
  const bool walked = WalkSpirv(words, error, [&](size_t i, uint32_t count, uint32_t opcode) {
    if (opcode == kOpDecorate && count >= 3) {
      const uint32_t target = words[i + 1];
      const uint32_t decoration = words[i + 2];
      if (decoration == kDecorationBuiltIn && count >= 4 && words[i + 3] == kBuiltInPosition) {
        position.insert(target);
      } else if (decoration == kDecorationInvariant) {
        invariant.insert(target);
      } else if (decoration == kDecorationNoContraction) {
        ++no_contraction;
      }
    }
  });
  if (!walked) return false;
  bool all_invariant = !position.empty();  // bool(position) and position <= invariant
  for (const uint32_t variable : position) {
    if (!invariant.contains(variable)) all_invariant = false;
  }
  traits->position_invariant = all_invariant;
  traits->no_contraction = no_contraction;
  return true;
}

bool CheckSpirvPosition(std::string_view name, int variant, std::span<const uint8_t> module,
                        Problems* problems) {
  const std::string prefix = VariantPrefix(name, variant);
  SpirvPositionTraits traits;
  std::string error;
  if (!ReadSpirvPositionTraits(module, &traits, &error)) {
    AddProblem(problems, prefix, "SPIR-V " + error);
    return false;
  }
  if (!traits.position_invariant || traits.no_contraction == 0) {
    AddProblem(problems, prefix,
               std::format("SPIR-V position Invariant {}, {} NoContraction decorations",
                           traits.position_invariant ? "true" : "false", traits.no_contraction));
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Position: DXIL (the text of the disassembly)
// ---------------------------------------------------------------------------
// The Python reads `dxc -dumpbin` with two regexes and a walk:
//   DXIL_DEF            ^\s*(%\d+) = (.*)$
//   DXIL_POSITION_STORE storeOutput\.f32\(i32 5, i32 0, i32 0, i8 \d, float (%\d+)
//   fp op               re.match(r"f(add|sub|mul|div|rem|neg)\b", definition)
//   fast                " fast " in f" {definition} "
//   operands            re.findall(r"%\d+", definition)
// done here by hand on string_views of the text (checked against the regex
// version on 120 blobs of the v8 pack, same counts).

namespace {

bool IsSpaceChar(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }
bool IsDigitChar(char c) { return c >= '0' && c <= '9'; }
bool IsWordChar(char c) {
  return IsDigitChar(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

// "%" and its digits starting at line[i], or an empty view.
std::string_view ValueToken(std::string_view line, size_t i) {
  if (i >= line.size() || line[i] != '%') return {};
  size_t j = i + 1;
  while (j < line.size() && IsDigitChar(line[j])) ++j;
  return j > i + 1 ? line.substr(i, j - i) : std::string_view();
}

void ScanLine(std::string_view line, std::unordered_map<std::string_view, std::string_view>* defs,
              std::vector<std::string_view>* roots) {
  // %n = <definition>
  size_t i = 0;
  while (i < line.size() && IsSpaceChar(line[i])) ++i;
  if (const std::string_view token = ValueToken(line, i); !token.empty()) {
    const size_t after = i + token.size();
    if (line.substr(after, 3) == " = ") (*defs)[token] = line.substr(after + 3);
  }
  // storeOutput.f32(i32 5, i32 0, i32 0, i8 <c>, float %x ... the first hit on the line that fits
  static constexpr std::string_view kStore = "storeOutput.f32(i32 5, i32 0, i32 0, i8 ";
  static constexpr std::string_view kFloat = ", float ";
  for (size_t at = line.find(kStore); at != std::string_view::npos; at = line.find(kStore, at + 1)) {
    const size_t digit = at + kStore.size();
    if (digit >= line.size() || !IsDigitChar(line[digit])) continue;
    if (line.substr(digit + 1, kFloat.size()) != kFloat) continue;
    if (const std::string_view root = ValueToken(line, digit + 1 + kFloat.size()); !root.empty()) {
      roots->push_back(root);
      break;
    }
  }
}

bool IsFpOp(std::string_view definition) {
  if (definition.size() < 4 || definition[0] != 'f') return false;
  const std::string_view op = definition.substr(1, 3);
  if (op != "add" && op != "sub" && op != "mul" && op != "div" && op != "rem" && op != "neg") return false;
  return definition.size() == 4 || !IsWordChar(definition[4]);  // the \b
}

bool IsFast(std::string_view definition) {
  static constexpr std::string_view kFast = "fast";
  for (size_t at = definition.find(kFast); at != std::string_view::npos;
       at = definition.find(kFast, at + 1)) {
    const bool space_before = at == 0 || definition[at - 1] == ' ';
    const size_t end = at + kFast.size();
    const bool space_after = end == definition.size() || definition[end] == ' ';
    if (space_before && space_after) return true;
  }
  return false;
}

}  // namespace

DxilPositionStats DxilPositionFastOps(std::string_view disassembly) {
  std::unordered_map<std::string_view, std::string_view> defs;
  std::vector<std::string_view> roots;
  for (size_t pos = 0; pos <= disassembly.size();) {
    size_t end = disassembly.find('\n', pos);
    if (end == std::string_view::npos) end = disassembly.size();
    std::string_view line = disassembly.substr(pos, end - pos);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    ScanLine(line, &defs, &roots);
    pos = end + 1;
  }

  DxilPositionStats stats;
  std::unordered_set<std::string_view> seen;
  while (!roots.empty()) {
    const std::string_view value = roots.back();
    roots.pop_back();
    if (!seen.insert(value).second) continue;
    const auto it = defs.find(value);
    const std::string_view definition = it == defs.end() ? std::string_view() : it->second;
    if (IsFpOp(definition)) {
      ++stats.fp_ops;
      if (IsFast(definition)) ++stats.fast_ops;
    }
    for (size_t i = 0; i < definition.size();) {
      if (const std::string_view operand = ValueToken(definition, i); !operand.empty()) {
        roots.push_back(operand);
        i += operand.size();
      } else {
        ++i;
      }
    }
  }
  return stats;
}

bool CheckDxilPositionText(std::string_view name, int variant, std::string_view disassembly,
                           Problems* problems, DxilPositionStats* stats) {
  const DxilPositionStats found = DxilPositionFastOps(disassembly);
  if (stats) *stats = found;
  if (found.fast_ops != 0 || found.fp_ops == 0) {
    AddProblem(problems, VariantPrefix(name, variant),
               std::format("{} of the {} fp ops reaching SV_Position are fast", found.fast_ops,
                           found.fp_ops));
    return false;
  }
  return true;
}

bool CheckDxilPosition(DxcWorker& worker, std::string_view name, int variant,
                       std::span<const uint8_t> dxil, Problems* problems, DxilPositionStats* stats) {
  std::string text;
  const DxcStatus status = worker.DisassembleDxil(dxil, &text);
  if (!status.ok()) {
    AddProblem(problems, VariantPrefix(name, variant), status.message);
    return false;
  }
  return CheckDxilPositionText(name, variant, text, problems, stats);
}

}  // namespace larecomp::shaderpack
