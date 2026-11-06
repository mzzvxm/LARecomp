// LARecomp shader pack builder -- the HDR variant of the game's tone map (see hdr_variant.h).
// Port of native/tools/build_hdr_variants.py.

#include "shaderpack/hdr_variant.h"

#include <array>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "native_gfx/shader_identity.h"
#include "shaderpack/contract_checks.h"
#include "shaderpack/hash.h"
#include "shaderpack/hlsl_source.h"
#include "shaderpack/log.h"
#include "shaderpack/pack_format.h"
#include "shaderpack/pack_writer.h"

#ifdef LARECOMP_HDR_HLSLI_EMBEDDED
namespace {
// hdr_tonemap_shoulder.hlsli, turned into bytes by CMake (src/shaderpack/CMakeLists.txt), with a
// closing 0 that is not part of the file (so that even an empty file makes an array).
const unsigned char kEmbeddedCurve[] = {
#include "hdr_hlsli_bytes.inc"
};
}  // namespace
#endif

namespace larecomp::shaderpack {

namespace {

// ---------------------------------------------------------------------------
// The anchors. The Python uses `re`; every pattern in it is a run of literal text with `\s*` or `\s+`
// between some of the pieces, so a literal-and-whitespace matcher with the same greedy, backtracking
// rule gives the same matches (and no std::regex over 30 KB of text).
// ---------------------------------------------------------------------------

// What `\s` and str.rstrip() take as white space in this (ASCII) text: str.isspace().
bool IsSpace(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return u == ' ' || (u >= '\t' && u <= '\r') || (u >= 0x1C && u <= 0x1F);
}

struct Piece {
  std::string_view literal;  // the text of a literal piece
  bool whitespace = false;   // a run of white space instead: at least `min` characters, as many as there are
  size_t min = 0;
};
constexpr Piece Lit(std::string_view text) { return Piece{text, false, 0}; }
constexpr Piece Ws(size_t min) { return Piece{{}, true, min}; }  // \s* is Ws(0), \s+ is Ws(1)

constexpr size_t kMaxPieces = 15;
using Bounds = std::array<size_t, kMaxPieces + 1>;  // where each piece started, and the end of the match

// Whether pieces[index..] match at `pos`. A white space run takes everything it can and gives back
// one character at a time when the rest fails (what a greedy `\s*` does: it matters when the next
// literal begins with a tab).
bool MatchFrom(std::string_view text, size_t pos, std::span<const Piece> pieces, size_t index, Bounds& bounds) {
  bounds[index] = pos;
  if (index == pieces.size()) return true;
  const Piece& piece = pieces[index];
  if (!piece.whitespace) {
    return text.substr(pos, piece.literal.size()) == piece.literal &&
           MatchFrom(text, pos + piece.literal.size(), pieces, index + 1, bounds);
  }
  size_t run = 0;
  while (pos + run < text.size() && IsSpace(text[pos + run])) ++run;
  if (run < piece.min) return false;
  for (size_t length = run;; --length) {
    if (MatchFrom(text, pos + length, pieces, index + 1, bounds)) return true;
    if (length == piece.min) return false;
  }
}

struct Match {
  size_t begin = 0;
  size_t end = 0;
  Bounds bounds{};
};

// re.findall: the matches that do not overlap, scanning left to right.
std::vector<Match> FindAll(std::string_view text, std::span<const Piece> pieces) {
  std::vector<Match> found;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t start = pos;
    if (!pieces.front().whitespace) {
      start = text.find(pieces.front().literal, pos);
      if (start == std::string_view::npos) break;
    }
    Match match;
    if (MatchFrom(text, start, pieces, 0, match.bounds)) {
      match.begin = start;
      match.end = match.bounds[pieces.size()];
      found.push_back(match);
      pos = match.end > start ? match.end : start + 1;
    } else {
      pos = start + 1;
    }
  }
  return found;
}

// regex_once(): replaces the pattern exactly once or says why not. The replacement gets the text of
// the pattern's group 1, which is pieces[group_first..group_last].
using Replacement = std::function<std::string(std::string_view group)>;

bool ReplaceOnce(std::string& text, std::span<const Piece> pieces, size_t group_first, size_t group_last,
                 std::string_view what, const Replacement& replacement, std::string* error) {
  const std::vector<Match> found = FindAll(text, pieces);
  if (found.size() != 1) {
    *error = std::format(
        "anchor '{}' found {} times (need 1): the HLSL tree is not the one this transform was written for",
        what, found.size());
    return false;
  }
  const Match& match = found.front();
  const size_t group_begin = match.bounds[group_first];
  const size_t group_end = match.bounds[group_last + 1];
  const std::string replaced = replacement(std::string_view(text).substr(group_begin, group_end - group_begin));
  text.replace(match.begin, match.end - match.begin, replaced);
  return true;
}

// str.count: the occurrences that do not overlap.
size_t CountOccurrences(std::string_view text, std::string_view needle) {
  size_t count = 0;
  for (size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

// A file read with Python's open(path, "r"): "\r\n" and a lone "\r" are a "\n".
std::string NormalizeNewlines(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r') {
      out.push_back('\n');
      if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
    } else {
      out.push_back(text[i]);
    }
  }
  return out;
}

}  // namespace

HdrTransformResult TransformToneMapHlsl(std::string_view hlsl, std::string_view include_text) {
  HdrTransformResult result;
  std::string text = NormalizeNewlines(hlsl);
  std::string include = NormalizeNewlines(include_text);
  while (!include.empty() && IsSpace(include.back())) include.pop_back();  // include_text.rstrip()

  // 1. g_HdrScene in the SPIR-V shared constants (header of every generated shader).
  static constexpr Piece kSpirvDeclaration[] = {
      Lit("#define g_InvResolutionScale"), Ws(1),
      Lit("vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 592)")};
  // 2. and in the DXIL shared constants block.
  static constexpr Piece kDxilDeclaration[] = {Lit("float2 g_InvResolutionScale : packoffset(c37.x);")};
  // 3. The curve and the finish, before main.
  static constexpr Piece kMain[] = {Lit("#ifndef __spirv__"), Ws(0), Lit("[shader(\"pixel\")]"), Ws(0),
                                    Lit("#endif"),            Ws(0), Lit("void main(")};
  // 4. The game's own code: the luminance curve (Ld = Ls (1 + Ls / W) / (1 + Ls), made in r0.z from r1.x = Ls)...
  static constexpr Piece kLuminanceCurve[] = {
      Lit("\tr3.z = r1.x * r0.z;"),
      Ws(0),
      Lit("\tps = clamp(rcp(r0.x), FLT_MIN, FLT_MAX);"),
      Ws(0),
      Lit("\tr0.x = ps;"),
      Ws(0),
      Lit("\tr0.xzw = r3.xzy * r0.yxy;")};
  // 5. ... and the last assignment (the game's encode).
  static constexpr Piece kEncode[] = {Lit("\toC0.xyz = "),
                                      Lit("select(r2.xyzz == 0.0, r1.xyzz, r0.xyzz)"),
                                      Lit(";")};

  const auto fail = [&](std::string error) {
    result.ok = false;
    result.hlsl.clear();
    result.error = std::move(error);
    return result;
  };
  std::string error;
  if (!ReplaceOnce(text, kSpirvDeclaration, 0, 2, "SPIR-V g_InvResolutionScale",
                   [](std::string_view group) {
                     return std::string(group) +
                            "\n// HDR scene variant (src/gpu/constants.h HdrSceneConstants): 16 bytes after the 608.\n"
                            "#define g_HdrScene vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 608, 0x10)";
                   },
                   &error)) {
    return fail(std::move(error));
  }
  if (!ReplaceOnce(text, kDxilDeclaration, 0, 0, "DXIL g_InvResolutionScale",
                   [](std::string_view group) {
                     // the group without its closing ';', then the line continuation and the new member
                     return std::string(group.substr(0, group.size() - 1)) +
                            "; \\\n    float4 g_HdrScene : packoffset(c38);";
                   },
                   &error)) {
    return fail(std::move(error));
  }
  if (!ReplaceOnce(text, kMain, 0, 6, "pixel shader main",
                   [&](std::string_view group) { return include + "\n\n" + std::string(group); }, &error)) {
    return fail(std::move(error));
  }
  if (!ReplaceOnce(text, kLuminanceCurve, 0, 6, "Ld = Ls (1 + Ls / W) / (1 + Ls)",
                   [](std::string_view group) {
                     return std::string(group) + "\n\tr0.z = HdrToneLd(r1.x, White.x, g_HdrScene);";
                   },
                   &error)) {
    return fail(std::move(error));
  }
  if (!ReplaceOnce(text, kEncode, 1, 1, "oC0.xyz",
                   [](std::string_view group) { return "\toC0.xyz = HdrFinish(" + std::string(group) + ".xyz);"; },
                   &error)) {
    return fail(std::move(error));
  }

  for (const std::string_view needle : {std::string_view("HdrToneLd(r1.x"), std::string_view("HdrFinish(select"),
                                        std::string_view("g_HdrScene : packoffset(c38)"),
                                        std::string_view("SharedConstants + 608")}) {
    const size_t count = CountOccurrences(text, needle);
    if (count != 1) return fail(std::format("the transformed HLSL has '{}' {} times", needle, count));
  }

  result.ok = true;
  result.hlsl = std::move(text);
  return result;
}

// ---------------------------------------------------------------------------
// The step
// ---------------------------------------------------------------------------

namespace {

// A shader the variant needs, by its identity on the retail game (FNV-1a 64 of the vertex-fetch
// normalised microcode, the runtime's own) and by its name (the group and entry point of the .updb
// path inside the container, as the translator names its HLSL files).
struct Target {
  std::string_view name;
  uint64_t key;
};
constexpr Target kPixelTarget = {"xrage_postfx__PSToneMap", 0xBC5F0EED0A746DEAull};
// The vertex shaders the tone map is drawn with: they make its uv and the adapted luminance (the
// second also the screen position the fog variants use). Which one the game pairs it with is told by
// the renderer's own check; the runtime requires one of these. The order is the pack's: slot 0, 1.
constexpr Target kVertexTargets[] = {
    {"xrage_postfx__VS_ragePassThroughNoXform4TexOnly", 0xF097B3E9236CB05Aull},
    {"xrage_postfx__VS_ragePassThroughNoXform4TexAndScrPos", 0xF25FA84098BACE43ull},
};
static_assert(std::size(kVertexTargets) <= kHdrVertexShaders);

// By identity first; when the game is not the retail one, by name, which has to be unique.
// CHECK: the Python looks the name up among the executable's containers only; this looks in every
// container read, so a second shader of the same name in the RPF makes the fallback fail where the
// Python would not (the identity lookup, which decides on the retail game, is not affected).
const ShaderSource* ResolveTarget(const ContainerSet& set, const Target& target, ShaderStage stage,
                                  std::string* error) {
  if (const ShaderSource* by_key = set.Find(target.key); by_key && by_key->stage == stage) return by_key;
  const ShaderSource* found = nullptr;
  size_t count = 0;
  for (const ShaderSource& shader : set.shaders) {
    if (shader.name != target.name) continue;
    ++count;
    found = &shader;
  }
  if (count != 1) {
    *error = std::format("{}: identity {} is not in the game's shaders and {} shaders have that name (need one)",
                         target.name, Hex64(target.key), count);
    return nullptr;
  }
  if (found->stage != stage) {
    *error = std::format("{} is not {} shader", target.name, stage == ShaderStage::kPixel ? "a pixel" : "a vertex");
    return nullptr;
  }
  SHADERPACK_LOG_WARN("HDR variant: {} was found by its name ({}), not by its known identity {}", target.name,
                      Hex64(found->key), Hex64(target.key));
  return found;
}

void Put32(std::vector<uint8_t>& out, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out.push_back(uint8_t(value >> shift));
}

void Put64(std::vector<uint8_t>& out, uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) out.push_back(uint8_t(value >> shift));
}

struct HdrPackEntry {
  uint64_t ps_key = 0;
  std::array<uint64_t, kHdrVertexShaders> vs_keys{};  // 0 = none
  std::vector<uint8_t> dxil;                          // empty: the format was not built
  std::vector<uint8_t> spirv;
};

// header | entries | blobs, as build_hdr_variants.py writes it (pack_format.h has the layout).
// `entries` are in increasing ps key order. False with *error when a size does not fit.
bool LayOutHdrPack(std::span<const HdrPackEntry> entries, uint32_t formats, std::vector<uint8_t>* out,
                   std::string* error) {
  out->clear();
  out->insert(out->end(), kHdrPackMagic, kHdrPackMagic + 4);
  Put32(*out, kHdrPackVersion);
  Put32(*out, uint32_t(entries.size()));
  Put32(*out, formats);
  Put32(*out, kHdrContract);
  Put32(*out, kPackVersion);  // the main pack the variants were built beside
  Put32(*out, 0);
  Put32(*out, 0);

  const uint64_t blob_base = uint64_t(kHdrPackHeaderBytes) + uint64_t(entries.size()) * kHdrPackEntryBytes;
  std::vector<uint8_t> blobs;
  for (const HdrPackEntry& entry : entries) {
    const size_t entry_start = out->size();
    Put64(*out, entry.ps_key);
    for (const uint64_t vs_key : entry.vs_keys) Put64(*out, vs_key);
    Put32(*out, 0);  // flags
    Put32(*out, 0);  // reserved
    for (const std::vector<uint8_t>* blob : {&entry.dxil, &entry.spirv}) {
      if (blob->empty()) {
        Put32(*out, 0);
        Put32(*out, 0);
        Put64(*out, 0);
        continue;
      }
      const uint64_t offset = blob_base + blobs.size();
      if (offset + blob->size() > kMaxPackBytes) {
        *error = "the HDR pack would pass 4 GiB (blob offsets are 32 bit)";
        return false;
      }
      Put32(*out, uint32_t(offset));
      Put32(*out, uint32_t(blob->size()));
      Put64(*out, mcla::native_gfx::Fnv1a64(blob->data(), blob->size()));
      blobs.insert(blobs.end(), blob->begin(), blob->end());
    }
    if (out->size() - entry_start != kHdrPackEntryBytes) {
      *error = "internal error: an HDR pack entry is not 80 bytes";
      return false;
    }
  }
  out->insert(out->end(), blobs.begin(), blobs.end());
  return true;
}

std::string PathText(const std::filesystem::path& path) {
  const auto u8 = path.u8string();  // never throws, unlike path::string() on a non-ANSI path
  return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::string JoinProblems(const Problems& problems) {
  std::string text;
  for (const std::string& problem : problems) {
    if (!text.empty()) text += '\n';
    text += problem;
  }
  return text;
}

bool BytesContain(std::span<const uint8_t> bytes, std::string_view needle) {
  return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()).find(needle) !=
         std::string_view::npos;
}

class HdrStepImpl final : public HdrStep {
 public:
  explicit HdrStepImpl(std::string curve) : curve_(std::move(curve)), curve_xxh3_(Xxh3(curve_)) {}

  std::vector<StampLine> ExpectedStampLines() const override {
    return {
        {"hdr_step_version", std::to_string(kHdrStepVersion)},
        {"hdr_pack_version", std::to_string(kHdrPackVersion)},
        {"hdr_contract", std::to_string(kHdrContract)},
        {"hdr_hlsli_xxh3", Hex64(curve_xxh3_)},
    };
  }

  HdrStepResult Build(const HdrStepInput& input) override {
    HdrStepResult result;
    try {
      BuildChecked(input, &result);
    } catch (const std::exception& exception) {
      Fail(&result, std::string("the HDR variant could not be built: ") + exception.what());
    } catch (...) {
      Fail(&result, "the HDR variant could not be built: unknown exception");
    }
    return result;
  }

 private:
  static void Fail(HdrStepResult* result, std::string message) {
    result->built = false;
    result->message = std::move(message);
    result->stamp_lines.clear();
  }

  void BuildChecked(const HdrStepInput& input, HdrStepResult* result) const {
    const std::filesystem::path final_path = input.out_dir / kHdrPackFileName;
    const std::filesystem::path temporary_path = input.out_dir / (std::string(kHdrPackFileName) + ".tmp");
    {
      // Whatever HDR pack is there was built beside a main pack that is being replaced (or from an
      // older transform): it must not be read while this step runs or when it fails.
      std::error_code ec;
      std::filesystem::remove(final_path, ec);
      std::filesystem::remove(temporary_path, ec);
    }
    const auto fail = [&](std::string message) {
      std::error_code ec;
      std::filesystem::remove(temporary_path, ec);
      Fail(result, std::move(message));
    };

    const uint32_t formats = input.formats & (kDxil | kSpirv);
    if (!input.containers || !input.dxc || !input.sink) return fail("the HDR step was not given its inputs");
    if (formats == 0) return fail("no shader format was asked for");
    ProgressSink& sink = *input.sink;
    sink.BeginPhase("Preparing the HDR scene shader", 1);

    // 1. The shaders: the tone map and the vertex shaders it may be drawn with.
    std::string error;
    const ShaderSource* pixel = ResolveTarget(*input.containers, kPixelTarget, ShaderStage::kPixel, &error);
    if (!pixel) return fail(error);
    HdrPackEntry entry;
    entry.ps_key = pixel->key;
    std::string vertex_keys_text;
    for (size_t i = 0; i < std::size(kVertexTargets); ++i) {
      const ShaderSource* vertex = ResolveTarget(*input.containers, kVertexTargets[i], ShaderStage::kVertex, &error);
      if (!vertex) return fail(error);
      entry.vs_keys[i] = vertex->key;
      if (i != 0) vertex_keys_text += ',';
      vertex_keys_text += Hex64(vertex->key);
    }

    // 2. The tone map's HLSL (the translator's own text, as the pack's main build compiles it) and the edits.
    TranslatedShader translated;
    if (!TranslateContainer(pixel->container, &translated, &error)) return fail(pixel->name + ": " + error);
    if (translated.stage != ShaderStage::kPixel) return fail(pixel->name + " is not a pixel shader");
    const HdrTransformResult variant = TransformToneMapHlsl(translated.hlsl, curve_);
    if (!variant.ok) return fail(pixel->name + ": " + variant.error);
    if (sink.Cancelled()) return fail("The shader build was cancelled.");

    // 3. The blobs: spec variant 0 (no alpha test), the formats of the main pack, the same dxc
    // arguments as every other pixel shader of the pack (dxc.h).
    DxcStatus status;
    const std::unique_ptr<DxcWorker> worker = input.dxc->CreateWorker(&status);
    if (!worker) return fail("the shader compiler could not start a worker: " + status.message);
    const std::string label = std::format("{} [{}] (HDR variant)", pixel->name, Hex64(pixel->key));
    CompileRequest request;
    request.hlsl = variant.hlsl;
    request.stage = ShaderStage::kPixel;
    request.spec_constants = 0;
    request.name = label;
    if ((formats & kDxil) != 0) {
      status = worker->CompileDxil(request, &entry.dxil);
      if (!status.ok()) return fail(status.message);
    }
    if (sink.Cancelled()) return fail("The shader build was cancelled.");
    if ((formats & kSpirv) != 0) {
      status = worker->CompileSpirv(request, &entry.spirv);
      if (!status.ok()) return fail(status.message);
    }

    // 4. What the runtime relies on, read back from the blobs: the colour output is there, and no
    // coverage output, sample mask or specialization constant (this is the draw without alpha to coverage).
    if (!entry.dxil.empty() && !BytesContain(entry.dxil, "SV_Target")) {
      return fail(label + ": the DXIL has no SV_Target");
    }
    Problems problems;
    BlobContractInput blob;
    blob.name = label;
    blob.stage = ShaderStage::kPixel;
    blob.variant = 0;
    blob.writes_oc0 = false;
    blob.dxil = entry.dxil;
    blob.spirv = entry.spirv;
    if (!CheckBlobContract(blob, &problems)) return fail(JoinProblems(problems));

    // 5. The file, under a temporary name and then moved into place.
    std::vector<uint8_t> bytes;
    const HdrPackEntry entries[] = {entry};
    if (!LayOutHdrPack(entries, formats, &bytes, &error)) return fail(error);
    {
      std::ofstream file(temporary_path, std::ios::binary | std::ios::trunc);
      if (file) file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
      if (file) file.flush();
      if (!file) return fail("the HDR pack could not be written to " + PathText(temporary_path));
    }
    if (!CommitFile(temporary_path, final_path, &error)) return fail(error);

    // 6. What it was built from.
    result->built = true;
    result->message = std::format("{} (vs {}): DXIL {} bytes, SPIR-V {} bytes, {} bytes in all", label,
                                  vertex_keys_text, entry.dxil.size(), entry.spirv.size(), bytes.size());
    result->stamp_lines = ExpectedStampLines();
    // Informational (CheckStamp compares only the lines above).
    result->stamp_lines.push_back({"hdr_ps_key", Hex64(entry.ps_key)});
    result->stamp_lines.push_back({"hdr_vs_keys", vertex_keys_text});
    result->stamp_lines.push_back({"hdr_pack_xxh3", Hex64(Xxh3(bytes.data(), bytes.size()))});
    sink.Step(pixel->name);
    SHADERPACK_LOG_INFO("HDR variant pack: {}", result->message);
  }

  std::string curve_;
  uint64_t curve_xxh3_ = 0;
};

// The embedded curve, or empty text when this build has none.
std::string EmbeddedCurve() {
#ifdef LARECOMP_HDR_HLSLI_EMBEDDED
  return std::string(reinterpret_cast<const char*>(kEmbeddedCurve), sizeof(kEmbeddedCurve) - 1);
#else
  return std::string();
#endif
}

}  // namespace

std::unique_ptr<HdrStep> CreateHdrStepFromText(std::string_view hlsli_text) {
  if (hlsli_text.empty()) return nullptr;
  return std::make_unique<HdrStepImpl>(std::string(hlsli_text));
}

std::unique_ptr<HdrStep> CreateHdrStep() {
  return CreateHdrStepFromText(EmbeddedCurve());
}

std::string HdrStepUnavailableReason() {
  if (!EmbeddedCurve().empty()) return std::string();
#ifdef LARECOMP_HDR_HLSLI_EMBEDDED
  return "the embedded HDR curve file is empty";
#else
  return "this build has no HDR curve embedded (src/gpu/hlsl/hdr_tonemap_shoulder.hlsli was not in the "
         "source tree when CMake configured; LARECOMP_HDR_HLSLI can name another file)";
#endif
}

}  // namespace larecomp::shaderpack
