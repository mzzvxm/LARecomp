// LARecomp shader pack builder -- variant "shadow_filter": the sun shadow collector's soft lookup and its
// contact hardening (see shadow_filter.h).

#include "shaderpack/variants/shadow_filter.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "shaderpack/pack_format.h"
#include "shaderpack/variants/stage_constants.h"
#include "shaderpack/variants/text_edit.h"

namespace larecomp::shaderpack {

using namespace textedit;

namespace {

// The game's lookup of the atlas: a texture fetch at the sampler of the shadow map's slot, and what follows it
// on a line of the translator's text. The four taps of the rotated cross each have exactly this form.
constexpr std::string_view kLookupPrefix =
    "tfetch2D(DepthTextureSampler0_Texture2DDescriptorIndex, DepthTextureSampler0_SamplerDescriptorIndex, ";
constexpr std::string_view kLookupSuffix = ", float2(0, 0)).x;";
// The shaders with a filter of their own (the car paint, the skinned metal, the glass) weigh a 2x2 block.
constexpr std::string_view kOwnFilterMarker = "getWeights2D(DepthTextureSampler0_Texture2DDescriptorIndex";
// The constants that make the four taps a cross of radii 1, 0.75, 0.5, 0.25 x count / 1280: c255.xyzw =
// (0.75, 1, 0.5, 0.25) / 1280 (the same line in every shader that has the cross), and the 0.25 of the average
// (c253.y; the register is named c253 in all of them).
constexpr std::string_view kRadiiLine =
    "float4 c255 = asfloat(uint4(0x3A19999A, 0x3A4CCCCD, 0x39CCCCCD, 0x394CCCCD));";
constexpr std::string_view kQuarterConstant = "0x3E800000";

// The helper and its constants, before main. g_ShadowFilter is declared by the two edits below it.
constexpr std::string_view kHelper = R"HLSL(// LARecomp variant shadow_filter (src/shaderpack/variants/shadow_filter.cpp): the sun shadow lookup with N taps
// on a Vogel disk instead of the game's four. g_ShadowFilter (16 bytes after the shared constants): x the number
// of taps, y the radius scale (1: the game's radius), z the noise frame (the R2 shift of a temporal upscaler's
// frame number), w the mode bits (1: true PCF taps, each a bilinear weighted compare of four texels taken at the
// receiver's own plane, instead of the sampler's own bilinear depth; 2: contact hardening, a blocker search first;
// 4: the disk is turned per pixel by noise, which only a temporal upscaler's history can average out: without
// the bit every pixel takes the same disk, so there is no grain; 32: that noise is the blue noise tile stepped along the R2 sequence, not the first version's gradient noise (a lattice); bits 8 to 15: the sun's size for the contact
// hardening, in sixteenths of the real sun's).
static const float kShadowFilterRadiusGain = 1.05;     // the weighted disk is as wide as the game's four taps
static const float kShadowFilterGoldenCos = -0.7373688780783197;  // cos and sin of the golden angle, 137.5 degrees
static const float kShadowFilterGoldenSin = 0.6754902942615238;
static const float kShadowFilterSunTan = 0.0047123890;  // tan of the sun's angular radius, 0.27 degrees
static const float kShadowFilterMinTexels = 0.75;       // contact hardening: the smallest kernel radius, in atlas texels
static const float kShadowFilterMaxRadii = 2.0;         // and the largest, in radii of the soft filter (the search covers it)
static const int kShadowFilterSearchTaps = 8;
static const float kShadowFilterFixedAngle = 2.399963;  // the disk's turn without noise (a radian count that no tap row lines up with)
static const float kShadowFilterMaxSlope = 8.0;         // the steepest receiver plane believed: 8 metres of depth per metre across

// The blue noise tile (src/gpu/blue_noise.h, hlsl/blue_noise.hlsli: 32 x 32 pixels, one byte a pixel, four to a word, pixel 4 w in the low byte; written by larecomp_blue_noise_test --emit,
// and the same words are checked against it by larecomp_shadow_filter_noise_test).
static const uint kShadowFilterTile[256] = {
    0x60183DD2u, 0x7AF84DBCu, 0xA1FE8CDBu, 0xAE16D46Eu, 0x6E143F75u, 0xE6A5094Au, 0xABDE0AC3u, 0x59E76D45u,
    0xE3A0F990u, 0x1A6A9906u, 0x1F4A30AEu, 0x296633E9u, 0xC0E597F7u, 0x217639DAu, 0x89374F64u, 0x20A809C5u,
    0x45286E4Eu, 0xC238D77Du, 0xBE73CF59u, 0xC1E19C89u, 0x2A5D034Bu, 0x92FDAF8Au, 0x15F6B9D4u, 0xBF80D55Du,
    0xAC8ACE0Du, 0x86F024C9u, 0x06F2980Fu, 0x820F583Cu, 0xB77CA5D4u, 0x044C6210u, 0x9F6A832Du, 0xF33C27E9u,
    0x12EF34A5u, 0x43A56550u, 0xAA5F29E6u, 0x24B5FACCu, 0x47FA356Au, 0xE59FC6E0u, 0xB61C42B1u, 0x629A774Bu,
    0xB75B76DFu, 0xBA0191E3u, 0x7F48D775u, 0x94437319u, 0x8C1ABFECu, 0x79361771u, 0x87CEF25Fu, 0x19BCD901u,
    0x3B8705C5u, 0x57F86F21u, 0xE394B21Eu, 0x5CD09F31u, 0xCF9C5014u, 0xBE8FF33Eu, 0x572A960Du, 0x528D34FCu,
    0xD3FE9A41u, 0xCE34C2A1u, 0xBD083A85u, 0xB002F164u, 0x2768DE7Cu, 0x24D657ACu, 0xC576DB51u, 0xF1236CA0u,
    0x12692BA8u, 0x9E0C7E49u, 0x4FFD66E8u, 0xE83F8C23u, 0xE80FA62Bu, 0xA36B0282u, 0x0BAF3EECu, 0x7CB4D844u,
    0xACCC57E3u, 0x44DC61EBu, 0x7BC4A91Au, 0xC06FDBA2u, 0x4DC93B88u, 0xCB42FABAu, 0xF0671389u, 0x0A601B8Au,
    0x387A1D8Fu, 0x8EB82292u, 0x330E5676u, 0x0B5417CBu, 0x6D90FE5Eu, 0x791D972Fu, 0x2995C231u, 0xC7EB50BEu,
    0x03F7BD3Cu, 0x2DF952D1u, 0xE196EECDu, 0x9DF4B548u, 0x08B11ED6u, 0xE2B05CD9u, 0xDC52F860u, 0x712FA478u,
    0x6B9E51DFu, 0x6D1282B3u, 0x7F2240AEu, 0x72248D69u, 0xF0447D32u, 0x0B4CC681u, 0x03B21FA0u, 0x9C13FC3Du,
    0x46278408u, 0xE5A436E7u, 0xFBBD6301u, 0xAD41D10Du, 0xA45AE4C4u, 0x8EF5152Cu, 0x688344D5u, 0x63B989CDu,
    0xD5ABC6F5u, 0x4CCA5E1Cu, 0x589BDF8Eu, 0x53EBA137u, 0xC1179300u, 0x37649D72u, 0xA6EFBB74u, 0x37D95426u,
    0x8D105A73u, 0x2398FF73u, 0xD618327Au, 0x7F1C65B4u, 0x3AEA68D3u, 0xC3E446D8u, 0x11592C05u, 0xA71977E7u,
    0x4DED2AE2u, 0xC43E0CBBu, 0x854EADF4u, 0xB896F605u, 0x87AA4929u, 0x7F1FB507u, 0x8BC79BFFu, 0x4793B336u,
    0x3195C380u, 0x56A981DDu, 0xE5C86707u, 0xDF324774u, 0x5F1DFD70u, 0xAD5591F1u, 0xDE6F3D61u, 0x0FCCFB4Bu,
    0xAF6C1CA9u, 0x71EA2261u, 0x283D96D8u, 0x135EC4A7u, 0x35BB84A2u, 0xDC2E71CBu, 0xAB20CF12u, 0x5D2D6A00u,
    0x04FA3CD1u, 0x309048CAu, 0x8EFB1BB6u, 0xCD91EA10u, 0x4ED70442u, 0x46EC0C99u, 0x56F1869Du, 0xEF87C199u,
    0x88559870u, 0xF214A3E4u, 0xC05D7D4Au, 0xF3216D4Fu, 0xF56BAA58u, 0xB282C325u, 0x79BC2867u, 0x0941DD16u,
    0x20BEE12Eu, 0x66C67739u, 0xD70AA2D0u, 0x86B63B81u, 0x7E19E22Eu, 0xD2385CA4u, 0x3448E705u, 0xB79E5AF5u,
    0xEF7B1551u, 0x8B08B25Cu, 0x2DED3F25u, 0xC706E6ACu, 0x45B89462u, 0x78F715DDu, 0x8BD6A355u, 0xD57D26B1u,
    0xA043AE8Au, 0x54FC30D4u, 0x6490B4DDu, 0x45709A1Du, 0x31D00BF9u, 0x2B94AF69u, 0x09681FBAu, 0xFE116CCAu,
    0x6C01CC61u, 0xA675911Au, 0xC84B6F16u, 0xA8D856FDu, 0x8B537922u, 0xDA4A00EEu, 0x519AFB85u, 0x35A740EDu,
    0xF697E921u, 0x39E14CB5u, 0x7C00F4BFu, 0x2F83113Au, 0x1BE89BBFu, 0x63CB7AC2u, 0x2EC13F0Eu, 0xBDDC9580u,
    0x5A39804Eu, 0x5F07C725u, 0xD99B2C88u, 0x5EEB93BCu, 0xAA673BD3u, 0xF03A972Cu, 0x17E475B4u, 0x700658D0u,
    0xDAB90EE2u, 0xEAA2788Cu, 0x62AD49D1u, 0x04B04D20u, 0x49DA187Eu, 0xA1185BFFu, 0x66A65326u, 0x8FF826B6u,
    0x136B9CC9u, 0x1E33F443u, 0x38E60D72u, 0x42CE74F7u, 0x0788B3F9u, 0x7ED2B774u, 0xF20292E0u, 0x36B07841u,
    0xABFC2347u, 0x55B963D2u, 0x8F7BC099u, 0x982BA314u, 0x9FC8546Eu, 0x690E40E0u, 0x8435C846u, 0x65169ED3u,
    0x305286ECu, 0xCD840A92u, 0xDE502AEEu, 0x0CEE5BC5u, 0x601E33E0u, 0xB1F69327u, 0xBA5BED1Bu, 0xB8DB5310u,
    0x77C9A702u, 0x40A828E9u, 0x03B36511u, 0x4FBB833Eu, 0xF3A9CF8Du, 0x3259CA85u, 0x25729D81u, 0x7B2F95FFu,
};

// The tile at a host pixel: uniform on (0, 1), no lattice and no low spatial frequencies.
float ShadowFilterTile(float2 pixel)
{
    int2 p = int2(floor(pixel));
    uint i = ((uint(p.y) & 31u) << 5u) | (uint(p.x) & 31u);
    return (float((kShadowFilterTile[i >> 2u] >> ((i & 3u) * 8u)) & 0xFFu) + 0.5) * (1.0 / 256.0);
}

// The disk's turn in turns, from a host pixel and the frame (0 to 1023). With `blue` (mode bit 5) the tile plus a step along the R2 sequence by the frame, the same for every pixel, as a 32 bit
// fraction so that the product wraps exactly; else the first version's interleaved gradient noise (a lattice) plus the R2 shift of the frame (as the ambient occlusion's).
float ShadowFilterNoise(float2 pixel, float frame, bool blue)
{
    if (blue)
    {
        return frac(ShadowFilterTile(pixel) + float(uint(frame) * 3242174889u) * (1.0 / 4294967296.0));
    }
    float gradient = frac(52.9829189 * frac(dot(floor(pixel), float2(0.06711056, 0.00583715))));
    return frac(gradient + frame * 0.75487766624669);
}

// 1 where the shadow map's depth at uv is not nearer than the receiver's (lit), else 0: the game's own compare.
// `depth` is the receiver's depth at uv. `slope` is the receiver plane's depth per uv along u and v (0: no plane, as
// the game's own taps): the Gather taps compare each of their four texels with the plane's depth at that texel, so a
// sloped receiver (the ground under a low sun) needs no extra bias to keep it from shadowing itself: the sampler's
// blended depth is a plane too, and this is the same compare texel by texel.
float ShadowFilterLookup(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 uv, float depth, bool gather,
                         float2 slope)
{
    if (gather)
    {
        Texture2D<float4> shadowMap = g_Texture2DDescriptorHeap[TFETCH_INDEX(resourceDescriptorIndex)];
        float2 dimensions = float2(getTexture2DDimensions(shadowMap));
        float2 weight = frac(uv * dimensions - 0.5);
        float4 stored = shadowMap.GatherRed(g_SamplerDescriptorHeap[samplerDescriptorIndex], uv);
        // Gather returns (x, y, z, w) = the texels (0, 1), (1, 1), (1, 0), (0, 0) from the lower left one: their
        // centres are (0, 1, 1, 0) - weight.x and (1, 1, 0, 0) - weight.y texels from the tap.
        float2 perTexel = slope / dimensions;
        float4 receiver = depth + perTexel.x * (float4(0.0, 1.0, 1.0, 0.0) - weight.x) +
                          perTexel.y * (float4(1.0, 1.0, 0.0, 0.0) - weight.y);
        float4 lit = stored >= receiver;
        return lerp(lerp(lit.w, lit.z, weight.x), lerp(lit.x, lit.y, weight.x), weight.y);
    }
    float lit = tfetch2DLod(resourceDescriptorIndex, samplerDescriptorIndex, uv, float2(0.0, 0.0), 0.0).x >= depth;
    return lit;
}

// The receiver's plane in the atlas: the depth's change per uv along u and along v, from how the pixel's own centre
// and depth change over the screen (a pixel's neighbours in its 2 x 2 block: the receiver is one surface there, or
// the pixels that straddle two give a wild number that the limit below holds down). The limit is the steepest
// slope that is worth believing: a receiver at kShadowFilterMaxSlope times the depth per metre over the metres per
// uv of the cascade (the light matrix's rows, as ShadowFilterPenumbra reads them).
float2 ShadowFilterPlane(float2 centre, float depth, int base)
{
    float2 uvX = ddx(centre);
    float2 uvY = ddy(centre);
    float2 depthXY = float2(ddx(depth), ddy(depth));
    float determinant = uvX.x * uvY.y - uvX.y * uvY.x;
    if (!(abs(determinant) > 1.0e-14))
        return float2(0.0, 0.0);
    float2 slope = float2(depthXY.x * uvY.y - uvX.y * depthXY.y, uvX.x * depthXY.y - uvY.x * depthXY.x) / determinant;
    float4 row0 = gLightMatrixArr(0 + base);
    float4 row1 = gLightMatrixArr(1 + base);
    float4 row2 = gLightMatrixArr(2 + base);
    float4 row3 = gLightMatrixArr(3 + base);
    float2 uvPerMetre = max(float2(length(float3(row0.x, row1.x, row2.x)), length(float3(row0.y, row1.y, row2.y))),
                            float2(1.0e-9, 1.0e-9));
    float depthPerMetre = max(length(float3(row0.z, row1.z, row2.z)) * row3.w, 1.0e-9);
    float2 limit = kShadowFilterMaxSlope * depthPerMetre / uvPerMetre;
    return clamp(slope, -limit, limit);
}

// Contact hardening: the half widths, along the atlas's u and v, of the kernel that a receiver needs when the
// blockers the search found lie `gap` (a difference of atlas depths) nearer to the sun. The sun's disc, seen
// from the receiver through the blockers' plane, is a disc of radius (the distance to the blockers) x tan(the
// sun's angular radius): that is the radius of the PCF's disc in the world. The cascade's light matrix turns it
// into the atlas: its rows base + 0 to 2 hold the gradients of u, v and the depth along the world's x, y and z
// (at right angles to one another, a rotation and a scale), so their lengths are the atlas uv per metre across
// the sun's rays (u and v: the texels are taller than wide) and the depth per metre along them (row 3's w is
// the factor the receiver's depth is multiplied by). The kernel is never narrower than minTexels atlas texels
// (the map cannot show a sharper edge) nor wider than maxRadius (the soft filter's twice).
float2 ShadowFilterPenumbra(int base, float gap, float sunTan, float texel, float maxRadius)
{
    float4 row0 = gLightMatrixArr(0 + base);
    float4 row1 = gLightMatrixArr(1 + base);
    float4 row2 = gLightMatrixArr(2 + base);
    float4 row3 = gLightMatrixArr(3 + base);
    float uPerMetre = length(float3(row0.x, row1.x, row2.x));
    float vPerMetre = length(float3(row0.y, row1.y, row2.y));
    float depthPerMetre = max(length(float3(row0.z, row1.z, row2.z)) * row3.w, 1.0e-9);
    float metres = gap / depthPerMetre * sunTan;
    float lowest = kShadowFilterMinTexels * texel;
    return clamp(metres * float2(uPerMetre, vPerMetre), float2(lowest, lowest), float2(maxRadius, maxRadius));
}

// The average of the game's four taps, replaced. tap1..tap4 are the game's tap coordinates (a cross: tap 1 at
// centre + 1.0 R u, tap 3 at centre - 0.5 R u, tap 2 at centre + 0.75 R v, tap 4 at centre - 0.25 R v, u and v
// at right angles, R = count / 1280): the centre and R come from taps 1 and 3. `cascadeBase` is the game's own
// a0 (4 x the cascade's index: the first row of its light matrix).
// Mode bit 2 (contact hardening) puts a blocker search first: 8 taps over a disc of twice the soft radius, the
// bilinear sampler's depth compared with the receiver's as the game does (nearer to the sun: a blocker). None
// found: lit, all found: in shadow (no more taps for the pixels that are not near an edge). Else the kernel is the
// penumbra of the average blocker (ShadowFilterPenumbra) and the PCF below runs on it, an ellipse along u and v.
float ShadowFilterSoft(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 tap1, float2 tap2,
                       float2 tap3, float2 tap4, float depth, float2 pixel, int cascadeBase)
{
    float4 constants = g_ShadowFilter;
    uint mode = uint(constants.w + 0.5);
    float2 centre = (tap1 + 2.0 * tap3) * (1.0 / 3.0);
    float radius = length(tap1 - tap3) * (1.0 / 1.5) * constants.y * kShadowFilterRadiusGain;
    int count = clamp(int(constants.x + 0.5), 4, 32);
    bool gather = (mode & 1u) != 0u;
    // Noise turns the disk from pixel to pixel: a grain that a temporal upscaler's history averages out. Without one
    // (plain MSAA, no upscaler) nothing would, so the disk keeps its turn and the edge has none.
    float angle = (mode & 4u) != 0u ? 6.28318530718 * ShadowFilterNoise(pixel, constants.z, (mode & 32u) != 0u) : kShadowFilterFixedAngle;
    float2 direction = float2(cos(angle), sin(angle));
    float2 kernel = float2(radius, radius);
    // The true PCF taps (mode bit 1) read the shadow map at the receiver's plane; the blocker search does too.
    float2 slope = gather ? ShadowFilterPlane(centre, depth, cascadeBase) : float2(0.0, 0.0);
    if ((mode & 2u) != 0u)
    {
        float maxRadius = kShadowFilterMaxRadii * radius;
        float blockerSum = 0.0;
        int blockers = 0;
        [loop]
        for (int j = 0; j < kShadowFilterSearchTaps; ++j)
        {
            float share = (float(j) + 0.5) / float(kShadowFilterSearchTaps);
            float2 offset = direction * (sqrt(share) * maxRadius);
            float stored = tfetch2DLod(resourceDescriptorIndex, samplerDescriptorIndex,
                                       centre + offset, float2(0.0, 0.0), 0.0).x;
            if (stored < depth + dot(slope, offset))
            {
                blockerSum += stored;
                blockers += 1;
            }
            direction = float2(direction.x * kShadowFilterGoldenCos - direction.y * kShadowFilterGoldenSin,
                               direction.x * kShadowFilterGoldenSin + direction.y * kShadowFilterGoldenCos);
        }
        if (blockers == 0)
            return 1.0;
        if (blockers == kShadowFilterSearchTaps)
            return 0.0;
        float sunTan = float(mode >> 8u) * (kShadowFilterSunTan * (1.0 / 16.0));
        kernel = ShadowFilterPenumbra(cascadeBase, depth - blockerSum / float(blockers), sunTan,
                                      constants.y * (1.0 / 1280.0), maxRadius);
    }
    float sum = 0.0;
    float total = 0.0;
    [loop]
    for (int i = 0; i < count; ++i)
    {
        float share = (float(i) + 0.5) / float(count);  // r^2: equal areas
        float weight = 1.0 - 0.5 * share;
        float2 offset = direction * (sqrt(share) * kernel);
        sum += weight * ShadowFilterLookup(resourceDescriptorIndex, samplerDescriptorIndex, centre + offset,
                                           depth + dot(slope, offset), gather, slope);
        total += weight;
        direction = float2(direction.x * kShadowFilterGoldenCos - direction.y * kShadowFilterGoldenSin,
                           direction.x * kShadowFilterGoldenSin + direction.y * kShadowFilterGoldenCos);
    }
    return sum / total;
}
)HLSL";

// ---------------------------------------------------------------------------
// Reading the translator's lines
// ---------------------------------------------------------------------------

// "r12.x": a register and what is picked of it. Nothing else counts (no spaces, no minus, no function).
struct Operand {
  std::string reg;      // "r12"
  std::string swizzle;  // "x", "xw", "xyzw"
};

bool ParseOperand(std::string_view text, Operand* out) {
  const size_t dot = text.find('.');
  if (dot == std::string_view::npos || dot < 2 || dot + 1 >= text.size() || text[0] != 'r') return false;
  for (size_t i = 1; i < dot; ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
  }
  for (size_t i = dot + 1; i < text.size(); ++i) {
    if (text[i] != 'x' && text[i] != 'y' && text[i] != 'z' && text[i] != 'w') return false;
  }
  out->reg = std::string(text.substr(0, dot));
  out->swizzle = std::string(text.substr(dot + 1));
  return true;
}

struct LineAt {
  size_t begin = 0;  // of the line, at its first tab
  size_t end = 0;    // at the '\n' after it (or the end of the text)
  std::string_view indent;
  std::string_view code;  // after the indentation, up to the line's end
};

LineAt LineOf(const std::string& text, size_t position) {
  LineAt line;
  const size_t newline_before = text.rfind('\n', position == 0 ? 0 : position - 1);
  line.begin = (newline_before == std::string::npos || position == 0) ? 0 : newline_before + 1;
  const size_t newline_after = text.find('\n', position);
  line.end = newline_after == std::string::npos ? text.size() : newline_after;
  const std::string_view all = std::string_view(text).substr(line.begin, line.end - line.begin);
  size_t blanks = 0;
  while (blanks < all.size() && (all[blanks] == '\t' || all[blanks] == ' ')) ++blanks;
  line.indent = all.substr(0, blanks);
  line.code = all.substr(blanks);
  return line;
}

// One of the four fetches: `<dest> = tfetch2D(<atlas>, <sampler>, <coordinate>, float2(0, 0)).x;`
struct Tap {
  LineAt line;
  Operand dest;
  Operand coordinate;
};

bool ParseTap(const LineAt& line, Tap* tap) {
  tap->line = line;
  const std::string_view code = line.code;
  const size_t equals = code.find(" = ");
  if (equals == std::string_view::npos || !ParseOperand(code.substr(0, equals), &tap->dest)) return false;
  std::string_view rest = code.substr(equals + 3);
  if (!rest.starts_with(kLookupPrefix) || !rest.ends_with(kLookupSuffix)) return false;
  rest.remove_prefix(kLookupPrefix.size());
  rest.remove_suffix(kLookupSuffix.size());
  return ParseOperand(rest, &tap->coordinate);
}

// The second of the comma separated words of `float4 <name> = asfloat(uint4(a, b, c, d));`, the line that
// starts at `at`; empty when the line is not that.
std::string_view SecondConstantWord(std::string_view line) {
  const size_t open = line.find("uint4(");
  if (open == std::string_view::npos) return {};
  const size_t first = line.find(',', open);
  if (first == std::string_view::npos) return {};
  const size_t second = line.find(',', first + 1);
  if (second == std::string_view::npos) return {};
  std::string_view word = line.substr(first + 1, second - first - 1);
  while (!word.empty() && IsSpace(word.front())) word.remove_prefix(1);
  while (!word.empty() && IsSpace(word.back())) word.remove_suffix(1);
  return word;
}

// One edit at an offset of the text, made in decreasing offset order so that none moves another.
struct Insertion {
  size_t at = 0;
  size_t remove = 0;  // characters taken out at `at` before `text` goes in
  std::string text;
};

VariantTransformResult Failed(const VariantShader& shader, std::string error) {
  VariantTransformResult result;
  result.error = std::format("{}: {}", shader.name, error);
  return result;
}

VariantTransformResult Skipped(std::string_view why) {
  VariantTransformResult result;
  result.skip = true;
  result.error = std::string(why);
  return result;
}

}  // namespace

VariantTransformResult TransformShadowFilterHlsl(const VariantShader& shader, std::string_view hlsl, const StageContext& ctx) {
  std::string text = NormalizeNewlines(hlsl);

  // 0. What kind of lookup it has: none, its own, or the rotated cross these edits are for.
  const size_t lookups = CountOccurrences(text, kLookupPrefix);
  if (lookups == 0) return Skipped(kShadowSkipNoLookup);
  if (text.find(kOwnFilterMarker) != std::string::npos) return Skipped(kShadowSkipOwnFilter);
  if (lookups != 4) {
    return Failed(shader, std::format("the atlas is read {} times (the rotated cross has 4 taps): the HLSL tree is "
                                      "not the one this transform was written for", lookups));
  }
  if (CountOccurrences(text, kRadiiLine) != 1) {
    return Failed(shader, "the line of the tap radii (c255) is not there exactly once: the HLSL tree is not the one "
                          "this transform was written for");
  }

  // 0b. What contact hardening reads: the cascade's light matrix, by the game's own address register
  // (`gLightMatrixArr(k + a0)`, k = 0 to 3: the gradients of u, v and the depth, and the offset).
  if (CountOccurrences(text, "int a0 = 0;") != 1) {
    return Failed(shader, "the address register `int a0 = 0;` is not declared exactly once: the HLSL tree is not "
                          "the one this transform was written for");
  }
  for (int row = 0; row < 4; ++row) {
    const std::string needle = std::format("gLightMatrixArr({} + a0)", row);
    if (CountOccurrences(text, needle) < 1) {
      return Failed(shader, std::format("`{}` is not read: the HLSL tree is not the one this transform was written "
                                        "for", needle));
    }
  }

  // 1. The four taps: each on a line of its own, in the order of the cross (coordinates .xw, .yz of one
  // register, .xy, .zw of another), into the four components of one register.
  std::vector<Tap> taps;
  for (size_t at = text.find(kLookupPrefix); at != std::string::npos; at = text.find(kLookupPrefix, at + 1)) {
    Tap tap;
    if (!ParseTap(LineOf(text, at), &tap)) {
      return Failed(shader, "a fetch of the atlas is not `rN.c = tfetch2D(atlas, sampler, rM.ab, float2(0, 0)).x;`: "
                            "the HLSL tree is not the one this transform was written for");
    }
    taps.push_back(std::move(tap));
  }
  static constexpr std::string_view kDestinations[4] = {"x", "y", "z", "w"};
  static constexpr std::string_view kCoordinates[4] = {"xw", "yz", "xy", "zw"};
  for (size_t i = 0; i < 4; ++i) {
    if (taps[i].dest.reg != taps[0].dest.reg || taps[i].dest.swizzle != kDestinations[i] ||
        taps[i].coordinate.swizzle != kCoordinates[i]) {
      return Failed(shader, std::format("tap {} is not the cross's (into {}.{}, from .{}): the HLSL tree is not "
                                        "the one this transform was written for",
                                        i + 1, taps[0].dest.reg, kDestinations[i], kCoordinates[i]));
    }
  }
  const std::string fetched = taps[0].dest.reg;

  // 2. The compare of the four with the receiver's depth, once, after the last tap: `rA.xyzw = rF.xyzw >=
  // rD.xxxx;`, and the average of its result with 0.25, once after that: `rE.x = dot(rA.wzxy, c253.yyyy);`.
  const std::string compare_needle = fetched + ".xyzw >= ";
  const size_t after_taps = taps[3].line.end;
  const std::string_view tail = std::string_view(text).substr(after_taps);
  if (CountOccurrences(tail, compare_needle) != 1) {
    return Failed(shader, std::format("`{}` is not there exactly once after the last tap: the HLSL tree is not the "
                                      "one this transform was written for", compare_needle));
  }
  const size_t compare_at = after_taps + tail.find(compare_needle);
  const LineAt compare_line = LineOf(text, compare_at);
  Operand compare_dest;
  std::string depth_reg;  // the register that holds the receiver's depth in .x
  {
    const std::string_view code = compare_line.code;
    const size_t equals = code.find(" = ");
    const size_t greater = code.find(" >= ");
    std::string_view depth_text;
    if (greater != std::string_view::npos) depth_text = code.substr(greater + 4);
    Operand probe;
    const bool shaped = equals != std::string_view::npos && greater != std::string_view::npos && greater > equals &&
                        depth_text.ends_with(".xxxx;") &&
                        ParseOperand(code.substr(0, equals), &compare_dest) && compare_dest.swizzle == "xyzw" &&
                        code.substr(equals + 3, greater - equals - 3) == fetched + ".xyzw" &&
                        ParseOperand(std::string(depth_text.substr(0, depth_text.size() - 6)) + ".x", &probe);
    if (!shaped) {
      return Failed(shader, "the compare of the four taps is not `rA.xyzw = rF.xyzw >= rD.xxxx;`: the HLSL tree is "
                            "not the one this transform was written for");
    }
    depth_reg = probe.reg;
  }
  const std::string average_needle = "dot(" + compare_dest.reg + ".wzxy, c253.yyyy);";
  const std::string_view after_compare = std::string_view(text).substr(compare_line.end);
  if (CountOccurrences(after_compare, average_needle) != 1) {
    return Failed(shader, std::format("`{}` is not there exactly once after the compare: the HLSL tree is not the "
                                      "one this transform was written for", average_needle));
  }
  const size_t average_at = compare_line.end + after_compare.find(average_needle);
  const LineAt average_line = LineOf(text, average_at);
  Operand average_dest;
  {
    const size_t equals = average_line.code.find(" = ");
    if (equals == std::string_view::npos || !ParseOperand(average_line.code.substr(0, equals), &average_dest) ||
        average_dest.swizzle != "x" || average_line.code.substr(equals + 3) != average_needle) {
      return Failed(shader, "the average of the four taps is not `rE.x = dot(rA.wzxy, c253.yyyy);`: the HLSL tree is "
                            "not the one this transform was written for");
    }
  }

  // 3. c253.y must be the 0.25 the average is made with.
  {
    const size_t at = text.find("float4 c253 = asfloat(uint4(");
    if (at == std::string::npos || SecondConstantWord(LineOf(text, at).code) != kQuarterConstant) {
      return Failed(shader, "c253.y is not 0.25 (0x3E800000): the HLSL tree is not the one this transform was "
                            "written for");
    }
  }

  // 4. The edits at offsets: the average becomes the helper's call, the taps and the depth are copied just
  // before the lines that overwrite or read them. In decreasing order of position.
  std::vector<Insertion> edits;
  {
    std::string call = std::string(average_line.indent) + average_dest.reg + ".x = ShadowFilterSoft(" +
                       "DepthTextureSampler0_Texture2DDescriptorIndex, DepthTextureSampler0_SamplerDescriptorIndex, "
                       "shadowTap1, shadowTap2, shadowTap3, shadowTap4, shadowDepth, iPos.xy, a0);";
    edits.push_back({average_line.begin, average_line.end - average_line.begin, std::move(call)});
    edits.push_back({compare_line.begin, 0,
                     std::string(compare_line.indent) + "shadowDepth = " + depth_reg + ".x;\n"});
    for (size_t i = 0; i < 4; ++i) {
      edits.push_back({taps[i].line.begin, 0,
                       std::format("{}shadowTap{} = {}.{};\n", taps[i].line.indent, i + 1, taps[i].coordinate.reg,
                                   taps[i].coordinate.swizzle)});
    }
  }
  std::sort(edits.begin(), edits.end(), [](const Insertion& a, const Insertion& b) { return a.at > b.at; });
  for (size_t i = 1; i < edits.size(); ++i) {
    if (edits[i].at + edits[i].remove > edits[i - 1].at) {
      return Failed(shader, "two edits of the lookup overlap: the HLSL tree is not the one this transform was "
                            "written for");
    }
  }
  for (const Insertion& edit : edits) text.replace(edit.at, edit.remove, edit.text);

  // 5. The anchored edits: the locals the copies go to, the constants and the helper.
  static constexpr Piece kCubeMap[] = {Lit("CubeMapData cubeMapData = (CubeMapData)0;")};
  static constexpr Piece kMain[] = {Lit("#ifndef __spirv__"), Ws(0), Lit("[shader(\"pixel\")]"), Ws(0),
                                    Lit("#endif"),            Ws(0), Lit("void main(")};
  std::string error;
  if (!ReplaceOnce(text, kCubeMap, 0, 0, "the start of main's registers",
                   [](std::string_view group) {
                     return std::string(group) +
                            "\n\tfloat2 shadowTap1 = 0.0, shadowTap2 = 0.0, shadowTap3 = 0.0, shadowTap4 = 0.0;"
                            "\n\tfloat shadowDepth = 0.0;";
                   },
                   &error)) {
    return Failed(shader, std::move(error));
  }
  static constexpr ConstantMember kMembers[] = {{"float4", "g_ShadowFilter"}};
  if (!DeclareConstants(text, ctx, "Shadow filter variant (src/gpu/shadow_filter.h ShadowFilterConstants): 16 bytes after the 608.", kMembers,
                        &error)) {
    return Failed(shader, std::move(error));
  }
  if (!ReplaceOnce(text, kMain, 0, 6, "pixel shader main",
                   [](std::string_view group) { return std::string(kHelper) + "\n" + std::string(group); }, &error)) {
    return Failed(shader, std::move(error));
  }

  // What the text must hold now: the helper's definition and its one call, one read of g_ShadowFilter (its declarations are
  // checked by DeclareConstants), and the four copies of the taps and the one of the depth.
  const struct {
    std::string_view needle;
    size_t want;
  } kChecks[] = {
      {"float ShadowFilterSoft(", 1}, {" = ShadowFilterSoft(", 1},
      {"float4 constants = g_ShadowFilter;", 1},
      {"\tshadowTap1 = ", 1},          {"\tshadowTap2 = ", 1},
      {"\tshadowTap3 = ", 1},          {"\tshadowTap4 = ", 1},
      {"\tshadowDepth = ", 1},        {"shadowDepth, iPos.xy, a0);", 1},
      {"gLightMatrixArr(0 + base)", 2}, {"float2 ShadowFilterPenumbra(", 1},
      {"float2 ShadowFilterPlane(", 1},
  };
  for (const auto& check : kChecks) {
    const size_t count = CountOccurrences(text, check.needle);
    if (count != check.want) {
      return Failed(shader, std::format("the transformed HLSL has '{}' {} times (need {})", check.needle, count,
                                        check.want));
    }
  }
  VariantTransformResult result;
  result.ok = true;
  result.hlsl = std::move(text);
  return result;
}

VariantDef MakeShadowFilterVariant() {
  VariantDef def;
  def.feature = kFeatureShadowFilter;
  def.name = "shadow_filter";
  def.version = kShadowFilterVariantVersion;
  def.pixel_name_suffixes = {kShadowBlendSuffix, kShadowBlendAlphaSuffix};
  def.needs_rpf = true;
  def.transform = [](const VariantShader& shader, std::string_view hlsl, const StageContext& ctx) {
    return TransformShadowFilterHlsl(shader, hlsl, ctx);
  };
  return def;
}

}  // namespace larecomp::shaderpack
