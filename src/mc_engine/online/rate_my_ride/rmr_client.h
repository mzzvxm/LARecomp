#pragma once
//
// Game-side Rate My Ride backend.
//
// Picks where requests go (re-evaluated per request, so toggling online at
// runtime applies to the next one):
//   rmr_server set            -> that server ("host[:port]")
//   mcla_online_enable on     -> the relay host (mcla_online_server) on
//                                rmr_server_port, i.e. the shared server
//   otherwise                 -> a local store in <user data>/ratemyride, which
//                                every profile on this machine shares
// Requests run on one background thread so the game never waits on the
// network; the caller gets the response back on that thread.
//

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "rmr_protocol.h"

namespace rmr::client {

bool Enabled();
bool Diag();

// Where the shared server is, by the rules above: true with its host and port,
// false when requests stay in a local store. The leaderboards ride on the same
// server, so they ask here too.
bool ResolveServer(std::string& host, uint16_t& port);

// The user data root (the directory the profiles live in).
std::filesystem::path UserDataRoot();

// The current player's secret token and the public owner key derived from it.
// Kept with the online identity (../identity/identity_store.h), outside the
// save folder; an old <user data>/<profile>/ratemyride/identity.key is moved
// there on first use.
Token PlayerToken();
uint64_t PlayerOwnerKey();

// Human-readable destination of the next request ("local <dir>" / "host:port").
std::string DescribeTarget();

using Done = std::function<void(Response&&)>;
void Submit(Request&& req, Done done);

}  // namespace rmr::client
