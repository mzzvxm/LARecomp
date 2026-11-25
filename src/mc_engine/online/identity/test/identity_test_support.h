#pragma once
//
// Fakes for the identity checks (online_tool/identity_commands_test.cpp, identity/test/identity_adapter_test.cpp): a
// nick master and a relay that checks nicks, both on 127.0.0.1 only (a listener on every interface would make Windows
// Firewall ask). The master is the real registry (master_server/identity_registry.cpp) behind a small loopback HTTP
// listener, so what a player's request meets is the code the real master runs; the relay verifies a proof the way
// larecomp_relay_server does. No real server, no real nick: nothing here leaves the machine.
//

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../../master_server/identity_registry.h"
#include "../../protocol/identity_protocol.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace idtest {

namespace fs = std::filesystem;
namespace idn = larecomp::identity;

#ifdef _WIN32
using Sock = SOCKET;
constexpr Sock kBadSock = INVALID_SOCKET;
inline void CloseSock(Sock s) { closesocket(s); }
#else
using Sock = int;
constexpr Sock kBadSock = -1;
inline void CloseSock(Sock s) { close(s); }
#endif

inline void StartSockets() {
#ifdef _WIN32
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
#endif
}

inline sockaddr_in Loopback(uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    return a;
}

inline uint16_t BoundPort(Sock s) {
    sockaddr_in a{};
    socklen_t len = sizeof(a);
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    return ntohs(a.sin_port);
}

inline bool Readable(Sock s, int ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{0, ms * 1000};
    return select(int(s) + 1, &set, nullptr, nullptr, &tv) > 0;
}

// A TCP port nobody listens on.
inline uint16_t ClosedTcpPort() {
    Sock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a = Loopback(0);
    bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    const uint16_t p = BoundPort(s);
    CloseSock(s);
    return p;
}

// A scratch folder that goes away with the object.
class Scratch {
public:
    explicit Scratch(const std::string& name) {
        path_ = fs::temp_directory_path() /
                ("lar_identity_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "_" + name);
        fs::create_directories(path_);
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    const fs::path& path() const { return path_; }
    fs::path operator/(const std::string& s) const { return path_ / s; }

private:
    fs::path path_;
};

// The nick master: the real registry behind a loopback HTTP listener.
class FakeMaster {
public:
    FakeMaster() {
        sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a = Loopback(0);
        bind(sock_, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        listen(sock_, 8);
        port_ = BoundPort(sock_);
    }
    ~FakeMaster() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        CloseSock(sock_);
    }
    // `dir` keeps the registry's two files. `hold_sec` is how long a recovered nick waits before it moves.
    bool Start(const fs::path& dir, int64_t hold_sec = 7 * 24 * 3600) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        idn::RegistryConfig cfg;
        cfg.dir = (dir / "").string();  // with a trailing separator
        cfg.transfer_hold_sec = hold_sec;
        std::string err;
        if (!registry_.Open(cfg, &err)) return false;
        thread_ = std::thread([this] { Loop(); });
        return true;
    }
    uint16_t port() const { return port_; }
    const uint8_t (&public_key() const)[32] { return registry_.public_key(); }
    std::string Address() const { return "127.0.0.1:" + std::to_string(port_); }
    int posts() const { return posts_; }
    // The next requests count as coming from this IP (the registry limits per IP).
    void SetPeer(const std::string& ip) {
        std::lock_guard<std::mutex> lk(mtx_);
        peer_ = ip;
    }
    std::string last_body() {
        std::lock_guard<std::mutex> lk(mtx_);
        return last_body_;
    }
    size_t nicks() const { return registry_.size(); }
    // Called (on the master's thread) before each POST is answered: a check can make something happen "while the master
    // thinks", such as the game starting.
    void OnPost(std::function<void()> f) {
        std::lock_guard<std::mutex> lk(mtx_);
        on_post_ = std::move(f);
    }

private:
    void Loop() {
        while (!stop_) {
            if (!Readable(sock_, 20)) continue;
            Sock c = accept(sock_, nullptr, nullptr);
            if (c == kBadSock) continue;
            std::string req;
            char tmp[2048];
            size_t header_end = std::string::npos;
            size_t want = 0;
            while (Readable(c, 500)) {
                const int n = int(recv(c, tmp, sizeof(tmp), 0));
                if (n <= 0) break;
                req.append(tmp, size_t(n));
                if (header_end == std::string::npos) {
                    header_end = req.find("\r\n\r\n");
                    if (header_end != std::string::npos) {
                        header_end += 4;
                        const size_t cl = req.find("Content-Length:");
                        want = header_end + (cl != std::string::npos ? size_t(std::atoll(req.c_str() + cl + 15)) : 0);
                    }
                }
                if (header_end != std::string::npos && req.size() >= want) break;
            }
            std::string body, status = "200 OK";
            const std::string line = req.substr(0, req.find("\r\n"));
            const std::string payload = header_end == std::string::npos ? "" : req.substr(header_end);
            if (line.rfind(std::string("POST ") + idn::kMasterPath + " ", 0) == 0) {
                ++posts_;
                std::string peer;
                std::function<void()> hook;
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    last_body_ = payload;
                    peer = peer_;
                    hook = on_post_;
                }
                if (hook) hook();
                body = registry_.Handle(payload, peer);
            } else if (line.rfind(std::string("GET ") + idn::kMasterKeyPath + " ", 0) == 0) {
                body = idn::ToHex(registry_.public_key(), 32) + "\n";
            } else {
                status = "404 Not Found";
                body = "not found\n";
            }
            const std::string reply = "HTTP/1.1 " + status + "\r\nContent-Type: text/plain\r\nContent-Length: " +
                                      std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            send(c, reply.data(), int(reply.size()), 0);
            CloseSock(c);
        }
    }

    Sock sock_ = kBadSock;
    uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    idn::Registry registry_;
    std::mutex mtx_;
    std::string peer_ = "10.0.0.1";
    std::string last_body_;
    std::function<void()> on_post_;
    std::atomic<int> posts_{0};
};

// A relay that does the nick check like larecomp_relay_server (identity_protocol.h): hello -> challenge, proof -> ack.
class FakeRelay {
public:
    FakeRelay() {
        sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in a = Loopback(0);
        bind(sock_, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        port_ = BoundPort(sock_);
    }
    ~FakeRelay() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        CloseSock(sock_);
    }
    // The master the relay names to its players, and whether it checks nicks at all.
    void Configure(const uint8_t (&master_key)[32], uint16_t master_port, bool enforce) {
        std::lock_guard<std::mutex> lk(mtx_);
        std::memcpy(master_key_, master_key, 32);
        master_port_ = master_port;
        enforce_ = enforce;
    }
    void SetSilent(bool silent) { silent_ = silent; }
    void Start() { thread_ = std::thread([this] { Loop(); }); }
    uint16_t port() const { return port_; }
    std::string Address() const { return "127.0.0.1:" + std::to_string(port_); }
    std::string CanonicalName() const { return idn::CanonicalRelayName("127.0.0.1", port_); }
    int hellos() const { return hellos_; }
    int proofs() const { return proofs_; }
    bool Admitted(const std::string& nick) {
        std::lock_guard<std::mutex> lk(mtx_);
        return admitted_.count(nick) != 0;
    }

private:
    void Loop() {
        while (!stop_) {
            if (!Readable(sock_, 20)) continue;
            uint8_t buf[2048];
            sockaddr_in from{};
            socklen_t flen = sizeof(from);
            const int n = int(recvfrom(sock_, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                       reinterpret_cast<sockaddr*>(&from), &flen));
            if (n < int(sizeof(larecomp::browser::Header) + 4) || silent_) continue;
            larecomp::browser::Header h;
            std::memcpy(&h, buf, sizeof(h));
            if (h.magic != larecomp::browser::kMagic || h.version != larecomp::browser::kVersion) continue;
            std::lock_guard<std::mutex> lk(mtx_);
            if (h.type == idn::kHello && n == int(sizeof(idn::HelloMsg))) {
                ++hellos_;
                idn::HelloMsg hello;
                std::memcpy(&hello, buf, sizeof(hello));
                idn::ChallengeMsg ch{};
                ch.hdr = {larecomp::browser::kMagic, larecomp::browser::kVersion, idn::kChallenge};
                ch.token = hello.token;
                ch.enforced = enforce_ ? 1 : 0;
                idn::RandomBytes(ch.nonce, 16);
                std::memcpy(ch.master_pubkey, master_key_, 32);
                ch.master_port = master_port_;
                std::snprintf(ch.master_host, sizeof(ch.master_host), "127.0.0.1");
                sendto(sock_, reinterpret_cast<const char*>(&ch), int(sizeof(ch)), 0,
                       reinterpret_cast<const sockaddr*>(&from), sizeof(from));
            } else if (h.type == idn::kProof && n == int(sizeof(idn::ProofMsg))) {
                ++proofs_;
                idn::ProofMsg p;
                std::memcpy(&p, buf, sizeof(p));
                uint8_t status = idn::kProofOk;
                if (!enforce_) status = idn::kProofNotEnforced;
                else if (!idn::CheckTicket(master_key_, p.ticket, idn::NowUnix())) status = idn::kProofBadTicket;
                else if (idn::NickFromField(p.relay, sizeof(p.relay)) != CanonicalName()) status = idn::kProofWrongRelay;
                else if (!idn::CheckSignature(p.sig, p.ticket.body.pubkey, idn::ProofMessage(p.nonce, p.relay, p.ticket)))
                    status = idn::kProofBadSignature;
                if (status == idn::kProofOk) admitted_.insert(idn::TicketNick(p.ticket));
                idn::ProofAckMsg ack{};
                ack.hdr = {larecomp::browser::kMagic, larecomp::browser::kVersion, idn::kProofAck};
                ack.token = p.token;
                ack.status = status;
                ack.admit_sec = uint32_t(idn::kAdmitSec);
                sendto(sock_, reinterpret_cast<const char*>(&ack), int(sizeof(ack)), 0,
                       reinterpret_cast<const sockaddr*>(&from), sizeof(from));
            }
        }
    }

    Sock sock_ = kBadSock;
    uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<bool> silent_{false};
    std::thread thread_;
    std::mutex mtx_;
    uint8_t master_key_[32] = {};
    uint16_t master_port_ = 0;
    bool enforce_ = true;
    std::set<std::string> admitted_;
    std::atomic<int> hellos_{0};
    std::atomic<int> proofs_{0};
};

}  // namespace idtest
