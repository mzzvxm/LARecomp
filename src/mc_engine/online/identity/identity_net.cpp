#include "identity_net.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "../rate_my_ride/rmr_net.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define ID_CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define INVALID_SOCKET (-1)
#define ID_CLOSESOCK ::close
#endif

namespace larecomp::identity::net {

namespace {

using Clock = std::chrono::steady_clock;

void EnsureSockets() {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);  // counted: harmless next to the SDK's own
    });
#endif
}

bool Resolve(const std::string& host, uint16_t port, sockaddr_in& out) {
    EnsureSockets();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    out = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    out.sin_port = htons(port);
    freeaddrinfo(res);
    return true;
}

// Sends `q` until an answer of `type` carrying `token` comes back.
int Exchange(socket_t s, const Options& opt, const void* q, int qlen, void* a, int alen, uint8_t type,
             uint32_t token) {
    for (int attempt = 0; attempt < opt.udp_tries; ++attempt) {
        send(s, static_cast<const char*>(q), qlen, 0);
        const auto deadline = Clock::now() + std::chrono::milliseconds(opt.udp_timeout_ms);
        while (Clock::now() < deadline) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(s, &set);
            const auto left =
                std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now()).count();
            timeval tv{long(left / 1000000), long(left % 1000000)};
            if (select(int(s) + 1, &set, nullptr, nullptr, &tv) <= 0) break;
            const int n = recv(s, static_cast<char*>(a), alen, 0);
            if (n < int(sizeof(browser::Header) + 4)) continue;
            const auto* h = static_cast<const browser::Header*>(a);
            uint32_t tok;
            std::memcpy(&tok, static_cast<const uint8_t*>(a) + sizeof(browser::Header), 4);
            if (h->magic == browser::kMagic && h->version == browser::kVersion && h->type == type &&
                tok == token)
                return n;
        }
    }
    return -1;
}

// A UDP socket connected to the relay.
struct Relay {
    socket_t sock = INVALID_SOCKET;
    Challenge::State state = Challenge::State::kNoAnswer;  // why it is not open

    Relay(const std::string& host, uint16_t port) {
        sockaddr_in addr{};
        if (!Resolve(host, port, addr)) {
            state = Challenge::State::kUnresolved;
            return;
        }
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET || connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            if (sock != INVALID_SOCKET) ID_CLOSESOCK(sock);
            sock = INVALID_SOCKET;
            state = Challenge::State::kNoSocket;
            return;
        }
        state = Challenge::State::kOk;
    }
    ~Relay() {
        if (sock != INVALID_SOCKET) ID_CLOSESOCK(sock);
    }
    Relay(const Relay&) = delete;
    Relay& operator=(const Relay&) = delete;
    bool open() const { return sock != INVALID_SOCKET; }
};

// Hello (as large as the challenge) -> challenge. False when the relay does not answer.
bool Hello(Relay& relay, const Options& opt, ChallengeMsg& ch) {
    uint8_t rnd[4];
    RandomBytes(rnd, 4);
    uint32_t token;
    std::memcpy(&token, rnd, 4);
    HelloMsg hello{};
    hello.hdr = {browser::kMagic, browser::kVersion, kHello};
    hello.token = token;
    ch = ChallengeMsg{};
    return Exchange(relay.sock, opt, &hello, sizeof(hello), &ch, sizeof(ch), kChallenge, token) == int(sizeof(ch));
}

}  // namespace

Fields MasterCall(const std::string& host, uint16_t port, const std::string& op, const std::string& nick,
                  const KeyPair& keys, const std::string& code, const Options& opt) {
    const std::string pub = ToHex(keys.pub, 32);
    const int64_t now = NowUnix() + opt.clock_offset_sec;
    const std::string salt = NewSalt();
    uint8_t sig[64];
    Sign(sig, keys, RequestMessage(op, nick, pub, now, salt, op == "recover" ? CanonicalCode(code) : ""));
    std::string body = "op=" + op + "\nnick=" + nick + "\npubkey=" + pub + "\ntime=" + std::to_string(now) +
                       "\nsalt=" + salt + "\nsig=" + ToHex(sig, 64) + "\n";
    if (!code.empty()) body += "code=" + code + "\n";
    const auto r = rmr::net::Post(host, port, kMasterPath, std::vector<uint8_t>(body.begin(), body.end()),
                                  opt.master_connect_ms, opt.master_io_ms);
    crypto_wipe(body.data(), body.size());
    Fields f;
    if (!r.transport_ok || r.status != 200) {
        f["status"] = "down";
        f["error"] = r.transport_ok ? "HTTP " + std::to_string(r.status) : r.error;
        return f;
    }
    ParseFields(std::string(r.body.begin(), r.body.end()), f);
    return f;
}

std::string ClockText(const Fields& f) {
    const int64_t server = std::atoll(f.count("server_time") ? f.at("server_time").c_str() : "0");
    if (!server) return "this PC's clock is wrong";
    const int64_t off = (NowUnix() - server) / 60;
    return "this PC's clock is " + std::to_string(off < 0 ? -off : off) + " min " + (off < 0 ? "behind" : "ahead");
}

Challenge AskChallenge(const std::string& host, uint16_t port, const Options& opt) {
    Challenge c;
    Relay relay(host, port);
    if (!relay.open()) {
        c.state = relay.state;
        return c;
    }
    ChallengeMsg ch;
    if (!Hello(relay, opt, ch)) {
        c.state = Challenge::State::kNoAnswer;
        return c;
    }
    c.state = Challenge::State::kOk;
    c.enforced = ch.enforced != 0;
    c.master_host = NickFromField(ch.master_host, sizeof(ch.master_host));
    if (c.master_host.empty()) c.master_host = host;
    c.master_port = ch.master_port ? ch.master_port : browser::kMasterPort;
    std::memcpy(c.master_key, ch.master_pubkey, 32);
    return c;
}

std::chrono::seconds Cycle(Status& s, const CycleInput& in, TicketHooks& hooks, CycleOutput* out) {
    const std::string& host = in.host;
    const KeyPair& keys = *in.keys;
    const Options& opt = in.options;
    s.relay = CanonicalRelayName(host, in.port);
    Relay relay(host, in.port);
    if (!relay.open()) {
        if (relay.state == Challenge::State::kUnresolved) {
            s.check = Check::kNoAnswer;
            s.detail = "cannot resolve " + host;
        } else {
            s.check = Check::kFailed;
            s.detail = "no UDP socket";
        }
        return std::chrono::seconds(30);
    }

    uint8_t rnd[4];
    ChallengeMsg ch;
    if (!Hello(relay, opt, ch)) {
        s.check = Check::kNoAnswer;
        s.detail = "the server does not answer nick checks (older relay, or offline)";
        return std::chrono::seconds(60);
    }
    if (!ch.enforced) {
        s.check = Check::kNotChecked;
        s.detail = "this server does not check nicks";
        return std::chrono::seconds(300);
    }

    std::string mhost = NickFromField(ch.master_host, sizeof(ch.master_host));
    if (mhost.empty()) mhost = host;
    const uint16_t mport = ch.master_port ? ch.master_port : browser::kMasterPort;
    s.master = mhost + ":" + std::to_string(mport);
    if (out) {
        out->master_known = true;
        out->master_host = mhost;
        out->master_port = mport;
        std::memcpy(out->master_key, ch.master_pubkey, 32);
    }

    const std::string& nick = in.nick;
    Ticket ticket{};
    const bool have = hooks.Cached(ch.master_pubkey, ticket);
    const bool known = have && TicketNick(ticket) == nick;
    bool cached = known && CheckTicket(ch.master_pubkey, ticket, NowUnix() + 3600);
    // A renewal asks the master even while the ticket in hand is still good: that is how a transfer someone started with
    // our recovery code shows up (transfer_pending_until in the reply).
    if (in.claim == ClaimMode::kRefresh && known) cached = false;
    if (!cached) {
        if (in.claim == ClaimMode::kNever || (in.claim == ClaimMode::kRefresh && !known)) {
            s.check = Check::kFailed;
            s.needs_claim = true;
            s.detail = "this server's nick registry has no ticket of yours for this nick yet; claim it first";
            return std::chrono::seconds(600);
        }
        const Fields f = MasterCall(mhost, mport, "claim", nick, keys, "", opt);
        const std::string status = f.count("status") ? f.at("status") : "";
        s.master_status = status;
        if (status == "down") {
            s.check = Check::kMasterDown;
            s.detail = "nick server " + s.master + " unreachable: " + (f.count("error") ? f.at("error") : "");
            return std::chrono::seconds(60);
        }
        if (status == "taken") {
            s.check = Check::kTaken;
            s.owner_nick = f.count("owner_nick") ? f.at("owner_nick") : nick;
            s.detail = "\"" + s.owner_nick + "\" belongs to someone else on this server";
            return std::chrono::seconds(600);
        }
        if (status == "pending") {
            s.check = Check::kClaimPending;
            s.claim_until = std::atoll(f.count("until") ? f.at("until").c_str() : "0");
            s.detail = "your recovery code is moving the nick to you";
            return std::chrono::seconds(600);
        }
        if (status == "bad_clock") {
            s.check = Check::kFailed;
            s.detail = ClockText(f) + "; fix it in Windows and press Check again";
            return std::chrono::seconds(600);
        }
        if (status != "ok" || !f.count("ticket") ||
            !FromHex(f.at("ticket"), reinterpret_cast<uint8_t*>(&ticket), sizeof(ticket)) ||
            !CheckTicket(ch.master_pubkey, ticket, NowUnix())) {
            s.check = Check::kFailed;
            s.detail = "nick server answered \"" + status + "\"";
            return std::chrono::seconds(120);
        }
        hooks.Put(ch.master_pubkey, ticket);
        if (f.count("recovery")) hooks.SetRecoveryCode(f.at("recovery"));
        s.incoming_until = std::atoll(f.count("transfer_pending_until") ? f.at("transfer_pending_until").c_str() : "0");
        if (f.count("transferred")) hooks.Note("\"" + nick + "\" is ours now (recovery code)");
    }

    ProofMsg proof{};
    proof.hdr = {browser::kMagic, browser::kVersion, kProof};
    RandomBytes(rnd, 4);
    std::memcpy(&proof.token, rnd, 4);
    std::memcpy(proof.nonce, ch.nonce, 16);
    std::snprintf(proof.relay, sizeof(proof.relay), "%s", s.relay.c_str());
    proof.ticket = ticket;
    Sign(proof.sig, keys, ProofMessage(proof.nonce, proof.relay, proof.ticket));
    ProofAckMsg ack{};
    if (Exchange(relay.sock, opt, &proof, sizeof(proof), &ack, sizeof(ack), kProofAck, proof.token) !=
        int(sizeof(ack))) {
        s.check = Check::kNoAnswer;
        s.detail = "the server did not answer the proof";
        return std::chrono::seconds(30);
    }
    switch (ack.status) {
        case kProofOk:
            s.check = Check::kVerified;
            s.detail = "nick verified on this server";
            if (s.incoming_until) s.detail += " - but someone asked for it with your recovery code";
            return std::chrono::seconds(ack.admit_sec > 60 ? ack.admit_sec / 2 : 30);
        case kProofWrongRelay:
            s.check = Check::kFailed;
            s.detail = "the server does not know itself as \"" + s.relay +
                       "\": its owner must add that to public_address or identity_names";
            return std::chrono::seconds(300);
        case kProofExpired:
        case kProofBadTicket:
            s.check = Check::kFailed;
            s.detail = "the server rejected the nick ticket; asking for a new one";
            if (in.claim != ClaimMode::kNever) hooks.Put(ch.master_pubkey, Ticket{});
            return std::chrono::seconds(20);
        default:
            s.check = Check::kFailed;
            s.detail = "proof refused (status " + std::to_string(ack.status) + ")";
            return std::chrono::seconds(30);
    }
}

}  // namespace larecomp::identity::net
