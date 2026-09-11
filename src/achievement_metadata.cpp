#include "achievement_metadata.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <string>

#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/system/achievement_manager.h>

#include "larecomp_log.h"

namespace larecomp {
namespace {

// The language the per-achievement sub-tables are keyed by. Read through the
// registry rather than REXCVAR_GET because user_language is a uint32_t on the
// stock SDK and a std::string on our fork; GetFlagByName gives text either way.
// A numeric value is the raw Xbox language id, so map it back to the name the
// TOML uses (same table as the SDK's xam_user.cpp).
std::string CurrentLanguageName() {
  const std::string raw = rex::cvar::GetFlagByName("user_language");
  if (raw.empty()) {
    return "English";
  }
  const bool numeric = std::all_of(raw.begin(), raw.end(),
                                   [](unsigned char c) { return std::isdigit(c) != 0; });
  if (!numeric) {
    return raw;
  }

  static const std::map<uint32_t, std::string> kLanguageMap = {
      {1, "English"},    {2, "Japanese"},
      {3, "German"},     {4, "French"},
      {5, "Spanish"},    {6, "Italian"},
      {7, "Korean"},     {8, "Traditional Chinese"},
      {9, "Portuguese"}, {11, "Polish"},
      {12, "Russian"},   {13, "Swedish"},
      {14, "Turkish"},   {15, "Norwegian"},
      {16, "Dutch"},     {17, "Simplified Chinese"}};

  const auto it = kLanguageMap.find(static_cast<uint32_t>(std::stoul(raw)));
  return it != kLanguageMap.end() ? it->second : std::string("English");
}

}  // namespace

size_t LoadAchievementMetadata(rex::system::AchievementManager& manager,
                               std::string_view toml_text, std::string_view source_name) {
  toml::table table;
  try {
    table = toml::parse(toml_text);
  } catch (const toml::parse_error& err) {
    LARECOMP_APP_ERROR("Achievement metadata failed to parse ({}): {}", source_name,
                       err.what());
    return 0;
  }

  const auto* entries = table["achievements"].as_array();
  if (!entries) {
    LARECOMP_APP_ERROR("Achievement metadata has no [[achievements]] entries: {}", source_name);
    return 0;
  }

  const std::string language = CurrentLanguageName();

  size_t loaded = 0;
  for (const auto& node : *entries) {
    const auto* entry = node.as_table();
    if (!entry) {
      continue;
    }

    rex::system::AchievementInfo info;
    info.id = static_cast<uint32_t>((*entry)["id"].value_or<int64_t>(0));

    // Layer onto what the title's XDBF table already registered.
    if (auto existing = manager.FindAchievement(info.id)) {
      info = *existing;
    }

    auto assign = [&](const toml::table& src) {
      if (auto label = src["label"].value<std::string>())
        info.label = *label;
      if (auto description = src["description"].value<std::string>())
        info.description = *description;
      if (auto unachieved = src["unachieved_description"].value<std::string>())
        info.unachieved_description = *unachieved;
      if (auto icon_path = src["icon_path"].value<std::string>())
        info.icon_path = *icon_path;
    };

    assign(*entry);
    if (auto image_id = (*entry)["image_id"].value<int64_t>())
      info.image_id = static_cast<uint32_t>(*image_id);
    if (auto gamerscore = (*entry)["gamerscore"].value<int64_t>())
      info.gamerscore = static_cast<uint32_t>(*gamerscore);
    if (auto flags = (*entry)["flags"].value<int64_t>())
      info.flags = static_cast<uint32_t>(*flags);

    // Absent language sub-table = keep the base text.
    if (const auto* localized = (*entry)[language].as_table()) {
      assign(*localized);
    }

    if (manager.RegisterAchievement(std::move(info))) {
      ++loaded;
    }
  }

  LARECOMP_APP_INFO("Loaded {} achievement metadata entries from {} (language {})", loaded,
                    source_name, language);
  return loaded;
}

}  // namespace larecomp
