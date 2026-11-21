#include "online_tool.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>

#include "../../../larecomp_version.h"  // LARECOMP_VERSION_LABEL (plain macros, no other includes)
#include "../protocol/identity_protocol.h"
#include "../relay_info.h"
#include "json_writer.h"

namespace larecomp::online_tool {
namespace {

namespace fs = std::filesystem;

std::filesystem::path PathFromUtf8(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

bool IsNumber(const std::string& s, int lo, int hi, int& out) {
    if (s.empty() || s.size() > 6) return false;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    if (v < lo || v > hi) return false;
    out = v;
    return true;
}

// A cached icon is only good when the file's own content still hashes to its name.
bool CachedIconOk(const fs::path& dir, uint32_t crc) {
    std::ifstream f(dir / PathFromUtf8(IconFileName(crc)), std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> bytes(browser::kIconMaxBytes + 1);
    f.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    bytes.resize(size_t(f.gcount()));
    return CheckIcon(bytes, crc).empty();
}

IconResult SettleIcon(bool wanted, const fs::path& dir, const browser::ServerView& v, const std::vector<uint8_t>& fetched) {
    IconResult r;
    if (!v.answered || v.icon_crc == 0) {
        r.state = IconResult::State::kNone;
        return r;
    }
    if (!wanted) {
        r.state = IconResult::State::kNotRequested;
        return r;
    }
    if (v.icon_size > browser::kIconMaxBytes) {
        r.state = IconResult::State::kTooLarge;
        return r;
    }
    const std::string name = IconFileName(v.icon_crc);
    if (fetched.empty()) {
        if (CachedIconOk(dir, v.icon_crc)) {
            r.state = IconResult::State::kCached;
            r.file = name;
        } else {
            r.state = IconResult::State::kUnavailable;
        }
        return r;
    }
    r.error = CheckIcon(fetched, v.icon_crc);
    if (!r.error.empty()) {
        r.state = IconResult::State::kInvalid;
        return r;
    }
    // Written beside its name and moved into place, so a reader never sees half an icon.
    static std::mt19937 rng{std::random_device{}()};
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), ".%08x.tmp", unsigned(rng()));
    const fs::path final_path = dir / PathFromUtf8(name);
    const fs::path temp_path = dir / PathFromUtf8(name + suffix);
    std::error_code ec;
    {
        std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(fetched.data()), std::streamsize(fetched.size()));
        out.close();
        if (!out) {
            fs::remove(temp_path, ec);
            r.state = IconResult::State::kWriteFailed;
            r.error = "cannot write the icons folder";
            return r;
        }
    }
    if (fs::exists(final_path, ec) && CachedIconOk(dir, v.icon_crc)) {
        fs::remove(temp_path, ec);  // another run was faster; its file has the same content
    } else {
        fs::remove(final_path, ec);  // a damaged copy from an earlier run goes
        fs::rename(temp_path, final_path, ec);
        if (ec) {
            fs::remove(temp_path, ec);
            r.state = IconResult::State::kWriteFailed;
            r.error = "cannot move the icon into place";
            return r;
        }
    }
    r.state = IconResult::State::kSaved;
    r.file = name;
    return r;
}

void BeginDocument(JsonWriter& j, const char* command, bool ok) {
    j.BeginObject();
    j.Field("ok", ok);
    j.Field("command", command);
    j.Field("schema", kSchema);
}

}  // namespace

// ---- command line ----------------------------------------------------------------------------------------------

const char* UsageText() {
    return
        "larecomp_online_tool: asks LARecomp's online servers for the launcher. One JSON document per call.\n"
        "\n"
        "  larecomp_online_tool --version                 the tool's version and JSON schema\n"
        "  larecomp_online_tool --protocol                what it speaks: relay messages, commands, limits\n"
        "  larecomp_online_tool servers [options]         the server list with each relay's details and rooms\n"
        "\n"
        "servers options:\n"
        "  --master H[:P]           the server list to read (port 31003 when absent); there is no built-in one\n"
        "  --servers a,b            the player's own relays (host[:port], port 31000 when absent)\n"
        "  --current H[:P]          the relay the game is set to\n"
        "  --icons DIR              keep relay icons there as <crc>.png (fetched only when this is given)\n"
        "  --timeout-ms N           one UDP query (default 800)\n"
        "  --master-timeout-ms N    the list's HTTP answer (default 3000)\n"
        "  --pretty                 indented output\n"
        "\n"
        "Exit code 0: the document is the answer (a list that does not answer is still an answer). 2: wrong command\n"
        "line. A document with \"ok\": false says why.\n";
}

const char* CommandName(Command c) {
    switch (c) {
        case Command::kHelp: return "help";
        case Command::kVersion: return "version";
        case Command::kProtocol: return "protocol";
        case Command::kServers: return "servers";
        case Command::kNone: break;
    }
    return "none";
}

ParseResult ParseArgs(const std::vector<std::string>& v) {
    ParseResult r;
    Args& a = r.args;
    auto fail = [&](std::string msg) {
        r.ok = false;
        r.error = std::move(msg);
        return r;
    };
    std::set<std::string> given;
    bool server_option = false;
    auto set_command = [&](Command c) {
        if (a.command != Command::kNone) return false;
        a.command = c;
        return true;
    };

    for (size_t i = 0; i < v.size(); ++i) {
        const std::string& tok = v[i];
        if (tok == "servers") {
            if (!set_command(Command::kServers)) return fail("only one command per call");
            continue;
        }
        if (tok == "--version") {
            if (!set_command(Command::kVersion)) return fail("only one command per call");
            continue;
        }
        if (tok == "--protocol") {
            if (!set_command(Command::kProtocol)) return fail("only one command per call");
            continue;
        }
        if (tok == "--help" || tok == "-h") {
            if (!set_command(Command::kHelp)) return fail("only one command per call");
            continue;
        }
        if (tok == "--pretty") {
            a.pretty = true;
            continue;
        }

        if (tok.rfind("--", 0) != 0) return fail("unknown command '" + tok + "'");
        std::string name = tok, value;
        bool has_value = false;
        if (const size_t eq = tok.find('='); eq != std::string::npos) {
            name = tok.substr(0, eq);
            value = tok.substr(eq + 1);
            has_value = true;
        }
        const bool known = name == "--master" || name == "--servers" || name == "--current" || name == "--icons" ||
                           name == "--timeout-ms" || name == "--master-timeout-ms";
        if (!known) return fail("unknown option '" + name + "'");
        if (!has_value) {
            if (i + 1 >= v.size() || v[i + 1].rfind("--", 0) == 0) return fail(name + " needs a value");
            value = v[++i];
        }
        if (!given.insert(name).second) return fail(name + " is given twice");
        server_option = true;
        if (name == "--master") a.master = value;
        else if (name == "--servers") a.servers = value;
        else if (name == "--current") a.current = value;
        else if (name == "--icons") a.icons_dir = value;
        else if (name == "--timeout-ms") {
            if (!IsNumber(value, 50, 10000, a.timeout_ms)) return fail("--timeout-ms is a number from 50 to 10000");
        } else {
            if (!IsNumber(value, 200, 60000, a.master_timeout_ms))
                return fail("--master-timeout-ms is a number from 200 to 60000");
        }
    }
    if (a.command == Command::kNone) return fail("no command (try --help)");
    if (server_option && a.command != Command::kServers) return fail("that option belongs to the servers command");
    r.ok = true;
    return r;
}

// ---- documents -------------------------------------------------------------------------------------------------

std::string RenderVersion(bool pretty) {
    JsonWriter j(pretty);
    BeginDocument(j, "version", true);
    j.Field("tool", kToolName);
    j.Field("version", kToolVersion);
    j.Field("game_label", LARECOMP_VERSION_LABEL);
    j.EndObject();
    return j.str();
}

std::string RenderProtocol(bool pretty) {
    namespace br = browser;
    JsonWriter j(pretty);
    BeginDocument(j, "protocol", true);
    j.Field("tool", kToolName);
    j.Field("version", kToolVersion);
    j.Key("commands").BeginArray().String("servers").EndArray();
    j.Key("relay").BeginObject();
    {
        char magic[5] = {char(br::kMagic & 255), char((br::kMagic >> 8) & 255), char((br::kMagic >> 16) & 255),
                         char((br::kMagic >> 24) & 255), 0};
        j.Field("magic", magic);
        j.Field("version", br::kVersion);
        j.Key("messages").BeginObject();
        j.Field("info_query", 20).Field("info", 21).Field("icon_query", 22).Field("icon_chunk", 23);
        j.Field("details_query", int(br::kDetailsQuery)).Field("details", int(br::kDetails));
        j.Field("rooms_query", int(br::kRoomsQuery)).Field("rooms", int(br::kRooms));
        j.Field("target_hint", int(br::kTargetHint)).Field("announce", int(br::kAnnounce));
        j.EndObject();
        j.Key("bytes").BeginObject();
        j.Field("header", int(sizeof(br::Header))).Field("details", int(sizeof(br::DetailsMsg)));
        j.Field("rooms", int(sizeof(br::RoomsMsg))).Field("room", int(sizeof(br::Room)));
        j.Field("target_hint", int(sizeof(br::TargetHintMsg))).Field("announce", int(sizeof(br::AnnounceMsg)));
        j.Field("info", int(relay_info::kInfoMsgBytes)).Field("icon_chunk", int(relay_info::kIconChunkMsgBytes));
        j.EndObject();
    }
    j.EndObject();
    j.Key("limits").BeginObject();
    j.Field("udp_timeout_ms", br::kQueryTimeoutMs);
    j.Field("master_connect_timeout_ms", br::kMasterConnectTimeoutMs);
    j.Field("master_io_timeout_ms", br::kMasterIoTimeoutMs);
    j.Field("max_servers", br::kMaxServers);
    j.Field("parallel_queries", br::kParallelQueries);
    j.Field("max_room_pages", br::kMaxRoomPages);
    j.Field("rooms_per_page", br::kRoomsPerPage);
    j.Field("room_props", br::kRoomProps);
    j.Field("icon_max_bytes", int(br::kIconMaxBytes));
    j.Field("icon_max_side", int(br::kIconMaxSide));
    j.Field("default_relay_port", int(br::kDefaultRelayPort));
    j.Field("default_master_port", int(br::kMasterPort));
    j.EndObject();
    j.EndObject();
    return j.str();
}

std::string RenderError(const std::string& command, const std::string& code, const std::string& message, bool pretty) {
    JsonWriter j(pretty);
    BeginDocument(j, command.c_str(), false);
    j.Key("error").BeginObject();
    j.Field("code", code);
    j.Field("message", message);
    j.EndObject();
    j.EndObject();
    return j.str();
}

const char* IconStateName(IconResult::State s) {
    switch (s) {
        case IconResult::State::kNotRequested: return "not_requested";
        case IconResult::State::kNone: return "none";
        case IconResult::State::kSaved: return "saved";
        case IconResult::State::kCached: return "cached";
        case IconResult::State::kUnavailable: return "unavailable";
        case IconResult::State::kInvalid: return "invalid";
        case IconResult::State::kTooLarge: return "too_large";
        case IconResult::State::kWriteFailed: return "write_failed";
    }
    return "none";
}

std::string RenderServers(const ServersReport& r, bool pretty) {
    namespace br = browser;
    JsonWriter j(pretty);
    BeginDocument(j, "servers", true);

    // The list.
    const auto& m = r.got.master;
    const char* state = "not_set";
    switch (m.state) {
        case br::MasterStatus::State::kNotSet: state = "not_set"; break;
        case br::MasterStatus::State::kOk: state = "ok"; break;
        case br::MasterStatus::State::kUnreachable: state = "unreachable"; break;
        case br::MasterStatus::State::kHttpError: state = "http_error"; break;
    }
    int listed = 0, answered = 0, players = 0, rooms = 0;
    for (const auto& s : r.got.servers) {
        listed += s.listed ? 1 : 0;
        if (!s.answered) continue;
        ++answered;
        players += s.players;
        rooms += s.rooms;
    }
    j.Key("master").BeginObject();
    j.Key("address");
    if (r.master_address.empty()) j.Null(); else j.String(r.master_address);
    j.Field("state", state);
    j.Field("message", m.text);
    j.Field("detail", m.detail);
    j.Field("http_status", m.http_status);
    j.Field("listed", listed);
    j.Field("via_favorites", m.via_favorites);
    j.EndObject();

    j.Key("summary").BeginObject();
    j.Field("servers", int(r.got.servers.size()));
    j.Field("answered", answered);
    j.Field("players", players);
    j.Field("rooms", rooms);
    j.Field("elapsed_ms", r.elapsed_ms);
    j.EndObject();

    j.Key("servers").BeginArray();
    for (size_t k = 0; k < r.got.servers.size(); ++k) {
        const br::ServerView& s = r.got.servers[k];
        const IconResult icon = k < r.icons.size() ? r.icons[k] : IconResult{};
        j.BeginObject();
        j.Field("address", s.address);
        j.Field("host", s.host);
        j.Field("port", int(s.port));
        j.Field("listed", s.listed);
        j.Field("favorite", s.favorite);
        j.Field("current", s.current);
        j.Field("answered", s.answered);
        j.Field("legacy", s.legacy);
        j.Key("ping_ms");
        if (s.answered && s.ping_ms >= 0) j.Int(s.ping_ms); else j.Null();
        j.Field("name", s.name);
        j.Field("description", s.description);
        j.Field("region", s.region);
        j.Key("tags").BeginArray();
        for (const auto& t : SplitTags(s.tags)) j.String(t);
        j.EndArray();
        j.Field("players", s.players);
        j.Field("rooms", s.rooms);
        // The flags are the relay's own answer: a legacy relay has none, so these are unknown, not false.
        j.Key("verified_nicks");
        if (s.answered && !s.legacy) j.Bool((s.flags & identity::kRelayVerifiedNicks) != 0); else j.Null();
        j.Key("password");
        if (s.answered && !s.legacy) j.Bool((s.flags & br::kRelayHasPassword) != 0); else j.Null();
        j.Key("icon").BeginObject();
        j.Key("crc");
        if (s.icon_crc) j.Hex32(s.icon_crc); else j.Null();
        j.Field("size", s.icon_size);
        j.Field("state", IconStateName(icon.state));
        j.Key("file");
        if (icon.file.empty()) j.Null(); else j.String(icon.file);
        j.Field("error", icon.error);
        j.EndObject();
        j.Key("mods").BeginObject();
        j.Field("required", s.mods_required);
        j.Field("optional", s.mods_optional);
        j.Field("http_port", int(s.http_port));
        j.Key("crc");
        if (s.mods_crc) j.Hex32(s.mods_crc); else j.Null();
        j.EndObject();
        j.Key("room_list").BeginArray();
        for (const auto& room : s.room_list) {
            j.BeginObject();
            j.Field("host_vip", VipText(room.host_vip));
            j.Field("host_gamertag", room.gamertag);
            j.Field("name", room.name);
            j.Field("players", room.cur_players);
            j.Field("max_players", room.max_players);
            j.Key("host_rtt_ms");
            if (room.host_rtt_ms >= 0) j.Int(room.host_rtt_ms); else j.Null();
            j.Field("active", room.active);
            j.Key("props").BeginArray();
            for (const auto& p : room.props) j.BeginObject().Field("id", p.id).Field("value", p.value).EndObject();
            j.EndArray();
            j.EndObject();
        }
        j.EndArray();
        j.EndObject();
    }
    j.EndArray();
    j.EndObject();
    return j.str();
}

// ---- the work --------------------------------------------------------------------------------------------------

ServersReport GatherServers(const Args& a) {
    const auto started = std::chrono::steady_clock::now();
    ServersReport rep;
    rep.master_address = a.master;

    const bool want_icons = !a.icons_dir.empty();
    fs::path dir;
    if (want_icons) {
        dir = PathFromUtf8(a.icons_dir);
        std::error_code ec;
        fs::create_directories(dir, ec);
    }

    browser::Options opts;
    opts.udp_timeout_ms = a.timeout_ms;
    opts.master_io_timeout_ms = a.master_timeout_ms;
    opts.fetch_icons = want_icons;
    opts.have_icon = [&](uint32_t crc) { return want_icons && CachedIconOk(dir, crc); };

    browser::Sources src;
    src.master = a.master;
    src.favorites = a.servers;
    src.current = a.current;
    rep.got = browser::Collect(src, opts, {});

    rep.icons.resize(rep.got.servers.size());
    for (size_t k = 0; k < rep.got.servers.size(); ++k)
        rep.icons[k] = SettleIcon(want_icons, dir, rep.got.servers[k], rep.got.icons[k]);

    rep.elapsed_ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
    return rep;
}

// ---- helpers ---------------------------------------------------------------------------------------------------

uint32_t Crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

std::string CheckIcon(const std::vector<uint8_t>& d, uint32_t crc) {
    static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (d.size() > browser::kIconMaxBytes) return "the icon is larger than 64 KB";
    if (d.size() < 33 || std::memcmp(d.data(), kSig, 8) != 0) return "the icon is not a PNG";
    if (std::memcmp(d.data() + 12, "IHDR", 4) != 0) return "the icon is not a PNG";
    auto be32 = [&](size_t o) {
        return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | uint32_t(d[o + 3]);
    };
    const uint32_t w = be32(16), h = be32(20);
    if (w == 0 || h == 0 || w > browser::kIconMaxSide || h > browser::kIconMaxSide)
        return "the icon is larger than 512x512 pixels";
    if (Crc32(d.data(), d.size()) != crc) return "the icon does not match the checksum the relay announced";
    return {};
}

std::string IconFileName(uint32_t crc) {
    static const char kDigits[] = "0123456789abcdef";
    std::string s(8, '0');
    for (int i = 7; i >= 0; --i, crc >>= 4) s[size_t(i)] = kDigits[crc & 15];
    return s + ".png";
}

std::string VipText(uint32_t vip) {
    return std::to_string((vip >> 24) & 255) + "." + std::to_string((vip >> 16) & 255) + "." +
           std::to_string((vip >> 8) & 255) + "." + std::to_string(vip & 255);
}

std::vector<std::string> SplitTags(const std::string& tags) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= tags.size()) {
        size_t comma = tags.find(',', pos);
        if (comma == std::string::npos) comma = tags.size();
        std::string t = tags.substr(pos, comma - pos);
        const size_t b = t.find_first_not_of(" \t");
        const size_t e = t.find_last_not_of(" \t");
        if (b != std::string::npos) out.push_back(t.substr(b, e - b + 1));
        pos = comma + 1;
    }
    return out;
}

}  // namespace larecomp::online_tool
