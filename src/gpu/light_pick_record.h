#pragma once
// ===========================================================================
// MCLA GPU renderer -- a recording of the pickers' inputs: the text format the game's recorder writes and light_pick_test.cpp replays
// (perf/LIGHT-UNIFY.md step U3, package LU-3)
// ===========================================================================
// To prove the unified picker (light_pick.h) picks what the legacy code picked, the test needs the INPUTS of the two old pickers frame after frame: the cars the rig's Tracker was given
// (headlight_rules::Candidate, with the frame's camera frustum) and the lights, the cap and the context the shadow replay's ViewPicker was given (local_shadow::Light, PickContext). With
// mcla_gpu_light_pick_record = <a path> (debug only) the rig writes `<path>.rig.txt` and the replay `<path>.views.txt`, one block per guest frame, in the format below; light_pick_test
// replays them (`larecomp_light_pick_test <path>.rig.txt <path>.views.txt`) through both modes. The test also writes a made up drive to this format and reads it back, so that the
// writer and the reader are held to each other: every number is written with 17 significant digits and comes back bit for bit.
//
//   R <frame> <now_s> <has_view> <beam_range> <beam_outer_degrees> <car count>        one guest frame of the rig; `now_s` is the scene clock's time in seconds
//   V <eye x3> <clip x16>                                                              the frame's view (only when has_view): the world -> clip matrix, row major, row vectors
//   C <player> <on_screen> <guest> <on> <camera_distance> <position x3> <forward x3> <up x3> <half_span> <front> <total> <spread>     one car, after its R line
//   W <frame> <max_cars> <have_reference> <reference x3> <have_screen> <view_projection x16> <frustum valid> <light count>      one frame of the view picker
//   F <eye x3> <clip x16>                                                              its frustum (only when valid)
//   L <pos x3> <dir x3> <cos_outer> <range> <car_key> <is_player> <index> <lamp_offset x3>     one light, after its W line
// Blank lines and lines starting with # are skipped.
//
// Pure: plain numbers and text, no renderer. The writing itself (a file stream) is the rig's and the replay's.
// ===========================================================================

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "headlight_rig_rules.h"
#include "light_influence.h"
#include "local_shadow_rules.h"

namespace mcla::gpu::light_pick::record {

// One guest frame of the rig's inputs.
struct RigFrame {
  uint64_t frame = 0;
  double now_s = 0.0;
  bool has_view = false;
  light_influence::ViewFrustum frustum;
  double beam_range = headlight_rules::kRange;
  double beam_outer_degrees = headlight_rules::kOuterDegrees;
  std::vector<headlight_rules::Candidate> cars;
};

// One frame of the view picker's inputs.
struct ViewFrame {
  uint64_t frame = 0;
  uint32_t max_cars = 0;
  std::vector<local_shadow::Light> lights;
  local_shadow::PickContext context;
};

namespace detail {

inline void Num(std::string* out, double v) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), " %.17g", v);
  *out += buf;
}
inline void Int(std::string* out, uint64_t v) { *out += " " + std::to_string(v); }
inline void Flag(std::string* out, bool v) { *out += v ? " 1" : " 0"; }
inline void Three(std::string* out, const double* v) {
  for (int i = 0; i < 3; ++i) Num(out, v[i]);
}

inline void FrustumLine(std::string* out, char tag, const light_influence::ViewFrustum& f) {
  *out += tag;
  Three(out, f.eye);
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) Num(out, f.clip[r][c]);
  }
  *out += "\n";
}

}  // namespace detail

inline std::string FormatRig(const RigFrame& f) {
  std::string out = "R";
  detail::Int(&out, f.frame);
  detail::Num(&out, f.now_s);
  detail::Flag(&out, f.has_view);
  detail::Num(&out, f.beam_range);
  detail::Num(&out, f.beam_outer_degrees);
  detail::Int(&out, f.cars.size());
  out += "\n";
  if (f.has_view) {
    detail::FrustumLine(&out, 'V', f.frustum);
  }
  for (const headlight_rules::Candidate& c : f.cars) {
    out += "C";
    detail::Flag(&out, c.player);
    detail::Flag(&out, c.on_screen);
    detail::Flag(&out, c.guest);
    detail::Num(&out, c.on);
    detail::Num(&out, c.camera_distance);
    detail::Three(&out, c.lamp.position);
    detail::Three(&out, c.lamp.forward);
    detail::Three(&out, c.lamp.up);
    detail::Num(&out, c.lamp.half_span);
    detail::Int(&out, c.lamp.front);
    detail::Int(&out, c.lamp.total);
    detail::Num(&out, c.lamp.spread);
    out += "\n";
  }
  return out;
}

inline std::string FormatView(const ViewFrame& f) {
  std::string out = "W";
  detail::Int(&out, f.frame);
  detail::Int(&out, f.max_cars);
  detail::Flag(&out, f.context.have_reference);
  detail::Three(&out, f.context.reference);
  detail::Flag(&out, f.context.have_screen);
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) detail::Num(&out, f.context.view_projection.m[r][c]);
  }
  detail::Flag(&out, f.context.frustum.valid);
  detail::Int(&out, f.lights.size());
  out += "\n";
  if (f.context.frustum.valid) {
    detail::FrustumLine(&out, 'F', f.context.frustum);
  }
  for (const local_shadow::Light& l : f.lights) {
    out += "L";
    detail::Three(&out, l.pos);
    detail::Three(&out, l.dir);
    detail::Num(&out, l.cos_outer);
    detail::Num(&out, l.range);
    detail::Int(&out, l.car_key);
    detail::Flag(&out, l.is_player);
    detail::Int(&out, l.index);
    detail::Three(&out, l.lamp_offset);
    out += "\n";
  }
  return out;
}

namespace detail {

// The tokens of one line after its tag.
class Tokens {
 public:
  explicit Tokens(std::string_view line) : line_(line) {}
  bool Number(double* out) {
    std::string_view t;
    if (!Next(&t)) {
      return false;
    }
    char buf[64];
    if (t.size() >= sizeof(buf)) {
      return false;
    }
    for (size_t i = 0; i < t.size(); ++i) buf[i] = t[i];
    buf[t.size()] = 0;
    char* end = nullptr;
    *out = std::strtod(buf, &end);
    return end != buf && *end == 0;
  }
  bool Uint(uint64_t* out) {
    double v = 0.0;
    if (!Number(&v) || v < 0.0) {
      return false;
    }
    *out = uint64_t(v + 0.5);
    return true;
  }
  bool Bool(bool* out) {
    double v = 0.0;
    if (!Number(&v)) {
      return false;
    }
    *out = v != 0.0;
    return true;
  }
  bool Three(double* out) { return Number(&out[0]) && Number(&out[1]) && Number(&out[2]); }
  bool Done() {
    std::string_view t;
    return !Next(&t);
  }

 private:
  bool Next(std::string_view* out) {
    while (pos_ < line_.size() && (line_[pos_] == ' ' || line_[pos_] == '\t' || line_[pos_] == '\r')) ++pos_;
    if (pos_ >= line_.size()) {
      return false;
    }
    const size_t start = pos_;
    while (pos_ < line_.size() && line_[pos_] != ' ' && line_[pos_] != '\t' && line_[pos_] != '\r') ++pos_;
    *out = line_.substr(start, pos_ - start);
    return true;
  }
  std::string_view line_;
  size_t pos_ = 0;
};

inline bool ReadFrustum(Tokens& t, light_influence::ViewFrustum* out) {
  double eye[3];
  double clip[4][4];
  if (!t.Three(eye)) {
    return false;
  }
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      if (!t.Number(&clip[r][c])) {
        return false;
      }
    }
  }
  return light_influence::MakeViewFrustum(clip, eye, out);
}

}  // namespace detail

// Reads a recording: the rig's frames and the view picker's, in the order of the text. False with `*error` set at the first line that is not one of the formats above.
inline bool Parse(std::string_view text, std::vector<RigFrame>* rig, std::vector<ViewFrame>* views, std::string* error) {
  size_t pos = 0;
  size_t line_no = 0;
  size_t cars_left = 0, lights_left = 0;
  bool want_view = false, want_frustum = false;
  const auto fail = [&](const std::string& why) {
    *error = "line " + std::to_string(line_no) + ": " + why;
    return false;
  };
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    const std::string_view line = text.substr(pos, end - pos);
    pos = end + 1;
    ++line_no;
    size_t first = 0;
    while (first < line.size() && (line[first] == ' ' || line[first] == '\t' || line[first] == '\r')) ++first;
    if (first >= line.size() || line[first] == '#') {
      continue;
    }
    const char tag = line[first];
    detail::Tokens t(line.substr(first + 1));
    switch (tag) {
      case 'R': {
        if (cars_left != 0 || lights_left != 0 || want_view || want_frustum) {
          return fail("a frame starts before the last one has all its lines");
        }
        RigFrame f;
        uint64_t count = 0;
        if (!t.Uint(&f.frame) || !t.Number(&f.now_s) || !t.Bool(&f.has_view) || !t.Number(&f.beam_range) || !t.Number(&f.beam_outer_degrees) || !t.Uint(&count) || !t.Done()) {
          return fail("an R line is <frame> <now_s> <has_view> <beam_range> <beam_outer_degrees> <car count>");
        }
        rig->push_back(f);
        cars_left = size_t(count);
        want_view = f.has_view;
        break;
      }
      case 'V': {
        if (!want_view || rig->empty()) {
          return fail("a V line with no R line that wants a view");
        }
        if (!detail::ReadFrustum(t, &rig->back().frustum) || !t.Done()) {
          return fail("a V line is <eye x3> <clip x16>, a camera");
        }
        want_view = false;
        break;
      }
      case 'C': {
        if (cars_left == 0 || want_view || rig->empty()) {
          return fail("a C line out of place");
        }
        headlight_rules::Candidate c;
        uint64_t front = 0, total = 0;
        if (!t.Bool(&c.player) || !t.Bool(&c.on_screen) || !t.Bool(&c.guest) || !t.Number(&c.on) || !t.Number(&c.camera_distance) || !t.Three(c.lamp.position) || !t.Three(c.lamp.forward) ||
            !t.Three(c.lamp.up) || !t.Number(&c.lamp.half_span) || !t.Uint(&front) || !t.Uint(&total) || !t.Number(&c.lamp.spread) || !t.Done()) {
          return fail("a C line is <player> <on_screen> <guest> <on> <camera_distance> <position x3> <forward x3> <up x3> <half_span> <front> <total> <spread>");
        }
        c.lamp.front = uint32_t(front);
        c.lamp.total = uint32_t(total);
        rig->back().cars.push_back(c);
        --cars_left;
        break;
      }
      case 'W': {
        if (cars_left != 0 || lights_left != 0 || want_view || want_frustum) {
          return fail("a frame starts before the last one has all its lines");
        }
        ViewFrame f;
        uint64_t max_cars = 0, count = 0;
        bool valid = false;
        if (!t.Uint(&f.frame) || !t.Uint(&max_cars) || !t.Bool(&f.context.have_reference) || !t.Three(f.context.reference) || !t.Bool(&f.context.have_screen)) {
          return fail("a W line is <frame> <max_cars> <have_reference> <reference x3> <have_screen> <view_projection x16> <frustum valid> <light count>");
        }
        for (int r = 0; r < 4; ++r) {
          for (int c = 0; c < 4; ++c) {
            if (!t.Number(&f.context.view_projection.m[r][c])) {
              return fail("a W line has 16 numbers of the view projection");
            }
          }
        }
        if (!t.Bool(&valid) || !t.Uint(&count) || !t.Done()) {
          return fail("a W line ends with <frustum valid> <light count>");
        }
        f.max_cars = uint32_t(max_cars);
        views->push_back(f);
        lights_left = size_t(count);
        want_frustum = valid;
        break;
      }
      case 'F': {
        if (!want_frustum || views->empty()) {
          return fail("an F line with no W line that wants a frustum");
        }
        if (!detail::ReadFrustum(t, &views->back().context.frustum) || !t.Done()) {
          return fail("an F line is <eye x3> <clip x16>, a camera");
        }
        want_frustum = false;
        break;
      }
      case 'L': {
        if (lights_left == 0 || want_frustum || views->empty()) {
          return fail("an L line out of place");
        }
        local_shadow::Light l;
        uint64_t key = 0, index = 0;
        if (!t.Three(l.pos) || !t.Three(l.dir) || !t.Number(&l.cos_outer) || !t.Number(&l.range) || !t.Uint(&key) || !t.Bool(&l.is_player) || !t.Uint(&index) || !t.Three(l.lamp_offset) ||
            !t.Done()) {
          return fail("an L line is <pos x3> <dir x3> <cos_outer> <range> <car_key> <is_player> <index> <lamp_offset x3>");
        }
        l.car_key = uint32_t(key);
        l.index = uint32_t(index);
        views->back().lights.push_back(l);
        --lights_left;
        break;
      }
      default:
        return fail(std::string("a line that starts with '") + tag + "' is none of R, V, C, W, F, L");
    }
  }
  if (cars_left != 0 || lights_left != 0 || want_view || want_frustum) {
    line_no = 0;
    *error = "the recording ends in the middle of a frame";
    return false;
  }
  return true;
}

}  // namespace mcla::gpu::light_pick::record
