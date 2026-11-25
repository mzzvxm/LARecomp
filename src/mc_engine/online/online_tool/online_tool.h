#pragma once
//
// larecomp_online_tool: the launcher's way to ask what the game's own online code asks (README.md has the command
// line and the JSON). It is built from the game's own sources: the wire messages (protocol/browser_protocol.h), the
// relay queries (relay_info.cpp) and the browser's asking and cleaning (server_browser/browser_query.cpp), so the two
// can never read an answer differently. No ReXGlue, no settings, no game running; one JSON document per call.
//
// This file is the part between main() and those sources: reading the command line, saving icons, writing the
// documents. It does no printing itself, so the check (online_tool_test.cpp) can look at every document.
//

#include <cstdint>
#include <string>
#include <vector>

#include "../server_browser/browser_query.h"
#include "online_tool_remote.h"

namespace larecomp::online_tool {

constexpr const char* kToolName = "larecomp_online_tool";
// The tool's own version: bumps with every command or option added.
constexpr const char* kToolVersion = "0.3.0";
// The version of the JSON layout. Bumps when a field changes meaning or goes; adding a field does not (readers ignore
// fields they do not know). The launcher refuses a tool whose schema it does not know.
constexpr int kSchema = 1;

enum class Command { kNone, kHelp, kVersion, kProtocol, kServers, kRmr, kLb, kTournament };

struct Args {
    Command command = Command::kNone;
    bool pretty = false;
    // servers
    std::string master;       // --master: the server list, "host[:port]"; empty or only spaces = no list (there is no built-in default)
    std::string servers;      // --servers: the player's own, comma-separated
    std::string current;      // --current: the relay the game is on
    std::string icons_dir;    // --icons: where to keep relay icons, <crc>.png; empty = icons are not fetched
    int timeout_ms = browser::kQueryTimeoutMs;                      // --timeout-ms: one UDP query
    int master_timeout_ms = browser::kMasterIoTimeoutMs;            // --master-timeout-ms: the list's HTTP answer
    // rmr, lb, tournament (online_tool_remote.h): what the word after the command and its options said
    RemoteArgs remote;
};

struct ParseResult {
    bool ok = false;
    Args args;
    std::string error;  // when !ok: one plain sentence
};

// `argv` without the program name. Options are "--name value" or "--name=value".
ParseResult ParseArgs(const std::vector<std::string>& argv);

const char* UsageText();
const char* CommandName(Command c);  // "servers", "version", "protocol", "help", "rmr", "lb", "tournament", "none"

// ---- documents (one JSON object each, no trailing newline) -----------------------------------------------------

std::string RenderVersion(bool pretty);
std::string RenderProtocol(bool pretty);
// {"ok":false,"schema":1,"error":{"code":..,"message":..}}
std::string RenderError(const std::string& command, const std::string& code, const std::string& message, bool pretty);

// What happened to one relay's icon.
struct IconResult {
    enum class State { kNotRequested, kNone, kSaved, kCached, kUnavailable, kInvalid, kTooLarge, kWriteFailed };
    State state = State::kNotRequested;
    std::string file;   // "<crc>.png" inside the icons folder, when kSaved or kCached
    std::string error;  // a plain reason when kInvalid or kWriteFailed
};
const char* IconStateName(IconResult::State s);

struct ServersReport {
    std::string master_address;  // as given; empty = none
    browser::Collected got;
    std::vector<IconResult> icons;  // parallel to got.servers
    int elapsed_ms = 0;
};

std::string RenderServers(const ServersReport& r, bool pretty);

// Asks the network (master, relays, icons) and keeps icons in args.icons_dir. Blocks for at most a few seconds in
// the usual case (the game's own timeouts); every failure is part of the report, never an exception.
ServersReport GatherServers(const Args& args);

// ---- icons ---------------------------------------------------------------------------------------------------------

// CRC-32 (the one the relay announces for its icon: IEEE, reflected, as zlib).
uint32_t Crc32(const uint8_t* p, size_t n);

// Empty when `png` is an acceptable icon for `crc` (a PNG no larger than 64 KB, IHDR 1..512 pixels each way, whose
// CRC-32 is `crc`); otherwise the plain reason.
std::string CheckIcon(const std::vector<uint8_t>& png, uint32_t crc);

// The file an icon is kept in: eight lowercase hex digits of its CRC and ".png".
std::string IconFileName(uint32_t crc);  // "89abcdef.png"

// "10.0.0.2" for 0x0A000002.
std::string VipText(uint32_t vip);

// Splits a relay's comma-separated tags into trimmed, non-empty words.
std::vector<std::string> SplitTags(const std::string& tags);

}  // namespace larecomp::online_tool
