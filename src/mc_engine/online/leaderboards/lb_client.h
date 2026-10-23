#pragma once
//
// Game-side leaderboard backend.
//
// Requests go where Rate My Ride's go (rmr_server, else the relay host when
// online is on, else a local store under <user data>/leaderboards shared by the
// profiles of this PC), on one background thread so the game never waits on
// the network. A server that answers but predates the leaderboards (HTTP 404
// on /lb/v1) is remembered and the local store serves instead, so stats are
// never thrown away while the server is being updated.
//

#include <cstdint>
#include <functional>
#include <string>

#include "lb_protocol.h"

namespace lb::client {

bool Enabled();
bool Diag();

// The current player's owner key (the same identity Rate My Ride uses).
uint64_t PlayerOwnerKey();

std::string DescribeTarget();

using Done = std::function<void(Response&&)>;

// The token is filled in here. `done` runs on the worker thread.
void Submit(Request&& req, Done done);

}  // namespace lb::client
