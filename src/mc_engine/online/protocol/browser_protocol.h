#pragma once
//
// Server browser wire protocol: what the relay says about itself and its
// rooms, and how a relay gets itself listed on the master server.
//
// Shared by three programs, so there is one copy of it:
//   - the relay (relay_server/relay_server.cpp) answers the queries;
//   - the master (master_server/master_server_main.cpp) takes announces and
//     checks them with a details query;
//   - the game (server_browser/*.cpp) asks.
//
// Every message starts with the relay protocol's v5 header (magic "MC02",
// version 5) and uses types the SDK's online client never sends (it uses
// 1..10; relay_info.cpp has 20..23). A query is always padded to the size of
// its answer, so nobody can use a relay or the master to send more bytes to
// someone else than they sent themselves.
//

#include <cstddef>
#include <cstdint>

namespace larecomp::browser {

constexpr uint32_t kMagic = 0x3230434D;  // "MC02"
constexpr uint32_t kVersion = 5;

// The master listens for announces on UDP and serves the list over HTTP on
// the same port number (TCP).
constexpr uint16_t kMasterPort = 31003;
// How often a relay announces itself, and how long the master keeps a relay
// it has not heard from.
constexpr int kAnnounceEverySec = 30;
constexpr int kListingTtlSec = 100;

enum Type : uint8_t {
    kDetailsQuery = 24,  // game/master -> relay
    kDetails = 25,       // relay -> asker
    kRoomsQuery = 26,    // game -> relay
    kRooms = 27,         // relay -> game
    kTargetHint = 28,    // game -> relay, and the relay's ack
    kAnnounce = 40,      // relay -> master
};

// Relay flags (DetailsMsg::flags).
constexpr uint8_t kRelayHasPassword = 0x01;  // reserved; no relay sets it yet

// Room flags (Room::flags).
constexpr uint8_t kRoomActive = 0x01;  // the host answered a search lately or has players

constexpr int kRoomsPerPage = 4;
constexpr int kRoomProps = 24;

#pragma pack(push, 1)

struct Header {
    uint32_t magic;
    uint32_t version;
    uint8_t type;
};

struct Prop {
    uint32_t id;
    uint32_t value;
};

// Everything the browser shows about one relay. Strings are NUL-terminated
// printable ASCII (the relay cleans them), but a reader must still treat them
// as hostile: terminate and filter again before use.
struct DetailsMsg {
    Header hdr;
    uint32_t token;  // echoed from the query
    // ListingProof(the relay's secret, token): lets the master check that the
    // relay answering is the one that announced, without the secret itself
    // ever being in an answer anyone can ask for.
    uint64_t proof;
    uint16_t players;      // games connected
    uint16_t rooms;        // live rooms
    uint16_t relay_port;   // the UDP port the relay is bound to
    uint8_t flags;
    uint8_t reserved;
    uint32_t icon_size;    // 0 = no icon; fetched with relay_info's icon chunks (types 22/23)
    uint32_t icon_crc;
    char name[48];
    char description[256];
    char region[24];
    char tags[96];  // comma-separated
    // Mods the server plays with (mods/ next to the relay). Their list and
    // files are served over HTTP on the same host at http_port:
    //   GET /mods/manifest                -> ModManifest text (below)
    //   GET /mods/file/<mod>/<path>       -> one listed file
    uint16_t http_port;     // 0 = no HTTP service (no mods to fetch)
    uint8_t mods_required;  // how many mods a player must have
    uint8_t mods_optional;  // how many more the server offers
    uint32_t mods_crc;      // changes whenever any mod file changes
};

// The manifest, one line per entry, '\n'-separated, fields by '\t':
//   mod  <name> <required 0|1> <file count> <total bytes>
//   file <mod> <relative path, '/'-separated> <bytes> <fnv1a64 hex>
// Mod names are [A-Za-z0-9_.-] and never start with '.'; paths never hold
// "..", a drive or a leading '/'. A reader checks all of that again.
constexpr const char* kModManifestPath = "/mods/manifest";
constexpr const char* kModFilePrefix = "/mods/file/";

// FNV-1a 64 over a whole file, streamed: what the manifest's hash is.
struct Fnv1a64 {
    uint64_t h = 0xcbf29ce484222325ull;
    void Add(const void* data, size_t n) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    }
};

// One hosted game on a relay. host_vip is how the game itself sees that host:
// the source of its LAN search answer (System Link) and the address in its
// session search result (Live), which is what the join filter matches.
struct Room {
    uint32_t host_vip;
    uint16_t host_rtt_ms;  // relay <-> host round trip; 0xFFFF = unknown
    uint8_t cur_players;
    uint8_t max_players;
    uint8_t flags;
    uint8_t prop_count;
    char gamertag[16];  // the host's
    char name[32];      // the session name the host advertised
    Prop props[kRoomProps];  // the host's session properties/contexts, raw
};

struct RoomsMsg {
    Header hdr;
    uint32_t token;
    uint16_t page;   // asked: which page; answered: the same
    uint16_t total;  // rooms on the relay right now (all pages)
    uint8_t count;   // valid entries in rooms[]
    Room rooms[kRoomsPerPage];
};

// The game's own host list (the SDK's search, answered by the relay) holds
// only two hosts, in no order the game controls. A player who picked a room
// in the browser sends this from the same machine first, so the relay puts
// that host at the top of the next list sent to this IP. Keyed by source IP
// (not port): the browser socket is not the game's. The hint expires on its
// own after a minute; host_vip 0 drops it now. The ack is this same message
// back, which also tells an old relay (no answer) from a new one.
struct TargetHintMsg {
    Header hdr;
    uint32_t token;     // echoed in the ack
    uint32_t host_vip;  // Room::host_vip; 0 = clear
};

// A relay asking to be listed. The master never trusts the address in it: it
// sends a details query there and lists the relay only if the answer's proof
// matches this secret. Only a relay that announces itself can be listed: its
// secret (random per run) is sent to the master alone.
struct AnnounceMsg {
    Header hdr;
    uint8_t secret[16];
    uint16_t public_port;   // the port players connect to
    char public_host[64];   // host name or IP players connect to; empty = the announce's source IP
    uint8_t pad[sizeof(DetailsMsg) - sizeof(Header) - 16 - 2 - 64];  // as large as the probe it triggers
};

#pragma pack(pop)

// SipHash-2-4 of the query token, keyed with the relay's secret.
inline uint64_t ListingProof(const uint8_t (&secret)[16], uint32_t token) {
    auto load = [&](int o) {
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i) v = (v << 8) | secret[o + i];
        return v;
    };
    auto rotl = [](uint64_t x, int b) { return (x << b) | (x >> (64 - b)); };
    const uint64_t k0 = load(0), k1 = load(8);
    uint64_t v0 = k0 ^ 0x736f6d6570736575ull, v1 = k1 ^ 0x646f72616e646f6dull;
    uint64_t v2 = k0 ^ 0x6c7967656e657261ull, v3 = k1 ^ 0x7465646279746573ull;
    auto round = [&] {
        v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32);
        v2 += v3; v3 = rotl(v3, 16); v3 ^= v2;
        v0 += v3; v3 = rotl(v3, 21); v3 ^= v0;
        v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32);
    };
    // One 4-byte message: no full 8-byte block, the last block carries it.
    const uint64_t m = (uint64_t(4) << 56) | token;
    v3 ^= m;
    round();
    round();
    v0 ^= m;
    v2 ^= 0xff;
    round();
    round();
    round();
    round();
    return v0 ^ v1 ^ v2 ^ v3;
}

static_assert(sizeof(AnnounceMsg) == sizeof(DetailsMsg), "announce must not amplify");
static_assert(sizeof(RoomsMsg) <= 1200, "a rooms page must fit one datagram");

}  // namespace larecomp::browser
