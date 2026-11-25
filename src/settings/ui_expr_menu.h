#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace mcla_menu {

// Generated values carry their stored text and their game label. A generator
// selects the nearest or matching entry where the setting uses a different
// representation from the generated values.
struct GeneratedChoice {
    std::string label;
    std::string id;
};

struct GeneratedChoices {
    const char* setting = nullptr;
    std::vector<GeneratedChoice> entries;
    int current = 0;
};

GeneratedChoices DisplaySizeChoices(std::string_view display_mode);
GeneratedChoices RenderScaleChoices(std::string_view renderer);
GeneratedChoices Fsr1ModeChoices();
GeneratedChoices SharpnessChoices(std::string_view setting);
std::string_view VendorDenoiserKind();
std::string_view VendorDenoiserState();

}  // namespace mcla_menu
