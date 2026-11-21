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
  p = SelectDenoiser(in);
  Check(p.why == Why::kNoConsumer, "no consumer is named before no room: nothing is asked of memory for an RR that is not wanted");
  Check(!PlanInputs{}.facts.failed && PlanInputs{}.room_ok, "the defaults: not failed, room");
  // Every reason has a text.
  for (int w = 0; w <= int(Why::kNoRoom); ++w) {
    Check(std::string(WhyText(Why(w))) != "?", "why " + std::to_string(w) + " has a text");
  }
  // The exhaustive table: RR only for the one combination.
  // The whole table: 3 settings x 3 switches x 9 yes/no inputs (RR-3 added the failure and the room). RR is chosen for exactly the combinations the rules name.
  int rr = 0;
  int rows = 0;
  for (int setting = 0; setting < 3; ++setting) {
    for (int debug = 0; debug < 3; ++debug) {
      for (int bits = 0; bits < (1 << 9); ++bits) {
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
        const Plan q = SelectDenoiser(t);
        const bool expect = t.debug == DebugSwitch::kOn && t.setting != Denoiser::kOurs && t.facts.build_has_rr && t.facts.files_ok &&
                            t.facts.streamline_running && t.facts.supported && t.upscaler_is_dlss &&
                            (t.setting == Denoiser::kRr || t.consumers_active) && t.guides_ready && !t.facts.failed && t.room_ok;
        Check(q.use_rr == expect, "the table: setting " + std::to_string(setting) + " switch " + std::to_string(debug) + " bits " + std::to_string(bits));
        Check(q.use_rr == (q.why == Why::kRrOn), "the table: use_rr and the reason agree");
        ++rows;
        if (q.use_rr) ++rr;
      }
    }
  }
  // AUTO needs every input including a consumer (1 case); RR does not need the consumer (2 cases); OURS never (0): 3 of 4608.
  Check(rows == 4608 && rr == 3, "the table size and the number of RR cases");
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
  // And on again: the context is made again and the switch is the same as the first time.
  t.plan_in.debug = DebugSwitch::kOn;
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
  CheckDispatch();
  CheckContextKeeper();
  CheckContextNeed();
  CheckFailureBook();
  CheckTestLever();
  CheckLiveSwitch();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
