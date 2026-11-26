#include "rt_denoise_v2_math.hlsli"

// Words 30 and 31 are reserved in every V2 adapter. Float metadata uses
// exact 24-bit integer channels for oct normals, IDs and hit/lobe/coverage.
static const uint kDn2Features = 30u;
static const uint kDn2Control = 31u;
#ifdef __spirv__
#ifdef RT_DN_OUT_UNORM8
[[vk::image_format("rgba8")]]
#else
[[vk::image_format("rgba16f")]]
#endif
#endif
RWTexture2D<float4> g_dn2_out : register(u0, space5);
#ifdef RT_DN_PASS_TEMPORAL
#ifdef __spirv__
[[vk::image_format("rgba32f")]]
#endif
RWTexture2D<float4> g_dn2_features : register(u1, space5);
#endif

float2 Dn2Oct(float3 n) {
  n /= abs(n.x) + abs(n.y) + abs(n.z);
  return n.z >= 0.0f ? n.xy : (1.0f - abs(n.yx)) * float2(n.x < 0.0f ? -1.0f : 1.0f, n.y < 0.0f ? -1.0f : 1.0f);
}

float3 Dn2Normal(float packed) {
  const uint bits = uint(packed);
  const float2 oct = float2(bits & 127u, (bits >> 7u) & 127u) * (2.0f / 127.0f) - 1.0f;
  float3 n = float3(oct, 1.0f - abs(oct.x) - abs(oct.y));
  n.xy += max(-n.z, 0.0f) * float2(n.x < 0.0f ? 1.0f : -1.0f, n.y < 0.0f ? 1.0f : -1.0f);
  return normalize(n);
}

uint Dn2Id(float packed) { return uint(packed) >> 14u; }
float Dn2Hit(float packed) { return exp2(float(uint(packed) & 4095u) * (1.0f / 320.0f)) - 1.0f; }
float Dn2Rough(float packed) { return float((uint(packed) >> 12u) & 63u) / 63.0f; }
float Dn2Share(float packed) { return float(uint(packed) >> 18u) / 63.0f; }

float4 Dn2Feature(RtDnPixel c, uint surface, float variance) {
  const uint2 oct = uint2(round(saturate(Dn2Oct(c.n) * 0.5f + 0.5f) * 127.0f));
  const uint h = uint(round(clamp(log2(1.0f + c.hit) * 320.0f, 0.0f, 4095.0f)));
  const uint r = uint(round(saturate(c.rough) * 63.0f));
  const uint s = uint(round(saturate(c.share) * 63.0f));
  return float4(c.z, float(oct.x | (oct.y << 7u) | (surface << 14u)), float(h | (r << 12u) | (s << 18u)), variance);
}

float4 Dn2ReadFeature(int2 p) { return g_textures[Dn2W(kDn2Features)].Load(int3(p, 0)); }
float4 Dn2ReadHistory(int2 p) { return g_textures[Dn2W(kDn2Control) & 65535u].Load(int3(p, 0)); }

float Dn2Samples(float a) {
#ifdef RT_DN_OUT_UNORM8
  return RtDnDecodeLength(a);
#else
  return a;
#endif
}

float Dn2StoreSamples(float samples) {
#ifdef RT_DN_OUT_UNORM8
  return RtDnEncodeLength(samples);
#else
  return samples;
#endif
}

float3 Dn2ClipSpace(float3 c) {
#ifdef RT_DN_VIS
  // The channels are unrelated visibility signals.
  return c;
#else
  return Dn2YCoCg(c);
#endif
}

float3 Dn2FromClip(float3 c) {
#ifdef RT_DN_VIS
  return saturate(c);
#else
  return Dn2Rgb(c);
#endif
}

float Dn2Square(float3 delta) {
#ifdef RT_DN_VIS
  return dot(delta, delta) / 3.0f;
#else
  const float3 yc = Dn2YCoCg(delta);
  return dot(yc * yc, float3(1.0f, 0.25f, 0.25f));
#endif
}

float Dn2Magnitude(float3 c) { return max(abs(c.x), max(abs(c.y), abs(c.z))); }
float3 Dn2Safe(float3 c) { return clamp(RtDnSane(c), -32752.0f, 32752.0f); }

bool Dn2Valid(RtDnPixel c) {
  return RtDnGuidesOk(c) && c.z > 0.0f && dot(c.n, c.n) > 0.25f && c.hit >= 0.0f;
}

float Dn2HitWeight(float centre, float tap, float rough) {
#if RT_DN_PROFILE == RT_DN_PROFILE_SPECULAR
  if (centre >= 4000.0f && tap >= 4000.0f) return 1.0f;
  // One noisy lobe sample can miss a reflection entirely. Let its neighbours
  // contribute at reduced weight, without treating a distant miss as a hit.
  if ((centre >= 4000.0f) != (tap >= 4000.0f)) return lerp(0.28f, 0.68f, rough);
  return RtDnHitWeight(centre, tap, lerp(0.55f, 1.35f, rough));
#else
  return 1.0f;
#endif
}

#ifdef RT_DN_PASS_TEMPORAL
struct Dn2History {
  float3 colour;
  float samples;
  float variance;
  float weight;
};

Dn2History Dn2Gather(float2 pixel, float2 lens, float3 origin, float3 normal, uint surface, float hit, float rough, bool reflection) {
  Dn2History h = (Dn2History)0;
  if (!Dn2InImage(pixel)) return h;
  const int2 base = int2(floor(pixel));
  const float2 f = frac(pixel);
  [unroll] for (uint i = 0; i < 4; ++i) {
    const int2 q = base + int2(i & 1u, i >> 1u);
    if (any(q < 0) || any(q >= RtDnSize())) continue;
    const float4 guide = Dn2ReadFeature(q);
    if (guide.x <= 0.0f) continue;
    const uint id = Dn2Id(guide.y);
    const float3 n = Dn2Normal(guide.y);
    const float cosine = dot(normal, n);
    const float plane = abs(dot(normal, Dn2Position(float2(q), guide.x, lens) - origin));
    float extra = 1.0f;
    if (reflection) {
      // The virtual image can move on the receiver. Validate its footprint
      // against the plane and hit, while the separate receiver test handles disocclusion.
      if (surface != id || plane > 0.028f * origin.z + 0.05f || cosine < 0.86f || abs(rough - Dn2Rough(guide.z)) > 0.16f) continue;
      extra = Dn2HitWeight(hit, Dn2Hit(guide.z), rough);
    } else {
#if RT_DN_PROFILE == RT_DN_PROFILE_SPECULAR
      if ((surface & 31u) != (id & 31u)) continue;
#else
      if (surface != id) continue;
#endif
      if (!Dn2AcceptGeometry(guide.x, origin.z, plane, cosine)) continue;
    }
    const float4 value = Dn2ReadHistory(q);
    const float samples = Dn2Samples(value.a);
    if (!(samples > 0.0f) || !RtDnIsNumber3(value.rgb) || !RtDnIsNumber(guide.w)) continue;
    const float w = ((i & 1u) ? f.x : 1.0f - f.x) * ((i & 2u) ? f.y : 1.0f - f.y) * extra;
    const float combined = h.weight + w;
    const float mix = w / max(combined, 1.0e-6f);
    h.variance = Dn2Variance(h.variance, guide.w, Dn2Square(value.rgb - h.colour), 1.0f - mix);
    h.colour = lerp(h.colour, value.rgb, mix);
    h.samples = lerp(h.samples, samples, mix);
    h.weight = combined;
  }
  return h;
}

void Dn2Temporal(uint3 id) {
  const int2 p = int2(id.xy);
  if (any(p >= RtDnSize())) return;
  RtDnPixel c = RtDnLoad(p);
  const float3 raw = Dn2Safe(RtDnSignal(p).rgb);
  if (!Dn2Valid(c)) {
    g_dn2_out[p] = float4(raw, 0.0f);
    g_dn2_features[p] = 0.0f;
    return;
  }
  c.n = normalize(c.n);
  const uint surface = Dn2SurfaceId(c, p);
  float3 sum = 0.0f, second = 0.0f;
  float3 low = 32752.0f, high = -32752.0f;
  float count = 0.0f;
  float energy = 0.0f, energy2 = 0.0f, peak = 0.0f;
  float hit = c.hit;
  [loop] for (int y = -1; y <= 1; ++y) {
    [loop] for (int x = -1; x <= 1; ++x) {
      const int2 q = p + int2(x, y);
      if ((x == 0 && y == 0) || any(q < 0) || any(q >= RtDnSize())) continue;
      const RtDnPixel t = RtDnLoad(q);
      if (!Dn2Valid(t) || !RtDnIsNumber3(t.colour) || surface != Dn2SurfaceId(t, q) ||
          !Dn2AcceptGeometry(t.z, c.z, abs(dot(c.n, t.pos - c.pos)), dot(c.n, t.n))) continue;
      const float3 value = Dn2Safe(t.colour);
      const float3 clip = Dn2ClipSpace(value);
      sum += clip;
      second += clip * clip;
      low = min(low, clip);
      high = max(high, clip);
      const float magnitude = Dn2Magnitude(value);
      energy += magnitude;
      energy2 += magnitude * magnitude;
      peak = max(peak, magnitude);
      count += 1.0f;
#if RT_DN_PROFILE == RT_DN_PROFILE_SPECULAR
      if (c.hit >= 4000.0f && t.hit > 0.0f) hit = min(hit, t.hit);
#endif
    }
  }
  float3 current = Dn2Safe(c.colour);
#if !defined(RT_DN_VIS) && RT_DN_PROFILE != RT_DN_PROFILE_AMBIENT
  if (count >= 4.0f) {
    const float mean = energy / count;
    const float sigma = sqrt(max(energy2 / count - mean * mean, 0.0f));
    current *= min(1.0f, Dn2FireflyCeiling(mean, sigma, peak) / max(Dn2Magnitude(current), 1.0e-5f));
  }
#endif
  const float3 clip = Dn2ClipSpace(current);
  const float3 mean = (sum + clip) / (count + 1.0f);
  const float3 spread = max((second + clip * clip) / (count + 1.0f) - mean * mean, 0.0f);
#ifdef RT_DN_VIS
  const float spatialVariance = dot(spread, float3(0.333333f, 0.333333f, 0.333333f));
#else
  const float spatialVariance = dot(spread, float3(1.0f, 0.25f, 0.25f));
#endif
  Dn2History h = (Dn2History)0;
  float reliability = 1.0f;
  float cap = 1.0f;
  if ((Dn2W(kDn2Control) & (1u << 19u)) != 0u) {
    const Dn2Motion motion = Dn2ReceiverMotion(surface);
    const float2 lens = Dn2PreviousLens();
    const float3 origin = Dn2Transform(motion, c.pos, 1.0f);
    const float3 n = Dn2Transform(motion, c.n, 0.0f);
    if (motion.valid && origin.z > 1.0e-3f && all(lens > 0.0f) && dot(n, n) > 1.0e-6f) {
      const float2 receiver = Dn2Project(origin, lens);
      const float3 normal = normalize(n);
      h = Dn2Gather(receiver, lens, origin, normal, surface, hit, c.rough, false);
      float2 image = receiver;
#if RT_DN_PROFILE == RT_DN_PROFILE_SPECULAR
      image = Dn2ReflectionPixel(motion, c, surface, hit, lens, receiver, reliability);
      const float receiverWeight = h.weight;
      h = (Dn2History)0;
      if (receiverWeight >= 0.25f) h = Dn2Gather(image, lens, origin, normal, surface, hit, c.rough, true);
      h.weight = min(h.weight, receiverWeight);
#endif
      cap = Dn2HistoryCap(length(image - float2(p)));
    }
  }
  const float3 sigma = sqrt(spread);
  const float3 a = max(min(low, clip), mean - 1.8f * sigma);
  const float3 b = min(max(high, clip), mean + 1.8f * sigma);
  const float3 clipped = Dn2FromClip(RtDnClipToBox(Dn2ClipSpace(h.colour), a, b));
  const float change = sqrt(Dn2Square(h.colour - clipped));
  const float tolerance = 2.0f * sqrt(spatialVariance) + 0.015f + 0.08f * Dn2Magnitude(current);
  const float consistent = 1.0f / (1.0f + change / tolerance);
  const float prior = h.weight >= 0.20f ? min(h.samples, cap - 1.0f) * min(1.0f, 1.8f * h.weight) * reliability * consistent : 0.0f;
  const float keep = prior / (1.0f + prior);
  const float samples = 1.0f + prior;
  const float temporalVariance = Dn2Variance(h.variance, 0.0f, Dn2Square(h.colour - current), keep);
  const float variance = lerp(max(temporalVariance, spatialVariance), temporalVariance, saturate((samples - 2.0f) / 4.0f));
  float3 result = lerp(current, clipped, keep);
#ifdef RT_DN_OUT_UNORM8
  const float3 dither = RtDnDither3(p);
  result = float3(RtDnStore8(result.x, dither.x), RtDnStore8(result.y, dither.y), RtDnStore8(result.z, dither.z));
#endif
  c.hit = hit;
  g_dn2_out[p] = float4(result, Dn2StoreSamples(samples));
  g_dn2_features[p] = Dn2Feature(c, surface, variance);
}
#else
float Dn2SpatialRadius(float4 feature, float rough, float samples, float deviation, float3 colour) {
#if RT_DN_PROFILE == RT_DN_PROFILE_SPECULAR
  const float pixelAngle = 2.0f * Dn2Lens().y / float(RtDnSize().y);
  const float base = clamp(1.35f + 2.0f * RtDnLobePixels(rough * rough, Dn2Hit(feature.z), feature.x, pixelAngle), 1.35f, 22.0f);
#elif defined(RT_DN_VIS)
  const float base = 8.0f;
#elif RT_DN_PROFILE == RT_DN_PROFILE_AMBIENT
  const float base = Dn2Id(feature.y) == 0 ? 10.0f : 4.0f;
#else
  const float base = 21.0f;
#endif
  return Dn2Radius(base, samples, deviation / (0.045f + Dn2Magnitude(colour)));
}

void Dn2Spatial(uint3 id) {
  const int2 p = int2(id.xy);
  if (any(p >= RtDnSize())) return;
  const uint control = Dn2W(kDn2Control);
  const uint iteration = (control >> 16u) & 7u;
  const bool final = (control & (1u << 20u)) != 0;
  const float4 guide = Dn2ReadFeature(p);
  const float4 signal = RtDnSignal(p);
  const float4 temporal = iteration == 0 ? signal : Dn2ReadHistory(p);
  const float samples = Dn2Samples(temporal.a);
  const float rough = Dn2Rough(guide.z);
  const float share = Dn2Share(guide.z);
  const float step = float(1u << iteration);
  float deviation = iteration == 0 ? sqrt(guide.w) : signal.a;
  float3 value = signal.rgb;
  if (guide.x > 0.0f && share > 0.0f && guide.w > 1.0e-8f && step * 0.8f <= Dn2SpatialRadius(guide, rough, samples, sqrt(guide.w), temporal.rgb)) {
    const float2 lens = Dn2Lens();
    const float3 n = Dn2Normal(guide.y);
    const float3 pos = Dn2Position(float2(p), guide.x, lens);
    const uint surface = Dn2Id(guide.y);
    const float hit = Dn2Hit(guide.z);
    float total = 4.0f;
    float3 sum = 4.0f * value;
    float variance = 16.0f * deviation * deviation;
    [loop] for (int y = -1; y <= 1; ++y) {
      [loop] for (int x = -1; x <= 1; ++x) {
        const int2 q = p + int2(x, y) * int(step);
        if ((x == 0 && y == 0) || any(q < 0) || any(q >= RtDnSize())) continue;
        const float4 tap = Dn2ReadFeature(q);
        if (tap.x <= 0.0f || surface != Dn2Id(tap.y)) continue;
        const float plane = abs(dot(n, Dn2Position(float2(q), tap.x, lens) - pos));
        const float cosine = dot(n, Dn2Normal(tap.y));
        if (plane > 0.025f * guide.x + 0.04f || cosine < 0.72f) continue;
        const float4 t = RtDnSignal(q);
        const float d = iteration == 0 ? sqrt(tap.w) : t.a;
        const float floor = 0.006f + 0.06f * min(Dn2Magnitude(value), Dn2Magnitude(t.rgb));
        const float width = 2.8f * max(deviation, d) + floor;
        const float colourWeight = exp2(-sqrt(Dn2Square(value - t.rgb)) / width);
        const float geometry = RtDnDepthWeight(plane, guide.x, 0.008f) * RtDnNormalWeight(cosine, 24.0f);
        const float specular = Dn2HitWeight(hit, Dn2Hit(tap.z), rough) * RtDnRoughWeight(rough, Dn2Rough(tap.z), 0.09f);
        const float w = (x == 0 || y == 0 ? 2.0f : 1.0f) * colourWeight * geometry * specular;
        sum += t.rgb * w;
        total += w;
        variance += w * w * d * d;
      }
    }
    value = sum / total;
    deviation = sqrt(variance) / total;
  }
  g_dn2_out[p] = final ? Dn2Composite(p, lerp(temporal.rgb, value, share), guide.x, Dn2StoreSamples(samples)) : float4(value, min(deviation, 65504.0f));
}
#endif
