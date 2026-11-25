#pragma once
//
// The Director's channel for agents: a TCP listener on 127.0.0.1 (loopback only: it binds nothing else, so the OS asks the user for nothing) that speaks JSON
// lines (DIRECTOR-PROTOCOL.md). One IO thread (select over the listener and the clients); the game thread never touches a socket except to hand a reply to a
// connection's out-buffer. A client must authenticate with the run's token first; at most kMaxClients are served at once. A request goes to the Engine's queue
// (Engine::Post) and is run by Director::Tick on the game thread; its reply and the events come back through the connection.
//
// Pure of the game: the test builds it with a local Engine and talks to it through real loopback sockets.
//

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "command.h"

namespace mc::cinematic::director {

// 128 random bits as 32 lower-case hex digits (the OS's entropy where the standard library has it, mixed with the clock and the process).
std::string MakeToken();
// A constant-time comparison (the token is a secret).
bool TokenEquals(std::string_view a, std::string_view b);

class LineServer {
  public:
    LineServer(Engine& engine, std::string token);
    ~LineServer();
    LineServer(const LineServer&) = delete;
    LineServer& operator=(const LineServer&) = delete;

    // What `auth` answers besides {proto}: version, pid, slot, scene, commands. Set before Start.
    void SetHello(std::function<Json()> hello) { hello_ = std::move(hello); }

    // Binds 127.0.0.1:`port` (0: any free port) and starts the IO thread. False with `error` when it could not (the port is taken, no sockets).
    bool Start(uint16_t port, std::string* error);
    // Closes every connection and joins the thread. Safe to call twice.
    void Stop();
    bool running() const { return running_.load(std::memory_order_acquire); }
    uint16_t port() const { return port_; }
    const std::string& token() const { return token_; }

    struct ClientInfo {
        int index = -1;
        std::string name;
        std::string last_cmd;
        double last_cmd_age_s = 0.0;
        uint64_t commands = 0;
    };
    // The authenticated clients now.
    std::vector<ClientInfo> Clients() const;

    struct Client;  // a connection (public so the file's own helpers can name it; nothing outside line_server.cpp uses it)

  private:
    void Run();
    void AcceptNew();
    void ReadFrom(const std::shared_ptr<Client>& c);
    void HandleLine(const std::shared_ptr<Client>& c, const std::string& line);
    void HandleAuth(const std::shared_ptr<Client>& c, const Json& obj, bool has_id, int64_t id);
    void Send(const std::shared_ptr<Client>& c, const std::string& line);
    void SendReply(const std::shared_ptr<Client>& c, const Reply& reply);
    void Drop(const std::shared_ptr<Client>& c, const char* why);
    void FlushOut(const std::shared_ptr<Client>& c);
    void BroadcastEvent(const std::string& line, std::string_view name);

    Engine& engine_;
    std::string token_;
    std::function<Json()> hello_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    uint16_t port_ = 0;
    std::thread thread_;

    // the sockets are held as plain integers so that this header needs no socket headers
    std::intptr_t listen_ = -1;
    mutable std::mutex clients_mutex_;
    std::vector<std::shared_ptr<Client>> clients_;
};

}  // namespace mc::cinematic::director
