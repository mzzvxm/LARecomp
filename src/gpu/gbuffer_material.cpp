// MCLA GPU renderer -- the G-buffer's material channel: the table file's reading. See gbuffer_material.h.

#include "gbuffer_material.h"

#include <fstream>
#include <iterator>
#include <system_error>

#include <rex/logging/log_category.h>

namespace mcla::gpu::gbuffer {

bool MaterialTable::LoadFile(const std::filesystem::path& file, std::string* why) {
  records_.clear();
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    *why = "cannot open " + file.string();
    return false;
  }
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) {
    *why = "cannot read " + file.string();
    return false;
  }
  return Load(bytes, why);
}

void MaterialTable::EnsureLoaded(const std::filesystem::path& folder) {
  if (tried_ || folder.empty()) {
    return;
  }
  tried_ = true;
  const std::filesystem::path file = folder / kMaterialTableFileName;
  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) {
    REXLOG_INFO("[gbuffer] material table: {} is not in {} (the shader pack build writes it beside the variant pack): the draws take the default material word", kMaterialTableFileName,
                folder.string());
    return;
  }
  std::string why;
  if (!LoadFile(file, &why)) {
    REXLOG_WARN("[gbuffer] material table: {} is not used: {} (the draws take the default material word)", file.string(), why);
    return;
  }
  size_t pixel = 0;
  size_t with_gloss = 0;
  for (const MaterialRecord& record : records_) {
    pixel += record.pixel ? 1 : 0;
    with_gloss += record.gloss_bank != GlossBank::kNone ? 1 : 0;
  }
  REXLOG_INFO("[gbuffer] material table: {} shaders from {} ({} pixel shaders; {} with a specular exponent to read)", records_.size(), file.string(), pixel, with_gloss);
}

}  // namespace mcla::gpu::gbuffer
