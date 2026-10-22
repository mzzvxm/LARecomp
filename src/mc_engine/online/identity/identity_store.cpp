#include "identity_store.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>

#include "larecomp_log.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#else
#include <sys/stat.h>
#endif

namespace larecomp::identity {

namespace fs = std::filesystem;

namespace {

constexpr const char* kStoreMagic = "LRIDSTORE1";
constexpr const char kBackupMagic[8] = {'L', 'R', 'I', 'D', 'B', 'A', 'K', '1'};
constexpr uint64_t kLegacyXuid = 0xB13EBABEBABEBABEull;
// Argon2id for the backup password: 64 MiB, 3 passes.
constexpr uint32_t kArgonBlocks = 64 * 1024;
constexpr uint32_t kArgonPasses = 3;

struct State {
    bool loaded = false;
    bool restart_needed = false;
    std::string profile_id;
    std::string nick;
    uint8_t seed[32] = {};
    KeyPair keys{};
    uint64_t xuid = 0;
    bool has_rmr = false;
    std::array<uint8_t, 16> rmr{};
    std::string pending_code;
    int64_t code_remind = 0;  // RecoveryCodeRemindAfter
};

std::mutex g_mtx;
State g;

fs::path IdentityDir() {
#ifdef _WIN32
    if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA"); local && *local)
        return fs::path(local) / "larecomp" / "identity";
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
        return fs::path(xdg) / "larecomp" / "identity";
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home) / ".local" / "share" / "larecomp" / "identity";
#endif
    return fs::current_path() / "identity";
}

fs::path KeyPath() { return IdentityDir() / (g.profile_id + ".key"); }
fs::path TicketPath() { return IdentityDir() / (g.profile_id + ".tickets"); }

// DPAPI on Windows: only this Windows user on this PC can read it back.
bool Seal(const std::string& plain, std::string& out) {
#ifdef _WIN32
    DATA_BLOB in{DWORD(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB sealed{};
    if (!CryptProtectData(&in, L"larecomp identity", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &sealed))
        return false;
    out.assign(reinterpret_cast<const char*>(sealed.pbData), sealed.cbData);
    LocalFree(sealed.pbData);
    return true;
#else
    out = plain;
    return true;
#endif
}

bool Unseal(const std::string& sealed, std::string& out) {
#ifdef _WIN32
    DATA_BLOB in{DWORD(sealed.size()), reinterpret_cast<BYTE*>(const_cast<char*>(sealed.data()))};
    DATA_BLOB plain{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain))
        return false;
    out.assign(reinterpret_cast<const char*>(plain.pbData), plain.cbData);
    SecureZeroMemory(plain.pbData, plain.cbData);
    LocalFree(plain.pbData);
    return true;
#else
    out = sealed;
    return true;
#endif
}

bool ReadFile(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Through a temp file and a rename, so a crash never leaves half a key.
bool WriteFileAtomic(const fs::path& p, const std::string& data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(data.data(), std::streamsize(data.size()));
        if (!out) return false;
    }
#ifndef _WIN32
    ::chmod(tmp.c_str(), 0600);
#endif
    fs::rename(tmp, p, ec);
    return !ec;
}

std::string Plaintext(const State& s) {
    std::string t = std::string(kStoreMagic) + "\n";
    t += "seed=" + ToHex(s.seed, 32) + "\n";
    if (s.has_rmr) t += "rmr=" + ToHex(s.rmr.data(), s.rmr.size()) + "\n";
    if (!s.pending_code.empty()) t += "code=" + s.pending_code + "\n";
    if (!s.pending_code.empty() && s.code_remind) t += "code_remind=" + std::to_string(s.code_remind) + "\n";
    return t;
}

bool ParsePlaintext(const std::string& text, State& s) {
    if (text.rfind(kStoreMagic, 0) != 0) return false;
    std::map<std::string, std::string> f;
    ParseFields(text, f);
    if (!FromHex(f["seed"], s.seed, 32)) return false;
    s.has_rmr = FromHex(f["rmr"], s.rmr.data(), s.rmr.size());
    s.pending_code = f["code"];
    s.code_remind = std::strtoll(f["code_remind"].c_str(), nullptr, 10);
    return true;
}

void DeriveFromSeed(State& s) {
    s.keys = KeyPairFromSeed(s.seed);
    s.xuid = XuidFromPublicKey(s.keys.pub);
}

bool SaveLocked() {
    std::string sealed;
    std::string plain = Plaintext(g);
    const bool ok = Seal(plain, sealed) && WriteFileAtomic(KeyPath(), sealed);
    crypto_wipe(plain.data(), plain.size());
    if (!ok) LARECOMP_APP_ERROR("[identity] cannot save {}", KeyPath().string());
    return ok;
}

// The id of the profile folder: the SDK's per-profile XUID (profile.xuid), or
// the profile name on an SDK that has none (every profile there has the
// legacy XUID).
template <typename Profile>
std::string ProfileIdFor(const Profile* p) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(p->xuid()));
    if (p->xuid() != kLegacyXuid) return buf;
    return "name_" + p->name();
}

// Only the fork SDK lets the XUID be set; the stock one keeps its constant.
template <typename Profile>
bool SetGuestXuid(Profile* p, uint64_t xuid) {
    if constexpr (requires { p->set_xuid(xuid); }) {
        p->set_xuid(xuid);
        return true;
    } else {
        return false;
    }
}

}  // namespace

void ApplyProfileIdentity() {
    auto* ks = rex::system::kernel_state();
    auto* profile = ks ? ks->user_profile() : nullptr;
    if (!profile) {
        LARECOMP_APP_WARN("[identity] no profile; online identity off");
        return;
    }
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g.loaded) return;
    g.profile_id = ProfileIdFor(profile);
    g.nick = profile->name();

    // An SDK without per-profile XUIDs files the key by name; once the SDK
    // has them, that key is this profile's and moves to the new id.
    {
        std::error_code ec;
        const fs::path by_name = IdentityDir() / ("name_" + g.nick + ".key");
        if (g.profile_id.rfind("name_", 0) != 0 && !fs::exists(KeyPath(), ec) && fs::exists(by_name, ec)) {
            fs::rename(by_name, KeyPath(), ec);
            const fs::path tickets = IdentityDir() / ("name_" + g.nick + ".tickets");
            if (!ec && fs::exists(tickets, ec)) fs::rename(tickets, TicketPath(), ec);
            LARECOMP_APP_INFO("[identity] key of '{}' moved to profile id {}", g.nick, g.profile_id);
        }
    }

    bool have = false;
    std::string sealed, plain;
    if (ReadFile(KeyPath(), sealed)) {
        if (Unseal(sealed, plain) && ParsePlaintext(plain, g)) {
            have = true;
        } else {
            // Not ours to read (another Windows user, or damaged). Keep it
            // aside instead of overwriting: a backup may still restore it.
            std::error_code ec;
            fs::rename(KeyPath(), KeyPath().string() + ".unreadable", ec);
            LARECOMP_APP_ERROR("[identity] {} is unreadable here; moved aside, starting a new identity",
                               KeyPath().string());
        }
        crypto_wipe(plain.data(), plain.size());
    }
    if (!have) {
        RandomBytes(g.seed, 32);
        if (!SaveLocked())
            LARECOMP_APP_ERROR("[identity] the new identity will not survive this session");
        else
            LARECOMP_APP_INFO("[identity] new identity for '{}' in {}", g.nick, KeyPath().string());
    }
    DeriveFromSeed(g);
    g.loaded = true;
    const bool set = SetGuestXuid(profile, g.xuid);
    LARECOMP_APP_INFO("[identity] '{}' key {} XUID {:016X}{}", g.nick, Fingerprint(g.keys.pub), g.xuid,
                      set ? "" : " (this SDK keeps its own XUID)");
}

bool Loaded() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g.loaded;
}

std::string Nick() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g.nick;
}

namespace {
fs::path g_user_data_root;
}

void SetUserDataRoot(const fs::path& root) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_user_data_root = root;
}

fs::path UserDataRoot() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_user_data_root;
}

uint64_t Xuid() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g.xuid;
}

KeyPair Keys() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g.keys;
}

std::string KeyFingerprint() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g.loaded ? Fingerprint(g.keys.pub) : std::string();
}

std::optional<std::array<uint8_t, 16>> RmrToken() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g.loaded || !g.has_rmr) return std::nullopt;
    return g.rmr;
}

void SetRmrToken(const std::array<uint8_t, 16>& token) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g.loaded) return;
    g.rmr = token;
    g.has_rmr = true;
    SaveLocked();
}

std::string PendingRecoveryCode() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g.pending_code;
}

void SetPendingRecoveryCode(const std::string& code) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g.loaded) return;
    g.pending_code = code;
    g.code_remind = 0;
    SaveLocked();
}

void AcknowledgeRecoveryCode() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g.loaded || g.pending_code.empty()) return;
    g.pending_code.clear();
    g.code_remind = 0;
    SaveLocked();
}

int64_t RecoveryCodeRemindAfter() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g.code_remind;
}

void SetRecoveryCodeRemindAfter(int64_t when) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g.loaded || g.pending_code.empty() || g.code_remind == when) return;
    g.code_remind = when;
    SaveLocked();
}

bool CachedTicket(const uint8_t (&master_key)[32], Ticket& out) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g.loaded) return false;
    std::string text;
    if (!ReadFile(TicketPath(), text)) return false;
    std::map<std::string, std::string> f;
    ParseFields(text, f);
    const auto it = f.find(ToHex(master_key, 32));
    if (it == f.end() || !FromHex(it->second, reinterpret_cast<uint8_t*>(&out), sizeof(out))) return false;
    return std::memcmp(out.body.pubkey, g.keys.pub, 32) == 0;
}

void StoreTicket(const uint8_t (&master_key)[32], const Ticket& ticket) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g.loaded) return;
    std::string text;
    std::map<std::string, std::string> f;
    if (ReadFile(TicketPath(), text)) ParseFields(text, f);
    f[ToHex(master_key, 32)] = ToHex(reinterpret_cast<const uint8_t*>(&ticket), sizeof(ticket));
    text.clear();
    for (const auto& [k, v] : f) text += k + "=" + v + "\n";
    WriteFileAtomic(TicketPath(), text);  // tickets are public; no sealing needed
}

fs::path DefaultBackupPath() {
    std::string nick = Nick();
    for (char& c : nick)
        if (std::string("<>:\"/\\|?*").find(c) != std::string::npos) c = '_';
#ifdef _WIN32
    if (const wchar_t* home = _wgetenv(L"USERPROFILE"); home && *home)
        return fs::path(home) / "Documents" / ("LARecomp identity - " + nick + ".lrid");
#else
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home) / ("LARecomp identity - " + nick + ".lrid");
#endif
    return fs::current_path() / ("LARecomp identity - " + nick + ".lrid");
}

namespace {

bool BackupKey(const std::string& password, const uint8_t (&salt)[16], uint8_t (&key)[32]) {
    std::vector<uint8_t> work(size_t(kArgonBlocks) * 1024);
    crypto_argon2_config config{CRYPTO_ARGON2_ID, kArgonBlocks, kArgonPasses, 1};
    crypto_argon2_inputs inputs{reinterpret_cast<const uint8_t*>(password.data()), salt,
                                uint32_t(password.size()), 16};
    crypto_argon2(key, 32, work.data(), config, inputs, crypto_argon2_no_extras);
    crypto_wipe(work.data(), work.size());
    return true;
}

}  // namespace

bool ExportBackup(const fs::path& path, const std::string& password, std::string* error) {
    State copy;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (!g.loaded) {
            if (error) *error = "no identity loaded";
            return false;
        }
        copy = g;
    }
    copy.pending_code.clear();  // a code belongs on paper, not in a file next to the key
    if (password.size() < 8) {
        if (error) *error = "use a password of at least 8 characters";
        return false;
    }
    uint8_t salt[16], nonce[24], mac[16], key[32];
    RandomBytes(salt, 16);
    RandomBytes(nonce, 24);
    BackupKey(password, salt, key);
    std::string plain = Plaintext(copy);
    std::string cipher(plain.size(), '\0');
    crypto_aead_lock(reinterpret_cast<uint8_t*>(cipher.data()), mac, key, nonce,
                     reinterpret_cast<const uint8_t*>(kBackupMagic), 8,
                     reinterpret_cast<const uint8_t*>(plain.data()), plain.size());
    crypto_wipe(plain.data(), plain.size());
    crypto_wipe(key, 32);
    crypto_wipe(copy.seed, 32);
    std::string file(kBackupMagic, 8);
    file.append(reinterpret_cast<const char*>(salt), 16);
    file.append(reinterpret_cast<const char*>(nonce), 24);
    file.append(reinterpret_cast<const char*>(mac), 16);
    file += cipher;
    if (!WriteFileAtomic(path, file)) {
        if (error) *error = "cannot write " + path.string();
        return false;
    }
    LARECOMP_APP_INFO("[identity] backup written to {}", path.string());
    return true;
}

bool ImportBackup(const fs::path& path, const std::string& password, std::string* error) {
    std::string file;
    if (!ReadFile(path, file)) {
        if (error) *error = "cannot read " + path.string();
        return false;
    }
    if (file.size() < 8 + 16 + 24 + 16 + 1 || std::memcmp(file.data(), kBackupMagic, 8) != 0) {
        if (error) *error = "not a LARecomp identity backup";
        return false;
    }
    uint8_t salt[16], nonce[24], mac[16], key[32];
    std::memcpy(salt, file.data() + 8, 16);
    std::memcpy(nonce, file.data() + 24, 24);
    std::memcpy(mac, file.data() + 48, 16);
    const std::string cipher = file.substr(64);
    BackupKey(password, salt, key);
    std::string plain(cipher.size(), '\0');
    const bool ok = crypto_aead_unlock(reinterpret_cast<uint8_t*>(plain.data()), mac, key, nonce,
                                       reinterpret_cast<const uint8_t*>(kBackupMagic), 8,
                                       reinterpret_cast<const uint8_t*>(cipher.data()), cipher.size()) == 0;
    crypto_wipe(key, 32);
    State restored;
    if (!ok || !ParsePlaintext(plain, restored)) {
        crypto_wipe(plain.data(), plain.size());
        if (error) *error = "wrong password, or the file is damaged";
        return false;
    }
    crypto_wipe(plain.data(), plain.size());
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g.loaded) {
        if (error) *error = "no profile loaded";
        return false;
    }
    // Keep what this session runs with (the guest has the old XUID) and write
    // the restored identity for the next launch.
    State next = g;
    std::memcpy(next.seed, restored.seed, 32);
    next.has_rmr = restored.has_rmr;
    next.rmr = restored.rmr;
    next.pending_code.clear();
    std::string sealed;
    std::string text = Plaintext(next);
    const bool saved = Seal(text, sealed) && WriteFileAtomic(KeyPath(), sealed);
    crypto_wipe(text.data(), text.size());
    crypto_wipe(next.seed, 32);
    if (!saved) {
        if (error) *error = "cannot save " + KeyPath().string();
        return false;
    }
    std::error_code ec;
    fs::remove(TicketPath(), ec);  // tickets name the old key
    g.restart_needed = true;
    LARECOMP_APP_INFO("[identity] backup {} imported; restart the game to use it", path.string());
    return true;
}

bool RestartNeeded() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g.restart_needed;
}

}  // namespace larecomp::identity
