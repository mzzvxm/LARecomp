#pragma once
//
// Named places ("spots") of the trailer toolkit's scene files (feature-parity/DIRECTOR-DESIGN.md section 4.3, package B DIR-VERBS).
//
// A scene that names a street corner by its three coordinates breaks the day the corner moves and says nothing to the next reader. A spot is
// a place with a name, written once in a `spots.txt` (ports\scripts\scenes\spots.txt is the shared one) and used in any statement that takes a
// position or a heading:
//
//   spot:<name>                          the spot's place: x,y,z. In `car <id> spawn <model> at spot:hw-street`, `cam 3 pos spot:hw-street@0,1.7,-8`,
//                                        `cam 3 look spot:hw-street`, `cam 0 rig tripod at spot:hw-street@4,1.2,0 track car:p1`, `start warp spot:x`.
//   spot:<name>@<r>,<u>,<f>              the place `r` metres to the spot's right, `u` up and `f` ahead of it, in the spot's HEADING frame (the
//                                        same frame as `attach car:<id> <right> <up> <forward>` and `near`: the spot's heading is where it faces).
//   spot:<name>@<r>,<u>,<f>,<turn>       the same, and `turn` degrees added to the spot's heading where the word is read as a heading (the oncoming
//                                        lane of a street is `@1.5,0,0,180`). A position word ignores the turn.
//   heading spot:<name>                  after `heading` (a car) or `yaw` (a camera): the spot's heading in degrees (plus the turn). The offset is
//   yaw spot:<name>                      not used for a heading, so the word that places a car can be copied to the word that turns it.
//
// The words are turned into plain numbers when a statement is READ (scene.cpp: cam, car and start statements), so the camera, the cars and the
// logs see coordinates and nothing else changes in them. A statement with a spot word that cannot be resolved is an error on its own line, with
// "did you mean" for a mistyped name.
//
// THE FILE (spots.txt): one spot a line, `<name> <x> <y> <z> <heading>   # <what is there> src: <where the numbers came from>`.
//   name      letters, digits, _ - . (not case sensitive), up to 40, starting with a letter or a digit
//   x y z     world metres (x east, y up, z south); y is the height a car stands at on the road there
//   heading   degrees, the scene's convention (car_verbs.h): 0 faces +Z, 90 faces +X; optional (a spot without one cannot be used with @ or as a heading)
// Blanks or commas separate the fields; `#` starts a comment (at the start of a line or after a blank) and the text after it is the spot's note,
// which tools show. A name written twice: the later line wins (a warning). The table is the shared file with the one beside the scene on top.
//
// Pure, like scene.h: everything the parser needs is inline here (every program that links scene.cpp builds without another source); the
// reading of the files (ParseSpots, SpotFilesFor, LoadSpotsFor) is in spots.cpp.
//
// INTEGRATION (the runtime is package A's, scene_runtime.cpp Load()): where it parses the scene text it asks for the table once and passes it,
// and it reloads the scene when a spots file changed (the watcher below), so a spot edited in the file moves the shot at once:
//
//     const LoadedSpots spots = LoadSpotsFor(g.path);                    // once for each load; keep it and the stamps in the runner
//     ParseOptions options;  options.spots = &spots.table;  options.path = g.path;
//     ParseResult r = ParseScene(text, options);
//     ...                                                                 // PollFile: also reload when SpotsChanged(g.path, spots)
//
// Without the options a scene parses as before (a spot word is an error: "no spot table"), and a camera `rig flyby` cannot find its route (the
// scene's folder is unknown). A take should copy the spots file beside the scene text it copies.
//

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "scene.h"

namespace mc::cinematic {

inline constexpr size_t kMaxSpotNameChars = 40;
inline constexpr double kMaxSpotCoordinate = 100000.0;  // the same bound as a car's position (car_verbs.h)
inline constexpr double kMaxSpotOffset = 10000.0;       // the largest @ offset or turn

struct Spot {
    std::string name;  // lower case
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double heading = 0.0;  // degrees in [0, 360): 0 faces +Z, 90 faces +X
    bool has_heading = false;
    std::string note;  // the comment after the numbers
    int line = 0;      // where it was written (0 for a spot made in code)
    std::string file;  // the file it was read from
};

namespace spots_detail {

inline char LowerChar(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

inline std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = LowerChar(c);
    return out;
}

inline bool ParseNumber(std::string_view s, double* out) {
    if (s.empty()) return false;
    const std::string copy(s);
    char* end = nullptr;
    const double v = std::strtod(copy.c_str(), &end);
    if (end == copy.c_str() || *end != '\0' || !std::isfinite(v)) return false;
    *out = v;
    return true;
}

// A coordinate or a number of degrees for a statement: "%.3f" with the zeros after the point dropped ("17.5", "-808.4", "0").
inline std::string FormatNumber(double v, int decimals = 3) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0" || s.empty()) s = "0";
    return s;
}

inline double NormalizeDegrees(double deg) {
    double d = std::fmod(deg, 360.0);
    if (d < 0.0) d += 360.0;
    if (d >= 360.0) d = 0.0;
    return d;
}

// The number of single-character edits between two short strings (for the "did you mean" of a mistyped name).
inline size_t EditDistance(std::string_view a, std::string_view b) {
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t sub = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, sub});
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

}  // namespace spots_detail

// A spot name: letters, digits and _ - . , starting with a letter or a digit, at most kMaxSpotNameChars.
inline bool IsSpotName(std::string_view s) {
    if (s.empty() || s.size() > kMaxSpotNameChars) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!(alnum || (i > 0 && (c == '_' || c == '-' || c == '.')))) return false;
    }
    return true;
}

// The table of spots. Names are not case sensitive; the order is the order they were added in (a name added again keeps its place).
class SpotTable {
  public:
    // Adds a spot; false (and the spot replaced) when the name was there already.
    bool Add(Spot spot) {
        spot.name = spots_detail::Lower(spot.name);
        for (Spot& s : spots_) {
            if (s.name == spot.name) {
                s = std::move(spot);
                return false;
            }
        }
        spots_.push_back(std::move(spot));
        return true;
    }
    const Spot* Find(std::string_view name) const {
        const std::string n = spots_detail::Lower(name);
        for (const Spot& s : spots_) {
            if (s.name == n) return &s;
        }
        return nullptr;
    }
    size_t size() const { return spots_.size(); }
    bool empty() const { return spots_.empty(); }
    const std::vector<Spot>& spots() const { return spots_; }

    // Up to `max` names that are near `name` (a prefix, a part of one, or a few edits away), nearest first, for "did you mean".
    std::vector<std::string> Suggest(std::string_view name, size_t max = 3) const {
        const std::string n = spots_detail::Lower(name);
        std::vector<std::pair<size_t, std::string>> scored;
        for (const Spot& s : spots_) {
            size_t d = spots_detail::EditDistance(n, s.name);
            const bool part = !n.empty() && (s.name.find(n) != std::string::npos || n.find(s.name) != std::string::npos);
            if (part) d = std::min<size_t>(d, 1);
            if (d <= 2 || (d <= 3 && n.size() >= 8)) scored.emplace_back(d, s.name);
        }
        std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<std::string> out;
        for (const auto& p : scored) {
            if (out.size() >= max) break;
            out.push_back(p.second);
        }
        return out;
    }

  private:
    std::vector<Spot> spots_;
};

// ---------------------------------------------------------------------------------------------------------------- the words of a statement

// One spot word, read: the name, the offset in the spot's frame, the turn.
struct SpotRef {
    std::string name;  // as written
    bool has_offset = false;
    double right = 0.0;
    double up = 0.0;
    double forward = 0.0;
    bool has_turn = false;
    double turn = 0.0;  // degrees added to the spot's heading when the word is read as a heading
};

// Whether a word is a spot word: it starts with `spot:` (not case sensitive).
inline bool IsSpotWord(std::string_view w) {
    return w.size() >= 5 && spots_detail::Lower(w.substr(0, 5)) == "spot:";
}

// Reads `spot:<name>[@<r>,<u>,<f>[,<turn>]]`. False with `error` when the word is a spot word and is malformed.
inline bool ParseSpotRef(std::string_view word, SpotRef* out, std::string* error) {
    *out = SpotRef{};
    if (!IsSpotWord(word)) {
        *error = "'" + std::string(word) + "' is not a spot word (spot:<name>)";
        return false;
    }
    std::string_view rest = word.substr(5);
    const size_t at = rest.find('@');
    const std::string_view name = at == std::string_view::npos ? rest : rest.substr(0, at);
    if (!IsSpotName(name)) {
        *error = "'" + std::string(word) + "': a spot name is letters, digits and _ - . (spot:hw-street, spot:hw-street@0,1.7,-8)";
        return false;
    }
    out->name = std::string(name);
    if (at == std::string_view::npos) return true;
    std::string_view off = rest.substr(at + 1);
    double v[4] = {0, 0, 0, 0};
    int n = 0;
    while (true) {
        const size_t comma = off.find(',');
        const std::string_view part = comma == std::string_view::npos ? off : off.substr(0, comma);
        if (n >= 4 || !spots_detail::ParseNumber(part, &v[n]) || std::fabs(v[n]) > kMaxSpotOffset) {
            *error = "'" + std::string(word) + "': after @ come the right, up and forward metres, and optionally a turn in degrees "
                     "(spot:" + out->name + "@1.5,0,-3 or spot:" + out->name + "@1.5,0,0,180)";
            return false;
        }
        ++n;
        if (comma == std::string_view::npos) break;
        off = off.substr(comma + 1);
    }
    if (n < 3) {
        *error = "'" + std::string(word) + "': @ needs right,up,forward (three numbers; spot:" + out->name + "@0,1.7,-8), got " + std::to_string(n);
        return false;
    }
    out->has_offset = true;
    out->right = v[0];
    out->up = v[1];
    out->forward = v[2];
    if (n == 4) {
        out->has_turn = true;
        out->turn = v[3];
    }
    return true;
}

// The world position of a reference: the spot plus the offset in the spot's heading frame (right = (-cos h, 0, sin h), up = Y, forward =
// (sin h, 0, cos h): the camera's and the car's frame, camera.h HeadingBasis). False with `error` when an offset is asked of a spot with no heading.
inline bool SpotPosition(const Spot& s, const SpotRef& r, double out[3], std::string* error) {
    out[0] = s.x;
    out[1] = s.y;
    out[2] = s.z;
    if (!r.has_offset || (r.right == 0.0 && r.up == 0.0 && r.forward == 0.0)) return true;
    if (!s.has_heading) {
        *error = "spot " + s.name + " has no heading, so an offset in its frame (@right,up,forward) means nothing: write a heading in spots.txt";
        return false;
    }
    const double h = s.heading * 3.14159265358979323846 / 180.0;
    const double sh = std::sin(h), ch = std::cos(h);
    out[0] += r.right * (-ch) + r.forward * sh;
    out[1] += r.up;
    out[2] += r.right * sh + r.forward * ch;
    return true;
}

// The heading a reference gives (degrees in [0, 360)): the spot's, plus the turn. The offset is not used.
inline bool SpotHeading(const Spot& s, const SpotRef& r, double* out, std::string* error) {
    if (!s.has_heading) {
        *error = "spot " + s.name + " has no heading: write one in spots.txt (the fifth number of its line)";
        return false;
    }
    *out = spots_detail::NormalizeDegrees(s.heading + (r.has_turn ? r.turn : 0.0));
    return true;
}

// Turns every spot word of a statement's words (the words after `cam <T>`, `car`, `start`) into numbers:
//   a spot word after `heading` or `yaw`  -> one word, the heading in degrees
//   any other spot word                   -> three words, x y z (every position slot of the grammar takes three numbers)
// `table` null means no table was loaded. False with `error` (the message for the statement's line) on the first word that cannot be read or
// resolved; `words` is then unchanged. Words that are not spot words are left alone.
inline bool ExpandSpotWords(std::vector<std::string>* words, const SpotTable* table, std::string* error) {
    bool any = false;
    for (const std::string& w : *words) any = any || IsSpotWord(w);
    if (!any) return true;
    const bool orbit = [&] {
        for (const std::string& w : *words) {
            if (spots_detail::Lower(w) == "orbit") return true;
        }
        return false;
    }();
    std::vector<std::string> out;
    out.reserve(words->size() + 8);
    for (size_t i = 0; i < words->size(); ++i) {
        const std::string& w = (*words)[i];
        if (!IsSpotWord(w)) {
            out.push_back(w);
            continue;
        }
        SpotRef ref;
        if (!ParseSpotRef(w, &ref, error)) return false;
        if (table == nullptr) {
            *error = "'" + w + "': no spot table is loaded for this scene (a spots.txt beside the scene file or in the shared scenes folder)";
            return false;
        }
        const Spot* spot = table->Find(ref.name);
        if (spot == nullptr) {
            *error = "unknown spot '" + ref.name + "'";
            const std::vector<std::string> similar = table->Suggest(ref.name);  // not `near`: <Windows.h> defines it
            if (!similar.empty()) {
                *error += " (did you mean ";
                for (size_t k = 0; k < similar.size(); ++k) *error += (k ? ", " : "") + similar[k];
                *error += "?)";
            } else {
                *error += " (" + std::to_string(table->size()) + " spots are known: see spots.txt)";
            }
            return false;
        }
        const std::string prev = i > 0 ? spots_detail::Lower((*words)[i - 1]) : std::string();
        if (prev == "heading" || prev == "yaw") {
            double deg = 0.0;
            if (!SpotHeading(*spot, ref, &deg, error)) return false;
            out.push_back(spots_detail::FormatNumber(deg, 2));
            continue;
        }
        if (orbit && prev == "at") {
            *error = "'" + w + "': after an orbit, `at` is an offset in the car's frame (right up forward), not a place in the world";
            return false;
        }
        double p[3];
        if (!SpotPosition(*spot, ref, p, error)) return false;
        for (double c : p) out.push_back(spots_detail::FormatNumber(c));
    }
    *words = std::move(out);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------- the files (spots.cpp)

// Reads the text of one spots file into `table` (later lines replace earlier ones of the same name). `file` is the name given in the messages and
// kept on each spot. Diagnostics: errors (a line that is not a spot) and warnings (a name given twice); a bad line is skipped.
bool ParseSpots(std::string_view text, std::string_view file, SpotTable* table, std::vector<Diagnostic>* diagnostics);

// The spots files that apply to a scene file, in the order they apply (a later one replaces a spot of an earlier one): the shared file of the
// scenes folder (`<scene folder>\..\scenes\spots.txt`), then `<scene folder>\..\spots.txt`, then `<scene folder>\spots.txt`. Only files that exist; a
// scene in ports\scripts\scenes (or any of its subfolders, recipes\) gets that folder's spots.txt.
std::vector<std::string> SpotFilesFor(const std::string& scene_path);

struct SpotFileStamp {
    std::string path;
    FileStamp stamp;
};

struct LoadedSpots {
    SpotTable table;
    std::vector<SpotFileStamp> files;  // what was read, with the stamps it had
    std::vector<std::string> problems;  // "spots.txt:12: error: ..." lines for the log
    bool Ok() const { return problems.empty(); }
};

// Reads every file SpotFilesFor names. A file that cannot be read or has a bad line says so in `problems`; the good lines still count.
LoadedSpots LoadSpotsFor(const std::string& scene_path);

// Whether the files changed since `loaded`: one of them has another stamp, or one appeared or went away. A cheap poll (a few stats).
bool SpotsChanged(const std::string& scene_path, const LoadedSpots& loaded);

// The INTEGRATION above in one call: the scene text parsed with the table of the spots files that apply to `scene_path` and with the path itself (a camera
// rig's route is looked for beside it). `loaded`, when given, receives what was read and the stamps it had (for SpotsChanged). An empty path: no table, no folder.
ParseResult ParseSceneWithSpots(std::string_view text, const std::string& scene_path, LoadedSpots* loaded = nullptr);

// The text of the spots files a scene uses, in the order they apply, one after the other (a later line of the same name replaces an earlier one, as in
// LoadSpotsFor). A take writes it as `spots.txt` beside its scene copy, so the copy reads the same places when it is run from the take folder. Empty when
// the scene has no spots file.
std::string SpotsTextFor(const std::string& scene_path);

}  // namespace mc::cinematic
