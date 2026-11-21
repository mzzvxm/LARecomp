// LARecomp shader pack builder -- the G-buffer's material table (see gbuffer_material_pack.h).

#include "shaderpack/gbuffer_material_pack.h"

#include <format>
#include <fstream>
#include <span>
#include <system_error>
#include <vector>

#include "gpu/gbuffer_material.h"
#include "shaderpack/pack_writer.h"

namespace larecomp::shaderpack {

namespace gb = mcla::gpu::gbuffer;

uint32_t GBufferMaterialRulesVersion() { return gb::kMaterialRulesVersion; }

MaterialTableResult WriteGBufferMaterialTable(const ContainerSet& containers, const std::filesystem::path& out_dir) {
  MaterialTableResult result;
  std::vector<gb::NamedShader> shaders;
  shaders.reserve(containers.shaders.size());
  for (const ShaderSource& shader : containers.shaders) {
    shaders.push_back({shader.key, shader.stage == ShaderStage::kPixel, shader.name});
  }
  const std::vector<gb::MaterialRecord> records = gb::BuildMaterialRecords(shaders);
  std::vector<uint8_t> bytes;
  std::string error;
  if (!gb::LayOutMaterialTable(records, &bytes, &error)) {
    result.message = error;
    return result;
  }
  const std::filesystem::path final_path = out_dir / gb::kMaterialTableFileName;
  const std::filesystem::path temporary_path = out_dir / (std::string(gb::kMaterialTableFileName) + ".tmp");
  {
    std::ofstream file(temporary_path, std::ios::binary | std::ios::trunc);
    if (file) file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    if (file) file.flush();
    if (!file) {
      std::error_code ec;
      std::filesystem::remove(temporary_path, ec);
      result.message = "the material table could not be written to " + temporary_path.string();
      return result;
    }
  }
  if (!CommitFile(temporary_path, final_path, &error)) {
    std::error_code ec;
    std::filesystem::remove(temporary_path, ec);
    result.message = error;
    return result;
  }
  size_t pixel = 0;
  size_t with_gloss = 0;
  for (const gb::MaterialRecord& record : records) {
    pixel += record.pixel ? 1 : 0;
    with_gloss += record.gloss_bank != gb::GlossBank::kNone ? 1 : 0;
  }
  result.written = true;
  result.records = records.size();
  result.message = std::format("{} shaders in the G-buffer material table ({} pixel; {} with a specular exponent), {} bytes", records.size(), pixel, with_gloss, bytes.size());
  return result;
}

}  // namespace larecomp::shaderpack
