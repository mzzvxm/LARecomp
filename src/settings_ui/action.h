#pragma once

#include <optional>
#include <string_view>

namespace larecomp::settings_ui {

int DumpSettingsUi(std::optional<std::string_view> output);
void RefreshSettingsUi();

}  // namespace larecomp::settings_ui
