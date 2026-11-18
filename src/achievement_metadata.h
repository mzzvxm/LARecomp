#pragma once

#include <cstddef>
#include <string_view>

namespace rex::xam {
class IAchievementManager;
}

namespace larecomp {

// Merges the [[achievements]] entries of a TOML document into the manager.
//
// This is the game-side copy of what the forked SDK did in
// AchievementManager::LoadMetadataString. rexglue 0.11 has IAchievementManager::LoadMetadataFile, which replaces the
// entries instead of layering them and knows no language sub-tables, so this stays.
// Entries layer onto whatever is already registered (normally the title's XDBF
// table) instead of replacing it, so a file carrying only translated text does
// not blank out gamerscore, flags or image ids. Optional per-language
// sub-tables keyed by the user's language ([achievements.Portuguese]) override
// the base text.
//
// Returns the number of entries applied.
size_t LoadAchievementMetadata(rex::xam::IAchievementManager& manager, std::string_view toml_text,
                               std::string_view source_name = "<memory>");

}  // namespace larecomp
