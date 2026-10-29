#include "identity_client.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include <rex/cvar.h>

#include "larecomp_log.h"
#include "../rate_my_ride/rmr_net.h"
#include "../relay_info.h"
#include "../protocol/identity_protocol.h"
#include "identity_store.h"

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

namespace larecomp::identity {

namespace {

using Clock = std::chrono::steady_clock;
using Fields = std::map<std::string, std::string>;

constexpr int kUdpTimeoutMs = 1000;
constexpr int kUdpTries = 3;
constexpr int kMasterConnectMs = 4000;
constexpr int kMasterIoMs = 8000;

std::mutex g_mtx;
std::condition_variable g_cv;
Status g_status;
bool g_started = false;
bool g_kick = false;
std::string g_recover_code;
bool g_cancel = false;
// What the last cycle talked to, for recover/cancel outside a cycle.
std::string g_master_host;
uint16_t g_master_port = 0;
uint8_t g_master_key[32] = {};

void Publish(const Status& s) {
    std::lock_guard<std::mutex> lk(g_mtx);
    const uint64_t gen = g_status.generation + 1;
    g_status = s;
    g_status.generation = gen;
}

bool OnlineOn() { return rex::cvar::GetFlagByName("mcla_online_enable") == "true"; }

bool Resolve(const std::string& host, uint16_t port, sockaddr_in& out) {
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
int Exchange(socket_t s, const void* q, int qlen, void* a, int alen, uint8_t type, uint32_t token) {
    for (int attempt = 0; attempt < kUdpTries; ++attempt) {
        send(s, static_cast<const char*>(q), qlen, 0);
        const auto deadline = Clock::now() + std::chrono::milliseconds(kUdpTimeoutMs);
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

Fields MasterCall(const std::string& host, uint16_t port, const std::string& op, const std::string& code) {
    const KeyPair keys = Keys();
    const std::string nick = Nick();
    const std::string pub = ToHex(keys.pub, 32);
    const int64_t now = NowUnix();
    const std::string salt = NewSalt();
    uint8_t sig[64];
    Sign(sig, keys, RequestMessage(op, nick, pub, now, salt, op == "recover" ? CanonicalCode(code) : ""));
    std::string body = "op=" + op + "\nnick=" + nick + "\npubkey=" + pub + "\ntime=" + std::to_string(now) +
                       "\nsalt=" + salt + "\nsig=" + ToHex(sig, 64) + "\n";
    if (!code.empty()) body += "code=" + code + "\n";
    const auto r = rmr::net::Post(host, port, kMasterPath, std::vector<uint8_t>(body.begin(), body.end()),
                                  kMasterConnectMs, kMasterIoMs);
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

// One pass: challenge, ticket, proof. Returns how long until the next pass.
std::chrono::seconds Cycle(Status& s) {
    std::string host;
    uint16_t port = 0;
    relay_info::ConfiguredServer(host, port);
    s.relay = CanonicalRelayName(host, port);
    sockaddr_in addr{};
    if (!Resolve(host, port, addr)) {
        s.check = Check::kNoAnswer;
        s.detail = "cannot resolve " + host;
        return std::chrono::seconds(30);
    }
    socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET || connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (sock != INVALID_SOCKET) ID_CLOSESOCK(sock);
        s.check = Check::kFailed;
        s.detail = "no UDP socket";
        return std::chrono::seconds(30);
    }
    struct Closer {
        socket_t s;
        ~Closer() { ID_CLOSESOCK(s); }
    } closer{sock};

    uint8_t rnd[4];
    RandomBytes(rnd, 4);
    uint32_t token;
    std::memcpy(&token, rnd, 4);
    HelloMsg hello{};
    hello.hdr = {browser::kMagic, browser::kVersion, kHello};
    hello.token = token;
    ChallengeMsg ch{};
    if (Exchange(sock, &hello, sizeof(hello), &ch, sizeof(ch), kChallenge, token) != int(sizeof(ch))) {
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
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_master_host = mhost;
        g_master_port = mport;
        std::memcpy(g_master_key, ch.master_pubkey, 32);
    }

    const std::string nick = Nick();
    Ticket ticket{};
    const bool cached = CachedTicket(ch.master_pubkey, ticket) &&
                        CheckTicket(ch.master_pubkey, ticket, NowUnix() + 3600) &&
                        TicketNick(ticket) == nick;
    if (!cached) {
        const Fields f = MasterCall(mhost, mport, "claim", "");
        const std::string status = f.count("status") ? f.at("status") : "";
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
        StoreTicket(ch.master_pubkey, ticket);
        if (f.count("recovery")) SetPendingRecoveryCode(f.at("recovery"));
        s.incoming_until = std::atoll(f.count("transfer_pending_until") ? f.at("transfer_pending_until").c_str() : "0");
        if (f.count("transferred"))
            LARECOMP_APP_INFO("[identity] \"{}\" is ours now (recovery code)", nick);
    }

    ProofMsg proof{};
    proof.hdr = {browser::kMagic, browser::kVersion, kProof};
    RandomBytes(rnd, 4);
    std::memcpy(&proof.token, rnd, 4);
    std::memcpy(proof.nonce, ch.nonce, 16);
    std::snprintf(proof.relay, sizeof(proof.relay), "%s", s.relay.c_str());
    proof.ticket = ticket;
    Sign(proof.sig, Keys(), ProofMessage(proof.nonce, proof.relay, proof.ticket));
    ProofAckMsg ack{};
    if (Exchange(sock, &proof, sizeof(proof), &ack, sizeof(ack), kProofAck, proof.token) != int(sizeof(ack))) {
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
            StoreTicket(ch.master_pubkey, Ticket{});
            return std::chrono::seconds(20);
        default:
            s.check = Check::kFailed;
            s.detail = "proof refused (status " + std::to_string(ack.status) + ")";
            return std::chrono::seconds(30);
    }
}

void HandleRequests(Status& s) {
    std::string code;
    bool cancel;
    std::string mhost;
    uint16_t mport;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        code.swap(g_recover_code);
        cancel = g_cancel;
        g_cancel = false;
        mhost = g_master_host;
        mport = g_master_port;
    }
    if (mhost.empty() || (code.empty() && !cancel)) return;
    if (!code.empty()) {
        const Fields f = MasterCall(mhost, mport, "recover", code);
        const std::string st = f.count("status") ? f.at("status") : "";
        if (st == "pending") {
            s.check = Check::kClaimPending;
            s.claim_until = std::atoll(f.at("until").c_str());
            s.detail = "recovery code accepted";
        } else if (st == "bad_code") {
            s.detail = "that recovery code is wrong";
        } else if (st == "rate_limited") {
            s.detail = "too many tries; wait an hour";
        } else if (st == "already_yours") {
            s.detail = "this nick is already yours";
        } else {
            s.detail = "recovery failed: " + (st.empty() ? std::string("no answer") : st);
        }
        LARECOMP_APP_INFO("[identity] recover \"{}\": {}", Nick(), st);
    }
    if (cancel) {
        const Fields f = MasterCall(mhost, mport, "cancel", "");
        const std::string st = f.count("status") ? f.at("status") : "";
        if (st == "ok") {
            s.incoming_until = 0;
            if (f.count("recovery")) SetPendingRecoveryCode(f.at("recovery"));
            s.detail = "transfer cancelled; you have a new recovery code";
        } else {
            s.detail = "cancel failed: " + (st.empty() ? std::string("no answer") : st);
        }
        LARECOMP_APP_INFO("[identity] cancel transfer of \"{}\": {}", Nick(), st);
    }
}

void Worker() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);  // counted: harmless next to the SDK's own
#endif
    std::string last_server;
    bool last_online = false;
    auto next = Clock::now();
    // Whether the last pass was online with the identity loaded. A recover or
    // cancel request only wakes the loop then: offline it stays queued until
    // the game goes online, instead of waking the loop on every pass.
    bool serving = false;
    Status s;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cv.wait_until(lk, (std::min)(next, Clock::now() + std::chrono::seconds(2)),
                            [serving] {
                                return g_kick ||
                                       (serving && (!g_recover_code.empty() || g_cancel));
                            });
        }
        const bool online = OnlineOn();
        bool kick;
        bool requests;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            kick = g_kick;
            g_kick = false;
            requests = !g_recover_code.empty() || g_cancel;
        }
        if (!online || !Loaded()) {
            if (last_online || s.check != Check::kOff) {
                s = Status{};
                Publish(s);
            }
            last_online = false;
            serving = false;
            // Offline the next look is 2 s away; `next` left in the past made
            // the wait return at once and the loop spin a core for as long as
            // the game stayed offline.
            next = Clock::now() + std::chrono::seconds(2);
            continue;
        }
        serving = true;
        const std::string server = rex::cvar::GetFlagByName("mcla_online_server") + "|" +
                                   rex::cvar::GetFlagByName("mcla_online_port");
        if (requests) {
            HandleRequests(s);
            Publish(s);
            kick = true;
        }
        if (!kick && online == last_online && server == last_server && Clock::now() < next) continue;
        last_online = online;
        last_server = server;
        Status fresh;
        fresh.incoming_until = s.incoming_until;
        fresh.check = Check::kWorking;
        fresh.detail = "checking the nick with the server...";
        fresh.relay = s.relay;
        Publish(fresh);
        next = Clock::now() + Cycle(fresh);
        const bool changed = fresh.check != s.check || fresh.detail != s.detail;
        s = fresh;
        Publish(s);
        if (changed)
            LARECOMP_APP_INFO("[identity] {} ({})", s.detail, s.relay);
    }
}

}  // namespace

void StartClient() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_started) return;
    g_started = true;
    std::thread(Worker).detach();
}

Status GetStatus() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_status;
}

void CheckNow() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_kick = true;
    g_cv.notify_all();
}

void RequestRecover(const std::string& code) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_recover_code = code;
    g_cv.notify_all();
}

void RequestCancelTransfer() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_cancel = true;
    g_cv.notify_all();
}

}  // namespace larecomp::identity
