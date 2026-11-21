// Check of the ray traced car scene's material rules (car_rt_material_rules.h): the table of the cars' shaders, the capture of a draw's material from its pixel constants, the class
// of a walked part, the half-float packing of the texture coordinates, the records the shaders read (the instances' rows and the frame's), the texture coordinate formats, a mesh's
// texel density and the mip level, the game's environment map's mapping (against its inverse), the reference of the shading's arithmetic, and that hlsl/car_rt_hit_shade.hlsli,
// hlsl/car_rt_refl_cs.hlsl, car_rt_scene.cpp and the settings hold the header's numbers.
// A program of its own: it needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_rt_material_rules_test, then run src/gpu/larecomp_car_rt_material_rules_test [<src dir>]
//   (the src folder is the one that holds gpu and settings; by default the one this file is in)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "car_rt_instances_rules.h"
#include "car_rt_material_rules.h"
#include "volumetrics_rules.h"

namespace ci = mcla::gpu::car_rt_instances;
namespace tp = mcla::gpu::temporal;
namespace crm = mcla::gpu::car_rt_material;
namespace cd = mcla::gpu::car_dump;
namespace cw = mcla::gpu::car_model_walk;

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

bool Near(double a, double b, double tolerance = 1e-5) { return std::fabs(a - b) <= tolerance * std::max(1.0, std::fabs(b)); }

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// A pixel bank of 256 registers of four big-endian floats.
struct Bank {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(256 * 16, 0);
  void Set(uint32_t reg, float x, float y, float z, float w) {
    const float v[4] = {x, y, z, w};
    for (uint32_t c = 0; c < 4; ++c) {
      uint32_t bits;
      std::memcpy(&bits, &v[c], 4);
      uint8_t* p = bytes.data() + (size_t(reg) * 4 + c) * 4;
      p[0] = uint8_t(bits >> 24);
      p[1] = uint8_t(bits >> 16);
      p[2] = uint8_t(bits >> 8);
      p[3] = uint8_t(bits);
    }
  }
};

crm::FetchInfo Texture(uint32_t slot_tag, uint16_t w = 512, uint16_t h = 256) {
  crm::FetchInfo f;
  f.texture = true;
  for (uint32_t i = 0; i < 6; ++i) {
    f.dwords[i] = slot_tag * 16 + i;
  }
  f.width = w;
  f.height = h;
  return f;
}

// The number after `name` in `text` ("static const float kX = 0.25;"), or NaN.
double Constant(const std::string& text, const std::string& name) {
  const size_t at = text.find(name + " = ");
  if (at == std::string::npos) {
    return std::nan("");
  }
  return std::strtod(text.c_str() + at + name.size() + 3, nullptr);
}

// The numbers of `static const float name[8] = {a, b, ...};`
std::vector<double> Array(const std::string& text, const std::string& name) {
  std::vector<double> out;
  const size_t at = text.find(name + "[8] = {");
  if (at == std::string::npos) {
    return out;
  }
  const char* p = text.c_str() + at + name.size() + 7;
  for (int i = 0; i < 8; ++i) {
    char* end = nullptr;
    out.push_back(std::strtod(p, &end));
    p = end;
    while (*p == ',' || *p == ' ') {
      ++p;
    }
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::string src = argc > 1 ? argv[1] : std::string();
  if (src.empty()) {
    std::string self = __FILE__;
    for (char& c : self) {
      c = c == '\\' ? '/' : c;
    }
    const size_t cut = self.rfind("/gpu/");
    src = cut != std::string::npos ? self.substr(0, cut) : ".";
  }

  // ---- the table ------------------------------------------------------------------------------------------------------------------
  {
    std::set<uint64_t> seen;
    bool unique = true, in_range = true, grey_ok = true;
    uint32_t per_class[crm::kClassCount] = {};
    for (size_t i = 0; i < crm::kShaderCount; ++i) {
      const crm::ShaderRule& r = crm::kShaders[i];
      unique = unique && r.ps != 0 && seen.insert(r.ps).second;
      in_range = in_range && r.diffuse_fetch >= -1 && r.diffuse_fetch < 32 && uint32_t(r.cls) < crm::kClassCount && r.env <= 2;
      grey_ok = grey_ok && r.grey >= 0.0f && r.grey <= 1.0f;
      ++per_class[uint32_t(r.cls)];
    }
    Check(unique, "no shader identity is listed twice, and none is 0");
    Check(in_range && grey_ok, "every rule has a fetch slot under 32, a class of the table, an environment kind 0 to 2 and a grey in 0..1");
    Check(per_class[0] == 0, "no rule names the class none (that is the answer for an unknown shader)");
    bool all = true;
    for (uint32_t c = 1; c < crm::kClassCount; ++c) {
      all = all && per_class[c] != 0;
    }
    Check(all, "every class of the table has a shader");
    Check(crm::kShaderCount >= 40, "the table holds the car shaders of the main pass (40 or more)");
    const crm::ShaderRule* paint = crm::FindShader(cd::kCarPaintPs);
    Check(paint && paint->cls == crm::Class::kPaint && paint->source == crm::Source::kPaintCustom && paint->diffuse_fetch == int8_t(crm::kVinylFetch) && paint->env == 1,
          "the customisable paint (car_dump::kCarPaintPs) is the paint class: its colours in the pixel constants, the vinyl at fetch 3, the environment map at 0 and 1");
    const crm::ShaderRule* chrome = crm::FindShader(0x2D8CA0430FF88ED7ull);
    Check(chrome && chrome->cls == crm::Class::kChrome && chrome->env == 2, "xChrome::PS_Chrome is the chrome class, environment map at 0 only");
    Check(crm::FindShader(0) == nullptr && crm::FindShader(0x1234ull) == nullptr, "the identity 0 and an unknown one find nothing");
    bool every = true;
    for (size_t i = 0; i < crm::kShaderCount; ++i) {
      every = every && crm::FindShader(crm::kShaders[i].ps) == &crm::kShaders[i];
    }
    Check(every, "the hash index finds every rule of the table");
  }

  // ---- the capture of a draw's material --------------------------------------------------------------------------------------------
  {
    Bank bank;
    bank.Set(101, 4.0f, 0.05f, 0.6f, 3.0f);  // DL1: fresnel power, min, max; flop
    bank.Set(102, 0.9f, 0.5f, 0.1f, 1.0f);   // DL2: colour A
    bank.Set(103, 0.2f, 0.2f, 0.3f, 0.0f);   // DL3: colour B
    const auto vinyl_only = [](uint32_t slot) { return slot == crm::kVinylFetch ? Texture(7) : crm::FetchInfo{}; };
    const crm::Material m = crm::Capture(cd::kCarPaintPs, bank.bytes.data(), vinyl_only);
    Check(m.cls == crm::Class::kPaint && (m.flags & crm::kFlagPaint) != 0 && (m.flags & crm::kFlagDecal) != 0, "a paint draw: class paint, the paint flag and the livery as a decal");
    Check(Near(m.tint_a[0], 0.9) && Near(m.tint_a[1], 0.5) && Near(m.tint_a[2], 0.1) && Near(m.tint_b[2], 0.3), "colour A is c102 and colour B c103");
    Check(m.has_fresnel && Near(m.fresnel[0], 0.05) && Near(m.fresnel[1], 0.6) && Near(m.fresnel[2], 4.0) && Near(m.flop, 3.0),
          "the fresnel is (min, max, power) from DL1's (power, min, max) and the flop is DL1.w");
    Check(m.diffuse.texture && m.diffuse.width == 512 && m.diffuse.dwords[0] == 7 * 16, "the livery's fetch is the draw's fetch 3");
    Check(m.env == 1, "the paint draw is marked as one that binds the environment map");

    const crm::Material none = crm::Capture(0xDEADBEEFull, bank.bytes.data(), vinyl_only);
    Check(!none.Known() && none.cls == crm::Class::kNone, "an unknown shader is class none");
    const crm::Material no_bank = crm::Capture(cd::kCarPaintPs, nullptr, vinyl_only);
    Check(no_bank.cls == crm::Class::kPaint && (no_bank.flags & crm::kFlagPaint) == 0 && Near(no_bank.tint_a[0], crm::DefaultsOf(crm::Class::kPaint).grey),
          "with no constants the paint is the class's grey and not a paint gradient");

    Bank bad = bank;
    bad.Set(102, std::nanf(""), 0.5f, 0.1f, 1.0f);
    const crm::Material nan_paint = crm::Capture(cd::kCarPaintPs, bad.bytes.data(), vinyl_only);
    Check((nan_paint.flags & crm::kFlagPaint) == 0, "a constant that is not a number leaves the paint as the class's grey");
    bad = bank;
    bad.Set(101, 4.0f, 0.9f, 0.1f, 3.0f);  // a min above the max
    const crm::Material odd = crm::Capture(cd::kCarPaintPs, bad.bytes.data(), vinyl_only);
    Check((odd.flags & crm::kFlagPaint) != 0 && !odd.has_fresnel, "a fresnel with the min above the max is not trusted: the class's numbers stand, the colours are kept");
    bad = bank;
    bad.Set(102, 7.0f, 0.5f, -3.0f, 1.0f);
    const crm::Material wide = crm::Capture(cd::kCarPaintPs, bad.bytes.data(), vinyl_only);
    Check(Near(wide.tint_a[0], crm::kTintMax) && Near(wide.tint_a[2], 0.0), "a colour is clamped to the packing's range");

    // the plain xCarPaint and the base colour
    Bank plain;
    plain.Set(104, 0.3f, 0.4f, 0.5f, 1.0f);
    plain.Set(105, 0.1f, 0.1f, 0.1f, 1.0f);
    plain.Set(103, 2.0f, 0.04f, 0.5f, 5.0f);
    const crm::Material p = crm::Capture(0x1C3CD52A0C7D098Dull, plain.bytes.data(), vinyl_only);
    Check(Near(p.tint_a[1], 0.4) && Near(p.tint_b[0], 0.1) && Near(p.fresnel[2], 2.0) && Near(p.flop, 5.0), "xCarPaint reads c104 and c105 and the fresnel from c103");
    Bank base;
    base.Set(100, 0.8f, 0.7f, 0.6f, 1.0f);
    const auto slot2 = [](uint32_t slot) { return slot == 2 ? Texture(3, 256, 256) : crm::FetchInfo{}; };
    const crm::Material b = crm::Capture(0x611497E6F65FED14ull, base.bytes.data(), slot2);
    Check(b.cls == crm::Class::kOther && Near(b.tint_a[0], 0.8) && b.diffuse.texture && b.diffuse.width == 256 && (b.flags & crm::kFlagDecal) == 0,
          "xBumpSpec: the base colour multiplies the diffuse at fetch 2, which is no decal");
    const crm::Material light = crm::Capture(0xCC710F7164C151DEull, base.bytes.data(), [](uint32_t slot) { return slot == 3 ? Texture(9) : crm::FetchInfo{}; });
    Check(light.cls == crm::Class::kLight && (light.flags & crm::kFlagEmissive) != 0 && light.emissive > 0.0f && light.diffuse.texture, "a lamp: class light, emissive, the illumination map at fetch 3");
    const crm::Material no_texture = crm::Capture(0x611497E6F65FED14ull, base.bytes.data(), [](uint32_t) { return crm::FetchInfo{}; });
    Check(!no_texture.diffuse.texture, "a fetch slot with no texture leaves the material without a diffuse");
    // a textured surface with no colour in its constants: the texture is the colour (tint 1), the class's grey is the fallback where the texture is not resident
    const crm::Material trim = crm::Capture(0xDBEFD3025AE7FBF3ull, base.bytes.data(), [](uint32_t slot) { return slot == 1 ? Texture(4, 128, 128) : crm::FetchInfo{}; });
    Check(trim.cls == crm::Class::kInterior && trim.diffuse.texture && Near(trim.tint_a[0], 1.0) && Near(trim.grey, crm::DefaultsOf(crm::Class::kInterior).grey),
          "an interior trim with its diffuse: the tint is 1 and the class's grey is kept as the fallback");
    crm::InstanceMaterial trim_im;
    trim_im.material = trim;
    uint32_t trim_rows[crm::kMaterialRows][4];
    crm::MakeRows(trim_im, 0x42u, trim_rows);
    const crm::RowsDecoded with_texture = crm::DecodeRows(trim_rows);
    crm::MakeRows(trim_im, crm::kNoSlot, trim_rows);
    const crm::RowsDecoded without = crm::DecodeRows(trim_rows);
    Check(Near(with_texture.tint_a[0], 1.0, 3e-3) && Near(without.tint_a[0], crm::DefaultsOf(crm::Class::kInterior).grey, 3e-3),
          "the record carries the tint 1 with the texture and the class's grey without it");
    const crm::Material plain_trim = crm::Capture(0xDBEFD3025AE7FBF3ull, base.bytes.data(), [](uint32_t) { return crm::FetchInfo{}; });
    Check(plain_trim.grey == 0.0f && Near(plain_trim.tint_a[0], crm::DefaultsOf(crm::Class::kInterior).grey), "a part with no texture at all is the class's grey from the start");
  }

  // ---- a walked car's parts ---------------------------------------------------------------------------------------------------------
  {
    Check(crm::ClassOfPart(cw::kBodyPart) == crm::Class::kPaint && crm::ClassOfPart(cw::kTyrePart) == crm::Class::kTyre && crm::ClassOfPart(cw::kRimPart) == crm::Class::kChrome &&
              crm::ClassOfPart(cw::kModelPart) == crm::Class::kPaint,
          "the body and a traffic car's model are paint, the tyre rubber, the rim chrome");
    Check(crm::ClassOfPart(2) == crm::Class::kPaint && crm::ClassOfPart(20) == crm::Class::kPaint && crm::ClassOfPart(21) == crm::Class::kPaint && crm::ClassOfPart(6) == crm::Class::kPaint,
          "the hood, the doors and the front bumper are paint");
    Check(crm::ClassOfPart(11) == crm::Class::kLight && crm::ClassOfPart(12) == crm::Class::kLight && crm::ClassOfPart(13) == crm::Class::kLight, "the headlight and the taillights are lights");
    Check(crm::ClassOfPart(23) == crm::Class::kChrome && crm::ClassOfPart(24) == crm::Class::kChrome, "the exhausts are chrome");
    bool interior = true;
    for (uint32_t p = 44; p <= 53; ++p) {
      interior = interior && crm::ClassOfPart(p) == crm::Class::kInterior;
    }
    Check(interior, "the interior, the seats, the door interiors, the stereo, the wheel and the gauge are interior");
    bool none = true;
    for (uint32_t p = 0; p < 64; ++p) {
      none = none && crm::ClassOfPart(p) != crm::Class::kNone;
    }
    Check(none, "no part is class none (a walked part always has a class colour)");

    const float a[3] = {0.9f, 0.5f, 0.2f}, bb[3] = {0.1f, 0.1f, 0.1f};
    const crm::Material wp = crm::WalkedMaterial(cw::kBodyPart, true, a, bb, 2.0f);
    Check((wp.flags & crm::kFlagPaint) != 0 && Near(wp.tint_a[0], 0.81) && Near(wp.tint_a[1], 0.25) && Near(wp.tint_b[0], 0.01), "a walked body takes the record's colours raised to the gamma");
    const crm::Material nopaint = crm::WalkedMaterial(cw::kBodyPart, false, a, bb, 2.0f);
    Check((nopaint.flags & crm::kFlagPaint) == 0 && Near(nopaint.tint_a[0], crm::DefaultsOf(crm::Class::kPaint).grey), "a walked car with no record is the paint class's grey");
    const crm::Material tyre = crm::WalkedMaterial(cw::kTyrePart, true, a, bb, 2.0f);
    Check(tyre.cls == crm::Class::kTyre && (tyre.flags & crm::kFlagPaint) == 0 && Near(tyre.tint_a[0], crm::DefaultsOf(crm::Class::kTyre).grey), "a walked tyre is black rubber whatever the paint");
    const crm::Material lamp = crm::WalkedMaterial(11, false, nullptr, nullptr, 2.0f);
    Check(lamp.cls == crm::Class::kLight && (lamp.flags & crm::kFlagEmissive) != 0, "a walked lamp glows at night");
  }

  // ---- half floats and packed colours ----------------------------------------------------------------------------------------------
  {
    bool exact = true;
    for (uint32_t h = 0; h < 0x10000u; ++h) {
      const uint32_t exponent = (h >> 10) & 0x1Fu;
      if (exponent == 31) {
        continue;  // inf and NaN
      }
      const float f = cd::HalfToFloat(uint16_t(h));
      const uint16_t back = crm::FloatToHalf(f);
      if (back != uint16_t(h) && !((h & 0x7FFFu) == 0 && (back & 0x7FFFu) == 0)) {
        exact = false;
      }
    }
    Check(exact, "every finite half float survives the round trip through a float");
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> dist(-8.0f, 8.0f);
    double worst = 0.0;
    for (int i = 0; i < 20000; ++i) {
      const float v = dist(rng);
      const float back = cd::HalfToFloat(crm::FloatToHalf(v));
      worst = std::max(worst, std::fabs(double(back) - v) / std::max(std::fabs(double(v)), 1.0 / 1024.0));
    }
    Check(worst < 1.0 / 1024.0 + 1e-6, "a float within a texture coordinate's range rounds to the nearest half (relative error under 2^-10)");
    Check(crm::FloatToHalf(1e9f) == 0x7BFFu && crm::FloatToHalf(-1e9f) == 0xFBFFu, "a number beyond the half's range saturates at the largest finite half");
    Check(crm::FloatToHalf(std::nanf("")) == 0 && crm::FloatToHalf(1e-12f) == 0 && crm::FloatToHalf(-1e-12f) == 0x8000u, "a NaN is 0, a tiny number rounds to a signed zero");
    Check(crm::FloatToHalf(1.0f) == 0x3C00u && crm::FloatToHalf(0.5f) == 0x3800u && crm::FloatToHalf(-2.0f) == 0xC000u, "1, 0.5 and -2 are the halves the shaders know");
    Check(crm::FloatToHalf(2049.0f) == 0x6800u && crm::FloatToHalf(2051.0f) == 0x6802u, "a tie rounds to even");
    float u = 0.0f, v = 0.0f;
    crm::UnpackUv(crm::PackUv(0.25f, 3.5f), &u, &v);
    Check(u == 0.25f && v == 3.5f, "a texture coordinate pair packs u into the low half and v into the high");
    crm::UnpackUv(crm::PackUv(-1.5f, 100.0f), &u, &v);
    Check(u == -1.5f && v == 100.0f, "a coordinate outside 0..1 (a tiled texture) survives");

    const float rgb[3] = {0.0f, 1.0f, 2.0f};
    float out[3];
    crm::UnpackRgb10(crm::PackRgb10(rgb), out);
    Check(Near(out[0], 0.0) && Near(out[1], 1.0, 2e-3) && Near(out[2], 2.0), "a colour packs into ten bits a channel over 0..2");
    const float over[3] = {5.0f, -1.0f, std::nanf("")};
    crm::UnpackRgb10(crm::PackRgb10(over), out);
    Check(Near(out[0], 2.0) && Near(out[1], 0.0) && Near(out[2], 0.0), "a colour out of range is clamped, a NaN is 0");
  }

  // ---- the records -------------------------------------------------------------------------------------------------------------------
  {
    crm::InstanceMaterial im;
    im.material.cls = crm::Class::kPaint;
    im.material.flags = crm::kFlagPaint | crm::kFlagDecal;
    im.material.tint_a[0] = 0.9f;
    im.material.tint_a[1] = 0.5f;
    im.material.tint_a[2] = 0.1f;
    im.material.tint_b[0] = im.material.tint_b[1] = im.material.tint_b[2] = 0.2f;
    im.material.has_fresnel = true;
    im.material.fresnel[0] = 0.05f;
    im.material.fresnel[1] = 0.6f;
    im.material.fresnel[2] = 4.0f;
    im.material.flop = 3.0f;
    im.material.diffuse = Texture(2, 512, 256);
    im.has_uv = true;
    im.uv_density = 0.5f;
    uint32_t rows[crm::kMaterialRows][4];
    crm::MakeRows(im, 0x80000123u, rows);
    const crm::RowsDecoded d = crm::DecodeRows(rows);
    Check(d.cls == crm::Class::kPaint && (d.flags & crm::kFlagPaint) != 0 && (d.flags & crm::kFlagDecal) != 0 && (d.flags & crm::kFlagUv) != 0, "the row holds the class and the flags, with the mesh's texture coordinates flagged");
    Check(Near(d.fresnel_min, 0.05, 0.01) && Near(d.fresnel_max, 0.6, 0.01) && Near(d.fresnel_power, 4.0, 0.01), "the fresnel is quantised to a byte (the power to 1/16)");
    Check(d.slot == 0x80000123u && Near(d.tint_a[0], 0.9, 3e-3) && Near(d.tint_a[1], 0.5, 3e-3) && Near(d.tint_b[2], 0.2, 3e-3) && Near(d.flop, 3.0), "the slot (gamma bit kept), the tints and the flop come back");
    Check(Near(d.lod_base, std::log2(0.5 * 512.0)), "the mip bias is log2 of the texels a metre of the mesh has (texture units a metre times the larger edge)");
    crm::MakeRows(im, crm::kNoSlot, rows);
    const crm::RowsDecoded none = crm::DecodeRows(rows);
    Check(none.slot == crm::kNoSlot && none.lod_base == 0.0f, "a texture that is not resident leaves no slot and no mip bias");
    im.has_uv = false;
    crm::MakeRows(im, 5, rows);
    Check((crm::DecodeRows(rows).flags & crm::kFlagUv) == 0 && crm::DecodeRows(rows).lod_base == 0.0f, "a mesh with no texture coordinates is not flagged and has no mip bias");
    crm::InstanceMaterial plain;
    uint32_t pr[crm::kMaterialRows][4];
    crm::MakeRows(plain, crm::kNoSlot, pr);
    Check(crm::DecodeRows(pr).cls == crm::Class::kNone, "an instance with no material is class none: the pass keeps today's stand-in");
    crm::InstanceMaterial glass;
    glass.material.cls = crm::Class::kGlass;
    crm::MakeRows(glass, crm::kNoSlot, pr);
    const crm::RowsDecoded gd = crm::DecodeRows(pr);
    Check(Near(gd.fresnel_min, crm::DefaultsOf(crm::Class::kGlass).fresnel_min, 0.01) && Near(gd.fresnel_max, 1.0, 0.01) && Near(gd.fresnel_power, 5.0, 0.01), "without constants the row carries the class's fresnel");
  }

  // ---- the frame's rows --------------------------------------------------------------------------------------------------------------
  {
    crm::FrameInput in;
    in.light.valid = true;
    in.light.sun[0] = 1.0f;
    in.light.sun[1] = 0.9f;
    in.light.sun[2] = 0.8f;
    in.light.ambient[0] = 0.3f;
    in.light.ambient[1] = 0.35f;
    in.light.ambient[2] = 0.4f;
    in.light.env_sharp = 0x80000010u;
    in.light.env_blur = 0x11u;
    in.sun_known = true;
    in.to_sun_view[0] = 0.0f;
    in.to_sun_view[1] = 0.8f;
    in.to_sun_view[2] = 0.6f;
    in.world_axes_view[1][1] = 0.9f;
    uint32_t rows[crm::kFrameRows][4];
    crm::MakeFrameRows(in, rows);
    const uint32_t flags = rows[0][3];
    Check((flags & crm::kFrameShade) != 0 && (flags & crm::kFrameShadow) != 0 && (flags & crm::kFrameEnv) != 0 && (flags & crm::kFrameSun) != 0, "a frame with light, an environment map and a sun has all four flags");
    Check(Near(crm::FromBits(rows[0][1]), 0.8) && Near(crm::FromBits(rows[1][0]), 1.0) && Near(crm::FromBits(rows[2][2]), 0.4) && rows[3][0] == 0x80000010u && rows[3][1] == 0x11u, "the rows hold the sun's direction, colour, the ambient and the map's slots");
    Check(Near(crm::FromBits(rows[1][3]), crm::kSunReach) && Near(crm::FromBits(rows[2][3]), 1.0) && Near(crm::FromBits(rows[3][3]), 1.0) && Near(crm::FromBits(rows[4][3]), 1.0), "the reach and the three gains are in the rows");
    Check(Near(crm::FromBits(rows[6][1]), 0.9), "the world's axes follow from row 5");
    Check(Near(crm::FromBits(rows[3][2]), 0.0), "in the sun the night factor is 0");
    crm::FrameInput night = in;
    night.light.sun[0] = night.light.sun[1] = night.light.sun[2] = 0.01f;
    night.sun_known = false;  // no sun direction: the colour's rule
    crm::MakeFrameRows(night, rows);
    Check(Near(crm::FromBits(rows[3][2]), 1.0), "in a dim light, with no sun direction, the night factor is 1 (the colour's rule)");
    crm::FrameInput off = in;
    off.tuning.enabled = false;
    off.tuning.shadow = false;
    off.light.env_sharp = 0;
    off.sun_known = false;
    crm::MakeFrameRows(off, rows);
    Check(rows[0][3] == 0u, "the switch off, no shadow ray, no map and no sun clears every flag");
    crm::FrameInput nolight = in;
    nolight.light.valid = false;
    crm::MakeFrameRows(nolight, rows);
    Check((rows[0][3] & crm::kFrameShade) == 0, "a frame whose light was not read is not shaded");
    // the night factor falls smoothly between its two thresholds
    float last = 2.0f;
    bool monotonic = true;
    for (int i = 0; i <= 100; ++i) {
      const float v = crm::kNightLow + (crm::kNightHigh - crm::kNightLow) * float(i) / 100.0f;
      const float sun[3] = {v, v, v};
      const float n = crm::NightFactor(sun);
      monotonic = monotonic && n <= last + 1e-6f;
      last = n;
    }
    Check(monotonic && last < 0.01f, "the night factor falls from 1 to 0 between the thresholds and never rises");
  }

  // ---- texture coordinates -----------------------------------------------------------------------------------------------------------
  {
    cd::PositionFormat format;
    Check(crm::UvFormat(0x002C235F, &format) && format == cd::PositionFormat::kHalf2 && crm::UvBytes(format) == 4, "k_16_16_FLOAT (0x002C235F) is a half2 of 4 bytes");
    Check(crm::UvFormat(0x002C23A5, &format) && format == cd::PositionFormat::kFloat2 && crm::UvBytes(format) == 8, "k_32_32_FLOAT (0x002C23A5) is a float2 of 8 bytes");
    Check(crm::UvFormat(0x002A23B9, &format) && format == cd::PositionFormat::kFloat3, "a float3 is taken (its first two components)");
    Check(!crm::UvFormat(0x00182886, &format) && !crm::UvFormat(0, &format), "a packed colour or no format is no texture coordinate");
    const uint8_t half2[4] = {0x3C, 0x00, 0xB8, 0x00};  // 1.0, -0.5 big endian
    float u = 0.0f, v = 0.0f;
    Check(crm::DecodeUv(half2, cd::PositionFormat::kHalf2, &u, &v) && u == 1.0f && v == -0.5f, "a half2 is read big endian, u first");
    const uint8_t float2[8] = {0x3F, 0x00, 0x00, 0x00, 0x40, 0x20, 0x00, 0x00};  // 0.5, 2.5
    Check(crm::DecodeUv(float2, cd::PositionFormat::kFloat2, &u, &v) && u == 0.5f && v == 2.5f, "a float2 is read big endian");
    const uint8_t nan2[8] = {0x7F, 0xC0, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00};
    Check(!crm::DecodeUv(nan2, cd::PositionFormat::kFloat2, &u, &v), "a coordinate that is not a number is refused");
    Check(!crm::DecodeUv(half2, cd::PositionFormat::kNone, &u, &v), "a format that is no coordinate is refused");

    // density: a 2 x 2 m quad with a 0..1 texture is 0.5 texture units a metre; with 0..4 it is 2
    const std::vector<float> pos = {0, 0, 0, 2, 0, 0, 2, 2, 0, 0, 2, 0};
    const std::vector<uint32_t> idx = {0, 1, 2, 0, 2, 3};
    const std::vector<float> uv1 = {0, 0, 1, 0, 1, 1, 0, 1};
    std::vector<float> uv4 = uv1;
    for (float& x : uv4) {
      x *= 4.0f;
    }
    Check(Near(crm::MeshUvDensity(pos, uv1, idx), 0.5) && Near(crm::MeshUvDensity(pos, uv4, idx), 2.0), "a quad's texel density is the square root of the texture area over the world area");
    std::vector<float> mirrored = uv1;
    for (size_t i = 0; i < mirrored.size(); i += 2) {
      mirrored[i] = 1.0f - mirrored[i];
    }
    Check(Near(crm::MeshUvDensity(pos, mirrored, idx), 0.5), "a mirrored texture has the same density");
    Check(crm::MeshUvDensity(pos, {}, idx) == 0.0f && crm::MeshUvDensity(pos, {0, 0, 1, 1}, idx) == 0.0f, "no texture coordinates, or the wrong number of them, give no density");
    const std::vector<float> flat = {1, 1, 1, 1, 1, 1, 1, 1};
    Check(crm::MeshUvDensity(pos, flat, idx) == 0.0f, "a mesh whose texture coordinates have no area has no density");
    const std::vector<uint32_t> bad = {0, 1, 99};
    Check(crm::MeshUvDensity(pos, uv1, bad) == 0.0f, "a triangle that names a vertex that is not there is skipped");
    // a few big triangles do not decide it: area weighted
    const std::vector<float> pos2 = {0, 0, 0, 10, 0, 0, 0, 10, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0};
    const std::vector<float> uv2 = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    const std::vector<uint32_t> idx2 = {0, 1, 2, 3, 4, 5};
    const float mixed = crm::MeshUvDensity(pos2, uv2, idx2);
    Check(mixed > 0.1f && mixed < 1.0f && Near(mixed, std::sqrt((0.5 + 0.5) / (50.0 + 0.5)), 1e-4), "the density is area weighted over the triangles");
    Check(Near(crm::FootprintLod(1.0f, 0.0f, 10), 0.0) && Near(crm::FootprintLod(4.0f, 0.0f, 10), 2.0) && Near(crm::FootprintLod(4.0f, 3.0f, 10), 5.0), "the level is log2 of the footprint in metres plus the mesh's bias");
    Check(Near(crm::FootprintLod(1e6f, 0.0f, 4), 3.0) && Near(crm::FootprintLod(1e-9f, -5.0f, 4), 0.0), "the level is clamped to the texture's levels");
  }

  // ---- the environment map ----------------------------------------------------------------------------------------------------------------
  {
    std::mt19937 rng(11);
    std::normal_distribution<float> n(0.0f, 1.0f);
    double worst = 0.0;
    bool in_discs = true;
    int left = 0, right = 0;
    for (int i = 0; i < 4000; ++i) {
      float r[3] = {n(rng), n(rng), n(rng)};
      const float length = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
      for (float& c : r) {
        c /= length;
      }
      float u, v;
      crm::EnvUv(r, &u, &v);
      in_discs = in_discs && u >= -1e-5f && u <= 1.0f + 1e-5f && v >= -1e-5f && v <= 1.0f + 1e-5f;
      (u < 0.5f ? left : right) += 1;
      float back[3];
      if (!crm::EnvDirection(u, v, back)) {
        worst = 1e9;
        continue;
      }
      for (int c = 0; c < 3; ++c) {
        worst = std::max(worst, double(std::fabs(back[c] - r[c])));
      }
    }
    Check(in_discs, "every direction maps inside the 512x256 image");
    Check(worst < 2e-4, "EnvDirection inverts EnvUv (the mapping is the paraboloid's: worst error " + std::to_string(worst) + ")");
    Check(left > 1500 && right > 1500, "the two hemispheres fill the two halves of the image");
    const float forward[3] = {0, 0, -1}, back[3] = {0, 0, 1}, up[3] = {0, 1, 0}, down[3] = {0, -1, 0};
    float u, v;
    crm::EnvUv(forward, &u, &v);
    Check(Near(u, 0.25) && Near(v, 0.5), "the axis direction -z is the centre of the left half");
    crm::EnvUv(back, &u, &v);
    Check(Near(u, 0.75) && Near(v, 0.5), "the axis direction +z is the centre of the right half");
    crm::EnvUv(up, &u, &v);
    Check(Near(v, 0.0) && Near(u, 0.75), "straight up is the top edge of the right disc's rim (b = 0: the rim)");
    crm::EnvUv(down, &u, &v);
    Check(Near(v, 1.0), "straight down is the bottom edge");
    // the two discs agree at the rim: the same v, the same distance from the centre
    const float rim_a[3] = {0.6f, 0.8f, 0.0f};
    float ua, va, ub, vb;
    const float rim_b[3] = {0.6f, 0.8f, -1e-4f};
    crm::EnvUv(rim_a, &ua, &va);
    crm::EnvUv(rim_b, &ub, &vb);
    Check(Near(va, vb, 1e-3) && std::fabs((ua - 0.75f) - (ub - 0.25f) * (-1.0f)) < 2e-3 + 0.0, "at the rim the two discs give the same v");
  }

  // ---- the shading ---------------------------------------------------------------------------------------------------------------------
  {
    crm::ShadeInput in;
    in.tuning.detail = 0.0f;  // the shading as it was (RT-HIT-SHADING-5's detail has its own checks below)
    in.cls = crm::Class::kOther;
    in.tint_a[0] = in.tint_a[1] = in.tint_a[2] = 1.0f;
    in.light.valid = true;
    for (int c = 0; c < 3; ++c) {
      in.light.sun[c] = 1.0f;
      in.light.ambient[c] = 0.2f;
    }
    in.fresnel[0] = in.fresnel[1] = 0.0f;  // no reflection: the diffuse alone
    in.normal[1] = 1.0f;
    in.ray[1] = -1.0f;
    in.ray[2] = 0.0f;
    float out[3];
    crm::ShadeReference(in, out);
    Check(Near(out[0], 0.2 * 1.0 + 1.0, 1e-4), "a white surface facing the sun and the sky shows the ambient (hemisphere 1) plus the sun");
    in.sun_visible = 0.0f;
    crm::ShadeReference(in, out);
    Check(Near(out[0], 0.2, 1e-4), "in the sun's shadow only the ambient lights it");
    in.sun_visible = 1.0f;
    in.normal[0] = 0.0f;
    in.normal[1] = -1.0f;
    in.ray[1] = 1.0f;
    in.to_sun[1] = 1.0f;
    crm::ShadeReference(in, out);
    Check(Near(out[0], 0.2 * 0.2, 1e-4), "a surface facing down takes the sun not at all and the darkest hemisphere share (0.2) of the ambient");
    in.normal[1] = 1.0f;
    in.ray[1] = -1.0f;

    // gain, spec, fresnel
    in.tuning.gain = 2.0f;
    crm::ShadeReference(in, out);
    Check(Near(out[0], 2.0 * 1.2, 1e-4), "the gain scales the colour");
    in.tuning.gain = 1.0f;
    in.cls = crm::Class::kGlass;
    in.tint_a[0] = in.tint_a[1] = in.tint_a[2] = 0.0f;  // a clear pane: no diffuse
    in.fresnel[0] = 0.04f;
    in.fresnel[1] = 1.0f;
    in.fresnel[2] = 5.0f;
    in.env[0] = in.env[1] = in.env[2] = 2.0f;
    crm::ShadeReference(in, out);
    const double head_on = out[0];
    Check(Near(head_on, 2.0 * 0.04 * 1.0, 1e-3) && head_on > 0.0, "a glass pane seen head on reflects the environment by the fresnel's minimum");
    in.ray[0] = 0.98f;
    in.ray[1] = -0.2f;
    const float length = std::sqrt(in.ray[0] * in.ray[0] + in.ray[1] * in.ray[1]);
    in.ray[0] /= length;
    in.ray[1] /= length;
    crm::ShadeReference(in, out);
    Check(out[0] > 4.0 * head_on, "at a grazing angle the reflection is several times stronger");
    in.ray[0] = 0.0f;
    in.ray[1] = -1.0f;
    in.env_known = false;
    crm::ShadeReference(in, out);
    Check(out[0] > 0.0f && out[0] < 2.0 * 0.04 * 1.0 + 1e-6, "with no environment map the ambient stands in for it");
    in.env_known = true;

    // paint: the gradient by N.V, the livery over it, the texture times the tint
    crm::ShadeInput paint;
    paint.cls = crm::Class::kPaint;
    paint.paint = true;
    paint.flop = 2.0f;
    for (int c = 0; c < 3; ++c) {
      paint.tint_a[c] = 0.8f;
      paint.tint_b[c] = 0.2f;
    }
    paint.normal[1] = 1.0f;
    paint.ray[1] = -1.0f;
    float albedo[3];
    crm::Albedo(paint, 1.0f, albedo);
    Check(Near(albedo[0], 0.8), "facing the camera the paint is colour A");
    crm::Albedo(paint, 0.0f, albedo);
    Check(Near(albedo[0], 0.2), "at the rim the paint is colour B");
    crm::Albedo(paint, 0.5f, albedo);
    Check(Near(albedo[0], 0.2 + 0.6 * 0.25, 1e-4), "between them the gradient follows (N.V)^flop");
    paint.textured = true;
    paint.decal = true;
    paint.texel[0] = 1.0f;
    paint.texel[1] = 0.0f;
    paint.texel[2] = 0.0f;
    paint.texel[3] = 1.0f;
    crm::Albedo(paint, 1.0f, albedo);
    Check(Near(albedo[0], 1.0) && Near(albedo[1], 0.0), "an opaque livery replaces the paint");
    paint.texel[3] = 0.0f;
    crm::Albedo(paint, 1.0f, albedo);
    Check(Near(albedo[0], 0.8), "a transparent livery leaves the paint");
    paint.decal = false;
    paint.texel[0] = paint.texel[1] = paint.texel[2] = 0.5f;
    paint.texel[3] = 1.0f;
    crm::Albedo(paint, 1.0f, albedo);
    Check(Near(albedo[0], 0.4), "a diffuse texture multiplies the tint");

    // night: the lamps glow, the day does not
    crm::ShadeInput lamp;
    lamp.cls = crm::Class::kLight;
    for (int c = 0; c < 3; ++c) {
      lamp.tint_a[c] = 0.5f;
      lamp.light.ambient[c] = 0.0f;
      lamp.light.sun[c] = 0.0f;
    }
    lamp.fresnel[0] = lamp.fresnel[1] = 0.0f;
    lamp.emissive = 1.0f;
    lamp.normal[1] = 1.0f;
    lamp.ray[1] = -1.0f;
    lamp.night = 0.0f;
    crm::ShadeReference(lamp, out);
    Check(Near(out[0], 0.0), "a lamp in the day, lit by nothing, is dark");
    lamp.night = 1.0f;
    crm::ShadeReference(lamp, out);
    Check(Near(out[0], 0.5 * crm::kEmissiveGain, 1e-4), "at night the lamp glows by its albedo and the emissive gain");

    // chrome shows the environment tinted by the metal, its diffuse is a tenth
    crm::ShadeInput metal;
    metal.cls = crm::Class::kChrome;
    for (int c = 0; c < 3; ++c) {
      metal.tint_a[c] = 0.9f;
      metal.light.ambient[c] = 0.0f;
      metal.light.sun[c] = 0.0f;
      metal.env[c] = 1.0f;
    }
    metal.fresnel[0] = 0.7f;
    metal.fresnel[1] = 1.0f;
    metal.fresnel[2] = 5.0f;
    metal.normal[1] = 1.0f;
    metal.ray[1] = -1.0f;
    crm::ShadeReference(metal, out);
    Check(Near(out[0], 1.0 * 0.7 * 1.0 * 0.9, 1e-3), "chrome head on shows the environment by the fresnel and the metal's tint");

    // the blend with the history
    Check(Near(crm::HitColour(2.0f, 10.0f, 0.0f), 2.0) && Near(crm::HitColour(2.0f, 10.0f, 1.0f), 10.0) && Near(crm::HitColour(2.0f, 10.0f, 0.5f), 6.0), "the hit's colour is the shaded one where the history shows none of it, the history's where it shows all, and between them the mean");
    bool smooth = true;
    float prev = crm::HitColour(2.0f, 10.0f, 0.0f);
    for (int i = 1; i <= 100; ++i) {
      const float c = crm::HitColour(2.0f, 10.0f, float(i) / 100.0f);
      smooth = smooth && c >= prev && c - prev <= 0.081f;
      prev = c;
    }
    Check(smooth, "the blend has no step: a hundredth of the visible share moves the colour by a hundredth of the difference");
  }

  // ---- RT-HIT-SHADING-5: the hit's own detail ------------------------------------------------------------------------------------------
  {
    const auto unit = [](float v[3]) {
      const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
      for (int c = 0; c < 3; ++c) {
        v[c] /= l;
      }
    };
    // the hash: the published values of PCG, the hit's cell and the spread of the two numbers it gives
    Check(crm::Pcg(0) == 129708002u && crm::Pcg(1) == 2831084092u && crm::Pcg(12345) == 4099845390u && crm::Pcg(0xFFFFFFFFu) == 3861530882u, "the sky ray's hash gives PCG's values");
    {
      const float hit[3] = {0.5f, 0.25f, 3.0f};
      float a = 0.0f, b = 0.0f;
      crm::OpenHash(hit, &a, &b);
      Check(Near(a, 0.95782470703125, 1e-6) && Near(b, 0.6495361328125, 1e-6), "the hit's cell (centimetres in view space) gives the same two numbers as the reference");
      double sum_a = 0.0, sum_b = 0.0;
      int count = 0;
      std::mt19937 rng(7);
      std::uniform_real_distribution<float> place(-8.0f, 8.0f);
      for (int i = 0; i < 20000; ++i) {
        const float p[3] = {place(rng), place(rng), place(rng) + 8.0f};
        crm::OpenHash(p, &a, &b);
        sum_a += a;
        sum_b += b;
        ++count;
      }
      Check(std::fabs(sum_a / count - 0.5) < 0.01 && std::fabs(sum_b / count - 0.5) < 0.01, "the two numbers the hash gives are spread evenly over [0, 1)");
    }
    // the sky ray's direction
    {
      const float up[3] = {0.0f, 1.0f, 0.0f};
      float normal[3] = {-1.0f, 0.0f, 0.0f};  // a vertical flank
      std::mt19937 rng(11);
      std::uniform_real_distribution<float> unit01(0.0f, 1.0f);
      double widest = 0.0;
      bool above = true, unit_ok = true;
      for (int i = 0; i < 4000; ++i) {
        float e[3];
        crm::SkyRayDirection(normal, up, unit01(rng), unit01(rng), e);
        const float length = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        unit_ok = unit_ok && std::fabs(length - 1.0f) < 1e-4f;
        above = above && crm::Dot3(e, normal) >= crm::kOpenMinAbove - 1e-4f;
        const float centre[3] = {-0.70710678f, 0.70710678f, 0.0f};
        widest = std::max(widest, std::acos(std::clamp(double(crm::Dot3(e, centre)), -1.0, 1.0)) * 180.0 / 3.14159265358979);
      }
      Check(unit_ok && above, "the sky ray is a unit vector kept over the tangent plane");
      Check(widest > 15.0 && widest < 19.6, "the sky ray of a vertical flank leaves at 45 degrees up and spreads over a cone of the jitter's tangent (widest " + std::to_string(widest) + " degrees)");
      float roof[3] = {0.0f, 1.0f, 0.0f};
      float e[3];
      crm::SkyRayDirection(roof, up, 0.3f, 0.7f, e);
      Check(e[1] > 0.93f, "the sky ray of a roof is within the jitter of straight up");
      float under[3] = {0.0f, -1.0f, 0.0f};
      crm::SkyRayDirection(under, up, 0.3f, 0.7f, e);
      Check(crm::Dot3(e, under) >= crm::kOpenMinAbove - 1e-4f, "a surface that faces the ground keeps its ray over its own tangent plane (the ground side)");
      float slope[3] = {-0.6f, 0.8f, 0.0f};
      bool slope_up = true;
      for (int i = 0; i < 100; ++i) {
        crm::SkyRayDirection(slope, up, unit01(rng), unit01(rng), e);
        slope_up = slope_up && e[1] > 0.5f;
      }
      Check(slope_up, "the sky ray of a surface that tilts up leans further up");
      // any normal and any way the camera is rolled: a finite unit ray over the tangent plane
      bool sane = true;
      std::normal_distribution<float> gauss(0.0f, 1.0f);
      for (int i = 0; i < 4000; ++i) {
        float nn[3] = {gauss(rng), gauss(rng), gauss(rng)};
        float uu[3] = {gauss(rng), gauss(rng), gauss(rng)};
        if (i % 7 == 0) {
          uu[0] = 1.0f;
          uu[1] = uu[2] = 0.0f;  // the world's up along the view's x
        }
        unit(nn);
        unit(uu);
        crm::SkyRayDirection(nn, uu, unit01(rng), unit01(rng), e);
        const float length = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        sane = sane && std::isfinite(length) && std::fabs(length - 1.0f) < 1e-4f && crm::Dot3(e, nn) >= 0.95f * crm::kOpenMinAbove;  // the lift (as ConeDirection's) is a push along the normal, not an exact angle
      }
      Check(sane, "the sky ray is a finite unit ray over the tangent plane for every normal and every roll of the camera");
    }
    // the fresnel, the lobe, the contact, the openness
    Check(Near(crm::CoatFresnel(1.0f), 0.04) && Near(crm::CoatFresnel(0.5f), 0.04 + 0.96 * std::pow(0.5, 5.0), 1e-4) && Near(crm::CoatFresnel(0.0f), 1.0), "the coat's fresnel is Schlick's with F0 0.04 and power 5");
    Check(Near(crm::GlintLobe(1.0f, crm::kSunGlintPower), 1.0) && crm::GlintLobe(0.9848f, crm::kSunGlintPower) < 0.1 && crm::GlintLobe(-0.5f, crm::kSunGlintPower) == 0.0f, "the sun's lobe is 1 on the axis, a tenth at 10 degrees and nothing behind");
    Check(Near(crm::ContactMultiplier(0.0f), crm::kContactMin) && Near(crm::ContactMultiplier(crm::kContactHeight), 1.0) && Near(crm::ContactMultiplier(2.0f), 1.0) && Near(crm::ContactMultiplier(-1.0f), 1.0) &&
              Near(crm::ContactMultiplier(0.5f * crm::kContactHeight), 0.5 * (1.0 + crm::kContactMin)),
          "the contact darkening is kContactMin on the ground, 1 from kContactHeight up and where the height is not known");
    {
      bool monotonic = true;
      float last = 0.0f;
      for (int i = 0; i <= 100; ++i) {
        const float v = crm::ContactMultiplier(crm::kContactHeight * float(i) / 100.0f);
        monotonic = monotonic && v >= last - 1e-6f;
        last = v;
      }
      Check(monotonic, "the contact darkening never falls with the height");
    }
    Check(Near(crm::OpenMultiplier(0.0f), crm::kOpenMin) && Near(crm::OpenMultiplier(1.0f), 1.0) && Near(crm::OpenMultiplier(0.5f), 0.5 * (1.0 + crm::kOpenMin)), "the sky's openness keeps kOpenMin closed in and all open");
    Check(crm::kContactMin > 0.0f && crm::kOpenMin > 0.0f && crm::kCarCentreRise == float(ci::kCentreRise), "the shader's ground is the car's record's centre taken down by the instances' own rise");

    // the night factor from the sun's height is the volumetric light's own
    {
      float worst = 0.0f;
      for (int i = -100; i <= 400; ++i) {
        const float degrees = float(i) * 0.1f;
        const float sine = std::sin(degrees * 3.14159265358979f / 180.0f);
        worst = std::max(worst, std::fabs(crm::NightFromSunHeight(sine) - mcla::gpu::volumetrics::NightFactor(degrees)));
      }
      Check(worst < 2e-4f && crm::kNightSunLow == -4.0f && crm::kNightSunHigh == 6.0f, "the night factor from the sun's height is the volumetric light's NightFactor(elevation) (worst " + std::to_string(worst) + ")");
      Check(crm::NightFromSunHeight(-1.0f) == 1.0f && crm::NightFromSunHeight(1.0f) == 0.0f, "straight under the world is night and straight over it is day");
    }
    // the frame's rows: the night factor from the sun's height where the sun is known, from the colour where it is not; the detail
    {
      crm::FrameInput in;
      in.light.valid = true;
      in.light.sun[0] = in.light.sun[1] = in.light.sun[2] = 0.05f;  // the draws' c39 at 14 h and at 23 h alike (rthit1): the colour's rule says night
      in.sun_known = true;
      const float elevation = 2.0f * 3.14159265358979f / 180.0f;
      in.to_sun_view[0] = 0.0f;
      in.to_sun_view[1] = std::sin(elevation);
      in.to_sun_view[2] = std::cos(elevation);
      uint32_t rows[crm::kFrameRows][4];
      crm::MakeFrameRows(in, rows);
      Check(Near(crm::FromBits(rows[3][2]), mcla::gpu::volumetrics::NightFactor(2.0f), 1e-3), "a sun 2 degrees over the horizon is a dusk's night factor, whatever the draws' colour says");
      in.to_sun_view[1] = std::sin(40.0f * 3.14159265358979f / 180.0f);
      in.to_sun_view[2] = std::cos(40.0f * 3.14159265358979f / 180.0f);
      crm::MakeFrameRows(in, rows);
      Check(Near(crm::FromBits(rows[3][2]), 0.0), "a sun high in the sky is day by the sun's height though the colour is dim");
      in.to_sun_view[1] = -std::sin(30.0f * 3.14159265358979f / 180.0f);
      crm::MakeFrameRows(in, rows);
      Check(Near(crm::FromBits(rows[3][2]), 1.0), "a sun under the horizon is night");
      in.world_axes_view[1][1] = 0.0f;
      in.world_axes_view[1][2] = 1.0f;  // a camera that looks straight down: the world's up is the view's z
      in.to_sun_view[1] = 0.0f;
      in.to_sun_view[2] = 1.0f;
      crm::MakeFrameRows(in, rows);
      Check(Near(crm::FromBits(rows[3][2]), 0.0), "the height is the sun's direction against the world's up in the view space, not the view's y");
      Check(Near(crm::FromBits(rows[5][3]), 1.0), "the frame's detail is in row 5's w (1 by default)");
      in.tuning.detail = 2.5f;
      crm::MakeFrameRows(in, rows);
      Check(Near(crm::FromBits(rows[5][3]), 2.5), "the detail setting is in the rows");
      in.tuning.detail = std::nanf("");
      crm::MakeFrameRows(in, rows);
      Check(Near(crm::FromBits(rows[5][3]), 0.0), "a detail that is not a number is 0");
    }

    // the shading: one input for every check below (a paint with no diffuse of its own light, to isolate each term)
    const auto base = []() {
      crm::ShadeInput in;
      in.cls = crm::Class::kPaint;
      in.light.valid = true;
      for (int c = 0; c < 3; ++c) {
        in.light.ambient[c] = 0.2f;
        in.light.sun[c] = 0.0f;
        in.tint_a[c] = in.tint_b[c] = 1.0f;
      }
      in.fresnel[0] = in.fresnel[1] = 0.0f;
      in.normal[1] = 1.0f;
      in.ray[1] = -1.0f;
      return in;
    };
    float out[3];
    {
      // the sky's openness
      crm::ShadeInput in = base();
      crm::ShadeReference(in, out);
      Check(Near(out[0], 0.2, 1e-4), "an open hit (the sky ray found nothing) is as it was");
      in.open = 0.0f;
      crm::ShadeReference(in, out);
      Check(Near(out[0], 0.2 * crm::kOpenMin, 1e-4), "a hit closed in by another car keeps kOpenMin of its ambient");
      in.tuning.detail = 0.5f;
      crm::ShadeReference(in, out);
      Check(Near(out[0], 0.2 * (1.0 - 0.5 * (1.0 - crm::kOpenMin)), 1e-4), "half the detail is half the way");
      in.tuning.detail = 0.0f;
      crm::ShadeReference(in, out);
      Check(Near(out[0], 0.2, 1e-4), "no detail: the openness does not matter");
      // the contact with the ground
      in = base();
      in.height = 0.0f;
      crm::ShadeReference(in, out);
      Check(Near(out[0], 0.2 * crm::kContactMin, 1e-4), "on the ground the ambient is kContactMin of itself");
      in.height = 0.8f;
      crm::ShadeReference(in, out);
      Check(Near(out[0], 0.2, 1e-4), "over kContactHeight the contact does nothing");
      in.height = 0.0f;
      in.open = 0.0f;
      crm::ShadeReference(in, out);
      Check(Near(out[0], 0.2 * crm::kContactMin * crm::kOpenMin, 1e-4), "closed in at the ground the two multiply");
      // the reflection of the environment is darkened by the same ground
      crm::ShadeInput glass = base();
      glass.cls = crm::Class::kOther;
      glass.tint_a[0] = glass.tint_a[1] = glass.tint_a[2] = 0.0f;
      for (int c = 0; c < 3; ++c) {
        glass.env[c] = 1.0f;
      }
      glass.fresnel[0] = glass.fresnel[1] = 0.3f;
      glass.tuning.detail = 0.0f;
      crm::ShadeReference(glass, out);
      const double open_env = out[0];
      glass.tuning.detail = 1.0f;
      glass.open = 0.0f;
      crm::ShadeReference(glass, out);
      Check(Near(out[0], open_env * crm::kOpenMin, 1e-4) && open_env > 0.0, "the environment's reflection is shut off with the sky");
      // the class's share of the ambient
      crm::ShadeInput tyre = base();
      tyre.cls = crm::Class::kTyre;
      crm::ShadeReference(tyre, out);
      Check(Near(out[0], 0.2 * 0.8, 1e-4), "a tyre keeps 0.8 of the ambient (the arch)");
      tyre.cls = crm::Class::kInterior;
      crm::ShadeReference(tyre, out);
      Check(Near(out[0], 0.2 * 0.45, 1e-4), "an interior or an underside keeps 0.45 of it (under the body)");
      tyre.tuning.detail = 0.0f;
      crm::ShadeReference(tyre, out);
      Check(Near(out[0], 0.2, 1e-4), "with no detail every class keeps all of it");
    }
    {
      // the clear coat's image of the sun by day: a lobe where the reflected ray points at the sun
      crm::ShadeInput in = base();
      for (int c = 0; c < 3; ++c) {
        in.tint_a[c] = in.tint_b[c] = 0.0f;
        in.light.ambient[c] = 0.0f;
      }
      in.light.sun[0] = 0.06f;
      in.light.sun[1] = 0.044f;
      in.light.sun[2] = 0.024f;
      in.night = 0.0f;
      in.ray[0] = 0.0f;
      in.ray[1] = -0.8f;
      in.ray[2] = 0.6f;  // down and away: the surface is a ground-facing roof tilted
      float n[3] = {0.0f, 1.0f, 0.0f};
      for (int c = 0; c < 3; ++c) {
        in.normal[c] = n[c];
      }
      const float nv = -crm::Dot3(in.normal, in.ray);
      float reflected[3];
      for (int c = 0; c < 3; ++c) {
        reflected[c] = in.ray[c] + 2.0f * nv * in.normal[c];
      }
      for (int c = 0; c < 3; ++c) {
        in.to_sun[c] = reflected[c];
      }
      crm::ShadeReference(in, out);
      const double peak = out[0];
      Check(Near(peak, 0.06 * crm::kSunRadiance * crm::CoatFresnel(nv), 1e-3) && peak > 0.0, "where the reflected ray points at the sun the coat shows the sun's colour times its radiance and the fresnel");
      Check(Near(out[1] / out[0], 0.044 / 0.06, 1e-3), "the glint has the sun's colour");
      // 10 degrees off: a tenth
      const float turn = 10.0f * 3.14159265358979f / 180.0f;
      const float off[3] = {0.0f, reflected[1] * std::cos(turn) - reflected[2] * std::sin(turn), reflected[2] * std::cos(turn) + reflected[1] * std::sin(turn)};
      float to_sun[3] = {off[0], off[1], off[2]};
      unit(to_sun);
      for (int c = 0; c < 3; ++c) {
        in.to_sun[c] = to_sun[c];
      }
      crm::ShadeReference(in, out);
      Check(out[0] < 0.2 * peak && out[0] > 0.0, "10 degrees off the lobe is a tenth of its peak or less");
      for (int c = 0; c < 3; ++c) {
        in.to_sun[c] = reflected[c];
      }
      in.sun_visible = 0.0f;
      crm::ShadeReference(in, out);
      Check(Near(out[0], 0.0, 1e-6), "a sun the cascades or a car's shadow hide has no image");
      in.sun_visible = 1.0f;
      in.tuning.detail = 0.0f;
      crm::ShadeReference(in, out);
      Check(Near(out[0], 0.0, 1e-6), "with no detail there is no glint");
      in.tuning.detail = 2.0f;
      crm::ShadeReference(in, out);
      Check(Near(out[0], 2.0 * peak, 1e-3), "the glint is scaled by the detail above 1");
      in.tuning.detail = 1.0f;
      in.cls = crm::Class::kTyre;
      crm::ShadeReference(in, out);
      Check(Near(out[0], 0.0, 1e-6), "rubber has no coat");
      in.cls = crm::Class::kChrome;
      in.fresnel[0] = 0.7f;
      in.fresnel[1] = 1.0f;
      in.fresnel[2] = 5.0f;
      for (int c = 0; c < 3; ++c) {
        in.tint_a[c] = 0.9f;
      }
      crm::ShadeReference(in, out);
      const double with_glint = out[0];
      in.tuning.detail = 0.0f;
      crm::ShadeReference(in, out);
      const float chrome_fresnel = 0.7f + 0.3f * std::pow(1.0f - nv, 5.0f);
      Check(Near(with_glint - out[0], 0.06 * crm::kSunRadiance * chrome_fresnel * 0.9, 2e-3), "chrome shows the sun by its own fresnel and tint");
      // at night the sun is gone and a lamp over the street shows where the reflected ray points up
      in = base();
      for (int c = 0; c < 3; ++c) {
        in.tint_a[c] = in.tint_b[c] = 0.0f;
        in.light.ambient[c] = 0.0f;
        in.light.sun[c] = 0.06f;
      }
      in.ray[0] = 1.0f;
      in.ray[1] = 0.0f;
      in.ray[2] = 0.0f;
      in.normal[0] = -0.70710678f;
      in.normal[1] = 0.70710678f;
      in.normal[2] = 0.0f;
      in.to_sun[0] = -0.7f;
      in.to_sun[1] = -0.5f;
      in.to_sun[2] = 0.5f;
      unit(in.to_sun);
      in.night = 1.0f;
      crm::ShadeReference(in, out);
      const double lamp_nv = 0.70710678;
      Check(Near(out[0], 0.06 * crm::kLampRadiance * crm::CoatFresnel(float(lamp_nv)), 1e-3), "at night a 45 degree shoulder mirrors a lamp straight up: the coat shows the lamp's glint");
      in.open = 0.0f;
      crm::ShadeReference(in, out);
      Check(Near(out[0], 0.0, 1e-6), "a hit closed in by another car does not see the lamp");
      in.open = 1.0f;
      in.sun_known = false;
      crm::ShadeReference(in, out);
      Check(Near(out[0], 0.0, 1e-6), "with no sun direction the frame has no hour and no lamp image");
    }
    {
      // the hour: the sun's end and the sky's night fill
      crm::ShadeInput in = base();
      in.tuning.detail = 0.0f;
      for (int c = 0; c < 3; ++c) {
        in.light.ambient[c] = 0.0f;
        in.light.sun[c] = 1.0f;
      }
      crm::ShadeReference(in, out);
      Check(Near(out[0], crm::kFillShare + 1.0, 1e-4), "by day a surface facing the sun shows the fill and the sun");
      in.night = 1.0f;
      crm::ShadeReference(in, out);
      Check(Near(out[0], crm::kNightFillShare, 1e-4), "at night the sun's light is gone and the sky's fill is kNightFillShare of its colour");
      in.night = 0.5f;
      crm::ShadeReference(in, out);
      Check(Near(out[0], 0.5 * (crm::kFillShare + crm::kNightFillShare) + 0.5, 1e-4), "at dusk both are halfway");
      in.sun_known = false;
      in.night = 0.97f;
      crm::ShadeReference(in, out);
      Check(Near(out[0], crm::kFillShare + 1.0, 1e-4), "a frame that does not know the sun keeps the sun and the day's fill (the colour's night factor only drives the lamps' glow)");
    }

    // A MODEL of the neighbour's flank as the player's door sees it (this is no picture, and its numbers say only that the pieces work together): a car side 4 m long and 1.5 m high, its body tucking
    // in under the sill and the beltline and leaning in above it, a glass band, two wheel arches; the ray goes along +x; by day in the shade of the player's car (the sky open above the player's roof line),
    // and at night. The coefficient of variation of the luminance over the flank against the shading as it was, and the mean.
    {
      struct Stats {
        double mean = 0.0;
        double cv = 0.0;
        double tail = 0.0;  // the share of the samples over four times the mean
      };
      const auto smooth = [](double a, double b, double x) {
        const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
      };
      const auto flank = [&](bool night_scene, bool detail_on) {
        std::vector<double> lum;
        for (int i = 0; i < 60; ++i) {
          const double h = 0.03 + 1.44 * double(i) / 59.0;
          // the normal's tilt over the horizon by height: under the sill, the door, the shoulder, the glass, the roof's edge
          double tilt = 0.0;
          if (h < 0.35) {
            tilt = -28.0 * (1.0 - h / 0.35);
          } else if (h < 0.75) {
            tilt = 0.0;
          } else if (h < 1.0) {
            tilt = 35.0 * (h - 0.75) / 0.25;
          } else if (h < 1.38) {
            tilt = 28.0;
          } else {
            tilt = 28.0 + 40.0 * (h - 1.38) / 0.12;
          }
          for (int j = 0; j < 80; ++j) {
            const double x = 4.0 * (double(j) + 0.5) / 80.0;
            const double yaw = (x - 2.0) / 2.0 * 30.0 * std::fabs((x - 2.0) / 2.0);  // the ends curve away
            const double pitch_rad = tilt * 3.14159265358979 / 180.0, yaw_rad = yaw * 3.14159265358979 / 180.0;
            crm::ShadeInput in;
            const bool glass = h > 1.0 && h < 1.4 && x > 0.4 && x < 3.6;
            const bool arch = h < 0.62 && (std::fabs(x - 0.8) < 0.38 || std::fabs(x - 3.2) < 0.38);
            in.cls = glass ? crm::Class::kGlass : arch ? crm::Class::kTyre : crm::Class::kPaint;
            in.paint = in.cls == crm::Class::kPaint;
            in.flop = 2.0f;
            const float a[3] = {0.8f, 0.25f, 0.05f}, b[3] = {0.2f, 0.05f, 0.01f};
            for (int c = 0; c < 3; ++c) {
              in.tint_a[c] = glass ? 0.02f : arch ? 0.04f : a[c];
              in.tint_b[c] = glass ? 0.02f : arch ? 0.04f : b[c];
            }
            in.fresnel[0] = glass ? 0.04f : arch ? 0.02f : 0.05f;
            in.fresnel[1] = glass ? 1.0f : arch ? 0.1f : 0.6f;
            in.fresnel[2] = 4.0f;
            in.light.valid = true;
            in.light.sun[0] = 0.06f;
            in.light.sun[1] = 0.044f;
            in.light.sun[2] = 0.024f;
            in.light.ambient[1] = 0.004f;
            in.light.ambient[2] = 0.004f;
            in.ray[0] = 1.0f;
            in.ray[1] = 0.0f;
            in.ray[2] = 0.0f;
            in.normal[0] = float(-std::cos(pitch_rad) * std::cos(yaw_rad));
            in.normal[1] = float(std::sin(pitch_rad));
            in.normal[2] = float(std::cos(pitch_rad) * std::sin(yaw_rad));
            in.to_sun[0] = night_scene ? -0.5f : -0.4f;  // the sun is behind the player's car: the flank is in its shadow by day
            in.to_sun[1] = night_scene ? -0.4f : 0.8f;
            in.to_sun[2] = 0.45f;
            unit(in.to_sun);
            in.sun_visible = 0.0f;
            // the environment at the reflected direction: the sky over the horizon, the buildings round it, the road under
            float r[3];
            const float twice = 2.0f * crm::Dot3(in.normal, in.ray);
            for (int c = 0; c < 3; ++c) {
              r[c] = in.ray[c] - twice * in.normal[c];
            }
            const double sky = smooth(0.15, 0.5, r[1]);
            const double road = 1.0 - smooth(-0.35, -0.05, r[1]);
            const double level = night_scene ? 0.02 : 0.35;
            const double e = 0.15 * level / 0.35 * (1.0 - sky) * (1.0 - road) + level * sky + 0.05 * level / 0.35 * road;
            for (int c = 0; c < 3; ++c) {
              in.env[c] = float(e);
            }
            in.night = night_scene ? 1.0f : 0.0f;
            in.sun_known = detail_on;
            if (!detail_on) {
              in.tuning.detail = 0.0f;
              in.night = night_scene ? 0.97f : 0.0f;
            } else {
              in.open = float(smooth(0.7, 1.15, h));  // the player's roof line over the flank
              in.height = float(h);
            }
            float rgb[3];
            crm::ShadeReference(in, rgb);
            lum.push_back(0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]);
          }
        }
        Stats s;
        for (const double v : lum) {
          s.mean += v;
        }
        s.mean /= double(lum.size());
        double var = 0.0;
        size_t over = 0;
        for (const double v : lum) {
          var += (v - s.mean) * (v - s.mean);
          over += v > 4.0 * s.mean ? 1u : 0u;
        }
        s.cv = std::sqrt(var / double(lum.size())) / s.mean;
        s.tail = double(over) / double(lum.size());
        return s;
      };
      const Stats day_old = flank(false, false), day_new = flank(false, true), night_old = flank(true, false), night_new = flank(true, true);
      std::printf("  the flank model: day CV %.2f -> %.2f (mean %.5f -> %.5f, %.1f%% over 4 x the mean), night CV %.2f -> %.2f (mean %.5f -> %.5f, %.1f%% over 4 x the mean)\n", day_old.cv, day_new.cv, day_old.mean,
                  day_new.mean, 100.0 * day_new.tail, night_old.cv, night_new.cv, night_old.mean, night_new.mean, 100.0 * night_new.tail);
      Check(day_new.cv > 1.2 * day_old.cv, "the flank model: by day the luminance varies at least a fifth more across the flank with the detail (CV " + std::to_string(day_old.cv) + " -> " + std::to_string(day_new.cv) + ")");
      Check(day_new.mean > 0.6 * day_old.mean && day_new.mean < 1.4 * day_old.mean, "the flank model: the detail does not move the flank's mean far (by day " + std::to_string(day_new.mean / day_old.mean) + " times)");
      Check(night_new.cv > 1.5 * night_old.cv && night_new.tail > 0.005, "the flank model: at night the lamp's glint is a heavy tail the flank had not (CV " + std::to_string(night_old.cv) + " -> " + std::to_string(night_new.cv) + ", " +
                                                                              std::to_string(100.0 * night_new.tail) + "% of the samples over 4 x the mean)");
      Check(night_new.mean > night_old.mean, "the flank model: the sky's night fill makes the flank at night brighter");
    }
  }

  // ---- the instance list's materials ----------------------------------------------------------------------------------------------------
  {
    const auto draw = [](uint32_t id, uint64_t key, double x) {
      ci::OtherDraw d;
      d.stencil_id = id;
      d.mesh_key = key;
      d.ps = 1;
      d.indices = 300;
      d.model_to_world = tp::Identity();
      d.model_to_world.m[3][0] = x;
      d.model_to_world.m[3][2] = 5.0;
      return d;
    };
    ci::OtherCar drawn, walked;
    drawn.stencil_id = 3;
    walked.stencil_id = ci::kIdSpace - 1;
    walked.remembered = true;
    walked.walked = true;
    walked.has_paint_ab = true;
    for (int c = 0; c < 3; ++c) {
      walked.paint_a[c] = 0.5f;
      walked.paint_b[c] = 0.1f;
    }
    std::vector<ci::OtherDraw> draws;
    ci::OtherDraw a = draw(3, 10, 0.0);
    a.material = crm::Capture(cd::kCarPaintPs, nullptr, [](uint32_t) { return crm::FetchInfo{}; });
    ci::OtherDraw b = draw(3, 11, 0.5);  // a shader the table does not know: class none
    ci::OtherDraw c = draw(ci::kIdSpace - 1, 20, 3.0);
    c.part = uint16_t(cw::kBodyPart);
    ci::OtherDraw t = draw(ci::kIdSpace - 1, 21, 3.5);
    t.part = uint16_t(cw::kTyrePart);
    ci::OtherDraw n = draw(ci::kIdSpace - 1, 22, 3.8);  // a walked part with no part slot and no material: class none
    draws = {a, b, c, t, n};
    drawn.Add(a.model_to_world, 2.0);
    drawn.Add(b.model_to_world, 2.0);
    walked.Add(c.model_to_world, 3.0);
    walked.Add(t.model_to_world, 3.0);
    walked.Add(n.model_to_world, 3.0);
    ci::List list;
    const tp::Mat4 view = tp::Identity();
    ci::AppendOthers(&list, draws, {drawn, walked}, view, false, 640, mcla::gpu::car_rt_rules::kMaxInstances,
                     [](uint64_t key, ci::MeshRef* ref) {
                       ref->first_index = 0;
                       ref->first_vertex = 0;
                       ref->has_uv = key != 11 && key != 22;
                       ref->uv_density = ref->has_uv ? 0.5f : 0.0f;
                       return true;
                     },
                     0, 2.0f);
    Check(list.items.size() == 5 && list.materials.size() == list.items.size(), "the list has a material for every item, in the items' order");
    if (list.materials.size() == 5) {
      Check(list.materials[0].material.cls == crm::Class::kPaint && list.materials[0].has_uv && Near(list.materials[0].uv_density, 0.5), "a drawn paint draw keeps its material and its mesh's texture coordinates");
      Check(list.materials[1].material.cls == crm::Class::kNone && !list.materials[1].has_uv, "a draw of a shader the table does not know stays class none: the pass keeps today's stand-in");
      Check(list.materials[2].material.cls == crm::Class::kPaint && Near(list.materials[2].material.tint_a[0], 0.25) && Near(list.materials[2].material.tint_b[0], 0.01) &&
                (list.materials[2].material.flags & crm::kFlagPaint) != 0,
            "a walked body takes the car's record colours raised to the gamma the list was given (2)");
      Check(list.materials[3].material.cls == crm::Class::kTyre && !list.materials[3].material.diffuse.texture, "a walked tyre is the tyre class with no texture");
      Check(list.materials[4].material.cls == crm::Class::kNone, "a walked draw with no part slot has no material");
    }
    ci::List cleared = list;
    cleared.Clear();
    Check(cleared.materials.empty() && cleared.items.empty(), "Clear empties the materials with the items");
  }

  // ---- the sun's cascades: the frame's rows, the collector's lookup, the world point ------------------------------------------------------------------
  {
    // the rows
    crm::FrameInput in;
    in.light.valid = true;
    in.light.sun[0] = in.light.sun[1] = in.light.sun[2] = 1.0f;
    in.sun_known = true;
    in.camera_known = true;
    in.camera_world[0] = 1234.5f;
    in.camera_world[1] = 10.0f;
    in.camera_world[2] = -987.25f;
    crm::SunCascades& sc = in.cascades;
    sc.valid = true;
    sc.atlas_slot = 0x42;
    sc.count = 4;
    sc.fade = 0.25f;
    sc.depth_bias = 0.001f;
    sc.right[0] = 1.0f;
    sc.top[2] = 1.0f;
    for (uint32_t i = 0; i < 4; ++i) {
      for (uint32_t j = 0; j < 4; ++j) {
        for (uint32_t c = 0; c < 4; ++c) {
          sc.rows[i][j][c] = float(100 * i + 10 * j + c);
        }
      }
      sc.box[i][0] = 1.0f + float(i);
      sc.box[i][1] = 10.0f * float(i + 1);
      sc.box[i][2] = -2.0f;
      sc.box[i][3] = 20.0f * float(i + 1);
    }
    uint32_t rows[crm::kFrameRows][4];
    crm::MakeFrameRows(in, rows);
    Check(crm::kFrameRows == 32 && crm::kFrameCascade == 16 && crm::kCascadeFirstRow + crm::kMaxCascades * crm::kCascadeRowsEach == crm::kFrameRows, "the frame has 32 rows: 8 of light, 4 of cascade header and axes, 4 cascades of 5");
    Check((rows[0][3] & crm::kFrameCascade) != 0 && crm::CascadesUsable(sc), "a frame with the cascades and the camera's place has the cascade flag");
    Check(rows[8][0] == 0x42 && rows[8][1] == 4 && Near(crm::FromBits(rows[8][2]), 0.001) && Near(crm::FromBits(rows[8][3]), 0.25), "row 8 holds the atlas's slot, the count, the depth bias and the fade");
    Check(Near(crm::FromBits(rows[9][0]), 1234.5) && Near(crm::FromBits(rows[9][1]), 10.0) && Near(crm::FromBits(rows[9][2]), -987.25), "row 9 holds the camera's place in the world");
    Check(Near(crm::FromBits(rows[10][0]), 1.0) && Near(crm::FromBits(rows[11][2]), 1.0) && Near(crm::FromBits(rows[10][2]), 0.0), "rows 10 and 11 hold the right and top axes");
    bool matrices = true, boxes = true;
    for (uint32_t i = 0; i < 4; ++i) {
      for (uint32_t j = 0; j < 4; ++j) {
        for (uint32_t c = 0; c < 4; ++c) {
          matrices = matrices && Near(crm::FromBits(rows[crm::kCascadeFirstRow + 5 * i + j][c]), double(100 * i + 10 * j + c));
        }
      }
      boxes = boxes && Near(crm::FromBits(rows[crm::kCascadeFirstRow + 5 * i + 4][0]), 1.0 + double(i)) && Near(crm::FromBits(rows[crm::kCascadeFirstRow + 5 * i + 4][3]), 20.0 * double(i + 1));
    }
    Check(matrices && boxes, "cascade i's four matrix rows are rows 12 + 5 i .. +3 and its box row 12 + 5 i + 4");
    // the rows around it are untouched: the lighting is as before
    Check(Near(crm::FromBits(rows[0][1]), 1.0) && Near(crm::FromBits(rows[1][0]), 1.0), "the lighting rows are as they were");
    auto flags_of = [&](const crm::FrameInput& f) {
      uint32_t r[crm::kFrameRows][4];
      crm::MakeFrameRows(f, r);
      return r[0][3];
    };
    crm::FrameInput x = in;
    x.tuning.cascade = false;
    Check((flags_of(x) & crm::kFrameCascade) == 0, "the switch off: no cascade flag");
    x = in;
    x.camera_known = false;
    Check((flags_of(x) & crm::kFrameCascade) == 0, "no camera place: no cascade flag (a hit's world point is not known)");
    x = in;
    x.cascades.valid = false;
    Check((flags_of(x) & crm::kFrameCascade) == 0, "cascades the collector did not give: no flag");
    x = in;
    x.cascades.atlas_slot = crm::kNoSlot;
    Check((flags_of(x) & crm::kFrameCascade) == 0, "no atlas slot: no flag");
    x = in;
    x.cascades.count = 0;
    Check((flags_of(x) & crm::kFrameCascade) == 0, "no cascade: no flag");
    x = in;
    x.cascades.rows[1][2][3] = std::nanf("");
    Check((flags_of(x) & crm::kFrameCascade) == 0, "a matrix that is not finite: no flag");
    x = in;
    for (uint32_t i = 0; i < 4; ++i) {
      x.cascades.box[i][1] = 0.0f;
    }
    Check((flags_of(x) & crm::kFrameCascade) == 0, "boxes with no extent (the cascades of a night or a storm: not drawn): no flag");
    x = in;
    x.cascades.count = 9;
    Check((flags_of(x) & crm::kFrameCascade) == 0, "a count over the four: no flag");
    // a frame with no cascade rows leaves rows 8 to 31 zero
    crm::FrameInput none = in;
    none.tuning.cascade = false;
    uint32_t zero_rows[crm::kFrameRows][4];
    crm::MakeFrameRows(none, zero_rows);
    bool all_zero = true;
    for (uint32_t r = 8; r < crm::kFrameRows; ++r) {
      for (uint32_t c = 0; c < 4; ++c) {
        all_zero = all_zero && zero_rows[r][c] == 0u;
      }
    }
    Check(all_zero, "a frame without the cascade flag has rows 8 to 31 zero");
  }
  {
    // the collector's lookup over a synthetic atlas: two nested cascades, a sun straight down
    //   cascade 0: the box +-10 m around the origin on x and z, u = (x + 10) / 20, v = (z + 10) / 20, depth = 0.5 - 0.01 y
    //   cascade 1: the box +-40 m, u = (x + 40) / 80, v = (z + 40) / 80, the same depth
    // the atlas is 64 x 64: 1 (nothing drawn) everywhere but a block of texels 30..33 that holds an occluder at depth 0.3 (y = 20 m)
    crm::SunCascades sc;
    sc.valid = true;
    sc.atlas_slot = 7;
    sc.count = 2;
    sc.right[0] = 1.0f;
    sc.top[2] = 1.0f;
    for (int i = 0; i < 2; ++i) {
      const float half = i == 0 ? 10.0f : 40.0f;
      sc.box[i][0] = 0.0f;
      sc.box[i][1] = half;
      sc.box[i][2] = 0.0f;
      sc.box[i][3] = half;
      sc.rows[i][0][0] = 1.0f / (2.0f * half);  // u from x
      sc.rows[i][3][0] = 0.5f;
      sc.rows[i][2][1] = 1.0f / (2.0f * half);  // v from z
      sc.rows[i][3][1] = 0.5f;
      sc.rows[i][1][2] = -0.01f;                // depth from y
      sc.rows[i][3][2] = 0.5f;
      sc.rows[i][3][3] = 1.0f;
    }
    auto atlas = [](int x, int y) { return (x >= 30 && x <= 33 && y >= 30 && y <= 33) ? 0.3f : 1.0f; };
    auto lit = [&](float x, float y, float z) {
      const float p[3] = {x, y, z};
      return crm::CascadeLit(sc, p, 64, 64, atlas);
    };
    // texel coordinate = u * 64 - 0.5, u = (x + 10) / 20 in cascade 0: x = 0.15625 is texel 32 exactly
    Check(Near(lit(0.15625f, 0.0f, 0.15625f), 0.0), "a point on the road under the occluder is shadowed");
    Check(Near(lit(0.15625f, 30.0f, 0.15625f), 1.0), "a point above the occluder (a smaller depth than the stored one) is lit");
    Check(Near(lit(-5.0f, 0.0f, -5.0f), 1.0), "a point away from the occluder is lit");
    Check(Near(lit(-0.625f, 0.0f, 0.15625f), 0.5), "at the occluder's edge the compare is bilinear: half of the weights lit");
    Check(Near(lit(20.0f, 0.0f, 0.0f), 1.0), "a point outside the first box and inside the second reads the second cascade (its texels there hold nothing)");
    Check(Near(lit(100.0f, 0.0f, 0.0f), 1.0) && Near(lit(0.0f, 0.0f, 100.0f), 1.0), "a point in none of the boxes has no shadow data: lit");
    // the finest box that holds the point: (-1, 0, -1) is in both boxes; its texel is 28.3 in cascade 0 (nothing drawn there) and 30.7 in cascade 1 (the occluder's)
    {
      const float p0[3] = {-1.0f, 0.0f, -1.0f};
      const float tc0 = ((p0[0] + 10.0f) / 20.0f) * 64.0f - 0.5f;
      const float tc1 = ((p0[0] + 40.0f) / 80.0f) * 64.0f - 0.5f;
      Check(tc0 < 29.0 && tc1 > 30.0 && tc1 < 31.0, "the same point has a texel clear of the occluder in cascade 0 and the occluder's in cascade 1");
      Check(Near(crm::CascadeLit(sc, p0, 64, 64, atlas), 1.0), "and the lookup takes cascade 0, the finest box that holds it (lit)");
      crm::SunCascades only_coarse = sc;
      only_coarse.box[0][1] = 0.1f;  // the first cascade's box no longer holds the point
      Check(Near(crm::CascadeLit(only_coarse, p0, 64, 64, atlas), 0.0), "with the first box too small the second cascade answers (the occluder's texel: shadowed)");
    }
    // the box tests use the offsets: |r + box.x| < half
    {
      crm::SunCascades moved = sc;
      moved.box[0][0] = -50.0f;  // |x - 50| < 10 holds for x in (40, 60)
      const float inside[3] = {50.0f, 0.0f, 0.0f};
      const float outside[3] = {0.15625f, 0.0f, 0.15625f};
      uint32_t reads = 0;
      auto counting = [&](int x, int y) {
        ++reads;
        return atlas(x, y);
      };
      (void)crm::CascadeLit(moved, inside, 64, 64, counting);
      Check(reads == 4, "a lookup reads four depth texels");
      Check(Near(crm::CascadeLit(moved, outside, 64, 64, atlas), 0.0), "the offset moves the box: the origin is in the second box now (shadowed there too)");
    }
    // the depth bias
    {
      crm::SunCascades biased = sc;
      biased.depth_bias = -0.3f;
      const float p[3] = {0.15625f, 0.0f, 0.15625f};
      Check(Near(crm::CascadeLit(biased, p, 64, 64, atlas), 1.0), "a bias that takes the receiver in front of the occluder lights it");
      biased.depth_bias = 0.25f;
      Check(Near(crm::CascadeLit(biased, p, 64, 64, atlas), 0.0), "a positive bias keeps it in shadow");
    }
    // a texel off the atlas is clamped, as the shader's
    {
      const float edge[3] = {-9.9f, 0.0f, -9.9f};
      Check(Near(crm::CascadeLit(sc, edge, 64, 64, atlas), 1.0), "the corner of the first cascade reads clamped texels (nothing there)");
    }
    crm::SunCascades empty;
    const float q[3] = {0.0f, 0.0f, 0.0f};
    Check(Near(crm::CascadeLit(empty, q, 64, 64, atlas), 1.0), "no cascades: lit");
    // the sun's visibility: the cascades with the collector's fade, and the cars' ray; the darker
    Check(Near(crm::SunVisibility(0.0f, 0.0f, 1.0f), 0.0) && Near(crm::SunVisibility(0.0f, 0.5f, 1.0f), 0.5) && Near(crm::SunVisibility(0.0f, 1.0f, 1.0f), 1.0),
          "the collector's fade lights what the cascades shadow (visibility is lerp(lit, 1, fade))");
    Check(Near(crm::SunVisibility(1.0f, 0.0f, 0.0f), 0.0) && Near(crm::SunVisibility(0.7f, 0.0f, 0.4f), 0.4) && Near(crm::SunVisibility(0.2f, 0.0f, 0.9f), 0.2), "the darker of the cascades' share and the cars' ray");
  }
  {
    // the camera's place and the world point of a view-space point: a rigid motion, with and without the z flip
    std::mt19937 rng(20261120);
    std::uniform_real_distribution<double> unit(-1.0, 1.0);
    bool camera_ok = true, round_trip = true, axes_ok = true;
    double worst = 0.0;
    for (int trial = 0; trial < 200; ++trial) {
      // an orthonormal basis from two random vectors (Gram-Schmidt), the third their cross product
      double a[3] = {unit(rng), unit(rng), unit(rng)}, b[3] = {unit(rng), unit(rng), unit(rng)};
      auto norm = [](double v[3]) {
        const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        for (int c = 0; c < 3; ++c) {
          v[c] /= n;
        }
      };
      norm(a);
      const double d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
      for (int c = 0; c < 3; ++c) {
        b[c] -= d * a[c];
      }
      norm(b);
      const double cr[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
      tp::Mat4 view = tp::Identity();
      for (int c = 0; c < 3; ++c) {
        view.m[0][c] = a[c];  // the rows of the matrix are what the world's axes become (a row vector times the matrix)
        view.m[1][c] = b[c];
        view.m[2][c] = cr[c];
        view.m[3][c] = 5000.0 * unit(rng);
      }
      const bool flip = (trial & 1) != 0;
      double cam[3];
      camera_ok = camera_ok && ci::CameraWorld(view, cam);
      float at_camera[3];
      camera_ok = camera_ok && ci::ViewPoint(view, flip, cam, at_camera) && std::fabs(at_camera[0]) < 0.02 && std::fabs(at_camera[1]) < 0.02 && std::fabs(at_camera[2]) < 0.02;
      float axes[3][3];
      for (int k = 0; k < 3; ++k) {
        const double e[3] = {k == 0 ? 1.0 : 0.0, k == 1 ? 1.0 : 0.0, k == 2 ? 1.0 : 0.0};
        axes_ok = axes_ok && ci::ViewVector(view, flip, e, axes[k]);
      }
      const float camf[3] = {float(cam[0]), float(cam[1]), float(cam[2])};
      for (int point = 0; point < 5; ++point) {
        const double p[3] = {cam[0] + 40.0 * unit(rng), cam[1] + 40.0 * unit(rng), cam[2] + 40.0 * unit(rng)};
        float v[3], w[3];
        round_trip = round_trip && ci::ViewPoint(view, flip, p, v);
        crm::WorldPoint(camf, axes, v, w);
        for (int c = 0; c < 3; ++c) {
          worst = std::max(worst, std::fabs(double(w[c]) - p[c]));
        }
      }
    }
    Check(camera_ok, "the camera's place is the world point whose view coordinates are 0 (flip or not)");
    Check(axes_ok && round_trip && worst < 0.03, "a view-space point turned back by the camera's place and the world's axes in view space is the world point (within the float's own error at 5 km)");
    std::printf("  the view -> world round trip: the largest error %.4f m over 1000 points at up to 5 km\n", worst);
    tp::Mat4 broken = tp::Identity();
    broken.m[3][1] = std::nan("");
    double c3[3];
    Check(!ci::CameraWorld(broken, c3), "a view matrix that is not finite has no camera place");
  }

  // ---- the shaders, the host and the settings hold the numbers ------------------------------------------------------------------------------------
  {
    const std::string hlsli = ReadFile(src + "/gpu/hlsl/car_rt_hit_shade.hlsli");
    const std::string refl = ReadFile(src + "/gpu/hlsl/car_rt_refl_cs.hlsl");
    const std::string scene = ReadFile(src + "/gpu/car_rt_scene.cpp");
    const std::string scene_h = ReadFile(src + "/gpu/car_rt_scene.h");
    const std::string frame_cpp = ReadFile(src + "/gpu/car_frame.cpp");
    const std::string settings = ReadFile(src + "/settings/generated/settings_gpu.h");
    const std::string manifest = ReadFile(src + "/settings/generated/manifest.json");
    const std::string cmake = ReadFile(src + "/gpu/CMakeLists.txt");
    if (hlsli.empty() || refl.empty() || scene.empty() || settings.empty()) {
      std::printf("the sources were not found under %s: the text checks are skipped\n", src.c_str());
    } else {
      Check(Has(hlsli, "#define CAR_MAT_CLASS_NONE 0") && Has(hlsli, "#define CAR_MAT_CLASS_PAINT 1") && Has(hlsli, "#define CAR_MAT_CLASS_GLASS 2") && Has(hlsli, "#define CAR_MAT_CLASS_TYRE 3") &&
                Has(hlsli, "#define CAR_MAT_CLASS_CHROME 4") && Has(hlsli, "#define CAR_MAT_CLASS_LIGHT 5") && Has(hlsli, "#define CAR_MAT_CLASS_INTERIOR 6") && Has(hlsli, "#define CAR_MAT_CLASS_OTHER 7"),
            "the shader's classes are the header's (0 none, 1 paint, 2 glass, 3 tyre, 4 chrome, 5 light, 6 interior, 7 other)");
      Check(Has(hlsli, "#define CAR_MAT_FLAG_PAINT 1") && Has(hlsli, "#define CAR_MAT_FLAG_DECAL 2") && Has(hlsli, "#define CAR_MAT_FLAG_UV 4") && Has(hlsli, "#define CAR_MAT_FLAG_EMISSIVE 8") &&
                crm::kFlagPaint == 1 && crm::kFlagDecal == 2 && crm::kFlagUv == 4 && crm::kFlagEmissive == 8,
            "the shader's material flags are the header's");
      Check(Has(hlsli, "#define CAR_MAT_FRAME_SHADE 1") && Has(hlsli, "#define CAR_MAT_FRAME_SHADOW 2") && Has(hlsli, "#define CAR_MAT_FRAME_ENV 4") && Has(hlsli, "#define CAR_MAT_FRAME_SUN 8") &&
                Has(hlsli, "#define CAR_MAT_FRAME_CASCADE 16") && crm::kFrameShade == 1 && crm::kFrameShadow == 2 && crm::kFrameEnv == 4 && crm::kFrameSun == 8 && crm::kFrameCascade == 16,
            "the shader's frame flags are the header's");
      Check(Near(Constant(hlsli, "static const uint kMatCascadeHeaderRow"), crm::kCascadeHeaderRow) && Near(Constant(hlsli, "static const uint kMatCameraRow"), crm::kCameraRow) &&
                Near(Constant(hlsli, "static const uint kMatCascadeRightRow"), crm::kCascadeRightRow) && Near(Constant(hlsli, "static const uint kMatCascadeTopRow"), crm::kCascadeTopRow) &&
                Near(Constant(hlsli, "static const uint kMatCascadeFirstRow"), crm::kCascadeFirstRow) && Near(Constant(hlsli, "static const uint kMatCascadeRowsEach"), crm::kCascadeRowsEach) &&
                Near(Constant(hlsli, "static const uint kMatMaxCascades"), crm::kMaxCascades) && Near(Constant(hlsli, "static const float kCascadeNormalOffset"), crm::kCascadeNormalOffset),
            "the shader's cascade rows and the normal offset of the lookup are the header's");
      Check(Has(hlsli, "const float r = dot(p, right);") && Has(hlsli, "const float t = dot(p, top);") && Has(hlsli, "inside += (abs(r + box.x) < box.y && abs(t + box.z) < box.w) ? 1u : 0u;") &&
                Has(hlsli, "const uint base = kMatCascadeFirstRow + (cascades - inside) * kMatCascadeRowsEach;") && Has(hlsli, "const float u = dot(p, float3(r0.x, r1.x, r2.x)) + r3.x;") &&
                Has(hlsli, "const float v = dot(p, float3(r0.y, r1.y, r2.y)) + r3.y;") &&
                Has(hlsli, "const float depth = (dot(p, float3(r0.z, r1.z, r2.z)) + r3.z) * (dot(p, float3(r0.w, r1.w, r2.w)) + r3.w) + asfloat(header.z);") &&
                Has(hlsli, "const float2 tc = float2(u, v) * float2(size) - 0.5;") && Has(hlsli, ">= depth ? 1.0 : 0.0;") &&
                Has(hlsli, "return lerp(lerp(s00, s10, fr.x), lerp(s01, s11, fr.x), fr.y);"),
            "the shader's cascade lookup is CascadeLit's (the volumetric light's, copied): the boxes, the finest that holds the point, the matrix, the bilinear compare");
      Check(Has(hlsli, "const float3 world = camera + float3(dot(lifted, f.axisX), dot(lifted, f.axisY), dot(lifted, f.axisZ));") && Has(hlsli, "const float3 lifted = hit + normal * kCascadeNormalOffset;") &&
                Has(hlsli, "visible = lit + (1.0 - lit) * saturate(asfloat(gCarRtMaterials[kMatCascadeHeaderRow].w));") && Has(hlsli, "visible = min(visible, CarMatSunVisible(") &&
                Has(hlsli, "asfloat(gCarRtMaterials[kMatCameraRow].xyz)"),
            "the shader's sun visibility is SunVisibility's: the cascades at the hit's world point (WorldPoint), the collector's fade, the darker with the cars' ray");
      Check(Near(Constant(hlsli, "static const uint kMatFrameRows"), double(crm::kFrameRows)) && Near(Constant(hlsli, "static const uint kMatRows"), double(crm::kMaterialRows)) &&
                Near(Constant(hlsli, "static const float kMatTintMax"), crm::kTintMax) && Near(Constant(hlsli, "static const float kConeLodScale"), crm::kConeLodScale) &&
                Near(Constant(hlsli, "static const float kEmissiveGain"), crm::kEmissiveGain),
            "the shader's row counts, packing range, cone scale and emissive gain are the header's");
      const std::vector<double> fmin = Array(hlsli, "static const float kClassFresnelMin"), fmax = Array(hlsli, "static const float kClassFresnelMax");
      const std::vector<double> lod = Array(hlsli, "static const float kClassEnvLod"), spec = Array(hlsli, "static const float kClassSpecGain");
      const std::vector<double> diffuse = Array(hlsli, "static const float kClassDiffuseGain");
      bool classes = fmin.size() == 8 && fmax.size() == 8 && lod.size() == 8 && spec.size() == 8 && diffuse.size() == 8;
      for (uint32_t c = 0; c < crm::kClassCount && classes; ++c) {
        const crm::ClassDefaults& d = crm::kDefaults[c];
        classes = Near(fmin[c], d.fresnel_min) && Near(fmax[c], d.fresnel_max) && Near(lod[c], d.env_lod) && Near(spec[c], d.spec_gain) && Near(diffuse[c], d.diffuse_gain);
      }
      Check(classes, "the shader's class tables (fresnel min and max, environment level, gains) are the header's defaults");
      Check(Has(hlsli, "float(v & 1023u), float((v >> 10) & 1023u), float((v >> 20) & 1023u)") && Has(hlsli, "(kMatTintMax / 1023.0)"), "the shader unpacks a colour as PackRgb10 packs it");
      Check(Has(hlsli, "m.cls = r0.x & 0xFu;") && Has(hlsli, "m.flags = (r0.x >> 4) & 0xFu;") && Has(hlsli, "float((r0.x >> 8) & 0xFFu) / 255.0") && Has(hlsli, "float((r0.x >> 16) & 0xFFu) / 255.0") &&
                Has(hlsli, "float((r0.x >> 24) & 0xFFu) / 16.0") && Has(hlsli, "m.diffuse = r0.y;") && Has(hlsli, "m.tintA = CarMatRgb10(r0.z);") && Has(hlsli, "m.tintB = CarMatRgb10(r0.w);") &&
                Has(hlsli, "m.flop = asfloat(r1.x);") && Has(hlsli, "m.lodBase = asfloat(r1.y);") && Has(hlsli, "m.emissive = asfloat(r1.z);"),
            "the shader reads the instance's two rows where MakeRows writes them");
      Check(Has(hlsli, "f.toSun = asfloat(r0.xyz);") && Has(hlsli, "f.flags = r0.w;") && Has(hlsli, "f.sun = asfloat(r1.xyz);") && Has(hlsli, "f.shadowReach = asfloat(r1.w);") && Has(hlsli, "f.ambient = asfloat(r2.xyz);") &&
                Has(hlsli, "f.ambientGain = asfloat(r2.w);") && Has(hlsli, "f.envSharp = r3.x;") && Has(hlsli, "f.envBlur = r3.y;") && Has(hlsli, "f.night = asfloat(r3.z);") && Has(hlsli, "f.specGain = asfloat(r3.w);") &&
                Has(hlsli, "f.sky = asfloat(r4.xyz);") && Has(hlsli, "f.gain = asfloat(r4.w);") && Has(hlsli, "gCarRtMaterials[5].xyz") && Has(hlsli, "gCarRtMaterials[6].xyz") && Has(hlsli, "gCarRtMaterials[7].xyz"),
            "the shader reads the frame's rows where MakeFrameRows writes them");
      Check(Has(hlsli, "const float a = -r.y;") && Has(hlsli, "const float b = -r.z;") && Has(hlsli, "const float c = -r.x;") && Has(hlsli, "uv = float2(0.25 - c / (4.0 + 4.0 * b), 0.5 + a / (2.0 + 2.0 * b));") &&
                Has(hlsli, "uv = float2(0.75 + c / (4.0 - 4.0 * b), 0.5 + a / (2.0 - 2.0 * b));"),
            "the shader's paraboloid mapping is EnvUv's");
      Check(Has(hlsli, "ByteAddressBuffer gCarRtUvs : register(t4, CAR_RT_SPACE_NAME);") && Has(hlsli, "StructuredBuffer<uint4> gCarRtMaterials : register(t5, CAR_RT_SPACE_NAME);") &&
                Has(scene, "addByteAddressBuffer(4)") && Has(scene, "addStructuredBuffer(5)") && Has(scene, "CreateDescriptorSet(set_builder_, 6, 0"),
            "the shader's bindings 4 and 5 are the host's (a byte address buffer and a structured buffer, six views in the set)");
      Check(Has(hlsli, "f16tof32(p0 & 0xFFFFu), f16tof32(p0 >> 16)"), "the shader reads a texture coordinate pair as PackUv packs it (u low)");
      Check(Has(hlsli, "const float hemisphere = clamp(0.6 + 0.4 * dot(normal, up), 0.2, 1.0);") && Has(hlsli, "pow(max(1.0 - nv, 1e-4), max(m.fresnelPower, 0.01))") &&
                Has(hlsli, "result += albedo * (m.emissive * kEmissiveGain * f.night);") && Has(hlsli, "albedo = lerp(m.tintB, m.tintA, pow(max(nv, 1e-4), m.flop));"),
            "the shader's lighting is ShadeReference's (the hemisphere term, the fresnel, the emissive, the paint's gradient)");
      Check(Has(refl, "#include \"car_rt_hit_shade.hlsli\"") && Has(refl, "static const bool kShadePlayerHits = true;") && Has(refl, "isShaded = CarHitShade(instance, primitive, query.CommittedTriangleBarycentrics(), hitNormal, direction, hit, footprint, coneTan, g_push.up.w, CarHitHeight(hitInstance, hitsOther, hit),") &&
                Has(refl, "if (seenShare < 1.0 && usable && (hitsOther || kShadePlayerHits))"),
            "the trace shades a hit on another car (and, in the opponents' variant, on the player's) only where the history does not show it in full");
      Check(Has(refl, "hidden = shaded;") && Has(refl, "else if (hitsOther)") && Has(refl, "hidden *= g_push.tune.x;") && Has(refl, "const float known = OtherCarColour(history, CarRtLoadCar(hitInstance), hidden);"),
            "a shaded hit takes no shade factor, and today's stand-in is what is left for a hit with no material");
      Check(Has(scene, "ReadTexcoords(&staged.uv_floats)") && Has(scene, "crm::MakeFrameRows(in, rows);") && Has(scene, "textures_->Peek2D(texture)") && Has(scene, "res.set->setBuffer(uvs_index_, uvs_.buffer.get(), 0);") &&
                Has(scene, "res.set->setBuffer(materials_index_, res.materials.get()") && Has(scene_h, "Shared uvs_;"),
            "the scene reads the texture coordinates once a mesh, writes the frame's rows every frame, asks the cache for the textures and binds both buffers");
      Check(Has(frame_cpp, "if (others_on_ && !skinned) {") && Has(frame_cpp, "out.material = MaterialOf(d);") && Has(frame_cpp, "ReadRegister(ps, 39, sun) && car_rt_material::ReadRegister(ps, 26, ambient)") &&
                Has(frame_cpp, "car_rt_material::ReadRegister(ps, 57, sky)"),
            "the car frame reads the materials only while the other cars are asked for, and the light from the pixel constants c39, c26 and c57");
      for (const char* name : {"mcla_gpu_car_rt_hit_shading", "mcla_gpu_car_rt_hit_shadow", "mcla_gpu_car_rt_hit_gain", "mcla_gpu_car_rt_hit_ambient", "mcla_gpu_car_rt_hit_spec", "mcla_gpu_car_rt_hit_cascade"}) {
        Check(Has(settings, std::string("REX_SETTING(") ) && Has(settings, std::string(", ") + name + ", ") && Has(settings, std::string("inline auto& ") + name + " = g_gpu." + name + ";") &&
                  Has(manifest, std::string("\"") + name + "\": {") && Has(scene, std::string("larecomp::settings::") + name),
              std::string("the setting ") + name + " is defined, aliased, in the manifest and read by the scene");
      }
      Check(Has(scene, "crm::MakeFrameRows(in, rows);") && Has(scene, "in.cascades = sun_cascades_;") && Has(scene, "car_rt_instances::CameraWorld(frame.view, camera)") &&
                Has(scene_h, "void SetSunCascades(const car_rt_material::SunCascades& cascades) { sun_cascades_ = cascades; }"),
            "the scene puts the cascades it was given and the camera's world place into the frame's rows");
      // RT-HIT-SHADING-5: the shader's numbers, tables and arithmetic are the header's
      Check(Near(Constant(hlsli, "static const float kNightFillShare"), crm::kNightFillShare) && Near(Constant(hlsli, "static const float kCoatF0"), crm::kCoatF0) && Near(Constant(hlsli, "static const float kCoatPower"), crm::kCoatPower) &&
                Near(Constant(hlsli, "static const float kSunRadiance"), crm::kSunRadiance) && Near(Constant(hlsli, "static const float kSunGlintPower"), crm::kSunGlintPower) &&
                Near(Constant(hlsli, "static const float kLampRadiance"), crm::kLampRadiance) && Near(Constant(hlsli, "static const float kLampGlintPower"), crm::kLampGlintPower) &&
                Near(Constant(hlsli, "static const float kOpenMin"), crm::kOpenMin) && Near(Constant(hlsli, "static const float kOpenReach"), crm::kOpenReach) && Near(Constant(hlsli, "static const float kOpenJitter"), crm::kOpenJitter) &&
                Near(Constant(hlsli, "static const float kOpenMinAbove"), crm::kOpenMinAbove) && Near(Constant(hlsli, "static const float kOpenHashCells"), crm::kOpenHashCells) &&
                Near(Constant(hlsli, "static const float kContactMin"), crm::kContactMin) && Near(Constant(hlsli, "static const float kContactHeight"), crm::kContactHeight) &&
                Near(Constant(hlsli, "static const float kCarCentreRise"), crm::kCarCentreRise),
            "the shader's detail numbers (the night fill, the coat's fresnel, the glints, the sky's openness, the contact) are the header's");
      const std::vector<double> class_ambient = Array(hlsli, "static const float kClassAmbient"), class_glint = Array(hlsli, "static const float kClassGlint");
      bool detail_classes = class_ambient.size() == 8 && class_glint.size() == 8;
      for (uint32_t c = 0; c < crm::kClassCount && detail_classes; ++c) {
        detail_classes = Near(class_ambient[c], crm::kClassAmbient[c]) && Near(class_glint[c], crm::kClassGlint[c]);
      }
      Check(detail_classes, "the shader's class tables of the ambient's share and the coat's glint are the header's");
      Check(Has(hlsli, "const uint state = v * 747796405u + 2891336453u;") && Has(hlsli, "const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;") && Has(hlsli, "return (word >> 22u) ^ word;") &&
                Has(hlsli, "const int3 cell = int3(floor(hit * kOpenHashCells));") && Has(hlsli, "CarMatPcg(asuint(cell.x) + CarMatPcg(asuint(cell.y) + CarMatPcg(asuint(cell.z))))") &&
                Has(hlsli, "const float a = float(h & 0xFFFFu) / 65536.0;") && Has(hlsli, "const float b = float(h >> 16) / 65536.0;"),
            "the shader's hash and the hit's cell are the header's (Pcg, OpenHash)");
      Check(Has(hlsli, "d = length2 > 0.3 ? d / length2 : up;") && Has(hlsli, "const float3 helper = abs(d.y) < 0.9 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);") &&
                Has(hlsli, "const float angle = 6.283185307179586 * a;") && Has(hlsli, "(kOpenJitter * sqrt(b))") && Has(hlsli, "if (above < kOpenMinAbove)") &&
                Has(hlsli, "e = normalize(e + (kOpenMinAbove - above) * normal);") && Has(hlsli, "ray.Origin = hit + normal * (0.010 + 0.003 * hit.z);") && Has(hlsli, "ray.TMax = kOpenReach;") &&
                Has(hlsli, "return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;"),
            "the shader's sky ray is SkyRayDirection's, cast kOpenReach metres against every car");
      Check(Has(hlsli, "const float night = sunKnown ? f.night : 0.0;") && Has(hlsli, "const float daySun = 1.0 - night;") && Has(hlsli, "if (ndl > 0.0 && daySun > 0.0)") &&
                Has(hlsli, "const float contact = height >= 0.0 ? kContactMin + (1.0 - kContactMin) * smoothstep(0.0, kContactHeight, height) : 1.0;") &&
                Has(hlsli, "const float ground = lerp(1.0, kOpenMin + (1.0 - kOpenMin) * openness, strength) * lerp(1.0, contact, strength);") &&
                Has(hlsli, "const float classAmbient = lerp(1.0, kClassAmbient[m.cls], strength);") && Has(hlsli, "env * (fresnel * kClassSpecGain[m.cls] * f.specGain) * tint * ground;") &&
                Has(hlsli, "const float coat = m.cls == CAR_MAT_CLASS_CHROME ? fresnel : kCoatF0 + (1.0 - kCoatF0) * pow(saturate(1.0 - nv), kCoatPower);") &&
                Has(hlsli, "pow(saturate(dot(reflected, toSun)), kSunGlintPower)") && Has(hlsli, "pow(saturate(dot(reflected, up)), kLampGlintPower)") &&
                Has(hlsli, "result += f.sun * (kSunRadiance * sunLobe * visible * daySun + kLampRadiance * lampLobe * night * openness) * (coat * kClassGlint[m.cls] * detail) * tint;") &&
                Has(hlsli, "f.detail = asfloat(gCarRtMaterials[5].w);"),
            "the shader's detail is ShadeReference's: the hour, the contact, the openness, the class, the coat's glints");
      Check(Has(refl, "return max(dot(hit - car.centre, g_push.up.xyz) + kCarCentreRise, 0.0);") && Has(refl, "row = hitInstance.draw;") && Has(refl, "row = g_push.opp.z;") && Has(refl, "if (car.radius == 0.0)"),
            "the pass takes a hit's height over its car's ground from the car's record (the instance's, or the player's in the opponents' variant)");
      Check(Has(scene, "in.tuning.detail = float(larecomp::settings::mcla_gpu_car_rt_hit_detail.Get());") && Has(settings, "mcla_gpu_car_rt_hit_detail, 1.0,") && Has(manifest, "\"mcla_gpu_car_rt_hit_detail\""),
            "the scene reads the detail setting into the frame's rows and the setting is defined");
      Check(!cmake.empty() && Has(cmake, "larecomp_car_rt_material_rules_test"), "the CMake file builds this test");
    }
  }

  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
