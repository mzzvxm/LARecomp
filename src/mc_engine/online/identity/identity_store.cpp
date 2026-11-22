#include "identity_store.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include <rex/kernel/title_runtime.h>
#include <rex/xam/user_profile.h>

#include "larecomp_log.h"
#include "identity_core.h"

namespace larecomp::identity {

namespace fs = std::filesystem;

// The tool writes "never remind" with the core's constant: it must be the game's.
static_assert(kNeverRemind == core::kNeverRemind, "identity_core.h and identity_store.h disagree about kNeverRemind");

namespace {

// The file format, DPAPI, the ticket cache and the backup file live in identity_core.cpp (larecomp_online_tool links it
// too); this file is the game's one loaded profile around it: its mutex, its log lines and the SDK's profile.
struct State {
    bool loaded = false;
    bool restart_needed = false;
    bool read_only = false;  // the key file is one that could not be read or moved aside: nothing is written over it
    std::string profile_id;
    std::string nick;
    core::KeyRecord rec;  // the seed, RMR token and pending recovery code, as in the key file
    KeyPair keys{};
    uint64_t xuid = 0;
};

std::mutex g_mtx;
State g;

fs::path IdentityDir() { return core::DefaultIdentityDir(); }
fs::path KeyPath() { return core::KeyPath(IdentityDir(), g.profile_id); }

void DeriveFromSeed(State& s) {
    s.keys = KeyPairFromSeed(s.rec.seed);
    s.xuid = XuidFromPublicKey(s.keys.pub);
}

bool SaveLocked() {
    if (g.read_only) return false;  // already reported when it was found
    const bool ok = core::SaveKey(IdentityDir(), g.profile_id, g.rec);
    if (!ok) LARECOMP_APP_ERROR("[identity] cannot save {}", KeyPath().string());
    return ok;
}

// The id of the profile folder: the SDK's per-profile XUID (UserProfile::GetXUID), or the profile name for a profile
// that still has the legacy XUID.
std::string ProfileIdFor(const rex::xam::UserProfile* p) { return core::ProfileId(p->GetXUID(), p->GetName()); }

}  // namespace

void ApplyProfileIdentity() {
    auto* rt = REX_RUNTIME();
    auto* profile = rt ? &rt->GetUserProfile() : nullptr;
    if (!profile) {
        LARECOMP_APP_WARN("[identity] no profile; online identity off");
        return;
    }
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g.loaded) return;
    g.profile_id = ProfileIdFor(profile);
    g.nick = profile->GetName();

    // An SDK without per-profile XUIDs files the key by name; once the SDK
    // has them, that key is this profile's and moves to the new id.
    if (core::MoveKeyFiledByName(IdentityDir(), g.profile_id, g.nick))
        LARECOMP_APP_INFO("[identity] key of '{}' moved to profile id {}", g.nick, g.profile_id);

    bool have = false;
    switch (core::LoadKey(IdentityDir(), g.profile_id, g.rec)) {
        case core::Load::kLoaded:
            have = true;
            break;
        case core::Load::kUnreadable:
            // Not ours to read (another Windows user, or damaged). Keep it
            // aside instead of overwriting: a backup may still restore it.
            if (core::SetAsideUnreadable(IdentityDir(), g.profile_id)) {
                LARECOMP_APP_ERROR("[identity] {} is unreadable here; moved aside, starting a new identity",
                                   KeyPath().string());
            } else {
                g.read_only = true;
                LARECOMP_APP_ERROR("[identity] {} is unreadable here and could not be moved aside; this session uses a "
                                   "new identity that is not saved, and the file is left alone",
                                   KeyPath().string());
            }
            break;
        case core::Load::kMissing:
            break;
    }
    if (!have) {
        RandomBytes(g.rec.seed, 32);
        if (!SaveLocked())
            LARECOMP_APP_ERROR("[identity] the new identity will not survive this session");
        else
            LARECOMP_APP_INFO("[identity] new identity for '{}' in {}", g.nick, KeyPath().string());
    }
    DeriveFromSeed(g);
    g.loaded = true;
    // The guest's XUID comes from the identity: the title is loaded and has not asked for it yet (UserProfile::set_xuid is
    // for exactly this, a front end deriving the XUID from its own key).
    profile->set_xuid(g.xuid);
    LARECOMP_APP_INFO("[identity] '{}' key {} XUID {:016X}", g.nick, Fingerprint(g.keys.pub), g.xuid);
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
    if (!g.loaded || !g.rec.has_rmr) return std::nullopt;
    return g.rec.rmr;
}

void SetRmrToken(const std::array<uint8_t, 16>& token) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g.loaded) return;
    g.rec.rmr = token;
    g.rec.has_rmr = true;
    SaveLocked();
}

std::string PendingRecoveryCode() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g.rec.pending_code;
}

void SetPendingRecoveryCode(const std::string& code) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g.loaded) return;
    g.rec.pending_code = code;
    g.rec.code_remind = 0;
    SaveLocked();
}

void AcknowledgeRecoveryCode() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g.loaded || g.rec.pending_code.empty()) return;
    g.rec.pending_code.clear();
    g.rec.code_remind = 0;
    SaveLocked();
}

int64_t RecoveryCodeRemindAfter() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g.rec.code_remind;
}

void SetRecoveryCodeRemindAfter(int64_t when) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g.loaded || g.rec.pending_code.empty() || g.rec.code_remind == when) return;
    g.rec.code_remind = when;
    SaveLocked();
}

bool CachedTicket(const uint8_t (&master_key)[32], Ticket& out) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g.loaded) return false;
    return core::LoadTicket(IdentityDir(), g.profile_id, master_key, g.keys.pub, out);
}

void StoreTicket(const uint8_t (&master_key)[32], const Ticket& ticket) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g.loaded) return;
    core::SaveTicket(IdentityDir(), g.profile_id, master_key, ticket);
}

fs::path DefaultBackupPath() { return core::DefaultBackupPath(Nick()); }

bool ExportBackup(const fs::path& path, const std::string& password, std::string* error) {
    core::KeyRecord copy;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (!g.loaded) {
            if (error) *error = "no identity loaded";
            return false;
        }
        copy = g.rec;
    }
    std::string file;
    const core::BackupResult made = core::MakeBackup(copy, password, file);
    if (made != core::BackupResult::kOk) {
        if (error) *error = core::BackupText(made);
        return false;
    }
    if (!core::WriteFileAtomic(path, file)) {
        if (error) *error = "cannot write " + path.string();
        return false;
    }
    LARECOMP_APP_INFO("[identity] backup written to {}", path.string());
    return true;
}

bool ImportBackup(const fs::path& path, const std::string& password, std::string* error) {
    std::string file;
    if (!core::ReadFile(path, file)) {
        if (error) *error = "cannot read " + path.string();
        return false;
    }
    core::KeyRecord restored;
    const core::BackupResult opened = core::OpenBackup(file, password, restored);
    if (opened != core::BackupResult::kOk) {
        if (error) *error = core::BackupText(opened);
        return false;
    }
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g.loaded) {
        if (error) *error = "no profile loaded";
        return false;
    }
    // Keep what this session runs with (the guest has the old XUID) and write
    // the restored identity for the next launch.
    core::KeyRecord next = g.rec;
    std::memcpy(next.seed, restored.seed, 32);
    // The key being replaced (or a file that could not be read) is kept beside the new one, so a wrong backup can be undone.
    // identity/test/identity_adapter_test.cpp: scenarios importkeeps and locked (the code before the split has neither rule, so
    // identity_adapter_compare.ps1 runs them on the code after the split alone).
    if ((g.read_only || !next.SameKeyAs(g.rec)) && !core::KeepCopyOfKey(IdentityDir(), g.profile_id)) {
        if (error) *error = "cannot keep a copy of the current key in " + IdentityDir().string();
        return false;
    }
    next.has_rmr = restored.has_rmr;
    next.rmr = restored.rmr;
    next.pending_code.clear();
    if (!core::SaveKey(IdentityDir(), g.profile_id, next)) {
        if (error) *error = "cannot save " + KeyPath().string();
        return false;
    }
    core::ForgetTickets(IdentityDir(), g.profile_id);  // tickets name the old key
    g.read_only = false;  // the file is a readable key of the backup's now
    g.restart_needed = true;
    LARECOMP_APP_INFO("[identity] backup {} imported; restart the game to use it", path.string());
    return true;
}

bool RestartNeeded() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g.restart_needed;
}

}  // namespace larecomp::identity
