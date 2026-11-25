//
// The spots file's reader and the lookup of the files a scene uses: see spots.h for the format and for what a spot word is (those parts are
// inline there, because scene.cpp needs them and every program that links scene.cpp builds without another source).
//

#include "spots.h"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace mc::cinematic {

namespace {

constexpr uintmax_t kMaxSpotsFileBytes = 4u * 1024 * 1024;

std::filesystem::path PathFromUtf8(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

std::string Utf8(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string Trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return std::string(s.substr(a, b - a));
}

// Blank or comma separated fields.
std::vector<std::string> Fields(std::string_view body) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : body) {
        if (c == ' ' || c == '\t' || c == '\r' || c == ',') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

FileStamp StampOf(const std::filesystem::path& p) {
    FileStamp s;
    std::error_code ec;
    s.exists = std::filesystem::is_regular_file(p, ec);
    if (s.exists) {
        s.size = static_cast<uint64_t>(std::filesystem::file_size(p, ec));
        s.mtime = static_cast<int64_t>(std::filesystem::last_write_time(p, ec).time_since_epoch().count());
    }
    return s;
}

}  // namespace

bool ParseSpots(std::string_view text, std::string_view file, SpotTable* table, std::vector<Diagnostic>* diagnostics) {
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);  // a byte order mark
    bool ok = true;
    const auto error = [&](int line, std::string message) {
        ok = false;
        if (diagnostics) diagnostics->push_back({line, Severity::kError, std::move(message)});
    };
    const auto warn = [&](int line, std::string message) {
        if (diagnostics) diagnostics->push_back({line, Severity::kWarning, std::move(message)});
    };
    int line_no = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        ++line_no;
        const std::string_view line = text.substr(pos, nl - pos);
        pos = nl + 1;

        // a comment starts at a # at the start of the line or after a blank; what follows it is the note
        std::string_view body = line;
        std::string note;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '#' && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t')) {
                body = line.substr(0, i);
                note = Trim(line.substr(i + 1));
                break;
            }
        }
        const std::vector<std::string> f = Fields(body);
        if (!f.empty()) {
            Spot s;
            s.line = line_no;
            s.file = std::string(file);
            s.note = note;
            bool good = true;
            if (!IsSpotName(f[0])) {
                error(line_no, "'" + f[0] + "' is not a spot name (letters, digits and _ - . , up to 40, starting with a letter or a digit)");
                good = false;
            } else if (f.size() != 4 && f.size() != 5) {
                error(line_no, "spot " + f[0] + ": <name> <x> <y> <z> [<heading>] (got " + std::to_string(f.size() - 1) + " numbers)");
                good = false;
            }
            double v[4] = {0, 0, 0, 0};
            for (size_t k = 1; good && k < f.size(); ++k) {
                if (!spots_detail::ParseNumber(f[k], &v[k - 1])) {
                    error(line_no, "spot " + f[0] + ": '" + f[k] + "' is not a number");
                    good = false;
                }
            }
            if (good && (std::fabs(v[0]) > kMaxSpotCoordinate || std::fabs(v[1]) > kMaxSpotCoordinate || std::fabs(v[2]) > kMaxSpotCoordinate)) {
                error(line_no, "spot " + f[0] + ": a coordinate outside the world (more than 100000 from the origin)");
                good = false;
            }
            if (good) {
                s.name = spots_detail::Lower(f[0]);
                s.x = v[0];
                s.y = v[1];
                s.z = v[2];
                if (f.size() == 5) {
                    s.heading = spots_detail::NormalizeDegrees(v[3]);
                    s.has_heading = true;
                }
                const Spot* before = table->Find(s.name);
                if (before != nullptr && before->file == s.file) {
                    warn(line_no, "spot " + s.name + " was already given on line " + std::to_string(before->line) + ": this line replaces it");
                }
                table->Add(std::move(s));
            }
        }
        if (nl >= text.size()) break;
    }
    return ok;
}

std::vector<std::string> SpotFilesFor(const std::string& scene_path) {
    std::vector<std::string> out;
    if (scene_path.empty()) return out;
    std::filesystem::path dir = PathFromUtf8(scene_path).parent_path();
    if (dir.empty()) dir = ".";
    const std::filesystem::path candidates[] = {dir / ".." / "scenes" / "spots.txt", dir / ".." / "spots.txt", dir / "spots.txt"};
    std::error_code ec;
    std::vector<std::string> seen;
    for (const std::filesystem::path& c : candidates) {
        if (!std::filesystem::is_regular_file(c, ec)) continue;
        const std::string key = Utf8(std::filesystem::weakly_canonical(c, ec));
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;  // the scenes folder itself is two of the three
        seen.push_back(key);
        out.push_back(Utf8(c.lexically_normal()));
    }
    return out;
}

LoadedSpots LoadSpotsFor(const std::string& scene_path) {
    LoadedSpots loaded;
    for (const std::string& path : SpotFilesFor(scene_path)) {
        const std::filesystem::path p = PathFromUtf8(path);
        loaded.files.push_back({path, StampOf(p)});
        std::error_code ec;
        const uintmax_t size = std::filesystem::file_size(p, ec);
        if (!ec && size > kMaxSpotsFileBytes) {
            loaded.problems.push_back(path + ": error: the file is " + std::to_string(size / 1024) + " KB: too big for a spots file");
            continue;
        }
        std::ifstream in(p, std::ios::binary);
        if (!in) {
            loaded.problems.push_back(path + ": error: cannot read the file");
            continue;
        }
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<Diagnostic> diags;
        ParseSpots(text, path, &loaded.table, &diags);
        for (const Diagnostic& d : diags) {
            // a warning is not a problem: the scene still resolves
            if (d.severity == Severity::kError) loaded.problems.push_back(FormatDiagnostic(d, path));
        }
    }
    return loaded;
}

bool SpotsChanged(const std::string& scene_path, const LoadedSpots& loaded) {
    const std::vector<std::string> now = SpotFilesFor(scene_path);
    if (now.size() != loaded.files.size()) return true;
    for (size_t i = 0; i < now.size(); ++i) {
        if (now[i] != loaded.files[i].path) return true;
        if (!(StampOf(PathFromUtf8(now[i])) == loaded.files[i].stamp)) return true;
    }
    return false;
}

ParseResult ParseSceneWithSpots(std::string_view text, const std::string& scene_path, LoadedSpots* loaded) {
    LoadedSpots local;
    LoadedSpots& spots = loaded != nullptr ? *loaded : local;
    spots = LoadSpotsFor(scene_path);
    ParseOptions options;
    options.spots = &spots.table;
    options.path = scene_path;
    return ParseScene(text, options);
}

std::string SpotsTextFor(const std::string& scene_path) {
    std::string out;
    for (const std::string& path : SpotFilesFor(scene_path)) {
        std::error_code ec;
        const uintmax_t size = std::filesystem::file_size(PathFromUtf8(path), ec);
        if (ec || size > kMaxSpotsFileBytes) continue;
        std::ifstream in(PathFromUtf8(path), std::ios::binary);
        if (!in) continue;
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        out += "# from " + path + "\n" + text;
        if (text.empty() || text.back() != '\n') out += '\n';
    }
    return out;
}

}  // namespace mc::cinematic
