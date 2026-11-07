// MCLA GPU renderer -- HDR10 output encoding (hdr.h).
//
// The present pass, FSR1's last pass and the overlay write the back buffer in
// SDR code values (the picture the game's display gamma table made, the UI as
// drawn). With an HDR10 swap chain (R10G10B10A2, Rec.2020, SMPTE ST 2084) they end
// with HdrFromSdr: decode the code values the way Windows shows SDR on an HDR
// desktop (the piecewise sRGB curve), put them at paper white in nits, roll the
// largest component off above the knee towards the peak, then encode as PQ.
// Compiled with -DPRESENT_HDR=1, -DRCAS_HDR=1 or -DOVERLAY_HDR=1 only; the SDR
// shaders never include this file.
//
// Written for HLSL 2018 too (FSR1's RCAS builds with it): no select(), no
// vector ternaries.
// CHECK: all four users compile to DXIL with the Windows SDK's dxc; their SPIR-V builds (dxc -spirv)
// were not run here.

#ifndef MCLA_GPU_HDR_OUTPUT_HLSLI
#define MCLA_GPU_HDR_OUTPUT_HLSLI

// The same four values in every pass's push constants (hdr.h HdrLevels).
struct HdrLevels {
  float white;  // SDR white in nits
  float peak;   // nits
  float knee;   // the roll-off starts here, nits
  uint flags;   // bit 0: decode with a 2.2 power, else the sRGB curve; bit 1: test pattern
};

float3 HdrSplat(float value) {
  return float3(value, value, value);
}

// An SDR code value (0..1) to light relative to SDR white.
float3 HdrSdrToLinear(float3 code, bool gamma22) {
  code = max(code, HdrSplat(0.0f));
  if (gamma22) {
    return pow(code, HdrSplat(2.2f));
  }
  const float3 low = code * (1.0f / 12.92f);
  const float3 high = pow((code + HdrSplat(0.055f)) * (1.0f / 1.055f), HdrSplat(2.4f));
  return lerp(low, high, step(HdrSplat(0.04045f), code));
}

// Brings the largest component above `knee` nits smoothly towards `peak` nits,
// scaling all three so the hue holds.
float3 HdrRollOff(float3 nits, float knee, float peak) {
  const float largest = max(nits.r, max(nits.g, nits.b));
  if (largest > knee && peak > knee) {
    const float span = peak - knee;
    nits *= (knee + span * (1.0f - exp2((knee - largest) / span * 1.44269504f))) / largest;
  }
  return nits;
}

// Rec.709 light in nits to HDR10 code values (Rec.2020 primaries, SMPTE ST 2084).
float3 HdrEncodePq(float3 nits) {
  const float3 bt2020 = float3(dot(nits, float3(0.627404f, 0.329283f, 0.043313f)),
                               dot(nits, float3(0.069097f, 0.919540f, 0.011362f)),
                               dot(nits, float3(0.016391f, 0.088013f, 0.895595f)));
  const float3 y = pow(clamp(bt2020 * (1.0f / 10000.0f), HdrSplat(0.0f), HdrSplat(1.0f)),
                       HdrSplat(0.1593017578125f));
  return pow((HdrSplat(0.8359375f) + 18.8515625f * y) / (HdrSplat(1.0f) + 18.6875f * y),
             HdrSplat(78.84375f));
}

// SDR code values to the HDR10 code values the back buffer takes.
float3 HdrFromSdr(float3 code, HdrLevels levels) {
  float3 nits = HdrSdrToLinear(code, (levels.flags & 1u) != 0u) * levels.white;
  nits = HdrRollOff(nits, levels.knee, levels.peak);
  return HdrEncodePq(nits);
}

// The test pattern (flag bit 1) over the picture's area, `uv` in 0..1. Top half: 24 bars from
// 100 nits doubling every 4 bars (100, 200, 400, 800, 1600, 3200 nits at bars 0, 4, 8, 12, 16,
// 20), not rolled off, for where the display stops showing steps. Bottom half: paper white on
// the left, the same bars rolled off towards the peak on the right.
float3 HdrTestPattern(float2 uv, HdrLevels levels) {
  const bool top = uv.y < 0.5f;
  const float bar_position = top ? uv.x : uv.x * 2.0f - 1.0f;
  const float bar_nits = 100.0f * exp2(floor(bar_position * 24.0f) * 0.25f);
  float nits = top ? bar_nits : (bar_position < 0.0f ? levels.white : bar_nits);
  const float knee = top ? 10000.0f : levels.knee;
  return HdrEncodePq(HdrRollOff(HdrSplat(nits), knee, levels.peak));
}

#endif  // MCLA_GPU_HDR_OUTPUT_HLSLI
