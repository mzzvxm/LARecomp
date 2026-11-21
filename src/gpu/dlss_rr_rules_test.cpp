// Check of the DLSS Ray Reconstruction rules (dlss_rr_rules.h): the Streamline ids, the formats, the tag plan, the camera matrices, the jitter length, the choice of
// denoiser, the frame gate and the plugin request. A program of its own: no renderer, no Windows, no GPU, no Streamline; exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_dlss_rr_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_dlss_rr_rules_test

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "dlss_rr_rules.h"

using namespace mcla::gpu::rr;

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

bool Near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

// A frame that has everything RR needs at 1920x1080 render and 3840x2160 output.
Inputs FullInputs() {
  Inputs in;
  in.render_size[0] = 1920;
  in.render_size[1] = 1080;
  in.output_size[0] = 3840;
  in.output_size[1] = 2160;
  const auto at_render = [&in](Format f) {
    Image i;
    i.present = true;
    i.width = in.render_size[0];
    i.height = in.render_size[1];
    i.format = f;
    return i;
  };
  in.color = at_render(Format::kRgba16Float);
  in.depth = at_render(Format::kR32Float);
  in.motion = at_render(Format::kRg16Float);
  in.diffuse_albedo = at_render(Format::kRgb10A2Unorm);
  in.specular_albedo = at_render(Format::kRgba8Unorm);
  in.normal_roughness = at_render(Format::kRgba16Float);
  in.output.present = true;
  in.output.width = in.output_size[0];
  in.output.height = in.output_size[1];
  in.output.format = Format::kRgba16Float;
  return in;
}

void CheckIds() {
  // sl_core_types.h, Streamline 2.14.16.
  Check(kSlFeatureDlssRr == 1001, "kFeatureDLSS_RR is 1001");
  Check(kSlBufferDepth == 0 && kSlBufferMotionVectors == 1 && kSlBufferScalingInputColor == 3 && kSlBufferScalingOutputColor == 4, "the SR buffer ids");
  Check(kSlBufferNormals == 5 && kSlBufferRoughness == 6 && kSlBufferAlbedo == 7 && kSlBufferSpecularAlbedo == 8, "the guide buffer ids");
  Check(kSlBufferSpecularMotionVectors == 10 && kSlBufferNormalRoughness == 14, "specular motion 10, normal+roughness 14");
  Check(kSlBufferColorBeforeTransparency == 40 && kSlBufferSpecularHitDistance == 42 && kSlBufferResponsivityMask == 68, "colour before transparency 40, hit distance 42, responsivity 68");
  for (uint32_t b = 0; b < uint32_t(Buffer::kCount); ++b) {
    const BufferInfo info = InfoOf(Buffer(b));
    Check(info.name && info.name[0] != '?', "buffer " + std::to_string(b) + " has a name");
    Check(info.at_output_size == (Buffer(b) == Buffer::kColorOut), "only the colour out is at the output size");
    for (uint32_t c = b + 1; c < uint32_t(Buffer::kCount); ++c) {
      Check(info.sl_type != InfoOf(Buffer(c)).sl_type, "buffer types are distinct");
    }
  }
  Check(kSkyHitDistance == 65504.0f && kSkyDiffuseAlbedo == 0.5f && kSkySpecularAlbedo == 0.0f && kSkyRoughness == 0.0f, "the sky's defaults (integration guide 3.4)");
}

void CheckFormats() {
  Check(FormatOk(Buffer::kColorIn, Format::kRgba16Float), "HDR colour in");
  Check(!FormatOk(Buffer::kColorIn, Format::kRgba8Unorm), "an 8 bit colour is not HDR");
  Check(FormatOk(Buffer::kDiffuseAlbedo, Format::kRgba8Unorm) && FormatOk(Buffer::kDiffuseAlbedo, Format::kRgb10A2Unorm), "linear albedo formats");
  Check(!FormatOk(Buffer::kDiffuseAlbedo, Format::kRgba8Srgb) && !FormatOk(Buffer::kSpecularAlbedo, Format::kRgba8Srgb), "sRGB albedos are refused");
  Check(!FormatOk(Buffer::kDiffuseAlbedo, Format::kR8Unorm), "an albedo needs 3 channels");
  Check(FormatOk(Buffer::kNormalRoughness, Format::kRgba16Float) && !FormatOk(Buffer::kNormalRoughness, Format::kRg16Float), "packed normals need 4 channels");
  Check(FormatOk(Buffer::kRoughness, Format::kR8Unorm) && !FormatOk(Buffer::kRoughness, Format::kRgba16Float), "a roughness is one channel");
  Check(FormatOk(Buffer::kMotion, Format::kRg16Float) && FormatOk(Buffer::kSpecularMotion, Format::kRg32Float) && !FormatOk(Buffer::kMotion, Format::kR16Float), "vectors are two floats");
  Check(FormatOk(Buffer::kDepth, Format::kR32Float) && !FormatOk(Buffer::kDepth, Format::kRg16Float), "depth is one float");
  Check(FormatOk(Buffer::kSpecularHitDistance, Format::kR16Float) && !FormatOk(Buffer::kSpecularHitDistance, Format::kRg16Float), "the hit distance is a scalar");
  Check(FormatOk(Buffer::kResponsivity, Format::kR8Snorm) && !FormatOk(Buffer::kResponsivity, Format::kR8Unorm), "the responsivity mask is signed");
  Check(FormatOk(Buffer::kColorOut, Format::kRgba16Float) && !FormatOk(Buffer::kColorOut, Format::kR16Float), "the output has 3 or 4 channels");
  Check(!FormatOk(Buffer::kColorIn, Format::kUnknown), "an unknown format is refused");
  Check(Channels(Format::kRgb10A2Unorm) == 4 && Channels(Format::kRg16Float) == 2 && Channels(Format::kD32Float) == 1 && Channels(Format::kUnknown) == 0, "channel counts");
  Check(IsFloat(Format::kRgba16Float) && !IsFloat(Format::kRgba8Unorm) && !IsFloat(Format::kRgb10A2Unorm), "float formats");
}

void CheckPlanHappyPaths() {
  // Packed normals + the primary motion standing in for the reflection's.
  Inputs in = FullInputs();
  TagPlan plan = PlanTags(in);
  Check(plan.verdict == Verdict::kOk && plan.culprit == Buffer::kCount, "the full packed frame is ok");
  Check(plan.normal_roughness == NormalRoughnessMode::kPacked, "packed normals say packed");
  Check(plan.specular == SpecularSource::kPrimaryMotion && !plan.needs_view_matrices, "no specular input: the primary motion stands in, no matrices needed");
  Check(plan.count == 8, "eight tags: colour, output, depth, motion, two albedos, normals+roughness, specular motion");
  const Buffer order[] = {Buffer::kColorIn, Buffer::kColorOut, Buffer::kDepth, Buffer::kMotion, Buffer::kDiffuseAlbedo, Buffer::kSpecularAlbedo, Buffer::kNormalRoughness, Buffer::kSpecularMotion};
  for (uint32_t i = 0; i < 8 && i < plan.count; ++i) {
    Check(plan.tags[i].buffer == order[i], "tag " + std::to_string(i) + " is in Streamline's order");
  }
  Check(plan.tags[1].output_extent && !plan.tags[0].output_extent && !plan.tags[7].output_extent, "only the output's tag has the output extent");
  Check(plan.tags[7].buffer == Buffer::kSpecularMotion && plan.tags[7].source == Buffer::kMotion, "the aliased tag is the specular type over the motion image");
  Check(InfoOf(plan.tags[7].buffer).sl_type == kSlBufferSpecularMotionVectors, "the aliased tag's Streamline type is the specular motion's");

  // Separate normals and roughness.
  in = FullInputs();
  in.normals = in.normal_roughness;
  in.normals.format = Format::kRgba16Float;
  in.roughness = in.normal_roughness;
  in.roughness.format = Format::kR8Unorm;
  in.normal_roughness = Image{};
  plan = PlanTags(in);
  Check(plan.verdict == Verdict::kOk && plan.normal_roughness == NormalRoughnessMode::kUnpacked, "separate normals and roughness are unpacked");
  Check(plan.count == 9 && plan.tags[6].buffer == Buffer::kNormals && plan.tags[7].buffer == Buffer::kRoughness, "normals then roughness, nine tags");

  // The hit distance with the matrices.
  in = FullInputs();
  in.specular_hit_distance = in.depth;
  in.specular_hit_distance.format = Format::kR16Float;
  in.have_view_matrices = true;
  plan = PlanTags(in);
  Check(plan.verdict == Verdict::kOk && plan.specular == SpecularSource::kHitDistance && plan.needs_view_matrices, "a hit distance with the matrices");
  Check(plan.tags[7].buffer == Buffer::kSpecularHitDistance && plan.tags[7].source == Buffer::kSpecularHitDistance, "the hit distance is tagged as itself");

  // Both specular inputs: the motion wins and the hit distance is not tagged.
  in.specular_motion = in.motion;
  plan = PlanTags(in);
  Check(plan.verdict == Verdict::kOk && plan.specular == SpecularSource::kSpecularMotion && plan.count == 8, "specular motion wins over a hit distance");
  Check(plan.tags[7].buffer == Buffer::kSpecularMotion && plan.tags[7].source == Buffer::kSpecularMotion && !plan.needs_view_matrices, "its own image, no matrices");

  // Optional guides come last, in order.
  in = FullInputs();
  in.responsivity = in.depth;
  in.responsivity.format = Format::kR8Snorm;
  in.color_before_transparency = in.color;
  plan = PlanTags(in);
  Check(plan.verdict == Verdict::kOk && plan.count == 10, "ten tags with both optional guides");
  Check(plan.tags[8].buffer == Buffer::kResponsivity && plan.tags[9].buffer == Buffer::kColorBeforeTransparency, "the optional guides follow the specular input");
  Check(plan.count <= kMaxTags, "the most tags fit");
}

void CheckPlanRefusals() {
  // Every required image, left out in turn.
  struct Row {
    const char* name;
    Image Inputs::*member;
    Buffer culprit;
  };
  const Row rows[] = {{"colour", &Inputs::color, Buffer::kColorIn},       {"output", &Inputs::output, Buffer::kColorOut},
                      {"depth", &Inputs::depth, Buffer::kDepth},          {"motion", &Inputs::motion, Buffer::kMotion},
                      {"diffuse albedo", &Inputs::diffuse_albedo, Buffer::kDiffuseAlbedo}, {"specular albedo", &Inputs::specular_albedo, Buffer::kSpecularAlbedo}};
  for (const Row& r : rows) {
    Inputs in = FullInputs();
    in.*(r.member) = Image{};
    const TagPlan plan = PlanTags(in);
    Check(plan.verdict == Verdict::kMissing && plan.culprit == r.culprit && plan.count == 0, std::string("missing ") + r.name);
  }
  Inputs in = FullInputs();
  in.normal_roughness = Image{};
  TagPlan plan = PlanTags(in);
  Check(plan.verdict == Verdict::kMissing && plan.culprit == Buffer::kNormals, "no normals at all");
  in.normals = in.color;
  in.normals.format = Format::kRgba16Float;
  plan = PlanTags(in);
  Check(plan.verdict == Verdict::kMissing && plan.culprit == Buffer::kRoughness, "normals without a roughness");

  in = FullInputs();
  in.allow_primary_motion_as_specular = false;
  plan = PlanTags(in);
  Check(plan.verdict == Verdict::kMissing && plan.culprit == Buffer::kSpecularMotion, "no specular input and no stand-in");

  in = FullInputs();
  in.specular_hit_distance = in.depth;
  in.specular_hit_distance.format = Format::kR16Float;
  in.have_view_matrices = false;
  plan = PlanTags(in);
  Check(plan.verdict == Verdict::kNeedsViewMatrices && plan.culprit == Buffer::kSpecularHitDistance && plan.needs_view_matrices, "a hit distance without the matrices");

  // Sizes.
  in = FullInputs();
  in.diffuse_albedo.width = 1280;
  plan = PlanTags(in);
  Check(plan.verdict == Verdict::kBadSize && plan.culprit == Buffer::kDiffuseAlbedo, "an albedo at another size");
  in = FullInputs();
  in.output.height = 1080;
  plan = PlanTags(in);
  Check(plan.verdict == Verdict::kBadSize && plan.culprit == Buffer::kColorOut, "an output that is not the output size");
  in = FullInputs();
  in.output_size[0] = 1280;
  in.output_size[1] = 720;
  in.output.width = 1280;
  in.output.height = 720;
  plan = PlanTags(in);
  Check(plan.verdict == Verdict::kBadSize && plan.culprit == Buffer::kColorOut, "an output smaller than the render size");
  in = FullInputs();
  in.render_size[0] = 0;
  plan = PlanTags(in);
  Check(plan.verdict == Verdict::kBadSize, "a zero render size");
  // The aliased motion is checked at the render size like the motion.
  in = FullInputs();
  in.motion.width = 960;
  plan = PlanTags(in);
  Check(plan.verdict == Verdict::kBadSize && plan.culprit == Buffer::kMotion, "the motion at another size is refused before its alias");

  // Formats.
  in = FullInputs();
  in.diffuse_albedo.format = Format::kRgba8Srgb;
  plan = PlanTags(in);
  Check(plan.verdict == Verdict::kBadFormat && plan.culprit == Buffer::kDiffuseAlbedo, "an sRGB albedo");
  in = FullInputs();
  in.color.format = Format::kRgba8Unorm;
  plan = PlanTags(in);
  Check(plan.verdict == Verdict::kBadFormat && plan.culprit == Buffer::kColorIn, "an LDR colour");
  in = FullInputs();
  in.motion.format = Format::kR16Float;
  plan = PlanTags(in);
  Check(plan.verdict == Verdict::kBadFormat, "a one channel motion image (and so its alias)");
  in = FullInputs();
  in.depth.format = Format::kUnknown;
  plan = PlanTags(in);
  Check(plan.verdict == Verdict::kBadFormat && plan.culprit == Buffer::kDepth, "an unknown depth format");
  Check(std::string(VerdictText(Verdict::kOk)) == "ok" && std::string(VerdictText(Verdict::kBadSize)) != "?", "verdict texts");
}

// out = a * b, row vectors.
Matrix4 Mul(const Matrix4& a, const Matrix4& b) {
  Matrix4 out;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += a.m[r * 4 + k] * b.m[k * 4 + c];
      out.m[r * 4 + c] = s;
    }
  }
  return out;
}

void CheckMatrices() {
  // A camera turned a quarter turn about the vertical and moved: right = -z, forward = +x (an orthonormal frame).
  Camera cam;
  cam.position[0] = 10;
  cam.position[1] = 2;
  cam.position[2] = -5;
  cam.right[0] = 0; cam.right[1] = 0; cam.right[2] = -1;
  cam.up[0] = 0; cam.up[1] = 1; cam.up[2] = 0;
  cam.forward[0] = 1; cam.forward[1] = 0; cam.forward[2] = 0;
  const Matrix4 v2w = CameraViewToWorld(cam);
  const Matrix4 w2v = WorldToCameraView(cam);
  const Matrix4 id = Mul(v2w, w2v);
  bool identity = true;
  for (int i = 0; i < 16; ++i) identity = identity && Near(id.m[i], (i % 5 == 0) ? 1.0f : 0.0f);
  Check(identity, "view-to-world times world-to-view is the identity");
  const Matrix4 id2 = Mul(w2v, v2w);
  identity = true;
  for (int i = 0; i < 16; ++i) identity = identity && Near(id2.m[i], (i % 5 == 0) ? 1.0f : 0.0f);
  Check(identity, "and the other way round");
  // The camera's own position is the view-space origin; a point 3 metres ahead is (0, 0, 3) in view space.
  const float ahead[4] = {13, 2, -5, 1};
  float view[3] = {};
  for (int c = 0; c < 3; ++c) {
    view[c] = ahead[0] * w2v.m[c] + ahead[1] * w2v.m[4 + c] + ahead[2] * w2v.m[8 + c] + ahead[3] * w2v.m[12 + c];
  }
  Check(Near(view[0], 0) && Near(view[1], 0) && Near(view[2], 3), "a point 3 m ahead is 3 m along view z");
  // Row 3 of view-to-world is the position, w column is (0, 0, 0, 1).
  Check(v2w.m[12] == 10 && v2w.m[13] == 2 && v2w.m[14] == -5 && v2w.m[15] == 1 && v2w.m[3] == 0 && v2w.m[7] == 0 && v2w.m[11] == 0, "the position is the last row");
  // The default camera gives the identity.
  const Matrix4 idm = CameraViewToWorld(Camera{});
  const Matrix4 idw = WorldToCameraView(Camera{});
  identity = true;
  for (int i = 0; i < 16; ++i) identity = identity && Near(idm.m[i], (i % 5 == 0) ? 1.0f : 0.0f) && Near(idw.m[i], (i % 5 == 0) ? 1.0f : 0.0f);
  Check(identity, "the default camera is the identity both ways");

  // MakeOptions carries the plan's roughness mode, the output size and both matrices.
  Inputs in = FullInputs();
  const TagPlan plan = PlanTags(in);
  const Options o = MakeOptions(Mode::kPerformance, 3840, 2160, plan, cam);
  Check(o.mode == Mode::kPerformance && o.output_size[0] == 3840 && o.output_size[1] == 2160, "options: mode and output size");
  Check(o.normal_roughness == NormalRoughnessMode::kPacked && o.colour_hdr && !o.alpha_upscaling && o.default_presets, "options: packed roughness, HDR, no alpha upscale, default presets");
  Check(Near(o.view_to_world.m[12], 10) && Near(o.world_to_view.m[12], -(10 * 0 + 2 * 0 + -5 * -1)), "options: the camera matrices");
}

void CheckJitter() {
  Check(JitterPhases(1.0) == 32, "DLAA: 32 positions (SR's 8 is not enough)");
  Check(JitterPhases(1.5) == 32, "1.5x: 32 (SR's 18)");
  Check(JitterPhases(2.0) == 32, "2x: 32 (SR's 32)");
  Check(JitterPhases(3.0) == 72, "3x: 72 (SR's, which is longer)");
  Check(JitterPhases(0.5) == 32, "a ratio under 1 is 1");
  Check(RatioOf(Mode::kDlaa) == 1.0 && RatioOf(Mode::kPerformance) == 2.0 && RatioOf(Mode::kUltraPerformance) == 3.0, "mode ratios");
  Check(Near(float(RatioOf(Mode::kQuality)), 1.5151f, 1e-3f) && Near(float(RatioOf(Mode::kBalanced)), 1.7241f, 1e-3f), "Quality 0.66 and Balanced 0.58");
  Check(Near(LinearRoughnessOfSmoothness(0.0f), 1.0f) && Near(LinearRoughnessOfSmoothness(1.0f), 0.0f) && Near(LinearRoughnessOfSmoothness(0.63f), 0.37f), "linear roughness is 1 - smoothness");
  Check(LinearRoughnessOfSmoothness(-1.0f) == 1.0f && LinearRoughnessOfSmoothness(2.0f) == 0.0f, "clamped");
}

PlanInputs ReadyInputs() {
  PlanInputs in;
  in.setting = Denoiser::kAuto;
  in.debug = DebugSwitch::kOn;
  in.facts.build_has_rr = true;
  in.facts.files_ok = true;
  in.facts.streamline_running = true;
  in.facts.supported = true;
  in.upscaler_is_dlss = true;
  in.consumers_active = true;
  in.guides_ready = true;
  return in;
}

void CheckSelection() {
  Plan p = SelectDenoiser(ReadyInputs());
  Check(p.use_rr && p.why == Why::kRrOn, "everything there: RR");
  PlanInputs in = ReadyInputs();
  in.debug = DebugSwitch::kOff;
  p = SelectDenoiser(in);
  Check(!p.use_rr && p.why == Why::kSwitchOff, "the debug switch off: ours");
  in = ReadyInputs();
  in.debug = DebugSwitch::kOff;
  in.setting = Denoiser::kOurs;
  p = SelectDenoiser(in);
  Check(!p.use_rr && p.why == Why::kSwitchOff, "the switch is the first reason");
  in = ReadyInputs();
  in.setting = Denoiser::kOurs;
  p = SelectDenoiser(in);
  Check(!p.use_rr && p.why == Why::kSettingOurs, "OURS: ours");
  in = ReadyInputs();
  in.facts.build_has_rr = false;
  p = SelectDenoiser(in);
  Check(!p.use_rr && p.why == Why::kBuildHasNoRr, "a build without RR");
  in = ReadyInputs();
  in.facts.files_ok = false;
  p = SelectDenoiser(in);
  Check(!p.use_rr && p.why == Why::kFilesMissing, "files missing");
  in = ReadyInputs();
  in.facts.streamline_running = false;
  p = SelectDenoiser(in);
  Check(!p.use_rr && p.why == Why::kStreamlineNotRunning, "Streamline still starting");
  in = ReadyInputs();
  in.facts.supported = false;
  p = SelectDenoiser(in);
  Check(!p.use_rr && p.why == Why::kNotSupported, "not supported");
  in = ReadyInputs();
  in.upscaler_is_dlss = false;
  p = SelectDenoiser(in);
  Check(!p.use_rr && p.why == Why::kUpscalerNotDlss, "FSR or no upscaler: ours");
  in = ReadyInputs();
  in.debug = DebugSwitch::kProbe;
  p = SelectDenoiser(in);
  Check(!p.use_rr && p.why == Why::kProbeOnly, "probe: loaded, never used");
  in = ReadyInputs();
  in.debug = DebugSwitch::kProbe;
  in.facts.supported = false;
  p = SelectDenoiser(in);
  Check(!p.use_rr && p.why == Why::kNotSupported, "probe says what is missing before it says it is a probe");
  in = ReadyInputs();
  in.consumers_active = false;
  p = SelectDenoiser(in);
  Check(!p.use_rr && p.why == Why::kNoConsumer, "AUTO with no consumer: ours");
  in = ReadyInputs();
  in.consumers_active = false;
  in.setting = Denoiser::kRr;
  p = SelectDenoiser(in);
  Check(p.use_rr && p.why == Why::kRrOn, "RR asked for by the player: RR without a consumer");
  in = ReadyInputs();
  in.guides_ready = false;
  p = SelectDenoiser(in);
  Check(!p.use_rr && p.why == Why::kGuidesNotReady, "no guides yet: ours");
  in = ReadyInputs();
  in.guides_ready = false;
  in.setting = Denoiser::kRr;
  p = SelectDenoiser(in);
  Check(!p.use_rr && p.why == Why::kGuidesNotReady, "RR asked for, no guides: ours");
  // Every reason has a text.
  for (int w = 0; w <= int(Why::kGuidesNotReady); ++w) {
    Check(std::string(WhyText(Why(w))) != "?", "why " + std::to_string(w) + " has a text");
  }
  // The exhaustive table: RR only for the one combination.
  // The whole table: 3 settings x 3 switches x 7 yes/no inputs. RR is chosen for exactly the combinations the rules name.
  int rr = 0;
  int rows = 0;
  for (int setting = 0; setting < 3; ++setting) {
    for (int debug = 0; debug < 3; ++debug) {
      for (int bits = 0; bits < (1 << 7); ++bits) {
        PlanInputs t;
        t.setting = Denoiser(setting);
        t.debug = DebugSwitch(debug);
        t.facts.build_has_rr = (bits >> 0) & 1;
        t.facts.files_ok = (bits >> 1) & 1;
        t.facts.streamline_running = (bits >> 2) & 1;
        t.facts.supported = (bits >> 3) & 1;
        t.upscaler_is_dlss = (bits >> 4) & 1;
        t.consumers_active = (bits >> 5) & 1;
        t.guides_ready = (bits >> 6) & 1;
        const Plan q = SelectDenoiser(t);
        const bool expect = t.debug == DebugSwitch::kOn && t.setting != Denoiser::kOurs && t.facts.build_has_rr && t.facts.files_ok &&
                            t.facts.streamline_running && t.facts.supported && t.upscaler_is_dlss &&
                            (t.setting == Denoiser::kRr || t.consumers_active) && t.guides_ready;
        Check(q.use_rr == expect, "the table: setting " + std::to_string(setting) + " switch " + std::to_string(debug) + " bits " + std::to_string(bits));
        Check(q.use_rr == (q.why == Why::kRrOn), "the table: use_rr and the reason agree");
        ++rows;
        if (q.use_rr) ++rr;
      }
    }
  }
  // AUTO needs every input including a consumer (1 case); RR does not need the consumer (2 cases); OURS never (0): 3 of 1152.
  Check(rows == 1152 && rr == 3, "the table size and the number of RR cases");
}

void CheckGate() {
  FrameFacts f;
  f.gbuffer_published = true;
  f.planes_valid = 7;
  f.gbuffer_size[0] = f.render_size[0] = 1920;
  f.gbuffer_size[1] = f.render_size[1] = 1080;
  Check(FrameGate(f) == Gate::kOpen, "a play frame with all three planes is open");
  FrameFacts g = f;
  g.gbuffer_published = false;
  Check(FrameGate(g) == Gate::kNoGBuffer, "no G-buffer");
  g = f;
  g.planes_valid = 1;
  Check(FrameGate(g) == Gate::kPlanesMissing, "the normal alone is not enough");
  g = f;
  g.planes_valid = 3;
  Check(FrameGate(g) == Gate::kPlanesMissing, "no material plane");
  g = f;
  g.gbuffer_size[0] = 1280;
  Check(FrameGate(g) == Gate::kSizeMismatch, "another size");
  g = f;
  g.cutscene = true;
  Check(FrameGate(g) == Gate::kCutscene, "a cutscene");
  g = f;
  g.pause_view = true;
  Check(FrameGate(g) == Gate::kPauseView, "the pause view");
  g = f;
  g.pause_view = true;
  g.gbuffer_published = false;
  Check(FrameGate(g) == Gate::kPauseView, "the pause view is named before the missing G-buffer");
  g = f;
  g.planes_valid = 0xF;
  Check(FrameGate(g) == Gate::kOpen, "a plane bit above the three does no harm");
  for (int i = 0; i <= int(Gate::kPauseView); ++i) Check(std::string(GateText(Gate(i))) != "?", "gate text " + std::to_string(i));
}

void CheckPluginAndParsers() {
  PluginInputs p;
  p.debug = DebugSwitch::kProbe;
  p.build_has_rr = true;
  p.files_ok = true;
  p.looks_rtx = true;
  p.d3d12 = true;
  Check(WantRrPlugin(p), "D3D12, an RTX card, files there, probe: the plugin is asked for");
  p.debug = DebugSwitch::kOn;
  Check(WantRrPlugin(p), "on as well");
  p.debug = DebugSwitch::kOff;
  Check(!WantRrPlugin(p), "off: never");
  p.debug = DebugSwitch::kOn;
  p.looks_rtx = false;
  Check(!WantRrPlugin(p), "not an RTX name");
  p.looks_rtx = true;
  p.files_ok = false;
  Check(!WantRrPlugin(p), "files missing");
  p.files_ok = true;
  p.build_has_rr = false;
  Check(!WantRrPlugin(p), "a build without RR");
  p.build_has_rr = true;
  p.d3d12 = false;
  p.dlss_saved_at_start = false;
  Check(!WantRrPlugin(p), "Vulkan with DLSS not saved: Streamline is not even started");
  p.dlss_saved_at_start = true;
  Check(WantRrPlugin(p), "Vulkan with DLSS saved at the start");
  Check(kFiles.size() == 2 && std::string(kFiles[0]) == "sl.dlss_d.dll" && std::string(kFiles[1]) == "nvngx_dlssd.dll", "the files");
  Check(kSignedFiles.size() == 1 && std::string(kSignedFiles[0]) == "sl.dlss_d.dll", "the signed plugin");

  Denoiser d = Denoiser::kOurs;
  Check(ParseDenoiser("auto", &d) && d == Denoiser::kAuto && ParseDenoiser("rr", &d) && d == Denoiser::kRr && ParseDenoiser("ours", &d) && d == Denoiser::kOurs, "denoiser words");
  Check(!ParseDenoiser("AUTO", &d) && !ParseDenoiser("", &d) && !ParseDenoiser("nrd", &d), "other words are refused");
  DebugSwitch s = DebugSwitch::kOn;
  Check(ParseDebugSwitch("off", &s) && s == DebugSwitch::kOff && ParseDebugSwitch("probe", &s) && s == DebugSwitch::kProbe && ParseDebugSwitch("on", &s) && s == DebugSwitch::kOn, "switch words");
  Check(!ParseDebugSwitch("true", &s) && !ParseDebugSwitch("1", &s), "other switch words are refused");
  Check(std::string(DenoiserName(Denoiser::kRr)) == "rr" && std::string(DebugSwitchName(DebugSwitch::kProbe)) == "probe", "names round trip");
}

}  // namespace

int main() {
  CheckIds();
  CheckFormats();
  CheckPlanHappyPaths();
  CheckPlanRefusals();
  CheckMatrices();
  CheckJitter();
  CheckSelection();
  CheckGate();
  CheckPluginAndParsers();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
