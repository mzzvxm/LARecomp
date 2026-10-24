#pragma once
//
// The master's nick registry (../protocol/identity_protocol.h): nick -> the
// public key that registered it first, a recovery code per nick, and tickets
// signed with the master's own key.
//
// No admin path: a nick changes hands only by its recovery code (after a hold
// the current owner can cancel from the game) or by sitting unused past the
// expiry. Everything lives in two files next to the exe:
//   identity_master.key  the master's signing seed (hex); made on first run
//   identity_db.txt      one nick per line, rewritten on every change
//

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

#include "../protocol/identity_protocol.h"

namespace larecomp::identity {

struct RegistryConfig {
    std::string dir;                              // with a trailing slash; empty = cwd
    int64_t transfer_hold_sec = 7 * 24 * 3600;    // recovery code -> new key, cancelable
    int64_t expire_sec = 90 * 24 * 3600;          // unused this long = free again
};

class Registry {
public:
    // Loads (or creates) the master key and the database. False = cannot run.
    bool Open(const RegistryConfig& config, std::string* error);

    const uint8_t (&public_key() const)[32] { return keys_.pub; }
    size_t size() const;

    // One POST to kMasterPath: `body` is the request text, `peer_ip` the
    // caller's address (rate limits). Returns the response text.
    std::string Handle(const std::string& body, const std::string& peer_ip);

private:
    struct Record {
        std::string display;  // as last claimed
        uint8_t pub[32] = {};
        int64_t created = 0;
        int64_t last_seen = 0;
        std::string recovery_hash;  // RecoveryHash(normalized, code)
        uint8_t pending_pub[32] = {};
        int64_t pending_until = 0;  // 0 = no transfer pending
    };

    std::string Claim(const std::map<std::string, std::string>& f, int64_t now,
                      const std::string& peer_ip);
    std::string Recover(const std::map<std::string, std::string>& f, int64_t now,
                        const std::string& peer_ip);
    std::string Cancel(const std::map<std::string, std::string>& f, int64_t now);

    // Signature, clock and replay checks shared by every op. On failure
    // returns the response to send; empty = the request is genuine.
    std::string Authenticate(const std::map<std::string, std::string>& f, const std::string& op,
                             const std::string& extra, int64_t now, std::string& nick,
                             uint8_t (&pub)[32]);

    std::string IssueTicket(const Record& r, int64_t now) const;
    bool Expired(const Record& r, int64_t now) const;
    void Save() const;
    bool RateLimited(std::unordered_map<std::string, std::pair<int64_t, int>>& table,
                     const std::string& key, int64_t window_sec, int max, int64_t now);

    mutable std::mutex mtx_;
    RegistryConfig config_;
    KeyPair keys_{};
    std::map<std::string, Record> db_;  // key = NormalizeNick
    std::unordered_map<std::string, int64_t> seen_sigs_;  // replay guard: sig hex -> time
    std::unordered_map<std::string, std::pair<int64_t, int>> ip_rate_;
    std::unordered_map<std::string, std::pair<int64_t, int>> recover_ip_rate_;
    std::unordered_map<std::string, std::pair<int64_t, int>> recover_nick_rate_;
    std::unordered_map<std::string, std::pair<int64_t, int>> register_ip_rate_;
};

}  // namespace larecomp::identity
