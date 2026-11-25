#pragma once
//
// The rules for "Use another gamertag" (F8 in the game, `identity rename` in larecomp_online_tool): which names a
// profile may take. One copy, so the two agree. The change itself is two small steps both sides also share through the
// profile store (profile/larecomp_profile_store.h): the save marker, then `user_name` in larecomp.toml; the SDK renames
// the save folder at the next start (see identity_ui.cpp).
//

#include <filesystem>
#include <string>

namespace larecomp::identity {

// Empty when `typed` may become the gamertag of the profile now called `current`, whose saves are the folders of
// `saves_root`. Otherwise the sentence the game shows for it. Checks the characters (letters, digits and spaces, up to
// 15), reserved names, "already your gamertag", a save folder that is already there under the new name, and the save
// folder of the current one being there.
std::string RenameProblem(const std::string& typed, const std::string& current,
                          const std::filesystem::path& saves_root);

}  // namespace larecomp::identity
