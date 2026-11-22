#pragma once
//
// The core of the player's online identity files: what is in a key file, how it is sealed, the ticket cache, the
// backup file, and which file a profile's key lives in. No ReXGlue, no settings, no logging: the game (identity_store.cpp,
// which adds the one loaded profile, its mutex and its log lines) and larecomp_online_tool (online_tool/
// identity_commands.cpp) both link this file, so a key written by one is read by the other, byte for byte.
//
// On disk (see identity_store.h for why it is outside the save folder):
//   <dir>/<profile id>.key      "LRIDSTORE1\nseed=<64 hex>\n[rmr=<32 hex>\n][code=<recovery code>\n[code_remind=<n>\n]]",
//                               sealed with DPAPI (this Windows user only); plain text with mode 0600 elsewhere
//   <dir>/<profile id>.tickets  "<master public key hex>=<ticket hex>" lines; public, never sealed
//   *.lrid                      a backup: "LRIDBAK1" + Argon2id salt + XChaCha20-Poly1305 nonce, MAC and body
//
// Every function that changes a file writes the whole file through a temporary one and a rename, so a crash never
// leaves half a key. None of them knows whether the game is running: whoever calls them for the tool checks that first
// (online_tool/game_guard.h), because the game reads and rewrites the whole key file on its own changes.
//

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

#include "../protocol/identity_protocol.h"

namespace larecomp::identity::core {

namespace fs = std::filesystem;

// The reminder time of a recovery code that is never to open the identity window by itself again (the same value as
// identity_store.h's kNeverRemind).
constexpr int64_t kNeverRemind = -1;

// The XUID every profile shared before the SDK had one per profile: such a profile is filed by its name.
constexpr uint64_t kLegacyXuid = 0xB13EBABEBABEBABEull;

// ---- the key file ----------------------------------------------------------------------------------------------

// What a key file holds. The secrets in it (seed, RMR token, recovery code) are wiped when the object goes away.
struct KeyRecord {
    uint8_t seed[32] = {};
    bool has_rmr = false;
    std::array<uint8_t, 16> rmr{};
    std::string pending_code;  // a recovery code the master handed out, kept until the player confirms it
    int64_t code_remind = 0;   // when the identity window may open by itself again for the code (see identity_store.h)

    KeyRecord() = default;
    KeyRecord(const KeyRecord&) = default;
    KeyRecord& operator=(const KeyRecord&) = default;
    ~KeyRecord() { Wipe(); }

    void Wipe();
    // The public key, the key pair and the XUID this seed gives.
    KeyPair Keys() const;
    uint64_t Xuid() const;
    std::string Fingerprint() const;
    bool SameKeyAs(const KeyRecord& other) const;  // same seed
};

// The plain text of a key file and back. Parse is false when it is not a key file or its seed is not 64 hex digits.
std::string Serialize(const KeyRecord& record);
bool Parse(const std::string& text, KeyRecord& out);

// DPAPI on Windows (current user, no UI, "larecomp identity"); a copy elsewhere. False when the OS refuses.
bool Seal(const std::string& plain, std::string& out);
bool Unseal(const std::string& sealed, std::string& out);
// "windows_dpapi" or "plain_file": how Seal protects a key on this OS.
const char* Protection();

bool ReadFile(const fs::path& path, std::string& out);
// Through "<path>.tmp" and a rename; creates the folder; mode 0600 elsewhere than Windows.
bool WriteFileAtomic(const fs::path& path, const std::string& data);

// %LOCALAPPDATA%\larecomp\identity (Windows), $XDG_DATA_HOME or ~/.local/share/larecomp/identity (elsewhere).
fs::path DefaultIdentityDir();
fs::path KeyPath(const fs::path& dir, const std::string& profile_id);
fs::path TicketPath(const fs::path& dir, const std::string& profile_id);

// The id a profile's files are named by: its XUID as 16 upper-case hex digits, or "name_<name>" for a profile that
// still has the legacy XUID.
std::string ProfileId(uint64_t xuid, const std::string& name);
// True for what ProfileId makes: 16 hex digits, or "name_" and 1 to 15 characters that are no path separator. The
// tool takes an id from its command line and builds a file name out of it, so this is the gate.
bool ValidProfileId(const std::string& id);
// <saves>/<gamertag>/profile.xuid, the SDK's per-profile id (16 hex digits). False when it is not there or unreadable.
bool ReadProfileXuid(const fs::path& saves_root, const std::string& gamertag, uint64_t& xuid);

enum class Load { kLoaded, kMissing, kUnreadable };
// kMissing: no file that can be read. kUnreadable: a file that is not ours to read (another Windows user, or damaged).
Load LoadKey(const fs::path& dir, const std::string& profile_id, KeyRecord& out);
// Seals and writes the whole record. False when it could not be written.
bool SaveKey(const fs::path& dir, const std::string& profile_id, const KeyRecord& record);
// "<id>.key" -> "<id>.key.unreadable", for a file that cannot be read: a backup may still restore it. An earlier set-aside file is
// never overwritten (the next is ".unreadable-2" and so on). False when nothing was moved; `kept` (optional) names the new file.
bool SetAsideUnreadable(const fs::path& dir, const std::string& profile_id, fs::path* kept = nullptr);
// Copies a key file that is about to be replaced by another identity to "<id>.key.replaced-<unix time>" (a free name), so the
// replaced key can be put back. True when there was no key file or the copy was made.
bool KeepCopyOfKey(const fs::path& dir, const std::string& profile_id, fs::path* kept = nullptr);
// An SDK without per-profile XUIDs filed the key by name; once it has them, that key is this profile's and moves to the
// new id (and its tickets with it). True when something moved.
bool MoveKeyFiledByName(const fs::path& dir, const std::string& profile_id, const std::string& nick);

// ---- tickets ---------------------------------------------------------------------------------------------------

// The ticket the master with this key last gave this key, when it is there and is for `pub`.
bool LoadTicket(const fs::path& dir, const std::string& profile_id, const uint8_t (&master_key)[32],
                const uint8_t (&pub)[32], Ticket& out);
void SaveTicket(const fs::path& dir, const std::string& profile_id, const uint8_t (&master_key)[32],
                const Ticket& ticket);
void ForgetTickets(const fs::path& dir, const std::string& profile_id);
// Every "<master key hex>" -> "<ticket hex>" of the file (empty when there is none).
std::map<std::string, std::string> ReadTicketFile(const fs::path& dir, const std::string& profile_id);

// ---- backup ----------------------------------------------------------------------------------------------------

constexpr size_t kMinPasswordLength = 8;

enum class BackupResult {
    kOk,
    kPasswordTooShort,  // export: under kMinPasswordLength bytes
    kNotABackup,        // import: not a LARecomp identity backup file
    kWrongPassword,     // import: wrong password, or the file is damaged (the two cannot be told apart)
};
// The words the game shows for each.
const char* BackupText(BackupResult r);

// The random parts of a backup, so a check can make the same file twice.
struct BackupRandom {
    uint8_t salt[16];
    uint8_t nonce[24];
};

// The backup file's bytes for `record`: key and RMR token, never the pending recovery code (that belongs on paper, not
// in a file next to the key). `fixed` is for the checks; leave it out otherwise.
BackupResult MakeBackup(const KeyRecord& record, const std::string& password, std::string& file_bytes,
                        const BackupRandom* fixed = nullptr);
// Opens a backup file's bytes. The pending code of `out` is empty.
BackupResult OpenBackup(const std::string& file_bytes, const std::string& password, KeyRecord& out);

// "LARecomp identity - <nick>.lrid" with the characters a file name cannot hold replaced by "_".
std::string BackupFileName(const std::string& nick);
// <Documents>/BackupFileName(nick) (the home folder elsewhere than Windows, the current folder as a last resort).
fs::path DefaultBackupPath(const std::string& nick);

}  // namespace larecomp::identity::core
