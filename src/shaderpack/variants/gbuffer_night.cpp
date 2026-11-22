// LARecomp shader pack builder -- the G-buffer stage of the night world: the PSMultiLight family (see gbuffer_night.h).

#include "shaderpack/variants/gbuffer_night.h"

#include <cstddef>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "shaderpack/pack_format.h"
#include "shaderpack/variants/stage_constants.h"
#include "shaderpack/variants/text_edit.h"

namespace larecomp::shaderpack {

using namespace textedit;

bool IsGBufferNightShader(std::string_view name) {
  for (const std::string_view suffix : kGBufferNightSuffixes) {
    if (name.ends_with(suffix)) return true;
  }
  return false;
}

namespace {

constexpr std::string_view kSun = "gMultiLightDynamicLightData(10)";  // the sun's direction (the pixel shader's constant c94)

// ---------------------------------------------------------------------------
// Reading the translator's lines
// ---------------------------------------------------------------------------

struct Line {
  size_t begin = 0;       // offset of the line's first character in the text
  size_t indent = 0;      // tabs and spaces before the code
  std::string_view code;  // the line without its indentation and trailing white space
};

std::vector<Line> LinesFrom(const std::string& text, size_t from) {
  std::vector<Line> lines;
  size_t at = from;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    Line line;
    line.begin = at;
    size_t first = at;
    while (first < end && (text[first] == '\t' || text[first] == ' ')) ++first;
    size_t last = end;
    while (last > first && IsSpace(text[last - 1])) --last;
    line.indent = first - at;
    line.code = std::string_view(text).substr(first, last - first);
    lines.push_back(line);
    at = end + 1;
  }
  return lines;
}

bool AllIn(std::string_view text, std::string_view letters) {
  if (text.empty()) return false;
  for (const char c : text) {
    if (letters.find(c) == std::string_view::npos) return false;
  }
  return true;
}

bool IsRegisterName(std::string_view name) {
  if (name.size() < 2 || name[0] != 'r') return false;
  for (size_t i = 1; i < name.size(); ++i) {
    if (name[i] < '0' || name[i] > '9') return false;
  }
  return true;
}

bool IsWordChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

// The destination of an assignment statement: `r10.xyz = <rhs>;` (a register of the game's: `lanes` is empty for a whole register) or `ps = ...`.
struct Dest {
  bool valid = false;
  bool is_register = false;  // rN (a register of the game's), not ps / p0 / a0 / aL
  std::string_view reg;
  std::string_view lanes;
  std::string_view rhs;  // after " = ", with the closing `;`
};

Dest ParseDest(std::string_view code) {
  Dest out;
  const size_t equals = code.find(" = ");
  if (equals == std::string_view::npos || equals == 0) return out;
  const std::string_view left = code.substr(0, equals);
  for (const char c : left) {
    if (!IsWordChar(c) && c != '.') return out;  // `float4 r0 = ...`, a condition, a call
  }
  const size_t dot = left.find('.');
  out.reg = dot == std::string_view::npos ? left : left.substr(0, dot);
  out.lanes = dot == std::string_view::npos ? std::string_view() : left.substr(dot + 1);
  if (out.reg == "ps" || out.reg == "p0" || out.reg == "a0" || out.reg == "aL") {
    out.valid = true;
  } else if (IsRegisterName(out.reg) && (dot == std::string_view::npos || (AllIn(out.lanes, "xyzw") && out.lanes.size() <= 4))) {
    out.valid = true;
    out.is_register = true;
  }
  out.rhs = code.substr(equals + 3);
  return out;
}

bool WritesLane(const Dest& dest, std::string_view reg, char lane) {
  return dest.valid && dest.is_register && dest.reg == reg && (dest.lanes.empty() || dest.lanes.find(lane) != std::string_view::npos);
}

// An operand of a dot: `r7.wxy` or the sun's `gMultiLightDynamicLightData(10).zxy`.
struct Operand {
  bool ok = false;
  bool sun = false;
  std::string reg;
  std::string lanes;
  size_t length = 0;  // characters taken from the text
};

Operand ParseOperand(std::string_view text) {
  Operand out;
  if (text.starts_with(kSun)) {
    if (text.size() <= kSun.size() || text[kSun.size()] != '.') return out;
    size_t at = kSun.size() + 1;
    size_t end = at;
    while (end < text.size() && std::string_view("xyzw").find(text[end]) != std::string_view::npos) ++end;
    out.sun = true;
    out.lanes = std::string(text.substr(at, end - at));
    out.length = end;
    out.ok = AllIn(out.lanes, "xyz") && out.lanes.size() >= 2 && out.lanes.size() <= 3;
    return out;
  }
  size_t end = 0;
  while (end < text.size() && (IsWordChar(text[end]) || text[end] == '.')) ++end;
  const std::string_view word = text.substr(0, end);
  const size_t dot = word.find('.');
  if (dot == std::string_view::npos) return out;
  out.reg = std::string(word.substr(0, dot));
  out.lanes = std::string(word.substr(dot + 1));
  out.length = end;
  out.ok = IsRegisterName(out.reg) && AllIn(out.lanes, "xyzw") && out.lanes.size() >= 2 && out.lanes.size() <= 3;
  return out;
}

// `dot(A, B)` at `at` (the position of "dot("): both operands, when it has the shape.
bool ParseDotAt(std::string_view code, size_t at, Operand* a, Operand* b) {
  const size_t first = at + 4;
  *a = ParseOperand(code.substr(first));
  if (!a->ok) return false;
  const size_t comma = first + a->length;
  if (code.substr(comma, 2) != ", ") return false;
  const size_t second = comma + 2;
  *b = ParseOperand(code.substr(second));
  if (!b->ok) return false;
  return code.substr(second + b->length, 1) == ")";
}

using Origin = std::map<std::pair<std::string, char>, char>;  // (register, lane) -> the sun's component the lane holds (scaled)

struct Found {
  bool ok = false;
  size_t line = 0;
  NightNormalHow how = NightNormalHow::kNone;
  // the normal's lanes in the order x, y, z of the sun's space: the register (empty: an interpolant, `interp`) and the lane
  std::string reg[3];
  char lane[3] = {0, 0, 0};
  int interp[3] = {-1, -1, -1};
};

std::string LaneList(const Found& f, bool as_interpolant) {
  // `r10.xyz` when the three lanes are of one register, else float3(a, b, c)
  auto name = [&](int i) { return as_interpolant ? std::format("iTexCoord{}", f.interp[i]) : f.reg[i]; };
  if (name(0) == name(1) && name(1) == name(2)) return std::format("{}.{}{}{}", name(0), f.lane[0], f.lane[1], f.lane[2]);
  return std::format("float3({}.{}, {}.{}, {}.{})", name(0), f.lane[0], name(1), f.lane[1], name(2), f.lane[2]);
}

}  // namespace

// ---------------------------------------------------------------------------
// The analysis
// ---------------------------------------------------------------------------

NightAnalysis AnalyseGBufferNight(std::string_view hlsl, std::string_view name) {
  NightAnalysis out;
  const std::string text = NormalizeNewlines(hlsl);
  const size_t main_at = text.rfind("\nvoid main(");
  if (main_at == std::string::npos) {
    out.why_not = std::string(kGBufferNightSkipUnlit);
    out.detail = "no `void main(`";
    return out;
  }
  const std::vector<Line> lines = LinesFrom(text, main_at + 1);
  std::vector<Dest> dests;
  dests.reserve(lines.size());
  for (const Line& line : lines) dests.push_back(ParseDest(line.code));

  // The interpolants the registers start as: `float4 rN = iTexCoordK;`.
  std::map<std::string, int> interps;
  for (const Line& line : lines) {
    if (!line.code.starts_with("float4 r") || !line.code.ends_with(";")) continue;
    const size_t equals = line.code.find(" = iTexCoord");
    if (equals == std::string_view::npos) continue;
    const std::string_view reg = line.code.substr(7, equals - 7);
    std::string_view digits = line.code.substr(equals + std::string_view(" = iTexCoord").size());
    digits.remove_suffix(1);
    if (!IsRegisterName(reg) || !AllIn(digits, "0123456789") || digits.size() > 2) continue;
    interps[std::string(reg)] = std::stoi(std::string(digits));
  }
  const auto written_before = [&](std::string_view reg, char lane, size_t before) {
    for (size_t j = 0; j < before && j < dests.size(); ++j) {
      if (WritesLane(dests[j], reg, lane)) return true;
    }
    return false;
  };
  const auto distinct3 = [](const std::string& lanes) { return lanes.size() == 3 && lanes[0] != lanes[1] && lanes[1] != lanes[2] && lanes[0] != lanes[2]; };

  // 1. The sun term.
  Found found;
  bool saw_tangent = false;
  bool saw_nested = false;
  Origin origin;
  char pending = 0;  // `ps = sun.c * k;` on the line before: the sun's component the next line's `rD.l = ps;` holds
  for (size_t i = 0; i < lines.size() && !found.ok; ++i) {
    const std::string_view code = lines[i].code;
    const Dest& dest = dests[i];
    std::vector<std::pair<std::pair<std::string, char>, char>> sets;
    if (pending != 0) {
      if (dest.valid && dest.is_register && dest.lanes.size() == 1 && dest.rhs == "ps;") sets.push_back({{std::string(dest.reg), dest.lanes[0]}, pending});
      pending = 0;
    }
    if (code.find(kSun) != std::string_view::npos) {
      // `r14.xyz = r5.www * gMultiLightDynamicLightData(10).zyx;`: the sun's lanes, scaled, in three lanes of a register
      if (dest.valid && dest.is_register && dest.lanes.size() == 3) {
        const size_t star = dest.rhs.find(" * ");
        if (star != std::string_view::npos && dest.rhs.substr(star + 3).starts_with(kSun)) {
          const Operand scale = ParseOperand(dest.rhs.substr(0, star));
          const Operand sun = ParseOperand(dest.rhs.substr(star + 3));
          if (scale.ok && sun.ok && sun.sun && distinct3(sun.lanes)) {
            for (size_t k = 0; k < 3; ++k) sets.push_back({{std::string(dest.reg), dest.lanes[k]}, sun.lanes[k]});
          }
        }
      }
      // `ps = gMultiLightDynamicLightData(10).z * r5.w;` and then `r19.x = ps;`
      if (code.starts_with("ps = ") && code.size() > 5 + kSun.size() + 2 && code.substr(5).starts_with(kSun) && code[5 + kSun.size()] == '.' &&
          std::string_view("xyz").find(code[5 + kSun.size() + 1]) != std::string_view::npos && code.substr(5 + kSun.size() + 2, 3) == " * ") {
        pending = code[5 + kSun.size() + 1];
      }
    }
    // A dot of the sun's direction with something: directly, or through the lanes that hold it scaled.
    for (size_t at = code.find("dot("); at != std::string_view::npos && !found.ok; at = code.find("dot(", at + 4)) {
      Operand a, b;
      if (!ParseDotAt(code, at, &a, &b)) continue;
      if (a.sun != b.sun) {
        const Operand& sun = a.sun ? a : b;
        const Operand& other = a.sun ? b : a;
        if (sun.lanes.size() == 2) {  // `dot(r14.xy, ...(10).xy) + ...(10).z`: the normal is (x, y, 1) in the decal's own frame
          saw_tangent = true;
          continue;
        }
        if (!distinct3(sun.lanes) || other.lanes.size() != 3) continue;
        for (size_t k = 0; k < 3; ++k) {
          const size_t c = size_t(sun.lanes[k] - 'x');
          found.reg[c] = other.reg;
          found.lane[c] = other.lanes[k];
        }
        found.ok = true;
        found.line = i;
        found.how = NightNormalHow::kSunTerm;
      } else if (!a.sun && a.lanes.size() == 3 && b.lanes.size() == 3) {
        // two registers: the one whose lanes hold the sun's three components (scaled) pairs them with the other's lanes
        const auto components = [&](const Operand& op, char out_components[3]) {
          for (size_t k = 0; k < 3; ++k) {
            const auto it = origin.find({op.reg, op.lanes[k]});
            if (it == origin.end()) return false;
            out_components[k] = it->second;
          }
          return out_components[0] != out_components[1] && out_components[1] != out_components[2] && out_components[0] != out_components[2];
        };
        char ca[3] = {}, cb[3] = {};
        const bool a_sun = components(a, ca);
        const bool b_sun = components(b, cb);
        if (a_sun == b_sun) continue;  // neither, or the sun dotted with itself (its length): not the normal
        const Operand& other = a_sun ? b : a;
        const char* comps = a_sun ? ca : cb;
        for (size_t k = 0; k < 3; ++k) {
          const size_t c = size_t(comps[k] - 'x');
          found.reg[c] = other.reg;
          found.lane[c] = other.lanes[k];
        }
        found.ok = true;
        found.line = i;
        found.how = NightNormalHow::kSunTerm;
      }
    }
    // What this line wrote is no longer what the sun's lanes held; the lanes it sets now hold the sun.
    if (dest.valid && dest.is_register) {
      for (const char lane : std::string_view("xyzw")) {
        if (dest.lanes.empty() || dest.lanes.find(lane) != std::string_view::npos) origin.erase({std::string(dest.reg), lane});
      }
    }
    for (const auto& [key, component] : sets) origin[key] = component;
  }
  // The term inside a branch (the powerlines light only the pixels a predicate keeps) is fine when the normal is an interpolant (the write reads it at the end of main);
  // a capture, which is a declaration before the term's statement, needs the statement to be at the top level of main.
  const auto lanes_are_interpolants = [&](const Found& f) {
    for (size_t c = 0; c < 3; ++c) {
      if (!interps.contains(f.reg[c]) || written_before(f.reg[c], f.lane[c], f.line)) return false;
    }
    return true;
  };
  if (found.ok && lines[found.line].indent != 1 && !lanes_are_interpolants(found)) {
    saw_nested = true;
    found = Found();
  }

  // 2. The lamp sum: an interpolant whose x, y and z are each a factor of a product before anything writes them.
  // 3. The normalisation: an interpolant whose x, y, z are dotted with themselves.
  const auto lanes_xyz_untouched_until = [&](const std::string& reg) {
    size_t end = lines.size();
    for (size_t j = 0; j < lines.size(); ++j) {
      for (const char lane : std::string_view("xyz")) {
        if (WritesLane(dests[j], reg, lane)) {
          end = j;
          break;
        }
      }
      if (end != lines.size()) break;
    }
    return end;
  };
  bool ambiguous = false;
  if (!found.ok && !saw_tangent && !saw_nested) {
    for (int pass = 0; pass < 2 && !found.ok && !ambiguous; ++pass) {
      std::vector<std::pair<std::string, int>> candidates;
      for (const auto& [reg, k] : interps) {
        const size_t end = lanes_xyz_untouched_until(reg);
        bool x = false, y = false, z = false;
        for (size_t j = 0; j < end; ++j) {
          const std::string_view code = lines[j].code;
          if (pass == 0) {
            // `... * r1.zzzz`, `r1.x * ...`: a lane (repeated or alone) next to a product sign
            const Dest& d = dests[j];
            if (!d.valid) continue;
            const std::string_view rhs = d.rhs;
            for (size_t at = rhs.find(reg); at != std::string_view::npos; at = rhs.find(reg, at + 1)) {
              if (at > 0 && (IsWordChar(rhs[at - 1]) || rhs[at - 1] == '.')) continue;
              if (at + reg.size() >= rhs.size() || rhs[at + reg.size()] != '.') continue;
              size_t end_lane = at + reg.size() + 1;
              while (end_lane < rhs.size() && std::string_view("xyzw").find(rhs[end_lane]) != std::string_view::npos) ++end_lane;
              const std::string_view run = rhs.substr(at + reg.size() + 1, end_lane - at - reg.size() - 1);
              if (run.empty() || run.find_first_not_of(run[0]) != std::string_view::npos || run[0] == 'w') continue;
              const bool times_before = at >= 3 && rhs.substr(at - 3, 3) == " * ";
              const bool times_after = rhs.substr(end_lane, 3) == " * ";
              if (!times_before && !times_after) continue;
              (run[0] == 'x' ? x : run[0] == 'y' ? y : z) = true;
            }
          } else {
            // `dot(r1.zxy, r1.zxy)`: the same three lanes twice
            for (size_t at = code.find("dot("); at != std::string_view::npos; at = code.find("dot(", at + 4)) {
              Operand a, b;
              if (!ParseDotAt(code, at, &a, &b) || a.sun || b.sun || a.reg != reg || b.reg != reg || a.lanes != b.lanes || a.lanes.size() != 3) continue;
              if (a.lanes.find('w') != std::string::npos || a.lanes[0] == a.lanes[1] || a.lanes[1] == a.lanes[2] || a.lanes[0] == a.lanes[2]) continue;
              x = y = z = true;
            }
          }
        }
        if (x && y && z) candidates.push_back({reg, k});
      }
      if (candidates.size() > 1) {
        ambiguous = true;
      } else if (candidates.size() == 1) {
        found.ok = true;
        found.line = 0;
        found.how = pass == 0 ? NightNormalHow::kLampTerm : NightNormalHow::kNormalisation;
        for (size_t c = 0; c < 3; ++c) {
          found.reg[c] = candidates[0].first;
          found.lane[c] = char('x' + c);
        }
      }
    }
  }

  if (!found.ok) {
    // Lit by the lamps (the grid's four, or the three dynamic slots 4 to 6) but with no normal in the sum: the attenuation alone.
    bool grid = false;
    for (const Line& line : lines) {
      grid = grid || line.code.find("LightSortScaleBias.") != std::string_view::npos || line.code.find("gMultiLightDynamicLightData(4)") != std::string_view::npos;
    }
    if (saw_tangent) {
      out.why_not = std::string(kGBufferNightSkipTangent);
      out.detail = "the sun term is dot(rN.xy, sun.xy) + sun.z: a normal (x, y, 1) in the decal's own frame";
    } else if (saw_nested) {
      out.why_not = std::string(kGBufferNightSkipNested);
      out.detail = "the sun term is not at the top level of main";
    } else if (ambiguous) {
      out.why_not = std::string(kGBufferNightSkipAmbiguous);
      out.detail = "more than one interpolant has the shape of the normal";
    } else if (grid) {
      out.why_not = std::string(kGBufferNightSkipLampsOnly);
      out.detail = "no sun term, and no interpolant the lamps' sum multiplies: the lamps' light is by distance only";
    } else {
      out.why_not = std::string(kGBufferNightSkipUnlit);
      out.detail = "no sun term and no lamps: the colour is not lit per pixel";
    }
    return out;
  }

  // Where the normal is read: the interpolants, when each lane is one nothing wrote before the term, else a capture before the term's statement.
  bool all_pristine = true;
  for (size_t c = 0; c < 3; ++c) {
    const auto it = interps.find(found.reg[c]);
    if (it == interps.end() || (found.how == NightNormalHow::kSunTerm && written_before(found.reg[c], found.lane[c], found.line))) all_pristine = false;
    if (it != interps.end()) found.interp[c] = it->second;
  }
  out.found = true;
  out.how = found.how;
  out.line = int(found.line);
  out.albedo = AnalyseGBufferAlbedo(text);  // RT2: the diffuse texel (a shader without one writes the "no data" code there)
  if (const NightAlbedoRow* row = name.empty() ? nullptr : FindGBufferNightAlbedoRow(name)) {
    // the shader has no texel of that shape: its recipe says where its colour is (gbuffer_night_albedo.h); it takes the place of what the texel analysis found, which is nothing on the retail text
    out.recipe = true;
    out.albedo = AnalyseGBufferNightAlbedoRow(text, *row);
  }
  if (all_pristine) {
    out.interpolant = true;
    out.expression = LaneList(found, true);
  } else {
    out.expression = "gbNormal";
    const Line& at = lines[found.line];
    out.capture_at = at.begin;
    out.capture = text.substr(at.begin, at.indent) + "float3 gbNormal = " + LaneList(found, false) + ";\n";
  }
  switch (found.how) {
    case NightNormalHow::kSunTerm:
      out.detail = std::format("sun term, line {}: {}", found.line, out.interpolant ? "the interpolant untouched -> " + out.expression : "captured " + LaneList(found, false));
      break;
    case NightNormalHow::kLampTerm:
      out.detail = std::format("lamp sum multiplies {} lane by lane -> {}", LaneList(found, false), out.expression);
      break;
    default:
      out.detail = std::format("{} is dotted with itself (normalised) -> {}", LaneList(found, false), out.expression);
      break;
  }
  return out;
}

namespace {

// The write of the planes after the game's last colour statement: RT1, the normal (under GB_MODE_WRITE_NORMAL), then RT2, the albedo and the lit weight (gbuffer_albedo.h, under
// GB_MODE_WRITE_ALBEDO), then RT3, the smoothness, the class and the flags of the draw's material word in g_GBufferMode.y (gbuffer_material_plane.h, under GB_MODE_WRITE_MATERIAL).
std::string NightWriteBlock(std::string_view normal_expression, const GBufferAlbedo& albedo) {
  return "\n\toC1 = float2(0.0f, 0.0f);\n"
         "\tif ((g_GBufferMode.x & uint(GB_MODE_WRITE_NORMAL)) != 0u) {\n"
         "\t\toC1 = GbRt1Encode(" +
         std::string(normal_expression) +
         ");\n"
         "\t}" +
         GBufferAlbedoWriteText(albedo) + GBufferMaterialWriteText();
}

}  // namespace

VariantTransformResult TransformGBufferNightHlsl(const VariantShader& shader, std::string_view hlsl, std::string_view hlsli_text, const StageContext& ctx) {
  VariantTransformResult result;
  const auto fail = [&](std::string message) {
    result.ok = false;
    result.skip = false;
    result.hlsl.clear();
    result.error = std::string(shader.name) + ": " + std::move(message);
    return result;
  };
  if (!IsGBufferNightShader(shader.name)) {
    result.skip = true;
    result.error = "not a shader the G-buffer variant's night stage is for";
    return result;
  }
  std::string text = NormalizeNewlines(hlsl);
  std::string helper = NormalizeNewlines(hlsli_text);
  while (!helper.empty() && IsSpace(helper.back())) helper.pop_back();
  if (helper.empty()) return fail("the helper text (gbuffer_common.hlsli) is empty");

  const NightAnalysis found = AnalyseGBufferNight(text, shader.name);
  if (!found.found) {
    result.skip = true;
    result.error = found.why_not;
    return result;
  }
  // A shader with a recipe is promised an albedo: when its anchor is not there the HLSL is not the one the recipe was written for, and the variant is not built (never a quiet "no data").
  if (found.recipe && !found.albedo.found) return fail("the albedo recipe does not fit this text: " + found.albedo.why_not);
  std::string error;

  // The interpolants the expression reads must be declared once, as the translator makes them.
  for (size_t at = found.expression.find("iTexCoord"); at != std::string::npos; at = found.expression.find("iTexCoord", at + 1)) {
    size_t end = at + 9;
    while (end < found.expression.size() && found.expression[end] >= '0' && found.expression[end] <= '9') ++end;
    const std::string index = found.expression.substr(at + 9, end - at - 9);
    const std::string declaration = "in centroid float4 iTexCoord" + index + " : TEXCOORD" + index;
    if (CountOccurrences(text, declaration) != 1) {
      return fail("the pixel shader does not declare `" + declaration + "` once: not the translator's HLSL this was written for");
    }
  }

  // 0. The captures: of a normal the shader computes, before the statement that uses it, and of the diffuse texel the shader overwrites later, right after its fetch (offsets of the text
  // as it is now: first, the later offset first so that the earlier one does not move).
  {
    const GBufferAlbedo& albedo = found.albedo;
    const bool capture_albedo = albedo.found && albedo.captured;
    const bool capture_normal = found.capture_at != std::string::npos;
    if (capture_albedo && capture_normal && albedo.capture_at <= found.capture_at) {
      text.insert(found.capture_at, found.capture);
      text.insert(albedo.capture_at, albedo.capture);
    } else {
      if (capture_albedo) text.insert(albedo.capture_at, albedo.capture);
      if (capture_normal) text.insert(found.capture_at, found.capture);
    }
  }

  // 1. The second, third and fourth outputs, after the game's colour output (before the SV_Coverage of the alpha test spec variant): RT1 the normal, RT2 the albedo and the lit weight, RT3
  // the smoothness and the class.
  static constexpr Piece kOutput[] = {Lit("\tout float4 oC0 : SV_Target0")};
  if (!ReplaceOnce(text, kOutput, 0, 0, "oC0 : SV_Target0 output",
                   [](std::string_view group) {
                     return std::string(group) + "\n\t, out float2 oC1 : SV_Target1" + std::string(kGBufferAlbedoOutput) + std::string(kGBufferMaterialOutput);
                   }, &error)) {
    return fail(std::move(error));
  }

  // 2. The block of constants (48 bytes: gbuffer_types.h GBufferConstants), where the chain gives it (byte 608 alone, 1280 after the headlight's block).
  static constexpr ConstantMember kMembers[] = {{"uint4", "g_GBufferMode"}, {"float4", "g_GBufferRay"}, {"float4", "g_GBufferCam"}};
  if (!DeclareConstants(text, ctx, "G-buffer variant (src/gpu/gbuffer_types.h GBufferConstants): 48 bytes after the 608.", kMembers, &error)) {
    return fail(std::move(error));
  }

  // 3. The helper, before main.
  static constexpr Piece kMain[] = {Lit("#ifndef __spirv__"), Ws(0), Lit("[shader(\"pixel\")]"), Ws(0), Lit("#endif"), Ws(0), Lit("void main(")};
  if (!ReplaceOnce(text, kMain, 0, 6, "pixel shader main",
                   [&](std::string_view group) { return helper + "\n\n" + std::string(group); }, &error)) {
    return fail(std::move(error));
  }

  // 4. The write, after the game's last statement of the colour (so after the alpha test's discard).
  static constexpr Piece kWrite[] = {Lit("oC0 = applyBlendPremult(oC0);")};
  if (!ReplaceOnce(text, kWrite, 0, 0, "the game's colour write",
                   [&](std::string_view group) { return std::string(group) + NightWriteBlock(found.expression, found.albedo); }, &error)) {
    return fail(std::move(error));
  }

  // What the text must hold now: the second, third and fourth outputs, one call each, the helpers once, the captures as many times as they were made.
  const std::string call = "oC1 = GbRt1Encode(" + found.expression + ");";
  const std::string albedo_call = "oC2 = float4(GbAlbedoEncode(" + found.albedo.expression + "), GbMaterialLit(g_GBufferMode.y));";
  const struct {
    std::string_view needle;
    size_t want;
  } kChecks[] = {
      {"oC1 : SV_Target1", 1},
      {"oC2 : SV_Target2", 1},
      {"oC3 : SV_Target3", 1},
      {call, 1},
      {"oC2 = float4(0.0f, 0.0f, 0.0f, 0.0f);", 1},
      {"oC3 = float2(0.0f, 0.0f);", 1},
      {kGBufferMaterialWriteLine, 1},
      {albedo_call, found.albedo.found ? 1u : 0u},
      {"float2 GbRt1Encode(float3 n)", 1},
      {"float2 GbOctEncode(float3 n)", 1},
      {"float3 GbAlbedoEncode(float3 albedo)", 1},
      {"float GbMaterialLit(uint word)", 1},
      {"float2 GbMaterialRt3(uint word)", 1},
      {"oC0 = applyBlendPremult(oC0);", 1},
      {"float3 gbNormal = ", found.capture_at == std::string::npos ? 0u : 1u},
      {"float3 gbAlbedo = ", found.albedo.found && found.albedo.captured ? 1u : 0u},
      {"\x01", 0},
  };
  for (const auto& check : kChecks) {
    const size_t count = CountOccurrences(text, check.needle);
    if (count != check.want) {
      return fail(std::format("the transformed HLSL has '{}' {} times (need {})", check.needle, count, check.want));
    }
  }
  result.ok = true;
  result.hlsl = std::move(text);
  return result;
}

}  // namespace larecomp::shaderpack
