#include "online_tool_remote.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>

#ifdef _WIN32
#include <io.h>
#define LARECOMP_ISATTY _isatty
#define LARECOMP_FILENO _fileno
#else
#include <unistd.h>
#define LARECOMP_ISATTY isatty
#define LARECOMP_FILENO fileno
#endif

#include "../leaderboards/lb_protocol.h"
#include "../rate_my_ride/rmr_net.h"
#include "../rate_my_ride/rmr_protocol.h"
#include "../server_browser/browser_query.h"
#include "../tournament_schedule.h"
#include "json_reader.h"
#include "json_writer.h"
#include "online_tool.h"

namespace larecomp::online_tool {
namespace {

namespace fs = std::filesystem;

constexpr int kDefaultTopCount = 10;
constexpr uint32_t kMaxViewId = 0xFFFF;  // lb_store.cpp: the title's views are 1..500, the store takes up to 0xFFFF
constexpr uint32_t kMaxWeek = 100000;    // about 1900 years: a week number above this is not a week
constexpr uint32_t kDefaultStandings = 100;

std::string Clean(const std::string& s, bool keep_newlines = false) {
    return browser::CleanWire(s.data(), s.size(), keep_newlines);
}

fs::path PathFromUtf8(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string Trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

std::vector<std::string> SplitOn(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t at = s.find(sep, pos);
        if (at == std::string::npos) at = s.size();
        out.push_back(s.substr(pos, at - pos));
        pos = at + 1;
    }
    return out;
}

bool ParseNumber(const std::string& s, long long lo, long long hi, long long& out) {
    if (s.empty() || s.size() > 12) return false;
    long long v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    if (v < lo || v > hi) return false;
    out = v;
    return true;
}

bool ParseHexDigits(const std::string& text, size_t max_digits, uint64_t& out) {
    size_t i = 0;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) i = 2;
    const size_t digits = text.size() - i;
    if (digits == 0 || digits > max_digits) return false;
    uint64_t v = 0;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= uint64_t(c - '0');
        else if (c >= 'a' && c <= 'f') v |= uint64_t(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= uint64_t(c - 'A' + 10);
        else return false;
    }
    out = v;
    return true;
}

// host or host:port, strictly: a plain host name or address, a port from 1 to 65535.
bool ParseEndpoint(const std::string& text, std::string& host, uint16_t& port, bool& has_port) {
    host.clear();
    has_port = false;
    if (text.empty() || text.size() > 253) return false;
    std::string h = text;
    const size_t colon = text.rfind(':');
    if (colon != std::string::npos) {
        long long p = 0;
        if (!ParseNumber(text.substr(colon + 1), 1, 65535, p)) return false;
        port = uint16_t(p);
        has_port = true;
        h = text.substr(0, colon);
    }
    if (h.empty()) return false;
    for (char c : h) {
        const bool plain = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' ||
                           c == '-' || c == '_';
        if (!plain) return false;
    }
    host = h;
    return true;
}

// ---- options ---------------------------------------------------------------------------------------------------------

struct OptInfo {
    const char* name;
    bool flag;
};

constexpr OptInfo kOptions[] = {
    {"--server", false},   {"--relay", false},       {"--port", false},        {"--connect-timeout-ms", false},
    {"--io-timeout-ms", false}, {"--deadline-ms", false}, {"--start", false},  {"--count", false},       {"--span", false},
    {"--min-ratings", false}, {"--weeks", false},    {"--photo-slots", false}, {"--owner", false},
    {"--view", false},     {"--file", false},        {"--record", false},      {"--rating", false},
    {"--photos", false},   {"--out", false},         {"--tables", false},      {"--token-stdin", true},
    {"--confirm", true},   {"--now", false},         {"--pretty", true},
};

const OptInfo* FindOption(const std::string& name) {
    for (const OptInfo& o : kOptions)
        if (name == o.name) return &o;
    return nullptr;
}

std::set<std::string> AllowedOptions(RemoteSub sub) {
    std::set<std::string> a = {"--pretty"};
    auto add = [&](std::initializer_list<const char*> names) {
        for (const char* n : names) a.insert(n);
    };
    const bool network = sub != RemoteSub::kLbViews && sub != RemoteSub::kTournamentNext;
    if (network) add({"--server", "--relay", "--port", "--connect-timeout-ms", "--io-timeout-ms", "--deadline-ms"});
    switch (sub) {
        case RemoteSub::kRmrTable: add({"--min-ratings"}); break;
        case RemoteSub::kRmrTop: add({"--min-ratings", "--start", "--count", "--owner", "--photos", "--photo-slots"}); break;
        case RemoteSub::kRmrMine: add({"--owner", "--token-stdin", "--photos", "--photo-slots"}); break;
        case RemoteSub::kRmrPhoto: add({"--file", "--out"}); break;
        case RemoteSub::kRmrRate: add({"--record", "--rating", "--confirm", "--token-stdin"}); break;
        case RemoteSub::kLbViews: add({"--tables"}); break;
        case RemoteSub::kLbSize: add({"--view", "--tables"}); break;
        case RemoteSub::kLbTop: add({"--view", "--start", "--count", "--owner", "--tables"}); break;
        case RemoteSub::kLbAround: add({"--view", "--owner", "--token-stdin", "--span", "--tables"}); break;
        case RemoteSub::kLbOwners: add({"--view", "--owner", "--tables"}); break;
        case RemoteSub::kTournamentCurrent: add({"--now"}); break;
        case RemoteSub::kTournamentStandings:
            add({"--start", "--count", "--owner", "--token-stdin", "--view", "--tables", "--now"});
            break;
        case RemoteSub::kTournamentNext: add({"--weeks", "--now"}); break;
        case RemoteSub::kTournamentRewards: add({"--token-stdin", "--now"}); break;
        case RemoteSub::kNone: break;
    }
    return a;
}

struct SubWord {
    RemoteGroup group;
    const char* word;
    RemoteSub sub;
};

constexpr SubWord kSubs[] = {
    {RemoteGroup::kRmr, "table", RemoteSub::kRmrTable},
    {RemoteGroup::kRmr, "top", RemoteSub::kRmrTop},
    {RemoteGroup::kRmr, "mine", RemoteSub::kRmrMine},
    {RemoteGroup::kRmr, "photo", RemoteSub::kRmrPhoto},
    {RemoteGroup::kRmr, "rate", RemoteSub::kRmrRate},
    {RemoteGroup::kLb, "views", RemoteSub::kLbViews},
    {RemoteGroup::kLb, "size", RemoteSub::kLbSize},
    {RemoteGroup::kLb, "top", RemoteSub::kLbTop},
    {RemoteGroup::kLb, "around", RemoteSub::kLbAround},
    {RemoteGroup::kLb, "owners", RemoteSub::kLbOwners},
    {RemoteGroup::kTournament, "current", RemoteSub::kTournamentCurrent},
    {RemoteGroup::kTournament, "standings", RemoteSub::kTournamentStandings},
    {RemoteGroup::kTournament, "next", RemoteSub::kTournamentNext},
    {RemoteGroup::kTournament, "rewards", RemoteSub::kTournamentRewards},
};

// ---- the server ------------------------------------------------------------------------------------------------------

struct ServerStatus {
    std::string address;  // "host:port", empty = none
    // "not_set" | "ok" | "unreachable" | "timed_out" | "http_error" | "no_service" | "bad_answer" | "refused" | "limit"
    std::string state = "not_set";
    std::string message;
    std::string detail;
    int http_status = 0;
    uint32_t err = 0;
};

const char* ErrText(uint32_t err) {
    switch (err) {
        case rmr::kInvalidArgs: return "the server refused the request (not allowed, or not valid)";
        case rmr::kConnection: return "the server reported a connection problem";
        case rmr::kBufferOverflow: return "the file is larger than the buffer asked for";
        case rmr::kNotAllowed: return "the server does not allow that";
        case rmr::kFileNotFound: return "the server has no such ride or file";
        case rmr::kUnknown: return "the server reported an error";
        case rmr::kTimedOut: return "the server reported a time-out";
        default: return "the server reported an error";
    }
}

class Session {
public:
    explicit Session(const RemoteArgs& a) : a_(a), started_(std::chrono::steady_clock::now()) {
        uint16_t port = uint16_t(a.port);
        std::string host;
        bool has_port = false;
        if (!a.server.empty()) {
            ParseEndpoint(a.server, host, port, has_port);
        } else if (!a.relay.empty()) {
            uint16_t ignored = 0;  // the relay's own UDP port is not the server's
            ParseEndpoint(a.relay, host, ignored, has_port);
        }
        if (!host.empty()) {
            host_ = host;
            port_ = port;
            set_ = true;
            st.address = host + ":" + std::to_string(port);
            st.state = "ok";
        } else {
            st.message = "no server is set";
        }
    }

    bool Set() const { return set_; }
    int Requests() const { return requests_; }

    // One Rate My Ride request. True when an answer was decoded (its err may still be non-zero: see Accept).
    bool Rmr(rmr::Request req, rmr::Response& resp) {
        req.token = token;
        std::vector<uint8_t> reply;
        if (!Exchange(rmr::kHttpPath, rmr::EncodeRequest(req), reply, false)) return false;
        resp = rmr::Response{};
        if (!rmr::DecodeResponse(reply, resp)) return Bad("the answer is not a Rate My Ride answer");
        return true;
    }

    bool Lb(lb::Request req, lb::Response& resp) {
        req.token = token;
        std::vector<uint8_t> reply;
        if (!Exchange(lb::kHttpPath, lb::EncodeRequest(req), reply, true)) return false;
        resp = lb::Response{};
        if (!lb::DecodeResponse(reply, resp)) return Bad("the answer is not a leaderboards answer");
        return true;
    }

    // False (and the state says why) when the server answered with an error code.
    bool Accept(uint32_t err) {
        if (err == 0) return true;
        Refuse(err);
        return false;
    }
    void Refuse(uint32_t err) {
        st.state = "refused";
        st.err = err;
        st.message = ErrText(err);
    }
    bool Bad(const std::string& why) {
        st.state = "bad_answer";
        st.message = why;
        failed_ = true;
        return false;
    }
    bool Usable() const { return set_ && !failed_; }

    ServerStatus st;
    rmr::Token token{};  // all zero unless the player's own token was given (--token-stdin)

private:
    bool Exchange(const char* path, const std::vector<uint8_t>& body, std::vector<uint8_t>& reply, bool is_lb) {
        if (!set_) return false;
        if (failed_) return false;
        if (requests_ >= kRemoteMaxRequests) {
            failed_ = true;
            st.state = "limit";
            st.message = "the call made as many requests as it may";
            return false;
        }
        if (std::chrono::steady_clock::now() - started_ > std::chrono::milliseconds(a_.deadline_ms)) {
            failed_ = true;
            st.state = "limit";
            st.message = "the call ran out of time";
            return false;
        }
        ++requests_;
        const rmr::net::PostResult r =
            rmr::net::Post(host_, port_, path, body, a_.connect_timeout_ms, a_.io_timeout_ms);
        if (!r.transport_ok) {
            failed_ = true;
            st.state = r.timed_out ? "timed_out" : "unreachable";
            st.message = "the server did not answer (" + st.address + ")";
            st.detail = r.error;
            return false;
        }
        st.http_status = r.status;
        if (is_lb && r.status == 404) {
            failed_ = true;
            st.state = "no_service";
            st.message = "this server has no leaderboards (an older larecomp_rmr_server)";
            return false;
        }
        if (r.status != 200) {
            failed_ = true;
            st.state = "http_error";
            st.message = "the server answered HTTP " + std::to_string(r.status);
            return false;
        }
        reply = r.body;
        return true;
    }

    const RemoteArgs& a_;
    std::chrono::steady_clock::time_point started_;
    std::string host_;
    uint16_t port_ = 0;
    bool set_ = false;
    bool failed_ = false;
    int requests_ = 0;
};

// ---- the game's own tables (online_tables.json) ----------------------------------------------------------------------

struct TableColumn {
    uint32_t prop = 0;
    int type = 0;
    uint32_t attr = 0;
    int agg = 0;
};

struct TableView {
    uint32_t id = 0;
    std::string name;  // empty = the file has no name for it
    std::vector<TableColumn> columns;
};

struct Tables {
    // "not_given" | "ok" | "not_found" | "unreadable" | "invalid" | "unknown_schema" | "stats_unavailable"
    std::string state = "not_given";
    std::string game_version;
    std::string error;
    std::map<uint32_t, TableView> views;
};

const char* TypeName(int type) {
    switch (type) {
        case lb::kTypeContext: return "context";
        case lb::kTypeInt32: return "int32";
        case lb::kTypeInt64: return "int64";
        case lb::kTypeDouble: return "double";
        case lb::kTypeFloat: return "float";
    }
    return "unknown";
}

std::string AggName(int agg) {
    switch (agg) {
        case lb::kAggLast: return "last";
        case lb::kAggSum: return "sum";
        case lb::kAggMin: return "min";
        case lb::kAggMax: return "max";
    }
    return "agg_" + std::to_string(agg);
}

Tables LoadTables(const std::string& path) {
    Tables t;
    if (path.empty()) return t;
    std::error_code ec;
    const fs::path p = PathFromUtf8(path);
    if (!fs::exists(p, ec)) {
        t.state = "not_found";
        t.error = "the file does not exist";
        return t;
    }
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        t.state = "unreadable";
        t.error = "the file cannot be read";
        return t;
    }
    std::string text(kTablesMaxBytes + 1, '\0');
    in.read(text.data(), std::streamsize(text.size()));
    text.resize(size_t(in.gcount()));
    if (text.size() > kTablesMaxBytes) {
        t.state = "invalid";
        t.error = "the file is larger than 8 MB";
        return t;
    }
    json::Value root;
    std::string err;
    if (!json::Reader::Parse(text, root, err) || !root.IsObject()) {
        t.state = "invalid";
        t.error = err.empty() ? "not a JSON object" : err;
        return t;
    }
    const json::Value* schema = root.Get("schema");
    if (!schema || schema->Int(-1) != 1) {
        t.state = "unknown_schema";
        t.error = "the file's schema is not 1";
        return t;
    }
    if (const json::Value* v = root.Get("game_version"); v && v->IsString()) t.game_version = Clean(v->Text());
    const json::Value* stats = root.Get("stats");
    const json::Value* available = stats ? stats->Get("available") : nullptr;
    if (!stats || !stats->IsObject() || !available || !available->IsBool() || !available->boolean) {
        t.state = "stats_unavailable";
        if (stats && stats->IsObject())
            if (const json::Value* e = stats->Get("error"); e && e->IsString()) t.error = Clean(e->Text());
        return t;
    }
    const json::Value* sets = stats->Get("column_sets");
    const json::Value* views = stats->Get("views");
    if (!sets || !sets->IsArray() || !views || !views->IsArray()) {
        t.state = "invalid";
        t.error = "stats has no column_sets and views";
        return t;
    }
    std::vector<std::vector<TableColumn>> columns(sets->items.size());
    for (size_t s = 0; s < sets->items.size(); ++s) {
        const json::Value& set = sets->items[s];
        if (!set.IsArray()) continue;
        for (const json::Value& c : set.items) {
            if (columns[s].size() >= lb::kMaxColumns) break;
            const json::Value* prop = c.Get("prop");
            const json::Value* type = c.Get("type");
            const json::Value* attr = c.Get("attr");
            const json::Value* agg = c.Get("agg");
            if (!prop || !type || !attr || !agg) continue;
            TableColumn col;
            col.prop = uint32_t(prop->Int());
            col.type = int(type->Int());
            col.attr = uint32_t(attr->Int());
            col.agg = int(agg->Int());
            columns[s].push_back(col);
        }
    }
    for (const json::Value& v : views->items) {
        if (t.views.size() >= 4096) break;
        const json::Value* id = v.Get("id");
        const json::Value* set = v.Get("column_set");
        if (!id || !id->is_integer || id->integer < 0 || id->integer > 0xFFFFFFFFll) continue;
        TableView view;
        view.id = uint32_t(id->integer);
        if (set && set->is_integer && set->integer >= 0 && size_t(set->integer) < columns.size())
            view.columns = columns[size_t(set->integer)];
        // The game's dump has no view names today (online_tables_core.h: CHECK); a "name" is used when a later one has it.
        if (const json::Value* name = v.Get("name"); name && name->IsString()) view.name = Clean(name->Text());
        t.views[view.id] = std::move(view);
    }
    t.state = "ok";
    return t;
}

const TableView* FindView(const Tables& t, uint32_t id) {
    auto it = t.views.find(id);
    return it == t.views.end() ? nullptr : &it->second;
}

// ---- JSON pieces -----------------------------------------------------------------------------------------------------

void BeginDocument(JsonWriter& j, const RemoteArgs& a) {
    j.BeginObject();
    j.Field("ok", true);
    j.Field("command", RemoteGroupName(a.group));
    j.Field("schema", kSchema);
    j.Field("sub", RemoteSubName(a.sub));
}

void WriteServer(JsonWriter& j, const Session& s, int elapsed_ms) {
    j.Key("server").BeginObject();
    j.Key("address");
    if (s.st.address.empty()) j.Null(); else j.String(s.st.address);
    j.Field("state", s.st.state);
    j.Field("message", s.st.message);
    j.Field("detail", s.st.detail);
    j.Field("http_status", s.st.http_status);
    j.Field("err", s.st.err);
    j.Field("requests", s.Requests());
    j.Field("elapsed_ms", elapsed_ms);
    j.EndObject();
}

void WriteTables(JsonWriter& j, const Tables& t) {
    j.Key("tables").BeginObject();
    j.Field("state", t.state);
    j.Field("game_version", t.game_version);
    j.Field("views", int(t.views.size()));
    j.Field("error", t.error);
    j.EndObject();
}

void WriteViewInfo(JsonWriter& j, uint32_t id, const Tables& t) {
    const TableView* v = FindView(t, id);
    j.BeginObject();
    j.Field("id", id);
    j.Key("name");
    if (v && !v->name.empty()) j.String(v->name); else j.Null();
    j.Key("known");
    if (t.state == "ok") j.Bool(v != nullptr); else j.Null();
    j.Key("columns").BeginArray();
    if (v) {
        for (const TableColumn& c : v->columns) {
            j.BeginObject();
            j.Field("attr", c.attr);
            j.Field("prop", Hex32Text(c.prop));
            j.Field("type", TypeName(c.type));
            j.Field("agg", AggName(c.agg));
            j.EndObject();
        }
    }
    j.EndArray();
    j.EndObject();
}

double FloatOf(int64_t bits) {
    const uint32_t lo = uint32_t(bits);
    float f;
    std::memcpy(&f, &lo, sizeof(f));
    return double(f);
}

double DoubleOf(int64_t bits) {
    double d;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
}

// A column's value as a number: an int32 or a context is the low 32 bits as a signed number, an int64 as it is, a
// float the low 32 bits as a float, a double the 64 bits as a double (lb_protocol.h).
void WriteValue(JsonWriter& j, uint8_t type, int64_t bits) {
    switch (type) {
        case lb::kTypeFloat: j.Double(FloatOf(bits), 7); break;
        case lb::kTypeDouble: j.Double(DoubleOf(bits), 15); break;
        case lb::kTypeInt64: j.Int(bits); break;
        default: j.Int(int32_t(bits)); break;
    }
}

void WriteRow(JsonWriter& j, const lb::Row& r, bool mine, int64_t week_starts) {
    j.BeginObject();
    j.Field("owner", Hex64Text(r.owner));
    j.Field("mine", mine);
    j.Field("gamertag", Clean(r.gamertag));
    j.Field("rank", r.rank);
    // The rating is the column 0xFFFE the server ranks by; it carries its own type.
    uint8_t rating_type = lb::kTypeInt32;
    for (const lb::Value& v : r.columns)
        if (v.attr == lb::kAttrRating) rating_type = v.type;
    j.Key("rating");
    WriteValue(j, rating_type, r.rating);
    j.Field("updated", r.updated);
    if (week_starts > 0) j.Field("in_week", r.updated >= week_starts);
    j.Key("columns").BeginArray();
    for (const lb::Value& v : r.columns) {
        j.BeginObject();
        j.Field("attr", int(v.attr));
        j.Field("type", TypeName(v.type));
        j.Key("value");
        WriteValue(j, v.type, v.bits);
        j.EndObject();
    }
    j.EndArray();
    j.EndObject();
}

// ---- photos ----------------------------------------------------------------------------------------------------------

struct PhotoResult {
    uint32_t slot = 0;
    uint32_t file_id = 0;
    uint32_t size = 0;
    // "none" | "not_requested" | "saved" | "cached" | "unavailable" | "invalid" | "too_large" | "write_failed"
    std::string state = "none";
    std::string file;
    std::string error;
};

bool WriteAtomic(const fs::path& dir, const std::string& name, const std::vector<uint8_t>& bytes) {
    static std::mt19937 rng{std::random_device{}()};
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), ".%08x.tmp", unsigned(rng()));
    const fs::path final_path = dir / PathFromUtf8(name);
    const fs::path temp_path = dir / PathFromUtf8(name + suffix);
    std::error_code ec;
    {
        std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        out.close();
        if (!out) {
            fs::remove(temp_path, ec);
            return false;
        }
    }
    fs::remove(final_path, ec);
    fs::rename(temp_path, final_path, ec);
    if (ec) {
        fs::remove(temp_path, ec);
        return false;
    }
    return true;
}

bool CachedPhotoOk(const fs::path& dir, const std::string& name, uint32_t size) {
    std::error_code ec;
    const fs::path p = dir / PathFromUtf8(name);
    if (!fs::is_regular_file(p, ec) || fs::file_size(p, ec) != size) return false;
    std::ifstream f(p, std::ios::binary);
    unsigned char head[3] = {};
    f.read(reinterpret_cast<char*>(head), 3);
    return f.gcount() == 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF;
}

// Fetches one file and keeps it in `dir` when it is a JPEG. `announced` is the size the record gives (0 = not known).
PhotoResult FetchPhoto(Session& s, const std::string& dir_text, uint32_t slot, uint32_t file_id, uint32_t announced) {
    PhotoResult r;
    r.slot = slot;
    r.file_id = file_id;
    r.size = announced;
    if (file_id == 0 || (slot > 0 && announced == 0)) return r;  // no photo in that slot
    if (dir_text.empty()) {
        r.state = "not_requested";
        return r;
    }
    if (announced > rmr::kMaxPhotoBytes) {
        r.state = "too_large";
        return r;
    }
    const fs::path dir = PathFromUtf8(dir_text);
    if (announced) {
        const std::string name = PhotoFileName(file_id, announced);
        if (CachedPhotoOk(dir, name, announced)) {
            r.state = "cached";
            r.file = name;
            return r;
        }
    }
    if (!s.Usable()) {
        r.state = "unavailable";
        return r;
    }
    rmr::Request req;
    req.op = rmr::Op::kDownload;
    req.file_id = file_id;
    req.max_bytes = rmr::kMaxPhotoBytes;
    rmr::Response resp;
    if (!s.Rmr(req, resp) || resp.err != rmr::kOk || resp.data.empty()) {
        r.state = "unavailable";
        r.error = resp.err == rmr::kFileNotFound ? "the server no longer has the photo" : "";
        return r;
    }
    if (!LooksLikeJpeg(resp.data)) {
        r.state = "invalid";
        r.error = "what the server sent is not a JPEG";
        return r;
    }
    if (announced && resp.data.size() != announced) {  // the record said another size: the file is not that photo
        r.state = "invalid";
        r.error = "the photo is not the size the ride announced";
        return r;
    }
    r.size = uint32_t(resp.data.size());
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string name = PhotoFileName(file_id, resp.data.size());
    if (!WriteAtomic(dir, name, resp.data)) {
        r.state = "write_failed";
        r.error = "cannot write the photos folder";
        return r;
    }
    r.state = "saved";
    r.file = name;
    return r;
}

// ---- rides -----------------------------------------------------------------------------------------------------------

void WriteRide(JsonWriter& j, const rmr::Record& r, bool mine, const std::vector<PhotoResult>& photos) {
    j.BeginObject();
    j.Field("id", Hex32Text(r.id));
    j.Field("owner", Hex64Text(r.owner));
    j.Field("mine", mine);
    j.Field("gamertag", Clean(r.gamertag));
    j.Field("ratings", r.num_ratings);
    j.Key("average").Double(std::round(double(r.average) * 100.0) / 100.0, 6);
    j.Field("rank", r.rank);
    j.Field("ranked", r.rank > 0);
    j.Field("content", int(r.content));
    j.Field("created", r.created);
    j.Field("updated", r.updated);
    j.Field("car_config_bytes", r.file_count > 0 ? r.file_sizes[0] : 0u);
    j.Key("sale");
    if (r.userdata.size() >= rmr::kUdUsedBytes) {
        j.BeginObject();
        j.Field("vehicle", rmr::LoadBe32(&r.userdata[rmr::kUdVehicle]));
        j.Field("price", rmr::LoadBe32(&r.userdata[rmr::kUdPrice]));
        j.Field("purchases", rmr::LoadBe32(&r.userdata[rmr::kUdPurchases]));
        j.Field("pending_reward", rmr::LoadBe32(&r.userdata[rmr::kUdPendingReward]));
        j.Field("for_sale", rmr::LoadBe32(&r.userdata[rmr::kUdForSale]) != 0);
        j.EndObject();
    } else {
        j.Null();
    }
    j.Key("photos").BeginArray();
    for (const PhotoResult& p : photos) {
        j.BeginObject();
        j.Field("slot", p.slot);
        j.Field("file_id", Hex32Text(p.file_id));
        j.Field("size", p.size);
        j.Field("state", p.state);
        j.Key("file");
        if (p.file.empty()) j.Null(); else j.String(p.file);
        j.Field("error", p.error);
        j.EndObject();
    }
    j.EndArray();
    j.EndObject();
}

// The photos of one ride (slots 1 and 2 of its files; slot 0 is the car's configuration, which is nothing to show).
std::vector<PhotoResult> RidePhotos(Session& s, const RemoteArgs& a, const rmr::Record& r) {
    std::vector<PhotoResult> out;
    for (uint32_t slot = 1; slot <= kRmrMaxPhotoSlots; ++slot) {
        PhotoResult p;
        if (slot < r.file_count && r.file_ids[slot] != 0 && r.file_sizes[slot] != 0) {
            if (slot <= a.photo_slots) {
                p = FetchPhoto(s, a.photos_dir, slot, r.file_ids[slot], r.file_sizes[slot]);
            } else {
                p.slot = slot;
                p.file_id = r.file_ids[slot];
                p.size = r.file_sizes[slot];
                p.state = "not_requested";
            }
        } else {
            p.slot = slot;
        }
        out.push_back(std::move(p));
    }
    return out;
}

// ---- the token and the owner -----------------------------------------------------------------------------------------

bool ParseToken(const std::string& text, rmr::Token& out) {
    const std::string hex = Trim(text);
    if (hex.size() != out.size() * 2) return false;
    for (size_t i = 0; i < out.size(); ++i) {
        uint64_t v = 0;
        if (!ParseHexDigits(hex.substr(i * 2, 2), 2, v)) return false;
        out[i] = uint8_t(v);
    }
    return true;
}

struct Identity {
    bool have_token = false;
    rmr::Token token{};
    bool have_owner = false;
    uint64_t owner = 0;  // the first --owner, else the token's
};

// The token is never printed, logged or put in a document. It arrives on stdin (or from the check as `token_text`).
bool ReadIdentity(const RemoteArgs& a, const std::string* token_text, Identity& id, std::string& error) {
    if (a.token_stdin) {
        std::string text;
        if (token_text) {
            text = *token_text;
        } else {
            if (LARECOMP_ISATTY(LARECOMP_FILENO(stdin))) {
                error = "--token-stdin reads the token from a pipe, not from a console";
                return false;
            }
            char buf[128] = {};
            if (std::fgets(buf, sizeof(buf), stdin)) text = buf;  // one line: no need to wait for the pipe to close
        }
        if (!ParseToken(text, id.token)) {
            std::fill(text.begin(), text.end(), '\0');
            error = "the token on stdin is not 32 hexadecimal digits";
            return false;
        }
        std::fill(text.begin(), text.end(), '\0');
        id.have_token = true;
    }
    if (!a.owners.empty()) {
        id.have_owner = true;
        id.owner = a.owners[0];
    } else if (id.have_token) {
        id.have_owner = true;
        id.owner = rmr::OwnerKeyFromToken(id.token);
    }
    return true;
}

bool IsMine(const Identity& id, uint64_t owner) { return id.have_owner && id.owner == owner; }

// ---- the commands ----------------------------------------------------------------------------------------------------

int ElapsedMs(std::chrono::steady_clock::time_point since) {
    return int(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count());
}

int64_t NowUnix(const RemoteArgs& a) {
    return a.now > 0 ? a.now : int64_t(std::time(nullptr));
}

std::string RmrTable(const RemoteArgs& a, Session& s, std::chrono::steady_clock::time_point t0) {
    rmr::Request req;
    req.op = rmr::Op::kTableSize;
    req.min_ratings = a.min_ratings;
    rmr::Response resp;
    const bool ok = s.Rmr(req, resp) && s.Accept(resp.err);

    JsonWriter j(a.pretty);
    BeginDocument(j, a);
    WriteServer(j, s, ElapsedMs(t0));
    j.Field("min_ratings", a.min_ratings);
    j.Key("ranked");
    if (ok) j.Uint(resp.count); else j.Null();
    j.EndObject();
    return j.str();
}

std::string RmrTop(const RemoteArgs& a, Session& s, const Identity& id, std::chrono::steady_clock::time_point t0) {
    const uint32_t want = std::min<uint32_t>(a.count ? a.count : kDefaultTopCount, kRmrMaxPage);
    rmr::Request size;
    size.op = rmr::Op::kTableSize;
    size.min_ratings = a.min_ratings;
    rmr::Response size_resp;
    bool ok = s.Rmr(size, size_resp) && s.Accept(size_resp.err);

    rmr::Request page;
    page.op = rmr::Op::kByRank;
    page.start = a.start;
    page.min_ratings = a.min_ratings;
    page.count = want;
    rmr::Response page_resp;
    if (ok) ok = s.Rmr(page, page_resp) && s.Accept(page_resp.err);
    if (page_resp.records.size() > want) page_resp.records.resize(want);

    std::vector<std::vector<PhotoResult>> photos;
    if (ok)
        for (const rmr::Record& r : page_resp.records) photos.push_back(RidePhotos(s, a, r));

    JsonWriter j(a.pretty);
    BeginDocument(j, a);
    WriteServer(j, s, ElapsedMs(t0));
    j.Field("min_ratings", a.min_ratings);
    j.Field("start", a.start);
    j.Field("count", want);
    j.Key("total_ranked");
    if (ok) j.Uint(size_resp.count); else j.Null();
    j.Key("rides").BeginArray();
    if (ok)
        for (size_t k = 0; k < page_resp.records.size(); ++k)
            WriteRide(j, page_resp.records[k], IsMine(id, page_resp.records[k].owner), photos[k]);
    j.EndArray();
    j.EndObject();
    return j.str();
}

std::string RmrMine(const RemoteArgs& a, Session& s, const Identity& id, std::chrono::steady_clock::time_point t0) {
    rmr::Request req;
    req.op = rmr::Op::kByOwner;
    req.owner = id.owner;
    req.count = 1;
    rmr::Response resp;
    const bool ok = s.Rmr(req, resp) && s.Accept(resp.err);
    const rmr::Record* ride = nullptr;
    if (ok)
        for (const rmr::Record& r : resp.records)
            if (r.owner == id.owner) {
                ride = &r;
                break;
            }
    std::vector<PhotoResult> photos;
    if (ride) photos = RidePhotos(s, a, *ride);

    JsonWriter j(a.pretty);
    BeginDocument(j, a);
    WriteServer(j, s, ElapsedMs(t0));
    j.Field("owner", Hex64Text(id.owner));
    j.Key("found");
    if (ok) j.Bool(ride != nullptr); else j.Null();
    j.Key("ride");
    if (ride) WriteRide(j, *ride, true, photos); else j.Null();
    j.EndObject();
    return j.str();
}

std::string RmrPhoto(const RemoteArgs& a, Session& s, std::chrono::steady_clock::time_point t0) {
    std::vector<PhotoResult> results;
    for (uint32_t id : a.files) {
        results.push_back(FetchPhoto(s, a.out_dir, 0, id, 0));
    }
    JsonWriter j(a.pretty);
    BeginDocument(j, a);
    WriteServer(j, s, ElapsedMs(t0));
    j.Key("photos").BeginArray();
    for (const PhotoResult& p : results) {
        j.BeginObject();
        j.Field("file_id", Hex32Text(p.file_id));
        j.Field("size", p.size);
        j.Field("state", p.state);
        j.Key("file");
        if (p.file.empty()) j.Null(); else j.String(p.file);
        j.Field("error", p.error);
        j.EndObject();
    }
    j.EndArray();
    j.EndObject();
    return j.str();
}

std::string RmrRate(const RemoteArgs& a, Session& s, std::chrono::steady_clock::time_point t0) {
    rmr::Request req;
    req.op = rmr::Op::kRate;
    req.record_id = a.record;
    req.rating = uint8_t(a.rating);
    rmr::Response resp;
    const bool answered = s.Rmr(req, resp);
    const char* result = "not_sent";
    if (answered) {
        if (resp.err == rmr::kOk) result = "rated";
        else if (resp.err == rmr::kFileNotFound) result = "no_such_ride";
        else if (resp.err == rmr::kInvalidArgs) result = "refused";
        else {
            s.Refuse(resp.err);
            result = "error";
        }
    }
    JsonWriter j(a.pretty);
    BeginDocument(j, a);
    WriteServer(j, s, ElapsedMs(t0));
    j.Field("result", result);
    j.Field("record", Hex32Text(a.record));
    j.Field("rating", a.rating);
    j.Key("ratings");
    if (answered && resp.err == rmr::kOk) j.Uint(resp.num_ratings); else j.Null();
    j.Key("average");
    if (answered && resp.err == rmr::kOk) j.Double(std::round(double(resp.average) * 100.0) / 100.0, 6); else j.Null();
    j.EndObject();
    return j.str();
}

std::string LbViews(const RemoteArgs& a, const Tables& tables) {
    JsonWriter j(a.pretty);
    BeginDocument(j, a);
    j.Key("server").Null();
    WriteTables(j, tables);
    j.Key("views").BeginArray();
    for (const auto& [id, v] : tables.views) WriteViewInfo(j, id, tables);
    j.EndArray();
    j.EndObject();
    return j.str();
}

std::string LbSize(const RemoteArgs& a, Session& s, const Tables& tables, std::chrono::steady_clock::time_point t0) {
    struct Board {
        uint32_t view;
        bool ok;
        uint32_t total;
    };
    std::vector<Board> boards;
    for (uint32_t view : a.views) {
        if (!s.Usable() || s.st.state == "refused") {  // nothing more is asked once the server has failed or refused
            boards.push_back({view, false, 0});
            continue;
        }
        lb::Request req;
        req.op = lb::Op::kSize;
        req.view = view;
        lb::Response resp;
        const bool ok = s.Lb(req, resp) && s.Accept(resp.err);
        boards.push_back({view, ok, ok ? resp.total : 0});
    }
    JsonWriter j(a.pretty);
    BeginDocument(j, a);
    WriteServer(j, s, ElapsedMs(t0));
    WriteTables(j, tables);
    j.Key("boards").BeginArray();
    for (const Board& b : boards) {
        const TableView* v = FindView(tables, b.view);
        j.BeginObject();
        j.Field("view", b.view);
        j.Key("name");
        if (v && !v->name.empty()) j.String(v->name); else j.Null();
        j.Key("known");
        if (tables.state == "ok") j.Bool(v != nullptr); else j.Null();
        j.Key("total");
        if (b.ok) j.Uint(b.total); else j.Null();
        j.EndObject();
    }
    j.EndArray();
    j.EndObject();
    return j.str();
}

// A board page of the lb commands: the rows come back in the server's order (its rank order).
std::string LbRows(const RemoteArgs& a, Session& s, const Identity& id, const Tables& tables, lb::Request req,
                   const char* extra_key, std::chrono::steady_clock::time_point t0) {
    const uint32_t view = a.views.empty() ? 0 : a.views[0];
    req.view = view;
    lb::Response resp;
    const bool ok = s.Lb(req, resp) && s.Accept(resp.err);
    if (resp.rows.size() > kLbMaxRows) resp.rows.resize(kLbMaxRows);

    JsonWriter j(a.pretty);
    BeginDocument(j, a);
    WriteServer(j, s, ElapsedMs(t0));
    WriteTables(j, tables);
    j.Key("view");
    WriteViewInfo(j, view, tables);
    j.Key("total");
    if (ok) j.Uint(resp.total); else j.Null();
    if (extra_key) {  // "found": for `around`, whether the player has a row in the view
        bool found = false;
        for (const lb::Row& r : resp.rows) found = found || IsMine(id, r.owner);
        j.Key(extra_key);
        if (ok) j.Bool(found); else j.Null();
    }
    j.Key("rows").BeginArray();
    if (ok)
        for (const lb::Row& r : resp.rows) WriteRow(j, r, IsMine(id, r.owner), 0);
    j.EndArray();
    j.EndObject();
    return j.str();
}

// ---- the tournament --------------------------------------------------------------------------------------------------

struct TournamentInfo {
    uint32_t week = 0;
    uint32_t version = 0;
    uint32_t slot = 0;
    uint32_t restriction_type = 0;
    uint32_t restriction_data = 0;
    uint32_t race_key = 0;
    std::string description;
    std::vector<std::string> lines;
    int64_t starts = 0;
    int64_t ends = 0;
};

uint32_t Be32At(const std::vector<uint8_t>& b, size_t at) {
    return (uint32_t(b[at]) << 24) | (uint32_t(b[at + 1]) << 16) | (uint32_t(b[at + 2]) << 8) | uint32_t(b[at + 3]);
}

// The 1556-byte file the game keeps at the menu object +48 (tournament_schedule.h): the text of the first language
// entry, "xx:name", without its prefix; the game breaks a line on '\r' and shows '\n' as a space.
bool DecodeDefinition(const rmr::Response& resp, TournamentInfo& out, std::string& why) {
    if (resp.data.size() < tournament::kDefinitionSize) {
        why = "the tournament file is shorter than the game's";
        return false;
    }
    if (resp.count > kMaxWeek) {
        why = "the tournament week is not a week";
        return false;
    }
    out.week = resp.count;
    out.version = Be32At(resp.data, 0);
    out.slot = Be32At(resp.data, 4);
    out.restriction_type = Be32At(resp.data, 8);
    out.restriction_data = Be32At(resp.data, 12);
    out.race_key = Be32At(resp.data, 16);
    out.starts = tournament::kFirstMondayUnix + int64_t(out.week) * tournament::kWeekSeconds;
    out.ends = out.starts + tournament::kWeekSeconds;
    // Six entries of 256 bytes: take the first one with text.
    std::string text;
    for (int e = 0; e < 6 && text.empty(); ++e) {
        const char* raw = reinterpret_cast<const char*>(resp.data.data()) + 20 + e * 256;
        size_t len = 0;
        while (len < 256 && raw[len]) ++len;
        std::string entry(raw, len);
        if (entry.size() >= 3 && entry[2] == ':') entry.erase(0, 3);
        for (char& c : entry) {
            if (c == '\n') c = ' ';
            else if (c == '\r') c = '\n';
        }
        text = Clean(entry, true);
    }
    out.description = text;
    for (const std::string& line : SplitOn(text, '\n')) {
        const std::string t = Trim(line);
        if (!t.empty()) out.lines.push_back(t);
    }
    return true;
}

const char* RestrictionName(uint32_t type) {
    switch (type) {
        case 0: return "car_model";
        case 1: return "car_field";
        case 2: return "car_class";
    }
    return "unknown";
}

std::vector<std::string> RewardList(uint32_t mask) {
    std::vector<std::string> out;
    for (uint32_t i = 0; i < 3; ++i)
        if (mask & (1u << i)) out.push_back(tournament::RewardNames(1u << i));
    return out;
}

void WriteTournament(JsonWriter& j, const TournamentInfo& t, int64_t now) {
    j.BeginObject();
    j.Field("week", t.week);
    j.Field("version", t.version);
    j.Field("slot", t.slot);
    j.Field("race_key", Hex32Text(t.race_key));
    const std::string race = tournament::RaceName(t.race_key);
    j.Key("race");
    if (race.empty()) j.Null(); else j.String(race);
    j.Key("restriction").BeginObject();
    j.Field("type", RestrictionName(t.restriction_type));
    j.Field("type_id", t.restriction_type);
    j.Field("any_car", t.restriction_data == tournament::kAnyCar);
    j.Key("data");
    if (t.restriction_data == tournament::kAnyCar) j.Null(); else j.String(Hex32Text(t.restriction_data));
    j.EndObject();
    j.Field("description", t.description);
    j.Key("lines").BeginArray();
    for (const std::string& l : t.lines) j.String(l);
    j.EndArray();
    j.Key("prizes").BeginArray();
    for (const PrizeLine& p : PrizesFromDescription(t.lines)) {
        j.BeginObject();
        j.Field("place", p.place);
        j.Key("rewards").BeginArray();
        for (const std::string& r : p.rewards) j.String(r);
        j.EndArray();
        j.EndObject();
    }
    j.EndArray();
    j.Field("starts", t.starts);
    j.Field("ends", t.ends);
    j.Field("now", now);
    j.Field("active", now >= t.starts && now < t.ends);
    j.Field("seconds_left", std::max<int64_t>(0, t.ends - now));
    j.EndObject();
}

// The definition: the same request the game's tournament menu sends (it also tells the server which week is being
// played, as it does for the game).
bool FetchDefinition(Session& s, TournamentInfo& out) {
    rmr::Request req;
    req.op = rmr::Op::kTournament;
    rmr::Response resp;
    if (!s.Rmr(req, resp) || !s.Accept(resp.err)) return false;
    std::string why;
    if (!DecodeDefinition(resp, out, why)) return s.Bad(why);
    return true;
}

std::string TournamentCurrent(const RemoteArgs& a, Session& s, std::chrono::steady_clock::time_point t0) {
    TournamentInfo t;
    const bool ok = FetchDefinition(s, t);
    JsonWriter j(a.pretty);
    BeginDocument(j, a);
    WriteServer(j, s, ElapsedMs(t0));
    j.Key("tournament");
    if (ok) WriteTournament(j, t, NowUnix(a)); else j.Null();
    j.EndObject();
    return j.str();
}

std::string TournamentStandings(const RemoteArgs& a, Session& s, const Identity& id, const Tables& tables,
                                std::chrono::steady_clock::time_point t0) {
    TournamentInfo t;
    bool ok = FetchDefinition(s, t);
    const bool have_definition = ok;

    // Which stats view holds the slot's board. The server settles the prizes on the slot itself as the view
    // (tournament_awards.cpp); the game's header comment says it maps 351.. to views 382.. (tournament_schedule.h).
    // CHECK: unproven which one the game writes; ask both when it is not settled, the first one that has rows wins.
    std::vector<uint32_t> candidates;
    const char* basis = "slot";
    if (a.view_given) {
        candidates.push_back(a.views[0]);
        basis = "override";
    } else if (ok) {
        candidates.push_back(t.slot);
        if (t.slot >= 351 && t.slot <= 381) candidates.push_back(t.slot + 31);
    }
    const uint32_t want = std::min<uint32_t>(a.count ? a.count : kDefaultStandings, kLbMaxRows);
    uint32_t view = candidates.empty() ? 0 : candidates[0];
    lb::Response board;
    bool board_ok = false;
    for (size_t k = 0; ok && k < candidates.size(); ++k) {
        lb::Request req;
        req.op = lb::Op::kByRank;
        req.view = candidates[k];
        req.start = a.start;
        req.count = want;
        lb::Response resp;
        const bool good = s.Lb(req, resp) && s.Accept(resp.err);
        if (!good) {
            ok = false;
            break;
        }
        if (k == 0 || resp.total > 0) {
            view = candidates[k];
            board = std::move(resp);
            board_ok = true;
            if (k > 0 && !a.view_given) basis = "slot_plus_31";
            if (board.total > 0) break;
        }
    }
    if (board.rows.size() > kLbMaxRows) board.rows.resize(kLbMaxRows);

    // The player's own row, wherever it is.
    bool have_mine = false;
    lb::Row mine;
    if (board_ok && id.have_owner) {
        lb::Request req;
        req.op = lb::Op::kByOwners;
        req.view = view;
        req.owners.push_back(id.owner);
        req.count = 1;
        lb::Response resp;
        if (s.Lb(req, resp) && s.Accept(resp.err))
            for (const lb::Row& r : resp.rows)
                if (r.owner == id.owner) {
                    mine = r;
                    have_mine = true;
                    break;
                }
    }

    JsonWriter j(a.pretty);
    BeginDocument(j, a);
    WriteServer(j, s, ElapsedMs(t0));
    WriteTables(j, tables);
    j.Key("tournament");
    if (have_definition) WriteTournament(j, t, NowUnix(a)); else j.Null();
    j.Key("board");
    if (board_ok) {
        j.BeginObject();
        j.Key("view");
        WriteViewInfo(j, view, tables);
        j.Field("view_basis", basis);
        j.Key("candidates").BeginArray();
        for (uint32_t c : candidates) j.Uint(c);
        j.EndArray();
        j.Field("total", board.total);
        j.Field("start", a.start);
        j.Key("rows").BeginArray();
        for (const lb::Row& r : board.rows) WriteRow(j, r, IsMine(id, r.owner), t.starts);
        j.EndArray();
        j.Key("mine");
        if (have_mine) WriteRow(j, mine, true, t.starts); else j.Null();
        j.EndObject();
    } else {
        j.Null();
    }
    j.EndObject();
    return j.str();
}

// The default rotation of the next weeks: what the server does unless its tournament.txt says otherwise. No network.
std::string TournamentNext(const RemoteArgs& a) {
    const int64_t now = NowUnix(a);
    const tournament::Definition current = tournament::Current(now, {});
    JsonWriter j(a.pretty);
    BeginDocument(j, a);
    j.Key("server").Null();
    j.Field("source", "default_rotation");
    j.Field("server_can_change", true);
    j.Field("now", now);
    j.Key("weeks").BeginArray();
    for (uint32_t k = 1; k <= a.weeks; ++k) {
        const tournament::Definition d = tournament::Current(current.ends + int64_t(k - 1) * tournament::kWeekSeconds, {});
        j.BeginObject();
        j.Field("week", d.week);
        j.Field("slot", d.table_id);
        j.Field("race_key", Hex32Text(d.race_key));
        j.Field("race", d.name);
        j.Field("starts", d.starts);
        j.Field("ends", d.ends);
        j.Key("prizes").BeginObject();
        j.Key("first").BeginArray();
        for (const std::string& r : RewardList(d.prize_first)) j.String(r);
        j.EndArray();
        j.Key("podium").BeginArray();
        for (const std::string& r : RewardList(d.prize_podium)) j.String(r);
        j.EndArray();
        j.Field("podium_places", d.podium);
        j.EndObject();
        j.EndObject();
    }
    j.EndArray();
    j.EndObject();
    return j.str();
}

std::string TournamentRewards(const RemoteArgs& a, Session& s, std::chrono::steady_clock::time_point t0) {
    rmr::Request req;
    req.op = rmr::Op::kRewards;
    rmr::Response resp;
    bool ok = s.Rmr(req, resp) && s.Accept(resp.err);
    uint32_t mask = 0;
    if (ok && !resp.data.empty()) {
        // The game's own reading of the file (tournament_schedule.h): "001\0" and eight big-endian words, each non-zero
        // one sets the career stat tREW[i].
        if (resp.data.size() == tournament::kRewardsFileSize && resp.data[0] == '0' && resp.data[1] == '0' &&
            resp.data[2] == '1' && resp.data[3] == 0) {
            for (uint32_t i = 0; i < 8; ++i)
                if (Be32At(resp.data, 4 + 4 * i) != 0) mask |= 1u << i;
        } else {
            ok = s.Bad("the rewards file is not the game's");
        }
    } else if (ok && resp.count != 0) {
        ok = s.Bad("the server named prizes but sent no rewards file");
    }
    JsonWriter j(a.pretty);
    BeginDocument(j, a);
    WriteServer(j, s, ElapsedMs(t0));
    j.Key("rewards");
    if (ok) {
        j.BeginObject();
        j.Field("mask", mask);
        j.Key("items").BeginArray();
        for (uint32_t i = 0; i < 3; ++i) {
            j.BeginObject();
            j.Field("bit", i);
            j.Field("name", tournament::RewardNames(1u << i));
            j.Field("won", (mask & (1u << i)) != 0);
            j.EndObject();
        }
        j.EndArray();
        j.Field("other_bits", mask & ~tournament::kRewardsKnown);
        j.EndObject();
    } else {
        j.Null();
    }
    j.EndObject();
    return j.str();
}

}  // namespace

// ---- public helpers --------------------------------------------------------------------------------------------------

bool ParseHex32(const std::string& text, uint32_t& out) {
    uint64_t v = 0;
    if (!ParseHexDigits(text, 8, v)) return false;
    out = uint32_t(v);
    return true;
}

bool ParseHex64(const std::string& text, uint64_t& out) { return ParseHexDigits(text, 16, out); }

std::string Hex32Text(uint32_t v) {
    static const char kDigits[] = "0123456789abcdef";
    std::string s(8, '0');
    for (int i = 7; i >= 0; --i, v >>= 4) s[size_t(i)] = kDigits[v & 15];
    return s;
}

std::string Hex64Text(uint64_t v) {
    static const char kDigits[] = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i, v >>= 4) s[size_t(i)] = kDigits[v & 15];
    return s;
}

std::vector<PrizeLine> PrizesFromDescription(const std::vector<std::string>& lines) {
    std::set<std::string> known;
    for (uint32_t i = 0; i < 3; ++i) known.insert(tournament::RewardNames(1u << i));
    std::vector<PrizeLine> out;
    for (const std::string& line : lines) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        PrizeLine p;
        p.place = Trim(line.substr(0, colon));
        if (p.place.empty() || p.place.size() > 12 || p.place[0] < '0' || p.place[0] > '9') continue;
        std::string rest = Trim(line.substr(colon + 1));
        while (!rest.empty() && rest.back() == '.') rest.pop_back();
        for (const std::string& name : SplitOn(rest, ',')) {
            const std::string n = Trim(name);
            if (known.count(n)) p.rewards.push_back(n);
        }
        if (!p.rewards.empty()) out.push_back(std::move(p));
    }
    return out;
}

bool LooksLikeJpeg(const std::vector<uint8_t>& b) {
    return b.size() >= 4 && b.size() <= rmr::kMaxPhotoBytes && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF;
}

std::string PhotoFileName(uint32_t file_id, size_t size) { return Hex32Text(file_id) + "-" + std::to_string(size) + ".jpg"; }

bool IsRemoteCommand(const std::string& word) { return word == "rmr" || word == "lb" || word == "tournament"; }

const char* RemoteGroupName(RemoteGroup g) {
    switch (g) {
        case RemoteGroup::kRmr: return "rmr";
        case RemoteGroup::kLb: return "lb";
        case RemoteGroup::kTournament: return "tournament";
        case RemoteGroup::kNone: break;
    }
    return "none";
}

const char* RemoteSubName(RemoteSub s) {
    for (const SubWord& w : kSubs)
        if (w.sub == s) return w.word;
    return "none";
}

const char* RemoteUsageText() {
    return
        "Rate My Ride, leaderboards and the weekly tournament (the server on TCP 31002; the game's own requests):\n"
        "\n"
        "  larecomp_online_tool rmr table|top|mine|photo|rate [options]\n"
        "  larecomp_online_tool lb views|size|top|around|owners [options]\n"
        "  larecomp_online_tool tournament current|standings|next|rewards [options]\n"
        "\n"
        "where the server is (the game's rule: --server is rmr_server, else the host of --relay, on --port):\n"
        "  --server H[:P]           the Rate My Ride + leaderboards server\n"
        "  --relay H[:P]            the relay the game plays on: its host is the server's host\n"
        "  --port N                 the server's port when none is given (31002)\n"
        "  --connect-timeout-ms N   200 to 60000, default 3000 (the game's)\n"
        "  --io-timeout-ms N        200 to 120000, default 15000 (the game's)\n"
        "  --deadline-ms N          1000 to 600000, default 60000: no new request starts after this long\n"
        "  --pretty                 indented output\n"
        "\n"
        "  rmr table [--min-ratings N]                     how many rides are ranked\n"
        "  rmr top [--start N] [--count N] [--min-ratings N] [--owner HEX] [--photos DIR [--photo-slots 1|2]]\n"
        "  rmr mine --owner HEX | --token-stdin [--photos DIR [--photo-slots 1|2]]\n"
        "  rmr photo --file ID[,ID] --out DIR              fetch photos (JPEG, kept as <id>-<size>.jpg)\n"
        "  rmr rate --record ID --rating 1..10 --confirm --token-stdin   CHANGES the player's rating of a ride\n"
        "  lb views [--tables FILE]                        the stats views the game's online_tables.json lists\n"
        "  lb size --view N[,N] [--tables FILE]            rows in each view\n"
        "  lb top --view N [--start N] [--count N] [--owner HEX] [--tables FILE]\n"
        "  lb around --view N --owner HEX | --token-stdin [--span N] [--tables FILE]\n"
        "  lb owners --view N --owner HEX[,HEX] [--tables FILE]\n"
        "  tournament current                              this week's race, car rule, prizes, start and end\n"
        "  tournament standings [--start N] [--count N] [--owner HEX | --token-stdin] [--view N] [--tables FILE]\n"
        "  tournament next [--weeks N]                     the default rotation of the next weeks (no server)\n"
        "  tournament rewards --token-stdin                the player's prizes\n"
        "\n"
        "--token-stdin reads the player's 32-digit token from stdin (never from the command line). --owner is the\n"
        "public owner key (16 hex digits).\n";
}

// ---- command line ----------------------------------------------------------------------------------------------------

RemoteParse ParseRemoteArgs(const std::vector<std::string>& v) {
    RemoteParse r;
    RemoteArgs& a = r.args;
    auto fail = [&](std::string msg) {
        r.ok = false;
        r.error = std::move(msg);
        return r;
    };
    if (v.empty() || !IsRemoteCommand(v[0])) return fail("not a rmr, lb or tournament command");
    a.group = v[0] == "rmr" ? RemoteGroup::kRmr : (v[0] == "lb" ? RemoteGroup::kLb : RemoteGroup::kTournament);
    if (v.size() < 2 || v[1].rfind("--", 0) == 0) return fail(v[0] + " needs a command (try --help)");
    for (const SubWord& w : kSubs)
        if (w.group == a.group && v[1] == w.word) a.sub = w.sub;
    if (a.sub == RemoteSub::kNone) return fail("unknown " + v[0] + " command '" + v[1] + "'");
    const std::string cmd = v[0] + " " + v[1];
    const std::set<std::string> allowed = AllowedOptions(a.sub);

    std::set<std::string> given;
    for (size_t i = 2; i < v.size(); ++i) {
        const std::string& tok = v[i];
        if (tok.rfind("--", 0) != 0) return fail("unexpected '" + tok + "'");
        std::string name = tok, value;
        bool has_value = false;
        if (const size_t eq = tok.find('='); eq != std::string::npos) {
            name = tok.substr(0, eq);
            value = tok.substr(eq + 1);
            has_value = true;
        }
        const OptInfo* info = FindOption(name);
        if (!info) return fail("unknown option '" + name + "'");
        if (!allowed.count(name)) return fail("option '" + name + "' does not belong to " + cmd);
        if (info->flag) {
            if (has_value) return fail(name + " takes no value");
        } else if (!has_value) {
            if (i + 1 >= v.size() || v[i + 1].rfind("--", 0) == 0) return fail(name + " needs a value");
            value = v[++i];
        }
        if (!given.insert(name).second) return fail(name + " is given twice");

        long long n = 0;
        if (name == "--pretty") a.pretty = true;
        else if (name == "--token-stdin") a.token_stdin = true;
        else if (name == "--confirm") a.confirm = true;
        else if (name == "--server" || name == "--relay") {
            std::string host;
            uint16_t port = 0;
            bool has_port = false;
            if (!ParseEndpoint(value, host, port, has_port)) return fail(name + " is a host name or address, optionally :port");
            (name == "--server" ? a.server : a.relay) = value;
        } else if (name == "--port") {
            if (!ParseNumber(value, 1, 65535, n)) return fail("--port is a number from 1 to 65535");
            a.port = int(n);
        } else if (name == "--connect-timeout-ms") {
            if (!ParseNumber(value, 200, 60000, n)) return fail("--connect-timeout-ms is a number from 200 to 60000");
            a.connect_timeout_ms = int(n);
        } else if (name == "--io-timeout-ms") {
            if (!ParseNumber(value, 200, 120000, n)) return fail("--io-timeout-ms is a number from 200 to 120000");
            a.io_timeout_ms = int(n);
        } else if (name == "--deadline-ms") {
            if (!ParseNumber(value, 1000, 600000, n)) return fail("--deadline-ms is a number from 1000 to 600000");
            a.deadline_ms = int(n);
        } else if (name == "--start") {
            if (!ParseNumber(value, 1, 1000000, n)) return fail("--start is a rank from 1 to 1000000");
            a.start = uint32_t(n);
        } else if (name == "--count") {
            const bool lbish = a.group != RemoteGroup::kRmr;
            if (!ParseNumber(value, 1, lbish ? kLbMaxRows : kRmrMaxPage, n))
                return fail(std::string("--count is a number from 1 to ") + (lbish ? "100" : "50"));
            a.count = uint32_t(n);
        } else if (name == "--span") {
            if (!ParseNumber(value, 1, 49, n)) return fail("--span is a number from 1 to 49");
            a.span = uint32_t(n);
        } else if (name == "--min-ratings") {
            if (!ParseNumber(value, 1, 1000, n)) return fail("--min-ratings is a number from 1 to 1000");
            a.min_ratings = uint32_t(n);
        } else if (name == "--weeks") {
            if (!ParseNumber(value, 1, 52, n)) return fail("--weeks is a number from 1 to 52");
            a.weeks = uint32_t(n);
        } else if (name == "--photo-slots") {
            if (!ParseNumber(value, 1, kRmrMaxPhotoSlots, n)) return fail("--photo-slots is 1 or 2");
            a.photo_slots = uint32_t(n);
        } else if (name == "--owner") {
            for (const std::string& part : SplitOn(value, ',')) {
                uint64_t owner = 0;
                if (!ParseHex64(part, owner) || owner == 0) return fail("--owner is 16 hexadecimal digits (a comma separates several)");
                if (a.owners.size() >= lb::kMaxOwners) return fail("--owner takes at most 100 owners");
                a.owners.push_back(owner);
            }
        } else if (name == "--view") {
            for (const std::string& part : SplitOn(value, ',')) {
                if (!ParseNumber(part, 1, kMaxViewId, n)) return fail("--view is a stats view number from 1 to 65535 (a comma separates several)");
                if (a.views.size() >= kLbMaxViewsPerCall) return fail("--view takes at most 32 views");
                a.views.push_back(uint32_t(n));
            }
            a.view_given = true;
        } else if (name == "--file") {
            for (const std::string& part : SplitOn(value, ',')) {
                uint32_t id = 0;
                if (!ParseHex32(part, id) || !rmr::IsRmrId(id)) return fail("--file is a Rate My Ride file id (8 hexadecimal digits, from a ride's photos)");
                if (a.files.size() >= 16) return fail("--file takes at most 16 files");
                a.files.push_back(id);
            }
        } else if (name == "--record") {
            if (!ParseHex32(value, a.record) || !rmr::IsRmrId(a.record)) return fail("--record is a ride id (8 hexadecimal digits)");
        } else if (name == "--rating") {
            if (!ParseNumber(value, 1, 10, n)) return fail("--rating is a number from 1 to 10");
            a.rating = int(n);
        } else if (name == "--photos") {
            if (value.empty()) return fail("--photos needs a folder");
            a.photos_dir = value;
        } else if (name == "--out") {
            if (value.empty()) return fail("--out needs a folder");
            a.out_dir = value;
        } else if (name == "--tables") {
            if (value.empty()) return fail("--tables needs a file");
            a.tables = value;
        } else if (name == "--now") {
            if (!ParseNumber(value, 1, 4102444800LL, n)) return fail("--now is a Unix time in seconds");
            a.now = n;
        }
    }

    // What each command needs.
    auto need = [&](bool have, const char* what) { return have ? std::string() : cmd + " needs " + what; };
    std::string missing;
    switch (a.sub) {
        case RemoteSub::kRmrMine: missing = need(!a.owners.empty() || a.token_stdin, "--owner or --token-stdin"); break;
        case RemoteSub::kRmrPhoto:
            missing = need(!a.files.empty(), "--file");
            if (missing.empty()) missing = need(!a.out_dir.empty(), "--out");
            break;
        case RemoteSub::kRmrRate:
            missing = need(a.record != 0, "--record");
            if (missing.empty()) missing = need(a.rating != 0, "--rating");
            if (missing.empty()) missing = need(a.token_stdin, "--token-stdin (the player's token)");
            if (missing.empty())
                missing = need(a.confirm, "--confirm (it changes the player's rating of that ride on the server)");
            break;
        case RemoteSub::kLbSize: missing = need(!a.views.empty(), "--view"); break;
        case RemoteSub::kLbTop:
        case RemoteSub::kLbOwners: missing = need(!a.views.empty(), "--view"); break;
        case RemoteSub::kLbAround:
            missing = need(!a.views.empty(), "--view");
            if (missing.empty()) missing = need(!a.owners.empty() || a.token_stdin, "--owner or --token-stdin");
            break;
        case RemoteSub::kTournamentRewards: missing = need(a.token_stdin, "--token-stdin (the player's token)"); break;
        default: break;
    }
    if (!missing.empty()) return fail(missing);
    if (a.sub == RemoteSub::kLbOwners && a.owners.empty()) return fail(cmd + " needs --owner");
    const bool single_view = a.sub == RemoteSub::kLbTop || a.sub == RemoteSub::kLbAround || a.sub == RemoteSub::kLbOwners ||
                             a.sub == RemoteSub::kTournamentStandings;
    if (single_view && a.views.size() > 1) return fail(cmd + " takes one --view");
    const bool single_owner = a.sub == RemoteSub::kLbAround || a.sub == RemoteSub::kRmrMine;
    if (single_owner && a.owners.size() > 1) return fail(cmd + " takes one --owner");
    if (given.count("--photo-slots") && !given.count("--photos")) return fail("--photo-slots needs --photos");
    r.ok = true;
    return r;
}

// ---- running ---------------------------------------------------------------------------------------------------------

std::string RunRemote(const RemoteArgs& a, int* exit_code, const std::string* token_text) {
    const auto t0 = std::chrono::steady_clock::now();
    if (exit_code) *exit_code = 0;

    Identity id;
    std::string error;
    if (!ReadIdentity(a, token_text, id, error)) {
        if (exit_code) *exit_code = 2;
        return RenderError(RemoteGroupName(a.group), "token", error, a.pretty);
    }

    Session s(a);
    // The token travels in clear over plain HTTP (the game's own protocol): it is sent only by the commands that need
    // it. The others use it, when it is given, to know the player's public owner key and send zeros.
    if (id.have_token && (a.sub == RemoteSub::kRmrRate || a.sub == RemoteSub::kTournamentRewards)) s.token = id.token;
    const bool wants_tables = !a.tables.empty();
    const Tables tables = wants_tables ? LoadTables(a.tables) : Tables{};

    switch (a.sub) {
        case RemoteSub::kRmrTable: return RmrTable(a, s, t0);
        case RemoteSub::kRmrTop: return RmrTop(a, s, id, t0);
        case RemoteSub::kRmrMine: return RmrMine(a, s, id, t0);
        case RemoteSub::kRmrPhoto: return RmrPhoto(a, s, t0);
        case RemoteSub::kRmrRate: return RmrRate(a, s, t0);
        case RemoteSub::kLbViews: return LbViews(a, tables);
        case RemoteSub::kLbSize: return LbSize(a, s, tables, t0);
        case RemoteSub::kLbTop: {
            lb::Request req;
            req.op = lb::Op::kByRank;
            req.start = a.start;
            req.count = std::min<uint32_t>(a.count ? a.count : kDefaultTopCount, kLbMaxRows);
            return LbRows(a, s, id, tables, req, nullptr, t0);
        }
        case RemoteSub::kLbAround: {
            lb::Request req;
            req.op = lb::Op::kByPivot;
            req.pivot = id.owner;
            req.count = 2 * a.span + 1;
            return LbRows(a, s, id, tables, req, "found", t0);
        }
        case RemoteSub::kLbOwners: {
            lb::Request req;
            req.op = lb::Op::kByOwners;
            req.owners = a.owners;
            req.count = uint32_t(a.owners.size());
            return LbRows(a, s, id, tables, req, nullptr, t0);
        }
        case RemoteSub::kTournamentCurrent: return TournamentCurrent(a, s, t0);
        case RemoteSub::kTournamentStandings: return TournamentStandings(a, s, id, tables, t0);
        case RemoteSub::kTournamentNext: return TournamentNext(a);
        case RemoteSub::kTournamentRewards: return TournamentRewards(a, s, t0);
        case RemoteSub::kNone: break;
    }
    if (exit_code) *exit_code = 2;
    return RenderError(RemoteGroupName(a.group), "usage", "no command", a.pretty);
}

}  // namespace larecomp::online_tool
