#pragma once
//
// Proves the player's gamertag to the relay the game is online with
// (identity_protocol.h): asks the relay for a challenge, gets a ticket from
// the master the relay names, and answers. A background thread repeats it
// before the relay's admission runs out, and whenever the server changes.
//
// The proof goes out from our own socket; the SDK's online client sends the
// register from its socket on the same machine, and the relay pairs the two by
// source IP and nick. Until the proof lands, the relay just does not answer
// the register, and the SDK client keeps retrying.
//

#include <cstdint>
#include <string>

#include "identity_net.h"  // Check and Status

namespace larecomp::identity {

// Starts the worker. After ApplyProfileIdentity; once.
void StartClient();

Status GetStatus();

// Ask again now (server changed, or the player pressed the button).
void CheckNow();

// Recovery code for our nick, owned by another key. The result shows up in
// GetStatus (kClaimPending on success).
void RequestRecover(const std::string& code);

// We own the nick and someone asked for it with the code: stop it. The
// master answers with a new code (PendingRecoveryCode).
void RequestCancelTransfer();

}  // namespace larecomp::identity
