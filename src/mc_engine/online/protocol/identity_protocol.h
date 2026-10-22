#pragma once
//
// Nick ownership: who may play under a gamertag on a relay that checks.
//
// Shared by three programs, so there is one copy of it:
//   - the master (master_server/identity_registry.cpp) keeps nick -> public
//     key, first come first served, and signs short-lived tickets;
//   - the relay (relay_server/relay_server.cpp) admits a gamertag only from an
//     IP that proved, with a fresh nonce, that it holds the ticket's key;
//   - the game (identity/*.cpp) holds the key and does both.
//
// Keys are EdDSA (Monocypher: Curve25519 + BLAKE2b). A relay operator pins the
// master's public key in server.toml; the relay never talks to the master.
//
// Relay side (UDP, the relay's own port, browser header "MC02" v5, types the
// SDK's online client never sends):
//   game  -> relay  kHello       (as large as the challenge: no amplification)
//   relay -> game   kChallenge   nonce for this source IP + the master to use
//   game  -> relay  kProof       ticket + signature over (nonce, ticket)
//   relay -> game   kProofAck    status; the IP+nick is admitted for a while
// The game's register comes from the SDK's socket, not ours, so the relay ties
// the two by source IP and nick, like the server browser's target hint.
//
// Master side (HTTP POST kMasterPath on the master's port, text "key=value"
// lines both ways): ops claim, recover, cancel. Every request is signed by the
// player's key and carries a wall-clock time the master checks.
//

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <random>
#include <string>

#include "../../../../thirdparty/monocypher/monocypher.h"
#include "browser_protocol.h"

namespace larecomp::identity {

constexpr uint8_t kTicketVersion = 1;
constexpr size_t kNickMax = 15;                 // Xbox gamertag limit
constexpr int64_t kTicketLifetimeSec = 24 * 3600;
constexpr int64_t kClockSkewSec = 600;          // request time vs master clock
constexpr int64_t kAdmitSec = 10 * 60;          // relay keeps an IP+nick admitted this long
constexpr int64_t kNonceEpochSec = 300;         // a challenge nonce is good for 1-2 epochs
constexpr const char* kMasterPath = "/identity/v1";
constexpr const char* kMasterKeyPath = "/identity/v1/pubkey";

enum Type : uint8_t {
    kHello = 29,
    kChallenge = 30,
    kProof = 31,
    kProofAck = 32,
};

enum ProofStatus : uint8_t {
    kProofOk = 0,
    kProofBadNonce = 1,      // stale or for another IP: ask again
    kProofBadTicket = 2,     // not signed by the master this relay trusts
    kProofExpired = 3,
    kProofBadSignature = 4,  // the ticket's key did not sign this
    kProofNotEnforced = 5,   // this relay checks no nicks
    kProofWrongRelay = 6,    // signed for another relay name (or the operator left
                             // public_address empty behind a tunnel)
};

// DetailsMsg::flags bit: the relay only admits verified nicks.
constexpr uint8_t kRelayVerifiedNicks = 0x02;

#pragma pack(push, 1)

struct TicketBody {
    uint8_t version;
    uint8_t nick_len;
    char nick[16];       // as the player shows it, zero padded
    uint8_t pubkey[32];  // the player's
    uint64_t expires;    // unix seconds, little endian
};

struct Ticket {
    TicketBody body;
    uint8_t sig[64];  // master's, over TicketMessage(body)
};

struct ChallengeMsg {
    browser::Header hdr;
    uint32_t token;  // echoed from the hello
    uint8_t enforced;
    uint8_t nonce[16];
    uint8_t master_pubkey[32];  // the key tickets must be signed with
    uint16_t master_port;
    char master_host[64];  // where to get one; empty = the relay's own host
};

struct HelloMsg {
    browser::Header hdr;
    uint32_t token;
    uint8_t pad[sizeof(ChallengeMsg) - sizeof(browser::Header) - 4];
};

struct ProofMsg {
    browser::Header hdr;
    uint32_t token;
    uint8_t nonce[16];
    // The relay as the player addressed it ("host:port", lower case). Signed,
    // so a relay that forwards another relay's challenge to its own players
    // gets a proof that relay rejects: it is not that relay's name.
    char relay[64];
    Ticket ticket;
    uint8_t sig[64];  // player's, over ProofMessage(nonce, relay, ticket)
};

struct ProofAckMsg {
    browser::Header hdr;
    uint32_t token;
    uint8_t status;      // ProofStatus
    uint32_t admit_sec;  // how long the relay admits this IP+nick
};

#pragma pack(pop)

static_assert(sizeof(HelloMsg) == sizeof(ChallengeMsg), "hello must not amplify");
static_assert(sizeof(ProofAckMsg) <= sizeof(ProofMsg), "ack must not amplify");

// ------------------------------------------------------------------ text ---

inline std::string ToHex(const uint8_t* p, size_t n) {
    static const char* kHex = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s += kHex[p[i] >> 4];
        s += kHex[p[i] & 15];
    }
    return s;
}

inline bool FromHex(const std::string& s, uint8_t* out, size_t n) {
    if (s.size() != n * 2) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned v = 0;
        for (int k = 0; k < 2; ++k) {
            const char c = s[i * 2 + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else return false;
        }
        out[i] = uint8_t(v);
    }
    return true;
}

// A nick the master accepts: 1..15 bytes, no control characters, and not
// only separators.
inline bool ValidNick(const std::string& nick);

// The form nicks are compared in, so "Neo", "NEO", "Ne0" and "N e o" are one
// nick: ASCII case folded, look-alike digits mapped to letters, separators
// dropped. Other bytes (UTF-8) are kept as they are.
inline std::string NormalizeNick(const std::string& nick) {
    std::string out;
    for (char ch : nick) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
        switch (c) {
            case ' ': case '_': case '-': case '.': continue;
            case '0': c = 'o'; break;
            case '1': case 'i': case '|': case '!': c = 'l'; break;
            case '5': case '$': c = 's'; break;
            case '3': c = 'e'; break;
            case '4': case '@': c = 'a'; break;
            case '7': c = 't'; break;
            default: break;
        }
        out += static_cast<char>(c);
    }
    return out;
}

inline bool ValidNick(const std::string& nick) {
    if (nick.empty() || nick.size() > kNickMax) return false;
    for (char ch : nick) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F || c == '\t') return false;
    }
    return !NormalizeNick(nick).empty();
}

// Gamertag field off the wire (maybe unterminated).
inline std::string NickFromField(const char* field, size_t size) {
    size_t n = 0;
    while (n < size && field[n]) ++n;
    return std::string(field, n);
}

// ---------------------------------------------------------------- crypto ---

// std::random_device is the OS generator on every toolchain this builds with
// (rand_s on Windows, getrandom / urandom elsewhere).
inline void RandomBytes(uint8_t* out, size_t n) {
    std::random_device rd;
    for (size_t i = 0; i < n; i += 4) {
        const uint32_t v = rd();
        for (size_t k = 0; k < 4 && i + k < n; ++k) out[i + k] = uint8_t(v >> (8 * k));
    }
}

struct KeyPair {
    uint8_t secret[64];  // Monocypher's expanded secret (seed + public)
    uint8_t pub[32];
};

// Same seed, same keys. The seed is the only thing a player needs to keep.
inline KeyPair KeyPairFromSeed(const uint8_t (&seed)[32]) {
    KeyPair kp;
    uint8_t copy[32];
    std::memcpy(copy, seed, 32);
    crypto_eddsa_key_pair(kp.secret, kp.pub, copy);  // wipes copy
    return kp;
}

inline std::string Fingerprint(const uint8_t (&pub)[32]) {
    uint8_t h[5];
    crypto_blake2b(h, sizeof(h), pub, 32);
    return ToHex(h, sizeof(h));
}

inline std::string TicketMessage(const TicketBody& body) {
    std::string m = "LRID-ticket-v1";
    m.append(reinterpret_cast<const char*>(&body), sizeof(body));
    return m;
}

inline std::string ProofMessage(const uint8_t (&nonce)[16], const char (&relay)[64],
                                const Ticket& ticket) {
    std::string m = "LRID-relay-v1";
    m.append(reinterpret_cast<const char*>(nonce), 16);
    m.append(relay, 64);
    m.append(reinterpret_cast<const char*>(&ticket), sizeof(ticket));
    return m;
}

// "Host:Port" as both sides compare it: lower case, no spaces.
inline std::string CanonicalRelayName(const std::string& host, uint16_t port) {
    std::string out;
    for (char c : host) {
        if (c == ' ') continue;
        out += (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
    }
    return out + ":" + std::to_string(port);
}

// What a player signs for the master. `salt` is random per request (EdDSA
// signs deterministically: without it two identical requests in the same
// second would look like a replay). `extra` is op specific (empty for most).
inline std::string RequestMessage(const std::string& op, const std::string& nick,
                                  const std::string& pubkey_hex, int64_t time,
                                  const std::string& salt, const std::string& extra) {
    return "LRID-master-v1|" + op + "|" + nick + "|" + pubkey_hex + "|" + std::to_string(time) +
           "|" + salt + "|" + extra;
}

inline std::string NewSalt() {
    uint8_t r[8];
    RandomBytes(r, sizeof(r));
    return ToHex(r, sizeof(r));
}

inline std::string TicketNick(const Ticket& t) {
    const size_t n = t.body.nick_len <= kNickMax ? t.body.nick_len : kNickMax;
    return std::string(t.body.nick, n);
}

inline bool CheckTicket(const uint8_t (&master_pub)[32], const Ticket& t, int64_t now_unix) {
    if (t.body.version != kTicketVersion || t.body.nick_len == 0 || t.body.nick_len > kNickMax)
        return false;
    const std::string m = TicketMessage(t.body);
    if (crypto_eddsa_check(t.sig, master_pub, reinterpret_cast<const uint8_t*>(m.data()), m.size()) != 0)
        return false;
    return int64_t(t.body.expires) > now_unix;
}

inline bool CheckSignature(const uint8_t (&sig)[64], const uint8_t (&pub)[32], const std::string& m) {
    return crypto_eddsa_check(sig, pub, reinterpret_cast<const uint8_t*>(m.data()), m.size()) == 0;
}

inline void Sign(uint8_t (&sig)[64], const KeyPair& kp, const std::string& m) {
    crypto_eddsa_sign(sig, kp.secret, reinterpret_cast<const uint8_t*>(m.data()), m.size());
}

// ------------------------------------------------------- recovery codes ---

// 20 characters of Crockford base32 (100 bits), shown as XXXXX-XXXXX-XXXXX-XXXXX.
inline std::string NewRecoveryCode() {
    static const char* kAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    uint8_t r[20];
    RandomBytes(r, sizeof(r));
    std::string code;
    for (int i = 0; i < 20; ++i) {
        if (i && i % 5 == 0) code += '-';
        code += kAlphabet[r[i] & 31];
    }
    crypto_wipe(r, sizeof(r));
    return code;
}

// Upper case, dashes and spaces dropped, look-alikes mapped (O->0, I/L->1),
// so a code read back from paper still matches.
inline std::string CanonicalCode(const std::string& code) {
    std::string out;
    for (char ch : code) {
        char c = ch;
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        if (c == '-' || c == ' ') continue;
        if (c == 'O') c = '0';
        if (c == 'I' || c == 'L') c = '1';
        out += c;
    }
    return out;
}

// What the master stores: the code keyed by the nick it recovers.
inline std::string RecoveryHash(const std::string& normalized_nick, const std::string& code) {
    const std::string c = CanonicalCode(code);
    uint8_t h[32];
    crypto_blake2b_keyed(h, sizeof(h), reinterpret_cast<const uint8_t*>(normalized_nick.data()),
                         normalized_nick.size() > 64 ? 64 : normalized_nick.size(),
                         reinterpret_cast<const uint8_t*>(c.data()), c.size());
    return ToHex(h, sizeof(h));
}

// ------------------------------------------------------------------ xuid ---

// The guest XUID a key plays under: the high 16 bits every recomp profile has
// had (0xB13E), over 48 bits of the key's hash, so the same key is the same
// XUID on any machine and nobody else's key gives it.
inline uint64_t XuidFromPublicKey(const uint8_t (&pub)[32]) {
    uint8_t h[8];
    crypto_blake2b(h, sizeof(h), pub, 32);
    uint64_t low = 0;
    for (int i = 0; i < 6; ++i) low |= uint64_t(h[i]) << (8 * i);
    uint64_t xuid = 0xB13E000000000000ull | low;
    if (xuid == 0xB13EBABEBABEBABEull) xuid ^= 1;  // never the legacy shared XUID
    return xuid;
}

// ----------------------------------------------------------- key=value ----

// One "key=value" per line; values never hold a newline.
template <typename Map>
inline void ParseFields(const std::string& text, Map& out) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq != std::string::npos) out[line.substr(0, eq)] = line.substr(eq + 1);
        pos = end + 1;
    }
}

inline int64_t NowUnix() { return int64_t(std::time(nullptr)); }

}  // namespace larecomp::identity
