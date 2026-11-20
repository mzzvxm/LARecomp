// Check of the shadow filter variant's text transform (shadow_filter.h). A program of its own: it needs no
// renderer, no ReXGlue, no compiler and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_shadow_filter_test     (src/shaderpack/CMakeLists.txt)
//   <build dir>/src/shaderpack/larecomp_shadow_filter_test [<folder of translated HLSL> [<folder for the results>]]
//
// Without arguments it runs on small texts made here, in the three register conventions of the game's lookup
// (the registers are what differs between the shapes; the fixtures hold the lines the anchors look at and
// some other lines that must stay as they are). With a folder of translated HLSL (the builder's own
// output, `<group>/<entry>.hlsl` files, for example shaderpack/baseline/hlsl-rpf of the development PC) it
// also runs on every `*PS_ShadowBlend*.hlsl` in it and checks the census of the retail game's 159: 147
// edited (in four texts), 7 that read no atlas and 5 with a filter of their own left out, none failing. A
// second folder receives the four texts, for a compile with dxc (-T ps_6_0 -E main -HV 2021 -spirv
// -fvk-use-dx-layout, and without -spirv for DXIL).

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "shaderpack/variants/shadow_filter.h"

namespace sp = larecomp::shaderpack;

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

size_t Count(std::string_view text, std::string_view needle) {
  size_t count = 0;
  for (size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

bool Contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

// ---------------------------------------------------------------------------
// Fixtures: the parts of a translated collector shader the transform looks at.
// ---------------------------------------------------------------------------

struct Shape {
  const char* name;
  const char* coordinates_a;  // the register of the taps 1 and 2 (the cross's first two coordinates)
  const char* coordinates_b;  // of the taps 3 and 4
  const char* fetched;        // the register the four depths go to
  const char* depth;          // the receiver's depth register (.x)
  const char* compared;       // where the compare puts its result
  const char* average;        // the register the average goes to
};

const Shape kShapeA = {"A", "r2", "r3", "r2", "r1", "r2", "r1"};
const Shape kShapeB = {"B", "r2", "r3", "r2", "r0", "r0", "r1"};
const Shape kShapeFoliage = {"foliage", "r1", "r3", "r1", "r0", "r1", "r0"};

std::string Fetch(const std::string& dest, const std::string& coordinate) {
  return "\t\t" + dest + " = tfetch2D(DepthTextureSampler0_Texture2DDescriptorIndex, "
         "DepthTextureSampler0_SamplerDescriptorIndex, " + coordinate + ", float2(0, 0)).x;\n";
}

struct Options {
  bool with_radii = true;
  bool with_quarter = true;
  bool wrong_swizzle = false;
  bool own_filter = false;
  bool no_lookup = false;
  bool fifth_lookup = false;
  bool with_average = true;
  bool with_main = true;
  bool with_a0 = true;           // `int a0 = 0;` (the game's address register: 4 x the cascade's index)
  bool with_light_rows = true;   // the four reads of the cascade's light matrix, gLightMatrixArr(k + a0)
  bool crlf = false;
};

std::string Fixture(const Shape& shape, const Options& options = Options()) {
  const std::string a = shape.coordinates_a;
  const std::string b = shape.coordinates_b;
  std::string t;
  t += "#define g_InvResolutionScale      vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 592)\n";
  t += "// in the header of every shader\n";
  t += "#define DEFINE_SHARED_CONSTANTS() \\\n    float4 g_PosOffset : packoffset(c36); \\\n"
       "    float2 g_InvResolutionScale : packoffset(c37.x);\n\n";
  t += "float4 getWeights2DStub;\n";
  if (options.with_main) {
    t += "#ifndef __spirv__\n[shader(\"pixel\")]\n#endif\nvoid main(\n\tin float4 iPos : SV_Position,\n"
         "\tout float4 oC0 : SV_Target0\n)\n{\n";
  } else {
    t += "void notmain()\n{\n";
  }
  t += options.with_quarter ? "\tfloat4 c253 = asfloat(uint4(0x3F000000, 0x3E800000, 0x3F800000, 0x3E22F983));\n"
                            : "\tfloat4 c253 = asfloat(uint4(0x3F000000, 0x3F000000, 0x3F800000, 0x3E22F983));\n";
  if (options.with_radii) {
    t += "\tfloat4 c255 = asfloat(uint4(0x3A19999A, 0x3A4CCCCD, 0x39CCCCCD, 0x394CCCCD));\n";
  }
  if (options.with_a0) t += "\tint a0 = 0;\n";
  t += "\tCubeMapData cubeMapData = (CubeMapData)0;\n\n";
  t += "\tr3.xyzw = r1.xxxx * c255.xyzw;\n";
  if (options.with_light_rows) {
    // The cascade's light matrix as the game reads it (the three lines of every edited shader's text).
    t += "\tr4.xyzw = r0.zzzz * gLightMatrixArr(2 + a0).xyzw + gLightMatrixArr(3 + a0).xyzw;\n";
    t += "\tr4.xyzw = r0.yyyy * gLightMatrixArr(1 + a0).xywz + r4.xywz;\n";
    t += "\tr4.xyzw = r0.xxxx * gLightMatrixArr(0 + a0).xyzw + r4.xywz;\n";
  }
  t += "\tif (p0)\n\t{\n\t\t" + a + ".xyzw = r4.xxyy + " + a + ".zxyw;\n\t}\n";
  const std::string xw = options.wrong_swizzle ? "zw" : "xw";
  if (options.no_lookup) {
    t += "\tif (p0)\n\t{\n\t\tr7.x = 0.0;\n\t}\n";
  } else if (options.own_filter) {
    t += "\tif (p0)\n\t{\n" + Fetch("r3.x", "r0.xy") +
         "\t\tr4.zw = getWeights2D(DepthTextureSampler0_Texture2DDescriptorIndex, "
         "DepthTextureSampler0_SamplerDescriptorIndex, r0.xy, float2(0, 0)).xy;\n\t}\n";
  } else {
    t += "\tif (p0)\n\t{\n" + Fetch(std::string(shape.fetched) + ".x", a + "." + xw) + "\t}\n";
    t += "\tif (p0)\n\t{\n" + Fetch(std::string(shape.fetched) + ".y", a + ".yz") + "\t}\n";
    t += "\tif (p0)\n\t{\n\t\t" + b + ".xyzw = r4.xyxy + " + b + ".xyzw;\n\t\tps = r4.z * r4.w;\n\t\t" +
         shape.depth + ".x = ps;\n\t}\n";
    t += "\tif (p0)\n\t{\n" + Fetch(std::string(shape.fetched) + ".z", b + ".xy") + "\t}\n";
    t += "\tif (p0)\n\t{\n" + Fetch(std::string(shape.fetched) + ".w", b + ".zw") + "\t}\n";
    if (options.fifth_lookup) t += "\tif (p0)\n\t{\n" + Fetch("r9.x", "r8.xy") + "\t}\n";
    t += "\tif (p0)\n\t{\n\t\t" + std::string(shape.compared) + ".xyzw = " + shape.fetched + ".xyzw >= " +
         shape.depth + ".xxxx;\n\t}\n";
    if (options.with_average) {
      t += "\tif (p0)\n\t{\n\t\t" + std::string(shape.average) + ".x = dot(" + shape.compared +
           ".wzxy, c253.yyyy);\n\t}\n";
    }
  }
  t += "\tr0.y = -ShaderGlobals.z + c253.x;\n\toC0.xyzw = max(r0.xxxw, r0.xxxw);\n\treturn;\n}\n";
  if (options.crlf) {
    std::string out;
    for (const char c : t) {
      if (c == '\n') out += '\r';
      out += c;
    }
    return out;
  }
  return t;
}

const sp::VariantShader kShader = {"fixture__PS_ShadowBlend", 1};

// The edits one shape must show, by what the text then holds.
void CheckEdited(const Shape& shape, const std::string& hlsl) {
  const std::string what = std::string("shape ") + shape.name + ": ";
  const sp::VariantTransformResult result = sp::TransformShadowFilterHlsl(kShader, hlsl);
  Check(result.ok && !result.skip, what + "the transform succeeds (" + result.error + ")");
  if (!result.ok) return;
  const std::string& out = result.hlsl;
  const std::string a = shape.coordinates_a;
  const std::string b = shape.coordinates_b;
  // The taps are copied from the registers they are fetched from, in front of the fetch lines.
  const size_t tap1 = out.find("\t\tshadowTap1 = " + a + ".xw;\n");
  const size_t fetch1 = out.find(std::string("\t\t") + shape.fetched + ".x = tfetch2D(");
  Check(tap1 != std::string::npos && fetch1 != std::string::npos && tap1 < fetch1,
        what + "tap 1 is copied before its fetch");
  const size_t tap2 = out.find("\t\tshadowTap2 = " + a + ".yz;\n");
  Check(tap2 != std::string::npos && tap2 > fetch1, what + "tap 2 is copied after fetch 1 and before fetch 2");
  Check(Contains(out, "\t\tshadowTap3 = " + b + ".xy;\n") && Contains(out, "\t\tshadowTap4 = " + b + ".zw;\n"),
        what + "taps 3 and 4 are copied");
  // The receiver's depth is copied before the compare that overwrites it, and the compare is still there.
  const std::string compare = std::string("\t\t") + shape.compared + ".xyzw = " + shape.fetched + ".xyzw >= " +
                              shape.depth + ".xxxx;\n";
  const size_t depth_copy = out.find(std::string("\t\tshadowDepth = ") + shape.depth + ".x;\n");
  const size_t compare_at = out.find(compare);
  Check(depth_copy != std::string::npos && compare_at != std::string::npos && depth_copy + 1 < compare_at &&
            out.find('\n', depth_copy) + 1 == compare_at,
        what + "the depth is copied right before the compare, which is kept");
  // The average is the helper's call, into the average's register.
  Check(Contains(out, std::string("\t\t") + shape.average + ".x = ShadowFilterSoft(DepthTextureSampler0_"
                      "Texture2DDescriptorIndex, DepthTextureSampler0_SamplerDescriptorIndex, shadowTap1, "
                      "shadowTap2, shadowTap3, shadowTap4, shadowDepth, iPos.xy, a0);\n"),
        what + "the average is the helper's call (with the game's a0)");
  Check(!Contains(out, "dot(" + std::string(shape.compared) + ".wzxy, c253.yyyy)"), what + "the game's average is gone");
  // The four fetches stay (dead code), once each.
  Check(Count(out, "= tfetch2D(DepthTextureSampler0_Texture2DDescriptorIndex") == 4,
        what + "the game's four fetches stay");
  // The declarations: locals after the cube map data, the constants in both backends, the helper before main.
  Check(Contains(out, "CubeMapData cubeMapData = (CubeMapData)0;\n\tfloat2 shadowTap1 = 0.0"),
        what + "the locals follow the cube map data");
  Check(Count(out, "SharedConstants + 608, 0x10)") == 1, what + "g_ShadowFilter in the SPIR-V constants");
  Check(Contains(out, "float2 g_InvResolutionScale : packoffset(c37.x); \\\n    float4 g_ShadowFilter : packoffset(c38);"),
        what + "g_ShadowFilter in the DXIL constants");
  Check(out.find("float ShadowFilterSoft(") < out.find("void main("), what + "the helper is before main");
  // Contact hardening's pieces: the penumbra from the light matrix's rows, the mode bit, the search's constants.
  Check(Count(out, "float2 ShadowFilterPenumbra(") == 1 && Count(out, "gLightMatrixArr(0 + base)") == 2 &&
            Count(out, "gLightMatrixArr(3 + base)") == 2,
        what + "the penumbra and the receiver plane's helpers read the four rows of the cascade by its base");
  // The noise: only with mode bit 4 (a temporal upscaler); the plane: only with bit 1 (true PCF taps), derivatives of the
  // pixel's own centre and depth, held to a slope limit; the search and the PCF taps both compare at the plane.
  Check(Contains(out, "(mode & 4u) != 0u ? 6.28318530718 * ShadowFilterNoise(pixel, constants.z) : kShadowFilterFixedAngle"),
        what + "the disk is turned by noise only with mode bit 4");
  Check(Count(out, "float2 ShadowFilterPlane(") == 1 && Count(out, "ShadowFilterPlane(centre, depth, cascadeBase)") == 1 &&
            Contains(out, "ddx(centre)") && Contains(out, "ddy(depth)") && Contains(out, "kShadowFilterMaxSlope"),
        what + "the receiver plane is worked out once, from the derivatives, behind the Gather bit");
  Check(Contains(out, "if (stored < depth + dot(slope, offset))") &&
            Contains(out, "depth + dot(slope, offset), gather, slope);"),
        what + "the blocker search and the taps compare at the receiver's plane");
  Check(Contains(out, "(mode & 2u) != 0u") && Contains(out, "kShadowFilterSearchTaps = 8") &&
            Contains(out, "kShadowFilterSunTan = 0.0047123890"),
        what + "the blocker search is behind mode bit 2, 8 taps, the sun's 0.27 degrees");
  // The game's own reads of its light matrix stay where they were (the helper's are another text: `+ base`).
  Check(Count(out, "gLightMatrixArr(0 + a0)") == 1 && Count(out, "gLightMatrixArr(3 + a0)") == 1,
        what + "the game's reads of its light matrix are untouched");
  Check(out.find("#ifndef __spirv__\n[shader(\"pixel\")]") != std::string::npos, what + "main's attribute is kept");
  // Everything else is as it was: the text without the edits' lines equals the fixture's.
  Check(Contains(out, "\tr0.y = -ShaderGlobals.z + c253.x;\n\toC0.xyzw = max(r0.xxxw, r0.xxxw);\n\treturn;\n}\n"),
        what + "the lines around are as they were");
  // The same input gives the same output.
  const sp::VariantTransformResult again = sp::TransformShadowFilterHlsl(kShader, hlsl);
  Check(again.ok && again.hlsl == out, what + "the transform is deterministic");
}

void CheckFails(const std::string& hlsl, const std::string& what, const std::string& error_has) {
  const sp::VariantTransformResult result = sp::TransformShadowFilterHlsl(kShader, hlsl);
  Check(!result.ok && !result.skip, what + ": it fails and does not skip");
  Check(Contains(result.error, error_has) && Contains(result.error, "fixture__PS_ShadowBlend"),
        what + ": the error names the shader and says '" + error_has + "' (got '" + result.error + "')");
}

void CheckSkips(const std::string& hlsl, const std::string& what, std::string_view why) {
  const sp::VariantTransformResult result = sp::TransformShadowFilterHlsl(kShader, hlsl);
  Check(!result.ok && result.skip && result.error == why, what + ": it is left out (" + result.error + ")");
}

void CheckFixtures() {
  for (const Shape* shape : {&kShapeA, &kShapeB, &kShapeFoliage}) {
    CheckEdited(*shape, Fixture(*shape));
    // The same text with Windows line ends gives the same edited text as with the others.
    Options crlf;
    crlf.crlf = true;
    const sp::VariantTransformResult plain = sp::TransformShadowFilterHlsl(kShader, Fixture(*shape));
    const sp::VariantTransformResult windows = sp::TransformShadowFilterHlsl(kShader, Fixture(*shape, crlf));
    Check(plain.ok && windows.ok && plain.hlsl == windows.hlsl,
          std::string("shape ") + shape->name + ": CRLF and LF texts give one result");
  }
  // The shapes give different texts (registers), and the call names its own registers.
  Check(sp::TransformShadowFilterHlsl(kShader, Fixture(kShapeA)).hlsl !=
            sp::TransformShadowFilterHlsl(kShader, Fixture(kShapeB)).hlsl,
        "shapes A and B give different texts");

  Options o;
  o = Options();
  o.no_lookup = true;
  CheckSkips(Fixture(kShapeA, o), "no lookup", sp::kShadowSkipNoLookup);
  o = Options();
  o.own_filter = true;
  CheckSkips(Fixture(kShapeA, o), "its own filter", sp::kShadowSkipOwnFilter);

  o = Options();
  o.fifth_lookup = true;
  CheckFails(Fixture(kShapeA, o), "five lookups", "5 times");
  o = Options();
  o.with_radii = false;
  CheckFails(Fixture(kShapeA, o), "no radii line", "c255");
  o = Options();
  o.with_quarter = false;
  CheckFails(Fixture(kShapeA, o), "c253.y is not a quarter", "c253.y");
  o = Options();
  o.wrong_swizzle = true;
  CheckFails(Fixture(kShapeA, o), "a tap from the wrong components", "tap 1");
  o = Options();
  o.with_average = false;
  CheckFails(Fixture(kShapeA, o), "no average", "c253.yyyy");
  o = Options();
  o.with_main = false;
  CheckFails(Fixture(kShapeA, o), "no main", "pixel shader main");
  // Contact hardening needs the game's address register and the four reads of the light matrix.
  o = Options();
  o.with_a0 = false;
  CheckFails(Fixture(kShapeA, o), "no address register", "int a0 = 0;");
  o = Options();
  o.with_light_rows = false;
  CheckFails(Fixture(kShapeA, o), "no light matrix rows", "gLightMatrixArr(0 + a0)");
  // A compare that does not read the fetched register, a fetch with another offset.
  std::string broken = Fixture(kShapeA);
  const size_t at = broken.find("r2.xyzw >= r1.xxxx");
  broken.replace(at, 6, "r5.xyzw");
  CheckFails(broken, "a compare of another register", "r2.xyzw >= ");
  broken = Fixture(kShapeA);
  broken.replace(broken.find("float2(0, 0)).x;"), 16, "float2(0.5, 0)).x;");
  CheckFails(broken, "a fetch with an offset", "tfetch2D");
  broken = Fixture(kShapeA);
  broken.replace(broken.find("gLightMatrixArr(3 + a0)"), 23, "gLightMatrixArr(3 + a1)");
  CheckFails(broken, "a light matrix row read by another register", "gLightMatrixArr(3 + a0)");
  // The original is not touched: the variant's text is a new string.
  const std::string original = Fixture(kShapeA);
  const std::string copy = original;
  (void)sp::TransformShadowFilterHlsl(kShader, original);
  Check(original == copy, "the input is not changed");

  // The variant's definition.
  const sp::VariantDef def = sp::MakeShadowFilterVariant();
  Check(def.feature == larecomp::shaderpack::kFeatureShadowFilter && def.name == "shadow_filter" && def.needs_rpf &&
            def.pixel_shaders.empty() && def.pixel_name_suffixes.size() == 2 && def.transform,
        "the definition: feature, name, the RPF, two name suffixes, a transform");
  Check(def.version == sp::kShadowFilterVariantVersion && def.version >= 1, "the definition carries the version");
}

// ---------------------------------------------------------------------------
// The game's own files (optional)
// ---------------------------------------------------------------------------

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void CheckGameFiles(const std::filesystem::path& folder, const std::filesystem::path& results) {
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(folder)) {
    const std::string name = entry.path().filename().string();
    if (entry.is_regular_file() && entry.path().extension() == ".hlsl" &&
        (name.ends_with("__PS_ShadowBlend.hlsl") || name.ends_with("__PS_ShadowBlendAlpha.hlsl"))) {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  size_t edited = 0;
  std::map<std::string, size_t> skipped;
  std::set<std::string> texts;
  for (const auto& file : files) {
    const std::string stem = file.stem().string();
    const sp::VariantShader shader = {stem, 0};
    const sp::VariantTransformResult result = sp::TransformShadowFilterHlsl(shader, ReadFile(file));
    if (result.ok) {
      ++edited;
      texts.insert(result.hlsl);
    } else if (result.skip) {
      ++skipped[result.error];
    } else {
      Check(false, "game file " + file.string() + ": " + result.error);
    }
  }
  std::printf("game files: %zu collector shaders, %zu edited in %zu texts", files.size(), edited, texts.size());
  for (const auto& [why, count] : skipped) std::printf("; %zu left out: %s", count, why.c_str());
  std::printf("\n");
  Check(files.size() == 159, "the retail game has 159 collector shaders (found " + std::to_string(files.size()) + ")");
  Check(edited == 147, "147 shaders are edited (" + std::to_string(edited) + ")");
  Check(texts.size() == 4, "in four texts (" + std::to_string(texts.size()) + ")");
  Check(skipped[std::string(sp::kShadowSkipNoLookup)] == 7, "7 read no shadow map");
  Check(skipped[std::string(sp::kShadowSkipOwnFilter)] == 5, "5 have a filter of their own");
  if (!results.empty()) {
    std::filesystem::create_directories(results);
    size_t n = 0;
    for (const std::string& text : texts) {
      std::ofstream(results / ("shadow_filter_" + std::to_string(++n) + ".hlsl"), std::ios::binary) << text;
    }
    std::printf("the %zu texts are in %s\n", texts.size(), results.string().c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  CheckFixtures();
  if (argc > 1) {
    CheckGameFiles(argv[1], argc > 2 ? std::filesystem::path(argv[2]) : std::filesystem::path());
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
