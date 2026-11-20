// LARecomp shader pack builder -- the pixel shader variant pack (see variant_pack.h).
// The HDR scene's step of the first version of this file was a port of native/tools/build_hdr_variants.py;
// the pack writer and the shader lookup are what it had, with a list of variants in front.

#include "shaderpack/variant_pack.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
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
#include "shaderpack/variant_chain.h"
#include "shaderpack/variant_compose.h"
#include "shaderpack/variant_overlap.h"
#include "shaderpack/variant_pack_format.h"
#include "shaderpack/variants/bloom_gauss.h"
#include "shaderpack/variants/car_chrome.h"
#include "shaderpack/variants/car_selfrefl.h"
#include "shaderpack/variants/env_map.h"
#include "shaderpack/variants/gbuffer.h"
#include "shaderpack/variants/glow_soft.h"
#include "shaderpack/variants/headlight.h"
#include "shaderpack/variants/hdr_scene.h"
#include "shaderpack/variants/luminance_scale.h"
#include "shaderpack/variants/shadow_filter.h"
#include "shaderpack/variants/ui_sharp.h"

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

// One entry to write (the file's layout and its writer are variant_pack_format.h's).
using PackEntry = VariantPackNewEntry;

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

// One compile of a variant's text for the formats asked (spec: the XENOS_SPEC_CONSTANTS value, 0 for spec variant 0), and what the runtime relies
// on, read back from the blobs: for a pixel shader the colour output is there, and the coverage output (SV_Coverage / the sample mask) exactly when
// the draw is the alpha test one (spec != 0), no specialization constant; for a vertex shader the position is invariant and no fast operation reaches
// it (the checks of the main pack's vertex shaders). Once per text: the same text gives the same blobs. False with *error.
bool CompileChecked(DxcWorker& worker, const std::function<bool()>& cancelled, uint32_t formats, ShaderStage stage, const std::string& hlsl,
                    uint32_t spec, const std::string& label, std::vector<uint8_t>* dxil, std::vector<uint8_t>* spirv, std::string* error) {
  CompileRequest request;
  request.hlsl = hlsl;
  request.stage = stage;
  request.spec_constants = spec;
  request.name = label;
  DxcStatus status;
  if ((formats & kDxil) != 0) {
    status = worker.CompileDxil(request, dxil);
    if (!status.ok()) {
      *error = status.message;
      return false;
    }
  }
  if (cancelled()) {
    *error = "The shader build was cancelled.";
    return false;
  }
  if ((formats & kSpirv) != 0) {
    status = worker.CompileSpirv(request, spirv);
    if (!status.ok()) {
      *error = status.message;
      return false;
    }
  }
  if (stage == ShaderStage::kPixel && !dxil->empty() && !BytesContain(*dxil, "SV_Target")) {
    *error = label + ": the DXIL has no SV_Target";
    return false;
  }
  Problems problems;
  BlobContractInput blob;
  blob.name = label;
  blob.stage = stage;
  blob.variant = spec != 0 ? 1 : 0;
  blob.writes_oc0 = stage == ShaderStage::kPixel && spec != 0;
  blob.dxil = *dxil;
  blob.spirv = *spirv;
  if (!CheckBlobContract(blob, &problems)) {
    *error = JoinProblems(problems);
    return false;
  }
  if (stage == ShaderStage::kVertex) {
    if (!spirv->empty()) CheckSpirvPosition(label, blob.variant, *spirv, &problems);
    if (!dxil->empty()) CheckDxilPosition(worker, label, blob.variant, *dxil, &problems);
    if (!problems.empty()) {
      *error = JoinProblems(problems);
      return false;
    }
  }
  return true;
}

struct Compiled {
  std::vector<uint8_t> dxil;
  std::vector<uint8_t> spirv;
};

// What one stage made for one shader on its own: the composition pass builds from these.
struct SoloShader {
  uint32_t feature = 0;
  uint64_t key = 0;
  bool vertex = false;
  uint32_t spec_mask = 0;  // the shader's spec mask
  VariantShader shader;
  const ShaderSource* source = nullptr;
};

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
    const ShaderStage stage = def.vertex ? ShaderStage::kVertex : ShaderStage::kPixel;
    for (const VariantShader& named : def.pixel_shaders) {
      const ShaderSource* pixel = ResolveShader(set, named, stage, error);
      if (!pixel) return false;
      targets->push_back({named, pixel});
    }
    for (const std::string_view suffix : def.pixel_name_suffixes) {
      for (const ShaderSource& shader : set.shaders) {
        if (shader.stage != stage || !std::string_view(shader.name).ends_with(suffix)) continue;
        const bool have = std::any_of(targets->begin(), targets->end(),
                                      [&](const Target& target) { return target.source->key == shader.key; });
        if (!have) targets->push_back({VariantShader{shader.name, shader.key}, &shader});
      }
    }
    if (targets->empty()) {
      *error = def.vertex ? "no vertex shader of the game is one it names" : "no pixel shader of the game is one it names";
      return false;
    }
    return true;
  }

  // One stage on its own: its vertex shaders, then each of its shaders translated, edited, compiled for the
  // formats asked and checked. False with *error (the first thing that went wrong) and nothing of it in
  // *entries; *note is a few words for the summary line (the compiles, the shaders left out); `solo` gets one
  // record per shader that was made, for the composition pass.
  bool BuildVariant(const VariantStepInput& input, DxcWorker& worker, const VariantDef& def, uint32_t formats,
                    std::vector<PackEntry>* entries, std::vector<SoloShader>* solo, std::string* note, std::string* error) const {
    ProgressSink& sink = *input.sink;
    const std::function<bool()> cancelled = [&sink] { return sink.Cancelled(); };
    if (!def.transform) {
      *error = "it has no transform";
      return false;
    }
    if (def.pixel_shaders.empty() && def.pixel_name_suffixes.empty()) {
      *error = def.vertex ? "it names no vertex shader" : "it names no pixel shader";
      return false;
    }
    const chain::FeatureInfo* row = chain::Find(def.feature);
    if (!row) {
      *error = std::format("its feature {} has no row in the chain's table (variant_chain.h kFeatures)", def.feature);
      return false;
    }
    if (row->name != def.name || (row->stage == chain::Stage::kVertex) != def.vertex) {
      *error = std::format("its feature {} is the table's {} ({} stage), not {} ({} stage)", def.feature, row->name,
                           row->stage == chain::Stage::kVertex ? "vertex" : "pixel", def.name, def.vertex ? "vertex" : "pixel");
      return false;
    }
    if (def.vertex_shaders.size() > kVariantVertexShaders) {
      *error = std::format("it names {} vertex shaders (at most {})", def.vertex_shaders.size(),
                           kVariantVertexShaders);
      return false;
    }
    const ShaderStage stage = def.vertex ? ShaderStage::kVertex : ShaderStage::kPixel;
    const char* const stage_text = def.vertex ? "vertex" : "pixel";

    // 1. The vertex shaders the pixel shaders are drawn with.
    std::array<uint64_t, kVariantVertexShaders> vs_keys{};
    for (size_t i = 0; i < def.vertex_shaders.size(); ++i) {
      const ShaderSource* vertex =
          ResolveShader(*input.containers, def.vertex_shaders[i], ShaderStage::kVertex, error);
      if (!vertex) return false;
      vs_keys[i] = vertex->key;
    }

    // 2. Each shader: the translator's own text, the edits, the blobs (spec variant 0: no alpha test,
    // the formats of the main pack, the same dxc arguments as every other shader of the pack, dxc.h).
    // Shaders whose edited text is the same are compiled once (the blobs depend on the text alone).
    std::vector<Target> targets;
    if (!ListTargets(*input.containers, def, &targets, error)) return false;
    std::unordered_map<std::string, Compiled> compiled;        // by the variant's HLSL
    std::unordered_map<std::string, Compiled> compiled_twin;   // the spec variant 1 twins: by the HLSL and the spec mask
    std::map<std::string, size_t> skipped;               // why a shader was left out -> how many
    std::vector<PackEntry> made;
    std::vector<SoloShader> solo_made;
    const StageContext context = SoloContext(def.feature);
    for (const Target& target : targets) {
      const ShaderSource* source = target.source;
      TranslatedShader translated;
      if (!TranslateContainer(source->container, &translated, error)) {
        *error = source->name + ": " + *error;
        return false;
      }
      if (translated.stage != stage) {
        *error = std::format("{} is not a {} shader", source->name, stage_text);
        return false;
      }
      const VariantTransformResult variant = def.transform(target.shader, translated.hlsl, context);
      if (variant.skip) {
        ++skipped[variant.error];
        sink.Step(source->name);
        continue;
      }
      if (!variant.ok) {
        *error = source->name + ": " + variant.error;
        return false;
      }
      if (sink.Cancelled()) {
        *error = "The shader build was cancelled.";
        return false;
      }

      PackEntry entry;
      entry.variant = chain::Word(chain::FeatureBit(def.feature), 0);
      entry.flags = def.vertex ? kVariantEntryVertex : 0;
      entry.key = source->key;
      entry.vs_keys = vs_keys;
      entry.vs_features = chain::VertexNeedsOf(chain::FeatureBit(def.feature));
      auto found = compiled.find(variant.hlsl);
      if (found == compiled.end()) {
        Compiled blobs;
        if (!CompileChecked(worker, cancelled, formats, stage, variant.hlsl, 0, std::format("{} [{}] ({} variant)", source->name, Hex64(source->key), def.name),
                            &blobs.dxil, &blobs.spirv, error)) {
          return false;
        }
        found = compiled.emplace(variant.hlsl, std::move(blobs)).first;
      }
      entry.dxil = found->second.dxil;
      entry.spirv = found->second.spirv;
      made.push_back(std::move(entry));
      // The spec variant 1 twin (the row's spec_twin): the same text compiled with the shader's own spec mask, for the draws that have the
      // alpha test or alpha to coverage on (a pixel shader), or the packed normal (a vertex shader). A shader with no spec mask has no
      // spec variant 1 in the main pack either, so it has no twin.
      if (row->spec_twin && translated.spec_mask != 0) {
        const std::string twin_key = variant.hlsl + std::format("\n//spec {}", translated.spec_mask);
        auto twin = compiled_twin.find(twin_key);
        if (twin == compiled_twin.end()) {
          Compiled blobs;
          if (!CompileChecked(worker, cancelled, formats, stage, variant.hlsl, translated.spec_mask,
                              std::format("{} [{}] ({} variant, spec variant 1)", source->name, Hex64(source->key), def.name), &blobs.dxil, &blobs.spirv,
                              error)) {
            return false;
          }
          twin = compiled_twin.emplace(twin_key, std::move(blobs)).first;
        }
        PackEntry at;
        at.variant = chain::Word(chain::FeatureBit(def.feature), 1);
        at.flags = def.vertex ? kVariantEntryVertex : 0;
        at.key = source->key;
        at.vs_keys = vs_keys;
        at.vs_features = chain::VertexNeedsOf(chain::FeatureBit(def.feature));
        at.dxil = twin->second.dxil;
        at.spirv = twin->second.spirv;
        made.push_back(std::move(at));
      }
      solo_made.push_back({def.feature, source->key, def.vertex, translated.spec_mask, target.shader, source});
      sink.Step(source->name);
    }
    if (made.empty()) {
      *error = std::format("none of its {} {} shaders is one its edits are for", targets.size(), stage_text);
      return false;
    }
    const uint32_t solo_word = chain::Word(chain::FeatureBit(def.feature), 0);
    const size_t shader_entries = size_t(std::count_if(made.begin(), made.end(), [&](const PackEntry& e) { return e.variant == solo_word; }));
    if (!skipped.empty() || compiled.size() != shader_entries || !compiled_twin.empty()) {
      const size_t compiles = compiled.size() + compiled_twin.size();
      *note = std::format("{} compile{}", compiles, compiles == 1 ? "" : "s");
      if (!compiled_twin.empty()) *note += std::format(", {} of them for spec variant 1", compiled_twin.size());
      for (const auto& [why, count] : skipped) *note += std::format("; {} left out: {}", count, why);
    }
    for (PackEntry& entry : made) entries->push_back(std::move(entry));
    for (SoloShader& s : solo_made) solo->push_back(std::move(s));
    return true;
  }

  // The compositions: for each shader that more than one stage made an entry for, the feature sets the chain's table allows
  // (variant_chain.h Permutations), each built like a variant: the stages applied in chain order to the translated text, compiled
  // and checked. A composition that cannot be made (a stage's anchors do not match after the stages before it, the text does not
  // compile) is left out with a warning; the stages themselves are in the pack alone. The texts are made here, in a fixed order; the
  // compiles (the cost: about 0.2 s of dxc for a lit shader's DXIL and SPIR-V) run on VariantStepInput::threads workers, one DxcWorker
  // each, and the entries are put together here in the same fixed order, so the pack does not depend on which worker finished first.
  // *compiles counts the distinct compiles done.
  void ComposePass(const VariantStepInput& input, DxcWorker& first_worker, uint32_t formats, const std::vector<SoloShader>& solo,
                   std::vector<PackEntry>* entries, std::vector<std::string>* left_out, size_t* composed, size_t* compiles) const {
    ProgressSink& sink = *input.sink;
    // Per shader (vertex shaders and pixel shaders have their own keys): the stages that made it alone.
    std::map<std::pair<bool, uint64_t>, std::vector<const SoloShader*>> by_shader;
    for (const SoloShader& s : solo) by_shader[{s.vertex, s.key}].push_back(&s);
    std::map<std::pair<bool, uint64_t>, chain::PermutationList> plans;
    for (const auto& [shader_key, stages] : by_shader) {
      if (stages.size() < 2) continue;
      uint32_t applicable = 0;
      for (const SoloShader* s : stages) applicable |= chain::FeatureBit(s->feature);
      chain::PermutationList plan = chain::Permutations(applicable, stages.front()->spec_mask != 0);
      if (std::any_of(plan.sets.begin(), plan.sets.end(), [](uint32_t set) { return std::popcount(set) >= 2; }) || plan.dropped != 0) {
        plans[shader_key] = std::move(plan);
      }
    }
    if (plans.empty()) return;

    // 1. The texts and the distinct compiles they need.
    struct Job {
      std::string hlsl;
      ShaderStage stage = ShaderStage::kPixel;
      uint32_t spec = 0;
      std::string label;
      Compiled blobs;
      std::string error;
      bool ok = false;
    };
    struct Planned {
      uint64_t key = 0;
      bool vertex = false;
      uint32_t set = 0;
      std::string name;
      std::vector<std::pair<uint32_t, size_t>> words;  // (spec bit 0 or 1, job)
    };
    std::vector<Job> jobs;
    std::map<std::string, size_t> job_of;  // by the text and the spec mask
    std::vector<Planned> planned;
    for (const auto& [shader_key, plan] : plans) {
      const SoloShader& first = *by_shader[shader_key].front();
      if (plan.dropped != 0) {
        SHADERPACK_LOG_WARN("shader variants: {} compositions of {} were left out (more than {} stages in one text or {} variants of one shader)",
                            plan.dropped, first.source->name, chain::kMaxStagesPerShader, chain::kMaxWordsPerShader);
      }
      TranslatedShader translated;
      std::string error;
      if (!TranslateContainer(first.source->container, &translated, &error)) {
        left_out->push_back(first.source->name + ": " + error);
        continue;
      }
      const ShaderStage stage = first.vertex ? ShaderStage::kVertex : ShaderStage::kPixel;
      for (const uint32_t set : plan.sets) {
        if (std::popcount(set) < 2) continue;
        if (sink.Cancelled()) return;
        const VariantTransformResult text = ComposeStages(variants_, set, first.shader, translated.hlsl);
        if (!text.ok) {
          const std::string why = std::format("{} with the features {:#x}: {}", first.source->name, set, text.error);
          SHADERPACK_LOG_WARN("shader variants: a composition was not built: {}", why);
          left_out->push_back(why);
          continue;
        }
        // Words: the set alone (spec 0), and its twin when the shader has a spec variant and every stage of the set has one.
        std::vector<uint32_t> specs = {0};
        if (translated.spec_mask != 0 && chain::HasTwin(set)) specs.push_back(translated.spec_mask);
        Planned plan_entry{first.key, first.vertex, set, first.source->name, {}};
        for (const uint32_t spec_mask : specs) {
          const std::string cache_key = text.hlsl + std::format("\n//spec {}", spec_mask);
          auto found = job_of.find(cache_key);
          if (found == job_of.end()) {
            Job job;
            job.hlsl = text.hlsl;
            job.stage = stage;
            job.spec = spec_mask;
            job.label = std::format("{} [{}] (features {:#x}{})", first.source->name, Hex64(first.key), set, spec_mask != 0 ? ", spec variant 1" : "");
            found = job_of.emplace(cache_key, jobs.size()).first;
            jobs.push_back(std::move(job));
          }
          plan_entry.words.emplace_back(spec_mask != 0 ? 1u : 0u, found->second);
        }
        planned.push_back(std::move(plan_entry));
      }
    }
    if (jobs.empty()) return;

    // 2. The compiles, on the step's threads (this thread is one of them while the others start: the first worker is the one the step made).
    sink.BeginPhase("Composing the shader variants", int(jobs.size()));
    const unsigned threads = std::max(1u, std::min<unsigned>(input.threads, unsigned(jobs.size())));
    std::atomic<size_t> next{0};
    std::atomic<size_t> finished{0};
    std::atomic<bool> stop{false};
    std::atomic<unsigned> running{threads};
    const std::function<bool()> stopped = [&stop] { return stop.load(std::memory_order_relaxed); };
    const auto work = [&](DxcWorker* given) {
      std::unique_ptr<DxcWorker> own;
      DxcWorker* worker = given;
      if (!worker) {
        DxcStatus status;
        own = input.dxc->CreateWorker(&status);
        worker = own.get();
      }
      for (;;) {
        const size_t index = worker ? next.fetch_add(1) : jobs.size();
        if (index >= jobs.size() || stop.load(std::memory_order_relaxed)) break;
        Job& job = jobs[index];
        try {
          job.ok = CompileChecked(*worker, stopped, formats, job.stage, job.hlsl, job.spec, job.label, &job.blobs.dxil, &job.blobs.spirv, &job.error);
        } catch (const std::exception& exception) {
          job.ok = false;
          job.error = std::string("exception: ") + exception.what();
        }
        finished.fetch_add(1);
      }
      own.reset();
      running.fetch_sub(1);
    };
    std::vector<std::thread> pool;
    for (unsigned i = 1; i < threads; ++i) pool.emplace_back(work, nullptr);
    std::thread main_worker(work, &first_worker);  // a DxcWorker is used by one thread at a time: this one's, while this function polls
    size_t reported = 0;
    bool cancelled = false;
    while (running.load() != 0) {
      if (!cancelled && sink.Cancelled()) {
        cancelled = true;
        stop.store(true);
      }
      for (const size_t done = finished.load(); reported < done; ++reported) sink.Step(jobs[std::min(reported, jobs.size() - 1)].label);
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    main_worker.join();
    for (std::thread& thread : pool) thread.join();
    for (const size_t done = finished.load(); reported < done; ++reported) sink.Step(jobs[std::min(reported, jobs.size() - 1)].label);
    if (cancelled) return;  // the caller sees sink.Cancelled() and fails the step

    // 3. The entries, in the order the compositions were planned.
    for (const Planned& plan_entry : planned) {
      bool ok = true;
      for (const auto& [spec, job] : plan_entry.words) {
        if (!jobs[job].ok) {
          const std::string why = std::format("{} with the features {:#x}: {}", plan_entry.name, plan_entry.set, jobs[job].error);
          SHADERPACK_LOG_WARN("shader variants: a composition was not built: {}", why);
          left_out->push_back(why);
          ok = false;
          break;
        }
      }
      if (!ok) continue;
      for (const auto& [spec, job] : plan_entry.words) {
        PackEntry entry;
        entry.variant = chain::Word(plan_entry.set, spec);
        entry.flags = plan_entry.vertex ? kVariantEntryVertex : 0;
        entry.key = plan_entry.key;
        entry.vs_features = chain::VertexNeedsOf(plan_entry.set);
        entry.dxil = jobs[job].blobs.dxil;
        entry.spirv = jobs[job].blobs.spirv;
        entries->push_back(std::move(entry));
      }
      ++*composed;
    }
    *compiles = size_t(std::count_if(jobs.begin(), jobs.end(), [](const Job& job) { return job.ok; }));
  }

  void BuildChecked(const VariantStepInput& input, VariantStepResult* result) const {
    const std::filesystem::path final_path = input.out_dir / kVariantPackFileName;
    const std::filesystem::path temporary_path = input.out_dir / (std::string(kVariantPackFileName) + ".tmp");
    {
      // Whatever variant pack is there was built beside a main pack that is being replaced (or from
      // older edits): it must not be read while this step runs or when it fails. Nor the first and second
      // versions' files, which nothing reads any more (the reader refuses a file of another version anyway).
      std::error_code ec;
      std::filesystem::remove(final_path, ec);
      std::filesystem::remove(temporary_path, ec);
      std::filesystem::remove(input.out_dir / kLegacyHdrPackFileName, ec);
      std::filesystem::remove(input.out_dir / kLegacyVariantPackFileName, ec);
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
    std::vector<VariantTarget> made_targets;  // every (variant, shader) entry, for the overlap check
    std::vector<SoloShader> solo;             // every shader a stage made alone, for the composition pass
    for (const VariantDef& def : variants_) {
      std::string error;
      std::string note;
      std::vector<PackEntry> made;
      if (BuildVariant(input, *worker, def, formats, &made, &solo, &note, &error)) {
        AddVariantLines(def, &variant_lines);
        std::string keys;
        size_t shader_count_made = 0;  // the shaders, not the entries (an alpha test twin is the same shader)
        for (const PackEntry& entry : made) {
          if (entry.variant != chain::Word(chain::FeatureBit(def.feature), 0)) continue;
          if (!keys.empty()) keys += ',';
          keys += Hex64(entry.key);
          ++shader_count_made;
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
        built_text += std::format("{} ({} shader{}{}{})", def.name, shader_count_made, shader_count_made == 1 ? "" : "s",
                                  note.empty() ? "" : "; ", note);
        for (PackEntry& entry : made) {
          made_targets.push_back({def.name, entry.key});
          entries.push_back(std::move(entry));
        }
      } else {
        if (sink.Cancelled()) return fail("The shader build was cancelled.");
        // A stage that failed has no solo records either (BuildVariant adds them only when it succeeds), so no composition of it is tried.
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

    // 1b. The compositions of the stages that edit the same shader and that the chain's table lets share a text.
    size_t composed = 0;
    size_t composed_compiles = 0;
    std::vector<std::string> composition_left_out;
    ComposePass(input, *worker, formats, solo, &entries, &composition_left_out, &composed, &composed_compiles);
    if (sink.Cancelled()) return fail("The shader build was cancelled.");
    variant_lines.push_back({std::string(kStampVariantsPrefix) + "composed", std::to_string(composed)});  // informational
    if (!composition_left_out.empty()) {
      std::string reasons;
      for (const std::string& why : composition_left_out) reasons += (reasons.empty() ? "" : "; ") + why;
      variant_lines.push_back({std::string(kStampVariantsPrefix) + "composed_left_out", reasons});  // informational
    }

    // A pixel shader that two variants made an entry for and that the chain does not compose is drawn with one of them only
    // (variant_overlap.h): say so. (A pair the table composes has its composed entries from the pass above.)
    std::string overlap_text;
    for (const VariantOverlap& overlap : FindVariantOverlaps(made_targets)) {
      uint32_t set = 0;
      for (const std::string& name : overlap.variants) {
        for (const VariantDef& def : variants_) {
          if (def.name == name) set |= chain::FeatureBit(def.feature);
        }
      }
      if (chain::CanCompose(set)) continue;
      if (!overlap_text.empty()) overlap_text += "; ";
      overlap_text += DescribeVariantOverlap(overlap);
    }
    if (!overlap_text.empty()) {
      SHADERPACK_LOG_WARN("shader variants: a pixel shader is in more than one variant that the chain does not compose, and the renderer draws only one of "
                          "them per draw (variant_chain.h kFeatures composes_with): {}", overlap_text);
    }

    // 2. The file: entries in increasing (variant, pixel shader) order, under a temporary name and then
    // moved into place.
    std::sort(entries.begin(), entries.end(), [](const PackEntry& a, const PackEntry& b) {
      return variant_pack_detail::SortKey(a.vertex(), a.variant, a.key) < variant_pack_detail::SortKey(b.vertex(), b.variant, b.key);
    });
    for (size_t i = 1; i < entries.size(); ++i) {
      if (entries[i].vertex() == entries[i - 1].vertex() && entries[i].variant == entries[i - 1].variant && entries[i].key == entries[i - 1].key) {
        return fail(std::format("variant {} has two entries for the shader {}", entries[i].variant, Hex64(entries[i].key)));
      }
    }
    std::string error;
    std::vector<uint8_t> bytes;
    if (!LayOutVariantPack(entries, formats, kPackVersion, &mcla::native_gfx::Fnv1a64, &bytes, &error)) return fail(error);
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
      if (composed != 0) result->message += std::format("; {} compositions ({} extra compiles)", composed, composed_compiles);
      if (!composition_left_out.empty()) result->message += std::format("; {} compositions left out", composition_left_out.size());
      if (!left_out_text.empty()) result->message += "; left out: " + left_out_text;
      if (!overlap_text.empty()) result->message += "; in more than one variant (only one is drawn): " + overlap_text;
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
  variants.push_back(MakeCarChromeVariant());
  variants.push_back(MakeLuminanceScaleVariant());
  variants.push_back(MakeUiSharpVariant());
  variants.push_back(MakeHeadlightVariant());
  VariantDef gbuffer;
  if (MakeGBufferVariant(&gbuffer, &why)) {
    variants.push_back(std::move(gbuffer));
  } else if (left_out) {
    left_out->push_back("gbuffer: " + why);
  }
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
