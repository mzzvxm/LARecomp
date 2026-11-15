// LARecomp shader pack builder -- the pixel shader variant pack (see variant_pack.h).
// The HDR scene's step of the first version of this file was a port of native/tools/build_hdr_variants.py;
// the pack writer and the shader lookup are what it had, with a list of variants in front.

#include "shaderpack/variant_pack.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "native_gfx/shader_identity.h"
#include "shaderpack/contract_checks.h"
#include "shaderpack/hash.h"
#include "shaderpack/hlsl_source.h"
#include "shaderpack/log.h"
#include "shaderpack/pack_format.h"
#include "shaderpack/pack_writer.h"
#include "shaderpack/variants/bloom_gauss.h"
#include "shaderpack/variants/car_selfrefl.h"
#include "shaderpack/variants/env_map.h"
#include "shaderpack/variants/glow_soft.h"
#include "shaderpack/variants/hdr_scene.h"
#include "shaderpack/variants/shadow_filter.h"

namespace larecomp::shaderpack {

namespace {

// By identity first; when the game is not the retail one, by name, which has to be unique.
// The name fallback looks in every container read (the Python builder looked in the executable's
// only), so a second shader of the same name in the RPF makes it fail where the Python would not.
// The identity lookup comes first and is what decides on the retail game.
const ShaderSource* ResolveShader(const ContainerSet& set, const VariantShader& target, ShaderStage stage,
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
  SHADERPACK_LOG_WARN("shader variants: {} was found by its name ({}), not by its known identity {}", target.name,
                      Hex64(found->key), Hex64(target.key));
  return found;
}

void Put32(std::vector<uint8_t>& out, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out.push_back(uint8_t(value >> shift));
}

void Put64(std::vector<uint8_t>& out, uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) out.push_back(uint8_t(value >> shift));
}

struct PackEntry {
  uint32_t variant = 0;
  uint64_t ps_key = 0;
  std::array<uint64_t, kVariantVertexShaders> vs_keys{};  // 0 = none
  std::vector<uint8_t> dxil;                              // empty: the format was not built
  std::vector<uint8_t> spirv;
};

// header | entries | blobs (pack_format.h has the layout). `entries` are in increasing (variant, ps key)
// order. False with *error when a size does not fit.
bool LayOutVariantPack(std::span<const PackEntry> entries, uint32_t formats, std::vector<uint8_t>* out,
                       std::string* error) {
  out->clear();
  out->insert(out->end(), kVariantPackMagic, kVariantPackMagic + 4);
  Put32(*out, kVariantPackVersion);
  Put32(*out, uint32_t(entries.size()));
  Put32(*out, formats);
  Put32(*out, kVariantContract);
  Put32(*out, kPackVersion);  // the main pack the variants were built beside
  Put32(*out, 0);
  Put32(*out, 0);

  const uint64_t blob_base = uint64_t(kVariantPackHeaderBytes) + uint64_t(entries.size()) * kVariantPackEntryBytes;
  std::vector<uint8_t> blobs;
  // A blob that is byte for byte one already laid out is not written twice: the entries point at one copy
  // (the shadow collector's shaders share a text per body shape, so 147 entries hold four pairs of blobs).
  std::map<std::pair<uint64_t, size_t>, std::vector<std::pair<uint64_t, size_t>>> laid_out;  // (fnv, size) -> (at, size)
  for (const PackEntry& entry : entries) {
    const size_t entry_start = out->size();
    Put64(*out, entry.ps_key);
    for (const uint64_t vs_key : entry.vs_keys) Put64(*out, vs_key);
    Put32(*out, entry.variant);
    Put32(*out, 0);  // reserved
    for (const std::vector<uint8_t>* blob : {&entry.dxil, &entry.spirv}) {
      if (blob->empty()) {
        Put32(*out, 0);
        Put32(*out, 0);
        Put64(*out, 0);
        continue;
      }
      const uint64_t fnv = mcla::native_gfx::Fnv1a64(blob->data(), blob->size());
      uint64_t offset = 0;
      bool shared = false;
      std::vector<std::pair<uint64_t, size_t>>& same = laid_out[{fnv, blob->size()}];
      for (const auto& placed : same) {
        if (std::equal(blob->begin(), blob->end(), blobs.begin() + std::ptrdiff_t(placed.first))) {
          offset = blob_base + placed.first;
          shared = true;
          break;
        }
      }
      if (!shared) {
        offset = blob_base + blobs.size();
        if (offset + blob->size() > kMaxPackBytes) {
          *error = "the variant pack would pass 4 GiB (blob offsets are 32 bit)";
          return false;
        }
        same.emplace_back(blobs.size(), blob->size());
        blobs.insert(blobs.end(), blob->begin(), blob->end());
      }
      Put32(*out, uint32_t(offset));
      Put32(*out, uint32_t(blob->size()));
      Put64(*out, fnv);
    }
    if (out->size() - entry_start != kVariantPackEntryBytes) {
      *error = "internal error: a variant pack entry is not 80 bytes";
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

std::string StampKey(const VariantDef& def, std::string_view what) {
  return std::string(kStampVariantsPrefix) + def.name + "_" + std::string(what);
}

// The lines that are true of this build for one variant: the version of its edits and the fingerprint of
// what else its text depends on. A variant that was tried and left out has them too (and a left_out line
// with the reason, AddLeftOutLine): the stamp says "this build's edits were tried on these game files", so
// the next start is not stale. The edits fail the same way on the same files; the failure is tried again
// when the variant's version changes (a new build with other edits) or the game files do.
void AddVariantLines(const VariantDef& def, std::vector<StampLine>* lines) {
  lines->push_back({StampKey(def, "version"), std::to_string(def.version)});
  if (def.input_xxh3 != 0) lines->push_back({StampKey(def, "input"), Hex64(def.input_xxh3)});
}

void AddLeftOutLine(const VariantDef& def, std::string_view why, std::vector<StampLine>* lines) {
  lines->push_back({StampKey(def, kStampVariantLeftOut), std::string(why)});  // informational
}

class VariantStepImpl final : public VariantStep {
 public:
  explicit VariantStepImpl(std::vector<VariantDef> variants) : variants_(std::move(variants)) {}

  std::vector<StampLine> ExpectedStampLines() const override {
    std::vector<StampLine> lines = StepLines();
    for (const VariantDef& def : variants_) AddVariantLines(def, &lines);
    return lines;
  }

  bool NeedsRpfShaders() const override {
    return std::any_of(variants_.begin(), variants_.end(), [](const VariantDef& def) { return def.needs_rpf; });
  }

  VariantStepResult Build(const VariantStepInput& input) override {
    VariantStepResult result;
    try {
      BuildChecked(input, &result);
    } catch (const std::exception& exception) {
      Fail(&result, std::string("the shader variants could not be built: ") + exception.what());
    } catch (...) {
      Fail(&result, "the shader variants could not be built: unknown exception");
    }
    return result;
  }

 private:
  static std::vector<StampLine> StepLines() {
    return {
        {std::string(kStampVariantsPrefix) + "step_version", std::to_string(kVariantStepVersion)},
        {std::string(kStampVariantsPrefix) + "pack_version", std::to_string(kVariantPackVersion)},
        {std::string(kStampVariantsPrefix) + "contract", std::to_string(kVariantContract)},
    };
  }

  static void Fail(VariantStepResult* result, std::string message) {
    result->built = false;
    result->message = std::move(message);
    result->stamp_lines.clear();
  }

  // The pixel shaders of a variant: the ones it names by identity (found by name when the game is not the
  // retail one), then every one whose name ends with one of its suffixes, in key order (the container set
  // is sorted by key), each once. False with *error when a named shader is not there.
  struct Target {
    VariantShader shader;
    const ShaderSource* source = nullptr;
  };
  static bool ListTargets(const ContainerSet& set, const VariantDef& def, std::vector<Target>* targets,
                          std::string* error) {
    targets->clear();
    for (const VariantShader& named : def.pixel_shaders) {
      const ShaderSource* pixel = ResolveShader(set, named, ShaderStage::kPixel, error);
      if (!pixel) return false;
      targets->push_back({named, pixel});
    }
    for (const std::string_view suffix : def.pixel_name_suffixes) {
      for (const ShaderSource& shader : set.shaders) {
        if (shader.stage != ShaderStage::kPixel || !std::string_view(shader.name).ends_with(suffix)) continue;
        const bool have = std::any_of(targets->begin(), targets->end(),
                                      [&](const Target& target) { return target.source->key == shader.key; });
        if (!have) targets->push_back({VariantShader{shader.name, shader.key}, &shader});
      }
    }
    if (targets->empty()) {
      *error = "no pixel shader of the game is one it names";
      return false;
    }
    return true;
  }

  // One variant: its vertex shaders, then each of its pixel shaders translated, edited, compiled for the
  // formats asked and checked. False with *error (the first thing that went wrong) and nothing of it in
  // *entries; *note is a few words for the summary line (the compiles, the shaders left out).
  bool BuildVariant(const VariantStepInput& input, DxcWorker& worker, const VariantDef& def, uint32_t formats,
                    std::vector<PackEntry>* entries, std::string* note, std::string* error) const {
    ProgressSink& sink = *input.sink;
    if (!def.transform) {
      *error = "it has no transform";
      return false;
    }
    if (def.pixel_shaders.empty() && def.pixel_name_suffixes.empty()) {
      *error = "it names no pixel shader";
      return false;
    }
    if (def.id < kFirstPackedVariant || def.id >= kVariantSlots) {
      *error = std::format("its id {} is not one of the variant pack's ({}..{})", def.id, kFirstPackedVariant,
                           kVariantSlots - 1);
      return false;
    }
    if (def.vertex_shaders.size() > kVariantVertexShaders) {
      *error = std::format("it names {} vertex shaders (at most {})", def.vertex_shaders.size(),
                           kVariantVertexShaders);
      return false;
    }

    // 1. The vertex shaders the pixel shaders are drawn with.
    std::array<uint64_t, kVariantVertexShaders> vs_keys{};
    for (size_t i = 0; i < def.vertex_shaders.size(); ++i) {
      const ShaderSource* vertex =
          ResolveShader(*input.containers, def.vertex_shaders[i], ShaderStage::kVertex, error);
      if (!vertex) return false;
      vs_keys[i] = vertex->key;
    }

    // 2. Each pixel shader: the translator's own text, the edits, the blobs (spec variant 0: no alpha test,
    // the formats of the main pack, the same dxc arguments as every other pixel shader of the pack, dxc.h).
    // Shaders whose edited text is the same are compiled once (the blobs depend on the text alone).
    std::vector<Target> targets;
    if (!ListTargets(*input.containers, def, &targets, error)) return false;
    struct Compiled {
      std::vector<uint8_t> dxil;
      std::vector<uint8_t> spirv;
    };
    std::unordered_map<std::string, Compiled> compiled;  // by the variant's HLSL
    std::map<std::string, size_t> skipped;               // why a shader was left out -> how many
    std::vector<PackEntry> made;
    for (const Target& target : targets) {
      const ShaderSource* pixel = target.source;
      TranslatedShader translated;
      if (!TranslateContainer(pixel->container, &translated, error)) {
        *error = pixel->name + ": " + *error;
        return false;
      }
      if (translated.stage != ShaderStage::kPixel) {
        *error = pixel->name + " is not a pixel shader";
        return false;
      }
      const VariantTransformResult variant = def.transform(target.shader, translated.hlsl);
      if (variant.skip) {
        ++skipped[variant.error];
        sink.Step(pixel->name);
        continue;
      }
      if (!variant.ok) {
        *error = pixel->name + ": " + variant.error;
        return false;
      }
      if (sink.Cancelled()) {
        *error = "The shader build was cancelled.";
        return false;
      }

      PackEntry entry;
      entry.variant = def.id;
      entry.ps_key = pixel->key;
      entry.vs_keys = vs_keys;
      auto found = compiled.find(variant.hlsl);
      if (found == compiled.end()) {
        Compiled blobs;
        const std::string label = std::format("{} [{}] ({} variant)", pixel->name, Hex64(pixel->key), def.name);
        CompileRequest request;
        request.hlsl = variant.hlsl;
        request.stage = ShaderStage::kPixel;
        request.spec_constants = 0;
        request.name = label;
        DxcStatus status;
        if ((formats & kDxil) != 0) {
          status = worker.CompileDxil(request, &blobs.dxil);
          if (!status.ok()) {
            *error = status.message;
            return false;
          }
        }
        if (sink.Cancelled()) {
          *error = "The shader build was cancelled.";
          return false;
        }
        if ((formats & kSpirv) != 0) {
          status = worker.CompileSpirv(request, &blobs.spirv);
          if (!status.ok()) {
            *error = status.message;
            return false;
          }
        }

        // 3. What the runtime relies on, read back from the blobs: the colour output is there, and no
        // coverage output, sample mask or specialization constant (this is the draw without alpha to
        // coverage). Once per text: the same text gives the same blobs.
        if (!blobs.dxil.empty() && !BytesContain(blobs.dxil, "SV_Target")) {
          *error = label + ": the DXIL has no SV_Target";
          return false;
        }
        Problems problems;
        BlobContractInput blob;
        blob.name = label;
        blob.stage = ShaderStage::kPixel;
        blob.variant = 0;
        blob.writes_oc0 = false;
        blob.dxil = blobs.dxil;
        blob.spirv = blobs.spirv;
        if (!CheckBlobContract(blob, &problems)) {
          *error = JoinProblems(problems);
          return false;
        }
        found = compiled.emplace(variant.hlsl, std::move(blobs)).first;
      }
      entry.dxil = found->second.dxil;
      entry.spirv = found->second.spirv;
      sink.Step(pixel->name);
      made.push_back(std::move(entry));
    }
    if (made.empty()) {
      *error = std::format("none of its {} pixel shaders is one its edits are for", targets.size());
      return false;
    }
    if (!skipped.empty() || compiled.size() != made.size()) {
      *note = std::format("{} compile{}", compiled.size(), compiled.size() == 1 ? "" : "s");
      for (const auto& [why, count] : skipped) *note += std::format("; {} left out: {}", count, why);
    }
    for (PackEntry& entry : made) entries->push_back(std::move(entry));
    return true;
  }

  void BuildChecked(const VariantStepInput& input, VariantStepResult* result) const {
    const std::filesystem::path final_path = input.out_dir / kVariantPackFileName;
    const std::filesystem::path temporary_path = input.out_dir / (std::string(kVariantPackFileName) + ".tmp");
    {
      // Whatever variant pack is there was built beside a main pack that is being replaced (or from
      // older edits): it must not be read while this step runs or when it fails. Nor the first version's
      // file, which nothing reads any more.
      std::error_code ec;
      std::filesystem::remove(final_path, ec);
      std::filesystem::remove(temporary_path, ec);
      std::filesystem::remove(input.out_dir / kLegacyHdrPackFileName, ec);
    }
    const auto fail = [&](std::string message) {
      std::error_code ec;
      std::filesystem::remove(temporary_path, ec);
      Fail(result, std::move(message));
    };

    const uint32_t formats = input.formats & (kDxil | kSpirv);
    if (!input.containers || !input.dxc || !input.sink) return fail("the variant step was not given its inputs");
    if (formats == 0) return fail("no shader format was asked for");
    if (variants_.empty()) return fail("this build has no shader variants");
    ProgressSink& sink = *input.sink;
    size_t shader_count = 0;
    for (const VariantDef& def : variants_) {
      std::vector<Target> targets;
      std::string ignored;
      shader_count += ListTargets(*input.containers, def, &targets, &ignored) ? targets.size()
                                                                              : def.pixel_shaders.size();
    }
    sink.BeginPhase("Preparing the shader variants", int(shader_count));

    DxcStatus status;
    const std::unique_ptr<DxcWorker> worker = input.dxc->CreateWorker(&status);
    if (!worker) return fail("the shader compiler could not start a worker: " + status.message);

    // 1. Every variant on its own: one that cannot be made (its edit does not match this game's text, its
    // text does not compile) is left out and said so; the others still go in.
    std::vector<PackEntry> entries;
    std::vector<StampLine> variant_lines;
    std::string built_text;
    std::string left_out_text;
    for (const VariantDef& def : variants_) {
      std::string error;
      std::string note;
      std::vector<PackEntry> made;
      if (BuildVariant(input, *worker, def, formats, &made, &note, &error)) {
        AddVariantLines(def, &variant_lines);
        std::string keys;
        for (const PackEntry& entry : made) {
          if (!keys.empty()) keys += ',';
          keys += Hex64(entry.ps_key);
        }
        variant_lines.push_back({StampKey(def, "shaders"), keys});  // informational
        if (!made.empty() && made.front().vs_keys[0] != 0) {
          std::string vertex_keys;
          for (const uint64_t key : made.front().vs_keys) {
            if (key == 0) continue;
            if (!vertex_keys.empty()) vertex_keys += ',';
            vertex_keys += Hex64(key);
          }
          variant_lines.push_back({StampKey(def, "vs_keys"), vertex_keys});  // informational
        }
        if (!built_text.empty()) built_text += ", ";
        built_text += std::format("{} ({} shader{}{}{})", def.name, made.size(), made.size() == 1 ? "" : "s",
                                  note.empty() ? "" : "; ", note);
        for (PackEntry& entry : made) entries.push_back(std::move(entry));
      } else {
        if (sink.Cancelled()) return fail("The shader build was cancelled.");
        SHADERPACK_LOG_WARN("the shader variant {} was not built: {}", def.name, error);
        if (!left_out_text.empty()) left_out_text += "; ";
        left_out_text += def.name + ": " + error;
        // Stamped like a built one, with the reason: a variant that cannot be made from these game files
        // would otherwise leave the stamp one line short and make every start rebuild the packs.
        AddVariantLines(def, &variant_lines);
        AddLeftOutLine(def, error, &variant_lines);
      }
    }
    // When no variant could be made (a game whose shader text none of the edits fit) the pack is written with
    // no entries: the renderer draws the game's own shaders and the stamp is current, instead of every start
    // translating the shaders again.

    // 2. The file: entries in increasing (variant, pixel shader) order, under a temporary name and then
    // moved into place.
    std::sort(entries.begin(), entries.end(), [](const PackEntry& a, const PackEntry& b) {
      return a.variant != b.variant ? a.variant < b.variant : a.ps_key < b.ps_key;
    });
    for (size_t i = 1; i < entries.size(); ++i) {
      if (entries[i].variant == entries[i - 1].variant && entries[i].ps_key == entries[i - 1].ps_key) {
        return fail(std::format("variant {} has two entries for the shader {}", entries[i].variant,
                                Hex64(entries[i].ps_key)));
      }
    }
    std::string error;
    std::vector<uint8_t> bytes;
    if (!LayOutVariantPack(entries, formats, &bytes, &error)) return fail(error);
    {
      std::ofstream file(temporary_path, std::ios::binary | std::ios::trunc);
      if (file) file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
      if (file) file.flush();
      if (!file) return fail("the variant pack could not be written to " + PathText(temporary_path));
    }
    if (!CommitFile(temporary_path, final_path, &error)) return fail(error);

    // 3. What it was built from: the lines of the step and of every variant that was tried, built or left out
    // (a left out one also has its reason: see AddVariantLines).
    size_t dxil_bytes = 0;
    size_t spirv_bytes = 0;
    for (const PackEntry& entry : entries) {
      dxil_bytes += entry.dxil.size();
      spirv_bytes += entry.spirv.size();
    }
    result->built = true;
    if (entries.empty()) {
      result->message = "no shader variant could be built (the game's own shaders are drawn): " + left_out_text;
    } else {
      result->message = std::format("{} shaders in the variants {}: DXIL {} bytes, SPIR-V {} bytes, {} bytes in all",
                                    entries.size(), built_text, dxil_bytes, spirv_bytes, bytes.size());
      if (!left_out_text.empty()) result->message += "; left out: " + left_out_text;
    }
    result->stamp_lines = StepLines();
    for (StampLine& line : variant_lines) result->stamp_lines.push_back(std::move(line));
    // Informational (CheckStamp compares only the lines it expects).
    result->stamp_lines.push_back(
        {std::string(kStampVariantsPrefix) + "pack_xxh3", Hex64(Xxh3(bytes.data(), bytes.size()))});
    SHADERPACK_LOG_INFO("shader variant pack: {}", result->message);
  }

  std::vector<VariantDef> variants_;
};

}  // namespace

std::vector<VariantDef> BuiltInVariants(std::vector<std::string>* left_out) {
  std::vector<VariantDef> variants;
  VariantDef hdr;
  std::string why;
  if (MakeHdrSceneVariant(&hdr, &why)) {
    variants.push_back(std::move(hdr));
  } else if (left_out) {
    left_out->push_back("hdr_scene: " + why);
  }
  variants.push_back(MakeBloomGaussVariant());
  variants.push_back(MakeEnvMapVariant());
  variants.push_back(MakeShadowFilterVariant());
  variants.push_back(MakeGlowSoftVariant());
  variants.push_back(MakeCarSelfReflVariant());
  return variants;
}

std::unique_ptr<VariantStep> CreateVariantStep(std::vector<VariantDef> variants) {
  if (variants.empty()) return nullptr;
  return std::make_unique<VariantStepImpl>(std::move(variants));
}

std::unique_ptr<VariantStep> CreateBuiltInVariantStep(std::vector<std::string>* left_out) {
  return CreateVariantStep(BuiltInVariants(left_out));
}

}  // namespace larecomp::shaderpack
