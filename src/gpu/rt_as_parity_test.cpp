// Standalone GPU proof. Run with a DXIL or SPIR-V blob of rt_as_parity_cs.hlsl.
#include <plume_render_interface.h>
#include <plume_render_interface_builders.h>
#if defined(_WIN32)
#include <plume_d3d12.h>
#endif

#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>

namespace plume {
std::unique_ptr<RenderInterface> CreateVulkanInterface();
#if defined(_WIN32)
std::unique_ptr<RenderInterface> CreateD3D12Interface();
#endif
}  // namespace plume

namespace {

using namespace plume;
constexpr auto kBuild = RenderBarrierStage::COMPUTE;
constexpr auto kAsInput = RenderBufferFlag::ACCELERATION_STRUCTURE_INPUT;
constexpr auto kScratch = RenderBufferFlag::ACCELERATION_STRUCTURE_SCRATCH | RenderBufferFlag::UNORDERED_ACCESS;
constexpr auto kReadWrite = RenderBufferAccess::READ | RenderBufferAccess::WRITE;
constexpr uint32_t kRays = 6;
using Hit = std::array<uint32_t, 5>;
using Results = std::array<Hit, kRays>;

struct As {
  std::unique_ptr<RenderBuffer> buffer;
  std::unique_ptr<RenderAccelerationStructure> structure;
};

As AllocateAs(RenderDevice& device, RenderAccelerationStructureType type, uint64_t bytes) {
  if (!bytes) return {};
  As result;
  result.buffer = device.createBuffer(RenderBufferDesc::AccelerationStructureBuffer(bytes));
  if (result.buffer) result.structure = device.createAccelerationStructure({type, {result.buffer.get(), 0}, bytes});
  return result;
}

std::unique_ptr<RenderBuffer> Upload(RenderDevice& device, const void* data, size_t bytes) {
  auto buffer = device.createBuffer(RenderBufferDesc::UploadBuffer(bytes, kAsInput));
  void* mapped = buffer ? buffer->map() : nullptr;
  if (!mapped) return {};
  std::memcpy(mapped, data, bytes);
  buffer->unmap();
  return buffer;
}

bool Fail(const char* step) {
  std::printf("FAIL AS compaction parity at %s\n", step);
  return false;
}

#if defined(_WIN32)
bool EnableD3D12Validation() {
  ID3D12Debug* debug = nullptr;
  if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))) || !debug) return Fail("enable D3D12 debug layer");
  debug->EnableDebugLayer();
  debug->Release();
  return true;
}

bool CheckD3D12Validation(RenderDevice& device) {
  auto* native = static_cast<D3D12Device&>(device).d3d;
  ID3D12InfoQueue* info = nullptr;
  if (!native || FAILED(native->QueryInterface(IID_PPV_ARGS(&info))) || !info) return Fail("D3D12 debug message queue");
  bool ok = true;
  std::vector<uint8_t> data;
  for (uint64_t i = 0; i < info->GetNumStoredMessages(); ++i) {
    SIZE_T bytes = 0;
    if (FAILED(info->GetMessage(i, nullptr, &bytes))) continue;
    data.resize(bytes);
    auto* message = reinterpret_cast<D3D12_MESSAGE*>(data.data());
    if (SUCCEEDED(info->GetMessage(i, message, &bytes)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
      std::printf("D3D12 validation %u: %s\n", uint32_t(message->ID), message->pDescription);
      ok = false;
    }
  }
  info->Release();
  return ok;
}
#endif

struct Commands {
  std::unique_ptr<RenderCommandQueue> queue;
  std::unique_ptr<RenderCommandList> list;
  std::unique_ptr<RenderCommandFence> fence;

  bool Init(RenderDevice& device) {
    queue = device.createCommandQueue(RenderCommandListType::DIRECT);
    if (queue) list = queue->createCommandList();
    fence = device.createCommandFence();
    return queue && list && fence;
  }

  void Submit() {
    list->end();
    queue->executeCommandLists(list.get(), fence.get());
    queue->waitForCommandFence(fence.get());
  }
};

bool Trace(RenderDevice& device, Commands& commands, const As& blas, const std::vector<char>& code,
           RenderShaderFormat format, bool fast_build, Results& results) {
  RenderTopLevelASInstance instances[] = {
      {{blas.buffer.get(), 0}, 17, 0x01, 0, true, {}},
      {{blas.buffer.get(), 0}, 29, 0x02, 0, true, {}}};
  for (auto& instance : instances) instance.bottomLevelASObject = blas.structure.get();
  RenderTopLevelASBuildInfo info;
  device.setTopLevelASBuildInfo(info, instances, 2, fast_build, !fast_build);
  auto input = Upload(device, info.instancesBufferData.data(), info.instancesBufferData.size());
  auto top = AllocateAs(device, RenderAccelerationStructureType::TOP_LEVEL, info.accelerationStructureSize);
  auto scratch = device.createBuffer(RenderBufferDesc::DefaultBuffer(info.scratchSize, kScratch));
  auto output = device.createBuffer(RenderBufferDesc::DefaultBuffer(sizeof(results), RenderBufferFlag::STORAGE | RenderBufferFlag::UNORDERED_ACCESS));
  auto readback = device.createBuffer(RenderBufferDesc::ReadbackBuffer(sizeof(results)));
  if (!input || !top.structure || !scratch || !output || !readback) return Fail("trace buffers");

  RenderDescriptorSetBuilder bindings;
  bindings.begin();
  const uint32_t tlas_index = bindings.addAccelerationStructure(0);
  const uint32_t output_index = bindings.addReadWriteByteAddressBuffer(1);
  bindings.end();
  auto set = device.createDescriptorSet(bindings.descriptorSetDesc);
  if (!set) return Fail("descriptors");
  set->setAccelerationStructure(tlas_index, top.structure.get());
  set->setBuffer(output_index, output.get(), sizeof(results));

  RenderPipelineLayoutBuilder layout_builder;
  layout_builder.begin(false, false);
  layout_builder.addDescriptorSet(bindings);
  layout_builder.end();
  auto layout = device.createPipelineLayout(layout_builder.layoutDesc);
  auto shader = device.createShader(code.data(), code.size(), "main", format);
  if (!layout || !shader) return Fail("trace shader and layout");
  auto pipeline = device.createComputePipeline({layout.get(), shader.get(), 8, 1, 1});
  if (!pipeline) return Fail("trace pipeline");

  auto& list = *commands.list;
  list.begin();
  list.barriers(kBuild, RenderBufferBarrier(scratch.get(), kReadWrite));
  list.buildTopLevelAS(top.structure.get(), {scratch.get(), 0}, {input.get(), 0}, info);
  const RenderBufferBarrier barriers[] = {
      {top.buffer.get(), RenderBufferAccess::READ}, {output.get(), RenderBufferAccess::WRITE}};
  list.barriers(kBuild, barriers, 2);
  list.setComputePipelineLayout(layout.get());
  list.setComputeDescriptorSet(set.get(), 0);
  list.setPipeline(pipeline.get());
  list.dispatch(1, 1, 1);
  list.barriers(RenderBarrierStage::COPY, RenderBufferBarrier(output.get(), RenderBufferAccess::READ));
  list.copyBufferRegion({readback.get(), 0}, {output.get(), 0}, sizeof(results));
  commands.Submit();

  const RenderRange range(0, sizeof(results));
  const void* data = readback->map(0, &range);
  if (!data) return Fail("trace readback");
  std::memcpy(results.data(), data, sizeof(results));
  const RenderRange no_writes(0, 0);
  readback->unmap(0, &no_writes);
  return true;
}

bool Run(RenderDevice& device, const std::vector<char>& code, RenderShaderFormat format) {
  const float positions[][3] = {
      {0,0,1}, {2,0,1}, {0,2,1}, {4,0,1}, {6,0,1}, {4,2,1},
      {0,0,0}, {2,0,0}, {0,2,0}, {4,0,0}, {6,0,0}, {4,2,0}};
  const uint32_t indices[] = {0,1,2, 3,4,5};
  auto vertices = Upload(device, positions, sizeof(positions));
  auto index = Upload(device, indices, sizeof(indices));
  auto query = device.createASCompactedSizeQuery(4);
  Commands commands;
  if (!vertices || !index || !query || !commands.Init(device)) return Fail("build inputs and query");

  using F = RenderFormat;
  const RenderBottomLevelASMesh geometry[] = {
      {{index.get(), 0}, {vertices.get(), 0}, F::R32_UINT, F::R32G32B32_FLOAT, 6, 6, 12, true},
      {{index.get(), 0}, {vertices.get(), sizeof(positions) / 2}, F::R32_UINT, F::R32G32B32_FLOAT, 6, 6, 12, false}};
  RenderBottomLevelASBuildInfo info;
  device.setBottomLevelASBuildInfo(info, geometry, 2, false, true, true);
  auto original = AllocateAs(device, RenderAccelerationStructureType::BOTTOM_LEVEL, info.accelerationStructureSize);
  auto scratch = device.createBuffer(RenderBufferDesc::DefaultBuffer(info.scratchSize, kScratch));
  if (!original.structure || !scratch) return Fail("original BLAS");

  // CHECK: run this fixture on D3D12 and Vulkan with validation enabled.
  auto& list = *commands.list;
  list.begin();
  list.barriers(kBuild, RenderBufferBarrier(scratch.get(), kReadWrite));
  list.buildBottomLevelAS(original.structure.get(), {scratch.get(), 0}, info);
  list.barriers(kBuild, RenderBufferBarrier(original.buffer.get(), RenderBufferAccess::READ));
  const RenderAccelerationStructure* sources[] = {original.structure.get()};
  list.writeBottomLevelASCompactedSizes(query.get(), sources, 1);
  commands.Submit();

  uint64_t reported = 0;
  if (!query->read(&reported, 1) || reported == 0 || reported > info.accelerationStructureSize) return Fail("compacted size");
  const uint64_t bytes = (reported + 255) & ~uint64_t(255);
  auto compact = AllocateAs(device, RenderAccelerationStructureType::BOTTOM_LEVEL, bytes);
  if (!compact.structure) return Fail("compact BLAS");
  list.begin();
  list.copyBottomLevelASCompact(compact.structure.get(), original.structure.get());
  list.barriers(kBuild, RenderBufferBarrier(compact.buffer.get(), RenderBufferAccess::READ));
  commands.Submit();

  Results baseline{}, packed{}, fast_build{};
  if (!Trace(device, commands, original, code, format, false, baseline) ||
      !Trace(device, commands, compact, code, format, false, packed) ||
      !Trace(device, commands, compact, code, format, true, fast_build)) return false;

  constexpr Hit miss = {UINT32_MAX, UINT32_MAX, UINT32_MAX, 0, 0};
  constexpr Results expected = {{
      {17, 1, 0, std::bit_cast<uint32_t>(1.0f), 1},
      {17, 0, 0, std::bit_cast<uint32_t>(2.0f), 1},
      {29, 1, 1, std::bit_cast<uint32_t>(1.0f), 1},
      miss, miss, miss}};
  for (uint32_t i = 0; i < kRays; ++i) {
    if (baseline[i] != expected[i] || packed[i] != baseline[i] || fast_build[i] != baseline[i]) {
      std::printf("Ray %u (instance, geometry, primitive, t bits, visibility)\n", i);
      const Hit* rows[] = {&expected[i], &baseline[i], &packed[i], &fast_build[i]};
      for (const Hit* row : rows) {
        std::printf("%u %u %u %#x %u\n", (*row)[0], (*row)[1], (*row)[2], (*row)[3], (*row)[4]);
      }
      return Fail("hit or visibility mismatch");
    }
  }
  std::printf("PASS %u rays, three builds matched, BLAS %llu -> %llu bytes (driver reported %llu)\n", kRays,
              static_cast<unsigned long long>(info.accelerationStructureSize), static_cast<unsigned long long>(bytes),
              static_cast<unsigned long long>(reported));
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3 && argc != 4) {
    std::puts("Usage: larecomp_rt_as_parity_test {d3d12|vulkan} {shader blob} [adapter]");
    return 2;
  }
  const std::string_view backend = argv[1];
  std::unique_ptr<plume::RenderInterface> render_interface;
  auto format = plume::RenderShaderFormat::UNKNOWN;
  if (backend == "vulkan") {
    render_interface = plume::CreateVulkanInterface();
    format = plume::RenderShaderFormat::SPIRV;
  }
#if defined(_WIN32)
  if (backend == "d3d12") {
    if (!EnableD3D12Validation()) return 1;
    render_interface = plume::CreateD3D12Interface();
    format = plume::RenderShaderFormat::DXIL;
  }
#endif
  auto device = render_interface ? render_interface->createDevice(argc == 4 ? argv[3] : "") : nullptr;
  if (!device || !device->getCapabilities().rayQuery) {
    Fail("ray query device");
    return 1;
  }
  std::ifstream file(argv[2], std::ios::binary);
  const std::vector<char> code((std::istreambuf_iterator<char>(file)), {});
  if (code.empty()) {
    Fail("shader blob");
    return 1;
  }
  const bool matched = Run(*device, code, format);
#if defined(_WIN32)
  if (backend == "d3d12" && !CheckD3D12Validation(*device)) return 1;
#endif
  return matched ? 0 : 1;
}
