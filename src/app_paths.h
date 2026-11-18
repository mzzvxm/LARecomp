#pragma once
//
// The folders LARecomp's first-launch code (ISO installer, save importer, profile picker, boot build) works with.
// It replaces rex::PathConfig of the 0.10 SDK (24 uses). The values live in rexglue settings now:
//   user_root -> app.user_root      cache_root -> app.cache_root     config_file -> app.config_file
//   game_root -> title.game_root    update_root -> title.update_root metadata_root -> title.metadata_root
// LarecompApp::CurrentPaths() reads them; the wizards that change one (the installer: game_root) write the setting
// back with Setting::Set before LaunchTitle (title.* are kUntilLaunch; app.* are frozen when OnInit starts).
//
#include <filesystem>

namespace larecomp {

struct Paths {
  std::filesystem::path user_root;
  std::filesystem::path cache_root;
  std::filesystem::path config_file;
  std::filesystem::path game_root;
  std::filesystem::path update_root;
  std::filesystem::path metadata_root;
};

}  // namespace larecomp
