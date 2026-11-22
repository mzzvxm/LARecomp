// MCLA GPU renderer -- the ray tracing micro-benchmark, the GPU side. See rt_bench.h and rt_bench_rules.h.

#include "rt_bench.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <plume_render_interface.h>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>
#include <rex/path_settings.h>

#include "backend.h"
#include "device.h"
#include "host_shaders.h"
#include "rt_tier.h"

namespace mcla::gpu::rt_bench {

namespace {

constexpr const char* kPrefix = "[gpu] rt bench:";
constexpr const char* kCacheFile = "rt_bench.txt";
constexpr uint64_t kStructureAlignment = 256;  // car_rt_scene_rules.h kStructureAlignment: the structures' offset alignment

uint64_t AlignUp(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

// The room a buffer gets over what the build asked for (CarRtScene::EnsureSlot's rule).
uint64_t Room(uint64_t bytes) {
  return AlignUp(bytes + bytes / 4, 4096);
}

struct Push {
  uint32_t info[4];   // x the dispatch's seed, y its number in the run, z the threads along a row, w the rays a thread traces
  float range[4];     // x the city's side, y the origins' height range, z the ray length, w unused
};

constexpr uint32_t kPushSpace = 2;  // the shader's (b0, space2)

// Every object the benchmark makes, so a failure at any step lets the destructors give the rest back (after the GPU is idle: every submit is waited for).
struct Bench {
  std::unique_ptr<plume::RenderBuffer> vertices, indices, blas_buffer, tlas_buffer, instances, scratch, counters, zero, readback;
  std::unique_ptr<plume::RenderAccelerationStructure> blas, tlas;
  plume::RenderBottomLevelASBuildInfo blas_info;
  plume::RenderTopLevelASBuildInfo tlas_info;
  plume::RenderDescriptorSetBuilder tlas_builder, counters_builder;
  std::unique_ptr<plume::RenderDescriptorSet> tlas_set, counters_set;
  std::unique_ptr<plume::RenderPipelineLayout> layout;
  std::unique_ptr<plume::RenderShader> shader;
  std::unique_ptr<plume::RenderPipeline> pipeline;
  std::unique_ptr<plume::RenderCommandList> list;
  std::unique_ptr<plume::RenderCommandFence> fence;
  uint32_t tlas_index = 0;
  uint32_t counters_index = 0;
};

// Maps an upload buffer, copies `bytes` into it, unmaps it.
bool Fill(plume::RenderBuffer& buffer, const void* data, uint64_t bytes) {
  void* mapped = buffer.map();
  if (!mapped) {
    return false;
  }
  std::memcpy(mapped, data, size_t(bytes));
  buffer.unmap();
  return true;
}

bool Fail(Measurement& m, const char* what) {
  REXLOG_WARN("{} {}: the benchmark made no measurement (the table's tier stands)", kPrefix, what);
  m.failed = true;
  return false;
}

// Builds the structures and the pipeline. Everything in `b` is made here; false (and a log line) on the first thing that cannot be.
bool Prepare(Device& device, Bench& b, const Scene& scene, Measurement& m) {
  plume::RenderDevice* dev = device.device();
  const uint64_t vertex_bytes = uint64_t(scene.positions.size()) * sizeof(float);
  const uint64_t index_bytes = uint64_t(scene.indices.size()) * sizeof(uint32_t);
  b.vertices = device.CreateBuffer(plume::RenderBufferDesc::UploadBuffer(vertex_bytes, plume::RenderBufferFlag::ACCELERATION_STRUCTURE_INPUT), "rt bench vertices");
  b.indices = device.CreateBuffer(plume::RenderBufferDesc::UploadBuffer(index_bytes, plume::RenderBufferFlag::ACCELERATION_STRUCTURE_INPUT), "rt bench indices");
  if (!b.vertices || !b.indices || !Fill(*b.vertices, scene.positions.data(), vertex_bytes) || !Fill(*b.indices, scene.indices.data(), index_bytes)) {
    return Fail(m, "the scene's buffers could not be made");
  }

  // The bottom level: the structure's triangles, opaque, built for tracing.
  const plume::RenderBottomLevelASMesh geometry(plume::RenderBufferReference(b.indices.get(), 0), plume::RenderBufferReference(b.vertices.get(), 0),
                                                plume::RenderFormat::R32_UINT, plume::RenderFormat::R32G32B32_FLOAT, uint32_t(scene.indices.size()),
                                                scene.VertexCount(), 3 * sizeof(float), /*isOpaque=*/true);
  dev->setBottomLevelASBuildInfo(b.blas_info, &geometry, 1, /*preferFastBuild=*/false, /*preferFastTrace=*/true);
  const uint64_t blas_bytes = AlignUp(b.blas_info.accelerationStructureSize, kStructureAlignment);
  if (blas_bytes == 0) {
    return Fail(m, "the bottom level's size is unknown");
  }
  b.blas_buffer = device.CreateBuffer(plume::RenderBufferDesc::AccelerationStructureBuffer(blas_bytes), "rt bench bottom level");
  if (!b.blas_buffer) {
    return Fail(m, "the bottom level's buffer could not be made");
  }
  b.blas = dev->createAccelerationStructure(plume::RenderAccelerationStructureDesc(
      plume::RenderAccelerationStructureType::BOTTOM_LEVEL, plume::RenderBufferReference(b.blas_buffer.get(), 0), blas_bytes));
  if (!b.blas) {
    return Fail(m, "the bottom level could not be made");
  }

  // The top level: the structure instanced in a grid.
  std::vector<plume::RenderTopLevelASInstance> instances;
  instances.reserve(scene.instances.size());
  for (uint32_t i = 0; i < scene.instances.size(); ++i) {
    plume::RenderTopLevelASInstance instance(plume::RenderBufferReference(b.blas_buffer.get(), 0), i, 0xFFu, 0, /*cullDisable=*/true,
                                             plume::RenderAffineTransform());
    std::memcpy(instance.transform.m, scene.instances[i].data(), sizeof(instance.transform.m));
    instances.push_back(instance);
  }
  dev->setTopLevelASBuildInfo(b.tlas_info, instances.data(), uint32_t(instances.size()), /*preferFastBuild=*/false, /*preferFastTrace=*/true);
  const uint64_t tlas_bytes = Room(b.tlas_info.accelerationStructureSize);
  if (b.tlas_info.accelerationStructureSize == 0 || b.tlas_info.instancesBufferData.empty()) {
    return Fail(m, "the top level's size is unknown");
  }
  // The top level sits at offset 0 of a buffer of its own (CarRtScene::EnsureSlot: Plume's D3D12 descriptor ignored the offset before the ray query work).
  b.tlas_buffer = device.CreateBuffer(plume::RenderBufferDesc::AccelerationStructureBuffer(tlas_bytes), "rt bench top level");
  b.instances = device.CreateBuffer(plume::RenderBufferDesc::UploadBuffer(b.tlas_info.instancesBufferData.size(),
                                                                          plume::RenderBufferFlag::ACCELERATION_STRUCTURE_INPUT),
                                    "rt bench instances");
  if (!b.tlas_buffer || !b.instances || !Fill(*b.instances, b.tlas_info.instancesBufferData.data(), b.tlas_info.instancesBufferData.size())) {
    return Fail(m, "the top level's buffers could not be made");
  }
  b.tlas = dev->createAccelerationStructure(plume::RenderAccelerationStructureDesc(
      plume::RenderAccelerationStructureType::TOP_LEVEL, plume::RenderBufferReference(b.tlas_buffer.get(), 0), tlas_bytes));
  if (!b.tlas) {
    return Fail(m, "the top level could not be made");
  }
  const uint64_t scratch_bytes = Room(std::max(b.blas_info.scratchSize, b.tlas_info.scratchSize));
  b.scratch = device.CreateBuffer(plume::RenderBufferDesc::DefaultBuffer(
                                      scratch_bytes, plume::RenderBufferFlag::ACCELERATION_STRUCTURE_SCRATCH | plume::RenderBufferFlag::UNORDERED_ACCESS),
                                  "rt bench scratch");
  if (!b.scratch) {
    return Fail(m, "the build's scratch buffer could not be made");
  }

  // The counters, a zero to start them from, and the readback.
  b.counters = device.CreateBuffer(
      plume::RenderBufferDesc::DefaultBuffer(kCounterBytes, plume::RenderBufferFlag::STORAGE | plume::RenderBufferFlag::UNORDERED_ACCESS), "rt bench counters");
  b.zero = device.CreateBuffer(plume::RenderBufferDesc::UploadBuffer(kCounterBytes), "rt bench zero");
  b.readback = device.CreateBuffer(plume::RenderBufferDesc::ReadbackBuffer(kCounterBytes), "rt bench readback");
  const uint32_t zeros[kCounterWords] = {};
  if (!b.counters || !b.zero || !b.readback || !Fill(*b.zero, zeros, kCounterBytes)) {
    return Fail(m, "the counters' buffers could not be made");
  }

  // The descriptor sets, the layout (the sets in the order the shader declares them, the push range at (b0, space2)), the shader and the pipeline.
  b.tlas_builder.begin();
  b.tlas_index = b.tlas_builder.addAccelerationStructure(0);  // t0, space0
  b.tlas_builder.end();
  b.counters_builder.begin();
  b.counters_index = b.counters_builder.addReadWriteByteAddressBuffer(0);  // u0, space1
  b.counters_builder.end();
  b.tlas_set = device.CreateDescriptorSet(b.tlas_builder, 1, 0, "rt bench top level set");
  b.counters_set = device.CreateDescriptorSet(b.counters_builder, 1, 0, "rt bench counters set");
  if (!b.tlas_set || !b.counters_set) {
    return Fail(m, "the descriptor sets could not be made");
  }
  b.tlas_set->setAccelerationStructure(b.tlas_index, b.tlas.get());
  b.counters_set->setBuffer(b.counters_index, b.counters.get(), kCounterBytes);

  plume::RenderPipelineLayoutBuilder layout;
  layout.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  layout.addDescriptorSet(b.tlas_builder);       // set 0
  layout.addDescriptorSet(b.counters_builder);   // set 1
  layout.addPushConstant(0, kPushSpace, sizeof(Push), plume::RenderShaderStageFlag::COMPUTE);
  layout.end();
  b.layout = device.CreatePipelineLayout(layout, "rt bench layout");
  const HostShaderBlob blob = RtBenchShader();
  if (!b.layout || !blob.data) {
    return Fail(m, "the layout or the shader is missing");
  }
  b.shader = device.CreateShader(blob.data, blob.size, "main", blob.name);
  if (!b.shader) {
    return Fail(m, "the shader was refused");
  }
  b.pipeline = device.CreateComputePipeline(plume::RenderComputePipelineDesc(b.layout.get(), b.shader.get(), kGroup, kGroup, 1), "rt bench pipeline");
  if (!b.pipeline) {
    return Fail(m, "the pipeline could not be made");
  }
  b.list = device.queue()->createCommandList();
  b.fence = dev->createCommandFence();
  if (!b.list || !b.fence) {
    return Fail(m, "the command list or its fence could not be made");
  }
  return true;
}

// Submits the list that is open and waits for it; false when the device was lost.
bool Submit(Device& device, Bench& b, const char* what) {
  b.list->end();
  const plume::RenderCommandList* lists[] = {b.list.get()};
  device.queue()->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, b.fence.get());
  if (device.CheckLost(what)) {
    return false;
  }
  device.queue()->waitForCommandFence(b.fence.get());
  return !device.CheckLost(what);
}

// One list: the counters cleared, then both structures built (the bottom level first, a barrier between, the top level after, a barrier before the compute
// shader reads it: the order CarRtScene::Record has).
bool BuildStructures(Device& device, Bench& b, Measurement& m) {
  b.list->begin();
  b.list->barriers(plume::RenderBarrierStage::COPY, plume::RenderBufferBarrier(b.counters.get(), plume::RenderBufferAccess::WRITE));
  b.list->copyBufferRegion(plume::RenderBufferReference(b.counters.get(), 0), plume::RenderBufferReference(b.zero.get(), 0), kCounterBytes);
  b.list->barriers(plume::RenderBarrierStage::COMPUTE, plume::RenderBufferBarrier(b.scratch.get(), plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE));
  b.list->buildBottomLevelAS(b.blas.get(), plume::RenderBufferReference(b.scratch.get(), 0), b.blas_info);
  const plume::RenderBufferBarrier before_top[] = {
      plume::RenderBufferBarrier(b.blas_buffer.get(), plume::RenderBufferAccess::READ),
      plume::RenderBufferBarrier(b.scratch.get(), plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE)};
  b.list->barriers(plume::RenderBarrierStage::COMPUTE, before_top, 2, nullptr, 0);
  b.list->buildTopLevelAS(b.tlas.get(), plume::RenderBufferReference(b.scratch.get(), 0), plume::RenderBufferReference(b.instances.get(), 0), b.tlas_info);
  b.list->barriers(plume::RenderBarrierStages(plume::RenderBarrierStage::GRAPHICS | plume::RenderBarrierStage::COMPUTE),
                   plume::RenderBufferBarrier(b.tlas_buffer.get(), plume::RenderBufferAccess::READ));
  b.list->barriers(plume::RenderBarrierStage::COMPUTE, plume::RenderBufferBarrier(b.counters.get(), plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE));
  if (!Submit(device, b, "rt bench build")) {
    return Fail(m, "the device was lost while the structures were built");
  }
  return true;
}

// `dispatches` dispatches of the trace in one list, submitted and waited for; the time in milliseconds (CPU clock around the submit and the wait, which is
// the GPU's time plus a fraction of a millisecond of submission: the dispatches take at least a few milliseconds). `seed` keeps every dispatch of the
// benchmark different. Negative on a lost device.
double TimedDispatches(Device& device, Bench& b, uint32_t dispatches, uint32_t* seed) {
  b.list->begin();
  b.list->setComputePipelineLayout(b.layout.get());
  b.list->setComputeDescriptorSet(b.tlas_set.get(), 0);
  b.list->setComputeDescriptorSet(b.counters_set.get(), 1);
  b.list->setPipeline(b.pipeline.get());
  for (uint32_t d = 0; d < dispatches; ++d) {
    Push push = {};
    push.info[0] = 0x9E3779B9u * (++*seed);
    push.info[1] = d;
    push.info[2] = kThreadsX;
    push.info[3] = kRaysPerThread;
    push.range[0] = kCitySide;
    push.range[1] = kMaxBoxHeight;
    push.range[2] = kRayLength;
    b.list->setComputePushConstants(0, &push, 0, sizeof(push));
    b.list->dispatch(kThreadsX / kGroup, kThreadsY / kGroup, 1);
  }
  const auto start = std::chrono::steady_clock::now();
  if (!Submit(device, b, "rt bench dispatch")) {
    return -1.0;
  }
  const auto end = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(end - start).count();
}

// Copies the counters to the readback buffer and reads them.
bool ReadCounters(Device& device, Bench& b, uint32_t* rays, uint32_t* hits) {
  b.list->begin();
  b.list->barriers(plume::RenderBarrierStage::COPY, plume::RenderBufferBarrier(b.counters.get(), plume::RenderBufferAccess::READ));
  b.list->copyBufferRegion(plume::RenderBufferReference(b.readback.get(), 0), plume::RenderBufferReference(b.counters.get(), 0), kCounterBytes);
  if (!Submit(device, b, "rt bench readback")) {
    return false;
  }
  const void* mapped = b.readback->map();
  if (!mapped) {
    return false;
  }
  uint32_t words[kCounterWords];
  std::memcpy(words, mapped, sizeof(words));
  b.readback->unmap();
  *rays = words[kCounterRays];
  *hits = words[kCounterHits];
  return true;
}

// CHECK: written without a build or a run (rt_bench.h): the barriers and states of the builds and the dispatches follow CarRtScene::Record; the first run
// on D3D12 with the debug layer and on Vulkan with the validation layer is the proof, and the "[gpu] rt bench:" line the first number.
Measurement Measure(Device& device) {
  Measurement m;
  Bench b;
  const Scene scene = MakeScene();
  if (!Prepare(device, b, scene, m) || !BuildStructures(device, b, m)) {
    return m;
  }
  uint32_t seed = 1;
  // The warm-up: one dispatch, the pipeline's and the structures' first use.
  m.warmup_ms = TimedDispatches(device, b, 1, &seed);
  if (m.warmup_ms < 0.0) {
    Fail(m, "the device was lost in the warm-up");
    return m;
  }
  const Plan plan = PlanRuns(m.warmup_ms);
  for (uint32_t run = 0; run < plan.runs; ++run) {
    const double ms = TimedDispatches(device, b, kDispatchesPerRun, &seed);
    if (ms < 0.0) {
      Fail(m, "the device was lost in a timed run");
      return m;
    }
    m.run_ms.push_back(ms);
  }
  // What the shader counted over all its dispatches proves they ran and the rays found the city.
  uint32_t rays = 0, hits = 0;
  if (!ReadCounters(device, b, &rays, &hits)) {
    Fail(m, "the counters could not be read");
    return m;
  }
  const uint64_t expected = uint64_t(kRaysPerDispatch) * (1u + uint64_t(plan.runs) * kDispatchesPerRun);
  if (rays != expected) {
    REXLOG_WARN("{} the shader counted {} rays, {} were asked for: the dispatches did not all run", kPrefix, rays, expected);
    Fail(m, "the shader's count does not match the dispatches");
    return m;
  }
  m.rays = rays;
  m.hits = hits;
  return m;
}

// --- the cache file -----------------------------------------------------------------------------------------------------------------

// The settings folder: where larecomp.toml is (the cache is the player's own state, like it), else the user folder; empty when neither is known.
std::filesystem::path CachePath() {
  std::error_code ec;
  const std::filesystem::path config = rex::GetPathSettings().config_file.Get();
  std::filesystem::path folder = config.empty() ? std::filesystem::path() : config.parent_path();
  if (folder.empty()) {
    folder = rex::GetPathSettings().user_root.Get();
  }
  if (folder.empty()) {
    return {};
  }
  return folder / kCacheFile;
}

bool ReadCache(const std::filesystem::path& path, std::string* key, Result* result) {
  if (path.empty()) {
    return false;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return ParseRecord(text, key, result);
}

void WriteCache(const std::filesystem::path& path, const KeyFacts& key, const Result& result) {
  if (path.empty()) {
    return;
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (out) {
    out << SerializeRecord(key, result);
  }
  if (!out) {
    REXLOG_WARN("{} the result could not be written to {}: the benchmark runs again at the next start", kPrefix, path.string());
  }
}

std::string Describe(const Result& r) {
  if (!r.usable()) {
    return std::format("{}", OutcomeName(r.outcome));
  }
  return std::format("{} ({:.0f} M rays/s, the fastest of {} {}, {:.1f}% of the rays hit)", OutcomeName(r.outcome), r.mrays_per_s,
                     r.runs != 0 ? r.runs : 1u, r.runs != 0 ? "runs" : "warm-up", r.hit_share * 100.0);
}

}  // namespace

Result RunIfWanted(Device& device) {
  Result result;
  if (!gpu_class::RtBenchWanted()) {
    REXLOG_INFO("{} not run: this card cannot trace rays here, or it shares memory with the CPU (the table's tier stands)", kPrefix);
    return result;
  }
  const std::string setting = larecomp::settings::mcla_gpu_rt_bench.Get();
  const KeyFacts key = gpu_class::RtBenchKey();
  const std::filesystem::path path = CachePath();
  std::string cached_key;
  Result cached;
  const bool cache_valid = ReadCache(path, &cached_key, &cached) && KeyMatches(cached_key, key);
  switch (ActionFor(setting, cache_valid)) {
    case Action::kSkip:
      REXLOG_INFO("{} off (mcla_gpu_rt_bench=off): the table's tier stands", kPrefix);
      return result;
    case Action::kUseCache:
      REXLOG_INFO("{} {} (kept in {} for this card, driver and API)", kPrefix, Describe(cached), path.string());
      gpu_class::NoteRtBench(cached);
      return cached;
    case Action::kRun:
      break;
  }
  const auto start = std::chrono::steady_clock::now();
  const Measurement measurement = Measure(device);
  result = Summarize(measurement);
  const double total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  REXLOG_INFO("{} {}; warm-up {:.2f} ms, runs {} ms, the whole benchmark {:.0f} ms (mcla_gpu_rt_bench={})", kPrefix, Describe(result), measurement.warmup_ms,
              measurement.run_ms.empty() ? std::string("none") : std::format("{:.2f}", *std::min_element(measurement.run_ms.begin(), measurement.run_ms.end())),
              total_ms, setting);
  WriteCache(path, key, result);
  gpu_class::NoteRtBench(result);
  return result;
}

}  // namespace mcla::gpu::rt_bench
