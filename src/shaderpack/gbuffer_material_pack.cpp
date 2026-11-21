// LARecomp shader pack builder -- the G-buffer's material table (see gbuffer_material_pack.h).

#include "shaderpack/gbuffer_material_pack.h"

#include <format>
#include <fstream>
#include <span>
#include <system_error>
#include <vector>

#include "gpu/gbuffer_material.h"
#include "gpu/gbuffer_materials.h"
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

  // The shader names file (GB-F2's census tool joins a pixel shader's identity, which the renderer logs, to its group and class). Nothing reads it at run time and it is not part of the
  // table: a failure to write it is a word in the message, never the step's.
  {
    const std::string names = gb::FormatShaderNamesTsv(shaders);
    const std::filesystem::path names_final = out_dir / gb::kShaderNamesFileName;
    const std::filesystem::path names_temporary = out_dir / (std::string(gb::kShaderNamesFileName) + ".tmp");
    bool ok = false;
    {
      std::ofstream file(names_temporary, std::ios::binary | std::ios::trunc);
      if (file) file.write(names.data(), std::streamsize(names.size()));
      if (file) file.flush();
      ok = bool(file);
    }
    std::string names_error;
    if (ok && CommitFile(names_temporary, names_final, &names_error)) {
      result.message += std::format("; the shader names file {} ({} bytes)", gb::kShaderNamesFileName, names.size());
    } else {
      std::error_code names_ec;
      std::filesystem::remove(names_temporary, names_ec);
      result.message += std::string("; the shader names file was not written") + (names_error.empty() ? "" : ": " + names_error);
    }
  }
  return result;
}

}  // namespace larecomp::shaderpack
