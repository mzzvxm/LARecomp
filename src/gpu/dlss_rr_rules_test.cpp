// Check of the DLSS Ray Reconstruction rules (dlss_rr_rules.h): the Streamline ids, the formats, the tag plan, the camera matrices, the jitter length, the choice of
// denoiser, the frame gate and the plugin request. A program of its own: no renderer, no Windows, no GPU, no Streamline; exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_dlss_rr_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_dlss_rr_rules_test

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "dlss_rr_rules.h"
#include "upscaler_math.h"

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
  // RR-3: what the temporal system hands the sequence's length (RuntimePhaseCount into upscaler_math.h JitterPhases): at least 32 for every mode while RR may run, whatever the runtime's own
  // count is (DLSS has none: 0; FFX's is 8 x ratio^2), Super Resolution's own count untouched otherwise, and never over the cap.
  for (uint32_t m = 0; m < uint32_t(Mode::kCount); ++m) {
    const double ratio = RatioOf(Mode(m));
    for (const uint32_t runtime : {0u, 8u, 18u, 32u, 72u}) {
      const uint32_t with_rr = mcla::gpu::upscaler_math::JitterPhases(ratio, RuntimePhaseCount(runtime, ratio, true));
      const uint32_t without = mcla::gpu::upscaler_math::JitterPhases(ratio, RuntimePhaseCount(runtime, ratio, false));
      Check(with_rr >= 32u && with_rr <= mcla::gpu::upscaler_math::kMaxPhases, "mode " + std::to_string(m) + " runtime " + std::to_string(runtime) + ": RR's sequence is 32 or longer, under the cap");
      Check(with_rr >= without, "RR's sequence is never shorter than Super Resolution's");
      Check(without == mcla::gpu::upscaler_math::JitterPhases(ratio, runtime), "RR not running: Super Resolution's sequence as it was");
      Check(with_rr >= std::min<uint32_t>(runtime, mcla::gpu::upscaler_math::kMaxPhases), "the runtime's own longer count is kept");
    }
  }
}

// The shipped state: the debug switch on auto (the row decides), the row on AUTO, an effect that RR cleans better than ours is on (so the consumer rule is met), the plugin was asked for, the PC supports it,
// DLSS is the upscaler and the frame's guides are there.
PlanInputs ReadyInputs() {
  PlanInputs in;
  in.setting = Denoiser::kAuto;
  in.debug = DebugSwitch::kRow;
  in.facts.build_has_rr = true;
  in.facts.files_ok = true;
  in.facts.streamline_running = true;
  in.facts.asked = true;
  in.facts.supported = true;
  in.upscaler_is_dlss = true;
  in.consumers_active = true;
  in.consumers_any = true;
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
  in.debug = DebugSwitch::kProbe;
  in.consumers_active = false;
  in.consumers_any = false;
  p = SelectDenoiser(in);
  Check(!p.use_rr && p.why == Why::kProbeOnly, "probe needs no consumer: it only asks and logs");
  // RR-5: the consumers. AUTO needs an effect AUTO trusts RR with; the row's RR value any effect with a raw mode; the debug switch's "on" none.
  in = ReadyInputs();
  in.consumers_active = false;
  p = SelectDenoiser(in);
  Check(!p.use_rr && p.why == Why::kNoConsumer, "AUTO with an effect that has a raw mode but that RR is not known to clean better: ours");
  in = ReadyInputs();
  in.consumers_active = false;
  in.consumers_any = false;
  p = SelectDenoiser(in);
  Check(!p.use_rr && p.why == Why::kNoConsumer, "AUTO with no consumer: ours");
  in = ReadyInputs();
  in.consumers_active = false;
  in.setting = Denoiser::kRr;
  p = SelectDenoiser(in);
  Check(p.use_rr && p.why == Why::kRrOn, "the row's RR value takes any effect with a raw mode, AUTO's list or not");
  in = ReadyInputs();
  in.consumers_active = false;
  in.consumers_any = false;
  in.setting = Denoiser::kRr;
  p = SelectDenoiser(in);
  Check(!p.use_rr && p.why == Why::kNoConsumer, "the row's RR value with nothing to denoise: ours (NVIDIA: disable it where there is no ray traced content; the row is grey)");
  in = ReadyInputs();
  in.consumers_active = false;
  in.consumers_any = false;
  in.debug = DebugSwitch::kOn;
  p = SelectDenoiser(in);
  Check(p.use_rr && p.why == Why::kRrOn, "the debug switch's on lifts the consumer rule: RR over a frame with nothing to denoise (the A/B and cost runs)");
  in = ReadyInputs();
  in.consumers_active = false;
  in.consumers_any = false;
  in.debug = DebugSwitch::kOn;
  in.setting = Denoiser::kOurs;
  p = SelectDenoiser(in);
  Check(p.use_rr && p.why == Why::kRrOn, "on stands for the row's RR value whatever the row says");
  in = ReadyInputs();
  in.guides_ready = false;
  p = SelectDenoiser(in);
  Check(!p.use_rr && p.why == Why::kGuidesNotReady, "no guides yet: ours");
  in = ReadyInputs();
  in.guides_ready = false;
  in.setting = Denoiser::kRr;
  p = SelectDenoiser(in);
  Check(!p.use_rr && p.why == Why::kGuidesNotReady, "RR asked for, no guides: ours");
  // RR-5: the plugin must have been asked for at Streamline's start. A start with the row on OURS leaves it out: where a start with another row would have loaded it, the answer is a restart.
  in = ReadyInputs();
  in.facts.asked = false;
  in.facts.loadable = true;
  p = SelectDenoiser(in);
  Check(!p.use_rr && p.why == Why::kNotLoaded, "not asked for at the start, the PC could have: the next start");
  in = ReadyInputs();
  in.facts.asked = false;
  p = SelectDenoiser(in);
  Check(!p.use_rr && p.why == Why::kNotSupported, "not asked for and the PC could not have: not supported");
  in = ReadyInputs();
  in.facts.asked = false;
  in.facts.loadable = true;
  in.facts.files_ok = false;
  p = SelectDenoiser(in);
  Check(p.why == Why::kFilesMissing, "files that were looked at and are not right are named before the missing plugin");
  in = ReadyInputs();
  in.facts.asked = false;
  in.facts.loadable = true;
  in.facts.streamline_running = false;
  p = SelectDenoiser(in);
  Check(p.why == Why::kStreamlineNotRunning, "a start still going on is named before the missing plugin (the plugin's state is not known yet)");
  in = ReadyInputs();
  in.facts.asked = false;
  in.facts.loadable = true;
  in.setting = Denoiser::kOurs;
  p = SelectDenoiser(in);
  Check(p.why == Why::kSettingOurs, "OURS is named before the missing plugin");
  // RR-3: a session that dropped RR stays on Super Resolution whatever else is true; no room in video memory says so before it says the guides are missing.
  in = ReadyInputs();
  in.facts.failed = true;
  p = SelectDenoiser(in);
  Check(!p.use_rr && p.why == Why::kRrFailed, "RR failed in this session: ours (Super Resolution goes on)");
  in = ReadyInputs();
  in.facts.failed = true;
  in.facts.supported = false;
  p = SelectDenoiser(in);
  Check(p.why == Why::kNotSupported, "not supported is named before a failure");
  in = ReadyInputs();
  in.room_ok = false;
  p = SelectDenoiser(in);
  Check(!p.use_rr && p.why == Why::kNoRoom, "no room in video memory: ours");
  in = ReadyInputs();
  in.room_ok = false;
  in.guides_ready = false;
  p = SelectDenoiser(in);
  Check(p.why == Why::kNoRoom, "no room is named before the missing guides (the guide pass is not wanted either)");
  in = ReadyInputs();
  in.room_ok = false;
  in.consumers_active = false;
  in.consumers_any = false;
  p = SelectDenoiser(in);
  Check(p.why == Why::kNoConsumer, "no consumer is named before no room: nothing is asked of memory for an RR that is not wanted");
  // The consumer rule is the first of the rules after the switch and the row: a PC that cannot run RR, or a build without it, answers "no effect wants it" and nothing more while nothing does.
  in = ReadyInputs();
  in.consumers_active = false;
  in.consumers_any = false;
  in.facts.build_has_rr = false;
  p = SelectDenoiser(in);
  Check(p.why == Why::kNoConsumer, "a build without RR and no consumer: the consumer is named (nothing wants RR, so nothing is missing)");
  Check(!PlanInputs{}.facts.failed && PlanInputs{}.room_ok && PlanInputs{}.debug == DebugSwitch::kRow && !PlanInputs{}.facts.asked && !PlanInputs{}.facts.loadable && !PlanInputs{}.consumers_any,
        "the defaults: the row decides, not failed, room, no plugin asked, no consumer");
  // Every reason has a text.
  for (int w = 0; w <= int(Why::kNotLoaded); ++w) {
    Check(std::string(WhyText(Why(w))) != "?", "why " + std::to_string(w) + " has a text");
  }
  // The whole table: 3 settings x 4 switches x 11 yes/no inputs. The oracle below is written out as the player-visible rules, not computed from the code under test: RR runs for exactly the combinations
  // it names.
  int rr = 0;
  int rows = 0;
  for (int setting = 0; setting < 3; ++setting) {
    for (int debug = 0; debug < 4; ++debug) {
      for (int bits = 0; bits < (1 << 11); ++bits) {
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
        t.facts.failed = (bits >> 7) & 1;
        t.room_ok = (bits >> 8) & 1;
        t.facts.asked = (bits >> 9) & 1;
        t.consumers_any = ((bits >> 10) & 1) || t.consumers_active;  // `active` implies `any`: the rules' own contract
        const Plan q = SelectDenoiser(t);
        // The player-visible rules: the row (or the switch's "on", which stands for RR) must not be OURS and the switch must not be off or probe; the PC must run it (build, files, started, plugin asked,
        // supported, not failed, DLSS the upscaler); there must be something to clean (AUTO: an effect RR is known to clean better; RR: any effect with a raw mode; the switch's on: nothing); then room and guides.
        const bool on = t.debug == DebugSwitch::kOn;
        const Denoiser eff = on ? Denoiser::kRr : t.setting;
        const bool consumer = on || (eff == Denoiser::kAuto ? t.consumers_active : t.consumers_any);
        const bool expect = t.debug != DebugSwitch::kOff && t.debug != DebugSwitch::kProbe && eff != Denoiser::kOurs && consumer && t.facts.build_has_rr && t.facts.files_ok &&
                            t.facts.streamline_running && t.facts.asked && t.facts.supported && !t.facts.failed && t.upscaler_is_dlss && t.room_ok && t.guides_ready;
        Check(q.use_rr == expect, "the table: setting " + std::to_string(setting) + " switch " + std::to_string(debug) + " bits " + std::to_string(bits));
        Check(q.use_rr == (q.why == Why::kRrOn), "the table: use_rr and the reason agree");
        ++rows;
        if (q.use_rr) ++rr;
      }
    }
  }
  // The count, worked out by hand. RR needs the other nine inputs (build, files, started, plugin asked, supported, not failed, DLSS, room, guides) in their one good state, so each (setting, switch, the two
  // consumer bits) is one row. The consumer bits make four patterns (active, any): (0,0) (0,1) (1,0) (1,1); `any` is true in the last three. Switch off and probe: none. Switch on: all three settings stand for RR and
  // the consumer rule is lifted: 3 x 4 = 12. Switch auto: AUTO needs `active`: (1,0) (1,1) = 2; the row's RR needs any: 3; OURS: none. 12 + 2 + 3 = 17.
  Check(rows == 3 * 4 * 2048, "the table size");
  Check(rr == 17, "the number of RR cases, worked out by hand (" + std::to_string(rr) + " of " + std::to_string(rows) + ")");
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
  Check(FrameGate(g) == Gate::kSizeMismatch, "another size (smaller than the render size)");
  // The sizes the runs really had (chain-rr's logs, 2026-11-21): the guest pads its main colour target, 1280x720 is drawn into 1280x736, so the planes are larger than the render size by the padding and
  // the gate must be open (it refused every one of these, and RR never ran). Render size, planes: 1x 1280x720 / 1280x736; DLSS Quality at 720p output 832x468 / 832x478; 4K at the DLAA's 2x 2560x1440 / 2560x1472;
  // 1080p output at Quality 1280x720 / 1280x736; ultra performance of a 4K output 1280x720 (3x of the 0.33 scale is 3840x2160 / 3840x2208).
  const struct {
    uint32_t render[2];
    uint32_t planes[2];
    const char* what;
  } real[] = {{{1280, 720}, {1280, 736}, "1x"},       {{832, 468}, {832, 478}, "0.65x"},       {{2560, 1440}, {2560, 1472}, "2x"},
              {{1920, 1080}, {1920, 1104}, "1.5x"},   {{3840, 2160}, {3840, 2208}, "3x"},      {{640, 360}, {640, 368}, "0.5x"}};
  for (const auto& r : real) {
    g = f;
    g.render_size[0] = r.render[0];
    g.render_size[1] = r.render[1];
    g.gbuffer_size[0] = r.planes[0];
    g.gbuffer_size[1] = r.planes[1];
    Check(FrameGate(g) == Gate::kOpen, std::string("the guest's padding is not a mismatch: planes ") + std::to_string(r.planes[0]) + "x" + std::to_string(r.planes[1]) + " for render " +
                                           std::to_string(r.render[0]) + "x" + std::to_string(r.render[1]) + " (" + r.what + ")");
    Check(GBufferCoversRenderSize(r.planes, r.render), std::string("the planes cover the render size at ") + r.what);
  }
  // What stays a mismatch: planes smaller in one direction, a much larger one (another view), a zero render size, and the limits of the padding.
  const struct {
    uint32_t render[2];
    uint32_t planes[2];
    bool covers;
    const char* what;
  } limits[] = {{{832, 468}, {832, 467}, false, "one row short"},
                {{832, 468}, {831, 478}, false, "one column short"},
                {{1280, 720}, {1280, 1080}, false, "a view half as tall again"},
                {{1280, 720}, {1920, 720}, false, "a view wider by half"},
                {{1280, 720}, {1280, 720 + 91}, false, "more than an eighth taller (91 rows of 720)"},
                {{1280, 720}, {1280, 720 + 90}, true, "exactly an eighth taller"},
                {{2560, 1440}, {2560, 1440 + 128}, true, "the pad limit exactly"},
                {{2560, 1440}, {2560, 1440 + 129}, false, "one over the pad limit"},
                {{4000, 4000}, {4000, 4000 + 128}, true, "the limit on a large render size"},
                {{4000, 4000}, {4000, 4000 + 129}, false, "one over, large render size"},
                {{0, 720}, {1280, 736}, false, "a zero render width"},
                {{1280, 0}, {1280, 736}, false, "a zero render height"},
                {{1280, 720}, {0, 0}, false, "empty planes"},
                {{0, 0}, {0, 0}, false, "nothing at all (no render size: never a covered one)"}};
  for (const auto& l : limits) {
    g = f;
    g.render_size[0] = l.render[0];
    g.render_size[1] = l.render[1];
    g.gbuffer_size[0] = l.planes[0];
    g.gbuffer_size[1] = l.planes[1];
    Check(GBufferCoversRenderSize(l.planes, l.render) == l.covers, std::string("covers: ") + l.what);
    Check(FrameGate(g) == (l.covers ? Gate::kOpen : Gate::kSizeMismatch), std::string("gate: ") + l.what);
  }
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
  // RR-5: the row decides what a start loads where the switch is on auto: not OURS, and only where an effect has a raw mode (RR-INT: the shipped table has two, so a start on AUTO loads it on an RTX card).
  PluginInputs row;
  row.build_has_rr = true;
  row.files_ok = true;
  row.looks_rtx = true;
  row.d3d12 = true;
  Check(row.debug == DebugSwitch::kRow && row.setting == Denoiser::kAuto && !row.raw_mode_built, "the defaults: the row on AUTO, no raw mode");
  Check(!WantRrPlugin(row) && !PluginWantedByChoice(row), "AUTO with no effect that has a raw mode: the plugin is not asked for (a player's start is what it was before the row)");
  // What temporal_dlss.cpp fills in: AnyRawModeBuilt() of the shipped table, and of a table with no raw mode at all.
  const ConsumerTable none_built = {{{"a", false, false}, {"b", false, false}, {"c", false, false}, {"d", false, false}}};
  row.raw_mode_built = AnyRawModeBuilt(none_built);
  Check(!WantRrPlugin(row) && !PluginWantedByChoice(row), "a table with no raw mode: the plugin is not asked for");
  row.raw_mode_built = AnyRawModeBuilt();
  Check(WantRrPlugin(row) && PluginWantedByChoice(row), "the shipped table has raw modes (RR-INT): AUTO's start asks for the plugin");
  row.setting = Denoiser::kRr;
  Check(WantRrPlugin(row), "the row's RR: asked for");
  row.setting = Denoiser::kOurs;
  Check(!WantRrPlugin(row) && !PluginWantedByChoice(row), "OURS: not asked for (nothing of RR costs a player who chose it)");
  row.debug = DebugSwitch::kOn;
  Check(WantRrPlugin(row), "OURS with the switch on: the switch forces it");
  row.debug = DebugSwitch::kProbe;
  Check(WantRrPlugin(row), "OURS with the switch on probe: loaded to be asked");
  row.debug = DebugSwitch::kOff;
  row.setting = Denoiser::kAuto;
  Check(!WantRrPlugin(row), "AUTO with the switch off: never");
  row.debug = DebugSwitch::kRow;
  row.looks_rtx = false;
  Check(PluginWantedByChoice(row) && !WantRrPlugin(row), "wanted by the choice but not an RTX card: not asked for (the files are not even looked at for the choice's answer alone)");
  row.looks_rtx = true;
  row.d3d12 = false;
  Check(PluginWantedByChoice(row) && !WantRrPlugin(row), "Vulkan with DLSS not saved: not asked for");
  row.dlss_saved_at_start = true;
  Check(WantRrPlugin(row), "Vulkan with DLSS saved: asked for");
  Check(kFiles.size() == 2 && std::string(kFiles[0]) == "sl.dlss_d.dll" && std::string(kFiles[1]) == "nvngx_dlssd.dll", "the files");
  Check(kSignedFiles.size() == 1 && std::string(kSignedFiles[0]) == "sl.dlss_d.dll", "the signed plugin");

  Denoiser d = Denoiser::kOurs;
  Check(ParseDenoiser("auto", &d) && d == Denoiser::kAuto && ParseDenoiser("rr", &d) && d == Denoiser::kRr && ParseDenoiser("ours", &d) && d == Denoiser::kOurs, "denoiser words");
  Check(!ParseDenoiser("AUTO", &d) && !ParseDenoiser("", &d) && !ParseDenoiser("nrd", &d), "other words are refused");
  DebugSwitch s = DebugSwitch::kOn;
  Check(ParseDebugSwitch("off", &s) && s == DebugSwitch::kOff && ParseDebugSwitch("probe", &s) && s == DebugSwitch::kProbe && ParseDebugSwitch("on", &s) && s == DebugSwitch::kOn &&
            ParseDebugSwitch("auto", &s) && s == DebugSwitch::kRow,
        "switch words (auto is the row deciding)");
  Check(!ParseDebugSwitch("true", &s) && !ParseDebugSwitch("1", &s) && !ParseDebugSwitch("", &s), "other switch words are refused");
  Check(std::string(DenoiserName(Denoiser::kRr)) == "rr" && std::string(DebugSwitchName(DebugSwitch::kProbe)) == "probe" && std::string(DebugSwitchName(DebugSwitch::kRow)) == "auto" &&
            std::string(DebugSwitchName(DebugSwitch::kOff)) == "off" && std::string(DebugSwitchName(DebugSwitch::kOn)) == "on",
        "names round trip");
  // The numbers of the first three words are the ones the switch had before RR-5 (the test loops of other files count 0..2); the new default is last.
  Check(int(DebugSwitch::kOff) == 0 && int(DebugSwitch::kProbe) == 1 && int(DebugSwitch::kOn) == 2 && int(DebugSwitch::kRow) == 3, "the switch's numbers");
}

// RR-5 + RR-INT: the consumers. The shipped table: the screen-space reflections (RR-4a) and the cars' ray traced reflections (RR-4b) have a raw mode; the city's reflections and the bounce light do not exist yet;
// NO effect is trusted to AUTO yet (auto_uses_rr waits for the RR-6 A/B), so AUTO is OURS wherever, and the row's RR value takes a frame while SSR or the cars' reflections are on. These checks hold the rules for
// whichever cells are flipped (written against a made-up table below) and say what the shipped table is.
void CheckConsumers() {
  Check(kConsumerCount == 4 && kAllConsumers == 0xFu, "four consumers");
  Check(AnyRawModeBuilt(), "today two effects have a raw mode (RR-INT)");
  const bool built[kConsumerCount] = {true, true, false, false};
  for (size_t i = 0; i < kConsumerCount; ++i) {
    Check(kConsumers[i].raw_mode_built == built[i], std::string("today ") + kConsumers[i].name + (built[i] ? " has a raw mode" : " has none (the effect does not exist yet)"));
    Check(!kConsumers[i].auto_uses_rr, std::string("today ") + kConsumers[i].name + ": AUTO does not trust RR with it before the RR-6 A/B");
    Check(!kConsumers[i].auto_uses_rr || kConsumers[i].raw_mode_built, std::string(kConsumers[i].name) + ": AUTO trusts RR only with an effect that has a raw mode");
    Check(kConsumers[i].name != nullptr && kConsumers[i].name[0] != '\0', "every consumer has a name");
  }
  Check(kConsumers[size_t(Consumer::kSsr)].raw_mode_built && kConsumers[size_t(Consumer::kCarRtReflections)].raw_mode_built, "SSR and the cars' ray traced reflections are the two with a raw mode");
  Check(!kConsumers[size_t(Consumer::kCityRtReflections)].raw_mode_built && !kConsumers[size_t(Consumer::kRtGi)].raw_mode_built, "the city's reflections and the bounce light have none yet");
  Check(AnyConsumerOn(ConsumerBit(Consumer::kSsr)) && AnyConsumerOn(ConsumerBit(Consumer::kCarRtReflections)) && AnyConsumerOn(kAllConsumers), "SSR on, the cars' reflections on: a consumer is on");
  Check(!AnyConsumerOn(0u) && !AnyConsumerOn(ConsumerBit(Consumer::kCityRtReflections)) && !AnyConsumerOn(ConsumerBit(Consumer::kRtGi)) && !AnyConsumerOn(ConsumerBit(Consumer::kCityRtReflections) | ConsumerBit(Consumer::kRtGi)),
        "nothing on, or only an effect with no raw mode: no consumer");
  Check(!AnyAutoConsumerOn(kAllConsumers) && !AnyAutoConsumerOn(ConsumerBit(Consumer::kSsr)), "all four on: AUTO still has no consumer it trusts (RR-6's A/B flips auto_uses_rr)");
  Check(ConsumerBit(Consumer::kSsr) == 1u && ConsumerBit(Consumer::kCarRtReflections) == 2u && ConsumerBit(Consumer::kCityRtReflections) == 4u && ConsumerBit(Consumer::kRtGi) == 8u, "the bits");
  Check(std::string(InfoOf(Consumer::kSsr).name) == "screen-space reflections", "InfoOf");
  // The rules over a made-up table with cells flipped: SSR built and trusted, the car's reflections built and not trusted, the city's reflections trusted but not built (an A/B verdict that came before the raw
  // mode: AUTO must still not count it), the bounce light neither.
  const ConsumerTable made = {{{"a", true, true}, {"b", true, false}, {"c", false, true}, {"d", false, false}}};
  const ConsumerTable none_cells = {{{"a", false, false}, {"b", false, false}, {"c", false, false}, {"d", false, false}}};
  Check(AnyRawModeBuilt(made) && AnyRawModeBuilt(kConsumers) && !AnyRawModeBuilt(none_cells), "a made-up table has raw modes, the shipped one has two, a table of false cells none");
  Check(AnyConsumerOn(1u, made) && AnyConsumerOn(2u, made) && !AnyConsumerOn(4u, made) && !AnyConsumerOn(8u, made) && !AnyConsumerOn(0u, made) && AnyConsumerOn(0xFu, made),
        "a made-up table: the built effects that are on are consumers, the others (and an empty set) are not");
  Check(AnyAutoConsumerOn(1u, made) && !AnyAutoConsumerOn(2u, made) && AnyAutoConsumerOn(3u, made) && !AnyAutoConsumerOn(4u, made) && !AnyAutoConsumerOn(12u, made) && !AnyAutoConsumerOn(0u, made),
        "a made-up table: AUTO trusts only an effect that is built AND that RR was measured better with");
  // Every subset of every table of two flags a cell: the consumers AUTO trusts are always a subset of the consumers, and a table with no raw mode has none of either.
  bool subset = true;
  bool empty_ok = true;
  for (int cells = 0; cells < 256; ++cells) {
    ConsumerTable tb{};
    for (size_t i = 0; i < kConsumerCount; ++i) {
      tb[i] = {"x", ((cells >> (2 * i)) & 1) != 0, ((cells >> (2 * i + 1)) & 1) != 0};
    }
    for (uint32_t on = 0; on <= kAllConsumers; ++on) {
      subset = subset && (!AnyAutoConsumerOn(on, tb) || AnyConsumerOn(on, tb));
      if (!AnyRawModeBuilt(tb)) empty_ok = empty_ok && !AnyConsumerOn(on, tb) && !AnyAutoConsumerOn(on, tb);
    }
  }
  Check(subset, "AUTO's consumers are always consumers (the plan's `active` implies `any`)");
  Check(empty_ok, "a table with no raw mode has no consumer");

  // RR-INT: the plan the way TemporalSystem::UpdateDenoiserPlan makes it from the board of the effects that are on (denoiser::NoteConsumer): the shipped table, a PC that could run RR, DLSS as the upscaler.
  const auto plan_from_board = [](Denoiser setting, uint32_t on, const ConsumerTable& table) {
    PlanInputs in = ReadyInputs();
    in.setting = setting;
    in.consumers_active = AnyAutoConsumerOn(on, table);
    in.consumers_any = AnyConsumerOn(on, table);
    return SelectDenoiser(in);
  };
  const uint32_t ssr = ConsumerBit(Consumer::kSsr);
  const uint32_t cars = ConsumerBit(Consumer::kCarRtReflections);
  const uint32_t city = ConsumerBit(Consumer::kCityRtReflections);
  const uint32_t gi = ConsumerBit(Consumer::kRtGi);
  // AUTO with and without consumers, the shipped table (nothing trusted yet): ours in every case, the reason the missing consumer.
  for (const uint32_t on : {0u, ssr, cars, ssr | cars, city, gi, kAllConsumers}) {
    const Plan p = plan_from_board(Denoiser::kAuto, on, kConsumers);
    Check(!p.use_rr && p.why == Why::kNoConsumer, "AUTO, the shipped table, effects on " + std::to_string(on) + ": ours (no effect AUTO trusts RR with yet)");
  }
  // The row's RR value with and without consumers: RR while SSR or the cars' reflections are on, ours (kNoConsumer) with nothing on or only effects that do not exist yet.
  for (const uint32_t on : {ssr, cars, ssr | cars, ssr | city, cars | gi, kAllConsumers}) {
    const Plan p = plan_from_board(Denoiser::kRr, on, kConsumers);
    Check(p.use_rr && p.why == Why::kRrOn, "DLSS RR, the shipped table, effects on " + std::to_string(on) + ": RR");
  }
  for (const uint32_t on : {0u, city, gi, city | gi}) {
    const Plan p = plan_from_board(Denoiser::kRr, on, kConsumers);
    Check(!p.use_rr && p.why == Why::kNoConsumer, "DLSS RR, the shipped table, effects on " + std::to_string(on) + ": no effect with a raw mode is on, ours");
  }
  for (const uint32_t on : {0u, ssr, kAllConsumers}) {
    const Plan p = plan_from_board(Denoiser::kOurs, on, kConsumers);
    Check(!p.use_rr && p.why == Why::kSettingOurs, "OURS, effects on " + std::to_string(on) + ": ours");
  }
  // The whole small table: the shipped plan is RR exactly where the row says DLSS RR and SSR or the cars' reflections are on.
  bool whole = true;
  for (int setting = 0; setting < 3; ++setting) {
    for (uint32_t on = 0; on <= kAllConsumers; ++on) {
      const Plan p = plan_from_board(Denoiser(setting), on, kConsumers);
      whole = whole && (p.use_rr == (Denoiser(setting) == Denoiser::kRr && (on & (ssr | cars)) != 0));
    }
  }
  Check(whole, "the shipped table: RR exactly where the row says DLSS RR and SSR or the cars' reflections are on (3 settings x 16 sets of effects)");
  // With AUTO's cell flipped for SSR (what RR-6's A/B may do) AUTO is RR exactly while SSR is on; the cars' reflections, built and not trusted, never bring it.
  ConsumerTable flipped = kConsumers;
  flipped[size_t(Consumer::kSsr)].auto_uses_rr = true;
  bool flip_ok = true;
  for (uint32_t on = 0; on <= kAllConsumers; ++on) {
    const Plan p = plan_from_board(Denoiser::kAuto, on, flipped);
    flip_ok = flip_ok && (p.use_rr == ((on & ssr) != 0)) && (p.use_rr || p.why == Why::kNoConsumer);
  }
  Check(flip_ok, "AUTO's cell flipped for SSR: AUTO is RR exactly while SSR is on");
  // The PC's own limits still come after: the RR value with SSR on on a PC that cannot (no support, not DLSS, files missing, failed) is ours with the PC's reason.
  PlanInputs pc = ReadyInputs();
  pc.setting = Denoiser::kRr;
  pc.consumers_active = false;
  pc.consumers_any = true;
  pc.facts.supported = false;
  Check(!SelectDenoiser(pc).use_rr && SelectDenoiser(pc).why == Why::kNotSupported, "DLSS RR with SSR on, a card that cannot: ours, not supported");
  pc.facts.supported = true;
  pc.upscaler_is_dlss = false;
  Check(!SelectDenoiser(pc).use_rr && SelectDenoiser(pc).why == Why::kUpscalerNotDlss, "DLSS RR with SSR on, FSR as the upscaler: ours");
  pc.upscaler_is_dlss = true;
  pc.debug = DebugSwitch::kOn;
  pc.consumers_any = false;
  Check(SelectDenoiser(pc).use_rr, "the debug switch's on lifts the consumer rule (the A/B and cost runs)");
}

// --- RR-3 -------------------------------------------------------------------------------------------------------------------------------------------------

void CheckDispatch() {
  // The whole table: 2^4 inputs. RR for exactly the one combination; every other one is a Super Resolution frame with the first reason that applies.
  int rr = 0;
  for (int bits = 0; bits < 16; ++bits) {
    DispatchInputs in;
    in.plan_rr = (bits >> 0) & 1;
    in.rr_failed = (bits >> 1) & 1;
    in.guides_now = (bits >> 2) & 1;
    in.context_ready = (bits >> 3) & 1;
    const DispatchChoice c = ChooseDispatch(in);
    const bool expect = in.plan_rr && !in.rr_failed && in.guides_now && in.context_ready;
    Check((c.kind == Dispatch::kRayReconstruction) == expect, "the dispatch table, bits " + std::to_string(bits));
    Check((c.kind == Dispatch::kRayReconstruction) == (c.why == DispatchWhy::kRr), "the dispatch kind and its reason agree, bits " + std::to_string(bits));
    if (c.kind == Dispatch::kRayReconstruction) ++rr;
  }
  Check(rr == 1, "RR for exactly one combination of sixteen");
  DispatchInputs in;
  Check(ChooseDispatch(in).why == DispatchWhy::kPlanOurs, "nothing set: the plan is not RR");
  in.plan_rr = true;
  in.rr_failed = true;
  in.guides_now = true;
  in.context_ready = true;
  Check(ChooseDispatch(in).why == DispatchWhy::kRrFailed && ChooseDispatch(in).kind == Dispatch::kSuperResolution, "a dropped RR is a Super Resolution frame (the upscaler is never lost)");
  in.rr_failed = false;
  in.guides_now = false;
  Check(ChooseDispatch(in).why == DispatchWhy::kNoGuides, "a missing guide is a Super Resolution frame");
  in.guides_now = true;
  in.context_ready = false;
  Check(ChooseDispatch(in).why == DispatchWhy::kContextNotReady, "RR's context still being made: Super Resolution goes on");
  for (int w = 0; w < int(DispatchWhy::kCount); ++w) Check(std::string(DispatchWhyText(DispatchWhy(w))) != "?", "dispatch reason text " + std::to_string(w));
}

void CheckContextKeeper() {
  Check(ContextWanted(Why::kRrOn) && ContextWanted(Why::kGuidesNotReady), "RR chosen, or chosen once the guides are there: the context is wanted");
  for (int w = 0; w <= int(Why::kNoRoom); ++w) {
    const Why why = Why(w);
    if (why == Why::kRrOn || why == Why::kGuidesNotReady) continue;
    Check(!ContextWanted(why), "no context is made for the reason " + std::to_string(w));
  }
  ContextKeeper k;
  Check(!k.keep() && !k.Update(false), "nothing wanted, nothing kept");
  Check(k.Update(true) && k.keep(), "wanted: kept at once");
  for (uint32_t i = 0; i < kContextHoldFrames; ++i) Check(k.Update(false), "held after the want stops, frame " + std::to_string(i));
  Check(!k.Update(false) && !k.keep(), "released after kContextHoldFrames frames of not being wanted");
  Check(k.Update(true), "wanted again: back at once");
  for (uint32_t i = 0; i < kContextHoldFrames / 2; ++i) k.Update(false);
  Check(k.Update(true), "wanted again half way through the hold");
  for (uint32_t i = 0; i < kContextHoldFrames; ++i) k.Update(false);
  Check(k.keep(), "the hold starts again from the last want (a toggled-off-and-on effect keeps its context)");
  k.Reset();
  Check(!k.keep(), "reset");
}

void CheckContextNeed() {
  constexpr uint64_t mib = kMiB;
  Check(NetworkBytes(1920, 1080) == 154 * mib && NetworkBytes(2560, 1440) == 268 * mib && NetworkBytes(3840, 2160) == 579 * mib, "NVIDIA's three figures at 1080p, 1440p and 4K");
  Check(NetworkBytes(0, 1080) == 0 && NetworkBytes(1920, 0) == 0, "no output: nothing");
  uint64_t last = 0;
  bool monotone = true;
  for (uint32_t h = 180; h <= 4320; h += 90) {
    const uint32_t w = h * 16 / 9;
    const uint64_t n = NetworkBytes(w, h);
    monotone = monotone && n > last;
    last = n;
  }
  Check(monotone, "the network's need grows with the output's pixels");
  const uint64_t mid = NetworkBytes(2200, 1240);
  Check(mid > 154 * mib && mid < 268 * mib, "between 1080p and 1440p: between the two figures");
  const uint64_t big = NetworkBytes(5120, 2880);
  Check(big > 579 * mib && big < 579 * mib * 3, "above 4K on the last slope");
  const uint64_t small = NetworkBytes(1280, 720);
  Check(small > 154 * mib / 3 && small < 154 * mib / 2, "under 1080p in proportion (720p is 4/9 of the pixels)");
  // The measured figure replaces the table, scaled by the pixels; the guides are added where they are not held yet.
  Check(ContextNeedBytes(3840, 2160, 0, 0, 0) == 579 * mib, "nothing measured: the table");
  Check(ContextNeedBytes(3840, 2160, 0, 0, 100 * mib) == 679 * mib, "the guides are added");
  Check(ContextNeedBytes(3840, 2160, 400 * mib, uint64_t(3840) * 2160, 0) == 400 * mib, "a context measured at this output: its own figure");
  Check(ContextNeedBytes(1920, 1080, 400 * mib, uint64_t(3840) * 2160, 0) == 100 * mib, "a figure measured at 4K is scaled to 1080p by the pixels");
  Check(ContextNeedBytes(1920, 1080, 400 * mib, 0, 0) == 154 * mib, "a figure with no pixel count is ignored");
  // The room line: the use plus the need under 90 % of the budget (vram_room.h).
  Check(HasRoomForContext(0, 0, 10000 * mib), "an adapter with no budget reports nothing to compare with: room");
  Check(HasRoomForContext(4000 * mib, 16000 * mib, 579 * mib), "plenty of room");
  Check(!HasRoomForContext(14000 * mib, 16000 * mib, 579 * mib), "a card nearly full: no room");
  Check(HasRoomForContext(7000 * mib, 8000 * mib, 100 * mib) && !HasRoomForContext(7000 * mib, 8000 * mib, 300 * mib), "8 GB: 100 MB fits under 90 %, 300 MB does not");
  // The wait after a refusal is the shared one: 5 s, 10 s ... one minute.
  Check(mcla::gpu::vram_room::NextWaitMs(0) == 5000 && mcla::gpu::vram_room::NextWaitMs(5000) == 10000 && mcla::gpu::vram_room::NextWaitMs(60000) == 60000, "the refusal's wait is the shared one");
}

void CheckFailureBook() {
  FailureBook b;
  Check(!b.dropped() && b.errors_in_row() == 0, "fresh");
  Check(!b.Note(FailureBook::Event::kRefused) && !b.Note(FailureBook::Event::kRefused) && !b.Note(FailureBook::Event::kRefused) && !b.Note(FailureBook::Event::kRefused), "refusals never drop RR");
  Check(!b.dropped() && b.refused() == 4, "four refusals counted");
  Check(!b.Note(FailureBook::Event::kError) && !b.Note(FailureBook::Event::kError), "two errors in a row: RR stays");
  Check(!b.Note(FailureBook::Event::kRefused) && b.errors_in_row() == 2, "a refusal between errors does not reset the count (nothing ran)");
  Check(!b.Note(FailureBook::Event::kRan) && b.errors_in_row() == 0, "a run resets the count");
  Check(!b.Note(FailureBook::Event::kError) && !b.Note(FailureBook::Event::kError), "two errors again");
  Check(b.Note(FailureBook::Event::kError) && b.dropped(), "the third in a row drops RR for the session");
  Check(!b.Note(FailureBook::Event::kError) && b.dropped(), "it is reported once");
  Check(b.errors() == 6 && b.ran() == 1, "the totals");
  FailureBook c;
  Check(c.Drop() && c.dropped() && !c.Drop(), "the feature that could not be made drops RR at once, once");
  FailureBook d;
  for (int i = 0; i < 1000; ++i) {
    d.Note(FailureBook::Event::kError);
    d.Note(FailureBook::Event::kError);
    d.Note(FailureBook::Event::kRan);
  }
  Check(!d.dropped(), "errors that a run breaks up never add up");
}

void CheckTestLever() {
  Check(TestLeverOf(0) == TestLever::kOff && TestLeverOf(1) == TestLever::kWithholdGuides && TestLeverOf(2) == TestLever::kRefuseGuides && TestLeverOf(3) == TestLever::kInjectErrors, "the lever's words");
  Check(TestLeverOf(-1) == TestLever::kOff && TestLeverOf(4) == TestLever::kOff && TestLeverOf(1000) == TestLever::kOff, "anything else is off (the cvar's range is 0 to 3)");
  // The phases: the first 30 frames of a run are untouched, then 30 act, 30 rest, and so on.
  bool ok = true;
  for (uint64_t f = 0; f < 600; ++f) ok = ok && (TestPhaseActive(f) == ((f % 60) >= 30));
  Check(ok && !TestPhaseActive(0) && !TestPhaseActive(29) && TestPhaseActive(30) && TestPhaseActive(59) && !TestPhaseActive(60), "the lever acts in the second half of each 60 frames");
  // Three injected errors in a row are exactly what drops RR (the lever's 3), and fewer never do.
  FailureBook b;
  bool dropped = false;
  for (uint32_t i = 0; i < kTestErrorFrames; ++i) dropped = b.Note(FailureBook::Event::kError) || dropped;
  Check(kTestErrorFrames == FailureBook::kMaxErrors && dropped && b.dropped(), "the lever's injected errors drop Ray Reconstruction, once");
  FailureBook c;
  for (uint32_t i = 0; i + 1 < kTestErrorFrames; ++i) c.Note(FailureBook::Event::kError);
  Check(!c.dropped(), "one error fewer does not");
  // The refusing lever: a frame with no diffuse albedo is refused by the tag plan (the backend then gives it to Super Resolution), and the same frame with it is accepted.
  Inputs in = FullInputs();
  in.diffuse_albedo = Image{};
  const TagPlan plan = PlanTags(in);
  Check(plan.verdict == Verdict::kMissing && plan.culprit == Buffer::kDiffuseAlbedo, "no diffuse albedo: the plan refuses the frame (mcla_gpu_dlss_rr_test 2)");
  Check(PlanTags(FullInputs()).verdict == Verdict::kOk, "... and the frame with it is accepted");
}

// A timeline of frames through the rules the temporal system and the backend apply: the plan (from the last frame's guides), the context keeper (the context is asked for while the plan wants it), a
// backend that makes the RR feature in `build_frames` frames, and the dispatch. The live switch without a restart: every move below is a setting or a frame kind changing while the game runs.
struct Timeline {
  PlanInputs plan_in;
  ContextKeeper keeper;
  bool requested = false;
  bool made = false;
  uint32_t asked_frame = 0;
  uint32_t build_frames = 5;
  uint32_t requests = 0;         // how many times a context was asked for (a rebuild counts)
  bool failed = false;
  bool last_guides = false;
  uint32_t frame = 0;
  std::vector<DispatchChoice> log;
  Plan plan;

  Timeline() {
    plan_in = ReadyInputs();
    plan_in.guides_ready = false;
  }
  // `gbuffer`: the frame has a G-buffer (a play frame); the guide pass runs where it is wanted (the plan's reasons ContextWanted, rr_guides_rules.h PassWanted: the same two words).
  void Step(bool gbuffer) {
    plan_in.guides_ready = last_guides;
    plan_in.facts.failed = failed;
    plan = SelectDenoiser(plan_in);
    const bool keep = keeper.Update(ContextWanted(plan.why));
    if (keep && !requested) {
      requested = true;
      made = false;
      asked_frame = frame;
      ++requests;
    } else if (!keep && requested) {
      requested = false;
      made = false;
    }
    if (requested && !made && frame >= asked_frame + build_frames) made = true;
    const bool guides_now = ContextWanted(plan.why) && gbuffer;
    DispatchInputs d;
    d.plan_rr = plan.use_rr;
    d.rr_failed = failed;
    d.guides_now = guides_now;
    d.context_ready = made;
    log.push_back(ChooseDispatch(d));
    last_guides = guides_now;
    ++frame;
  }
  uint32_t Count(Dispatch kind) const {
    uint32_t n = 0;
    for (const DispatchChoice& c : log) n += c.kind == kind;
    return n;
  }
};

void CheckLiveSwitch() {
  Timeline t;
  // The switch goes on while the game runs: the first frames are Super Resolution's (guides not there, then the context being made), then RR.
  for (int i = 0; i < 20; ++i) t.Step(true);
  Check(t.log[0].kind == Dispatch::kSuperResolution && t.log[0].why == DispatchWhy::kPlanOurs, "frame 0: no guides yet, Super Resolution");
  Check(t.log[1].kind == Dispatch::kSuperResolution && t.log[1].why == DispatchWhy::kContextNotReady, "frame 1: the plan is RR, the context is being made: Super Resolution goes on");
  int first_rr = -1;
  for (int i = 0; i < 20 && first_rr < 0; ++i) {
    if (t.log[size_t(i)].kind == Dispatch::kRayReconstruction) first_rr = i;
  }
  Check(first_rr == 5, "RR from the frame its context was made (frame 5 with a 5 frame build)");
  for (int i = first_rr; i < 20; ++i) Check(t.log[size_t(i)].kind == Dispatch::kRayReconstruction, "RR from then on, frame " + std::to_string(i));
  Check(t.requests == 1, "one context asked for");
  // The pause menu opens: frames without a G-buffer are Super Resolution's, the context stays (it is held), and RR is back one frame after the play frames return (the plan is made
  // from the frame before's guides), with no rebuild.
  for (int i = 0; i < 40; ++i) t.Step(false);
  for (int i = 0; i < 40; ++i) {
    const DispatchChoice& c = t.log[20 + size_t(i)];
    Check(c.kind == Dispatch::kSuperResolution && (c.why == DispatchWhy::kNoGuides || c.why == DispatchWhy::kPlanOurs), "a pause frame is Super Resolution's, frame " + std::to_string(i));
  }
  for (int i = 0; i < 10; ++i) t.Step(true);
  Check(t.log[60].kind == Dispatch::kSuperResolution, "the first play frame after the pause: the plan still has the pause's guides, Super Resolution");
  Check(t.log[61].kind == Dispatch::kRayReconstruction, "one frame later RR again");
  Check(t.requests == 1, "the pause did not rebuild the context");
  // A single frame with no guides (a pass whose pipeline was still being made, a cutscene frame) is one Super Resolution frame and two with the plan's lag; RR goes on after it.
  t.Step(false);
  t.Step(true);
  t.Step(true);
  t.Step(true);
  Check(t.log[70].kind == Dispatch::kSuperResolution && t.log[73].kind == Dispatch::kRayReconstruction && t.requests == 1, "a lone frame without guides: Super Resolution for it and the next, then RR, no rebuild");
  // The debug switch goes off while the game runs: Super Resolution at once, the context held, then released after kContextHoldFrames frames.
  t.plan_in.debug = DebugSwitch::kOff;
  t.Step(true);
  Check(t.log.back().kind == Dispatch::kSuperResolution && t.log.back().why == DispatchWhy::kPlanOurs && t.requested, "switched off: Super Resolution at once, the context is still there");
  for (uint32_t i = 0; i < kContextHoldFrames - 1; ++i) t.Step(true);
  Check(t.requested, "held for the whole hold");
  t.Step(true);
  t.Step(true);
  Check(!t.requested, "released after the hold");
  // And on again (the switch back on auto, the row deciding): the context is made again and the switch is the same as the first time.
  t.plan_in.debug = DebugSwitch::kRow;
  for (int i = 0; i < 12; ++i) t.Step(true);
  Check(t.requests == 2 && t.log.back().kind == Dispatch::kRayReconstruction, "on again: one more context, RR");
  // RR fails in the session (three errors in a row, or the feature could not be made): Super Resolution from then on, every frame, and the context is not wanted any more.
  t.failed = true;
  for (int i = 0; i < 5; ++i) t.Step(true);
  for (int i = 0; i < 5; ++i) {
    const DispatchChoice& c = t.log[t.log.size() - 1 - size_t(i)];
    Check(c.kind == Dispatch::kSuperResolution && c.why == DispatchWhy::kPlanOurs, "failed: Super Resolution (the plan already says ours)");
  }
  Check(t.plan.why == Why::kRrFailed && !ContextWanted(t.plan.why), "the plan says why and wants no context");
  // A context that takes long to make: Super Resolution all along, never a plain frame.
  Timeline u;
  u.build_frames = 1000;
  for (int i = 0; i < 50; ++i) u.Step(true);
  Check(u.Count(Dispatch::kRayReconstruction) == 0 && u.log.back().why == DispatchWhy::kContextNotReady, "a context that takes long to make: Super Resolution all along");
  // No consumer (AUTO): nothing is asked for, ever.
  Timeline v;
  v.plan_in.consumers_active = false;
  for (int i = 0; i < 700; ++i) v.Step(true);
  Check(v.requests == 0 && v.Count(Dispatch::kRayReconstruction) == 0, "AUTO with no consumer: no context, no guides, no RR");
  // The player's OURS and the card without support behave the same.
  Timeline w;
  w.plan_in.setting = Denoiser::kOurs;
  for (int i = 0; i < 100; ++i) w.Step(true);
  Check(w.requests == 0 && w.Count(Dispatch::kRayReconstruction) == 0, "OURS: no context");
  Timeline x;
  x.plan_in.facts.supported = false;
  for (int i = 0; i < 100; ++i) x.Step(true);
  Check(x.requests == 0 && x.Count(Dispatch::kRayReconstruction) == 0, "a card that cannot run RR: no context");
  // No room: the RR context is never asked for; with room again it is.
  Timeline y;
  y.plan_in.room_ok = false;
  for (int i = 0; i < 50; ++i) y.Step(true);
  Check(y.requests == 0 && y.plan.why == Why::kNoRoom, "no room: no context, the reason is the room");
  y.plan_in.room_ok = true;
  for (int i = 0; i < 20; ++i) y.Step(true);
  Check(y.requests == 1 && y.log.back().kind == Dispatch::kRayReconstruction, "room again: the context is made and RR runs");
}

// RR-4: the effects' raw modes. The table over every frame and both switches, against a second statement of the rule (the conjunction, and the order of the reasons).
void CheckRawModes() {
  int raw_count = 0;
  bool identity_when_plan_not_rr = true;
  bool all_reasons_in_order = true;
  bool agrees = true;
  for (int bits = 0; bits < 32; ++bits) {
    RawFrame f;
    f.plan_rr = (bits & 1) != 0;
    f.failed = (bits & 2) != 0;
    f.context_ready = (bits & 4) != 0;
    f.armed = (bits & 8) != 0;
    f.play_view = (bits & 16) != 0;
    for (int sw = 0; sw < 4; ++sw) {
      const bool s_ssr = (sw & 1) != 0;
      const bool s_car = (sw & 2) != 0;
      const RawModes m = RawModesOf(f, s_ssr, s_car);
      const bool want_ssr = f.plan_rr && !f.failed && f.context_ready && f.armed && f.play_view && s_ssr;
      const bool want_car = f.plan_rr && !f.failed && f.context_ready && f.armed && f.play_view && s_car;
      agrees = agrees && m.Ssr() == want_ssr && m.CarRefl() == want_car;
      raw_count += int(m.Ssr()) + int(m.CarRefl());
      if (!f.plan_rr) {
        // the plan is not RR (Super Resolution, FSR, no upscaler, OURS): the effects run as they always did
        identity_when_plan_not_rr = identity_when_plan_not_rr && !m.Ssr() && !m.CarRefl() && m.ssr == RawWhy::kPlanOurs && m.car_refl == RawWhy::kPlanOurs;
      }
      // the reasons come in a fixed order, and the switch is the last word
      const RawWhy expect_ssr = !f.plan_rr ? RawWhy::kPlanOurs : f.failed ? RawWhy::kFailed : !f.context_ready ? RawWhy::kNoContext : (!f.armed || !f.play_view) ? RawWhy::kNoRrFrame : !s_ssr ? RawWhy::kSwitchOff : RawWhy::kRaw;
      const RawWhy expect_car = !f.plan_rr ? RawWhy::kPlanOurs : f.failed ? RawWhy::kFailed : !f.context_ready ? RawWhy::kNoContext : (!f.armed || !f.play_view) ? RawWhy::kNoRrFrame : !s_car ? RawWhy::kSwitchOff : RawWhy::kRaw;
      all_reasons_in_order = all_reasons_in_order && m.ssr == expect_ssr && m.car_refl == expect_car;
    }
  }
  Check(agrees, "raw is exactly: the plan says RR, nothing failed, the context is made, the frame is upscaled and a play frame, and the effect's own switch is on (32 frames x 4 switch cells)");
  Check(raw_count == 4, "one frame of the table is raw: in the four switch cells that is 2 + 2 raw effects (each effect is raw in the two cells where its own switch is on)");
  Check(identity_when_plan_not_rr, "with the plan not RR no effect is raw, whatever else is true (Super Resolution, FSR, no upscaler, OURS: the effects run as they did)");
  Check(all_reasons_in_order, "the reasons come in order: plan, failed, context, frame, switch");
  {
    RawFrame ok;
    ok.plan_rr = ok.context_ready = ok.armed = ok.play_view = true;
    const RawModes both = RawModesOf(ok, true, true);
    const RawModes one = RawModesOf(ok, true, false);
    const RawModes none = RawModesOf(ok, false, false);
    Check(both.Ssr() && both.CarRefl() && one.Ssr() && !one.CarRefl() && one.car_refl == RawWhy::kSwitchOff && !none.Ssr() && !none.CarRefl(), "one switch at a time: each effect has its own, and 0 keeps its own filter while RR takes the frame");
    Check(both == RawModesOf(ok, true, true) && !(both == one), "the modes compare");
    RawFrame pause = ok;
    pause.play_view = false;
    RawFrame plain = ok;
    plain.armed = false;
    Check(RawModesOf(pause, true, true).ssr == RawWhy::kNoRrFrame && RawModesOf(plain, true, true).car_refl == RawWhy::kNoRrFrame, "the pause, garage and map worlds and an unarmed frame are not raw (a Super Resolution frame has nobody to clean the noise)");
  }
  Check(RawSwitchOn(1) && RawSwitchOn(2) && !RawSwitchOn(0), "the debug switches: 0 off, anything else on (the default is 1)");
  for (int w = 0; w < int(RawWhy::kCount); ++w) {
    const std::string text = RawWhyText(RawWhy(w));
    Check(!text.empty() && text != "?", "raw reason " + std::to_string(w) + " has a text");
  }
  for (int e = 0; e < int(RawEffect::kCount); ++e) {
    const std::string name = RawEffectName(RawEffect(e));
    Check(!name.empty() && name != "?", "raw effect " + std::to_string(e) + " has a name");
  }
  // the hit distance a raw effect writes
  Check(RawHitDistance(false, false, 0.0f) == 0.0f && RawHitDistance(false, true, 5.0f) == 0.0f, "nothing traced: 0 (the surface itself is the reflection)");
  Check(RawHitDistance(true, true, 3.5f) == 3.5f && RawHitDistance(true, true, 0.0f) == 0.0f && RawHitDistance(true, true, -1.0f) == 0.0f, "a hit: the ray's length (0 for none)");
  Check(RawHitDistance(true, false, 0.0f) == kRawMissDistance && kRawMissDistance == 4096.0f && kRawMissDistance < kSkyHitDistance, "a miss: far behind (4096, under the sky's 65504)");
  Check(RawHitDistance(true, true, 60000.0f) == kRawMissDistance, "a hit beyond the cap is the cap");
}

}  // namespace

int main() {
  CheckRawModes();
  CheckIds();
  CheckFormats();
  CheckPlanHappyPaths();
  CheckPlanRefusals();
  CheckMatrices();
  CheckJitter();
  CheckSelection();
  CheckGate();
  CheckPluginAndParsers();
  CheckConsumers();
  CheckDispatch();
  CheckContextKeeper();
  CheckContextNeed();
  CheckFailureBook();
  CheckTestLever();
  CheckLiveSwitch();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
