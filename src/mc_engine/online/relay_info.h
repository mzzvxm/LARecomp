#pragma once
//
// What the relay says about itself: its name, how many games are on it, and
// its icon. Asked over the relay's own UDP port with message types the online
// client never uses (20..23, protocol header v5), so it reaches the relay
// through whatever tunnel the game traffic already goes through. A relay
// older than 16/10/2026 does not answer them; Query then reports !ok.
//

#include <cstdint>
#include <string>
#include <vector>

namespace larecomp::relay_info {

struct Info {
    bool ok = false;        // the relay answered
    std::string name;       // its custom name, empty if none is set
    int players = -1;       // games registered on it
    int sessions = -1;      // of those, hosts with a live session
    int ping_ms = -1;       // round trip of the query
    uint32_t icon_size = 0; // 0 = no icon
    uint32_t icon_crc = 0;
};

// One query, waiting at most timeout_ms for the answer. Blocking.
Info Query(const std::string& host, uint16_t port, int timeout_ms);

// The icon (a PNG), fetched in chunks. Empty on any failure. Blocking.
std::vector<uint8_t> FetchIcon(const std::string& host, uint16_t port, uint32_t crc,
                               uint32_t size, int timeout_ms);

// mcla_online_server / mcla_online_port as the online client reads them:
// "host" or "host:port", the port cvar otherwise.
void ConfiguredServer(std::string& host, uint16_t& port);

}  // namespace larecomp::relay_info
