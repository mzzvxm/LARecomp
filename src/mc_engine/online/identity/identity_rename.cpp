#include "identity_rename.h"

#include <system_error>

#include "../../../profile/larecomp_profile_store.h"

namespace larecomp::identity {

std::string RenameProblem(const std::string& typed, const std::string& current,
                          const std::filesystem::path& root) {
    namespace pf = larecomp::profiles;
    const std::string name = pf::SanitizeName(typed);
    if (name.empty() || name != typed) return "Use only letters, digits and spaces, up to 15 characters.";
    if (pf::IsReservedName(name)) return "That name is reserved.";
    if (pf::SameName(name, current)) return "That is already your gamertag.";
    if (root.empty()) return "The save folder is not known; change user_name by hand.";
    std::error_code ec;
    if (std::filesystem::exists(root / name, ec)) return "You already have a save with that name.";
    if (!std::filesystem::exists(root / current, ec)) return "Your save folder was not found; change user_name by hand.";
    return {};
}

}  // namespace larecomp::identity
