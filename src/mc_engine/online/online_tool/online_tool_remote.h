#pragma once
//
// larecomp_online_tool, the commands that read the Rate My Ride + leaderboards server (TCP 31002): `rmr`, `lb` and
// `tournament`. They ask what the game asks, with the game's own codecs (rate_my_ride/rmr_protocol.h,
// leaderboards/lb_protocol.h, tournament_schedule.h) and the game's own HTTP client (rate_my_ride/rmr_net.h), so a
// change to the protocol changes the game and the tool together. README.md has the command line and the JSON.
//
// Like the rest of the tool this file does no printing and reads no settings: online_tool_main.cpp prints what
// RunRemote returns, and the check (online_tool_remote_test.cpp) looks at every document.
//

#include <cstdint>
#include <string>
#include <vector>

namespace larecomp::online_tool {

enum class RemoteGroup { kNone, kRmr, kLb, kTournament };

// The words after the group, one enum for all three (each group allows its own).
enum class RemoteSub {
    kNone,
    // rmr
    kRmrTable, kRmrTop, kRmrMine, kRmrPhoto, kRmrRate,
    // lb
    kLbViews, kLbSize, kLbTop, kLbAround, kLbOwners,
    // tournament
    kTournamentCurrent, kTournamentStandings, kTournamentNext, kTournamentRewards,
};

struct RemoteArgs {
    RemoteGroup group = RemoteGroup::kNone;
    RemoteSub sub = RemoteSub::kNone;

    // Where: the game's rule (rmr_client.cpp ResolveTarget). --server is rmr_server ("host[:port]"), --relay is
    // mcla_online_server (only its host counts: the relay's own UDP port is not the server's), --port is
    // rmr_server_port. --server wins when both are given. Neither: no server ("not_set").
    std::string server;
    std::string relay;
    int port = 31002;
    int connect_timeout_ms = 3000;  // --connect-timeout-ms (the game's)
    int io_timeout_ms = 15000;      // --io-timeout-ms (the game's)
    int deadline_ms = 60000;        // --deadline-ms: no new request is started after this long (the one running may finish)

    // What
    uint32_t start = 1;             // --start: first rank (1-based)
    uint32_t count = 0;             // --count: rows or rides (0 = the command's default)
    uint32_t span = 5;              // --span: rows each side of the player (lb around)
    uint32_t min_ratings = 3;       // --min-ratings: a ride is ranked from this many ratings (the game sends 3)
    uint32_t weeks = 4;             // --weeks: tournament next
    uint32_t photo_slots = 1;       // --photo-slots: how many of a ride's two photos --photos fetches
    std::vector<uint64_t> owners;   // --owner a,b: owner keys (16 hex digits; public, they are in every record)
    std::vector<uint32_t> views;    // --view a,b: stats views
    bool view_given = false;        // --view was on the command line
    std::vector<uint32_t> files;    // --file a,b: Rate My Ride file ids (hex)
    uint32_t record = 0;            // --record: a ride's id (hex)
    int rating = 0;                 // --rating 1..10
    std::string photos_dir;         // --photos DIR: keep the photos of the rides listed
    std::string out_dir;            // --out DIR: where rmr photo writes
    std::string tables;             // --tables FILE|DIR: the game's online_tables.json, or the folder it is in (names and columns of stats views)
    bool token_stdin = false;       // --token-stdin: the player's Rate My Ride token (32 hex digits) is on stdin
    bool confirm = false;           // --confirm: the command changes the player's data on the server
    int64_t now = 0;                // --now UNIX: the clock for the checks (0 = the PC's clock)
    bool pretty = false;
};

// True for "rmr", "lb" and "tournament".
bool IsRemoteCommand(const std::string& word);

struct RemoteParse {
    bool ok = false;
    RemoteArgs args;
    std::string error;  // when !ok: one plain sentence
};

// `argv` without the program name; argv[0] is the group. Options are "--name value" or "--name=value".
RemoteParse ParseRemoteArgs(const std::vector<std::string>& argv);

const char* RemoteUsageText();
const char* RemoteGroupName(RemoteGroup g);
const char* RemoteSubName(RemoteSub s);

// The limits the commands keep (also in `--protocol`).
constexpr int kRemoteMaxRequests = 40;  // requests one call may make
constexpr uint32_t kRmrMaxPage = 50;
constexpr uint32_t kLbMaxRows = 100;
constexpr size_t kLbMaxViewsPerCall = 32;
constexpr uint32_t kRmrMaxPhotoSlots = 2;
constexpr size_t kTablesMaxBytes = 8 * 1024 * 1024;

// Runs the command (the network, the files) and returns its document; `exit_code` is 0 (the document is the answer)
// or 2 (the command line asked for something that cannot be done: the document has "ok": false).
// Blocks for as long as the game's own timeouts allow at most, per request; every failure of the server is part of the
// document, never an exception. Reads stdin only for --token-stdin (one line); the check passes the token's text as
// `token_text` instead.
std::string RunRemote(const RemoteArgs& args, int* exit_code, const std::string* token_text = nullptr);

// The pieces the check looks at on their own.

// "52000001" or "0x52000001": a hex number of up to 8 digits; false otherwise.
bool ParseHex32(const std::string& text, uint32_t& out);
// Up to 16 hex digits (optionally 0x); false otherwise.
bool ParseHex64(const std::string& text, uint64_t& out);
std::string Hex32Text(uint32_t v);  // 8 lowercase digits
std::string Hex64Text(uint64_t v);  // 16 lowercase digits

// The prize lines of a tournament description ("1st: Audi R8, TIS rims, hydraulics."): the places and the reward
// names the game knows. A line that names no known reward is left out.
struct PrizeLine {
    std::string place;
    std::vector<std::string> rewards;
};
std::vector<PrizeLine> PrizesFromDescription(const std::vector<std::string>& lines);

// A JPEG the tool is willing to keep: starts with FF D8 FF, at most 204,799 bytes.
bool LooksLikeJpeg(const std::vector<uint8_t>& bytes);

// The file a photo is kept in: "<file id, 8 digits>-<size>.jpg".
std::string PhotoFileName(uint32_t file_id, size_t size);

}  // namespace larecomp::online_tool
