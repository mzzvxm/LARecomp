#include "identity_core.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

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

namespace larecomp::identity::core {

namespace {

constexpr const char* kStoreMagic = "LRIDSTORE1";
constexpr const char kBackupMagic[8] = {'L', 'R', 'I', 'D', 'B', 'A', 'K', '1'};
// Argon2id for the backup password: 64 MiB, 3 passes.
constexpr uint32_t kArgonBlocks = 64 * 1024;
constexpr uint32_t kArgonPasses = 3;

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

// ---- the key file ----------------------------------------------------------------------------------------------

void KeyRecord::Wipe() {
    crypto_wipe(seed, sizeof(seed));
    crypto_wipe(rmr.data(), rmr.size());
    if (!pending_code.empty()) crypto_wipe(pending_code.data(), pending_code.size());
    pending_code.clear();
    has_rmr = false;
    code_remind = 0;
}

KeyPair KeyRecord::Keys() const { return KeyPairFromSeed(seed); }

uint64_t KeyRecord::Xuid() const {
    const KeyPair kp = Keys();
    return XuidFromPublicKey(kp.pub);
}

std::string KeyRecord::Fingerprint() const {
    const KeyPair kp = Keys();
    return identity::Fingerprint(kp.pub);
}

bool KeyRecord::SameKeyAs(const KeyRecord& other) const { return std::memcmp(seed, other.seed, 32) == 0; }

std::string Serialize(const KeyRecord& r) {
    std::string t = std::string(kStoreMagic) + "\n";
    t += "seed=" + ToHex(r.seed, 32) + "\n";
    if (r.has_rmr) t += "rmr=" + ToHex(r.rmr.data(), r.rmr.size()) + "\n";
    if (!r.pending_code.empty()) t += "code=" + r.pending_code + "\n";
    if (!r.pending_code.empty() && r.code_remind) t += "code_remind=" + std::to_string(r.code_remind) + "\n";
    return t;
}

bool Parse(const std::string& text, KeyRecord& out) {
    if (text.rfind(kStoreMagic, 0) != 0) return false;
    std::map<std::string, std::string> f;
    ParseFields(text, f);
    if (!FromHex(f["seed"], out.seed, 32)) return false;
    out.has_rmr = FromHex(f["rmr"], out.rmr.data(), out.rmr.size());
    out.pending_code = f["code"];
    out.code_remind = std::strtoll(f["code_remind"].c_str(), nullptr, 10);
    return true;
}

// DPAPI on Windows: only this Windows user on this PC can read it back.
bool Seal(const std::string& plain, std::string& out) {
#ifdef _WIN32
    DATA_BLOB in{DWORD(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB sealed{};
    if (!CryptProtectData(&in, L"larecomp identity", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &sealed))
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

const char* Protection() {
#ifdef _WIN32
    return "windows_dpapi";
#else
    return "plain_file";
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

fs::path DefaultIdentityDir() {
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

fs::path KeyPath(const fs::path& dir, const std::string& id) { return dir / (id + ".key"); }
fs::path TicketPath(const fs::path& dir, const std::string& id) { return dir / (id + ".tickets"); }

std::string ProfileId(uint64_t xuid, const std::string& name) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(xuid));
    if (xuid != kLegacyXuid) return buf;
    return "name_" + name;
}

bool ValidProfileId(const std::string& id) {
    if (id.size() == 16) {
        for (char c : id)
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) return false;
        return true;
    }
    if (id.rfind("name_", 0) != 0 || id.size() < 6 || id.size() > 5 + kNickMax) return false;
    for (size_t i = 5; i < id.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(id[i]);
        if (c < 0x20 || c == 0x7F || std::strchr("<>:\"/\\|?*", c) != nullptr) return false;
    }
    return id.find("..") == std::string::npos;
}

bool ReadProfileXuid(const fs::path& saves_root, const std::string& gamertag, uint64_t& xuid) {
    std::string text;
    const fs::path folder(std::u8string(reinterpret_cast<const char8_t*>(gamertag.data()), gamertag.size()));
    if (!ReadFile(saves_root / folder / "profile.xuid", text)) return false;
    if (text.size() > 31) text.resize(31);  // the SDK reads no more than that
    char* end = nullptr;
    const uint64_t v = std::strtoull(text.c_str(), &end, 16);
    // UserProfile::IsValidXuid: not 0, not all ones, and bits 54-55 clear.
    if (end == text.c_str() || v == 0 || v == ~0ull || (v & 0x00C0000000000000ull) != 0) return false;
    xuid = v;
    return true;
}

Load LoadKey(const fs::path& dir, const std::string& id, KeyRecord& out) {
    std::string sealed, plain;
    if (!ReadFile(KeyPath(dir, id), sealed)) return Load::kMissing;
    const bool ok = Unseal(sealed, plain) && Parse(plain, out);
    crypto_wipe(plain.data(), plain.size());
    return ok ? Load::kLoaded : Load::kUnreadable;
}

bool SaveKey(const fs::path& dir, const std::string& id, const KeyRecord& record) {
    std::string sealed;
    std::string plain = Serialize(record);
    const bool ok = Seal(plain, sealed) && WriteFileAtomic(KeyPath(dir, id), sealed);
    crypto_wipe(plain.data(), plain.size());
    return ok;
}

bool SetAsideUnreadable(const fs::path& dir, const std::string& id) {
    std::error_code ec;
    fs::rename(KeyPath(dir, id), KeyPath(dir, id).string() + ".unreadable", ec);
    return !ec;
}

bool MoveKeyFiledByName(const fs::path& dir, const std::string& id, const std::string& nick) {
    std::error_code ec;
    const fs::path by_name = dir / ("name_" + nick + ".key");
    if (id.rfind("name_", 0) == 0 || fs::exists(KeyPath(dir, id), ec) || !fs::exists(by_name, ec)) return false;
    fs::rename(by_name, KeyPath(dir, id), ec);
    const fs::path tickets = dir / ("name_" + nick + ".tickets");
    if (!ec && fs::exists(tickets, ec)) fs::rename(tickets, TicketPath(dir, id), ec);
    return true;
}

// ---- tickets ---------------------------------------------------------------------------------------------------

std::map<std::string, std::string> ReadTicketFile(const fs::path& dir, const std::string& id) {
    std::map<std::string, std::string> f;
    std::string text;
    if (ReadFile(TicketPath(dir, id), text)) ParseFields(text, f);
    return f;
}

bool LoadTicket(const fs::path& dir, const std::string& id, const uint8_t (&master_key)[32],
                const uint8_t (&pub)[32], Ticket& out) {
    std::string text;
    if (!ReadFile(TicketPath(dir, id), text)) return false;
    std::map<std::string, std::string> f;
    ParseFields(text, f);
    const auto it = f.find(ToHex(master_key, 32));
    if (it == f.end() || !FromHex(it->second, reinterpret_cast<uint8_t*>(&out), sizeof(out))) return false;
    return std::memcmp(out.body.pubkey, pub, 32) == 0;
}

void SaveTicket(const fs::path& dir, const std::string& id, const uint8_t (&master_key)[32], const Ticket& ticket) {
    std::string text;
    std::map<std::string, std::string> f;
    if (ReadFile(TicketPath(dir, id), text)) ParseFields(text, f);
    f[ToHex(master_key, 32)] = ToHex(reinterpret_cast<const uint8_t*>(&ticket), sizeof(ticket));
    text.clear();
    for (const auto& [k, v] : f) text += k + "=" + v + "\n";
    WriteFileAtomic(TicketPath(dir, id), text);  // tickets are public; no sealing needed
}

void ForgetTickets(const fs::path& dir, const std::string& id) {
    std::error_code ec;
    fs::remove(TicketPath(dir, id), ec);  // tickets name the old key
}

// ---- backup ----------------------------------------------------------------------------------------------------

const char* BackupText(BackupResult r) {
    switch (r) {
        case BackupResult::kOk: return "";
        case BackupResult::kPasswordTooShort: return "use a password of at least 8 characters";
        case BackupResult::kNotABackup: return "not a LARecomp identity backup";
        case BackupResult::kWrongPassword: return "wrong password, or the file is damaged";
    }
    return "";
}

BackupResult MakeBackup(const KeyRecord& record, const std::string& password, std::string& file_bytes,
                        const BackupRandom* fixed) {
    KeyRecord copy = record;
    copy.pending_code.clear();  // a code belongs on paper, not in a file next to the key
    if (password.size() < kMinPasswordLength) return BackupResult::kPasswordTooShort;
    uint8_t salt[16], nonce[24], mac[16], key[32];
    if (fixed) {
        std::memcpy(salt, fixed->salt, 16);
        std::memcpy(nonce, fixed->nonce, 24);
    } else {
        RandomBytes(salt, 16);
        RandomBytes(nonce, 24);
    }
    BackupKey(password, salt, key);
    std::string plain = Serialize(copy);
    std::string cipher(plain.size(), '\0');
    crypto_aead_lock(reinterpret_cast<uint8_t*>(cipher.data()), mac, key, nonce,
                     reinterpret_cast<const uint8_t*>(kBackupMagic), 8,
                     reinterpret_cast<const uint8_t*>(plain.data()), plain.size());
    crypto_wipe(plain.data(), plain.size());
    crypto_wipe(key, 32);
    file_bytes.assign(kBackupMagic, 8);
    file_bytes.append(reinterpret_cast<const char*>(salt), 16);
    file_bytes.append(reinterpret_cast<const char*>(nonce), 24);
    file_bytes.append(reinterpret_cast<const char*>(mac), 16);
    file_bytes += cipher;
    return BackupResult::kOk;
}

BackupResult OpenBackup(const std::string& file, const std::string& password, KeyRecord& out) {
    if (file.size() < 8 + 16 + 24 + 16 + 1 || std::memcmp(file.data(), kBackupMagic, 8) != 0)
        return BackupResult::kNotABackup;
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
    KeyRecord restored;
    if (!ok || !Parse(plain, restored)) {
        crypto_wipe(plain.data(), plain.size());
        return BackupResult::kWrongPassword;
    }
    crypto_wipe(plain.data(), plain.size());
    out = restored;
    out.pending_code.clear();
    out.code_remind = 0;
    return BackupResult::kOk;
}

std::string BackupFileName(const std::string& nick) {
    std::string clean = nick;
    for (char& c : clean)
        if (std::string("<>:\"/\\|?*").find(c) != std::string::npos) c = '_';
    return "LARecomp identity - " + clean + ".lrid";
}

fs::path DefaultBackupPath(const std::string& nick) {
    const std::string name = BackupFileName(nick);
#ifdef _WIN32
    if (const wchar_t* home = _wgetenv(L"USERPROFILE"); home && *home)
        return fs::path(home) / "Documents" / name;
#else
    if (const char* home = std::getenv("HOME"); home && *home) return fs::path(home) / name;
#endif
    return fs::current_path() / name;
}

}  // namespace larecomp::identity::core
