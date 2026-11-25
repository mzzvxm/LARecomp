#pragma once
//
// The server browser's wire side: who to ask (the master's list, the player's own addresses, the relay the game is
// on) and what each relay says about itself, its rooms and its icon (browser_protocol.h, relay_info.h).
//
// No ReXGlue, no settings, no threads of its own that outlive a call: the game's browser client
// (browser_client.cpp) reads its three addresses from the settings and calls this, and larecomp_online_tool
// (online_tool/) calls the same code from the command line for the launcher, so the two ask and read answers in
// exactly the same way. Everything here blocks; call it from a worker thread.
//

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../protocol/browser_protocol.h"
#include "../relay_info.h"

namespace larecomp::browser {

// The game's own limits (the browser has always used these).
constexpr int kQueryTimeoutMs = 800;         // one UDP query, details, a rooms page, an icon chunk
constexpr int kMasterConnectTimeoutMs = 1500;
constexpr int kMasterIoTimeoutMs = 3000;
constexpr int kMaxServers = 128;             // the list is cut here
constexpr int kParallelQueries = 8;
constexpr int kMaxRoomPages = 16;            // 64 rooms
constexpr uint16_t kDefaultRelayPort = 31000;
constexpr size_t kIconMaxBytes = 64 * 1024;
constexpr uint32_t kIconMaxSide = 512;

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
    uint8_t flags = 0;      // DetailsMsg::flags (kRelayHasPassword, identity::kRelayVerifiedNicks); 0 for a legacy relay
    uint32_t icon_size = 0;
    uint32_t icon_crc = 0;
    // Mods it plays with, fetched over HTTP on host:http_port (modloader/modsbrowser/mod_download.h).
    uint16_t http_port = 0;
    int mods_required = 0;
    int mods_optional = 0;
    uint32_t mods_crc = 0;
    std::vector<RoomView> room_list;
};

// "host" or "host:port" (port kept when absent or invalid).
void SplitHostPort(const std::string& in, std::string& host, uint16_t& port);

// `in` without the white space at both ends. A server list address (the online_master_server setting) is always read through this:
// blank (empty, or only white space) means no list, and a stray space around an address is not part of it.
std::string TrimAddress(const std::string& in);

// The relay's answer to a target hint; false when it did not answer (an
// older relay) or the query failed. Blocking, worker threads only.
bool SendTargetHint(const std::string& host, uint16_t port, uint32_t host_vip, int timeout_ms);

// The length (1..4) of the strictly valid UTF-8 sequence at `p` (`avail` bytes left), or 0 when it is not one:
// stray or missing continuation bytes, overlong forms, surrogates, above U+10FFFF.
size_t Utf8SequenceLength(const unsigned char* p, size_t avail);

// Strings off the wire: `raw` is `cap` bytes that need not end in a NUL. Stops at the first NUL, keeps strictly
// valid UTF-8 only (stray bytes, overlong forms, surrogates and code points above U+10FFFF are dropped) and drops
// control characters (newlines only where asked).
std::string CleanWire(const char* raw, size_t cap, bool keep_newlines = false);

// One address to ask.
struct Target {
    std::string address;  // "host:port"
    std::string host;
    uint16_t port = 0;
    bool listed = false;
    bool favorite = false;
    bool current = false;
};

// Adds `text` ("host" or "host:port", spaces dropped; port 31000 when absent) to `list` unless it is empty, not a
// plain host name or address, a duplicate (the flags of the first copy are widened instead) or the list is full.
void AddTarget(std::vector<Target>& list, std::set<std::string>& seen, const std::string& text, bool listed,
               bool favorite, bool current);

// What the browser takes from a details answer (name, description, region, tags, counts, mods, icon).
void DecodeDetails(const DetailsMsg& a, ServerView& v);

// What the browser takes from a legacy relay's info answer (name, players, rooms, icon). The name is a fixed field off the wire
// and is cleaned like every other text of an answer.
void DecodeLegacy(const relay_info::Info& info, ServerView& v);

// Whether `data` starts with a PNG signature and an IHDR that claims at most `max_side` pixels each way. A file of 64 KB can claim
// a picture of hundreds of megabytes, and the decoder allocates what the header says: an icon is decoded only after this.
bool PngHeaderFits(const uint8_t* data, size_t size, uint32_t max_side);

// Appends the rooms of one answer page, skipping hosts already in `seen`; returns how many entries the page held
// (clamped to a page), whether or not they were new.
int AppendRooms(const RoomsMsg& page, std::set<uint32_t>& seen, std::vector<RoomView>& out);

// Switches of one run. The defaults are the game's.
struct Options {
    int udp_timeout_ms = kQueryTimeoutMs;
    int master_connect_timeout_ms = kMasterConnectTimeoutMs;
    int master_io_timeout_ms = kMasterIoTimeoutMs;
    bool fetch_icons = true;
    // Asked with a crc before its icon is fetched: true = already on hand, do not fetch.
    std::function<bool(uint32_t)> have_icon;
};

// What one relay says about itself, its rooms and (when it changed) its icon. `icon_out` stays empty when no icon
// was fetched (none, unchanged, not wanted, or the fetch failed).
ServerView QueryServer(const Target& t, uint32_t known_icon_crc, const Options& opts, std::vector<uint8_t>& icon_out);

struct MasterAnswer {
    bool ok = false;
    bool transport_ok = false;  // the master answered something (any HTTP status)
    int http_status = 0;
    std::string text;           // the game's one-line status when !ok
    std::string detail;         // the transport's reason ("connect timed out", ...); may be empty
    std::vector<std::string> lines;
};

// The master's list, as "host:port" lines. !ok when `address` is not a master (or does not answer).
MasterAnswer FetchMaster(const std::string& address, const Options& opts);

// The lines of a master's list body ('\n'-separated; a '\r' stays on the line, AddTarget drops it).
std::vector<std::string> SplitLines(const std::string& body);

// Where to look. All three may be blank (empty or only white space). A blank `master` means there is no server list:
// it is not asked, and only the player's own addresses and the current relay are.
struct Sources {
    std::string master;     // "host[:port]" of the server list (port 31003 when absent); blank (empty or only white space) = no list
    std::string favorites;  // comma-separated "host[:port]": the player's own
    std::string current;    // "host[:port]" of the relay the game is on
};

struct MasterStatus {
    enum class State { kNotSet, kOk, kUnreachable, kHttpError };
    State state = State::kNotSet;  // of the master in Sources::master
    std::string text;              // the game's status line; empty = fine
    std::string detail;
    int http_status = 0;
    int lines = 0;                 // lines the master listed
    int via_favorites = 0;         // addresses of the player's own that turned out to be masters
};

struct Collected {
    std::vector<ServerView> servers;             // in the order they were asked: the list, own, current, then
                                                 // the relays of masters found among the player's own
    std::vector<std::vector<uint8_t>> icons;     // parallel to servers; empty when none was fetched
    MasterStatus master;
};

// Asks the master, then every address (kParallelQueries at a time). An address of the player's own that is no relay
// but answers /servers is treated as a master: its relays are asked too and it leaves the result.
Collected Collect(const Sources& src, const Options& opts, const std::map<std::string, uint32_t>& known_icon_crcs);

}  // namespace larecomp::browser
