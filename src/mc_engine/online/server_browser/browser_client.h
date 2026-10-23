#pragma once
//
// The server browser's network side: which relays exist (the master's list,
// the player's own addresses, the relay the game is on) and what each says
// about itself and its rooms (browser_protocol.h). Everything runs on worker
// threads; the window reads a snapshot.
//

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../protocol/browser_protocol.h"

namespace larecomp::browser {

struct RoomView {
    uint32_t host_vip = 0;
    int host_rtt_ms = -1;  // relay <-> host, -1 unknown
    int cur_players = 0;
    int max_players = 0;
    bool active = false;
    std::string gamertag;
    std::string name;
    std::vector<Prop> props;
};

struct ServerView {
    std::string address;  // "host:port", the key
    std::string host;
    uint16_t port = 0;
    bool listed = false;    // on the master's list
    bool favorite = false;  // in server_browser_servers
    bool current = false;   // the relay mcla_online_server points at

    bool answered = false;  // the last query got details back
    bool legacy = false;    // an older relay: name, players and ping only, no rooms
    bool queried = false;   // asked at least once
    int ping_ms = -1;
    std::string name, description, region, tags;
    int players = 0;
    int rooms = 0;
    uint32_t icon_crc = 0;
    // Mods it plays with, fetched over HTTP on host:http_port (modloader/modsbrowser/mod_download.h).
    uint16_t http_port = 0;
    int mods_required = 0;
    int mods_optional = 0;
    uint32_t mods_crc = 0;
    std::vector<RoomView> room_list;
};

struct Snapshot {
    std::vector<ServerView> servers;
    bool refreshing = false;
    std::string master_status;  // empty = fine
    uint64_t generation = 0;    // bumps whenever anything changes
};

// A decoded icon waiting for a texture, keyed by the server address.
struct IconUpdate {
    std::string address;
    uint32_t crc = 0;
    std::vector<uint8_t> png;
};

class Client {
public:
    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Asks the master and every known relay again. Any thread.
    void Refresh();
    // While true, refreshes on its own every few seconds.
    void SetAutoRefresh(bool on);

    Snapshot Take() const;
    // Icons that arrived since the last call.
    std::vector<IconUpdate> TakeIcons();

    struct State;  // the worker's, defined in browser_client.cpp

private:
    std::shared_ptr<State> state_;
};

// The relay's answer to a target hint; false when it did not answer (an
// older relay) or the query failed. Blocking, worker threads only.
bool SendTargetHint(const std::string& host, uint16_t port, uint32_t host_vip, int timeout_ms);

// "host" or "host:port" (port kept when absent or invalid).
void SplitHostPort(const std::string& in, std::string& host, uint16_t& port);

}  // namespace larecomp::browser
