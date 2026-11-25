#include "garage_fixes.h"

#include <cstddef>
#include <cstdlib>
#include <iterator>

// Three defects, all in tune/garage:
//
//   Low riders (Impala 64, Malibu 65, Bel Air 55, Riviera 63) ship the side
//   exhaust models exh_side1..4 and no menu reaches them: the exterior menu
//   needs category 47 under ext_side, and Parts.gtun a side_exhaust section.
//
//   CLK63 AMG ships models 0..4 for bumpers, skirts, spoiler, lamps and hood,
//   and its Parts.gtun stops at the two "_ex" parts, leaving 3 and 4 out.
//
//   The player city bus (vp_citybus_07) has a car config and no garage tune at
//   all; its performance menu crashes. It gets the performance list only.
//
// A Parts.gtun is sections of "<!-- name -->", an entry count, and four lines an
// entry: string key, brand, price, model variant. An exterior.gtun section is
// its name, an entry count, and one "id name" line an entry.

namespace mc::garage_fixes {
namespace {

struct Part {
    const char* section;
    const char* key;
    int brand;
    int price;
    int variant;
};

constexpr Part kSideExhaust[] = {
    {"side_exhaust", "side_exhaust_0", 37, 0, 0},
    {"side_exhaust", "side_exhaust_1", 0, 130, 4},
    {"side_exhaust", "side_exhaust_2", 0, 182, 1},
    {"side_exhaust", "side_exhaust_3", 0, 230, 2},
    {"side_exhaust", "side_exhaust_4", 0, 273, 3},
};

constexpr Part kClk63[] = {
    {"bumper_front", "bumper_f_3", 0, 440, 3},
    {"bumper_front", "bumper_f_4", 0, 525, 4},
    {"bumper_rear", "bumper_r_3", 0, 375, 3},
    {"bumper_rear", "bumper_r_4", 0, 450, 4},
    {"skirts", "skirts_3", 0, 380, 3},
    {"skirts", "skirts_4", 0, 450, 4},
    {"spoiler", "spoiler_3", 0, 1015, 3},
    {"spoiler", "spoiler_4", 0, 1200, 4},
    {"headlight", "headlight_3", 0, 445, 3},
    {"headlight", "headlight_4", 0, 565, 4},
    {"taillight", "taillight_3", 0, 445, 3},
    {"taillight", "taillight_4", 0, 565, 4},
    {"hood", "hood_3", 0, 1260, 3},
    {"hood", "hood_4", 0, 1440, 4},
};

struct Upgrade {
    const char* key;
    int price;
    int second_price;
};

// The bus's performance list; an entry's index is its position here.
constexpr Upgrade kBusUpgrades[] = {
    {"gp_exhaust", 600, 880},    {"gp_headers", 600, 880},
    {"gp_intake", 560, 830},     {"gp_turbo", 1130, 1650},
    {"gp_engine", 2250, 3300},   {"gp_chip", 900, 1320},
    {"gp_nitrous", 3150, 4620},  {"gp_clutch", 900, 1320},
    {"gp_gears", 1580, 2310},    {"gp_shockSprings", 2400, 3520},
    {"gp_swaybars", 980, 1430},  {"gp_brakes", 830, 1210},
    {"gp_tires", 1800, 2640},
};

constexpr const char* kLowRiders[] = {
    "vpd_chv_impala_64",
    "vpd_chv_malibu_65",
    "vpd_chv_belair_55",
    "vpd_buk_riviera_63",
};
constexpr const char* kClk63Parts = "tune/garage/vpd_mbz_clk63amg_08_Parts.gtun";
constexpr const char* kBusPerformance = "tune/garage/vp_citybus_07_Performance.gtun";

std::string TunePath(const char* car, const char* kind) {
    return std::string("tune/garage/") + car + "_" + kind + ".gtun";
}

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

struct Lines {
    std::vector<std::string> all;
    std::string newline;

    explicit Lines(const std::string& text)
        : newline(text.find("\r\n") != std::string::npos ? "\r\n" : "\n") {
        for (size_t at = 0; at <= text.size();) {
            const size_t end = text.find('\n', at);
            std::string line = text.substr(
                at, end == std::string::npos ? std::string::npos : end - at);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            all.push_back(std::move(line));
            if (end == std::string::npos) break;
            at = end + 1;
        }
    }

    std::string Join() const {
        std::string out;
        for (size_t i = 0; i < all.size(); ++i)
            out += all[i] + (i + 1 < all.size() ? newline : "");
        return out;
    }

    size_t Find(const std::string& what, size_t from = 0) const {
        while (from < all.size() && Trim(all[from]) != what) ++from;
        return from;
    }

    size_t NextFilled(size_t from) const {
        while (from < all.size() && Trim(all[from]).empty()) ++from;
        return from;
    }

    // Appends one entry (one or more lines) to the section headed by `header`,
    // after its last entry, and raises its count. False when the section is not
    // there or already holds a line equal to `entry.front()`.
    bool Append(const std::string& header, const std::vector<std::string>& entry) {
        const size_t head = Find(header);
        if (head == all.size()) return false;
        const size_t count_at = NextFilled(head + 1);
        if (count_at == all.size()) return false;
        const int count = std::atoi(Trim(all[count_at]).c_str());
        if (count <= 0) return false;

        size_t after = count_at + 1;
        const int wanted = count * static_cast<int>(entry.size());
        for (int seen = 0; seen < wanted; ++after) {
            if (after == all.size()) return false;  // shorter than its count
            const std::string line = Trim(all[after]);
            if (line.empty()) continue;
            if (line == entry.front()) return false;
            ++seen;
        }
        all[count_at] = std::to_string(count + 1);
        all.insert(all.begin() + static_cast<std::ptrdiff_t>(after), entry.begin(),
                   entry.end());
        return true;
    }
};

std::vector<std::string> EntryLines(const Part& part) {
    return {part.key, std::to_string(part.brand), std::to_string(part.price),
            std::to_string(part.variant)};
}

bool AddClk63Parts(std::string& text) {
    Lines lines(text);
    size_t added = 0;
    for (const Part& part : kClk63)
        added += lines.Append(std::string("<!-- ") + part.section + " -->",
                              EntryLines(part));
    if (!added) return false;
    text = lines.Join();
    return true;
}

bool AddSideExhaustParts(std::string& text) {
    Lines lines(text);
    if (lines.Find("<!-- side_exhaust -->") != lines.all.size()) return false;
    // The game's own sections: without them this is not a Parts.gtun.
    if (lines.Find("<!-- skirts -->") == lines.all.size()) return false;

    while (!lines.all.empty() && Trim(lines.all.back()).empty()) lines.all.pop_back();
    lines.all.push_back("");
    lines.all.push_back("<!-- side_exhaust -->");
    lines.all.push_back(std::to_string(std::size(kSideExhaust)));
    for (const Part& part : kSideExhaust)
        for (std::string& line : EntryLines(part)) lines.all.push_back(std::move(line));
    lines.all.push_back("");
    text = lines.Join();
    return true;
}

bool AddSideExhaustMenu(std::string& text) {
    Lines lines(text);
    if (!lines.Append("ext_side", {"47 side_exhaust"})) return false;
    text = lines.Join();
    return true;
}

bool MakeBusPerformance(std::string& text) {
    if (!text.empty()) return false;
    text = std::to_string(std::size(kBusUpgrades)) + "\r\n";
    int index = 0;
    for (const Upgrade& up : kBusUpgrades) {
        text += std::string(up.key) + "\r\n" + std::to_string(index++) + "\r\n" +
                std::to_string(up.price) + "\r\n" +
                std::to_string(up.second_price) + "\r\n";
    }
    return true;
}

}  // namespace

std::vector<std::string> Paths() {
    std::vector<std::string> out;
    for (const char* car : kLowRiders) {
        out.push_back(TunePath(car, "Parts"));
        out.push_back(TunePath(car, "exterior"));
    }
    out.push_back(kClk63Parts);
    out.push_back(kBusPerformance);
    return out;
}

bool IsNewFile(const std::string& path) { return path == kBusPerformance; }

bool Apply(const std::string& path, std::string& text) {
    if (path == kBusPerformance) return MakeBusPerformance(text);
    if (path == kClk63Parts) return AddClk63Parts(text);
    for (const char* car : kLowRiders) {
        if (path == TunePath(car, "Parts")) return AddSideExhaustParts(text);
        if (path == TunePath(car, "exterior")) return AddSideExhaustMenu(text);
    }
    return false;
}

}  // namespace mc::garage_fixes
