#include "identity_commands.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <set>

#include "game_guard.h"
#include "json_writer.h"
#include "online_tool.h"
#include "../identity/identity_core.h"
#include "../identity/identity_net.h"
#include "../identity/identity_rename.h"
#include "../rate_my_ride/rmr_net.h"
#include "../../../profile/larecomp_profile_store.h"

namespace larecomp::online_tool {
namespace {

namespace fs = std::filesystem;
namespace core = identity::core;
namespace net = identity::net;

constexpr int kMaxSecretBytes = 1024;      // a password or a recovery code line
constexpr int64_t kMaxBackupFileBytes = 1024 * 1024;

fs::path PathFromUtf8(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

// A path as UTF-8 with the OS's own separators (so "C:/x" and "C:\x" are not mixed in one document).
std::string Utf8(const fs::path& p) {
    fs::path q = p;
    q.make_preferred();
    const std::u8string u = q.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

bool IsNumber(const std::string& s, int lo, int hi, int& out) {
    if (s.empty() || s.size() > 7) return false;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    if (v < lo || v > hi) return false;
    out = v;
    return true;
}

std::string Hex16(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(v));
    return buf;
}

// Equal without telling where they differ.
bool SameText(const std::string& a, const std::string& b) {
    unsigned diff = unsigned(a.size() ^ b.size());
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) diff |= unsigned(uint8_t(a[i]) ^ uint8_t(b[i]));
    return diff == 0;
}

// ---- the command line ------------------------------------------------------------------------------------------

struct IdArgs {
    std::string action;  // show check backup restore recovery_show recovery_confirm recovery_remind recovery_check recover cancel rename
    bool pretty = false;
    std::string profile, saves, gamertag, identity_dir, server, game_exe, config, file, nick;
    bool claim = false, refresh = false, password_stdin = false, code_stdin = false, overwrite = false,
         replace = false, never = false;
    int days = 0;
    net::Options opt;  // --timeout-ms, --master-timeout-ms, --clock-offset-sec
};

struct IdParseResult {
    bool ok = false;
    IdArgs args;
    std::string error;
};

const std::set<std::string>& ValueOptions() {
    static const std::set<std::string> s = {"--profile", "--saves",      "--gamertag",          "--identity-dir",
                                            "--server",  "--game-exe",   "--config",            "--file",
                                            "--nick",    "--days",       "--timeout-ms",        "--master-timeout-ms",
                                            "--clock-offset-sec"};
    return s;
}

const std::set<std::string>& FlagOptions() {
    static const std::set<std::string> s = {"--pretty",        "--claim",     "--refresh", "--password-stdin",
                                            "--code-stdin",    "--overwrite", "--replace", "--never"};
    return s;
}

// What each action accepts besides the profile options.
const std::map<std::string, std::set<std::string>>& ActionOptions() {
    static const std::set<std::string> net_opts = {"--timeout-ms", "--master-timeout-ms", "--clock-offset-sec"};
    auto with = [&](std::set<std::string> own, bool with_net) {
        if (with_net) own.insert(net_opts.begin(), net_opts.end());
        return own;
    };
    static const std::map<std::string, std::set<std::string>> m = {
        {"show", {}},
        {"check", with({"--server", "--claim", "--refresh", "--game-exe"}, true)},
        {"backup", {"--file", "--password-stdin", "--overwrite"}},
        {"restore", {"--file", "--password-stdin", "--replace", "--game-exe"}},
        {"recovery_show", {}},
        {"recovery_confirm", {"--game-exe", "--code-stdin"}},
        {"recovery_remind", {"--game-exe", "--days", "--never"}},
        {"recovery_check", {"--code-stdin"}},
        {"recover", with({"--server", "--code-stdin"}, true)},
        {"cancel", with({"--server", "--game-exe"}, true)},
        {"rename", with({"--nick", "--config", "--game-exe", "--server", "--claim"}, true)},
    };
    return m;
}

bool ProfileOption(const std::string& name) {
    return name == "--profile" || name == "--saves" || name == "--gamertag" || name == "--identity-dir" ||
           name == "--pretty";
}

IdParseResult ParseIdentityArgs(const std::vector<std::string>& v) {
    IdParseResult r;
    IdArgs& a = r.args;
    auto fail = [&](std::string msg) {
        r.ok = false;
        r.error = std::move(msg);
        return r;
    };
    std::vector<std::string> words;
    std::map<std::string, std::string> opts;  // name -> value ("" for flags)
    for (size_t i = 0; i < v.size(); ++i) {
        const std::string& tok = v[i];
        if (tok.rfind("--", 0) != 0) {
            words.push_back(tok);
            continue;
        }
        std::string name = tok, value;
        bool has_value = false;
        if (const size_t eq = tok.find('='); eq != std::string::npos) {
            name = tok.substr(0, eq);
            value = tok.substr(eq + 1);
            has_value = true;
        }
        if (name == "--password" || name == "--code" || name == "--passphrase")
            return fail("a password or a recovery code is never taken from the command line: use " + name +
                        "-stdin and write it on stdin");
        if (FlagOptions().count(name)) {
            if (has_value) return fail(name + " takes no value");
        } else if (ValueOptions().count(name)) {
            if (!has_value) {
                if (i + 1 >= v.size() || v[i + 1].rfind("--", 0) == 0) return fail(name + " needs a value");
                value = v[++i];
            }
        } else {
            return fail("unknown option '" + name + "'");
        }
        if (!opts.emplace(name, value).second) return fail(name + " is given twice");
    }

    if (words.empty()) return fail("no identity command (show, check, backup, restore, recovery, recover, cancel, rename)");
    std::string action = words[0];
    if (action == "recovery") {
        if (words.size() < 2) return fail("recovery needs show, confirm, remind or check");
        if (words[1] != "show" && words[1] != "confirm" && words[1] != "remind" && words[1] != "check")
            return fail("unknown recovery command '" + words[1] + "'");
        action += "_" + words[1];
        words.erase(words.begin() + 1);
    }
    if (words.size() > 1) return fail("unexpected '" + words[1] + "'");
    const auto& allowed_by_action = ActionOptions();
    const auto spec = allowed_by_action.find(action);
    if (spec == allowed_by_action.end()) return fail("unknown identity command '" + action + "'");
    a.action = action;
    for (const auto& [name, value] : opts) {
        if (!ProfileOption(name) && !spec->second.count(name))
            return fail("the option " + name + " does not belong to '" + action + "'");
        if (name == "--pretty") a.pretty = true;
        else if (name == "--profile") a.profile = value;
        else if (name == "--saves") a.saves = value;
        else if (name == "--gamertag") a.gamertag = value;
        else if (name == "--identity-dir") a.identity_dir = value;
        else if (name == "--server") a.server = value;
        else if (name == "--game-exe") a.game_exe = value;
        else if (name == "--config") a.config = value;
        else if (name == "--file") a.file = value;
        else if (name == "--nick") a.nick = value;
        else if (name == "--claim") a.claim = true;
        else if (name == "--refresh") a.refresh = true;
        else if (name == "--password-stdin") a.password_stdin = true;
        else if (name == "--code-stdin") a.code_stdin = true;
        else if (name == "--overwrite") a.overwrite = true;
        else if (name == "--replace") a.replace = true;
        else if (name == "--never") a.never = true;
        else if (name == "--days") {
            if (!IsNumber(value, 1, 3650, a.days)) return fail("--days is a number from 1 to 3650");
        } else if (name == "--timeout-ms") {
            if (!IsNumber(value, 50, 10000, a.opt.udp_timeout_ms)) return fail("--timeout-ms is a number from 50 to 10000");
        } else if (name == "--master-timeout-ms") {
            if (!IsNumber(value, 200, 60000, a.opt.master_io_ms))
                return fail("--master-timeout-ms is a number from 200 to 60000");
        } else if (name == "--clock-offset-sec") {
            // Only for checking the master's clock window; signed.
            char* end = nullptr;
            const long long off = std::strtoll(value.c_str(), &end, 10);
            if (value.empty() || *end != '\0') return fail("--clock-offset-sec is a number of seconds");
            a.opt.clock_offset_sec = off;
        }
    }

    auto need = [&](bool have, const char* option) -> bool {
        if (!have) r.error = std::string(option) + " is required for '" + a.action + "'";
        return have;
    };
    if (a.profile.empty() && !(!a.saves.empty() && !a.gamertag.empty()))
        return fail("name the profile: --profile <id>, or --saves <folder> with --gamertag <name>");
    const bool needs_gamertag = a.action == "check" || a.action == "recover" || a.action == "cancel" || a.action == "rename";
    if (needs_gamertag && a.gamertag.empty() && a.profile.rfind("name_", 0) != 0)
        return fail("--gamertag is required for '" + a.action + "' (the nick that is proven or changed)");
    if (a.action == "check") {
        if (!need(!a.server.empty(), "--server")) return r;
        if (a.claim && a.refresh) return fail("give --claim or --refresh, not both");
        if ((a.claim || a.refresh) && !need(!a.game_exe.empty(), "--game-exe")) return r;
    } else if (a.action == "backup") {
        if (!need(!a.file.empty(), "--file") || !need(a.password_stdin, "--password-stdin")) return r;
    } else if (a.action == "restore") {
        if (!need(!a.file.empty(), "--file") || !need(a.password_stdin, "--password-stdin") ||
            !need(!a.game_exe.empty(), "--game-exe"))
            return r;
    } else if (a.action == "recovery_confirm") {
        if (!need(!a.game_exe.empty(), "--game-exe")) return r;
    } else if (a.action == "recovery_remind") {
        if (!need(!a.game_exe.empty(), "--game-exe")) return r;
        if (a.never == (a.days != 0)) return fail("give --days <n> or --never");
    } else if (a.action == "recovery_check") {
        if (!need(a.code_stdin, "--code-stdin")) return r;
    } else if (a.action == "recover") {
        if (!need(!a.server.empty(), "--server") || !need(a.code_stdin, "--code-stdin")) return r;
    } else if (a.action == "cancel") {
        if (!need(!a.server.empty(), "--server") || !need(!a.game_exe.empty(), "--game-exe")) return r;
    } else if (a.action == "rename") {
        if (!need(!a.nick.empty(), "--nick") || !need(!a.saves.empty(), "--saves") ||
            !need(!a.config.empty(), "--config") || !need(!a.game_exe.empty(), "--game-exe"))
            return r;
        if (!a.server.empty() && !a.claim) return fail("--server is for --claim: give both or neither");
        if (a.claim && a.server.empty()) return fail("--claim needs --server (the relay whose nick server registers the name)");
    }
    r.ok = true;
    return r;
}

// ---- documents -------------------------------------------------------------------------------------------------

using Extra = std::function<void(JsonWriter&)>;

void Begin(JsonWriter& j, const IdArgs& a, bool ok) {
    j.BeginObject();
    j.Field("ok", ok);
    j.Field("command", "identity");
    j.Field("action", a.action);
    j.Field("schema", kSchema);
}

IdentityResult Done(JsonWriter& j) {
    j.EndObject();
    return {j.str(), 0};
}

// Exit code 1: the command could not be done; `extra` adds fields to the "error" object.
IdentityResult Fail(const IdArgs& a, const std::string& code, const std::string& message, const Extra& extra = {}) {
    JsonWriter j(a.pretty);
    Begin(j, a, false);
    j.Key("error").BeginObject();
    j.Field("code", code);
    j.Field("message", message);
    if (extra) extra(j);
    j.EndObject();
    j.EndObject();
    return {j.str(), 1};
}

// ---- the profile -----------------------------------------------------------------------------------------------

struct Profile {
    std::string id;       // the profile's id as the game names it
    std::string file_id;  // the id its key file is named by: differs only for a key still filed by name
    std::string gamertag;
    fs::path dir;         // the identity folder
};

bool PlainName(const std::string& s) {
    if (s.empty() || s.size() > identity::kNickMax) return false;
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F || std::strchr("<>:\"/\\|?*", c) != nullptr) return false;
    }
    return s != "." && s != "..";
}

IdentityResult ResolveProfile(const IdArgs& a, Profile& p) {
    p.dir = a.identity_dir.empty() ? core::DefaultIdentityDir() : PathFromUtf8(a.identity_dir);
    p.gamertag = a.gamertag;
    if (!p.gamertag.empty() && !PlainName(p.gamertag))
        return Fail(a, "bad_gamertag", "the gamertag is empty, longer than 15 bytes or holds characters a folder name cannot");
    if (!a.profile.empty()) {
        if (!core::ValidProfileId(a.profile))
            return Fail(a, "bad_profile", "a profile id is 16 hex digits, or name_ and the gamertag");
        p.id = a.profile;
        if (p.id.size() == 16)
            for (char& c : p.id) c = (c >= 'a' && c <= 'f') ? char(c - 'a' + 'A') : c;
        if (p.gamertag.empty() && p.id.rfind("name_", 0) == 0) p.gamertag = p.id.substr(5);
    } else {
        uint64_t xuid = 0;
        if (!PlainName(p.gamertag)) return Fail(a, "bad_gamertag", "name the profile by a gamertag that is a plain folder name");
        if (!core::ReadProfileXuid(PathFromUtf8(a.saves), p.gamertag, xuid))
            return Fail(a, "profile_not_found",
                        "no profile.xuid in the saves folder of that gamertag: the game has not made this profile yet");
        p.id = core::ProfileId(xuid, p.gamertag);
    }
    p.file_id = p.id;
    // An SDK without per-profile ids filed the key by name; the game moves it at its next start.
    std::error_code ec;
    if (p.id.rfind("name_", 0) != 0 && !p.gamertag.empty() && !fs::exists(core::KeyPath(p.dir, p.id), ec) &&
        fs::exists(core::KeyPath(p.dir, "name_" + p.gamertag), ec))
        p.file_id = "name_" + p.gamertag;
    return {};
}

void ProfileBlock(JsonWriter& j, const Profile& p) {
    j.Key("profile").BeginObject();
    j.Field("id", p.id);
    j.Key("gamertag");
    if (p.gamertag.empty()) j.Null(); else j.String(p.gamertag);
    j.Field("identity_dir", Utf8(p.dir));
    j.Field("key_file", Utf8(core::KeyPath(p.dir, p.file_id)));
    j.Field("filed_by", p.file_id == p.id ? "id" : "name");
    j.EndObject();
}

// kLoaded, or the failure document.
bool LoadOrFail(const IdArgs& a, const Profile& p, core::KeyRecord& rec, IdentityResult& fail) {
    if (core::LoadKey(p.dir, p.file_id, rec) == core::Load::kLoaded) return true;
    std::error_code ec;
    if (fs::exists(core::KeyPath(p.dir, p.file_id), ec))
        fail = Fail(a, "key_unreadable",
                    "the key file cannot be read by this user on this PC, or is damaged: restore a backup of it");
    else
        fail = Fail(a, "no_key", "this profile has no key yet: start the game once with it");
    return false;
}

// ---- the game must be closed -----------------------------------------------------------------------------------

class Guard {
public:
    explicit Guard(std::string exe) : exe_(std::move(exe)) {}
    // True when the game is not running (and nothing that might be it could not be inspected).
    bool Closed() {
        look_ = LookForGame(exe_);
        return !look_.running && !look_.unsure;
    }
    IdentityResult Refusal(const IdArgs& a) const {
        const GameLook l = look_;
        return Fail(a, "game_running",
                    l.running ? "the game is running: close it, then try again"
                              : "a process that looks like the game could not be inspected: close the game, then try again",
                    [l](JsonWriter& j) {
                        j.Field("running", l.running);
                        j.Field("unsure", l.unsure);
                        j.Field("how", l.how);
                    });
    }

private:
    std::string exe_;
    GameLook look_;
};

// ---- what the master says --------------------------------------------------------------------------------------

struct MasterWord {
    std::string code;
    std::string message;
};

MasterWord MasterError(const std::string& status, const net::Fields& reply, const std::string& master) {
    if (status == "down")
        return {"master_unreachable", "the nick server " + master + " does not answer" +
                                          (reply.count("error") && !reply.at("error").empty() ? " (" + reply.at("error") + ")" : "")};
    if (status == "bad_clock") return {"bad_clock", net::ClockText(reply) + "; fix it in Windows and try again"};
    if (status == "rate_limited")
        return {"rate_limited",
                "the nick server is limiting this PC for now (30 requests a minute, 3 new nicks a day, 10 recoveries an hour): try again later"};
    if (status == "bad_code") return {"bad_code", "that recovery code is wrong"};
    if (status == "unknown_nick") return {"unknown_nick", "the nick server has no such nick registered"};
    if (status == "already_yours") return {"already_yours", "this nick is already yours"};
    if (status == "not_owner") return {"not_owner", "this nick belongs to another key"};
    if (status == "nothing_pending") return {"nothing_pending", "nobody is moving this nick"};
    if (status == "taken") return {"nick_taken", "the nick belongs to someone else on this server"};
    if (status == "bad_nick") return {"bad_nick", "the nick server does not accept that nick"};
    return {"master_refused", "the nick server answered \"" + (status.empty() ? std::string("nothing") : status) + "\""};
}

bool ParseServer(const std::string& in, std::string& host, uint16_t& port) {
    port = 31000;
    rmr::net::SplitHostPort(in, host, port);
    return !host.empty() && host.size() <= 253;
}

// Tickets and the recovery code, kept in the profile's files while the game is closed (identity_net.h).
class ToolHooks final : public net::TicketHooks {
public:
    ToolHooks(const Profile& p, const uint8_t (&pub)[32], bool may_write, Guard* guard)
        : p_(p), may_write_(may_write), guard_(guard) {
        std::memcpy(pub_, pub, 32);
    }
    bool Cached(const uint8_t (&master_key)[32], identity::Ticket& out) override {
        return core::LoadTicket(p_.dir, p_.file_id, master_key, pub_, out);
    }
    void Put(const uint8_t (&master_key)[32], const identity::Ticket& ticket) override {
        if (!may_write_ || !StillClosed()) return;
        core::SaveTicket(p_.dir, p_.file_id, master_key, ticket);
        const identity::Ticket zero{};
        if (std::memcmp(&ticket, &zero, sizeof(ticket)) != 0) ticket_stored = true;
    }
    void SetRecoveryCode(const std::string& code) override {
        if (!may_write_) return;
        // The master hands a code out once: if it cannot be kept it is reported, never dropped.
        core::KeyRecord rec;
        if (!StillClosed() || core::LoadKey(p_.dir, p_.file_id, rec) != core::Load::kLoaded) {
            lost_code = code;
            return;
        }
        rec.pending_code = code;
        rec.code_remind = 0;
        if (!core::SaveKey(p_.dir, p_.file_id, rec)) lost_code = code;
    }
    void Note(const std::string&) override { transferred = true; }

    bool ticket_stored = false;
    bool transferred = false;
    bool game_started = false;   // the game started while the pass ran: its writes were skipped
    std::string lost_code;       // a code the master gave that could not be stored

private:
    bool StillClosed() {
        if (guard_->Closed()) return true;
        game_started = true;
        return false;
    }
    Profile p_;
    uint8_t pub_[32];
    bool may_write_;
    Guard* guard_;
};

const char* CheckName(identity::Check c) {
    switch (c) {
        case identity::Check::kOff: return "off";
        case identity::Check::kWorking: return "working";
        case identity::Check::kNoAnswer: return "no_answer";
        case identity::Check::kNotChecked: return "not_checked";
        case identity::Check::kVerified: return "verified";
        case identity::Check::kTaken: return "taken";
        case identity::Check::kClaimPending: return "claim_pending";
        case identity::Check::kMasterDown: return "master_down";
        case identity::Check::kFailed: return "failed";
    }
    return "failed";
}

std::string Clip(const std::string& s, size_t n) { return s.size() > n ? s.substr(0, n) : s; }

// The secret line of stdin, or the failure document.
bool ReadSecret(const IdArgs& a, const ReadSecretLine& read, std::string& out, IdentityResult& fail) {
    out.clear();
    if (!read || !read(out) || out.empty()) {
        fail = Fail(a, "secret_missing", "nothing was written on stdin: the first line of stdin is read");
        return false;
    }
    if (out.size() >= 3 && static_cast<uint8_t>(out[0]) == 0xEF && static_cast<uint8_t>(out[1]) == 0xBB &&
        static_cast<uint8_t>(out[2]) == 0xBF)
        out.erase(0, 3);  // a byte order mark from a writer that adds one
    if (out.size() > size_t(kMaxSecretBytes)) {
        crypto_wipe(out.data(), out.size());
        out.clear();
        fail = Fail(a, "secret_too_long", "the line on stdin is longer than 1024 bytes");
        return false;
    }
    return true;
}

struct Wipe {
    std::string& s;
    ~Wipe() {
        if (!s.empty()) crypto_wipe(s.data(), s.size());
        s.clear();
    }
};

// ---- show ------------------------------------------------------------------------------------------------------

IdentityResult DoShow(const IdArgs& a, const Profile& p) {
    core::KeyRecord rec;
    const core::Load l = core::LoadKey(p.dir, p.file_id, rec);
    const bool have = l == core::Load::kLoaded;
    std::error_code ec;
    const fs::path key_path = core::KeyPath(p.dir, p.file_id);
    const char* state = have ? "ok" : (fs::exists(key_path, ec) ? "unreadable" : "missing");
    identity::KeyPair kp{};
    if (have) kp = rec.Keys();

    JsonWriter j(a.pretty);
    Begin(j, a, true);
    ProfileBlock(j, p);
    j.Key("key").BeginObject();
    j.Field("state", state);
    j.Field("protection", core::Protection());
    j.Key("fingerprint");
    if (have) j.String(identity::Fingerprint(kp.pub)); else j.Null();
    j.Key("xuid");
    if (have) j.String(Hex16(identity::XuidFromPublicKey(kp.pub))); else j.Null();
    j.Key("rmr_token");  // only whether there is one
    if (have) j.Bool(rec.has_rmr); else j.Null();
    j.Field("unreadable_copy", fs::exists(key_path.string() + ".unreadable", ec));
    j.EndObject();

    j.Key("recovery").BeginObject();
    j.Key("pending");
    if (have) j.Bool(!rec.pending_code.empty()); else j.Null();
    j.Key("remind_after");  // 0 = the game may remind now, -1 = never, else the unix time from which it may
    if (have && !rec.pending_code.empty()) j.Int(rec.code_remind); else j.Null();
    j.EndObject();

    j.Key("tickets").BeginArray();
    const int64_t now = identity::NowUnix();
    for (const auto& [master_hex, ticket_hex] : core::ReadTicketFile(p.dir, p.file_id)) {
        uint8_t master[32];
        identity::Ticket t{};
        if (!identity::FromHex(master_hex, master, 32) ||
            !identity::FromHex(ticket_hex, reinterpret_cast<uint8_t*>(&t), sizeof(t)) || t.body.nick_len == 0)
            continue;
        j.BeginObject();
        j.Field("master", identity::Fingerprint(master));
        j.Field("nick", identity::TicketNick(t));
        j.Field("expires", int64_t(t.body.expires));
        j.Field("valid", identity::CheckTicket(master, t, now));
        j.Field("for_this_key", have && std::memcmp(t.body.pubkey, kp.pub, 32) == 0);
        j.EndObject();
    }
    j.EndArray();
    return Done(j);
}

// ---- check -----------------------------------------------------------------------------------------------------

// The nick, the key and the relay of the networked commands.
struct Online {
    std::string host;
    uint16_t port = 31000;
};

IdentityResult BadServer(const IdArgs& a) {
    return Fail(a, "bad_server", "--server is host or host:port of a relay");
}

IdentityResult DoCheck(const IdArgs& a, const Profile& p) {
    Online o;
    if (!ParseServer(a.server, o.host, o.port)) return BadServer(a);
    const bool may_write = a.claim || a.refresh;
    Guard guard(a.game_exe);
    if (may_write && !guard.Closed()) return guard.Refusal(a);

    core::KeyRecord rec;
    IdentityResult fail;
    if (!LoadOrFail(a, p, rec, fail)) return fail;
    if (may_write) {
        if (!rec.pending_code.empty())
            return Fail(a, "recovery_code_unconfirmed",
                        "write down the recovery code and confirm it first (identity recovery show, then confirm): "
                        "a new one from the nick server would replace it");
        // The master gives a recovery code once: make sure the key file can be written before it is asked.
        if (!core::SaveKey(p.dir, p.file_id, rec))
            return Fail(a, "store_not_writable", "the key file cannot be written: nothing was asked of the nick server");
    }

    const identity::KeyPair keys = rec.Keys();
    ToolHooks hooks(p, keys.pub, may_write, &guard);
    net::CycleInput in;
    in.host = o.host;
    in.port = o.port;
    in.nick = p.gamertag;
    in.keys = &keys;
    in.claim = a.claim ? net::ClaimMode::kAlways : (a.refresh ? net::ClaimMode::kRefresh : net::ClaimMode::kNever);
    in.options = a.opt;
    identity::Status s;
    net::CycleOutput out;
    net::Cycle(s, in, hooks, &out);

    if (!hooks.lost_code.empty()) {
        const std::string code = hooks.lost_code;
        return Fail(a, "recovery_code_not_saved",
                    "the nick server registered the nick and gave a recovery code, but it could not be stored: write it "
                    "down now, it is not shown again",
                    [code](JsonWriter& j) { j.Field("recovery_code", code); });
    }
    if (hooks.game_started) return guard.Refusal(a);

    bool pending = false;
    if (may_write) {
        core::KeyRecord after;
        pending = core::LoadKey(p.dir, p.file_id, after) == core::Load::kLoaded && !after.pending_code.empty();
    }
    JsonWriter j(a.pretty);
    Begin(j, a, true);
    ProfileBlock(j, p);
    j.Key("server").BeginObject();
    j.Field("relay", s.relay);
    j.Key("master");
    if (out.master_known) j.String(s.master); else j.Null();
    j.EndObject();
    j.Key("status").BeginObject();
    j.Field("state", s.needs_claim ? "needs_claim" : CheckName(s.check));
    j.Field("detail", Clip(s.detail, 512));
    j.Field("master_status", s.master_status);
    j.Key("owner_nick");
    if (s.owner_nick.empty()) j.Null(); else j.String(Clip(s.owner_nick, 64));
    j.Field("claim_until", s.claim_until);
    j.Field("incoming_until", s.incoming_until);
    j.EndObject();
    j.Key("claim").BeginObject();
    j.Field("mode", a.claim ? "always" : (a.refresh ? "refresh" : "never"));
    j.Field("asked_master", !s.master_status.empty());
    j.Field("ticket_stored", hooks.ticket_stored);
    j.Field("transferred", hooks.transferred);
    j.Field("recovery_code_pending", pending);
    j.EndObject();
    return Done(j);
}

// ---- backup and restore ----------------------------------------------------------------------------------------

bool InsideFolder(const fs::path& file, const fs::path& dir) {
    std::error_code ec;
    const fs::path f = fs::weakly_canonical(file, ec);
    if (ec) return false;
    const fs::path d = fs::weakly_canonical(dir, ec);
    if (ec) return false;
    auto fi = f.begin();
    for (auto di = d.begin(); di != d.end(); ++di, ++fi) {
        if (fi == f.end() || *fi != *di) return false;
    }
    return true;
}

IdentityResult DoBackup(const IdArgs& a, const Profile& p, const ReadSecretLine& read) {
    std::string password;
    Wipe wipe{password};
    IdentityResult fail;
    if (!ReadSecret(a, read, password, fail)) return fail;
    core::KeyRecord rec;
    if (!LoadOrFail(a, p, rec, fail)) return fail;

    const fs::path target = PathFromUtf8(a.file);
    std::error_code ec;
    if (fs::is_directory(target, ec)) return Fail(a, "bad_file", "--file is a folder: name the backup file");
    if (InsideFolder(target, p.dir))
        return Fail(a, "bad_file", "the backup must not be written into the identity folder: keep it somewhere else");
    if (fs::exists(target, ec) && !a.overwrite)
        return Fail(a, "file_exists", "that file exists: choose another name, or give --overwrite");

    std::string bytes;
    const core::BackupResult made = core::MakeBackup(rec, password, bytes);
    if (made == core::BackupResult::kPasswordTooShort)
        return Fail(a, "password_too_short", core::BackupText(made));
    // The file is read back before it is trusted: a backup that cannot be opened is worse than none.
    core::KeyRecord check;
    if (core::OpenBackup(bytes, password, check) != core::BackupResult::kOk || !check.SameKeyAs(rec))
        return Fail(a, "backup_verify_failed", "the backup could not be read back: nothing was written");
    if (!core::WriteFileAtomic(target, bytes))
        return Fail(a, "cannot_write", "cannot write " + Utf8(target));

    JsonWriter j(a.pretty);
    Begin(j, a, true);
    ProfileBlock(j, p);
    j.Field("file", Utf8(target));
    j.Field("bytes", uint64_t(bytes.size()));
    j.Field("verified", true);
    j.Field("fingerprint", rec.Fingerprint());
    j.Key("includes").BeginObject();
    j.Field("key", true);
    j.Field("rmr_token", rec.has_rmr);
    j.Field("recovery_code", false);  // that belongs on paper, not in a file
    j.EndObject();
    return Done(j);
}

IdentityResult DoRestore(const IdArgs& a, const Profile& p, const ReadSecretLine& read) {
    Guard guard(a.game_exe);
    if (!guard.Closed()) return guard.Refusal(a);
    std::string password;
    Wipe wipe{password};
    IdentityResult fail;
    if (!ReadSecret(a, read, password, fail)) return fail;

    const fs::path source = PathFromUtf8(a.file);
    std::error_code ec;
    if (!fs::is_regular_file(source, ec)) return Fail(a, "bad_file", "cannot read " + Utf8(source));
    if (int64_t(fs::file_size(source, ec)) > kMaxBackupFileBytes)
        return Fail(a, "not_a_backup", core::BackupText(core::BackupResult::kNotABackup));
    std::string bytes;
    if (!core::ReadFile(source, bytes)) return Fail(a, "bad_file", "cannot read " + Utf8(source));
    core::KeyRecord restored;
    switch (core::OpenBackup(bytes, password, restored)) {
        case core::BackupResult::kOk: break;
        case core::BackupResult::kNotABackup: return Fail(a, "not_a_backup", core::BackupText(core::BackupResult::kNotABackup));
        default: return Fail(a, "wrong_password", core::BackupText(core::BackupResult::kWrongPassword));
    }

    // What is there now: a key that is not this one is only replaced when the caller said so, and is kept.
    core::KeyRecord current;
    const core::Load now = core::LoadKey(p.dir, p.file_id, current);
    const bool has_current = now == core::Load::kLoaded;
    const bool different = has_current && !current.SameKeyAs(restored);
    if (different && !a.replace) {
        const std::string was = current.Fingerprint(), will = restored.Fingerprint();
        return Fail(a, "would_replace_key",
                    "this profile already has another key: restoring replaces it (the old file is kept beside it); "
                    "give --replace to go on",
                    [was, will](JsonWriter& j) {
                        j.Field("current_fingerprint", was);
                        j.Field("backup_fingerprint", will);
                    });
    }

    // Right before the first write.
    if (!guard.Closed()) return guard.Refusal(a);
    std::string kept;
    const fs::path key_path = core::KeyPath(p.dir, p.file_id);
    std::error_code exists_ec;
    if (!has_current && fs::exists(key_path, exists_ec)) {
        // A file that is there and cannot be read: kept aside, never overwritten (the game does the same).
        core::SetAsideUnreadable(p.dir, p.file_id);
        kept = Utf8(key_path) + ".unreadable";
    } else if (different) {
        kept = Utf8(key_path) + ".replaced-" + std::to_string(identity::NowUnix());
        std::error_code mv;
        fs::rename(key_path, PathFromUtf8(kept), mv);
        if (mv) return Fail(a, "cannot_keep_old_key", "the old key could not be set aside: nothing was changed");
    }
    // The key that is already there stays as it is (its recovery code and tickets are still good); another key comes
    // with nothing of the old one's, as in the game's own import. The token is the backup's, and one the backup lacks
    // is not thrown away.
    const bool same_key = has_current && !different;
    core::KeyRecord next;
    if (same_key) next = current;
    std::memcpy(next.seed, restored.seed, 32);
    if (restored.has_rmr) {
        next.has_rmr = true;
        next.rmr = restored.rmr;
    } else if (has_current && current.has_rmr) {
        next.has_rmr = true;
        next.rmr = current.rmr;
    }
    if (!core::SaveKey(p.dir, p.file_id, next)) {
        // Put the old key back where it was: a profile must not be left without one.
        if (different && !kept.empty()) {
            std::error_code back;
            fs::rename(PathFromUtf8(kept), key_path, back);
        }
        return Fail(a, "cannot_write", "the key file cannot be written: " + Utf8(key_path));
    }
    if (!same_key) core::ForgetTickets(p.dir, p.file_id);  // tickets name the old key

    JsonWriter j(a.pretty);
    Begin(j, a, true);
    ProfileBlock(j, p);
    j.Field("file", Utf8(source));
    j.Field("fingerprint", next.Fingerprint());
    j.Field("xuid", Hex16(next.Xuid()));
    j.Key("previous_fingerprint");
    if (has_current) j.String(current.Fingerprint()); else j.Null();
    j.Key("previous_kept_as");
    if (kept.empty()) j.Null(); else j.String(kept);
    j.Field("restart_needed", true);
    return Done(j);
}

// ---- the recovery code -----------------------------------------------------------------------------------------

constexpr const char* kCodeAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

bool CodeFormatOk(const std::string& typed) {
    const std::string c = identity::CanonicalCode(typed);
    if (c.size() != 20) return false;
    for (char ch : c)
        if (std::strchr(kCodeAlphabet, ch) == nullptr) return false;
    return true;
}

IdentityResult DoRecoveryShow(const IdArgs& a, const Profile& p) {
    core::KeyRecord rec;
    IdentityResult fail;
    if (!LoadOrFail(a, p, rec, fail)) return fail;
    JsonWriter j(a.pretty);
    Begin(j, a, true);
    ProfileBlock(j, p);
    j.Key("recovery").BeginObject();
    j.Field("pending", !rec.pending_code.empty());
    j.Key("code");  // the one secret this tool prints, because it was asked for
    if (rec.pending_code.empty()) j.Null(); else j.String(rec.pending_code);
    j.Key("remind_after");
    if (rec.pending_code.empty()) j.Null(); else j.Int(rec.code_remind);
    j.EndObject();
    IdentityResult r = Done(j);
    return r;
}

IdentityResult DoRecoveryCheck(const IdArgs& a, const Profile& p, const ReadSecretLine& read) {
    std::string typed;
    Wipe wipe{typed};
    IdentityResult fail;
    if (!ReadSecret(a, read, typed, fail)) return fail;
    core::KeyRecord rec;
    const bool have = core::LoadKey(p.dir, p.file_id, rec) == core::Load::kLoaded;
    JsonWriter j(a.pretty);
    Begin(j, a, true);
    ProfileBlock(j, p);
    j.Key("recovery").BeginObject();
    j.Field("valid_format", CodeFormatOk(typed));
    j.Key("matches_pending");  // null: nothing to compare with
    if (have && !rec.pending_code.empty())
        j.Bool(SameText(identity::CanonicalCode(typed), identity::CanonicalCode(rec.pending_code)));
    else
        j.Null();
    j.EndObject();
    return Done(j);
}

IdentityResult DoRecoveryConfirm(const IdArgs& a, const Profile& p, const ReadSecretLine& read) {
    Guard guard(a.game_exe);
    if (!guard.Closed()) return guard.Refusal(a);
    std::string typed;
    Wipe wipe{typed};
    IdentityResult fail;
    if (a.code_stdin && !ReadSecret(a, read, typed, fail)) return fail;
    core::KeyRecord rec;
    if (!LoadOrFail(a, p, rec, fail)) return fail;
    if (rec.pending_code.empty()) return Fail(a, "no_pending_code", "there is no recovery code waiting to be confirmed");
    // The player may type it back first: confirming forgets the only copy this PC has.
    if (a.code_stdin && !SameText(identity::CanonicalCode(typed), identity::CanonicalCode(rec.pending_code)))
        return Fail(a, "code_mismatch", "that is not the recovery code that is waiting: nothing was changed");
    if (!guard.Closed()) return guard.Refusal(a);
    rec.pending_code.clear();
    rec.code_remind = 0;
    if (!core::SaveKey(p.dir, p.file_id, rec)) return Fail(a, "cannot_write", "the key file cannot be written");
    JsonWriter j(a.pretty);
    Begin(j, a, true);
    ProfileBlock(j, p);
    j.Field("confirmed", true);
    return Done(j);
}

IdentityResult DoRecoveryRemind(const IdArgs& a, const Profile& p) {
    Guard guard(a.game_exe);
    if (!guard.Closed()) return guard.Refusal(a);
    core::KeyRecord rec;
    IdentityResult fail;
    if (!LoadOrFail(a, p, rec, fail)) return fail;
    if (rec.pending_code.empty()) return Fail(a, "no_pending_code", "there is no recovery code waiting to be confirmed");
    rec.code_remind = a.never ? core::kNeverRemind : identity::NowUnix() + int64_t(a.days) * 86400;  // -1: only from F8
    if (!guard.Closed()) return guard.Refusal(a);
    if (!core::SaveKey(p.dir, p.file_id, rec)) return Fail(a, "cannot_write", "the key file cannot be written");
    JsonWriter j(a.pretty);
    Begin(j, a, true);
    ProfileBlock(j, p);
    j.Field("remind_after", rec.code_remind);
    return Done(j);
}

// ---- recover and cancel ----------------------------------------------------------------------------------------

// The relay's challenge names the master its players use; false with the failure when that cannot be found.
bool FindMaster(const IdArgs& a, const Online& o, net::Challenge& ch, IdentityResult& fail) {
    ch = net::AskChallenge(o.host, o.port, a.opt);
    if (ch.state == net::Challenge::State::kUnresolved) {
        fail = Fail(a, "relay_unresolved", "cannot find the server " + o.host);
        return false;
    }
    if (ch.state != net::Challenge::State::kOk) {
        fail = Fail(a, "relay_no_answer", "the server does not answer nick checks (an older relay, or it is down)");
        return false;
    }
    if (!ch.enforced) {
        fail = Fail(a, "relay_does_not_check_nicks", "this server does not check nicks: it has no nick server to ask");
        return false;
    }
    return true;
}

IdentityResult DoRecover(const IdArgs& a, const Profile& p, const ReadSecretLine& read) {
    Online o;
    if (!ParseServer(a.server, o.host, o.port)) return BadServer(a);
    std::string code;
    Wipe wipe{code};
    IdentityResult fail;
    if (!ReadSecret(a, read, code, fail)) return fail;
    // The master counts every try (5 an hour per nick): a code that cannot be right is not sent.
    if (!CodeFormatOk(code))
        return Fail(a, "bad_code_format", "a recovery code is 20 letters and digits, like XXXXX-XXXXX-XXXXX-XXXXX");
    core::KeyRecord rec;
    if (!LoadOrFail(a, p, rec, fail)) return fail;
    net::Challenge ch;
    if (!FindMaster(a, o, ch, fail)) return fail;

    const identity::KeyPair keys = rec.Keys();
    const std::string master = ch.master_host + ":" + std::to_string(ch.master_port);
    const net::Fields f = net::MasterCall(ch.master_host, ch.master_port, "recover", p.gamertag, keys, code, a.opt);
    const std::string status = f.count("status") ? f.at("status") : "";
    if (status != "pending") {
        const MasterWord w = MasterError(status, f, master);
        return Fail(a, w.code, w.message, [master, status](JsonWriter& j) {
            j.Field("master", master);
            j.Field("master_status", status);
        });
    }
    JsonWriter j(a.pretty);
    Begin(j, a, true);
    ProfileBlock(j, p);
    j.Field("master", master);
    j.Field("result", "pending");
    j.Field("until", int64_t(std::atoll(f.count("until") ? f.at("until").c_str() : "0")));
    return Done(j);
}

IdentityResult DoCancel(const IdArgs& a, const Profile& p) {
    Online o;
    if (!ParseServer(a.server, o.host, o.port)) return BadServer(a);
    Guard guard(a.game_exe);
    if (!guard.Closed()) return guard.Refusal(a);
    core::KeyRecord rec;
    IdentityResult fail;
    if (!LoadOrFail(a, p, rec, fail)) return fail;
    if (!core::SaveKey(p.dir, p.file_id, rec))
        return Fail(a, "store_not_writable", "the key file cannot be written: nothing was asked of the nick server");
    net::Challenge ch;
    if (!FindMaster(a, o, ch, fail)) return fail;

    const identity::KeyPair keys = rec.Keys();
    const std::string master = ch.master_host + ":" + std::to_string(ch.master_port);
    const net::Fields f = net::MasterCall(ch.master_host, ch.master_port, "cancel", p.gamertag, keys, "", a.opt);
    const std::string status = f.count("status") ? f.at("status") : "";
    if (status != "ok" || !f.count("recovery")) {
        const MasterWord w = MasterError(status, f, master);
        return Fail(a, w.code, w.message, [master, status](JsonWriter& j) {
            j.Field("master", master);
            j.Field("master_status", status);
        });
    }
    // The transfer is stopped and the old code is void: the new one replaces it, kept before anything else.
    const std::string new_code = f.at("recovery");
    bool saved = false;
    core::KeyRecord fresh;
    if (guard.Closed() && core::LoadKey(p.dir, p.file_id, fresh) == core::Load::kLoaded) {
        fresh.pending_code = new_code;
        fresh.code_remind = 0;
        saved = core::SaveKey(p.dir, p.file_id, fresh);
    }
    if (!saved)
        return Fail(a, "recovery_code_not_saved",
                    "the transfer was cancelled and the nick server gave a new recovery code, but it could not be stored: "
                    "write it down now, it is not shown again",
                    [new_code](JsonWriter& j) { j.Field("recovery_code", new_code); });
    JsonWriter j(a.pretty);
    Begin(j, a, true);
    ProfileBlock(j, p);
    j.Field("master", master);
    j.Field("result", "cancelled");
    j.Field("recovery_code_pending", true);
    return Done(j);
}

// ---- rename ----------------------------------------------------------------------------------------------------

IdentityResult DoRename(const IdArgs& a, const Profile& p) {
    namespace pf = larecomp::profiles;
    const fs::path saves = PathFromUtf8(a.saves);
    const std::string current = p.gamertag;
    const std::string problem = identity::RenameProblem(a.nick, current, saves);
    if (!problem.empty()) return Fail(a, "nick_not_allowed", problem);
    if (!identity::ValidNick(a.nick)) return Fail(a, "nick_not_allowed", "the nick server does not accept that nick");

    Guard guard(a.game_exe);
    if (!guard.Closed()) return guard.Refusal(a);

    bool claimed = false;
    if (a.claim) {
        Online o;
        if (!ParseServer(a.server, o.host, o.port)) return BadServer(a);
        core::KeyRecord rec;
        IdentityResult fail;
        if (!LoadOrFail(a, p, rec, fail)) return fail;
        // A new nick is a new registration, which comes with a new recovery code that would replace one not yet confirmed.
        if (!rec.pending_code.empty())
            return Fail(a, "recovery_code_unconfirmed",
                        "write down the recovery code of the current nick and confirm it first: the new nick gets its "
                        "own code, which would replace it");
        if (!core::SaveKey(p.dir, p.file_id, rec))
            return Fail(a, "store_not_writable", "the key file cannot be written: nothing was asked of the nick server");
        const identity::KeyPair keys = rec.Keys();
        ToolHooks hooks(p, keys.pub, true, &guard);
        net::CycleInput in;
        in.host = o.host;
        in.port = o.port;
        in.nick = a.nick;
        in.keys = &keys;
        in.claim = net::ClaimMode::kAlways;
        in.options = a.opt;
        identity::Status s;
        net::CycleOutput out;
        net::Cycle(s, in, hooks, &out);
        if (!hooks.lost_code.empty()) {
            const std::string code = hooks.lost_code;
            return Fail(a, "recovery_code_not_saved",
                        "the nick server registered the new nick and gave a recovery code, but it could not be stored: "
                        "write it down now, it is not shown again",
                        [code](JsonWriter& j) { j.Field("recovery_code", code); });
        }
        if (hooks.game_started) return guard.Refusal(a);
        identity::Ticket t{};
        claimed = out.master_known && core::LoadTicket(p.dir, p.file_id, out.master_key, keys.pub, t) &&
                  identity::TicketNick(t) == a.nick;
        if (!claimed) {
            if (s.check == identity::Check::kNotChecked)
                return Fail(a, "relay_does_not_check_nicks", "this server does not check nicks: there is nothing to claim the nick on");
            if (s.check == identity::Check::kNoAnswer && !out.master_known)
                return Fail(a, "relay_no_answer", "the server does not answer nick checks (an older relay, or it is down)");
            const MasterWord w = s.check == identity::Check::kTaken
                                     ? MasterWord{"nick_taken", "\"" + Clip(s.owner_nick, 64) + "\" belongs to someone else on this server"}
                                     : (s.check == identity::Check::kClaimPending
                                            ? MasterWord{"claim_pending", "a transfer of that nick to this key is pending"}
                                            : MasterError(s.check == identity::Check::kMasterDown ? "down" : s.master_status,
                                                          {{"error", s.detail}}, s.master));
            const std::string master_status = s.master_status;
            return Fail(a, w.code, w.message + ": the gamertag was not changed",
                        [master_status](JsonWriter& j) { j.Field("master_status", master_status); });
        }
    }

    // The two writes of "Use another gamertag", then the key follows the profile (identity_ui.cpp).
    if (!guard.Closed()) return guard.Refusal(a);
    if (!pf::WriteMarker(saves, current)) return Fail(a, "marker_not_written", "could not write the save marker");
    std::vector<pf::ConfigEdit> edits;
    edits.push_back({"user_name", pf::QuoteTomlString(a.nick)});
    if (!pf::EditConfig(PathFromUtf8(a.config), edits))
        return Fail(a, "config_not_written", "could not write " + Utf8(PathFromUtf8(a.config)));

    // A key still filed by the gamertag (a profile without its own id) is looked for under the new name next time.
    bool moved = false;
    if (p.file_id.rfind("name_", 0) == 0) {
        std::error_code ec;
        fs::rename(core::KeyPath(p.dir, p.file_id), core::KeyPath(p.dir, "name_" + a.nick), ec);
        moved = !ec;
        if (!ec && fs::exists(core::TicketPath(p.dir, p.file_id), ec))
            fs::rename(core::TicketPath(p.dir, p.file_id), core::TicketPath(p.dir, "name_" + a.nick), ec);
    }

    JsonWriter j(a.pretty);
    Begin(j, a, true);
    ProfileBlock(j, p);
    j.Field("from", current);
    j.Field("to", a.nick);
    j.Field("claimed", claimed);
    j.Key("key_moved");
    if (p.file_id.rfind("name_", 0) == 0) j.Bool(moved); else j.Null();
    j.Field("restart_needed", true);
    return Done(j);
}

}  // namespace

// ---- entry -----------------------------------------------------------------------------------------------------

std::vector<std::string> IdentityActions() {
    return {"show", "check", "backup", "restore", "recovery show", "recovery confirm", "recovery remind",
            "recovery check", "recover", "cancel", "rename"};
}

const char* IdentityUsageText() {
    return
        "identity commands (the player's online identity; README-identity.md):\n"
        "  identity show                 gamertag, XUID, key fingerprint, whether a recovery code waits, tickets (no secret)\n"
        "  identity check --server H     what a relay and its nick server say about the nick; --claim registers it, --refresh\n"
        "                                renews a registration already made\n"
        "  identity backup --file F      a backup file, password on stdin (--password-stdin)\n"
        "  identity restore --file F     put a backup's key back, password on stdin; the game must be closed\n"
        "  identity recovery show|confirm|remind|check\n"
        "  identity recover --server H   move a nick to this key with its recovery code (on stdin: --code-stdin)\n"
        "  identity cancel --server H    stop someone moving your nick with your recovery code\n"
        "  identity rename --nick N      use another gamertag (--claim --server H registers it first)\n"
        "  profile: --profile <id> | --saves <folder> --gamertag <name>   [--identity-dir D]   [--pretty]\n"
        "  commands that change files need --game-exe <larecomp.exe> and refuse while the game runs.\n";
}

IdentityResult RunIdentity(const std::vector<std::string>& args, const ReadSecretLine& read_secret_line) {
    const IdParseResult parsed = ParseIdentityArgs(args);
    if (!parsed.ok) return {RenderError("identity", "usage", parsed.error, false), 2};
    const IdArgs& a = parsed.args;
    Profile p;
    if (IdentityResult bad = ResolveProfile(a, p); !bad.json.empty()) return bad;

    if (a.action == "show") return DoShow(a, p);
    if (a.action == "check") return DoCheck(a, p);
    if (a.action == "backup") return DoBackup(a, p, read_secret_line);
    if (a.action == "restore") return DoRestore(a, p, read_secret_line);
    if (a.action == "recovery_show") return DoRecoveryShow(a, p);
    if (a.action == "recovery_check") return DoRecoveryCheck(a, p, read_secret_line);
    if (a.action == "recovery_confirm") return DoRecoveryConfirm(a, p, read_secret_line);
    if (a.action == "recovery_remind") return DoRecoveryRemind(a, p);
    if (a.action == "recover") return DoRecover(a, p, read_secret_line);
    if (a.action == "cancel") return DoCancel(a, p);
    if (a.action == "rename") return DoRename(a, p);
    return {RenderError("identity", "usage", "unknown identity command", false), 2};
}

}  // namespace larecomp::online_tool
