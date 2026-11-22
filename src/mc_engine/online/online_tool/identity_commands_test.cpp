// Check of the identity core (identity/identity_core.cpp), the relay and master exchange (identity/identity_net.cpp) and
// `larecomp_online_tool identity ...` (identity_commands.cpp). A program of its own: no game, no ReXGlue, no real server
// and no real nick. The only network it touches is 127.0.0.1: a fake relay and the real nick registry behind a loopback
// listener (identity/test/identity_test_support.h). Keys, backups and profiles are made in scratch folders. Exits 0 when
// every check holds.
//
//   cmake --build <build dir> --target larecomp_online_tool_identity_test     (CMakeLists.txt)
//
// or, with no build directory at all (Windows):
//   clang++ -std=c++23 -D_CRT_SECURE_NO_WARNINGS identity_commands_test.cpp identity_commands.cpp game_guard.cpp
//           online_tool.cpp ../server_browser/browser_query.cpp ../relay_info.cpp ../rate_my_ride/rmr_net.cpp
//           ../identity/identity_core.cpp ../identity/identity_net.cpp ../identity/identity_rename.cpp
//           ../master_server/identity_registry.cpp ../../../profile/larecomp_profile_store.cpp
//           -x c++ ../../../../thirdparty/monocypher/monocypher.c -lws2_32 -lcrypt32 -o identity_test.exe
//
// What it pins:
//   - the key file format and the backup file format (a golden backup made by the code as it was before the split);
//   - the seal, the ticket cache, the profile id rules, the legacy by-name move;
//   - the closed-game rule (a held game marker refuses every writing command, and nothing changes);
//   - that no document ever holds the key, the Rate My Ride token, a password or a recovery code (except the one
//     `recovery show` was asked for);
//   - every command against the real registry: claim, taken, recover, cancel, the hold and the move, rename, the limits
//     (3 new nicks a day, recovery tries), a wrong clock, a master that is down, a relay that does not check nicks.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "game_guard.h"
#include "identity_commands.h"
#include "online_tool.h"
#include "../identity/identity_core.h"
#include "../identity/identity_net.h"
#include "../identity/identity_rename.h"
#include "../identity/test/identity_test_support.h"
#include "../../../game_marker_core.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
namespace core = larecomp::identity::core;
namespace idn = larecomp::identity;
namespace ot = larecomp::online_tool;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAILED: %s\n", what.c_str());
    }
}

void CheckEq(const std::string& got, const std::string& want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failed;
        std::printf("FAILED: %s: got '%s', want '%s'\n", what.c_str(), got.c_str(), want.c_str());
    }
}

void CheckInt(long long got, long long want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failed;
        std::printf("FAILED: %s: got %lld, want %lld\n", what.c_str(), got, want);
    }
}

std::string FromHexText(const std::string& hex) {
    std::string out(hex.size() / 2, '\0');
    idn::FromHex(hex, reinterpret_cast<uint8_t*>(out.data()), out.size());
    return out;
}

// ---- a small JSON reader: the check reads the documents the way the launcher does --------------------------------

struct JV {
    enum Type { kNull, kBool, kNum, kStr, kArr, kObj } t = kNull;
    bool b = false;
    long long n = 0;
    std::string s;
    std::vector<JV> a;
    std::vector<std::string> keys;
    std::vector<JV> o;

    const JV* Get(const std::string& k) const {
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == k) return &o[i];
        return nullptr;
    }
    // "error.code"
    const JV* At(const std::string& path) const {
        const JV* cur = this;
        size_t pos = 0;
        while (cur && pos <= path.size()) {
            const size_t dot = path.find('.', pos);
            cur = cur->Get(path.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos));
            if (dot == std::string::npos) break;
            pos = dot + 1;
        }
        return cur;
    }
    std::string Str(const std::string& path) const {
        const JV* v = At(path);
        return v && v->t == kStr ? v->s : std::string("<missing>");
    }
    bool IsTrue(const std::string& path) const {
        const JV* v = At(path);
        return v && v->t == kBool && v->b;
    }
    bool IsFalse(const std::string& path) const {
        const JV* v = At(path);
        return v && v->t == kBool && !v->b;
    }
    bool IsNull(const std::string& path) const {
        const JV* v = At(path);
        return v && v->t == kNull;
    }
    long long Num(const std::string& path, long long fallback = -999999) const {
        const JV* v = At(path);
        return v && v->t == kNum ? v->n : fallback;
    }
};

struct JsonParser {
    const std::string& in;
    size_t i = 0;
    std::string err;
    explicit JsonParser(const std::string& s) : in(s) {}

    void Ws() {
        while (i < in.size() && (in[i] == ' ' || in[i] == '\n' || in[i] == '\r' || in[i] == '\t')) ++i;
    }
    bool Fail(const std::string& m) {
        if (err.empty()) err = m + " at " + std::to_string(i);
        return false;
    }
    bool Str(std::string& out) {
        if (i >= in.size() || in[i] != '"') return Fail("string expected");
        ++i;
        while (i < in.size() && in[i] != '"') {
            unsigned char c = static_cast<unsigned char>(in[i]);
            if (c < 0x20) return Fail("raw control character in a string");
            if (c == '\\') {
                if (++i >= in.size()) return Fail("bad escape");
                switch (in[i]) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        if (i + 4 >= in.size()) return Fail("bad \\u");
                        unsigned v = unsigned(std::strtoul(in.substr(i + 1, 4).c_str(), nullptr, 16));
                        i += 4;
                        if (v < 0x80) out += char(v);
                        else if (v < 0x800) { out += char(0xC0 | (v >> 6)); out += char(0x80 | (v & 63)); }
                        else { out += char(0xE0 | (v >> 12)); out += char(0x80 | ((v >> 6) & 63)); out += char(0x80 | (v & 63)); }
                        break;
                    }
                    default: return Fail("bad escape");
                }
                ++i;
            } else {
                out += char(c);
                ++i;
            }
        }
        if (i >= in.size()) return Fail("unterminated string");
        ++i;
        return true;
    }
    bool Value(JV& v) {
        Ws();
        if (i >= in.size()) return Fail("value expected");
        const char c = in[i];
        if (c == '{') {
            v.t = JV::kObj;
            ++i;
            Ws();
            if (i < in.size() && in[i] == '}') { ++i; return true; }
            for (;;) {
                Ws();
                std::string k;
                if (!Str(k)) return false;
                Ws();
                if (i >= in.size() || in[i] != ':') return Fail("':' expected");
                ++i;
                JV child;
                if (!Value(child)) return false;
                v.keys.push_back(k);
                v.o.push_back(std::move(child));
                Ws();
                if (i < in.size() && in[i] == ',') { ++i; continue; }
                if (i < in.size() && in[i] == '}') { ++i; return true; }
                return Fail("',' or '}' expected");
            }
        }
        if (c == '[') {
            v.t = JV::kArr;
            ++i;
            Ws();
            if (i < in.size() && in[i] == ']') { ++i; return true; }
            for (;;) {
                JV child;
                if (!Value(child)) return false;
                v.a.push_back(std::move(child));
                Ws();
                if (i < in.size() && in[i] == ',') { ++i; continue; }
                if (i < in.size() && in[i] == ']') { ++i; return true; }
                return Fail("',' or ']' expected");
            }
        }
        if (c == '"') { v.t = JV::kStr; return Str(v.s); }
        if (in.compare(i, 4, "true") == 0) { v.t = JV::kBool; v.b = true; i += 4; return true; }
        if (in.compare(i, 5, "false") == 0) { v.t = JV::kBool; v.b = false; i += 5; return true; }
        if (in.compare(i, 4, "null") == 0) { v.t = JV::kNull; i += 4; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            size_t j = i + 1;
            while (j < in.size() && in[j] >= '0' && in[j] <= '9') ++j;
            v.t = JV::kNum;
            v.n = std::atoll(in.substr(i, j - i).c_str());
            i = j;
            return true;
        }
        return Fail("unexpected character");
    }
};

bool ParseJson(const std::string& text, JV& out, std::string& err) {
    JsonParser p(text);
    if (!p.Value(out)) {
        err = p.err;
        return false;
    }
    p.Ws();
    if (p.i != text.size()) {
        err = "trailing text";
        return false;
    }
    return true;
}

// ---- running the command ----------------------------------------------------------------------------------------

struct Doc {
    std::string text;
    bool may_hold_code = false;  // `recovery show` and a failure that says the code could not be stored
};
std::vector<Doc> g_docs;
std::vector<std::string> g_secrets;  // hex of every seed and token, every password, every recovery code seen

struct Run {
    ot::IdentityResult r;
    JV doc;
    bool ok() const { return r.exit_code == 0 && doc.IsTrue("ok"); }
    std::string code() const { return doc.Str("error.code"); }
};

// Runs `identity <args>` with `lines` as what stdin holds.
Run Id(const std::vector<std::string>& args, const std::vector<std::string>& lines = {}, bool may_hold_code = false) {
    std::deque<std::string> queue(lines.begin(), lines.end());
    Run run;
    run.r = ot::RunIdentity(args, [&](std::string& out) {
        if (queue.empty()) return false;
        out = queue.front();
        queue.pop_front();
        return true;
    });
    std::string err;
    const bool parsed = ParseJson(run.r.json, run.doc, err);
    Check(parsed, "the document of 'identity " + (args.empty() ? std::string() : args[0]) + "' is valid JSON: " + err);
    Check(run.doc.IsTrue("ok") == (run.r.exit_code == 0), "ok matches the exit code for identity " + (args.empty() ? std::string() : args[0]));
    Check(run.r.exit_code == 0 || run.r.exit_code == 1 || run.r.exit_code == 2, "exit code is 0, 1 or 2");
    CheckEq(run.doc.Str("command"), "identity", "command field");
    CheckInt(run.doc.Num("schema"), 1, "schema field");
    g_docs.push_back({run.r.json, may_hold_code});
    return run;
}

// A copy of `base` with more arguments.
std::vector<std::string> With(std::vector<std::string> base, std::initializer_list<std::string> more) {
    base.insert(base.end(), more.begin(), more.end());
    return base;
}
std::vector<std::string> With(std::vector<std::string> base, const std::vector<std::string>& more) {
    base.insert(base.end(), more.begin(), more.end());
    return base;
}

// ---- profiles and the fake game ---------------------------------------------------------------------------------

struct TestProfile {
    std::string gamertag;
    std::string id;
    uint8_t base = 0;
    fs::path identity_dir;
    fs::path saves;
    fs::path config;      // larecomp.toml
    fs::path game_exe;
    std::string seed_hex;
    std::string rmr_hex;
    std::string fingerprint;

    // `--identity-dir D --saves S --gamertag G`
    std::vector<std::string> Who() const {
        return {"--identity-dir", identity_dir.string(), "--saves", saves.string(), "--gamertag", gamertag};
    }
    std::vector<std::string> Closed() const { return With(Who(), {"--game-exe", game_exe.string()}); }
};

// A profile whose game has run once: profile.xuid in its save folder and a key in the identity folder.
TestProfile MakeProfile(const fs::path& root, const std::string& gamertag, uint8_t base, bool with_rmr = true,
                        const std::string& pending_code = "") {
    TestProfile p;
    p.gamertag = gamertag;
    p.base = base;
    p.identity_dir = root / "identity";
    p.saves = root / "saves";
    p.config = root / "game" / "larecomp.toml";
    p.game_exe = root / "game" / "larecomp.exe";
    fs::create_directories(p.saves / gamertag);
    fs::create_directories(p.config.parent_path());
    const uint64_t xuid = 0xB13E000000000000ull | (0x1000ull * base) | 0x77;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llX\n", static_cast<unsigned long long>(xuid));
    std::ofstream(p.saves / gamertag / "profile.xuid", std::ios::binary) << buf;
    p.id = core::ProfileId(xuid, gamertag);
    core::KeyRecord rec;
    for (int i = 0; i < 32; ++i) rec.seed[i] = uint8_t(base + i);
    if (with_rmr) {
        rec.has_rmr = true;
        for (int i = 0; i < 16; ++i) rec.rmr[size_t(i)] = uint8_t(0xA0 + base + i);
    }
    rec.pending_code = pending_code;
    p.seed_hex = idn::ToHex(rec.seed, 32);
    if (with_rmr) p.rmr_hex = idn::ToHex(rec.rmr.data(), 16);
    p.fingerprint = rec.Fingerprint();
    core::SaveKey(p.identity_dir, p.id, rec);
    g_secrets.push_back(p.seed_hex);
    if (with_rmr) g_secrets.push_back(p.rmr_hex);
    return p;
}

// What a running game holds (game_marker.h), so that the tool sees it.
class FakeGame {
public:
    explicit FakeGame(const fs::path& exe, const fs::path& runtime_dir) {
        fs::path abs = fs::absolute(exe);
        abs.make_preferred();
        const std::u8string u = abs.u8string();
        const std::string path(reinterpret_cast<const char*>(u.data()), u.size());
#ifdef _WIN32
        (void)runtime_dir;
        const std::string name = larecomp::marker::WindowsMutexName(larecomp::marker::KeyOfPath(path, true));
        handle_ = CreateMutexW(nullptr, FALSE, std::wstring(name.begin(), name.end()).c_str());
#else
        const std::string folder = larecomp::marker::LinuxLockFolder(runtime_dir.string(), uint32_t(getuid()));
        fs::create_directories(folder);
        chmod(folder.c_str(), 0700);
        fd_ = open((folder + "/" + larecomp::marker::LinuxLockFileName(larecomp::marker::KeyOfPath(path, false))).c_str(),
                   O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd_ >= 0) flock(fd_, LOCK_SH | LOCK_NB);
#endif
    }
    ~FakeGame() {
#ifdef _WIN32
        if (handle_) CloseHandle(handle_);
#else
        if (fd_ >= 0) close(fd_);
#endif
    }
    FakeGame(const FakeGame&) = delete;
    FakeGame& operator=(const FakeGame&) = delete;

private:
#ifdef _WIN32
    HANDLE handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

std::string ReadAll(const fs::path& p) {
    std::string s;
    core::ReadFile(p, s);
    return s;
}

bool CodeShape(const std::string& code) {
    if (code.size() != 23) return false;
    for (size_t i = 0; i < code.size(); ++i) {
        if (i % 6 == 5) {
            if (code[i] != '-') return false;
        } else if (std::strchr("0123456789ABCDEFGHJKMNPQRSTVWXYZ", code[i]) == nullptr) {
            return false;
        }
    }
    return true;
}

// The golden backup: made with the algorithm as it was in identity_store.cpp before the split (4364d9ca), seed 00..1f,
// RMR token a0..af, password "correct horse battery staple", salt 10..1f, nonce 30..47.
const char* kGoldenBackup =
    "4c52494442414b31101112131415161718191a1b1c1d1e1f303132333435363738393a3b3c3d3e3f4041424344454647ca0a8c9d03060441"
    "65d0991af468f5646642cba168d01f24ea64b85c248f7b86d81ef999a90cfb566290143feeb5ea232ad8134957b8f105393426ec7c326ae4"
    "5c20e8f3a10643ab802c43ac6cb324e81fb57bcec5d90a2382398b751e023a77f9ffbf6398a4e1e612109dee517d18680671eae0a84a98c7"
    "27e4a7c3f415ad7f9f031c167be9";

// ---- the core -----------------------------------------------------------------------------------------------------

void TestCore(const fs::path& tmp) {
    // The key file's plain text, byte for byte.
    core::KeyRecord rec;
    for (int i = 0; i < 32; ++i) rec.seed[i] = uint8_t(i);
    rec.has_rmr = true;
    for (int i = 0; i < 16; ++i) rec.rmr[size_t(i)] = uint8_t(0xA0 + i);
    CheckEq(core::Serialize(rec),
            "LRIDSTORE1\nseed=000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\n"
            "rmr=a0a1a2a3a4a5a6a7a8a9aaabacadaeaf\n",
            "key file text without a code");
    rec.pending_code = "ABCDE-FGHJK-MNPQR-STVWX";
    CheckEq(core::Serialize(rec).substr(core::Serialize(rec).find("code=")),
            "code=ABCDE-FGHJK-MNPQR-STVWX\n", "key file text with a code and no reminder");
    rec.code_remind = 1234;
    CheckEq(core::Serialize(rec).substr(core::Serialize(rec).find("code=")),
            "code=ABCDE-FGHJK-MNPQR-STVWX\ncode_remind=1234\n", "key file text with a reminder");
    core::KeyRecord back;
    Check(core::Parse(core::Serialize(rec), back), "the key file text parses");
    Check(back.SameKeyAs(rec) && back.has_rmr && back.rmr == rec.rmr && back.pending_code == rec.pending_code &&
              back.code_remind == 1234,
          "the key file text round trips");
    core::KeyRecord junk;
    Check(!core::Parse("LRIDSTORE2\nseed=00\n", junk), "another magic is not a key file");
    Check(!core::Parse("LRIDSTORE1\nseed=0011\n", junk), "a short seed is not a key file");
    Check(!core::Parse("LRIDSTORE1\nseed=zz0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\n", junk),
          "a seed with a non-hex digit is not a key file");
    core::KeyRecord norefs;
    Check(core::Parse("LRIDSTORE1\nseed=000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\n", norefs) &&
              !norefs.has_rmr && norefs.pending_code.empty(),
          "a key file without a token or a code parses with neither");

    // Keys, fingerprint and XUID follow the seed (the public key is derived, not stored).
    const idn::KeyPair kp = rec.Keys();
    CheckEq(rec.Fingerprint(), idn::Fingerprint(kp.pub), "fingerprint of the derived public key");
    CheckInt(int64_t(rec.Xuid() >> 48), 0xB13E, "the XUID a key gives starts with B13E");

    // The seal.
    std::string sealed, plain;
    Check(core::Seal("hello identity", sealed), "Seal works");
    Check(core::Unseal(sealed, plain) && plain == "hello identity", "Unseal gives back what Seal sealed");
#ifdef _WIN32
    Check(sealed != "hello identity" && sealed.find("hello identity") == std::string::npos, "DPAPI output is not the plain text");
    CheckEq(core::Protection(), "windows_dpapi", "protection name on Windows");
    std::string bad;
    Check(!core::Unseal("this is not a DPAPI blob at all", bad), "a file that is not sealed does not unseal");
#else
    CheckEq(core::Protection(), "plain_file", "protection name elsewhere");
#endif

    // A recovery code the master sends back is kept only when it is one printable line of a sane length.
    Check(idn::PlausibleRecoveryCode("ABCDE-FGHJK-MNPQR-STVWX") && idn::PlausibleRecoveryCode(idn::NewRecoveryCode()),
          "a real recovery code is plausible");
    Check(!idn::PlausibleRecoveryCode("") && !idn::PlausibleRecoveryCode(std::string(65, 'A')) &&
              !idn::PlausibleRecoveryCode(std::string(1024 * 1024, 'A')) && !idn::PlausibleRecoveryCode("AB\nCD") &&
              !idn::PlausibleRecoveryCode(std::string("AB\0CD", 5)) && !idn::PlausibleRecoveryCode("AB\rCD") &&
              !idn::PlausibleRecoveryCode("caf\xC3\xA9") && !idn::PlausibleRecoveryCode("AB\x7F"),
          "an empty, long or non-printable text is not a recovery code");

    // Profile ids.
    CheckEq(core::ProfileId(0xB13E00000000ABCDull, "Neo"), "B13E00000000ABCD", "profile id is the XUID in 16 upper-case digits");
    CheckEq(core::ProfileId(core::kLegacyXuid, "Neo"), "name_Neo", "the legacy XUID files the key by name");
    Check(core::ValidProfileId("B13E00000000ABCD") && core::ValidProfileId("b13e00000000abcd"), "16 hex digits are an id");
    Check(core::ValidProfileId("name_Neo") && core::ValidProfileId("name_Two Words"), "name_ and a gamertag are an id");
    Check(!core::ValidProfileId("") && !core::ValidProfileId("B13E00000000ABC") && !core::ValidProfileId("B13E00000000ABCDE") &&
              !core::ValidProfileId("../../Windows/x") && !core::ValidProfileId("name_") && !core::ValidProfileId("name_a/b") &&
              !core::ValidProfileId("name_a\\b") && !core::ValidProfileId("name_..") && !core::ValidProfileId("name_0123456789abcdefg") &&
              !core::ValidProfileId("B13E00000000ABCG"),
          "an id that could name another file is refused");

    // profile.xuid.
    {
        const fs::path saves = tmp / "px";
        fs::create_directories(saves / "Neo");
        uint64_t x = 0;
        Check(!core::ReadProfileXuid(saves, "Neo", x), "no profile.xuid is not a profile");
        std::ofstream(saves / "Neo" / "profile.xuid", std::ios::binary) << "B13E00000000ABCD\n";
        Check(core::ReadProfileXuid(saves, "Neo", x) && x == 0xB13E00000000ABCDull, "profile.xuid is read as hex");
        std::ofstream(saves / "Neo" / "profile.xuid", std::ios::binary | std::ios::trunc) << "0\n";
        Check(!core::ReadProfileXuid(saves, "Neo", x), "XUID 0 is not valid");
        std::ofstream(saves / "Neo" / "profile.xuid", std::ios::binary | std::ios::trunc) << "00C0000000000001\n";
        Check(!core::ReadProfileXuid(saves, "Neo", x), "an XUID with bits 54-55 set is not valid (the SDK's rule)");
        std::ofstream(saves / "Neo" / "profile.xuid", std::ios::binary | std::ios::trunc) << "not hex\n";
        Check(!core::ReadProfileXuid(saves, "Neo", x), "text is not an XUID");
    }

    // Saving, loading, an unreadable file, the move of a key filed by name.
    {
        const fs::path dir = tmp / "store";
        core::KeyRecord a, got;
        for (int i = 0; i < 32; ++i) a.seed[i] = uint8_t(9 + i);
        a.has_rmr = true;
        a.rmr[0] = 7;
        Check(core::LoadKey(dir, "B13E000000000001", got) == core::Load::kMissing, "no file is kMissing");
        Check(core::SaveKey(dir, "B13E000000000001", a), "SaveKey writes (and makes the folder)");
        Check(core::LoadKey(dir, "B13E000000000001", got) == core::Load::kLoaded && got.SameKeyAs(a) && got.rmr[0] == 7,
              "LoadKey reads what SaveKey wrote");
        std::string raw = ReadAll(core::KeyPath(dir, "B13E000000000001"));
#ifdef _WIN32
        Check(raw.find("LRIDSTORE1") == std::string::npos, "the key file on Windows is sealed: no plain text in it");
#endif
#ifdef _WIN32
        Check(raw.find(idn::ToHex(a.seed, 32)) == std::string::npos, "the seed is not in the key file in the clear");
#else
        struct stat st;
        Check(stat(core::KeyPath(dir, "B13E000000000001").string().c_str(), &st) == 0 && (st.st_mode & 0777) == 0600,
              "elsewhere than Windows the key file is mode 0600");
#endif
        std::ofstream(core::KeyPath(dir, "B13E000000000002"), std::ios::binary) << "garbage that is not a key";
        Check(core::LoadKey(dir, "B13E000000000002", got) == core::Load::kUnreadable, "a file that is not a key is kUnreadable");
        Check(core::SetAsideUnreadable(dir, "B13E000000000002") && fs::exists(core::KeyPath(dir, "B13E000000000002").string() + ".unreadable") &&
                  !fs::exists(core::KeyPath(dir, "B13E000000000002")),
              "an unreadable key is moved aside, never overwritten");
        // A second unreadable file does not take the first one's place: it was perhaps the only copy of a key.
        {
            std::ofstream(core::KeyPath(dir, "B13E000000000002"), std::ios::binary) << "second garbage";
            fs::path kept;
            const fs::path first = core::KeyPath(dir, "B13E000000000002").string() + ".unreadable";
            Check(core::SetAsideUnreadable(dir, "B13E000000000002", &kept) && kept.filename().string().find(".unreadable-2") != std::string::npos &&
                      ReadAll(first) == "garbage that is not a key" && ReadAll(kept) == "second garbage" &&
                      !fs::exists(core::KeyPath(dir, "B13E000000000002")),
                  "a second unreadable key is kept beside the first, which is not overwritten");
        }
        // The key a backup replaces is copied first; nothing to copy is fine.
        {
            fs::path copy;
            Check(core::KeepCopyOfKey(dir, "B13E0000000000FF", &copy) && copy.empty(), "no key file, nothing to keep");
            Check(core::KeepCopyOfKey(dir, "B13E000000000001", &copy) && !copy.empty() &&
                      ReadAll(copy) == ReadAll(core::KeyPath(dir, "B13E000000000001")) &&
                      copy.filename().string().find(".key.replaced-") != std::string::npos,
                  "the key about to be replaced is copied to .replaced-<time>");
            core::KeyRecord back;
            Check(core::LoadKey(dir, "B13E000000000001", back) == core::Load::kLoaded && back.SameKeyAs(a), "the copy left the key itself alone");
        }
        // A saved key leaves no temporary file behind.
        Check(!fs::exists(core::KeyPath(dir, "B13E000000000001").string() + ".tmp"), "SaveKey leaves no .tmp file");
        // By name -> by id.
        core::SaveKey(dir, "name_Old", a);
        core::SaveTicket(dir, "name_Old", {}, idn::Ticket{});
        Check(core::MoveKeyFiledByName(dir, "B13E000000000003", "Old") && fs::exists(core::KeyPath(dir, "B13E000000000003")) &&
                  !fs::exists(core::KeyPath(dir, "name_Old")) && fs::exists(core::TicketPath(dir, "B13E000000000003")),
              "a key filed by name moves to the profile's id with its tickets");
        Check(!core::MoveKeyFiledByName(dir, "B13E000000000003", "Old"), "nothing moves twice");
        Check(!core::MoveKeyFiledByName(dir, "name_Neo", "Neo"), "a profile with no id of its own has nothing to move");
    }

    // Tickets.
    {
        const fs::path dir = tmp / "tickets";
        uint8_t master[32], other[32], pub[32], pub2[32];
        for (int i = 0; i < 32; ++i) {
            master[i] = uint8_t(i);
            other[i] = uint8_t(100 + i);
            pub[i] = uint8_t(50 + i);
            pub2[i] = uint8_t(70 + i);
        }
        idn::Ticket t{};
        t.body.version = 1;
        t.body.nick_len = 3;
        std::memcpy(t.body.nick, "Neo", 3);
        std::memcpy(t.body.pubkey, pub, 32);
        t.body.expires = 123456789;
        idn::Ticket got{};
        Check(!core::LoadTicket(dir, "X", master, pub, got), "no ticket file, no ticket");
        core::SaveTicket(dir, "X", master, t);
        Check(core::LoadTicket(dir, "X", master, pub, got) && std::memcmp(&got, &t, sizeof(t)) == 0, "a saved ticket comes back");
        Check(!core::LoadTicket(dir, "X", master, pub2, got), "a ticket for another key is not this key's");
        Check(!core::LoadTicket(dir, "X", other, pub, got), "a ticket of another master is not found");
        core::SaveTicket(dir, "X", other, t);
        CheckInt(int64_t(core::ReadTicketFile(dir, "X").size()), 2, "one ticket per master");
        core::ForgetTickets(dir, "X");
        Check(core::ReadTicketFile(dir, "X").empty(), "ForgetTickets removes the file");
    }

    // The backup file.
    {
        const std::string golden = FromHexText(kGoldenBackup);
        CheckInt(int64_t(golden.size()), 182, "golden backup size");
        core::KeyRecord seeded;
        for (int i = 0; i < 32; ++i) seeded.seed[i] = uint8_t(i);
        seeded.has_rmr = true;
        for (int i = 0; i < 16; ++i) seeded.rmr[size_t(i)] = uint8_t(0xA0 + i);
        seeded.pending_code = "ABCDE-FGHJK-MNPQR-STVWX";  // never goes into a backup
        seeded.code_remind = 99;
        core::BackupRandom fixed;
        for (int i = 0; i < 16; ++i) fixed.salt[i] = uint8_t(0x10 + i);
        for (int i = 0; i < 24; ++i) fixed.nonce[i] = uint8_t(0x30 + i);
        std::string made;
        Check(core::MakeBackup(seeded, "correct horse battery staple", made, &fixed) == core::BackupResult::kOk, "MakeBackup works");
        Check(made == golden, "a backup is byte for byte what the code before the split made");
        core::KeyRecord opened;
        Check(core::OpenBackup(golden, "correct horse battery staple", opened) == core::BackupResult::kOk, "the old backup opens");
        Check(opened.SameKeyAs(seeded) && opened.has_rmr && opened.rmr == seeded.rmr && opened.pending_code.empty(),
              "the old backup holds the key and the token, no code");
        Check(core::OpenBackup(golden, "correct horse battery stapl", opened) == core::BackupResult::kWrongPassword, "a wrong password");
        std::string damaged = golden;
        damaged[100] = char(damaged[100] ^ 1);
        Check(core::OpenBackup(damaged, "correct horse battery staple", opened) == core::BackupResult::kWrongPassword, "a damaged file");
        Check(core::OpenBackup(golden.substr(0, 70), "correct horse battery staple", opened) == core::BackupResult::kWrongPassword ||
                  core::OpenBackup(golden.substr(0, 70), "correct horse battery staple", opened) == core::BackupResult::kNotABackup,
              "a cut-off file is refused");
        Check(core::OpenBackup(golden.substr(0, 40), "x", opened) == core::BackupResult::kNotABackup, "too short is not a backup");
        Check(core::OpenBackup("LRIDBAK2" + golden.substr(8), "correct horse battery staple", opened) == core::BackupResult::kNotABackup,
              "another magic is not a backup");
        std::string other;
        Check(core::MakeBackup(seeded, "short", other) == core::BackupResult::kPasswordTooShort, "a password under 8 bytes is refused");
        Check(core::MakeBackup(seeded, "12345678", other) == core::BackupResult::kOk, "8 bytes is enough");
        std::string again;
        core::MakeBackup(seeded, "12345678", again);
        Check(other != again, "two backups of one key differ (random salt and nonce)");
        CheckEq(core::BackupText(core::BackupResult::kPasswordTooShort), "use a password of at least 8 characters", "the game's text for a short password");
        CheckEq(core::BackupText(core::BackupResult::kNotABackup), "not a LARecomp identity backup", "the game's text for a wrong file");
        CheckEq(core::BackupText(core::BackupResult::kWrongPassword), "wrong password, or the file is damaged", "the game's text for a wrong password");
        CheckEq(core::BackupFileName("a<b>c:d"), "LARecomp identity - a_b_c_d.lrid", "a backup file name drops what a file name cannot hold");
    }
}

// ---- the closed-game rule -----------------------------------------------------------------------------------------

void TestGuard(const fs::path& tmp, const std::string& self_exe) {
    const fs::path exe = tmp / "guard" / "larecomp.exe";
    const fs::path runtime = tmp / "runtime";
#ifndef _WIN32
    setenv("XDG_RUNTIME_DIR", runtime.string().c_str(), 1);
#endif
    ot::GameLook look = ot::LookForGame(exe.string());
    Check(!look.running && !look.unsure, "no game of this exe: not running");
    {
        FakeGame game(exe, runtime);
        look = ot::LookForGame(exe.string());
        Check(look.running && look.how == "marker", "a game that holds the marker is running");
#ifdef _WIN32
        // The marker name does not care about the case of the path (Windows).
        std::string upper = exe.string();
        for (char& c : upper) c = (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c;
        Check(ot::LookForGame(upper).running, "the same path in another case is the same game");
        std::string slashes = exe.string();
        for (char& c : slashes) c = c == '\\' ? '/' : c;
        Check(ot::LookForGame(slashes).running, "the same path with forward slashes is the same game");
#endif
        const fs::path other = tmp / "guard2" / "larecomp.exe";
        Check(!ot::LookForGame(other.string()).running, "another install's game is not this one");
    }
    look = ot::LookForGame(exe.string());
    Check(!look.running, "the marker is gone when the game ends");
    // A game with no marker is found in the process list: this very program is a process of its own file.
    look = ot::LookForGame(self_exe);
    Check(look.running && look.how == "process", "a process of the program file is found without a marker");
}

// ---- the commands on files ----------------------------------------------------------------------------------------

void TestShow(const fs::path& tmp) {
    const fs::path root = tmp / "show";
    TestProfile p = MakeProfile(root, "Neo", 1, true, "ABCDE-FGHJK-MNPQR-STVWX");
    Run r = Id(With({"show"}, {"--identity-dir", p.identity_dir.string(), "--saves", p.saves.string(), "--gamertag", "Neo"}));
    Check(r.ok(), "show works");
    CheckEq(r.doc.Str("action"), "show", "show: action");
    CheckEq(r.doc.Str("profile.id"), p.id, "show: profile id from profile.xuid");
    CheckEq(r.doc.Str("profile.gamertag"), "Neo", "show: gamertag");
    CheckEq(r.doc.Str("profile.filed_by"), "id", "show: filed by id");
    CheckEq(r.doc.Str("key.state"), "ok", "show: key state");
    CheckEq(r.doc.Str("key.fingerprint"), p.fingerprint, "show: fingerprint");
    {
        core::KeyRecord rec;
        core::LoadKey(p.identity_dir, p.id, rec);
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(rec.Xuid()));
        CheckEq(r.doc.Str("key.xuid"), buf, "show: the XUID the key gives");
    }
    Check(r.doc.IsTrue("key.rmr_token"), "show: says there is a Rate My Ride token");
    Check(r.doc.IsTrue("recovery.pending"), "show: says a recovery code waits");
    CheckInt(r.doc.Num("recovery.remind_after"), 0, "show: reminder 0 = now");
    Check(r.doc.Get("tickets") && r.doc.Get("tickets")->t == JV::kArr && r.doc.Get("tickets")->a.empty(), "show: no tickets yet");
    CheckEq(r.doc.Str("key.protection"), core::Protection(), "show: protection");
    // The order of the fields is part of the schema the launcher reads.
    std::string order;
    for (const auto& k : r.doc.keys) order += k + ",";
    CheckEq(order, "ok,command,action,schema,profile,key,recovery,tickets,", "show: top-level fields in order");
    // No secret.
    Check(r.r.json.find(p.seed_hex) == std::string::npos, "show: the seed is not in the document");
    Check(r.r.json.find(p.rmr_hex) == std::string::npos, "show: the token is not in the document");
    Check(r.r.json.find("ABCDE") == std::string::npos, "show: the recovery code is not in the document");

    // By id instead of by saves.
    Run by_id = Id({"show", "--identity-dir", p.identity_dir.string(), "--profile", p.id});
    Check(by_id.ok() && by_id.doc.IsNull("profile.gamertag"), "show by --profile alone has no gamertag");
    Run lower = Id({"show", "--identity-dir", p.identity_dir.string(), "--profile", "b13e000000001077", "--gamertag", "Neo"});
    CheckEq(lower.doc.Str("profile.id"), "B13E000000001077", "a lower-case id is made upper case");

    // A ticket shows (public: master, nick, expiry, whether it is good and for this key).
    {
        const uint8_t master_seed[32] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                                         17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
        const idn::KeyPair master = idn::KeyPairFromSeed(master_seed);
        core::KeyRecord rec;
        core::LoadKey(p.identity_dir, p.id, rec);
        const idn::KeyPair mine = rec.Keys();
        idn::Ticket t{};
        t.body.version = idn::kTicketVersion;
        t.body.nick_len = 3;
        std::memcpy(t.body.nick, "Neo", 3);
        std::memcpy(t.body.pubkey, mine.pub, 32);
        t.body.expires = uint64_t(idn::NowUnix() + 3600);
        idn::Sign(t.sig, master, idn::TicketMessage(t.body));
        core::SaveTicket(p.identity_dir, p.id, master.pub, t);
        Run s = Id(With({"show"}, {"--identity-dir", p.identity_dir.string(), "--profile", p.id}));
        const JV* tickets = s.doc.Get("tickets");
        Check(tickets && tickets->a.size() == 1, "show: one ticket");
        if (tickets && tickets->a.size() == 1) {
            const JV& t0 = tickets->a[0];
            CheckEq(t0.Str("nick"), "Neo", "ticket nick");
            Check(t0.IsTrue("valid") && t0.IsTrue("for_this_key"), "the ticket is good and for this key");
            CheckEq(t0.Str("master"), idn::Fingerprint(master.pub), "the master is shown by its fingerprint");
        }
    }

    // A key filed by name is found through the gamertag.
    {
        TestProfile q = MakeProfile(root, "Legacy", 2);
        fs::rename(core::KeyPath(q.identity_dir, q.id), core::KeyPath(q.identity_dir, "name_Legacy"));
        Run s = Id(With({"show"}, q.Who()));
        CheckEq(s.doc.Str("key.state"), "ok", "a key still filed by name is read");
        CheckEq(s.doc.Str("profile.filed_by"), "name", "and says it is filed by name");
    }
    // Missing, unreadable, not a profile.
    {
        TestProfile q = MakeProfile(root, "Ghost", 3);
        fs::remove(core::KeyPath(q.identity_dir, q.id));
        Run s = Id(With({"show"}, q.Who()));
        Check(s.ok() && s.doc.Str("key.state") == "missing" && s.doc.IsNull("key.fingerprint") && s.doc.IsNull("recovery.pending"),
              "show: a profile with no key yet says so");
        std::ofstream(core::KeyPath(q.identity_dir, q.id), std::ios::binary) << "not a key";
        s = Id(With({"show"}, q.Who()));
        Check(s.ok() && s.doc.Str("key.state") == "unreadable", "show: an unreadable key says so");
        Run none = Id({"show", "--identity-dir", q.identity_dir.string(), "--saves", q.saves.string(), "--gamertag", "Nobody"});
        CheckEq(none.code(), "profile_not_found", "a gamertag with no profile.xuid");
        CheckInt(none.r.exit_code, 1, "profile_not_found exits 1");
    }
    // Bad ids and names never reach a file name.
    {
        Run s = Id({"show", "--identity-dir", p.identity_dir.string(), "--profile", "../../evil"});
        CheckEq(s.code(), "bad_profile", "a path as a profile id");
        s = Id({"show", "--identity-dir", p.identity_dir.string(), "--saves", p.saves.string(), "--gamertag", "a/b"});
        CheckEq(s.code(), "bad_gamertag", "a path as a gamertag");
        s = Id({"show", "--identity-dir", p.identity_dir.string(), "--saves", p.saves.string(), "--gamertag", "0123456789ABCDEFG"});
        CheckEq(s.code(), "bad_gamertag", "a gamertag over 15 bytes");
    }
}

void TestUsage() {
    struct Case {
        std::vector<std::string> args;
        const char* what;
    };
    const std::vector<Case> cases = {
        {{}, "no command"},
        {{"fly"}, "unknown command"},
        {{"show"}, "no profile named"},
        {{"show", "--profile"}, "option without value"},
        {{"show", "--profile", "B13E000000000001", "--bogus"}, "unknown option"},
        {{"show", "--profile", "B13E000000000001", "--profile", "B13E000000000002"}, "option given twice"},
        {{"show", "--profile", "B13E000000000001", "extra"}, "extra word"},
        {{"show", "--profile", "B13E000000000001", "--pretty=yes"}, "flag with a value"},
        {{"show", "--profile", "B13E000000000001", "--claim"}, "an option of another command"},
        {{"backup", "--profile", "B13E000000000001", "--file", "x", "--password", "hunter22"}, "a password on the command line"},
        {{"recover", "--profile", "B13E000000000001", "--gamertag", "N", "--server", "h", "--code", "XXXXX"}, "a code on the command line"},
        {{"backup", "--profile", "B13E000000000001", "--file", "x"}, "backup without --password-stdin"},
        {{"backup", "--profile", "B13E000000000001", "--password-stdin"}, "backup without --file"},
        {{"restore", "--profile", "B13E000000000001", "--file", "x", "--password-stdin"}, "restore without --game-exe"},
        {{"check", "--profile", "B13E000000000001", "--gamertag", "N"}, "check without --server"},
        {{"check", "--profile", "B13E000000000001", "--server", "h"}, "check without a gamertag"},
        {{"check", "--profile", "B13E000000000001", "--gamertag", "N", "--server", "h", "--claim"}, "claim without --game-exe"},
        {{"check", "--profile", "B13E000000000001", "--gamertag", "N", "--server", "h", "--claim", "--refresh", "--game-exe", "g"}, "claim and refresh together"},
        {{"check", "--profile", "B13E000000000001", "--gamertag", "N", "--server", "h", "--timeout-ms", "3"}, "a timeout out of range"},
        {{"recovery"}, "recovery alone"},
        {{"recovery", "fly", "--profile", "B13E000000000001"}, "unknown recovery command"},
        {{"recovery", "remind", "--profile", "B13E000000000001", "--game-exe", "g"}, "remind with neither --days nor --never"},
        {{"recovery", "remind", "--profile", "B13E000000000001", "--game-exe", "g", "--days", "3", "--never"}, "remind with both"},
        {{"recovery", "remind", "--profile", "B13E000000000001", "--game-exe", "g", "--days", "0"}, "remind --days 0"},
        {{"recovery", "check", "--profile", "B13E000000000001"}, "recovery check without --code-stdin"},
        {{"rename", "--profile", "B13E000000000001", "--gamertag", "N", "--nick", "M", "--saves", "s", "--config", "c"}, "rename without --game-exe"},
        {{"rename", "--saves", "s", "--gamertag", "N", "--nick", "M", "--config", "c", "--game-exe", "g", "--server", "h"}, "rename --server without --claim"},
        {{"rename", "--saves", "s", "--gamertag", "N", "--nick", "M", "--config", "c", "--game-exe", "g", "--claim"}, "rename --claim without --server"},
    };
    for (const Case& c : cases) {
        Run r = Id(c.args);
        Check(r.r.exit_code == 2 && r.code() == "usage", std::string("usage error (exit 2): ") + c.what + " [" + r.r.json.substr(0, 160) + "]");
    }
    Check(ot::IdentityUsageText() != nullptr && std::strlen(ot::IdentityUsageText()) > 100, "there is a usage text");
    const auto actions = ot::IdentityActions();
    Check(std::find(actions.begin(), actions.end(), "rename") != actions.end() && std::find(actions.begin(), actions.end(), "recovery check") != actions.end(),
          "the action list names every action");
}

void TestBackupRestore(const fs::path& tmp) {
    const fs::path root = tmp / "bk";
    const fs::path outdir = tmp / "bk-out";
    fs::create_directories(outdir);
    TestProfile a = MakeProfile(root / "pcA", "Mira", 10, true, "ABCDE-FGHJK-MNPQR-STVWX");
    const std::string pw = "correct horse battery staple";
    g_secrets.push_back(pw);
    const fs::path file = outdir / "mira.lrid";

    // While the game runs a backup is allowed (it only reads).
    {
        FakeGame game(a.game_exe, tmp / "runtime");
        Run r = Id(With({"backup", "--password-stdin", "--file", file.string()}, {"--identity-dir", a.identity_dir.string(), "--profile", a.id}), {pw});
        Check(r.ok(), "a backup is allowed while the game runs (it only reads)");
        Check(fs::exists(file), "the backup file is there");
        Check(r.doc.IsTrue("verified") && r.doc.IsFalse("includes.recovery_code") && r.doc.IsTrue("includes.rmr_token") && r.doc.IsTrue("includes.key"),
              "the backup says what it holds and that it was read back");
        CheckEq(r.doc.Str("fingerprint"), a.fingerprint, "the backup's key fingerprint");
        CheckInt(r.doc.Num("bytes"), int64_t(fs::file_size(file)), "the backup's size");
        Check(r.doc.Str("file") != "<missing>" && r.doc.Str("file").size() > 5, "the backup's path is in the document");
    }
    // The file is the game's format.
    {
        core::KeyRecord opened;
        const std::string bytes = ReadAll(file);
        Check(core::OpenBackup(bytes, pw, opened) == core::BackupResult::kOk && opened.SameKeyAs([&] {
                  core::KeyRecord k;
                  core::LoadKey(a.identity_dir, a.id, k);
                  return k;
              }()),
              "the tool's backup opens with the core and holds this key");
        Check(opened.pending_code.empty(), "the backup holds no recovery code");
        Check(bytes.find("ABCDE") == std::string::npos, "the code is not in the file in the clear");
    }
    // Refusals.
    {
        Run r = Id(With({"backup", "--password-stdin", "--file", file.string()}, {"--identity-dir", a.identity_dir.string(), "--profile", a.id}), {pw});
        CheckEq(r.code(), "file_exists", "a second backup does not overwrite");
        r = Id(With({"backup", "--password-stdin", "--overwrite", "--file", file.string()}, {"--identity-dir", a.identity_dir.string(), "--profile", a.id}), {pw});
        Check(r.ok(), "--overwrite replaces it");
        r = Id(With({"backup", "--password-stdin", "--file", (a.identity_dir / "x.lrid").string()}, {"--identity-dir", a.identity_dir.string(), "--profile", a.id}), {pw});
        CheckEq(r.code(), "bad_file", "a backup is not written into the identity folder");
        r = Id(With({"backup", "--password-stdin", "--file", outdir.string()}, {"--identity-dir", a.identity_dir.string(), "--profile", a.id}), {pw});
        CheckEq(r.code(), "bad_file", "a folder is not a backup file");
        r = Id(With({"backup", "--password-stdin", "--file", (outdir / "s.lrid").string()}, {"--identity-dir", a.identity_dir.string(), "--profile", a.id}), {"short"});
        CheckEq(r.code(), "password_too_short", "a short password");
        Check(!fs::exists(outdir / "s.lrid"), "no file for a short password");
        r = Id(With({"backup", "--password-stdin", "--file", (outdir / "s.lrid").string()}, {"--identity-dir", a.identity_dir.string(), "--profile", a.id}), {});
        CheckEq(r.code(), "secret_missing", "nothing on stdin");
        r = Id(With({"backup", "--password-stdin", "--file", (outdir / "s.lrid").string()}, {"--identity-dir", a.identity_dir.string(), "--profile", a.id}), {std::string(2000, 'x')});
        CheckEq(r.code(), "secret_too_long", "a password line over 1024 bytes");
        r = Id(With({"backup", "--password-stdin", "--file", (outdir / "bom.lrid").string()}, {"--identity-dir", a.identity_dir.string(), "--profile", a.id}), {"\xEF\xBB\xBF" + pw});
        Check(r.ok(), "a byte order mark before the password is ignored");
        core::KeyRecord o;
        Check(core::OpenBackup(ReadAll(outdir / "bom.lrid"), pw, o) == core::BackupResult::kOk, "...and the password is the one without it");
        const fs::path nokey = root / "pcA" / "none";
        r = Id({"backup", "--password-stdin", "--file", (outdir / "s.lrid").string(), "--identity-dir", nokey.string(), "--profile", "B13E000000000099"}, {pw});
        CheckEq(r.code(), "no_key", "no key, no backup");
    }

    // Restore on another PC (a profile with its own, different, new key).
    TestProfile b = MakeProfile(root / "pcB", "Mira", 20, false);
    {
        Run r = Id(With({"restore", "--password-stdin", "--file", file.string()}, b.Closed()), {pw});
        CheckEq(r.code(), "would_replace_key", "restoring over another key asks first");
        CheckEq(r.doc.Str("error.current_fingerprint"), b.fingerprint, "...and shows the current key's fingerprint");
        CheckEq(r.doc.Str("error.backup_fingerprint"), a.fingerprint, "...and the backup's");
        core::KeyRecord still;
        core::LoadKey(b.identity_dir, b.id, still);
        CheckEq(still.Fingerprint(), b.fingerprint, "nothing changed");
    }
    {
        FakeGame game(b.game_exe, tmp / "runtime");
        Run r = Id(With({"restore", "--password-stdin", "--replace", "--file", file.string()}, b.Closed()), {pw});
        CheckEq(r.code(), "game_running", "restore refuses while the game runs");
        Check(r.doc.IsTrue("error.running"), "...and says it runs");
        core::KeyRecord still;
        core::LoadKey(b.identity_dir, b.id, still);
        CheckEq(still.Fingerprint(), b.fingerprint, "the key is untouched when the game runs");
    }
    {
        Run r = Id(With({"restore", "--password-stdin", "--replace", "--file", file.string()}, b.Closed()), {"wrong password here"});
        CheckEq(r.code(), "wrong_password", "a wrong password");
        r = Id(With({"restore", "--password-stdin", "--replace", "--file", (outdir / "missing.lrid").string()}, b.Closed()), {pw});
        CheckEq(r.code(), "bad_file", "a file that is not there");
        std::ofstream(outdir / "notabackup.lrid", std::ios::binary) << "this is just text, long enough to pass the size check and more";
        r = Id(With({"restore", "--password-stdin", "--replace", "--file", (outdir / "notabackup.lrid").string()}, b.Closed()), {pw});
        CheckEq(r.code(), "not_a_backup", "a file that is not a backup");
        core::KeyRecord still;
        core::LoadKey(b.identity_dir, b.id, still);
        CheckEq(still.Fingerprint(), b.fingerprint, "refused restores change nothing");
    }
    core::SaveTicket(b.identity_dir, b.id, {}, idn::Ticket{});
    {
        Run r = Id(With({"restore", "--password-stdin", "--replace", "--file", file.string()}, b.Closed()), {pw});
        Check(r.ok(), "restore with --replace works");
        CheckEq(r.doc.Str("fingerprint"), a.fingerprint, "the restored key is the backup's");
        CheckEq(r.doc.Str("previous_fingerprint"), b.fingerprint, "the old key's fingerprint is shown");
        Check(r.doc.IsTrue("restart_needed"), "restart is needed");
        const std::string kept = r.doc.Str("previous_kept_as");
        Check(kept != "<missing>" && fs::exists(fs::path(std::u8string(reinterpret_cast<const char8_t*>(kept.data()), kept.size()))),
              "the old key file is kept beside the new one");
        core::KeyRecord now;
        Check(core::LoadKey(b.identity_dir, b.id, now) == core::Load::kLoaded && now.Fingerprint() == a.fingerprint, "the key file holds the backup's key");
        Check(now.has_rmr && idn::ToHex(now.rmr.data(), 16) == a.rmr_hex, "the Rate My Ride token is the backup's");
        Check(now.pending_code.empty(), "no recovery code after a restore");
        Check(core::ReadTicketFile(b.identity_dir, b.id).empty(), "the tickets of the old key are forgotten");
        // The key of both is now the same: restoring again changes nothing and needs no --replace.
        // The key that is already there keeps its recovery code and tickets (they are still good).
        core::KeyRecord same;
        core::LoadKey(b.identity_dir, b.id, same);
        same.pending_code = "CDEFG-HJKMN-PQRST-VWXYZ";
        core::SaveKey(b.identity_dir, b.id, same);
        core::SaveTicket(b.identity_dir, b.id, {}, idn::Ticket{});
        g_secrets.push_back(same.pending_code);
        r = Id(With({"restore", "--password-stdin", "--file", file.string()}, b.Closed()), {pw});
        Check(r.ok() && r.doc.IsNull("previous_kept_as"), "restoring the key that is already there needs no --replace and keeps nothing");
        core::KeyRecord after;
        Check(core::LoadKey(b.identity_dir, b.id, after) == core::Load::kLoaded && after.pending_code == "CDEFG-HJKMN-PQRST-VWXYZ",
              "...and its waiting recovery code stays");
        Check(!core::ReadTicketFile(b.identity_dir, b.id).empty(), "...and so do its tickets");
    }
    // A backup with no token does not throw away the token the profile has.
    {
        TestProfile g = MakeProfile(root / "pcG", "Mira", 60, true);
        core::KeyRecord src;
        for (int i = 0; i < 32; ++i) src.seed[i] = uint8_t(200 + i);
        std::string bytes;
        core::MakeBackup(src, pw, bytes);
        const fs::path notoken = outdir / "notoken.lrid";
        std::ofstream(notoken, std::ios::binary) << bytes;
        Run r = Id(With({"restore", "--password-stdin", "--replace", "--file", notoken.string()}, g.Closed()), {pw});
        Check(r.ok(), "a backup without a token restores");
        core::KeyRecord now;
        core::LoadKey(g.identity_dir, g.id, now);
        Check(now.SameKeyAs(src) && now.has_rmr && idn::ToHex(now.rmr.data(), 16) == g.rmr_hex, "...and the profile keeps its own token");
    }
    // A profile with no key at all, and one whose key cannot be read.
    {
        TestProfile c = MakeProfile(root / "pcC", "Mira", 30, false);
        fs::remove(core::KeyPath(c.identity_dir, c.id));
        Run r = Id(With({"restore", "--password-stdin", "--file", file.string()}, c.Closed()), {pw});
        Check(r.ok() && r.doc.IsNull("previous_fingerprint") && r.doc.IsNull("previous_kept_as"), "restore into a profile with no key needs no --replace");
        TestProfile d = MakeProfile(root / "pcD", "Mira", 40, false);
        std::ofstream(core::KeyPath(d.identity_dir, d.id), std::ios::binary | std::ios::trunc) << "damaged";
        r = Id(With({"restore", "--password-stdin", "--file", file.string()}, d.Closed()), {pw});
        Check(r.ok(), "restore replaces an unreadable key (kept aside as .unreadable)");
        Check(fs::exists(core::KeyPath(d.identity_dir, d.id).string() + ".unreadable"), "the unreadable file is kept");
    }
    // A pre-split golden backup restores.
    {
        const fs::path gold = outdir / "golden.lrid";
        const std::string bytes = FromHexText(kGoldenBackup);
        std::ofstream(gold, std::ios::binary) << bytes;
        TestProfile e = MakeProfile(root / "pcE", "Mira", 50, false);
        Run r = Id(With({"restore", "--password-stdin", "--replace", "--file", gold.string()}, e.Closed()), {pw});
        Check(r.ok(), "a backup made by the code before the split restores");
        core::KeyRecord now;
        core::LoadKey(e.identity_dir, e.id, now);
        Check(now.has_rmr && now.rmr[0] == 0xA0 && now.seed[31] == 31, "...with its key and token");
    }
}

void TestRecoveryCode(const fs::path& tmp) {
    const fs::path root = tmp / "rc";
    const std::string code = "ABCDE-FGHJK-MNPQR-STVWX";
    TestProfile p = MakeProfile(root, "Kit", 11, true, code);
    g_secrets.push_back(code);

    // show prints the code: the one secret the tool prints, because it was asked for.
    Run s = Id(With({"recovery", "show"}, p.Who()), {}, true);
    Check(s.ok() && s.doc.IsTrue("recovery.pending") && s.doc.Str("recovery.code") == code, "recovery show gives the waiting code");

    // check: the format and whether it matches what waits.
    Run c = Id(With({"recovery", "check", "--code-stdin"}, p.Who()), {"abcde fghjk mnpqr stvwx"});
    Check(c.ok() && c.doc.IsTrue("recovery.valid_format") && c.doc.IsTrue("recovery.matches_pending"),
          "a code typed in lower case with spaces matches");
    c = Id(With({"recovery", "check", "--code-stdin"}, p.Who()), {"ABCDE-FGHJK-MNPQR-STVWY"});
    Check(c.doc.IsTrue("recovery.valid_format") && c.doc.IsFalse("recovery.matches_pending"), "a different valid code does not match");
    c = Id(With({"recovery", "check", "--code-stdin"}, p.Who()), {"not a code"});
    Check(c.doc.IsFalse("recovery.valid_format"), "text that is no code");
    c = Id(With({"recovery", "check", "--code-stdin"}, p.Who()), {"ABCDE-FGHJK-MNPQR-STVWU"});
    Check(c.doc.IsFalse("recovery.valid_format"), "a code with a letter that is not in the alphabet (U)");
    c = Id(With({"recovery", "check", "--code-stdin"}, p.Who()), {"oiabc-defgh-jkmnp-qrstv"});
    Check(c.doc.IsTrue("recovery.valid_format"), "O and I are read as 0 and 1, as the master does");
    c = Id(With({"recovery", "check", "--code-stdin"}, p.Who()), {code});
    Check(c.r.json.find("ABCDE") == std::string::npos, "recovery check never prints a code");

    // remind
    {
        FakeGame game(p.game_exe, tmp / "runtime");
        Run r = Id(With({"recovery", "confirm"}, p.Closed()));
        CheckEq(r.code(), "game_running", "confirm refuses while the game runs");
        r = Id(With({"recovery", "remind", "--days", "5"}, p.Closed()));
        CheckEq(r.code(), "game_running", "remind refuses while the game runs");
        Check(Id(With({"recovery", "show"}, p.Who()), {}, true).doc.IsTrue("recovery.pending"), "the code still waits");
    }
    const int64_t before = idn::NowUnix();
    Run r = Id(With({"recovery", "remind", "--days", "5"}, p.Closed()));
    Check(r.ok() && r.doc.Num("remind_after") >= before + 5 * 86400 && r.doc.Num("remind_after") <= before + 5 * 86400 + 60, "remind in 5 days");
    r = Id(With({"recovery", "remind", "--never"}, p.Closed()));
    CheckInt(r.doc.Num("remind_after"), -1, "remind never is -1 (the game's kNeverRemind)");
    Run sh = Id(With({"show"}, p.Who()));
    CheckInt(sh.doc.Num("recovery.remind_after"), -1, "show reports the reminder");

    // confirm
    r = Id(With({"recovery", "confirm", "--code-stdin"}, p.Closed()), {"ZZZZZ-ZZZZZ-ZZZZZ-ZZZZZ"});
    CheckEq(r.code(), "code_mismatch", "confirm with a code that is not the waiting one");
    Check(Id(With({"recovery", "show"}, p.Who()), {}, true).doc.IsTrue("recovery.pending"), "a refused confirm keeps the code");
    r = Id(With({"recovery", "confirm", "--code-stdin"}, p.Closed()), {code});
    Check(r.ok() && r.doc.IsTrue("confirmed"), "confirm with the code typed back");
    Run after = Id(With({"recovery", "show"}, p.Who()), {}, true);
    Check(after.doc.IsFalse("recovery.pending") && after.doc.IsNull("recovery.code"), "the code is gone after confirm");
    r = Id(With({"recovery", "confirm"}, p.Closed()));
    CheckEq(r.code(), "no_pending_code", "confirm with nothing waiting");
    r = Id(With({"recovery", "remind", "--days", "2"}, p.Closed()));
    CheckEq(r.code(), "no_pending_code", "remind with nothing waiting");
    // The key and token survive the rewrite.
    core::KeyRecord rec;
    Check(core::LoadKey(p.identity_dir, p.id, rec) == core::Load::kLoaded && idn::ToHex(rec.seed, 32) == p.seed_hex && rec.has_rmr, "the key and token survive confirm");
    // confirm without typing the code back
    TestProfile q = MakeProfile(root, "Kit2", 12, true, "BCDEF-GHJKM-NPQRS-TVWXY");
    r = Id(With({"recovery", "confirm"}, q.Closed()));
    Check(r.ok(), "confirm without typing the code back");
}

void TestRename(const fs::path& tmp) {
    const fs::path root = tmp / "rn";
    TestProfile p = MakeProfile(root, "Quill", 13);
    auto base = [&](const std::string& nick) {
        std::vector<std::string> a = {"rename", "--nick", nick, "--config", p.config.string()};
        for (const auto& x : p.Closed()) a.push_back(x);
        return a;
    };
    CheckEq(Id(base("bad/name")).code(), "nick_not_allowed", "rename: a path character");
    CheckEq(Id(base("bad/name")).doc.Str("error.message"), "Use only letters, digits and spaces, up to 15 characters.", "rename: the game's sentence for it");
    CheckEq(Id(base("cache")).doc.Str("error.message"), "That name is reserved.", "rename: a reserved name");
    CheckEq(Id(base("QUILL")).doc.Str("error.message"), "That is already your gamertag.", "rename: the same name in another case");
    fs::create_directories(p.saves / "Taken");
    CheckEq(Id(base("Taken")).doc.Str("error.message"), "You already have a save with that name.", "rename: a save of that name exists");
    CheckEq(Id(base("This name is far too long")).code(), "nick_not_allowed", "rename: too long");
    Check(!fs::exists(p.config), "refused renames wrote no config");
    // The rules are the game's: the same function answers.
    CheckEq(idn::RenameProblem("Fine Name", "Quill", p.saves), "", "RenameProblem: a good name");
    CheckEq(idn::RenameProblem("Fine Name", "Quill", ""), "The save folder is not known; change user_name by hand.", "RenameProblem: no saves folder");
    CheckEq(idn::RenameProblem("Fine Name", "Nobody Here", p.saves), "Your save folder was not found; change user_name by hand.", "RenameProblem: the current save folder is not there");

    {
        FakeGame game(p.game_exe, tmp / "runtime");
        CheckEq(Id(base("Quill Two")).code(), "game_running", "rename refuses while the game runs");
        Check(!fs::exists(p.config) && !fs::exists(p.saves / ".profile"), "...and writes nothing");
    }
    Run r = Id(base("Quill Two"));
    Check(r.ok(), "rename works");
    CheckEq(r.doc.Str("from"), "Quill", "rename: from");
    CheckEq(r.doc.Str("to"), "Quill Two", "rename: to");
    Check(r.doc.IsFalse("claimed") && r.doc.IsTrue("restart_needed") && r.doc.IsNull("key_moved"), "rename without --claim: not claimed, restart needed");
    const std::string toml = ReadAll(p.config);
    Check(toml.find("user_name = \"Quill Two\"") != std::string::npos, "larecomp.toml has user_name");
    std::string marker = ReadAll(p.saves / ".profile");
    while (!marker.empty() && (marker.back() == '\n' || marker.back() == '\r')) marker.pop_back();
    CheckEq(marker, "Quill", "the save marker names the folder in use now (the SDK renames it at the next start)");
    // The key stays with the profile id.
    core::KeyRecord kept;
    Check(core::LoadKey(p.identity_dir, p.id, kept) == core::Load::kLoaded && kept.Fingerprint() == p.fingerprint, "the key file is still the profile's");
}

// ---- the commands that talk to a relay and a master ---------------------------------------------------------------

void TestNetwork(const fs::path& tmp) {
    const fs::path root = tmp / "net";
    idtest::FakeMaster master;
    Check(master.Start(root / "master", 3), "the nick master starts (the real registry, a 3 second hold)");
    idtest::FakeRelay relay;
    relay.Configure(master.public_key(), master.port(), true);
    relay.Start();
    const std::string server = relay.Address();
    master.SetPeer("10.0.1.1");

    auto check_args = [&](const TestProfile& p, std::initializer_list<std::string> more) {
        std::vector<std::string> a = {"check", "--server", server};
        for (const auto& x : p.Closed()) a.push_back(x);
        a.insert(a.end(), more.begin(), more.end());
        return a;
    };
    auto recover_args = [&](const TestProfile& p, const std::string& srv, std::initializer_list<std::string> more) {
        std::vector<std::string> a = {"recover", "--server", srv, "--code-stdin"};
        for (const auto& x : p.Who()) a.push_back(x);
        a.insert(a.end(), more.begin(), more.end());
        return a;
    };
    auto cancel_args = [&](const TestProfile& p) {
        std::vector<std::string> a = {"cancel", "--server", server};
        for (const auto& x : p.Closed()) a.push_back(x);
        return a;
    };
    auto rename_args = [&](const TestProfile& p, const std::string& nick) {
        std::vector<std::string> a = {"rename", "--nick", nick, "--config", p.config.string(), "--claim", "--server", server};
        for (const auto& x : p.Closed()) a.push_back(x);
        return a;
    };
    auto show_code = [&](const TestProfile& p) {
        Run r = Id(With({"recovery", "show"}, p.Who()), {}, true);
        const std::string code = r.doc.Str("recovery.code");
        if (CodeShape(code)) g_secrets.push_back(code);
        return code;
    };

    // ---- a nick is claimed only when asked ----
    TestProfile A = MakeProfile(root / "pcA", "Nova", 60);
    Run r = Id(check_args(A, {}));
    Check(r.ok(), "check works");
    CheckEq(r.doc.Str("status.state"), "needs_claim", "a check with no ticket says the nick needs a claim");
    CheckEq(r.doc.Str("claim.mode"), "never", "a plain check never claims");
    Check(r.doc.IsFalse("claim.asked_master"), "a plain check does not ask the master");
    CheckInt(master.posts(), 0, "the master saw nothing");
    Check(relay.hellos() >= 1, "the relay was asked");
    Check(!fs::exists(core::TicketPath(A.identity_dir, A.id)), "a plain check writes nothing");
    CheckEq(r.doc.Str("server.relay"), relay.CanonicalName(), "the relay is named as it is signed");
    CheckEq(r.doc.Str("server.master"), master.Address(), "the master the relay names is shown");

    {
        FakeGame game(A.game_exe, tmp / "runtime");
        r = Id(check_args(A, {"--claim"}));
        CheckEq(r.code(), "game_running", "claiming refuses while the game runs");
        CheckInt(master.posts(), 0, "...before anything is asked of the master");
        r = Id(check_args(A, {"--refresh"}));
        CheckEq(r.code(), "game_running", "so does a refresh");
    }

    r = Id(check_args(A, {"--claim"}));
    Check(r.ok(), "check --claim works");
    CheckEq(r.doc.Str("status.state"), "verified", "the claim registers the nick and the relay admits it");
    CheckEq(r.doc.Str("status.master_status"), "ok", "the master said ok");
    Check(r.doc.IsTrue("claim.asked_master") && r.doc.IsTrue("claim.ticket_stored") && r.doc.IsTrue("claim.recovery_code_pending") &&
              r.doc.IsFalse("claim.transferred"),
          "the claim stored a ticket and a recovery code");
    CheckInt(master.posts(), 1, "one request reached the master");
    CheckInt(int64_t(master.nicks()), 1, "one nick is registered");
    Check(relay.Admitted("Nova"), "the relay admitted Nova");
    {
        // The request the master got holds no secret.
        const std::string body = master.last_body();
        Check(body.find(A.seed_hex) == std::string::npos && body.find(A.rmr_hex) == std::string::npos, "the request to the master holds neither the seed nor the token");
        Check(body.find("op=claim") != std::string::npos && body.find("sig=") != std::string::npos, "it is a signed claim");
    }
    const std::string code_a = show_code(A);
    Check(CodeShape(code_a), "recovery show gives the code the master handed out");
    Check(r.r.json.find(code_a) == std::string::npos, "the claim document does not hold the code");
    Run shown = Id(With({"show"}, A.Who()));
    Check(shown.doc.Get("tickets") && shown.doc.Get("tickets")->a.size() == 1 && shown.doc.Get("tickets")->a[0].IsTrue("valid") &&
              shown.doc.Get("tickets")->a[0].IsTrue("for_this_key") && shown.doc.Get("tickets")->a[0].Str("nick") == "Nova",
          "show lists the ticket");
    {
        // The key and token are as they were (only the code was added).
        core::KeyRecord rec;
        Check(core::LoadKey(A.identity_dir, A.id, rec) == core::Load::kLoaded && idn::ToHex(rec.seed, 32) == A.seed_hex && rec.has_rmr &&
                  idn::ToHex(rec.rmr.data(), 16) == A.rmr_hex && rec.pending_code == code_a,
              "the claim kept the key and the token and added the code");
    }
    // A new claim while a code waits would replace it: refused.
    r = Id(check_args(A, {"--claim"}));
    CheckEq(r.code(), "recovery_code_unconfirmed", "a claim while a recovery code waits is refused");
    CheckInt(master.posts(), 1, "...without asking the master");

    // Typing the code back, then confirming.
    Check(Id(With({"recovery", "confirm", "--code-stdin"}, A.Closed()), {code_a}).ok(), "the code is confirmed");
    // A check with the ticket in hand proves the nick without asking the master.
    const int proofs_before = relay.proofs();
    r = Id(check_args(A, {}));
    CheckEq(r.doc.Str("status.state"), "verified", "with the ticket in hand a plain check verifies");
    Check(r.doc.IsFalse("claim.asked_master"), "...without asking the master");
    CheckInt(master.posts(), 1, "...the master still saw one request");
    CheckInt(relay.proofs(), proofs_before + 1, "...and the relay one more proof");

    // ---- someone else on another PC, the same nick ----
    TestProfile B = MakeProfile(root / "pcB", "Nova", 70);
    r = Id(check_args(B, {"--claim"}));
    Check(r.ok(), "a claim of a taken nick is an answer");
    CheckEq(r.doc.Str("status.state"), "taken", "the nick is taken");
    CheckEq(r.doc.Str("status.master_status"), "taken", "the master said taken");
    CheckEq(r.doc.Str("status.owner_nick"), "Nova", "the owner's spelling is shown");
    Check(r.doc.IsFalse("claim.ticket_stored") && r.doc.IsFalse("claim.recovery_code_pending"), "nothing was stored for the taker");
    Check(core::ReadTicketFile(B.identity_dir, B.id).empty(), "no ticket for the taker");
    CheckInt(master.posts(), 2, "the master saw the second request");

    // ---- recover ----
    {
        Run bad = Id(recover_args(B, server, {}), {"hello"});
        CheckEq(bad.code(), "bad_code_format", "a recovery code of the wrong shape is not sent");
        CheckInt(master.posts(), 2, "...the master saw nothing");
        Run wrong = Id(recover_args(B, server, {}), {"ZZZZZ-ZZZZZ-ZZZZZ-ZZZZZ"});
        CheckEq(wrong.code(), "bad_code", "a wrong recovery code");
        CheckEq(wrong.doc.Str("error.master_status"), "bad_code", "...is the master's bad_code");
        CheckInt(master.posts(), 3, "...it was sent");
        Run clock = Id(recover_args(B, server, {"--clock-offset-sec", "3600"}), {code_a});
        CheckEq(clock.code(), "bad_clock", "a clock an hour off");
        Check(clock.doc.Str("error.message").find("clock") != std::string::npos, "...and the sentence is about the clock");
        CheckInt(int64_t(master.nicks()), 1, "the nick is still the first player's");
        const int64_t t0 = idn::NowUnix();
        Run ok = Id(recover_args(B, server, {}), {code_a});
        Check(ok.ok(), "the right code is accepted");
        CheckEq(ok.doc.Str("result"), "pending", "the transfer is pending");
        Check(ok.doc.Num("until") >= t0 + 2 && ok.doc.Num("until") <= t0 + 10, "the master holds the nick for its hold time");
        CheckEq(ok.doc.Str("master"), master.Address(), "the master asked is the one the relay named");
        Check(ok.r.json.find(code_a) == std::string::npos, "the recover document does not hold the code");
        const std::string body = master.last_body();
        Check(body.find(A.seed_hex) == std::string::npos && body.find(B.seed_hex) == std::string::npos, "the recover request holds no seed");
    }

    // ---- the owner sees it (a refresh asks the master even with a good ticket) ----
    r = Id(check_args(A, {"--refresh"}));
    Check(r.ok(), "a refresh works");
    CheckEq(r.doc.Str("status.state"), "verified", "the owner is still verified");
    Check(r.doc.IsTrue("claim.asked_master"), "a refresh asks the master even with a good ticket");
    Check(r.doc.Num("status.incoming_until") > idn::NowUnix(), "...and learns that someone is moving the nick");
    Check(r.doc.Str("status.detail").find("someone asked") != std::string::npos, "...and says so");
    CheckEq(r.doc.Str("claim.mode"), "refresh", "claim mode");

    // ---- cancel ----
    TestProfile C = MakeProfile(root / "pcC", "Nova", 80);
    {
        Run nope = Id(cancel_args(C));
        CheckEq(nope.code(), "not_owner", "someone else cannot cancel");
        Run c = Id(cancel_args(A));
        Check(c.ok(), "the owner cancels the transfer");
        CheckEq(c.doc.Str("result"), "cancelled", "result");
        Check(c.doc.IsTrue("recovery_code_pending"), "a new recovery code is kept");
        const std::string code_a2 = show_code(A);
        Check(CodeShape(code_a2) && code_a2 != code_a, "the new code is not the old one");
        Check(c.r.json.find(code_a2) == std::string::npos, "the cancel document does not hold the new code");
        Run again = Id(cancel_args(A));
        CheckEq(again.code(), "nothing_pending", "a second cancel has nothing to stop");
        Run old = Id(recover_args(B, server, {}), {code_a});
        CheckEq(old.code(), "bad_code", "the old code no longer works");
        {
            FakeGame game(A.game_exe, tmp / "runtime");
            Run gr = Id(cancel_args(A));
            CheckEq(gr.code(), "game_running", "cancel refuses while the game runs");
        }
        Check(Id(With({"recovery", "confirm", "--code-stdin"}, A.Closed()), {code_a2}).ok(), "the new code is confirmed");

        // The new code, the hold, and the move.
        Run again2 = Id(recover_args(B, server, {}), {code_a2});
        Check(again2.ok() && again2.doc.Str("result") == "pending", "the new code starts a new transfer");
        Run early = Id(check_args(B, {"--claim"}));
        CheckEq(early.doc.Str("status.state"), "claim_pending", "a claim before the hold is over is pending");
        Check(early.doc.Num("status.claim_until") > idn::NowUnix(), "...until a time that is shown");
        std::this_thread::sleep_for(std::chrono::milliseconds(3500));
        Run claim_b = Id(check_args(B, {"--claim"}));
        Check(claim_b.ok(), "after the hold the taker claims");
        CheckEq(claim_b.doc.Str("status.state"), "verified", "...and the nick is theirs");
        Check(claim_b.doc.IsTrue("claim.transferred") && claim_b.doc.IsTrue("claim.recovery_code_pending") && claim_b.doc.IsTrue("claim.ticket_stored"),
              "...with a new recovery code and a ticket");
        Check(relay.Admitted("Nova"), "the relay admits Nova");
        const std::string code_b = show_code(B);
        Check(CodeShape(code_b) && code_b != code_a2, "the new owner has a code of their own");
        Run lost = Id(check_args(A, {"--refresh"}));
        CheckEq(lost.doc.Str("status.state"), "taken", "the old owner now finds the nick taken");
        // The recovery tries of the nick are counted (5 an hour): the 5th goes through, the 6th does not.
        Run fifth = Id(recover_args(A, server, {}), {"ZZZZZ-ZZZZZ-ZZZZZ-ZZZZY"});
        CheckEq(fifth.code(), "bad_code", "the old owner guesses (the 5th try of the hour for this nick)");
        Run sixth = Id(recover_args(A, server, {}), {"ZZZZZ-ZZZZZ-ZZZZZ-ZZZZX"});
        Check(!sixth.ok(), "a sixth try within the hour is refused");
        CheckEq(sixth.code(), "rate_limited", "...as rate_limited");
    }

    // ---- the master's limit of new nicks, and a wrong clock ----
    {
        master.SetPeer("203.0.113.7");
        std::vector<TestProfile> r5;
        for (int i = 0; i < 5; ++i) r5.push_back(MakeProfile(root / ("pcR" + std::to_string(i)), "Rate" + std::to_string(i), uint8_t(100 + i), true));
        for (int i = 0; i < 3; ++i) {
            Run c = Id(check_args(r5[size_t(i)], {"--claim"}));
            CheckEq(c.doc.Str("status.state"), "verified", "new nick " + std::to_string(i + 1) + " of the day registers");
            show_code(r5[size_t(i)]);
            Id(With({"recovery", "confirm"}, r5[size_t(i)].Closed()));
        }
        Run fourth = Id(check_args(r5[3], {"--claim"}));
        Check(fourth.ok(), "the fourth new nick of the day is an answer");
        CheckEq(fourth.doc.Str("status.state"), "failed", "...it fails");
        CheckEq(fourth.doc.Str("status.master_status"), "rate_limited", "...with the master's rate_limited");
        Check(fourth.doc.IsTrue("claim.asked_master") && fourth.doc.IsFalse("claim.ticket_stored"), "...nothing stored");
        // A rename that would claim is stopped the same way, and leaves the game's files alone.
        Run rr = Id(rename_args(r5[4], "Rate Five"));
        CheckEq(rr.code(), "rate_limited", "a rename that claims is stopped by the master's limit");
        Check(!fs::exists(r5[4].config) && !fs::exists(r5[4].saves / ".profile"), "...and writes neither the marker nor larecomp.toml");
        master.SetPeer("10.0.1.2");

        TestProfile k = MakeProfile(root / "pcK", "Clock1", 110);
        Run ck = Id(check_args(k, {"--claim", "--clock-offset-sec", "3600"}));
        CheckEq(ck.doc.Str("status.state"), "failed", "a clock an hour off fails the claim");
        CheckEq(ck.doc.Str("status.master_status"), "bad_clock", "...as bad_clock");
        Check(ck.doc.Str("status.detail").find("clock") != std::string::npos, "...and the sentence is about the clock");
        Check(core::ReadTicketFile(k.identity_dir, k.id).empty(), "nothing stored");
    }

    // ---- rename with a claim ----
    {
        master.SetPeer("10.0.1.3");
        TestProfile p = MakeProfile(root / "pcN", "Rowan", 120);
        const int before = master.posts();
        Run rn = Id(rename_args(p, "Rowan Two"));
        Check(rn.ok(), "rename --claim works");
        Check(rn.doc.IsTrue("claimed") && rn.doc.IsTrue("restart_needed"), "the new nick is claimed, a restart is needed");
        CheckEq(rn.doc.Str("from"), "Rowan", "from");
        CheckEq(rn.doc.Str("to"), "Rowan Two", "to");
        CheckInt(master.posts(), before + 1, "one request reached the master");
        Check(ReadAll(p.config).find("user_name = \"Rowan Two\"") != std::string::npos, "larecomp.toml has the new name");
        Run sh = Id(With({"show"}, p.Who()));
        Check(sh.doc.Get("tickets") && sh.doc.Get("tickets")->a.size() == 1 && sh.doc.Get("tickets")->a[0].Str("nick") == "Rowan Two", "the ticket is for the new nick");
        Check(sh.doc.IsTrue("recovery.pending"), "the new nick has a recovery code waiting");
        show_code(p);
        // A code waits: another rename with a claim would replace it.
        Run again = Id(rename_args(p, "Rowan Three"));
        CheckEq(again.code(), "recovery_code_unconfirmed", "a second renaming claim while a code waits is refused");
        CheckInt(master.posts(), before + 1, "...without asking the master");
        Check(ReadAll(p.config).find("Rowan Three") == std::string::npos, "...and the config is not touched");

        // Someone else wants that name: taken, nothing changes.
        TestProfile w = MakeProfile(root / "pcO", "Wren", 130);
        Run taken = Id(rename_args(w, "Rowan Two"));
        CheckEq(taken.code(), "nick_taken", "renaming to a taken nick is refused");
        Check(taken.doc.Str("error.message").find("Rowan Two") != std::string::npos, "...naming the owner's spelling");
        Check(!fs::exists(w.config) && !fs::exists(w.saves / ".profile"), "...and writes neither the marker nor larecomp.toml");
        // The master compares nicks normalised ("ROWAN  TWO" is the same nick).
        Run same = Id(rename_args(w, "ROWAN  TWO"));
        Check(!same.ok(), "the master's normalised comparison also refuses a look-alike");
    }
    // A game that starts while the master answers: what it handed out is reported, never dropped.
    {
        master.SetPeer("10.0.1.4");
        TestProfile p = MakeProfile(root / "pcG", "Late", 170);
        std::unique_ptr<FakeGame> late;
        master.OnPost([&] { late = std::make_unique<FakeGame>(p.game_exe, tmp / "runtime"); });
        Run c = Id(check_args(p, {"--claim"}));
        master.OnPost(nullptr);
        CheckEq(c.code(), "recovery_code_not_saved", "a game that starts during the claim: the code could not be stored");
        const std::string lost = c.doc.Str("error.recovery_code");
        Check(CodeShape(lost), "...and the code is in the error, so that it is not lost");
        g_secrets.push_back(lost);
        g_docs.back().may_hold_code = true;
        core::KeyRecord rec;
        Check(core::LoadKey(p.identity_dir, p.id, rec) == core::Load::kLoaded && rec.pending_code.empty(), "...the key file was left alone while the game runs");
        Check(core::ReadTicketFile(p.identity_dir, p.id).empty(), "...and so were the tickets");
        late.reset();
    }
    // A key filed by name (a profile with no id of its own) follows the rename.
    {
        TestProfile p = MakeProfile(root / "pcY", "Oldie", 180);
        fs::rename(core::KeyPath(p.identity_dir, p.id), core::KeyPath(p.identity_dir, "name_Oldie"));
        Run rn = Id({"rename", "--nick", "Oldie Two", "--config", p.config.string(), "--game-exe", p.game_exe.string(), "--identity-dir", p.identity_dir.string(),
                     "--saves", p.saves.string(), "--gamertag", "Oldie"});
        Check(rn.ok() && rn.doc.IsTrue("key_moved"), "rename moves a key that is filed by name");
        Check(fs::exists(core::KeyPath(p.identity_dir, "name_Oldie Two")) && !fs::exists(core::KeyPath(p.identity_dir, "name_Oldie")), "the key file follows the new name");
    }
    // A relay that does not check nicks has nothing to claim on.
    {
        idtest::FakeRelay lax;
        lax.Configure(master.public_key(), master.port(), false);
        lax.Start();
        TestProfile p = MakeProfile(root / "pcL", "Lax", 140);
        Run c = Id({"check", "--server", lax.Address(), "--claim", "--game-exe", p.game_exe.string(), "--identity-dir", p.identity_dir.string(),
                    "--saves", p.saves.string(), "--gamertag", "Lax"});
        CheckEq(c.doc.Str("status.state"), "not_checked", "a relay that checks no nicks");
        Check(c.doc.IsFalse("claim.asked_master"), "...so the master is not asked");
        Run rc = Id(recover_args(p, lax.Address(), {}), {"ABCDE-FGHJK-MNPQR-STVWX"});
        CheckEq(rc.code(), "relay_does_not_check_nicks", "recover has no master to ask there");
        Run rn = Id({"rename", "--nick", "Lax Two", "--config", p.config.string(), "--claim", "--server", lax.Address(), "--game-exe", p.game_exe.string(),
                     "--identity-dir", p.identity_dir.string(), "--saves", p.saves.string(), "--gamertag", "Lax"});
        CheckEq(rn.code(), "relay_does_not_check_nicks", "rename --claim has nothing to claim on there");
        Check(!fs::exists(p.config), "...and changes nothing");
    }
    // A relay that does not answer; a master that is down.
    {
        idtest::FakeRelay mute;
        mute.Configure(master.public_key(), master.port(), true);
        mute.SetSilent(true);
        mute.Start();
        TestProfile p = MakeProfile(root / "pcM", "Mute", 150);
        Run c = Id({"check", "--server", mute.Address(), "--timeout-ms", "100", "--identity-dir", p.identity_dir.string(), "--saves", p.saves.string(), "--gamertag", "Mute"});
        CheckEq(c.doc.Str("status.state"), "no_answer", "a relay that does not answer");
        Run rc = Id(recover_args(p, mute.Address(), {"--timeout-ms", "100"}), {"ABCDE-FGHJK-MNPQR-STVWX"});
        CheckEq(rc.code(), "relay_no_answer", "recover says the relay does not answer");

        idtest::FakeRelay dead;
        dead.Configure(master.public_key(), idtest::ClosedTcpPort(), true);
        dead.Start();
        TestProfile q = MakeProfile(root / "pcD", "Dead", 160);
        Run d = Id({"check", "--server", dead.Address(), "--claim", "--master-timeout-ms", "500", "--game-exe", q.game_exe.string(), "--identity-dir", q.identity_dir.string(),
                    "--saves", q.saves.string(), "--gamertag", "Dead"});
        CheckEq(d.doc.Str("status.state"), "master_down", "a master that is down");
        Check(d.doc.Str("status.detail").find("unreachable") != std::string::npos, "...is said to be unreachable");
        Check(d.doc.IsFalse("claim.ticket_stored") && d.doc.IsFalse("claim.recovery_code_pending"), "...and nothing is stored");
        Run rc2 = Id(recover_args(q, dead.Address(), {"--master-timeout-ms", "500"}), {"ABCDE-FGHJK-MNPQR-STVWX"});
        CheckEq(rc2.code(), "master_unreachable", "recover reports an unreachable master");
        Run cn = Id({"cancel", "--server", dead.Address(), "--master-timeout-ms", "500", "--game-exe", q.game_exe.string(), "--identity-dir", q.identity_dir.string(),
                     "--saves", q.saves.string(), "--gamertag", "Dead"});
        CheckEq(cn.code(), "master_unreachable", "cancel reports an unreachable master");
    }
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    idtest::StartSockets();
    idtest::Scratch scratch("main");
    const fs::path tmp = scratch.path();
    std::string self = argv[0];

    TestCore(tmp);
    TestGuard(tmp, self);
    TestUsage();
    TestShow(tmp);
    TestBackupRestore(tmp);
    TestRecoveryCode(tmp);
    TestRename(tmp);
    TestNetwork(tmp);

    // Nothing printed holds a secret.
    for (const Doc& d : g_docs) {
        std::string found;
        for (const std::string& secret : g_secrets) {
            if (secret.empty()) continue;
            if (d.may_hold_code && secret.size() == 23) continue;
            if (d.text.find(secret) != std::string::npos) found = secret.substr(0, 4) + "...";
        }
        Check(found.empty(), "a document holds a secret (" + found + "): " + d.text.substr(0, 160));
    }
    std::printf("%d documents were checked for the key, the token, passwords and recovery codes\n", int(g_docs.size()));
    std::printf("identity_commands_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
