// What the city's ray tracing needs to be right on Vulkan, as checks of the sources (CITY-VK, 2026-11-22). Direct3D 12 forgives what Vulkan does not; none of these
// can be seen without a Vulkan device and its validation layer, and all of them can be read in the text:
//
//   1. every shader that traces rays (RayQuery or RaytracingAccelerationStructure in the shader or in anything it includes, however deep) is built through
//      larecomp_gpu_host_shader_rt, at Shader Model 6.5: that function is the only one that gives the SPIR-V blob -fspv-target-env=vulkan1.2 (SPIR-V 1.5, which the ray query
//      extension needs); a ray shader built through the plain function validates as SPIR-V 1.0 and then fails spirv-val, or worse, loads on no driver. The car's three shaders
//      were checked by car_rt_scene_rules_test; this does it for every shader of the build (the city's shadow, the probe update's and the per-pixel trace, the reflection
//      fallback's, the benchmark, the ones that come next).
//   2. the format of every typed storage image: Vulkan takes it from the shader ([[vk::image_format("rgba16f")]]) and not from the image, and a view of another format
//      than the shader says is a validation error and garbage on a driver; Direct3D 12 reads the resource's own format and never notices. The table below pairs each image's
//      declaration in the shader with the format the C++ makes the image in.
//   3. the alignments Vulkan insists on for the structures' inputs: vertex and index addresses to the size of the component (4), a structure at a multiple of 256, a build's
//      scratch address at the device's offset alignment (the buffer's own base is aligned by Plume, so a build at offset 0 is right; the structures placed at an offset are
//      256 multiples), the pool's blocks at 16.
//
// A program of its own: it reads the source folder it is given (default: the folder this file is in), needs no renderer and no device, exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_city_rt_vulkan_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_city_rt_vulkan_test [<src/gpu dir>]

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "car_rt_scene_rules.h"
#include "citypack/city_hit_format.h"

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

std::string ReadAll(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------- the build

struct HostShader {
  std::string stem;
  std::string profile;
  std::string source;  // SOURCE argument, else the stem
  bool rt = false;     // built through larecomp_gpu_host_shader_rt
};

// Every call of larecomp_gpu_host_shader( and larecomp_gpu_host_shader_rt( at the start of a line of src/gpu/CMakeLists.txt (the comments' own mentions are inside a line that starts with #).
std::vector<HostShader> HostShaders(const std::string& cmake) {
  std::vector<HostShader> out;
  const std::regex call(R"(\n(larecomp_gpu_host_shader(_rt)?)\(([^)]*)\))");
  for (std::sregex_iterator it(cmake.begin(), cmake.end(), call), end; it != end; ++it) {
    std::string args = (*it)[3].str();
    std::vector<std::string> words;
    std::string word;
    for (char c : args + " ") {
      if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
        if (!word.empty()) words.push_back(word);
        word.clear();
      } else {
        word.push_back(c);
      }
    }
    if (words.size() < 2) continue;
    HostShader s;
    s.stem = words[0];
    s.profile = words[1];
    s.source = s.stem;
    s.rt = (*it)[2].matched;
    for (size_t i = 2; i + 1 < words.size(); ++i) {
      if (words[i] == "SOURCE") s.source = words[i + 1];
    }
    out.push_back(s);
  }
  return out;
}

// The text of a shader source and of everything it includes (#include "name" relative to hlsl/; a name that is not there, a third party's header, is left out).
void Closure(const std::filesystem::path& hlsl, const std::string& name, std::set<std::string>* seen, std::string* text) {
  if (!seen->insert(name).second) return;
  const std::string body = ReadAll(hlsl / name);
  if (body.empty()) return;
  *text += body;
  const std::regex inc(R"(#\s*include\s+\"([^\"]+)\")");
  for (std::sregex_iterator it(body.begin(), body.end(), inc), end; it != end; ++it) {
    Closure(hlsl, (*it)[1].str(), seen, text);
  }
}

void TestRayShadersBuiltForVulkan(const std::filesystem::path& folder) {
  const std::string cmake = ReadAll(folder / "CMakeLists.txt");
  Check(!cmake.empty(), "src/gpu/CMakeLists.txt is there");
  if (cmake.empty()) return;
  const std::vector<HostShader> shaders = HostShaders(cmake);
  Check(shaders.size() > 100, "the build's host shader calls were found (" + std::to_string(shaders.size()) + ")");
  const std::filesystem::path hlsl = folder / "hlsl";
  std::set<std::string> traces;
  uint32_t rt_calls = 0;
  for (const HostShader& s : shaders) {
    std::set<std::string> seen;
    std::string text;
    Closure(hlsl, s.source + ".hlsl", &seen, &text);
    if (text.empty()) {
      Check(false, "hlsl/" + s.source + ".hlsl (" + s.stem + ") is there");
      continue;
    }
    const bool traces_rays = Has(text, "RayQuery<") || Has(text, "RaytracingAccelerationStructure");
    if (traces_rays) traces.insert(s.stem);
    if (traces_rays) {
      Check(s.rt, s.stem + " traces rays (RayQuery in hlsl/" + s.source + ".hlsl or what it includes) and is built through larecomp_gpu_host_shader_rt: Vulkan 1.2, SPIR-V 1.5");
    }
    if (s.rt) {
      ++rt_calls;
      Check(s.profile == "cs_6_5", s.stem + " is built through larecomp_gpu_host_shader_rt at cs_6_5 (it is " + s.profile + ")");
    }
  }
  // The shaders of the city's ray tracing are there and trace (a rename or a lost registration would drop one from the loop above without a word).
  const char* const kCity[] = {"city_rt_shadow_cs", "gi_update_trace_cs", "gi_pixel_trace_cs", "gi_pixel_trace_ms_cs", "ssr_rt_trace_cs", "ssr_rt_trace_ms_cs",
                               "car_rt_refl_cs", "car_rt_refl_opp_cs", "car_rt_refl_glass_cs", "car_rt_ao_cs", "car_rt_check_cs", "rt_bench_cs"};
  for (const char* stem : kCity) {
    Check(traces.count(stem) == 1, std::string(stem) + " is a host shader that traces rays");
  }
  Check(rt_calls >= 12, "at least twelve shaders go through the ray query function (" + std::to_string(rt_calls) + ")");
  // The function itself, as car_rt_scene_rules_test reads it: the SPIR-V argument is for the SPIR-V blob only.
  const size_t fn = cmake.find("function(larecomp_gpu_host_shader_rt STEM PROFILE)");
  Check(fn != std::string::npos && Has(cmake.substr(fn, cmake.find("endfunction()", fn) - fn), "SPIRV_ARGS -fspv-target-env=vulkan1.2"),
        "larecomp_gpu_host_shader_rt passes -fspv-target-env=vulkan1.2 to the SPIR-V compile");
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------- storage images

// One typed storage image: where the shader declares it, the format it names, and where the C++ makes the image and in which format.
struct ImagePair {
  const char* hlsl;       // a file of hlsl/ (the declaration is in it, or in a file it includes)
  const char* variable;   // the RWTexture2D's name
  const char* format;     // [[vk::image_format("...")]]
  const char* cpp;        // a file of src/gpu
  const char* marker;     // the image's name in an EnsureImage call (the format is the last RenderFormat before it), or a constant that is set to a RenderFormat (the format follows it)
  bool constant;          // marker is a constant
  const char* host;       // plume::RenderFormat::<host> the C++ makes it in
};

const ImagePair kImages[] = {
    {"city_rt_shadow_cs.hlsl", "g_output", "rgba8", "city_rt_shadow.cpp", "city rt shadow image", false, "R8G8B8A8_UNORM"},
    {"car_rt_ao_cs.hlsl", "g_output", "rgba8", "car_rt_ao.cpp", "car rt ao image", false, "R8G8B8A8_UNORM"},
    {"car_rt_check_cs.hlsl", "gView", "rgba8", "car_rt_scene.cpp", "car rt check view", false, "R8G8B8A8_UNORM"},
    {"car_rt_refl_cs.hlsl", "g_output", "rgba16f", "car_rt_refl.cpp", "car rt refl image", false, "R16G16B16A16_FLOAT"},
    {"car_rt_refl_cs.hlsl", "g_aux", "rgba8", "car_rt_refl.cpp", "car rt refl aux", false, "R8G8B8A8_UNORM"},
    {"car_rt_refl_denoise_cs.hlsl", "g_output", "rgba16f", "car_rt_refl.cpp", "car rt refl scratch", false, "R16G16B16A16_FLOAT"},
    {"car_rt_refl_hit_cs.hlsl", "g_output", "r16f", "car_rt_refl.cpp", "car rt refl hit distance", false, "R16_FLOAT"},
    {"ssr_rt_trace_cs.hlsl", "g_trace", "rgba16f", "ssr.cpp", "kFormat", true, "R16G16B16A16_FLOAT"},
    {"rt_denoise_engine.hlsli", "g_dn_out", "rgba16f", "rt_denoise.cpp", "rt denoise image", false, "R16G16B16A16_FLOAT"},
    {"gi_pixel_trace_cs.hlsl", "g_signal", "rgba16f", "gi_pixel.cpp", "gi pixel signal", false, "R16G16B16A16_FLOAT"},
    {"gi_pixel_trace_cs.hlsl", "g_depth", "r32f", "gi_pixel.cpp", "gi pixel depth 0", false, "R32_FLOAT"},
    {"gi_pixel_trace_cs.hlsl", "g_normal", "rgba16f", "gi_pixel.cpp", "gi pixel normal", false, "R16G16B16A16_FLOAT"},
    {"gi_pixel_trace_cs.hlsl", "g_hit", "rg16f", "gi_pixel.cpp", "gi pixel hit", false, "R16G16_FLOAT"},
    {"gi_field_reset_cs.hlsl", "g_irradiance", "rgba16f", "gi_field.cpp", "kIrradianceFormat", true, "R16G16B16A16_FLOAT"},
    {"gi_field_reset_cs.hlsl", "g_moments", "rg16f", "gi_field.cpp", "kMomentsFormat", true, "R16G16_FLOAT"},
    {"gi_field_reset_cs.hlsl", "g_weight", "r8", "gi_field.cpp", "kWeightFormat", true, "R8_UNORM"},
    {"gi_field_weights_cs.hlsl", "g_weight", "r8", "gi_field.cpp", "kWeightFormat", true, "R8_UNORM"},
    {"gi_update_blend_cs.hlsl", "g_irradiance", "rgba16f", "gi_field.cpp", "kIrradianceFormat", true, "R16G16B16A16_FLOAT"},
    {"gi_update_blend_cs.hlsl", "g_moments", "rg16f", "gi_field.cpp", "kMomentsFormat", true, "R16G16_FLOAT"},
    // INTEGRATION (feat/rt-lights-d1 on the city line): the ray traced light visibility's images, the normal one in the SNORM format the typed store of the D3D12 table needs
    {"rt_lights_trace_cs.hlsl", "g_signal0", "rgba8", "rt_lights.cpp", "rt lights signal", false, "R8G8B8A8_UNORM"},
    {"rt_lights_trace_cs.hlsl", "g_depth", "r16f", "rt_lights.cpp", "rt lights depth 0", false, "R16_FLOAT"},
    {"rt_lights_trace_cs.hlsl", "g_normal", "rgba8snorm", "rt_lights.cpp", "rt lights normal and share", false, "R8G8B8A8_SNORM"},
};

// The Vulkan format a [[vk::image_format]] name stands for, as the host format that has it: the table Plume's toVk(RenderFormat) and the SPIR-V image formats agree on.
std::string HostFormatOf(const std::string& spirv_format) {
  static const std::map<std::string, std::string> kMap = {{"rgba8", "R8G8B8A8_UNORM"}, {"rgba16f", "R16G16B16A16_FLOAT"}, {"rg16f", "R16G16_FLOAT"},
                                                          {"r16f", "R16_FLOAT"},        {"r32f", "R32_FLOAT"},            {"r8", "R8_UNORM"},
                                                          {"rgba8snorm", "R8G8B8A8_SNORM"}};
  const auto it = kMap.find(spirv_format);
  return it == kMap.end() ? std::string() : it->second;
}

std::string CppFormatOf(const std::string& cpp, const std::string& marker, bool constant) {
  const size_t at = marker.empty() ? std::string::npos : cpp.find(constant ? marker : "\"" + marker + "\"");
  if (at == std::string::npos) return std::string();
  const std::regex format(R"(plume::RenderFormat::(\w+))");
  if (constant) {
    // kFormat = plume::RenderFormat::X: the first RenderFormat after the constant's name
    const std::string tail = cpp.substr(at, 200);
    std::smatch m;
    return std::regex_search(tail, m, format) ? m[1].str() : std::string();
  }
  // EnsureImage(image, width, height, plume::RenderFormat::X, "name", flags): the last RenderFormat before the name, inside the same call
  const size_t from = at > 260 ? at - 260 : 0;
  const std::string head = cpp.substr(from, at - from);
  std::string last;
  for (std::sregex_iterator it(head.begin(), head.end(), format), end; it != end; ++it) last = (*it)[1].str();
  return last;
}

void TestStorageImageFormats(const std::filesystem::path& folder) {
  const std::filesystem::path hlsl = folder / "hlsl";
  for (const ImagePair& p : kImages) {
    const std::string label = std::string(p.hlsl) + " " + p.variable + " (" + p.format + ") against " + p.cpp + " " + p.marker;
    std::set<std::string> seen;
    std::string shader;
    Closure(hlsl, p.hlsl, &seen, &shader);
    const std::regex declaration(std::string(R"(\[\[vk::image_format\(\")") + p.format + R"(\"\)\]\]\s*(?:#endif\s*)*RWTexture2D<[^>]+>\s+)" + p.variable + R"(\b)");
    Check(std::regex_search(shader, declaration), label + ": the shader declares the image with that format for SPIR-V");
    Check(HostFormatOf(p.format) == p.host, label + ": the name stands for the host format " + p.host);
    const std::string cpp = ReadAll(folder / p.cpp);
    const std::string made = CppFormatOf(cpp, p.marker, p.constant);
    Check(made == p.host, label + ": the C++ makes the image in " + p.host + " (found '" + made + "')");
  }
  // Every image format the shaders name is one of the seven the table knows: a new one is a new row above.
  std::set<std::string> named;
  for (const auto& entry : std::filesystem::directory_iterator(folder / "hlsl")) {
    const std::string text = ReadAll(entry.path());
    const std::regex attribute(R"(vk::image_format\(\"(\w+)\"\))");
    for (std::sregex_iterator it(text.begin(), text.end(), attribute), end; it != end; ++it) named.insert((*it)[1].str());
  }
  for (const std::string& format : named) {
    Check(!HostFormatOf(format).empty(), "the storage image format '" + format + "' of some shader is one the checks know (add the format and its images to this test)");
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------- alignments

void TestAlignments() {
  namespace rr = mcla::gpu::car_rt_rules;
  namespace cf = larecomp::citypack;
  // Vulkan: an acceleration structure's offset in its buffer is a multiple of 256; the structures are placed at multiples of kStructureAlignment (the car's shared pages) or at
  // the start of a buffer of their own (the city's), and Plume rounds a structure's size and its scratch size to 256 as well.
  Check(rr::kStructureAlignment % 256 == 0, "the structures are placed at multiples of 256 (VkAccelerationStructureCreateInfoKHR::offset)");
  // Vertex data: the address is aligned to the size of the format's component (4 for R32G32B32_FLOAT); indices to the index type (4 for R32_UINT). The pool's blocks start on
  // 16 and every section inside a block does too (city_hit_format.h), so the build's addresses are 16 aligned and more.
  Check(cf::kPoolAlignment % 16 == 0, "the pool's blocks and sections are 16 aligned (vertex and index addresses need 4)");
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path folder = std::filesystem::path(__FILE__).parent_path();
  if (argc > 1) folder = argv[1];
  if (!std::filesystem::exists(folder / "CMakeLists.txt")) {
    std::printf("city_rt_vulkan_test: %s has no CMakeLists.txt: give it the src/gpu folder\n", folder.string().c_str());
    return 2;
  }
  TestRayShadersBuiltForVulkan(folder);
  TestStorageImageFormats(folder);
  TestAlignments();
  if (g_failed != 0) {
    std::printf("city_rt_vulkan_test: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("city_rt_vulkan_test: %d checks passed\n", g_checks);
  return 0;
}
