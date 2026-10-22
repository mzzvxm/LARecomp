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

namespace larecomp::identity {

enum class Check {
    kOff,            // online is off
    kWorking,        // asking right now
    kNoAnswer,       // the relay does not answer (old relay, or down)
    kNotChecked,     // the relay checks no nicks: anyone plays under any nick
    kVerified,       // the relay admits this nick from here
    kTaken,          // someone else owns the nick on the relay's master
    kClaimPending,   // our recovery code moves the nick to us at claim_until
    kMasterDown,     // the master the relay names does not answer
    kFailed,         // anything else; see detail
};

struct Status {
    Check check = Check::kOff;
    std::string detail;       // human-readable, for the window
    std::string relay;        // host:port as signed
    std::string master;       // host:port the relay named
    std::string owner_nick;   // kTaken: how the owner spells it
    int64_t claim_until = 0;  // kClaimPending: unix time the nick moves to us
    // Someone used our recovery code: the nick moves to them at this unix time
    // unless we cancel. 0 = nothing pending.
    int64_t incoming_until = 0;
    uint64_t generation = 0;  // bumps whenever anything above changes
};

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
