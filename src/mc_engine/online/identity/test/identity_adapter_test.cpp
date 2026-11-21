// Check that the game's identity code (identity_store.cpp, identity_client.cpp) behaves the same on top of the shared core
// (identity_core.cpp, identity_net.cpp) as it did before the split. The game's two files are compiled here WITHOUT the
// SDK, against small stand-ins for the SDK's profile and runtime, the settings and the log (test/stubs), and exercised
// through their public API on scratch folders, a fake relay and the real nick registry on 127.0.0.1.
//
// The same program is built against the code as it was before the split (a copy of the tree at 4364d9ca) and after it;
// both must pass every check, and both print what the code logged, so the two outputs can be compared line by line
// (identity_adapter_compare.ps1 does that). Nothing here knows how the code is organised inside: the key files are
// written and read by the check's own independent code (the file format, DPAPI, Argon2id for the backup).
//
//   identity_adapter_test <scenario>        scenario: fresh | existing | unreadable | legacy | ops | client
//
// It needs LOCALAPPDATA (Windows) or XDG_DATA_HOME to point at a scratch folder: the scenarios set it themselves.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "identity_test_support.h"
#include "../identity_client.h"
#include "../identity_store.h"
#include "../../protocol/identity_protocol.h"
#include "../../rate_my_ride/rmr_net.h"

#include <larecomp_log.h>
#include <rex/kernel/title_runtime.h>
#include <rex/xam/user_profile.h>
#include <settings/larecomp_settings.h>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#else
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
namespace idn = larecomp::identity;

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

// ---- the check's own reading and writing of the files ------------------------------------------------------------

bool SealForCheck(const std::string& plain, std::string& out) {
#ifdef _WIN32
    DATA_BLOB in{DWORD(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB sealed{};
    if (!CryptProtectData(&in, L"larecomp identity", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &sealed)) return false;
    out.assign(reinterpret_cast<const char*>(sealed.pbData), sealed.cbData);
    LocalFree(sealed.pbData);
    return true;
#else
    out = plain;
    return true;
#endif
}

bool UnsealForCheck(const std::string& sealed, std::string& out) {
#ifdef _WIN32
    DATA_BLOB in{DWORD(sealed.size()), reinterpret_cast<BYTE*>(const_cast<char*>(sealed.data()))};
    DATA_BLOB plain{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain)) return false;
    out.assign(reinterpret_cast<const char*>(plain.pbData), plain.cbData);
    LocalFree(plain.pbData);
    return true;
#else
    out = sealed;
    return true;
#endif
}

std::string Slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void Spit(const fs::path& p, const std::string& data) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary | std::ios::trunc) << data;
}

// What is in a key file, as plain "key=value" lines (empty when it cannot be read).
std::map<std::string, std::string> ReadKeyFile(const fs::path& p) {
    std::map<std::string, std::string> f;
    std::string sealed, plain;
    sealed = Slurp(p);
    if (sealed.empty() || !UnsealForCheck(sealed, plain)) return f;
    idn::ParseFields(plain, f);
    f["magic_ok"] = plain.rfind("LRIDSTORE1\n", 0) == 0 ? "1" : "0";
    return f;
}

void WriteKeyFile(const fs::path& p, const std::vector<std::pair<std::string, std::string>>& lines) {
    std::string plain = "LRIDSTORE1\n";
    for (const auto& [k, v] : lines) plain += k + "=" + v + "\n";
    std::string sealed;
    SealForCheck(plain, sealed);
    Spit(p, sealed);
}

std::string SeedHex(int base) {
    uint8_t s[32];
    for (int i = 0; i < 32; ++i) s[i] = uint8_t(base + i);
    return idn::ToHex(s, 32);
}

// The backup format, independently: "LRIDBAK1" salt(16) nonce(24) mac(16) body, Argon2id 64 MiB x3, XChaCha20-Poly1305.
bool OpenBackupForCheck(const std::string& file, const std::string& password, std::string& plain) {
    if (file.size() < 65 || file.compare(0, 8, "LRIDBAK1") != 0) return false;
    uint8_t salt[16], nonce[24], mac[16], key[32];
    std::memcpy(salt, file.data() + 8, 16);
    std::memcpy(nonce, file.data() + 24, 24);
    std::memcpy(mac, file.data() + 48, 16);
    std::vector<uint8_t> work(size_t(64 * 1024) * 1024);
    crypto_argon2_config config{CRYPTO_ARGON2_ID, 64 * 1024, 3, 1};
    crypto_argon2_inputs inputs{reinterpret_cast<const uint8_t*>(password.data()), salt, uint32_t(password.size()), 16};
    crypto_argon2(key, 32, work.data(), config, inputs, crypto_argon2_no_extras);
    const std::string cipher = file.substr(64);
    plain.assign(cipher.size(), '\0');
    const int rc = crypto_aead_unlock(reinterpret_cast<uint8_t*>(plain.data()), mac, key, nonce,
                                      reinterpret_cast<const uint8_t*>("LRIDBAK1"), 8,
                                      reinterpret_cast<const uint8_t*>(cipher.data()), cipher.size());
    crypto_wipe(key, 32);
    return rc == 0;
}

std::string FromHexText(const std::string& hex) {
    std::string out(hex.size() / 2, '\0');
    idn::FromHex(hex, reinterpret_cast<uint8_t*>(out.data()), out.size());
    return out;
}

// The golden backup (seed 00..1f, token a0..af, "correct horse battery staple"): see identity_commands_test.cpp.
const char* kGoldenBackup =
    "4c52494442414b31101112131415161718191a1b1c1d1e1f303132333435363738393a3b3c3d3e3f4041424344454647ca0a8c9d03060441"
    "65d0991af468f5646642cba168d01f24ea64b85c248f7b86d81ef999a90cfb566290143feeb5ea232ad8134957b8f105393426ec7c326ae4"
    "5c20e8f3a10643ab802c43ac6cb324e81fb57bcec5d90a2382398b751e023a77f9ffbf6398a4e1e612109dee517d18680671eae0a84a98c7"
    "27e4a7c3f415ad7f9f031c167be9";

// ---- the environment ----------------------------------------------------------------------------------------------

fs::path g_appdata;  // what LOCALAPPDATA / XDG_DATA_HOME points at
fs::path IdentityDir() { return g_appdata / "larecomp" / "identity"; }

void SetEnvironment(const fs::path& appdata) {
    g_appdata = appdata;
    fs::create_directories(appdata);
#ifdef _WIN32
    _putenv_s("LOCALAPPDATA", appdata.string().c_str());
#else
    setenv("XDG_DATA_HOME", appdata.string().c_str(), 1);
#endif
}

rex::xam::UserProfile* g_profile = nullptr;

void SetProfile(const std::string& name, uint64_t xuid) {
    g_profile = new rex::xam::UserProfile(name, xuid);
    rex::kernel::g_runtime.profile = g_profile;
}

std::string Hex16(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(v));
    return buf;
}

bool WaitFor(const std::function<bool()>& pred, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return pred();
}

// Whether the code has logged a line with this text (the worker's replies to recover and cancel are short-lived in the
// status: the next pass replaces them, so what it logged is what shows they happened).
bool LogHas(const std::string& text) {
    std::lock_guard<std::mutex> lk(idstub::LogMutex());
    for (const std::string& line : idstub::LogLines())
        if (line.find(text) != std::string::npos) return true;
    return false;
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

// ---- scenarios --------------------------------------------------------------------------------------------------

constexpr uint64_t kXuid = 0xB13E0000000042ABull;

void Fresh(const fs::path&) {
    SetProfile("Tester", kXuid);
    Check(!idn::Loaded(), "not loaded before");
    idn::ApplyProfileIdentity();
    Check(idn::Loaded(), "loaded after");
    CheckEq(idn::Nick(), "Tester", "nick");
    const fs::path key = IdentityDir() / (Hex16(kXuid) + ".key");
    Check(fs::exists(key), "a key file named by the profile's XUID");
    const auto f = ReadKeyFile(key);
    Check(f.count("magic_ok") && f.at("magic_ok") == "1" && f.count("seed") && f.at("seed").size() == 64, "the key file holds LRIDSTORE1 and a 64 digit seed");
    uint8_t seed[32];
    Check(idn::FromHex(f.count("seed") ? f.at("seed") : "", seed, 32), "the seed is hex");
    const idn::KeyPair kp = idn::KeyPairFromSeed(seed);
    CheckEq(idn::KeyFingerprint(), idn::Fingerprint(kp.pub), "the fingerprint is the key's");
    CheckEq(Hex16(idn::Xuid()), Hex16(idn::XuidFromPublicKey(kp.pub)), "the XUID is the key's");
    CheckEq(Hex16(g_profile->GetXUID()), Hex16(idn::Xuid()), "the guest's XUID was set from the identity");
    Check(!idn::RmrToken().has_value(), "no Rate My Ride token yet");
    CheckEq(idn::PendingRecoveryCode(), "", "no recovery code yet");
    Check(!idn::RestartNeeded(), "no restart needed");
    Check(std::memcmp(idn::Keys().pub, kp.pub, 32) == 0, "Keys() are the seed's");
    idn::ApplyProfileIdentity();  // once
    Check(idn::Loaded(), "a second call changes nothing");
    Check(!fs::exists(IdentityDir() / (Hex16(kXuid) + ".key.tmp")), "no temp file is left");
}

void Existing(const fs::path&) {
    SetProfile("Tester", kXuid);
    WriteKeyFile(IdentityDir() / (Hex16(kXuid) + ".key"),
                 {{"seed", SeedHex(1)}, {"rmr", "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"}, {"code", "ABCDE-FGHJK-MNPQR-STVWX"}, {"code_remind", "777"}});
    idn::ApplyProfileIdentity();
    uint8_t seed[32];
    idn::FromHex(SeedHex(1), seed, 32);
    const idn::KeyPair kp = idn::KeyPairFromSeed(seed);
    CheckEq(idn::KeyFingerprint(), idn::Fingerprint(kp.pub), "an existing key is the one used");
    CheckEq(Hex16(idn::Xuid()), Hex16(idn::XuidFromPublicKey(kp.pub)), "its XUID");
    Check(idn::RmrToken().has_value() && (*idn::RmrToken())[0] == 0xA0 && (*idn::RmrToken())[15] == 0xAF, "its Rate My Ride token");
    CheckEq(idn::PendingRecoveryCode(), "ABCDE-FGHJK-MNPQR-STVWX", "its waiting code");
    CheckEq(std::to_string(idn::RecoveryCodeRemindAfter()), "777", "its reminder");
    Check(fs::exists(IdentityDir() / (Hex16(kXuid) + ".key")) && !fs::exists(IdentityDir() / (Hex16(kXuid) + ".key.unreadable")), "the file is left as it was");
}

void Unreadable(const fs::path&) {
    SetProfile("Tester", kXuid);
    Spit(IdentityDir() / (Hex16(kXuid) + ".key"), "this is not a key file at all");
    idn::ApplyProfileIdentity();
    Check(idn::Loaded(), "a new identity is made");
    Check(fs::exists(IdentityDir() / (Hex16(kXuid) + ".key.unreadable")), "the file that cannot be read is kept aside");
    CheckEq(Slurp(IdentityDir() / (Hex16(kXuid) + ".key.unreadable")), "this is not a key file at all", "...untouched");
    const auto f = ReadKeyFile(IdentityDir() / (Hex16(kXuid) + ".key"));
    Check(f.count("magic_ok") && f.at("magic_ok") == "1", "and a readable key stands where it was");
}

void Legacy(const fs::path&) {
    // The SDK has a per-profile XUID now; the key an older SDK filed by the gamertag becomes this profile's.
    SetProfile("Tester", kXuid);
    WriteKeyFile(IdentityDir() / "name_Tester.key", {{"seed", SeedHex(7)}});
    Spit(IdentityDir() / "name_Tester.tickets", "00=11\n");
    idn::ApplyProfileIdentity();
    uint8_t seed[32];
    idn::FromHex(SeedHex(7), seed, 32);
    CheckEq(idn::KeyFingerprint(), idn::Fingerprint(idn::KeyPairFromSeed(seed).pub), "the key filed by name is the profile's key");
    Check(!fs::exists(IdentityDir() / "name_Tester.key") && fs::exists(IdentityDir() / (Hex16(kXuid) + ".key")), "it moved to the profile's id");
    Check(!fs::exists(IdentityDir() / "name_Tester.tickets") && fs::exists(IdentityDir() / (Hex16(kXuid) + ".tickets")), "and its tickets with it");
}

void LegacyXuid(const fs::path&) {
    // A profile that still has the shared XUID is filed by name.
    SetProfile("Tester", 0xB13EBABEBABEBABEull);
    idn::ApplyProfileIdentity();
    Check(fs::exists(IdentityDir() / "name_Tester.key"), "a profile with the legacy XUID gets name_<gamertag>.key");
}

void Ops(const fs::path& tmp) {
    SetProfile("Tester", kXuid);
    idn::ApplyProfileIdentity();
    const fs::path key = IdentityDir() / (Hex16(kXuid) + ".key");
    const std::string seed_hex = ReadKeyFile(key)["seed"];

    // Rate My Ride token.
    std::array<uint8_t, 16> tok;
    for (size_t i = 0; i < 16; ++i) tok[i] = uint8_t(0x40 + i);
    idn::SetRmrToken(tok);
    Check(idn::RmrToken().has_value() && *idn::RmrToken() == tok, "the token is kept in memory");
    auto f = ReadKeyFile(key);
    CheckEq(f["rmr"], "404142434445464748494a4b4c4d4e4f", "and in the file");
    CheckEq(f["seed"], seed_hex, "the seed in the file is unchanged");

    // Recovery code, reminder, acknowledge.
    idn::SetPendingRecoveryCode("ABCDE-FGHJK-MNPQR-STVWX");
    CheckEq(idn::PendingRecoveryCode(), "ABCDE-FGHJK-MNPQR-STVWX", "the code is kept");
    f = ReadKeyFile(key);
    CheckEq(f["code"], "ABCDE-FGHJK-MNPQR-STVWX", "in the file");
    Check(!f.count("code_remind"), "no reminder line while it is 0");
    idn::SetRecoveryCodeRemindAfter(idn::kNeverRemind);
    CheckEq(std::to_string(idn::RecoveryCodeRemindAfter()), "-1", "never remind");
    f = ReadKeyFile(key);
    CheckEq(f["code_remind"], "-1", "in the file");
    CheckEq(f["rmr"], "404142434445464748494a4b4c4d4e4f", "the token survives");
    idn::SetPendingRecoveryCode("BCDEF-GHJKM-NPQRS-TVWXY");
    CheckEq(std::to_string(idn::RecoveryCodeRemindAfter()), "0", "a new code resets the reminder");
    idn::AcknowledgeRecoveryCode();
    CheckEq(idn::PendingRecoveryCode(), "", "acknowledged");
    f = ReadKeyFile(key);
    Check(!f.count("code") && !f.count("code_remind"), "the file has no code any more");
    idn::SetRecoveryCodeRemindAfter(5);
    CheckEq(std::to_string(idn::RecoveryCodeRemindAfter()), "0", "no reminder is set when no code waits");
    CheckEq(f["seed"], seed_hex, "the seed is unchanged");

    // Tickets.
    uint8_t master[32];
    for (int i = 0; i < 32; ++i) master[i] = uint8_t(200 + i);
    idn::Ticket t{};
    t.body.version = 1;
    t.body.nick_len = 6;
    std::memcpy(t.body.nick, "Tester", 6);
    std::memcpy(t.body.pubkey, idn::Keys().pub, 32);
    t.body.expires = 4102444800ull;
    idn::Ticket got{};
    Check(!idn::CachedTicket(master, got), "no ticket yet");
    idn::StoreTicket(master, t);
    Check(idn::CachedTicket(master, got) && std::memcmp(&got, &t, sizeof(t)) == 0, "a stored ticket comes back");
    const std::string tickets = Slurp(IdentityDir() / (Hex16(kXuid) + ".tickets"));
    Check(tickets.rfind(idn::ToHex(master, 32) + "=", 0) == 0 && tickets.back() == '\n', "the ticket file is '<master key hex>=<ticket hex>' lines");
    idn::Ticket other = t;
    other.body.pubkey[0] ^= 1;
    uint8_t master2[32] = {1};
    idn::StoreTicket(master2, other);
    Check(!idn::CachedTicket(master2, got), "a ticket for another key is not this key's");

    // Backup: export, read with the check's own code; import the golden file made before the split.
    const fs::path out = tmp / "ops-out";
    fs::create_directories(out);
    std::string err;
    Check(!idn::ExportBackup(out / "short.lrid", "short", &err) && err == "use a password of at least 8 characters", "a short password: the game's sentence");
    Check(idn::ExportBackup(out / "a.lrid", "correct horse battery staple", &err), "export works");
    std::string plain;
    Check(OpenBackupForCheck(Slurp(out / "a.lrid"), "correct horse battery staple", plain), "the exported file opens with the format as specified");
    Check(plain.find("seed=" + seed_hex) != std::string::npos && plain.find("rmr=404142434445464748494a4b4c4d4e4f") != std::string::npos && plain.find("code") == std::string::npos,
          "it holds the seed and the token and no code");
    CheckEq(plain, "LRIDSTORE1\nseed=" + seed_hex + "\nrmr=404142434445464748494a4b4c4d4e4f\n", "the exact text inside");
    Check(!idn::ImportBackup(out / "a.lrid", "wrong password!", &err) && err == "wrong password, or the file is damaged", "wrong password: the game's sentence");
    Check(!idn::ImportBackup(out / "none.lrid", "correct horse battery staple", &err) && err.rfind("cannot read ", 0) == 0, "a file that is not there");
    Spit(out / "junk.lrid", std::string(100, 'x'));
    Check(!idn::ImportBackup(out / "junk.lrid", "correct horse battery staple", &err) && err == "not a LARecomp identity backup", "not a backup");
    Check(!idn::RestartNeeded(), "failed imports need no restart");

    Spit(out / "golden.lrid", FromHexText(kGoldenBackup));
    Check(idn::ImportBackup(out / "golden.lrid", "correct horse battery staple", &err), "import of the file made before the split");
    Check(idn::RestartNeeded(), "the game must restart to use it");
    f = ReadKeyFile(key);
    CheckEq(f["seed"], "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", "the key file now holds the backup's seed");
    CheckEq(f["rmr"], "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf", "and token");
    Check(!f.count("code"), "no code");
    Check(!fs::exists(IdentityDir() / (Hex16(kXuid) + ".tickets")), "the tickets of the old key are gone");
    // This session keeps the old XUID (the guest has it).
    CheckEq(idn::KeyFingerprint(), idn::Fingerprint(idn::Keys().pub), "the running identity is unchanged until the restart");
    CheckEq(Hex16(idn::Xuid()), Hex16(g_profile->GetXUID()), "...and so is the guest's XUID");
    Check(std::string(idn::DefaultBackupPath().filename().string()) == "LARecomp identity - Tester.lrid", "the default backup name");
}

// A request to the master by hand (the check's rival player), independent of the code under test.
std::string RivalCall(uint16_t port, const std::string& op, const std::string& nick, const idn::KeyPair& kp, const std::string& code) {
    const std::string pub = idn::ToHex(kp.pub, 32);
    const int64_t now = idn::NowUnix();
    const std::string salt = idn::NewSalt();
    uint8_t sig[64];
    idn::Sign(sig, kp, idn::RequestMessage(op, nick, pub, now, salt, op == "recover" ? idn::CanonicalCode(code) : ""));
    std::string body = "op=" + op + "\nnick=" + nick + "\npubkey=" + pub + "\ntime=" + std::to_string(now) + "\nsalt=" + salt + "\nsig=" + idn::ToHex(sig, 64) + "\n";
    const auto r = rmr::net::Post("127.0.0.1", port, idn::kMasterPath, std::vector<uint8_t>(body.begin(), body.end()), 2000, 4000);
    return std::string(r.body.begin(), r.body.end());
}

void Client(const fs::path& tmp) {
    idtest::StartSockets();
    idtest::FakeMaster master;
    Check(master.Start(tmp / "master", 2), "the master starts (2 second hold)");
    idtest::FakeRelay relay;
    relay.Configure(master.public_key(), master.port(), true);
    relay.Start();
    idtest::FakeRelay mute;
    mute.Configure(master.public_key(), master.port(), true);
    mute.SetSilent(true);
    mute.Start();

    // A rival owns the nick first.
    uint8_t rival_seed[32];
    for (int i = 0; i < 32; ++i) rival_seed[i] = uint8_t(90 + i);
    const idn::KeyPair rival = idn::KeyPairFromSeed(rival_seed);
    std::map<std::string, std::string> reply;
    idn::ParseFields(RivalCall(master.port(), "claim", "Tester", rival, ""), reply);
    CheckEq(reply["status"], "ok", "the rival registers the nick first");
    const std::string rival_code = reply["recovery"];
    Check(CodeShape(rival_code), "and gets a recovery code");

    SetProfile("Tester", kXuid);
    idn::ApplyProfileIdentity();
    larecomp::cvar::SetFlagByName("mcla_online_enable", "false");
    larecomp::cvar::SetFlagByName("mcla_online_server", relay.Address());
    idn::StartClient();
    idn::StartClient();  // once
    Check(idn::GetStatus().check == idn::Check::kOff, "offline: the status is off");

    larecomp::cvar::SetFlagByName("mcla_online_enable", "true");
    idn::CheckNow();
    Check(WaitFor([] { return idn::GetStatus().check == idn::Check::kTaken; }, 15000), "the rival's nick is taken");
    {
        const idn::Status s = idn::GetStatus();
        CheckEq(s.owner_nick, "Tester", "the owner's spelling");
        CheckEq(s.relay, relay.CanonicalName(), "the relay as signed");
        CheckEq(s.master, master.Address(), "the master the relay named");
        Check(s.detail.find("belongs to someone else") != std::string::npos, "the detail says so");
    }

    // Recover with a wrong code, then the right one.
    idn::RequestRecover("ZZZZZ-ZZZZZ-ZZZZZ-ZZZZZ");
    Check(WaitFor([] { return LogHas("recover \"Tester\": bad_code"); }, 15000), "a wrong recovery code is answered bad_code");
    idn::RequestRecover(rival_code);
    Check(WaitFor([] { return idn::GetStatus().check == idn::Check::kClaimPending; }, 15000), "the right code: the nick is coming");
    {
        const idn::Status s = idn::GetStatus();
        Check(s.claim_until > idn::NowUnix() - 1, "until a time that is shown");
        Check(LogHas("recover \"Tester\": pending"), "the master's answer was pending");
    }
    // After the hold the claim goes through and the code is kept.
    std::this_thread::sleep_for(std::chrono::milliseconds(2600));
    idn::CheckNow();
    Check(WaitFor([] { return idn::GetStatus().check == idn::Check::kVerified; }, 15000), "after the hold the nick is verified");
    Check(relay.Admitted("Tester"), "the relay admitted it");
    Check(CodeShape(idn::PendingRecoveryCode()), "the master's new recovery code is kept");
    {
        std::map<std::string, std::string> kf = ReadKeyFile(IdentityDir() / (Hex16(kXuid) + ".key"));
        CheckEq(kf["code"], idn::PendingRecoveryCode(), "...in the key file");
        const std::string tickets = Slurp(IdentityDir() / (Hex16(kXuid) + ".tickets"));
        Check(tickets.rfind(idn::ToHex(master.public_key(), 32) + "=", 0) == 0, "and the ticket in the ticket file");
    }
    CheckEq(idn::GetStatus().detail, "nick verified on this server", "the detail");
    idn::Ticket cached{};
    Check(idn::CachedTicket(master.public_key(), cached) && idn::TicketNick(cached) == "Tester", "the ticket is cached for the next pass");

    // Cancel with nothing to cancel.
    idn::RequestCancelTransfer();
    Check(WaitFor([] { return LogHas("cancel transfer of \"Tester\": nothing_pending"); }, 15000), "cancel with nothing pending is answered nothing_pending");

    // A relay that does not answer; then online off.
    larecomp::cvar::SetFlagByName("mcla_online_server", mute.Address());
    idn::CheckNow();
    Check(WaitFor([] { return idn::GetStatus().check == idn::Check::kNoAnswer; }, 20000), "a relay that does not answer");
    CheckEq(idn::GetStatus().detail, "the server does not answer nick checks (older relay, or offline)", "the detail");
    larecomp::cvar::SetFlagByName("mcla_online_enable", "false");
    Check(WaitFor([] { return idn::GetStatus().check == idn::Check::kOff; }, 6000), "online off: the status is off again");
    Check(idn::GetStatus().generation > 5, "the status generation counted the changes");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: identity_adapter_test fresh|existing|unreadable|legacy|legacyxuid|ops|client\n");
        return 2;
    }
    const std::string scenario = argv[1];
    idtest::Scratch scratch("adapter_" + scenario);
    SetEnvironment(scratch.path() / "appdata");
    if (scenario == "fresh") Fresh(scratch.path());
    else if (scenario == "existing") Existing(scratch.path());
    else if (scenario == "unreadable") Unreadable(scratch.path());
    else if (scenario == "legacy") Legacy(scratch.path());
    else if (scenario == "legacyxuid") LegacyXuid(scratch.path());
    else if (scenario == "ops") Ops(scratch.path());
    else if (scenario == "client") Client(scratch.path());
    else {
        std::printf("unknown scenario\n");
        return 2;
    }
    // What the code logged, for comparing before and after the split (the scratch folder's path is replaced).
    const std::string root = scratch.path().string();
    for (std::string line : idstub::LogLines()) {
        for (size_t at; (at = line.find(root)) != std::string::npos;) line.replace(at, root.size(), "<TMP>");
        std::printf("LOG %s\n", line.c_str());
    }
    std::printf("identity_adapter_test[%s]: %d checks, %d failed\n", scenario.c_str(), g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
