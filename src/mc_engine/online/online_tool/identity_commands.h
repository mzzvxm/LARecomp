#pragma once
//
// `larecomp_online_tool identity ...`: the player's online identity from the command line, for the launcher (README-
// identity.md has the commands and the documents). It is built from the game's own identity code: the key file, its
// DPAPI seal, the ticket cache and the backup file (identity/identity_core.*), and the relay and master exchange
// (identity/identity_net.*), so a key written here is the game's key and the other way round.
//
// Rules the code keeps (the first two are the point of this file):
//   - the key, the Rate My Ride token and a password are never printed or put in a command line; a password or a
//     recovery code comes in on stdin. The only secret that is ever printed is the recovery code `recovery show` was
//     asked for (and, as a last resort, one that could not be stored, so that it is not lost);
//   - a command that changes a file of the identity (or larecomp.toml) refuses while the game runs, checked when it
//     starts and again right before each write (the game rewrites the whole key file on its own changes);
//   - the master is asked only for what the command was told to ask: nothing registers a nick unless --claim is given
//     (or `rename` is told to claim), so the master's limits (30 requests a minute, 3 new nicks a day) are never used
//     up by accident.
//

#include <functional>
#include <string>
#include <vector>

namespace larecomp::online_tool {

struct IdentityResult {
    std::string json;     // one document, no trailing newline
    int exit_code = 0;    // 0 = ok, 1 = the command could not be done (error in the document), 2 = wrong command line
};

// Gives the first line of stdin (without its line end); false when there is none. Only asked for by the commands that
// take a password or a recovery code.
using ReadSecretLine = std::function<bool(std::string&)>;

// `args` are the words after "identity".
IdentityResult RunIdentity(const std::vector<std::string>& args, const ReadSecretLine& read_secret_line);

// The names of the actions, for --protocol ("show", "check", "backup", ...).
std::vector<std::string> IdentityActions();

// The usage text of the identity commands (part of --help).
const char* IdentityUsageText();

}  // namespace larecomp::online_tool
