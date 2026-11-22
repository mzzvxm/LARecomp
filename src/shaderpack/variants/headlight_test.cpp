// Check of the headlight variant's text transform (headlight.h). A program of its own: it needs no renderer, no ReXGlue, no compiler and no game files,
// and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_headlight_variant_test     (src/shaderpack/CMakeLists.txt)
//   <build dir>/src/shaderpack/larecomp_headlight_variant_test [<folder of translated HLSL> [<folder for the results>]]
//
// Without arguments it runs on small texts made here (the shapes of the translator's lines the edits look at: the grid lookup, the ambient's statement in
// its permuted and its one-channel forms, a position that is a copy chain, and the shapes that are left out), and checks that the helper's numbers are
// src/gpu/headlight_rig_rules.h's and that the constants it declares sit where HeadlightConstants has them. With a folder of translated HLSL (the builder's
// own output, `<group>/<entry>.hlsl` files, for example shaderpack/baseline/hlsl-rpf of the development PC) it also runs on every pixel shader of the lit
// family in it (the names that end with one of kHeadlightSuffixes), prints the census (edited, left out and why) and checks the retail game's numbers.
// A second folder receives every edited text, for a compile with dxc (-T ps_6_0 -E main -HV 2021 -spirv -fvk-use-dx-layout, and without -spirv for
// DXIL; with -DXENOS_SPEC_CONSTANTS=0 and =2 for the alpha test twin).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "gpu/headlight_rig_rules.h"
#include "gpu/light_shadow_filter.h"
#include "shaderpack/variants/headlight.h"
#include "shaderpack/variants/shadow_filter.h"

namespace sp = larecomp::shaderpack;
namespace hr = mcla::gpu::headlight_rules;
namespace sf = larecomp::gpu::shadow_filter;

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
  for (size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) ++count;
  return count;
}

bool Contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

struct Shape {
  const char* name;
  std::string registers;   // the declarations of the interpolants
  std::string before_grid; // lines before the grid lookup (copies, other statements)
  std::string grid;        // the grid lookup
  std::string ambient;     // the statement(s) that add the ambient colour, one tab each
  const char* position;    // what the edit must call the pixel's world position
  const char* terms;       // what each ambient statement must end with
};

std::string Fixture(const Shape& shape, bool with_main = true, bool with_cube = true) {
  std::string t;
  t += "#define g_InvResolutionScale      vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 592)\n";
  t += "#define DEFINE_SHARED_CONSTANTS() \\\n    float4 g_PosOffset : packoffset(c36); \\\n    float2 g_InvResolutionScale : packoffset(c37.x);\n\n";
  t += "float4 tfetch2DLod(uint a, uint b, float2 c, float2 d, float e) { return 0.0; }\n";
  if (with_main) {
    t += "#ifndef __spirv__\n[shader(\"pixel\")]\n#endif\nvoid main(\n\tin float4 iPos : SV_Position,\n\tout float4 oC0 : SV_Target0\n)\n{\n";
  } else {
    t += "void notmain()\n{\n";
  }
  t += "\tfloat4 c254 = asfloat(uint4(0xBF800000, 0x3F800000, 0x40000000, 0x3FA00000));\n";
  t += shape.registers;
  t += "\tfloat ps = 0.0;\n\tint a0 = 0;\n";
  if (with_cube) t += "\tCubeMapData cubeMapData = (CubeMapData)0;\n\n";
  t += shape.before_grid;
  t += shape.grid;
  t += "\tr5.xyzw = tfetch2D(ColorT1Sampler_Texture2DDescriptorIndex, ColorT1Sampler_SamplerDescriptorIndex, r4.xy, float2(0, 0)).xwzy;\n";
  t += shape.ambient;
  t += "\tr3.yzw = r6.xzw * r3.xyz;\n\toC0.w = max(gLightAmbient.w, gLightAmbient.w);\n\treturn;\n}\n";
  return t;
}

const Shape kDefault = {
    "default",
    "\tfloat4 r0 = iTexCoord0;\n\tfloat4 r1 = iTexCoord1;\n\tfloat4 r2 = iTexCoord2;\n\tfloat4 r3 = iColor1;\n",
    "\tr9.x = r0.x * r3.w;\n",
    "\tr2.zw = r1.xz * LightSortScaleBias.xy + LightSortScaleBias.zw;\n",
    "\tr3.xyz = r3.xyz + gLightAmbient.xyz;\n",
    "iTexCoord1.xyz",
    " + gLightAmbient.xyz + hlLight.xyz;\n"};

// The traffic body's shape: the lanes in another order, the ambient's swizzle permuted.
const Shape kPermuted = {
    "permuted",
    "\tfloat4 r0 = iTexCoord1;\n\tfloat4 r1 = iTexCoord2;\n",
    "",
    "\tr2.xy = r0.zx * LightSortScaleBias.yx + LightSortScaleBias.wz;\n",
    "\tr2.xyz = r2.zxy + gLightAmbient.zxy;\n",
    "iTexCoord1.xyz",
    " + gLightAmbient.zxy + hlLight.zxy;\n"};

// The ambient one channel at a time (the scalar unit), the product form and a four-lane grid.
const Shape kScalar = {
    "scalar",
    "\tfloat4 r0 = iTexCoord0;\n\tfloat4 r2 = iTexCoord3;\n",
    "",
    "\tr4.xy = r0.xz * LightSortScaleBias.xy + LightSortScaleBias.zw;\n",
    "\tps = gLightAmbient.x * r0.w;\n\tr6.x = ps;\n\tps = gLightAmbient.y + r3.y;\n\tr6.y = ps;\n\tps = gLightAmbient.z + r3.x;\n\tr6.z = ps;\n",
    "iTexCoord0.xyz",
    ""};

// The road's shape: the grid's lanes are copies of an interpolant's, some in branches, and an earlier write that is not a copy.
const Shape kCopies = {
    "copies",
    "\tfloat4 r1 = iTexCoord1;\n\tfloat4 r5 = iColor0;\n\tbool p0 = false;\n",
    "\tif (!p0)\n\t{\n\t\tps = max(r1.x, r1.x);\n\t\tr5.y = ps;\n\t}\n\tif (p0)\n\t{\n\t\tr5.xyzw = r8.xyzw * r5.xyzw;\n\t}\n"
    "\tif (p0)\n\t{\n\t\tr5.z = max(r1.y, r1.y);\n\t\tps = max(r1.x, r1.x);\n\t\tr5.y = ps;\n\t}\n\tr5.x = max(r1.z, r1.z);\n",
    "\tr8.zw = r5.yx * LightSortScaleBias.xy + LightSortScaleBias.zw;\n",
    "\tr6.xyz = r8.zxw + gLightAmbient.zxy;\n",
    "iTexCoord1.xyz",
    " + gLightAmbient.zxy + hlLight.zxy;\n"};

const sp::VariantShader kShader = {"fixture__PSMultiLight", 1};

std::string WithAmbient(const Shape& base, std::string ambient) {
  Shape shape = base;
  shape.ambient = std::move(ambient);
  return Fixture(shape);
}

void CheckEdited(const Shape& shape) {
  const std::string what = std::string("shape ") + shape.name + ": ";
  const sp::VariantTransformResult result = sp::TransformHeadlightHlsl(kShader, Fixture(shape));
  Check(result.ok && !result.skip, what + "the transform succeeds (" + result.error + ")");
  if (!result.ok) return;
  const std::string& out = result.hlsl;
  Check(Contains(out, std::string("CubeMapData cubeMapData = (CubeMapData)0;\n\tfloat4 hlLight = HeadlightLight(") + shape.position + ");"),
        what + "the light is made at the start of main from the pixel's world position " + shape.position);
  if (std::string_view(shape.terms).size() != 0) {
    Check(Contains(out, shape.terms), what + "the ambient statement ends with the term (" + shape.terms + ")");
  } else {
    Check(Contains(out, "gLightAmbient.x * r0.w + hlLight.x;") && Contains(out, "gLightAmbient.y + r3.y + hlLight.y;") &&
              Contains(out, "gLightAmbient.z + r3.x + hlLight.z;"),
          what + "each one-channel statement gets its own channel's term");
  }
  // The alpha statement and the lines around are as they were.
  Check(Contains(out, "oC0.w = max(gLightAmbient.w, gLightAmbient.w);\n\treturn;\n}\n"), what + "the alpha statement is untouched");
  Check(Contains(out, "\tr3.yzw = r6.xzw * r3.xyz;\n"), what + "the line after the ambient is as it was");
  // The helper is before main, once; the declarations are in both backends.
  Check(out.find("float4 HeadlightLight(") < out.find("void main("), what + "the helper is before main");
  Check(Count(out, "float4 HeadlightLight(") == 1 && Count(out, " = HeadlightLight(") == 1, what + "one helper, one call");
  Check(Contains(out, "float2 g_InvResolutionScale : packoffset(c37.x); \\\n    float4 g_HlHeader : packoffset(c38); \\\n"),
        what + "the constants follow the DXIL declaration");
  Check(Contains(out, "#define g_HlHeader vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 608, 0x10)\n"),
        what + "the constants follow the SPIR-V declaration");
  Check(out.find("#ifndef __spirv__\n[shader(\"pixel\")]") != std::string::npos, what + "main's attribute is kept");
  const sp::VariantTransformResult again = sp::TransformHeadlightHlsl(kShader, Fixture(shape));
  Check(again.ok && again.hlsl == out, what + "the transform is deterministic");
  // The same text with Windows line ends gives the same result.
  std::string crlf;
  for (const char c : Fixture(shape)) {
    if (c == '\n') crlf += '\r';
    crlf += c;
  }
  const sp::VariantTransformResult windows = sp::TransformHeadlightHlsl(kShader, crlf);
  Check(windows.ok && windows.hlsl == out, what + "CRLF and LF texts give one result");
}

void CheckSkips(const std::string& hlsl, const std::string& what, std::string_view why) {
  const sp::VariantTransformResult result = sp::TransformHeadlightHlsl(kShader, hlsl);
  Check(!result.ok && result.skip && result.error == why, what + ": it is left out (" + result.error + ")");
}

void CheckFixtures() {
  for (const Shape* shape : {&kDefault, &kPermuted, &kScalar, &kCopies}) CheckEdited(*shape);
  Check(sp::TransformHeadlightHlsl(kShader, Fixture(kDefault)).hlsl != sp::TransformHeadlightHlsl(kShader, Fixture(kPermuted)).hlsl,
        "different shapes give different texts");

  // What is left out, and why.
  CheckSkips(Fixture(kDefault, false), "no main", sp::kHeadlightSkipNoGrid);
  {
    Shape none = kDefault;
    none.grid = "";
    CheckSkips(Fixture(none), "no grid lookup", sp::kHeadlightSkipNoGrid);
    Shape two = kDefault;
    two.grid += two.grid;
    CheckSkips(Fixture(two), "two grid lookups", sp::kHeadlightSkipNoGrid);
    Shape computed = kDefault;
    computed.before_grid = "\tr1.xyz = r2.xyz * c254.yyy;\n";  // the grid's register is computed, not copied
    CheckSkips(Fixture(computed), "a computed position", sp::kHeadlightSkipPosition);
    Shape colour = kDefault;
    colour.registers = "\tfloat4 r0 = iTexCoord0;\n\tfloat4 r1 = iColor0;\n\tfloat4 r3 = iColor1;\n";
    CheckSkips(Fixture(colour), "a position that is not an iTexCoord", sp::kHeadlightSkipPosition);
    Shape same_lane = kDefault;
    same_lane.grid = "\tr2.zw = r1.xx * LightSortScaleBias.xy + LightSortScaleBias.zw;\n";
    CheckSkips(Fixture(same_lane), "both lanes from one component", sp::kHeadlightSkipPosition);
    Shape mixed = kCopies;
    mixed.before_grid += "\tr5.y = max(r1.y, r1.y);\n";  // a copy of another lane: the lane is not one source's
    CheckSkips(Fixture(mixed), "copies that disagree", sp::kHeadlightSkipPosition);
    Shape two_registers = kCopies;
    two_registers.before_grid = "\tr5.y = max(r1.x, r1.x);\n\tr5.x = max(r2.z, r2.z);\n";
    two_registers.registers += "\tfloat4 r2 = iTexCoord2;\n";
    CheckSkips(Fixture(two_registers), "lanes from two interpolants", sp::kHeadlightSkipPosition);
  }
  CheckSkips(WithAmbient(kDefault, "\tr19.xyz = select(r1.xxx > 0.0, c254.zzz, gLightAmbient.xyz);\n"), "the ambient picked by a flag (an interior)", sp::kHeadlightSkipAmbient);
  CheckSkips(WithAmbient(kDefault, "\toC0.xyz = r1.xyz * gLightAmbient.xyz + r0.xyz;\n"), "the ambient into the output", sp::kHeadlightSkipAmbient);
  CheckSkips(WithAmbient(kDefault, "\tif (p0)\n\t{\n\t\tr3.xyz = r3.xyz + gLightAmbient.xyz;\n\t}\n"), "the ambient in a branch", sp::kHeadlightSkipAmbient);
  CheckSkips(WithAmbient(kDefault, "\tr2.xyzw = r6.wwww * gLightAmbient.zxyy + r0.zxyw;\n"), "a four lane statement with a repeated channel", sp::kHeadlightSkipAmbient);
  CheckSkips(WithAmbient(kDefault, "\tr3.xy = r3.xy + gLightAmbient.xyz;\n"), "lanes and swizzle of another length", sp::kHeadlightSkipAmbient);
  CheckSkips(WithAmbient(kDefault, "\tr3.xyz = r3.xyz + r3.xyz;\n"), "no ambient at all", sp::kHeadlightSkipAmbient);
  CheckSkips(WithAmbient(kDefault, "\tr3.xy = r3.xy + gLightAmbient.xy;\n"), "only two channels", sp::kHeadlightSkipAmbient);
  CheckSkips(WithAmbient(kDefault, "\tr3.xyz = r3.xyz + gLightAmbient.xyz;\n\tr4.xyz = r4.xyz + gLightAmbient.xyz;\n"), "the ambient added twice", sp::kHeadlightSkipAmbient);

  // The text that came out wrong after the edits is an error, not a skip: an anchor that is not there.
  {
    const sp::VariantTransformResult result = sp::TransformHeadlightHlsl(kShader, Fixture(kDefault, true, false));
    Check(!result.ok && !result.skip && Contains(result.error, "fixture__PSMultiLight") && Contains(result.error, "main's registers"),
          "no cube map line: an error that names the shader");
  }
  // In a composition the block is where the chain puts it: after a 16 byte stage that is applied before it (the HDR scene's, feature 0) it is at byte 624, in both
  // backends and in every member's offset; the text is the same one shifted by 16.
  {
    namespace chain = larecomp::shaderpack::chain;
    const sp::StageContext shifted = sp::ContextIn(sp::kFeatureHeadlight, chain::FeatureBit(sp::kFeatureHdrScene) | chain::FeatureBit(sp::kFeatureHeadlight));
    const sp::VariantTransformResult moved = sp::TransformHeadlightHlsl(kShader, Fixture(kDefault), shifted);
    const sp::VariantTransformResult alone = sp::TransformHeadlightHlsl(kShader, Fixture(kDefault));
    Check(shifted.constants_offset == 624 && moved.ok && alone.ok, "composed after a 16 byte stage: the block's offset is 624");
    Check(Contains(moved.hlsl, "#define g_HlHeader vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 624, 0x10)\n") &&
              Contains(moved.hlsl, "g_HlLight(INDEX) vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 688 + (INDEX) * 16, 0x10)") &&
              Contains(moved.hlsl, "float4 g_HlHeader : packoffset(c39); \\\n") && Contains(moved.hlsl, "float4 g_HlLights[12] : packoffset(c43);") &&
              Contains(moved.hlsl, "float4 g_HlShadowArr[48] : packoffset(c58);") && Count(moved.hlsl, "SharedConstants + 608") == 0,
          "composed: every member is 16 bytes further in both backends");
    Check(moved.hlsl.size() == alone.hlsl.size(), "composed: the same text but for the offsets (the digits count the same)");
  }
  // The input is not touched.
  const std::string original = Fixture(kDefault);
  const std::string copy = original;
  (void)sp::TransformHeadlightHlsl(kShader, original);
  Check(original == copy, "the input is not changed");

  // The definition.
  const sp::VariantDef def = sp::MakeHeadlightVariant();
  Check(def.feature == larecomp::shaderpack::kFeatureHeadlight && larecomp::shaderpack::chain::Find(def.feature) &&
            larecomp::shaderpack::chain::Find(def.feature)->name == def.name && larecomp::shaderpack::chain::Find(def.feature)->spec_twin &&
            def.name == "headlight" && def.needs_rpf && def.pixel_shaders.empty() && def.pixel_name_suffixes.size() == 5 && def.transform,
        "the definition: its feature (and the table's row, with the alpha test twin), name, the RPF, five name suffixes, a transform");
  Check(def.version == sp::kHeadlightVariantVersion && def.version >= 1, "the definition carries the version");
  Check(larecomp::shaderpack::chain::Find(def.feature)->constants_bytes == 1072 && larecomp::shaderpack::chain::SoloWord(def.feature) >= larecomp::shaderpack::kFirstPackedVariant,
        "the table's row says 1072 bytes and the word is a packed one");
  // The lit family is in no other variant by name: none of its suffixes is, or ends with, the shadow collector's (the other variants name single shaders
  // that are not in it: the car paint, the chrome, the sample pass, the UI's textured shaders; shaderpack/variant_overlap.h).
  for (const std::string_view suffix : sp::kHeadlightSuffixes) {
    for (const std::string_view other : {sp::kShadowBlendSuffix, sp::kShadowBlendAlphaSuffix}) {
      Check(!suffix.ends_with(other) && !other.ends_with(suffix), std::string(suffix) + " and " + std::string(other) + " name different shaders");
    }
  }
  Check(larecomp::shaderpack::kFeatureHeadlight != larecomp::shaderpack::kFeatureUiSharp && larecomp::shaderpack::kFeatureHeadlight != larecomp::shaderpack::kFeatureGBuffer &&
            larecomp::shaderpack::kFeatureHeadlight != larecomp::shaderpack::kFeatureLampShadow,
        "the feature is not another one's");
}

// ---------------------------------------------------------------------------
// The helper against the rules, the constants against the block
// ---------------------------------------------------------------------------

double ConstantOf(std::string_view text, std::string_view name) {
  const std::string key = "static const float " + std::string(name) + " = ";
  const size_t at = text.find(key);
  if (at == std::string_view::npos) return -1e30;
  return std::stod(std::string(text.substr(at + key.size(), 24)));
}

void CheckHelper() {
  const std::string_view helper = sp::HeadlightHelperText();
  const struct {
    const char* name;
    double want;
  } kConstants[] = {
      {"kHlPeak", hr::kPeak},           {"kHlFalloffDistance", hr::kFalloffDistance}, {"kHlSurfaceWrap", hr::kSurfaceWrap},
      {"kHlCutLow", hr::kCutLow},       {"kHlCutHigh", hr::kCutHigh},                 {"kHlNearStart", hr::kNearStart},
      {"kHlNearSpan", hr::kNearSpan},     {"kHlPcfMinRadius", sf::kPcfMinRadius},   {"kHlPcfMaxRadius", sf::kPcfMaxRadius},
      {"kHlLampRadius", sf::kLampRadius}, {"kHlMaxSlope", sf::kMaxSlope},           {"kHlSlopeBias", sf::kSlopeBias},
      {"kHlBorderFade", sf::kBorderFade}, {"kHlDegenerate", sf::kDegenerate},
  };
  for (const auto& c : kConstants) {
    const double got = ConstantOf(helper, c.name);
    Check(std::fabs(got - c.want) < 1e-6, std::string("the helper's ") + c.name + " is the rules' (" + std::to_string(got) + " against " + std::to_string(c.want) + ")");
  }
  // The shadow hook's lookup (src/gpu/light_shadow_filter.h ViewLit is its CPU twin, light_shadow_filter_test.cpp holds the twin to the scenes): FOUR GATHERS cover the 4 x 4 texels
  // around the point (every texel is read: nothing thinner than a texel falls between taps), each compared with the receiver's depth at that texel on the receiver's plane; the
  // kernel is a box of 2 r texels, r from the lamp's penumbra through the average blocker held to [0.5, 1.5]; the shadow fades out at the tile's border.
  Check(sf::kPcfMaxRadius == 1.5f && sf::kPcfMinRadius == 0.5f, "the kernel's half width is 0.5 to 1.5 texels: the 4 x 4 footprint holds a box of half width 1.5 wherever the point is in its texel");
  Check(Contains(helper, "float4 HeadlightQuadLit(") && Contains(helper, "float HeadlightShadow(uint view, float3 world, float3 normal)"), "the quad compare and the lookup (it takes the surface's normal)");
  Check(Count(helper, ".GatherRed(mapSampler, ") == 5 && Contains(helper, "map.GatherRed(mapSampler, corner);") && Contains(helper, "corner + float2(2.0 * t.x, 0.0)") &&
            Contains(helper, "corner + float2(0.0, 2.0 * t.y)") && Contains(helper, "corner + 2.0 * t"),
        "four gathers, at the corners of the four quads of the footprint (and the wide footprint's one, in its loop)");
  Check(Contains(helper, "float2 first = floor(c - 0.5) - 1.0;") && Contains(helper, "float2 corner = (first + 1.0) * t;"), "the footprint starts one texel before the bilinear footprint, the quads' corner is a texel in");
  Check(Contains(helper, "Texture2D<float4> map = g_Texture2DDescriptorHeap[TFETCH_INDEX(atlas.x)];") && Contains(helper, "SamplerState mapSampler = g_SamplerDescriptorHeap[atlas.y];"),
        "the atlas is read through the guest's heaps, as tfetch2DLod does");
  Check(!Contains(helper, "HeadlightShadowTexel") && !Contains(helper, "corners == 0.0") && !Contains(helper, "kHlShadowKernel"), "the 3 x 3 box and its four tap shortcut are gone");
  // The plane: the Jacobians of u, v and z in the world point, the receiver's slope over one texel from the normal, its limit and the degenerate case.
  Check(Contains(helper, "float3 ju = (row0.xyz - s.x * axis) * (invW / t.x);") && Contains(helper, "float3 jv = (row1.xyz - s.y * axis) * (invW / t.y);") &&
            Contains(helper, "float3 jz = (row2.xyz - s.z * axis) * invW;"),
        "the Jacobians of u and v (in texels) and of the depth");
  Check(Contains(helper, "float scaleA = dot(row2.xyz, axis) / dot(axis, axis);") && Contains(helper, "float2 limit = kHlMaxSlope * (scaleA - s.z) * invW / float2(lenU, lenV);"),
        "A = f / (f - n) comes from the matrix's own rows, the slope limit from it");
  Check(Contains(helper, "float3 vn = cross(jv, normal);") && Contains(helper, "float det = dot(ju, vn);") &&
            Contains(helper, "slope = clamp(float2(dot(jz, vn), dot(jz, cross(normal, ju))) / det, -limit, limit);") && Contains(helper, "if (abs(det) > kHlDegenerate * lenU * lenV)"),
        "the slope of the plane over one texel along u and v, held to the limit, none when the plane is edge on");
  Check(Contains(helper, "float depth = s.z - (shadowParams.x + kHlSlopeBias * (abs(slope.x) + abs(slope.y)));"), "the bias is the view's constant and a share of the plane's own change over a texel");
  Check(Contains(helper, "float4 need = min(depth + slope.x * rx + slope.y * ry, 1.0);") && Contains(helper, "return float4(stored >= need);") && Count(helper, ">= need") == 2 && Contains(helper, "float4 isLit = float4(q >= need);"),
        "the compare is the game's `>=` against the plane's depth at the texel's centre, never above 1: the quad function's and the wide footprint's, no other");
  // The quads' component order and the offsets of each texel's centre from the point.
  Check(Contains(helper, "float4 ox = o0.x + float4(0.0, 1.0, 2.0, 3.0);") && Contains(helper, "float2 o0 = first + 0.5 - c;"), "the offsets of the footprint's centres from the point");
  Check(Contains(helper, "HeadlightQuadLit(q00, depth, slope, float4(ox.x, ox.y, ox.y, ox.x), float4(oy.y, oy.y, oy.x, oy.x))") &&
            Contains(helper, "HeadlightQuadLit(q10, depth, slope, float4(ox.z, ox.w, ox.w, ox.z), float4(oy.y, oy.y, oy.x, oy.x))") &&
            Contains(helper, "HeadlightQuadLit(q01, depth, slope, float4(ox.x, ox.y, ox.y, ox.x), float4(oy.w, oy.w, oy.z, oy.z))") &&
            Contains(helper, "HeadlightQuadLit(q11, depth, slope, float4(ox.z, ox.w, ox.w, ox.z), float4(oy.w, oy.w, oy.z, oy.z))"),
        "each quad's components x y z w are the texels (0, 1) (1, 1) (1, 0) (0, 0) of the quad: its columns and rows are offset accordingly");
  // The shortcuts: nothing nearer in all 16 texels is lit, all 16 nearer is in shadow (the fade still applies); the penumbra and the weights.
  Check(Contains(helper, "if (blockers < 0.5)") && Contains(helper, "if (blockers > 15.5)") && Contains(helper, "return lerp(1.0, 0.0, fade);"), "the shortcuts: no blocker in the footprint is lit, all sixteen is in shadow");
  Check(Contains(helper, "float radius = lampRadius * 0.5 * (lenU + lenV) * (s.z - blockerSum / blockers) / max(scaleA - s.z, 1.0e-6);") &&
            Contains(helper, "radius = clamp(radius, kHlPcfMinRadius, kHlPcfMaxRadius);"),
        "the kernel's half width is the lamp's disc through the average blocker, held to its range");
  // THE TWO SETTINGS and the wide footprint (HSQ-PCSS): the block's header y is the lamp's radius (0: the default), z the gathers a side (0: 2); 3 and 4 take HeadlightShadowWide, whose arithmetic is
  // light_shadow_filter.h ViewLit's for any footprint (the twin and the deferred pass's copy are held to each other bit for bit in shadow_atlas_test.cpp; this text is held to them by what follows).
  Check(Contains(helper, "float lampRadius = g_HlHeader.y > 0.0 ? g_HlHeader.y : kHlLampRadius;") && Contains(helper, "int gathers = clamp(int(g_HlHeader.z + 0.5), 2, 4);") &&
            sf::kMinGathers == 2 && sf::kMaxGathers == 4,
        "the lamp's radius and the gathers a side come from the block's header (0: the defaults), held to 2..4 as the twin's MakeParams does");
  Check(Contains(helper, "if (gathers > 2)") && Contains(helper, "return HeadlightShadowWide(s.xy / t, t, depth, slope, lenU, lenV, s.z, scaleA, gathers, lampRadius, fade);"),
        "more than two gathers a side take the wide footprint, with the plane's numbers");
  Check(Count(helper, "float HeadlightShadowWide(") == 1 && Contains(helper, "float2 first = floor(c - 0.5) - float(gathers - 1);") && Contains(helper, "float2 corner = (first + 1.0) * t;") &&
            Contains(helper, "map.GatherRed(mapSampler, corner + float2(2.0 * float(qa) * t.x, 2.0 * float(qb) * t.y))"),
        "the wide footprint starts G - 1 texels before the bilinear one and reads G x G gathers two texels apart");
  Check(Contains(helper, "float stored[64];") && Contains(helper, "float lit[64];") && Contains(helper, "float wx[8];") && Contains(helper, "float wy[8];") &&
            (2 * sf::kMaxGathers) * (2 * sf::kMaxGathers) == 64 && 2 * sf::kMaxGathers == 8,
        "the wide footprint's arrays hold the largest one the twin allows (8 x 8)");
  Check(Contains(helper, "float4 ox = o0.x + float(colBase) + float4(0.0, 1.0, 1.0, 0.0);") && Contains(helper, "float4 oy = o0.y + float(rowBase) + float4(1.0, 1.0, 0.0, 0.0);") &&
            Contains(helper, "stored[(rowBase + 1) * side + colBase] = q.x;") && Contains(helper, "stored[(rowBase + 1) * side + colBase + 1] = q.y;") &&
            Contains(helper, "stored[rowBase * side + colBase + 1] = q.z;") && Contains(helper, "stored[rowBase * side + colBase] = q.w;"),
        "each quad's components x y z w are the texels (0, 1) (1, 1) (1, 0) (0, 0) of the quad in the wide footprint too");
  Check(Contains(helper, "if (blockers > float(side * side) - 0.5)") &&
            Contains(helper, "float radius = lampRadius * 0.5 * (lenU + lenV) * (sz - blockerSum / blockers) / max(scaleA - sz, 1.0e-6);") &&
            Contains(helper, "radius = clamp(radius, kHlPcfMinRadius, float(gathers) - 0.5);"),
        "the wide kernel's half width is the same penumbra, held to [0.5, G - 0.5] (the footprint holds a box of that half width), all texels in shadow is in shadow");
  Check(Contains(helper, "wx[k] = saturate(min(edge.x + 1.0, c.x + radius) - max(edge.x, c.x - radius));") && Contains(helper, "wy[k] = saturate(min(edge.y + 1.0, c.y + radius) - max(edge.y, c.y - radius));") &&
            Contains(helper, "share = saturate(share / (4.0 * radius * radius));"),
        "the wide footprint counts each texel by the part of it the box covers");
  Check(!Contains(helper, " c0 ") && !Contains(helper, " r0 ") && !Contains(helper, "int c0") && !Contains(helper, "int r0"), "the wide path keeps off the game's register names");
  Check(Contains(helper, "float4 wx = saturate(min(edgeX + 1.0, c.x + radius) - max(edgeX, c.x - radius));") && Contains(helper, "float4 wy = saturate(min(edgeY + 1.0, c.y + radius) - max(edgeY, c.y - radius));") &&
            Contains(helper, "share = saturate(share / (4.0 * radius * radius));"),
        "each texel is counted by the part of it the box covers, the weights sum to 2 r on each axis");
  Check(Contains(helper, "float4 row0Lit = float4(l00.w, l00.z, l10.w, l10.z);") && Contains(helper, "float4 row1Lit = float4(l00.x, l00.y, l10.x, l10.y);") &&
            Contains(helper, "float4 row2Lit = float4(l01.w, l01.z, l11.w, l11.z);") && Contains(helper, "float4 row3Lit = float4(l01.x, l01.y, l11.x, l11.y);"),
        "the footprint's rows are put together from the quads in the order the twin does");
  Check(Contains(helper, "float fade = saturate(min(border.x, border.y) * (1.0 / kHlBorderFade));") && Contains(helper, "return lerp(1.0, share, fade);"), "the shadow fades out toward the tile's border");
  Check(!Contains(helper, "SV_Position") && !Contains(helper, "iPos") && !Contains(helper, "Noise(") && !Contains(helper, "frac(sin"), "nothing reads the screen position or a noise: no grain for anything to resolve");
  // The shadow hook's number of views and taps, the light count, the loop bound.
  Check(Contains(helper, "min(uint(g_HlHeader.x + 0.5), 4u)") && hr::kMaxLights == 4, "the helper takes at most four lights, as the rules do");
  Check(Contains(helper, "view * 6u + 5u") && sizeof(larecomp::gpu::LocalShadowView) == 96, "a shadow view is six registers (96 bytes) in the block");
  // A light is a car's and has up to two views, one for each lamp: its share is the mean of the views it has (HL-PER-LAMP).
  Check(Contains(helper, "uint4 shadowOfB = g_HlShadowOfB;") && Contains(helper, "uint viewA = shadowOf[i];") && Contains(helper, "uint viewB = shadowOfB[i];"),
        "a light's two views are read from the two tables");
  Check(Count(helper, "HeadlightShadow(view") == 2 && Contains(helper, "seen += HeadlightShadow(viewA, world, normal);") && Contains(helper, "seen += HeadlightShadow(viewB, world, normal);") &&
            Contains(helper, "scale *= seen / views;"),
        "each view of a light is looked up once and the share is the mean of the views it has");
  Check(Contains(helper, "if (scale > 0.0 && atlas.x != 0xFFFFFFFFu)"), "no shadow lookup for a term that is zero already, and none with no atlas");
  Check(Contains(helper, "dist2 >= range2 || dist2 < 1.0e-6") && Contains(helper, "if (cosine <= b.w)"), "the helper's early-outs are the rules' (reach, cone)");
  Check(Contains(helper, "ddx(world)") && Contains(helper, "ddy(world)"), "the derivatives are taken before the early-out");
  Check(helper.find("ddx(world)") < helper.find("if (count == 0u"), "... in that order");

  // The constants the declarations name sit where HeadlightConstants has them: 608 + offset in SPIR-V, c(38 + offset / 16) in DXIL.
  const sp::VariantTransformResult result = sp::TransformHeadlightHlsl(kShader, Fixture(kDefault));
  Check(result.ok, "the fixture transforms for the layout check");
  const std::string& out = result.hlsl;
  const struct {
    const char* spirv;
    const char* dxil;
    size_t offset;
  } kFields[] = {
      {"g_HlHeader vk::RawBufferLoad<float4>", "g_HlHeader : packoffset", offsetof(hr::HeadlightConstants, header)},
      {"g_HlCamera vk::RawBufferLoad<float4>", "g_HlCamera : packoffset", offsetof(hr::HeadlightConstants, camera)},
      {"g_HlBoundsLo vk::RawBufferLoad<float4>", "g_HlBoundsLo : packoffset", offsetof(hr::HeadlightConstants, bounds_lo)},
      {"g_HlBoundsHi vk::RawBufferLoad<float4>", "g_HlBoundsHi : packoffset", offsetof(hr::HeadlightConstants, bounds_hi)},
      {"g_HlLight(INDEX) vk::RawBufferLoad<float4>", "g_HlLights[12] : packoffset", offsetof(hr::HeadlightConstants, lights)},
      {"g_HlShadowOf vk::RawBufferLoad<uint4>", "g_HlShadowOf : packoffset", offsetof(hr::HeadlightConstants, shadow_of)},
      {"g_HlShadowOfB vk::RawBufferLoad<uint4>", "g_HlShadowOfB : packoffset", offsetof(hr::HeadlightConstants, shadow_of_b)},
      {"g_HlAtlas vk::RawBufferLoad<uint4>", "g_HlAtlas : packoffset", offsetof(hr::HeadlightConstants, atlas)},
      {"g_HlShadow(INDEX) vk::RawBufferLoad<float4>", "g_HlShadowArr[48] : packoffset", offsetof(hr::HeadlightConstants, shadow)},
  };
  for (const auto& f : kFields) {
    const std::string spirv = std::string("#define ") + f.spirv + "(g_PushConstants.SharedConstants + " + std::to_string(608 + f.offset);
    Check(Contains(out, spirv), std::string("SPIR-V: ") + f.spirv + " at " + std::to_string(608 + f.offset));
    const std::string dxil = std::string(f.dxil) + "(c" + std::to_string(38 + f.offset / 16) + ")";
    Check(Contains(out, dxil), std::string("DXIL: ") + f.dxil + " at c" + std::to_string(38 + f.offset / 16));
  }
  Check(608 + sizeof(hr::HeadlightConstants) == 1680, "the block ends at byte 1680 of the draw's constants");
  // The cbuffer's last register is c104: 38 + 1072 / 16 - 1.
  Check(38 + sizeof(hr::HeadlightConstants) / 16 - 1 == 104, "the last register is c104");
  Check(larecomp::gpu::kMaxShadowViews == 8 && sizeof(hr::HeadlightConstants::shadow) == 8 * sizeof(larecomp::gpu::LocalShadowView), "the block holds eight views (two for each of four lights)");
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
    if (!entry.is_regular_file() || entry.path().extension() != ".hlsl") continue;
    const std::string name = entry.path().filename().string();
    for (const std::string_view suffix : sp::kHeadlightSuffixes) {
      if (name.ends_with(std::string(suffix) + ".hlsl")) {
        files.push_back(entry.path());
        break;
      }
    }
  }
  std::sort(files.begin(), files.end());
  size_t edited = 0;
  std::map<std::string, size_t> skipped;
  std::map<std::string, std::vector<std::string>> skipped_names;
  std::set<std::string> texts;
  std::vector<std::pair<std::string, std::string>> made;
  std::set<std::string> edited_names;
  for (const auto& file : files) {
    const std::string stem = file.stem().string();
    const sp::VariantShader shader = {stem, 0};
    const sp::VariantTransformResult result = sp::TransformHeadlightHlsl(shader, ReadFile(file));
    if (result.ok) {
      ++edited;
      texts.insert(result.hlsl);
      made.emplace_back(stem, result.hlsl);
      edited_names.insert(stem);
    } else if (result.skip) {
      ++skipped[result.error];
      skipped_names[result.error].push_back(stem);
    } else {
      Check(false, "game file " + file.string() + ": " + result.error);
    }
  }
  std::printf("game files: %zu lit shaders, %zu edited in %zu texts", files.size(), edited, texts.size());
  for (const auto& [why, count] : skipped) std::printf("; %zu left out: %s", count, why.c_str());
  std::printf("\n");
  for (const auto& [why, names] : skipped_names) {
    std::printf("  left out (%s):", why.c_str());
    for (const std::string& n : names) std::printf(" %s", n.c_str());
    std::printf("\n");
  }
  Check(files.size() == 120, "the retail game has 120 lit shaders (found " + std::to_string(files.size()) + ")");
  Check(edited == 58, "58 shaders are edited (" + std::to_string(edited) + ")");
  Check(texts.size() == 56, "in 56 texts (" + std::to_string(texts.size()) + ")");
  Check(skipped[std::string(sp::kHeadlightSkipNoGrid)] == 18, "18 read no light grid");
  Check(skipped[std::string(sp::kHeadlightSkipPosition)] == 0, "none has a world position the edits cannot find");
  Check(skipped[std::string(sp::kHeadlightSkipAmbient)] == 44, "44 add their ambient in a shape the edits do not know (the interior shaders and a few more)");
  Check(edited + skipped[std::string(sp::kHeadlightSkipNoGrid)] + skipped[std::string(sp::kHeadlightSkipPosition)] +
                skipped[std::string(sp::kHeadlightSkipAmbient)] == files.size(),
        "every shader is edited or left out with one of the three reasons");
  // The shaders that matter for the picture: the road, the sidewalk, the buildings and the props, the tunnels and the traffic body are edited.
  for (const char* name : {"xCityRoad__PSMultiLight", "xCitySidewalk__PSMultiLight", "xCityDefault__PSMultiLight", "xCityBumpSpec__PSMultiLight",
                           "xCityTerrain__PSMultiLight", "xCityTunnelRoad__PSMultiLight", "xCityRiverRoad__PSMultiLight", "xPropDefault__PSMultiLight",
                           "xPropBumpSpec__PSMultiLight", "xCityGroundGrime__PSMultiLight", "xCityWindowLower__PSMultiLight", "xAmbientVehicle__PSMultiLight"}) {
    Check(edited_names.count(name) == 1, std::string(name) + " is edited");
  }
  // Every edited text carries the helper once and no two shaders share a text (their registers differ).
  for (const auto& [name, text] : made) {
    Check(Count(text, "float4 HeadlightLight(") == 1, name + ": one helper");
  }
  if (!results.empty()) {
    std::filesystem::create_directories(results);
    for (const auto& [name, text] : made) {
      std::ofstream(results / (name + ".hlsl"), std::ios::binary) << text;
    }
    std::printf("the %zu texts are in %s\n", made.size(), results.string().c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  CheckFixtures();
  CheckHelper();
  if (argc > 1) {
    CheckGameFiles(argv[1], argc > 2 ? std::filesystem::path(argv[2]) : std::filesystem::path());
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
