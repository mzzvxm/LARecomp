#pragma once
//
// The network half of the nick proof (identity_protocol.h), with nothing of the game in it: the pass that asks a relay
// for a challenge, gets a ticket from the master the relay names, and answers; and one signed request to a master.
// The game's worker (identity_client.cpp) and larecomp_online_tool (online_tool/identity_commands.cpp) both call it, so
// both ask the relay and the master the same way. What differs is passed in: the server, the nick, the keys, where
// tickets and recovery codes are kept, and whether the pass may claim a nick (the game always may; the tool only when
// asked).
//

#include <chrono>
#include <cstdint>
#include <map>
#include <string>

#include "../protocol/identity_protocol.h"

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
    // The master's own word for its last answer in this pass ("ok", "taken", "rate_limited", "bad_clock", "down", ...);
    // empty when the pass did not ask the master.
    std::string master_status;
    // kFailed because the pass was not allowed to claim and the master has no ticket of ours on record here (the tool
    // only; the game's pass always may claim).
    bool needs_claim = false;
};

namespace net {

using Fields = std::map<std::string, std::string>;

struct Options {
    int udp_timeout_ms = 1000;      // one datagram exchange, per try
    int udp_tries = 3;
    int master_connect_ms = 4000;
    int master_io_ms = 8000;
    int64_t clock_offset_sec = 0;   // added to the time a request carries: for the checks of a master's clock window only
};

// Whether a pass may register or refresh the nick with the master (the one call with side effects).
enum class ClaimMode {
    kAlways,   // ask for a ticket whenever there is no good one (the game)
    kRefresh,  // renew a registration already made: asks the master whenever the store holds a ticket for this nick, even
               // a good one (that is how a transfer someone started with our recovery code is noticed); else needs_claim
    kNever,    // never: use a ticket that is already there, else report needs_claim
};

// Where a pass finds and keeps what the master hands out.
class TicketHooks {
public:
    virtual ~TicketHooks() = default;
    // The ticket of this key the master with `master_key` last gave, of any age (empty/invalid = false).
    virtual bool Cached(const uint8_t (&master_key)[32], Ticket& out) = 0;
    virtual void Put(const uint8_t (&master_key)[32], const Ticket& ticket) = 0;
    // The master registered the nick (or moved it to us, or cancelled a transfer) and handed out a recovery code. It
    // is shown once ever: keep it before anything else happens.
    virtual void SetRecoveryCode(const std::string& code) = 0;
    // One line for the log (what the game writes after "[identity] ").
    virtual void Note(const std::string&) {}
};

struct CycleInput {
    std::string host;               // the relay
    uint16_t port = 31000;
    std::string nick;
    const KeyPair* keys = nullptr;
    ClaimMode claim = ClaimMode::kAlways;
    Options options;
};

struct CycleOutput {
    bool master_known = false;      // the relay answered with a master
    std::string master_host;
    uint16_t master_port = 0;
    uint8_t master_key[32] = {};
};

// One pass: challenge, ticket, proof. Fills `s` (check, detail, relay, master, ...) and returns how long until the
// next pass. `out` may be null.
std::chrono::seconds Cycle(Status& s, const CycleInput& in, TicketHooks& hooks, CycleOutput* out);

// One signed request to the master (op "claim", "recover" or "cancel"; `code` for recover). The reply's fields, or
// status=down with error=<reason> when the master could not be reached.
Fields MasterCall(const std::string& host, uint16_t port, const std::string& op, const std::string& nick,
                  const KeyPair& keys, const std::string& code, const Options& options);

// "this PC's clock is 12 min behind" for a bad_clock reply.
std::string ClockText(const Fields& reply);

struct Challenge {
    enum class State { kOk, kUnresolved, kNoSocket, kNoAnswer } state = State::kNoAnswer;
    bool enforced = false;          // the relay checks nicks
    std::string master_host;        // where tickets come from (the relay's own host when it named none)
    uint16_t master_port = 0;
    uint8_t master_key[32] = {};
};
// The relay's hello/challenge exchange alone (the first step of Cycle): tells which master a relay's players use.
Challenge AskChallenge(const std::string& host, uint16_t port, const Options& options);

}  // namespace net

}  // namespace larecomp::identity
