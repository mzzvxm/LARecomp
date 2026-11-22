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
#include "browser_query.h"  // RoomView, ServerView, SplitHostPort, SendTargetHint: the wire side, shared with larecomp_online_tool

namespace larecomp::browser {

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

}  // namespace larecomp::browser
