#include "identity_registry.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace larecomp::identity {

namespace {

constexpr const char* kKeyFile = "identity_master.key";
constexpr const char* kDbFile = "identity_db.txt";
constexpr const char* kDbHeader = "# larecomp nick registry v1";

// Per caller IP: requests per minute of any kind, then recovery attempts.
constexpr int kRequestsPerIpPerMinute = 30;
constexpr int kRecoverPerIpPerHour = 10;
constexpr int kRecoverPerNickPerHour = 5;
constexpr int kRegistrationsPerIpPerDay = 3;

std::string Field(const std::map<std::string, std::string>& f, const char* k) {
    auto it = f.find(k);
    return it == f.end() ? std::string() : it->second;
}

std::string Reply(std::initializer_list<std::pair<const char*, std::string>> kv) {
    std::string s;
    for (const auto& [k, v] : kv) s += std::string(k) + "=" + v + "\n";
    return s;
}

bool Zero(const uint8_t (&k)[32]) {
    for (uint8_t b : k)
        if (b) return false;
    return true;
}

std::vector<std::string> SplitTabs(const std::string& line) {
    std::vector<std::string> out;
    size_t pos = 0;
    for (;;) {
        const size_t t = line.find('\t', pos);
        out.push_back(line.substr(pos, t == std::string::npos ? std::string::npos : t - pos));
        if (t == std::string::npos) break;
        pos = t + 1;
    }
    return out;
}

}  // namespace

bool Registry::Open(const RegistryConfig& config, std::string* error) {
    std::lock_guard<std::mutex> lk(mtx_);
    config_ = config;

    // Master key: made once, then kept. Losing it means every relay must pin
    // the new public key and every ticket in the wild stops working.
    uint8_t seed[32];
    {
        std::ifstream in(config_.dir + kKeyFile);
        std::string hex;
        if (in && (in >> hex) && FromHex(hex, seed, sizeof(seed))) {
        } else if (!in) {
            RandomBytes(seed, sizeof(seed));
            std::ofstream out(config_.dir + kKeyFile, std::ios::trunc);
            out << ToHex(seed, sizeof(seed)) << "\n";
            if (!out) {
                if (error) *error = "cannot write " + config_.dir + kKeyFile;
                return false;
            }
        } else {
            if (error) *error = config_.dir + kKeyFile + " is unreadable; refusing to replace it";
            return false;
        }
    }
    keys_ = KeyPairFromSeed(seed);
    crypto_wipe(seed, sizeof(seed));

    db_.clear();
    std::ifstream in(config_.dir + kDbFile);
    std::string line;
    while (in && std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        // norm, display, pub, created, last_seen, recovery_hash, pending_pub, pending_until
        const auto c = SplitTabs(line);
        if (c.size() != 8) continue;
        Record r;
        r.display = c[1];
        if (!FromHex(c[2], r.pub, 32)) continue;
        r.created = std::atoll(c[3].c_str());
        r.last_seen = std::atoll(c[4].c_str());
        r.recovery_hash = c[5];
        if (!c[6].empty() && !FromHex(c[6], r.pending_pub, 32)) continue;
        r.pending_until = std::atoll(c[7].c_str());
        db_[c[0]] = r;
    }
    return true;
}

size_t Registry::size() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return db_.size();
}

void Registry::Save() const {
    const std::string path = config_.dir + kDbFile;
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
        out << kDbHeader << "\n";
        for (const auto& [norm, r] : db_) {
            out << norm << '\t' << r.display << '\t' << ToHex(r.pub, 32) << '\t' << r.created << '\t'
                << r.last_seen << '\t' << r.recovery_hash << '\t'
                << (Zero(r.pending_pub) ? std::string() : ToHex(r.pending_pub, 32)) << '\t'
                << r.pending_until << "\n";
        }
        if (!out) {
            std::printf("identity: cannot write %s\n", tmp.c_str());
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) std::printf("identity: cannot replace %s: %s\n", path.c_str(), ec.message().c_str());
}

bool Registry::RateLimited(std::unordered_map<std::string, std::pair<int64_t, int>>& table,
                           const std::string& key, int64_t window_sec, int max, int64_t now) {
    if (table.size() > 65536) table.clear();
    auto& slot = table[key];
    const int64_t window = now / window_sec;
    if (slot.first != window) slot = {window, 0};
    return ++slot.second > max;
}

bool Registry::Expired(const Record& r, int64_t now) const {
    return now - r.last_seen > config_.expire_sec;
}

std::string Registry::IssueTicket(const Record& r, int64_t now) const {
    Ticket t{};
    t.body.version = kTicketVersion;
    t.body.nick_len = uint8_t(r.display.size());
    std::memcpy(t.body.nick, r.display.data(), r.display.size());
    std::memcpy(t.body.pubkey, r.pub, 32);
    t.body.expires = uint64_t(now + kTicketLifetimeSec);
    Sign(t.sig, keys_, TicketMessage(t.body));
    return ToHex(reinterpret_cast<const uint8_t*>(&t), sizeof(t));
}

std::string Registry::Authenticate(const std::map<std::string, std::string>& f,
                                   const std::string& op, const std::string& extra, int64_t now,
                                   std::string& nick, uint8_t (&pub)[32]) {
    nick = Field(f, "nick");
    const std::string pub_hex = Field(f, "pubkey");
    const std::string sig_hex = Field(f, "sig");
    const int64_t time = std::atoll(Field(f, "time").c_str());
    uint8_t sig[64];
    if (!ValidNick(nick)) return Reply({{"status", "bad_nick"}});
    if (!FromHex(pub_hex, pub, 32) || !FromHex(sig_hex, sig, 64)) return Reply({{"status", "bad_request"}});
    if (time < now - kClockSkewSec || time > now + kClockSkewSec)
        return Reply({{"status", "bad_clock"}, {"server_time", std::to_string(now)}});
    const std::string salt = Field(f, "salt");
    if (salt.size() != 16) return Reply({{"status", "bad_request"}});
    if (!CheckSignature(sig, pub, RequestMessage(op, nick, pub_hex, time, salt, extra)))
        return Reply({{"status", "bad_signature"}});
    // A captured request can't be sent again (the clock window bounds how
    // long the guard has to remember it).
    for (auto it = seen_sigs_.begin(); it != seen_sigs_.end();) {
        if (now - it->second > 2 * kClockSkewSec)
            it = seen_sigs_.erase(it);
        else
            ++it;
    }
    if (!seen_sigs_.emplace(sig_hex, now).second) return Reply({{"status", "replay"}});
    return {};
}

std::string Registry::Handle(const std::string& body, const std::string& peer_ip) {
    std::map<std::string, std::string> f;
    ParseFields(body, f);
    const int64_t now = NowUnix();
    std::lock_guard<std::mutex> lk(mtx_);
    if (RateLimited(ip_rate_, peer_ip, 60, kRequestsPerIpPerMinute, now))
        return Reply({{"status", "rate_limited"}});
    const std::string op = Field(f, "op");
    if (op == "claim") return Claim(f, now, peer_ip);
    if (op == "recover") return Recover(f, now, peer_ip);
    if (op == "cancel") return Cancel(f, now);
    return Reply({{"status", "bad_request"}});
}

std::string Registry::Claim(const std::map<std::string, std::string>& f, int64_t now,
                            const std::string& peer_ip) {
    std::string nick;
    uint8_t pub[32];
    if (std::string err = Authenticate(f, "claim", "", now, nick, pub); !err.empty()) return err;
    const std::string norm = NormalizeNick(nick);
    auto it = db_.find(norm);

    if (it == db_.end() || (Expired(it->second, now) && std::memcmp(it->second.pub, pub, 32) != 0)) {
        // Squatting guard: few new nicks per caller IP per day.
        if (RateLimited(register_ip_rate_, peer_ip, 86400, kRegistrationsPerIpPerDay, now))
            return Reply({{"status", "rate_limited"}});
        if (it != db_.end())
            std::printf("identity: \"%s\" expired (unused %lld days), now \"%s\"\n",
                        it->second.display.c_str(),
                        static_cast<long long>((now - it->second.last_seen) / 86400), nick.c_str());
        Record r;
        r.display = nick;
        std::memcpy(r.pub, pub, 32);
        r.created = now;
        r.last_seen = now;
        const std::string code = NewRecoveryCode();
        r.recovery_hash = RecoveryHash(norm, code);
        db_[norm] = r;
        Save();
        std::printf("identity: registered \"%s\" key %s\n", nick.c_str(), Fingerprint(pub).c_str());
        return Reply({{"status", "ok"}, {"ticket", IssueTicket(r, now)}, {"recovery", code}});
    }

    Record& r = it->second;
    if (std::memcmp(r.pub, pub, 32) == 0) {
        r.last_seen = now;
        r.display = nick;  // the owner may change case or spacing
        Save();
        std::string reply = Reply({{"status", "ok"}, {"ticket", IssueTicket(r, now)}});
        if (r.pending_until) reply += Reply({{"transfer_pending_until", std::to_string(r.pending_until)}});
        return reply;
    }

    if (r.pending_until && std::memcmp(r.pending_pub, pub, 32) == 0) {
        if (now < r.pending_until)
            return Reply({{"status", "pending"}, {"until", std::to_string(r.pending_until)}});
        // The hold ran out and the owner never cancelled: the nick moves.
        std::printf("identity: \"%s\" moved to key %s (recovery code, hold over)\n", nick.c_str(),
                    Fingerprint(pub).c_str());
        std::memcpy(r.pub, pub, 32);
        std::memset(r.pending_pub, 0, 32);
        r.pending_until = 0;
        r.display = nick;
        r.last_seen = now;
        const std::string code = NewRecoveryCode();
        r.recovery_hash = RecoveryHash(norm, code);
        Save();
        return Reply({{"status", "ok"}, {"ticket", IssueTicket(r, now)}, {"recovery", code},
                      {"transferred", "1"}});
    }

    return Reply({{"status", "taken"}, {"owner_nick", r.display}});
}

std::string Registry::Recover(const std::map<std::string, std::string>& f, int64_t now,
                              const std::string& peer_ip) {
    const std::string code = Field(f, "code");
    std::string nick;
    uint8_t pub[32];
    if (std::string err = Authenticate(f, "recover", CanonicalCode(code), now, nick, pub); !err.empty())
        return err;
    const std::string norm = NormalizeNick(nick);
    if (RateLimited(recover_ip_rate_, peer_ip, 3600, kRecoverPerIpPerHour, now) ||
        RateLimited(recover_nick_rate_, norm, 3600, kRecoverPerNickPerHour, now))
        return Reply({{"status", "rate_limited"}});
    auto it = db_.find(norm);
    if (it == db_.end()) return Reply({{"status", "unknown_nick"}});
    Record& r = it->second;
    if (std::memcmp(r.pub, pub, 32) == 0) return Reply({{"status", "already_yours"}});
    if (r.recovery_hash != RecoveryHash(norm, code)) {
        std::printf("identity: wrong recovery code for \"%s\" from %s\n", r.display.c_str(),
                    peer_ip.c_str());
        return Reply({{"status", "bad_code"}});
    }
    std::memcpy(r.pending_pub, pub, 32);
    r.pending_until = now + config_.transfer_hold_sec;
    Save();
    std::printf("identity: transfer of \"%s\" to key %s pending until %lld\n", r.display.c_str(),
                Fingerprint(pub).c_str(), static_cast<long long>(r.pending_until));
    return Reply({{"status", "pending"}, {"until", std::to_string(r.pending_until)}});
}

std::string Registry::Cancel(const std::map<std::string, std::string>& f, int64_t now) {
    std::string nick;
    uint8_t pub[32];
    if (std::string err = Authenticate(f, "cancel", "", now, nick, pub); !err.empty()) return err;
    auto it = db_.find(NormalizeNick(nick));
    if (it == db_.end() || std::memcmp(it->second.pub, pub, 32) != 0)
        return Reply({{"status", "not_owner"}});
    Record& r = it->second;
    if (!r.pending_until) return Reply({{"status", "nothing_pending"}});
    // Whoever asked had the code, so it is no secret any more: a new one.
    std::memset(r.pending_pub, 0, 32);
    r.pending_until = 0;
    r.last_seen = now;
    const std::string code = NewRecoveryCode();
    r.recovery_hash = RecoveryHash(it->first, code);
    Save();
    std::printf("identity: owner of \"%s\" cancelled a transfer\n", r.display.c_str());
    return Reply({{"status", "ok"}, {"recovery", code}});
}

}  // namespace larecomp::identity
