#pragma once
//
// The player's online identity on this PC: the key that owns the gamertag
// (identity_protocol.h), the guest XUID that key gives, and the Rate My Ride /
// leaderboard token.
//
// None of it lives in the save folder, so sharing a save shares no identity.
// Each profile folder carries only a random id (the SDK's profile.xuid); the
// key is under %LOCALAPPDATA%\larecomp\identity\<id>.key, encrypted with DPAPI
// (readable by this Windows user only). Elsewhere: ~/.local/share/larecomp/
// identity, mode 0600. The same save on another PC finds no key there and
// starts a new identity (new XUID; the nick needs a backup or its recovery
// code to follow).
//

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "../protocol/identity_protocol.h"

namespace larecomp::identity {

// Loads or creates the identity of the signed-in profile and hands its XUID
// to the guest. OnPostSetup, before the guest runs; once.
void ApplyProfileIdentity();

// False until ApplyProfileIdentity worked (or when it could not).
bool Loaded();

std::string Nick();  // the profile's gamertag

// The folder the saves live in (<root>/<gamertag>/...). Set once from
// SetupEnvironment; empty until then.
void SetUserDataRoot(const std::filesystem::path& root);
std::filesystem::path UserDataRoot();
uint64_t Xuid();
KeyPair Keys();
std::string KeyFingerprint();

// Rate My Ride / leaderboard owner token, kept with the key. Empty until set.
std::optional<std::array<uint8_t, 16>> RmrToken();
void SetRmrToken(const std::array<uint8_t, 16>& token);

// A recovery code the master handed out, kept until the player confirms he
// wrote it down (it is never shown again).
std::string PendingRecoveryCode();
void SetPendingRecoveryCode(const std::string& code);
void AcknowledgeRecoveryCode();

// When the identity window may open by itself again for the pending code:
// 0 = now, a unix time = from then on, kNeverRemind = only from F8. A new code
// resets it to 0.
constexpr int64_t kNeverRemind = -1;
int64_t RecoveryCodeRemindAfter();
void SetRecoveryCodeRemindAfter(int64_t when);

// Last ticket from the master with this key, per master.
bool CachedTicket(const uint8_t (&master_key)[32], Ticket& out);
void StoreTicket(const uint8_t (&master_key)[32], const Ticket& ticket);

// Backup: the key and token in a file sealed with a password (Argon2id +
// XChaCha20-Poly1305). Importing replaces this profile's identity; it takes
// effect on the next launch (the guest already has the old XUID).
std::filesystem::path DefaultBackupPath();
bool ExportBackup(const std::filesystem::path& path, const std::string& password, std::string* error);
bool ImportBackup(const std::filesystem::path& path, const std::string& password, std::string* error);
bool RestartNeeded();

}  // namespace larecomp::identity
