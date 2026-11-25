#include "browser_join.h"
#include "settings/immortal.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>

#include <rex/ppc/call.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/heap.h>

#include "../online_common.h"
#include "../system_link.h"
#include "browser_join_core.h"
#include "discord_rpc/discord_rpc.h"  // RpcActivity: where the game's state machine is
#include "join_server.h"
#include "join_server_core.h"
#include "larecomp_guest.h"
#include "mc_engine/keyboard_native.h"  // CurrentMovieName: the UI's current movie
#include "mc_engine/menus/pause_menu.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace larecomp::browser {
namespace {

using Clock = std::chrono::steady_clock;
namespace reason = joinserver::reason;
constexpr uint32_t kUiManagerPtr = 0x8286D804;      // *: mcUIManager; +52 -> root node
constexpr uint32_t kNavigatorAt = 52;               // mcUIManager +52 = FlashNavigator (the root node Hsm() reads)
constexpr uint32_t kNavLayerCountAt = 263992;       // FlashNavigator: modal message boxes in its FIFO, 0..3
// What the game's own search start reads (join_server_core.h, SearchFacts; generated code of native\tree-t11, sub_8226C038 > sub_82298F60 >
// sub_824FD470). The net manager is *0x8286D800 (system_link.cpp); every offset below is from that code, none was watched in a run.
constexpr uint32_t kNetMgrPtr = 0x8286D800;         // *: the net manager
constexpr uint32_t kNetSessionPtr = 0x8286F1D8;     // *: the net session manager; +696 = 3 once the quick match has started a search
constexpr uint32_t kLocalUsersPtr = 0x8288E5AC;     // *: P; *P = the local users' array (sub_82388580); entry 0's +4 = the player's slot index
constexpr uint32_t kNetMgrPlayersAt = 4284;         // net manager +4284: the player slots (10832 bytes each)
constexpr uint32_t kNetMgrSearchAt = 4292;          // +4292: the search manager (+0 = its busy word, +528 = the search object)
constexpr uint32_t kNetMgrOptionsAt = 4304;         // +4304: the holder of the options; its first word = the options (+2352 = the net mode)
constexpr uint32_t kPlayerSlotSize = 10832;
constexpr uint32_t kPlayerFlagsAt = 16 + 92;        // the slot's record starts at +16; its byte +92: 0x80 signed in, 0x40 on Live
constexpr uint32_t kSearchObjectAt = 528;
constexpr uint32_t kSearchBusyRecordAt = 1436;      // in the search object (sub_824FBD50)
constexpr uint32_t kOptionsNetModeAt = 2352;
constexpr uint32_t kSessionStateAt = 696;
constexpr uint32_t kFindStateFn = 0x82691650;       // (hsm, name) -> state
constexpr uint32_t kActivateStateFn = 0x8268DD70;   // (state, hsm)
constexpr uint32_t kHsmGotoVfunc = 32;              // VirtualHSM vtable: goto(state)
constexpr uint32_t kGuestMallocFn = 0x82130528;
constexpr uint32_t kPropTable = 0x827DEE20;
constexpr int kPropRecords = 32;
constexpr uint32_t kPropRecordSize = 76;
constexpr int kPropEnumSlots = 16;  // char* at +8 .. +71

constexpr auto kOnlineWait = std::chrono::seconds(12);
constexpr auto kSearchWait = std::chrono::seconds(45);
constexpr auto kJoinWait = std::chrono::seconds(30);

std::mutex g_mutex;
JoinRequest& g_request = larecomp::Leak<JoinRequest>();  // guarded by g_mutex
JoinStatus& g_status = larecomp::Leak<JoinStatus>();  // guarded by g_mutex
bool g_request_new = false;  // guarded by g_mutex
// What the search hooks compare against; 0 = no filter. Written by the tick,
// read by the hooks (both on the guest thread, but the UI reads it too).
std::atomic<uint32_t> g_target_vip{0};
std::atomic<bool> g_world_ready{false};

constexpr uint32_t kPlayerManagerPtr = 0x82874374;  // *: mcPlayerManager
// The pause manager: +4 is the game's paused flag, +10 the run-one-frame-while-paused bit (Game_IsPaused, sub_822BF228). The
// pause menu holds the game, so this is "the pause menu is up".
constexpr uint32_t kPauseStatePtr = 0x828CCEA0;
constexpr uint32_t kGetPlayerFn = 0x822A3998;       // (manager, index) -> player
constexpr auto kWorldWait = std::chrono::seconds(120);

// Guest thread only.
JoinPhase g_phase = JoinPhase::kIdle;
Clock::time_point g_phase_deadline{};
int g_start_step = 0;
bool g_world_wait_logged = false;  // the "waiting for the player's car" line was written for this join
bool g_roam_wait_logged = false;   // the "waiting for free roam" line was written for this join
joinserver::QuietGate g_message_gate;  // a message box on screen holds the join back (join_server_core.h, QuietGate::Allows)
bool g_message_wait_logged = false;    // the "a message box is on screen" line was written for this join
PauseGate g_pause_gate;  // what the join does about an open pause menu (browser_join_core.h)
PageWait g_page_wait;    // what the join waits for between the goto into the net menu and the search (browser_join_core.h)
SearchVerify g_search_verify;  // whether the goto into the search was taken (browser_join_core.h)

rex::kernel::GuestAddressSpace* Mem() {
    auto* rt = REX_RUNTIME();
    return rt ? larecomp::guest::AddressSpace(rt) : nullptr;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

// The game is paused (the game's own flag, as adaptive_triggers.cpp and the cutscene gallery read it): the world stands still, whoever
// paused it. NOT "the pause menu is up": in an online session the pause menu is a local pause and this stays false with it open, and
// the full map or a toolkit pause set it with no pause menu (PauseMenuOpen is the pause screen).
bool GamePaused() {
    auto* mem = Mem();
    if (!mem) return false;
    const uint32_t p = GuestRead32(mem, kPauseStatePtr);
    if (!IsGuestPtr(p)) return false;
    return *mem->TranslateVirtual<const uint8_t*>(p + 4) != 0 && *mem->TranslateVirtual<const uint8_t*>(p + 10) == 0;
}

// The UI's current movie is the pause movie (keyboard_native's tracker, as of the last input poll): the pause pages draw in it, and so
// does the net menu the join goes to. Any thread.
bool PauseMovieUp() {
    char name[32] = {};
    larecomp::keyboard::CurrentMovieName(name, sizeof(name));
    return std::strcmp(name, "PAUSEMOVIE") == 0;
}

// One look at the pause menu for the gate: the pause screen's open byte (the fact), the paused flag and the movie (what lingers).
Look LookAtPause() {
    Look look;
    look.pause_screen = PauseMenuOpen();
    look.paused = GamePaused();
    look.pause_movie = PauseMovieUp();
    return look;
}

// Where the game's state machine is (discord_rpc's activations: "Cruising", "Racing...", "Garage"; a pause menu in free roam is free roam).
Activity CurrentActivity() {
    switch (RpcActivity()) {
    case LarecompDiscordState::FreeRoam: return Activity::kFreeRoam;
    case LarecompDiscordState::Race: return Activity::kRace;
    case LarecompDiscordState::Garage: return Activity::kGarage;
    default: return Activity::kOther;
    }
}

const char* ActivityName(Activity a) {
    switch (a) {
    case Activity::kFreeRoam: return "free roam";
    case Activity::kRace: return "race";
    case Activity::kGarage: return "garage";
    default: return "other (title, load, ...)";
    }
}

// Whether `len` bytes at the host address `p` can be read (committed, not a guard page). A guest word that was never filled in
// (the player's car before it exists) is read as a pointer by the game; asking first keeps the probe below from faulting the game
// on a pointer that is not backed.
bool HostReadable(const uint8_t* p, size_t len) {
    if (!p || len == 0) return false;
#if defined(_WIN32)
    constexpr DWORD kRead = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                            PAGE_EXECUTE_WRITECOPY;
    size_t done = 0;
    while (done < len) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(p + done, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0 || (mbi.Protect & kRead) == 0) return false;
        const uint8_t* end = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        if (end <= p + done) return false;
        done = size_t(end - p);
    }
    return true;
#else
    // No VirtualQuery: the kernel reads one byte of every page of this very process, and says EFAULT for one that is reserved
    // and not committed (cars.cpp Mapped does the same).
    constexpr uintptr_t kPage = 0x1000;
    const uintptr_t first = reinterpret_cast<uintptr_t>(p);
    const uintptr_t end = first + len;
    for (uintptr_t page = first & ~(kPage - 1); page < end; page += kPage) {
        const uintptr_t at = page < first ? first : page;
        uint8_t probe = 0;
        iovec local{&probe, 1};
        iovec remote{reinterpret_cast<void*>(at), 1};
        const ssize_t n = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
        if (n < 0 && (errno == ENOSYS || errno == EPERM)) return true;  // not allowed here: the caller's range check is all there is
        if (n != 1) return false;
    }
    return true;
#endif
}

// One big-endian guest word, or false when its memory is not readable.
bool ReadGuestWord(uint32_t ea, uint32_t* out) {
    auto* mem = Mem();
    if (!mem || !PlausibleGuestPtr(ea)) return false;
    if (!HostReadable(mem->TranslateVirtual<const uint8_t*>(ea), 4)) return false;
    *out = GuestRead32(mem, ea);
    return true;
}

template <typename... Args>
uint32_t CallGuest(uint32_t fn_addr, Args... args) {
    auto* rt = REX_RUNTIME();
    PPCFunc* fn = rt ? rt->FindFunction(fn_addr).value_or(nullptr) : nullptr;
    if (!fn) return 0;
    return larecomp::guest::Call<uint32_t>(fn, static_cast<uint32_t>(args)...);
}

void SetStatus(JoinPhase phase, std::string text) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_status.phase = phase;
    g_status.text = std::move(text);
}

// A guest copy of `name`, made once: state names are looked up every join.
uint32_t GuestName(const char* name) {
    static std::map<std::string, uint32_t>& cache = larecomp::Leak<std::map<std::string, uint32_t>>();
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    auto* mem = Mem();
    if (!mem) return 0;
    const size_t len = std::strlen(name) + 1;
    const uint32_t p = CallGuest(kGuestMallocFn, uint32_t(len));
    if (!IsGuestPtr(p)) return 0;
    std::memcpy(mem->TranslateVirtual<uint8_t*>(p), name, len);
    cache[name] = p;
    return p;
}

uint32_t Hsm() {
    auto* mem = Mem();
    if (!mem) return 0;
    const uint32_t ui = GuestRead32(mem, kUiManagerPtr);
    if (!IsGuestPtr(ui)) return 0;
    const uint32_t root = GuestRead32(mem, ui + 52);
    return IsGuestPtr(root) ? root + 4 : 0;
}

uint32_t FindState(uint32_t hsm, const char* name) {
    const uint32_t n = GuestName(name);
    return n ? CallGuest(kFindStateFn, hsm, n) : 0;
}

bool GotoState(uint32_t hsm, const char* name) {
    auto* mem = Mem();
    const uint32_t state = FindState(hsm, name);
    if (!state || !mem) {
        LARECOMP_APP_WARN("[browser] no '{}' state to go to", name);
        return false;
    }
    const uint32_t goto_fn = GuestRead32(mem, GuestRead32(mem, hsm) + kHsmGotoVfunc);
    // The return value is the machine's last vfunc's, not whether it moved:
    // LAN returns 1, QuickMatchSearching 0, and both are entered. Only a
    // missing state counts as a failure.
    const uint32_t ret = CallGuest(goto_fn, hsm, state);
    LARECOMP_APP_INFO("[browser] goto({}) (returned {})", name, ret);
    return true;
}

// Whether the machine already sits in `name` or one of its children.
// sub_82691300's own walk: state vfunc +204 (hsm) -> its region, whose +0 is
// the region's current state; vfunc +328 -> parent state.
bool InState(uint32_t hsm, const char* name) {
    auto* mem = Mem();
    const uint32_t state = FindState(hsm, name);
    if (!state || !mem) return false;
    const uint32_t region = CallGuest(GuestRead32(mem, GuestRead32(mem, state) + 204), state, hsm);
    if (!IsGuestPtr(region)) return false;
    uint32_t s = GuestRead32(mem, region);
    for (int depth = 0; IsGuestPtr(s) && depth < 32; ++depth) {
        if (s == state) return true;
        s = CallGuest(GuestRead32(mem, GuestRead32(mem, s) + 328), s);
    }
    return false;
}

bool ActivateState(uint32_t hsm, const char* name) {
    const uint32_t state = FindState(hsm, name);
    if (!state) {
        LARECOMP_APP_WARN("[browser] no '{}' state to activate", name);
        return false;
    }
    CallGuest(kActivateStateFn, state, hsm);
    return true;
}

// The join ends: failed (`reason` is one of join_server_core.h's words, `text` what the strip shows) or idle (handed over to the
// game, which joined or hosts). --join_server follows it through JoinServerNoteFinish.
void Finish(JoinPhase phase, const char* why, std::string text) {
    g_target_vip.store(0);
    g_phase = phase;
    g_start_step = 0;
    g_pause_gate.Reset();
    g_page_wait = PageWait{};
    g_search_verify = SearchVerify{};
    SetStatus(phase, text);
    JoinServerNoteFinish(phase == JoinPhase::kFailed, why, text);
}

std::string VipText(uint32_t vip) {
    return std::to_string(vip >> 24) + "." + std::to_string((vip >> 16) & 0xFF) + "." +
           std::to_string((vip >> 8) & 0xFF) + "." + std::to_string(vip & 0xFF);
}

}  // namespace

void RequestJoin(const JoinRequest& request) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_request = request;
    g_request_new = true;
    g_status.phase = JoinPhase::kGoingOnline;
    g_status.text = "Preparing to join " + request.label + "...";
}

void ShowJoinFailure(const std::string& text) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_status.phase = JoinPhase::kFailed;
    g_status.text = text;
}

void CancelJoin() {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_request = JoinRequest{};
    g_request_new = true;
}

JoinStatus GetJoinStatus() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_status;
}

// The city is up and the player is in it: what the online menus' fill dereferences (sub_822A5680: player -> +880 -> +16 -> +48, an
// object with a vtable) AND the player's car, which the menu's setMenuCam probe reads (sub_82654738: racer+48 -> entity, entity+28 ->
// matrix, no null check). The front-end city behind the title screen has the first and not the second: a goto into the net menu
// from there was the crash of ojnew-cli (browser_join_core.h, "the player's car"). So this is false until the player has pressed
// Start and the car is on the road.
bool ProbeWorld() {
    auto* mem = Mem();
    if (!mem) return false;
    const uint32_t manager = GuestRead32(mem, kPlayerManagerPtr);
    if (!IsGuestPtr(manager)) return false;
    const uint32_t player = CallGuest(kGetPlayerFn, manager, 0u);
    if (!IsGuestPtr(player)) return false;
    const uint32_t a = GuestRead32(mem, player + 880);
    if (!IsGuestPtr(a)) return false;
    const uint32_t b = GuestRead32(mem, a + 16);
    if (!IsGuestPtr(b)) return false;
    const uint32_t c = GuestRead32(mem, b + 48);
    if (!IsGuestPtr(c) || !IsGuestPtr(GuestRead32(mem, c))) return false;
    return PlayerCarReady(player, ReadGuestWord);
}

bool WorldReady() { return g_world_ready.load(std::memory_order_relaxed); }

bool ModalMessageUp() {
    auto* mem = Mem();
    if (!mem) return false;
    const uint32_t ui = GuestRead32(mem, kUiManagerPtr);
    if (!IsGuestPtr(ui)) return false;
    const uint32_t nav = GuestRead32(mem, ui + kNavigatorAt);
    if (!IsGuestPtr(nav)) return false;
    const uint32_t count = GuestRead32(mem, nav + kNavLayerCountAt);
    return count != 0 && count <= 3;
}

// The words the game's own search start asks about, read behind readability checks (nothing is called, nothing written). A word that
// cannot be read stops the walk and says which one in failed_at. Guest thread.
joinserver::SearchFacts ReadSearchFacts() {
    joinserver::SearchFacts f;
    auto* mem = Mem();
    if (!mem) {
        f.failed_at = "no guest memory";
        return f;
    }
    const auto word = [&](uint32_t ea, uint32_t* out, const char* what) {
        if (ReadGuestWord(ea, out)) return true;
        f.failed_at = what;
        return false;
    };
    uint32_t netmgr = 0, players = 0, search = 0, holder = 0, options = 0, session = 0;
    if (!word(kNetMgrPtr, &netmgr, "the net manager pointer") || !word(netmgr + kNetMgrPlayersAt, &players, "the player slots") ||
        !word(netmgr + kNetMgrSearchAt, &search, "the search manager") || !word(netmgr + kNetMgrOptionsAt, &holder, "the options holder") ||
        !word(holder, &options, "the options") || !word(kNetSessionPtr, &session, "the net session manager"))
        return f;
    uint32_t net_mode = 0;
    if (!word(search, &f.search_flag, "the search manager's busy word") ||
        !word(search + kSearchObjectAt, &f.handle, "the search object's handle") ||
        !word(search + kSearchObjectAt + kSearchBusyRecordAt, &f.busy_record, "the search object's record") ||
        !word(options + kOptionsNetModeAt, &net_mode, "the net mode") || !word(session + kSessionStateAt, &f.session_state, "the session state"))
        return f;
    f.net_mode = int32_t(net_mode);
    if (f.busy_record != 0 && !word(f.busy_record, &f.busy_state, "the busy record")) return f;
    // The local player's slot index: the users' array, entry 0, its +4 (sub_82388580 and the lookup sub_82270170).
    uint32_t table = 0, array = 0, user = 0;
    if (!word(kLocalUsersPtr, &table, "the local users table") || !word(table, &array, "the local users array") ||
        !word(array, &user, "the first local user") || !word(user + 4, &f.player_slot, "the local player's index"))
        return f;
    if (f.player_slot >= 4) {
        f.failed_at = "a local player index above 3";
        return f;
    }
    const uint32_t flags_at = players + f.player_slot * kPlayerSlotSize + kPlayerFlagsAt;
    if (!PlausibleGuestPtr(flags_at) || !HostReadable(mem->TranslateVirtual<const uint8_t*>(flags_at), 1)) {
        f.failed_at = "the player's slot";
        return f;
    }
    f.player_flags = *mem->TranslateVirtual<const uint8_t*>(flags_at);
    f.read = true;
    return f;
}

void TickServerBrowserJoin() {
    // A few times a second is plenty; it is a guest call.
    static int frames = 0;
    if (++frames >= 15 || g_phase == JoinPhase::kStarting) {
        frames = 0;
        g_world_ready.store(ProbeWorld(), std::memory_order_relaxed);
    }
    JoinServerTick();  // --join_server: may file a RequestJoin, which the code below picks up in this same call
    JoinRequest req;
    bool fresh = false;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_request_new) {
            g_request_new = false;
            fresh = true;
        }
        req = g_request;
    }
    const auto now = Clock::now();

    if (fresh) {
        g_target_vip.store(0);
        g_start_step = 0;
        g_world_wait_logged = false;
        g_roam_wait_logged = false;
        g_message_gate = joinserver::QuietGate{};
        g_message_wait_logged = false;
        g_pause_gate.Reset();
        g_page_wait = PageWait{};
        g_search_verify = SearchVerify{};
        if (!req.host_vip && !req.any_room) {
            g_phase = JoinPhase::kIdle;
            SetStatus(JoinPhase::kIdle, {});
            return;
        }
        LARECOMP_APP_INFO("[browser] join {} ({}, {}, {})", req.label,
                          req.any_room ? std::string("any session") : "host " + VipText(req.host_vip),
                          req.lan ? "System Link" : "Live", req.cruise ? "cruise" : "match");
        {
            // Where the game is when the join is asked for: a crash report of a join names it (the pause screen, the flag, the movie,
            // the state machine's activity, whether a session is up).
            const Look look = LookAtPause();
            LARECOMP_APP_INFO("[browser] join asked for: pause screen {}, paused flag {}, pause movie {}, activity {}, session up {}",
                              look.pause_screen ? 1 : 0, look.paused ? 1 : 0, look.pause_movie ? 1 : 0, ActivityName(CurrentActivity()),
                              SystemLinkSessionUp() ? 1 : 0);
        }
        g_phase = JoinPhase::kGoingOnline;
        g_phase_deadline = now + kOnlineWait;
        if (!SystemLinkOnlineConnected() && SystemLinkOnlinePending() <= 0) SystemLinkOnlineRequest(true);
    }

    switch (g_phase) {
    case JoinPhase::kGoingOnline:
        if (SystemLinkOnlinePending() == 0 && SystemLinkOnlineConnected()) {
            g_phase = JoinPhase::kStarting;
            g_start_step = 0;
            g_phase_deadline = now + kWorldWait;
            SetStatus(JoinPhase::kStarting, "Opening " + std::string(req.lan ? "SYSTEM LINK" : "XBOX LIVE") + "...");
        } else if (now > g_phase_deadline) {
            Finish(JoinPhase::kFailed, reason::kOnlineFailed, "The server did not answer: online could not be switched on");
        }
        break;

    case JoinPhase::kStarting: {
        const uint32_t hsm = Hsm();
        if (!hsm || !g_world_ready.load(std::memory_order_relaxed)) {
            // Boot screens, the title screen, a load, the garage: wait for the city with the player's car in it (a goto into
            // the net menu without the car crashed the game, browser_join_core.h).
            if (g_start_step == 0) {
                if (!g_world_wait_logged) {
                    g_world_wait_logged = true;
                    LARECOMP_APP_INFO("[browser] the join waits for the player's car (none at the title screen, in a load or the garage)");
                }
                SetStatus(JoinPhase::kStarting, "Waiting for the city to load to join " + req.label + "...");
            }
            // The deadline holds in the second step too (the page was entered, then the world went away: a load, the title screen,
            // a quit from the pause menu). It was checked in the first step only, so a join that lost its world there waited for ever,
            // with no failure for --join_server's status file and no end for the player.
            // CHECK: not run in a game (a join that loses its world after the goto into the page); the first step's deadline is measured.
            if (now > g_phase_deadline) Finish(JoinPhase::kFailed, reason::kWorldTimeout, "Join cancelled: the city never loaded");
            break;
        }
        if (g_start_step == 0) {
            // Where the game is. A join starts from free roam only: from a race or the garage the goto into the net menu leaves the
            // machine half-built, and from inside a session the game never starts a search (browser_join_core.h, item 5). The title
            // and the loads on the way are other states: those are waited for.
            const Activity where = CurrentActivity();
            const bool in_session = SystemLinkSessionUp();
            const Refusal no = RefuseStart(where, in_session);
            if (no != Refusal::kNone) {
                const char* why = no == Refusal::kRace ? reason::kRaceOn : no == Refusal::kGarage ? reason::kGarageOpen : reason::kInSession;
                LARECOMP_APP_INFO("[browser] the join is refused: {} (activity {}, session up {})", why, ActivityName(where), in_session ? 1 : 0);
                Finish(JoinPhase::kFailed, why, joinserver::FailureMessage(why, joinserver::Target{}));
                break;
            }
            if (!AtFreeRoam(where)) {
                if (!g_roam_wait_logged) {
                    g_roam_wait_logged = true;
                    LARECOMP_APP_INFO("[browser] the join waits for free roam (activity {})", ActivityName(where));
                }
                SetStatus(JoinPhase::kStarting, "Waiting for free roam to join " + req.label + "...");
                if (now > g_phase_deadline) Finish(JoinPhase::kFailed, reason::kWorldTimeout, "Join cancelled: the game never got to free roam");
                break;
            }
            const int64_t now_ms = NowMs();
            // And no message box on screen. The game's first one after Start is still up when the machine reaches "Cruising", and a goto
            // into the net menu under it never got its search going (--join_server's runs, join_server_core.h, QuietGate); a player cannot
            // press anything under it either, so a join that was clicked earlier (while the city loaded) waits here. A box that was up
            // holds the join until it has been gone for a short while; no box seen = no wait. It cancels with message_box at the deadline.
            // CHECK: not run in a game (the F6 join clicked at the title screen); the wait itself is --join_server's, measured there.
            if (!g_message_gate.Allows(ModalMessageUp(), now_ms)) {
                if (!g_message_wait_logged) {
                    g_message_wait_logged = true;
                    LARECOMP_APP_INFO("[browser] the join waits for the game's message box to close");
                }
                SetStatus(JoinPhase::kStarting, "Waiting for the message box to close to join " + req.label + "...");
                if (now > g_phase_deadline)
                    Finish(JoinPhase::kFailed, reason::kMessageBox, joinserver::FailureMessage(reason::kMessageBox, joinserver::Target{}));
                break;
            }
            if (g_message_wait_logged) {
                g_message_wait_logged = false;  // once per wait: the pause gate below may keep this step going for a few ticks
                LARECOMP_APP_INFO("[browser] the message box is gone (it held the join back for {} ms); the join goes on",
                                  g_message_gate.HeldMs(now_ms));
            }
            const char* net_state = req.lan ? "LAN" : "Online";
            const Look look = LookAtPause();
            // The pause screen is open on the very page the join needs (SYSTEM LINK for a System Link join, XBOX LIVE for a Live one:
            // the browser was opened from its row): that is where the game itself starts its online searches, the page has been up for
            // seconds, and the search is entered from it with no transition of ours (browser_join_core.h, ChoosePath).
            if (look.pause_screen && ChoosePath(true, InState(hsm, net_state)) == Path::kInPage) {
                LARECOMP_APP_INFO("[browser] the pause menu is open on the '{}' page: searching from it, the way the game does (no close, no goto)", net_state);
                LARECOMP_APP_INFO("[browser] starting from: paused {}, in LAN {}, in Online {}", look.paused ? 1 : 0, req.lan ? 1 : 0, req.lan ? 0 : 1);
                g_page_wait.Start(now_ms);
                g_page_wait.Skip();  // the page is up: nothing to wait for
                g_start_step = 1;
                break;
            }
            // Any other start. Not with the pause menu up: the machine is inside the pause screen's own states, on another page, and
            // a goto from there takes down UI objects the pause screen still holds (the crash of "join while in the pause menu").
            // Close it the way CONTINUE does, wait until the game runs, and start from free roam (browser_join_core.h). "Up" is the
            // pause screen's open byte, not the paused flag: a pause menu in an online session leaves the flag at 0.
            const bool waited = g_pause_gate.SawPause();
            const PauseGate::Step gate = g_pause_gate.Update(look, now_ms);
            if (gate != PauseGate::Step::kProceed) {
                if (gate == PauseGate::Step::kClose) {
                    const bool closed = PauseMenuClose();
                    LARECOMP_APP_INFO("[browser] the pause menu is open: closing it before the join starts (try {}{})",
                                      g_pause_gate.tries(), closed ? "" : "; the game's close is not available");
                }
                const bool by_player = g_pause_gate.NeedsPlayer(now_ms);
                SetStatus(JoinPhase::kStarting, (by_player ? "Close the pause menu to join " : "Closing the pause menu to join ") +
                                                    req.label + (by_player ? "" : "..."));
                if (now > g_phase_deadline) Finish(JoinPhase::kFailed, reason::kPauseOpen, "Join cancelled: the pause menu stayed open");
                break;
            }
            if (waited) LARECOMP_APP_INFO("[browser] the pause menu is closed; the join starts from free roam");
            // The menu's onenter picks the net mode and clears the options.
            // Already in it (the browser opened from the SYSTEM LINK / XBOX
            // LIVE menu's own row): no goto. Re-entering it reloads the pause
            // movie, and the goto's tail (sub_82221BE0) then walks the parents
            // of the UIObject that reload freed (read of guest 0x50).
            const bool in_lan = InState(hsm, "LAN");
            const bool in_online = InState(hsm, "Online");
            const bool in_target = req.lan ? in_lan : in_online;
            // Where the machine is when the goto is made: the next crash report of a join names it.
            LARECOMP_APP_INFO("[browser] starting from: paused {}, in LAN {}, in Online {}", look.paused ? 1 : 0, in_lan ? 1 : 0,
                              in_online ? 1 : 0);
            g_page_wait.Start(now_ms);
            if (in_target) {
                LARECOMP_APP_INFO("[browser] already in '{}'; searching from it", net_state);
                g_page_wait.Skip();  // the page is up: nothing to wait for
            } else if (!GotoState(hsm, net_state)) {
                Finish(JoinPhase::kFailed, reason::kMenuUnavailable, "The game's online menu could not be opened");
                break;
            }
            g_start_step = 1;
            break;
        }
        // The net menu's page is a pause page the goto just loaded: the search is entered from it when it is up, as a player does
        // (a few frames and a quarter of a second after it showed), not on the very next frame (browser_join_core.h, item 4).
        const char* net_state = req.lan ? "LAN" : "Online";
        const bool in_page = InState(hsm, net_state);
        const bool page_shown = PauseMovieUp();
        if (!g_page_wait.Ready(in_page, page_shown, NowMs())) {
            SetStatus(JoinPhase::kStarting, "Opening " + std::string(req.lan ? "SYSTEM LINK" : "XBOX LIVE") + " to join " + req.label + "...");
            break;
        }
        if (g_page_wait.GaveUp())
            LARECOMP_APP_WARN("[browser] the '{}' page did not report being up within {} ms (in it {}, pause movie {}): the search goes on",
                              net_state, PageWait::kGiveUpMs, in_page ? 1 : 0, page_shown ? 1 : 0);
        // The options the confirm screen would set, then the search. The script's GameMode state names are swapped:
        // OLGameMode_Cruise sets 'Single' and OLGameMode_Single sets 'Cruise'.
        ActivateState(hsm, "OLGameType_Standard");
        ActivateState(hsm, req.cruise ? "OLGameMode_Single" : "OLGameMode_Cruise");
        // A picked room: only its host passes the search, and the game may
        // not host instead. Any room: the game's own rules, hosting included.
        g_target_vip.store(req.any_room ? 0 : req.host_vip);
        GotoState(hsm, "QuickMatchSearching");
        g_search_verify.Start(NowMs());
        g_phase = JoinPhase::kSearching;
        g_phase_deadline = now + kSearchWait;
        SetStatus(JoinPhase::kSearching,
                  req.any_room ? "Looking for a session on " + req.label + " (none: you host it)..."
                               : "Searching for " + req.label + "...");
        break;
    }

    case JoinPhase::kSearching: {
        // The goto into the search is looked at: a moment later the machine must be in it, or past the page it started from. Still in
        // the page and not in the search = the goto was dropped (the free-roam run, ojnew-cli, where the relay never saw the game): ask
        // again, twice at most.
        if (g_search_verify.Active()) {
            const uint32_t hsm = Hsm();
            if (hsm) {
                const char* net_state = req.lan ? "LAN" : "Online";
                const auto verdict = g_search_verify.Update(InState(hsm, "QuickMatchSearching"), InState(hsm, net_state), NowMs());
                if (verdict == SearchVerify::Verdict::kRetry) {
                    LARECOMP_APP_WARN("[browser] the game is still on the '{}' page and not in the search after the goto: asking for the search again (try {})",
                                      net_state, g_search_verify.retries());
                    ActivateState(hsm, "OLGameType_Standard");
                    ActivateState(hsm, req.cruise ? "OLGameMode_Single" : "OLGameMode_Cruise");
                    GotoState(hsm, "QuickMatchSearching");
                } else if (verdict == SearchVerify::Verdict::kGiveUp) {
                    LARECOMP_APP_WARN("[browser] the game never entered the search ({} goto tries): the join waits out its time", g_search_verify.retries() + 1);
                }
            }
        }
        if (now > g_phase_deadline) {
            if (!req.any_room)
                Finish(JoinPhase::kFailed, reason::kHostNoAnswer, req.label + " did not answer the search");
            else if (SystemLinkSessionUp())
                Finish(JoinPhase::kIdle, "", {});  // joined, or hosting for the next player
            else
                Finish(JoinPhase::kFailed, reason::kNoSession, joinserver::FailureMessage(reason::kNoSession, joinserver::Target{}));
        }
        break;
    }

    case JoinPhase::kJoining:
        if (now > g_phase_deadline) {
            if (SystemLinkSessionUp())
                Finish(JoinPhase::kIdle, "", {});
            else
                Finish(JoinPhase::kFailed, reason::kNoSession, joinserver::FailureMessage(reason::kNoSession, joinserver::Target{}));
        }
        break;

    default:
        break;
    }
}

// ------------------------------------------------------------ properties ---

namespace {

struct PropDef {
    uint32_t id = 0;
    std::string name;
    std::vector<std::string> values;
};

// The game fills some ids in at boot, so the table is read once it is complete
// enough (GameMode, the first record, is always set).
const std::vector<PropDef>* PropTable() {
    static std::mutex m;
    static std::vector<PropDef>& table = larecomp::Leak<std::vector<PropDef>>();
    static bool ready = false;
    std::lock_guard<std::mutex> lk(m);
    if (ready) return &table;
    auto* mem = Mem();
    if (!mem) return nullptr;
    std::vector<PropDef> t;
    for (int i = 0; i < kPropRecords; ++i) {
        const uint32_t rec = kPropTable + uint32_t(i) * kPropRecordSize;
        PropDef d;
        d.id = GuestRead32(mem, rec);
        const uint32_t name = GuestRead32(mem, rec + 4);
        if (IsGuestPtr(name)) d.name = GuestCStr(mem, name, 32);
        for (int k = 0; k < kPropEnumSlots; ++k) {
            const uint32_t v = GuestRead32(mem, rec + 8 + uint32_t(k) * 4);
            if (!IsGuestPtr(v)) break;
            d.values.push_back(GuestCStr(mem, v, 32));
        }
        t.push_back(std::move(d));
    }
    if (t.empty() || t[0].id == 0) return nullptr;
    table = std::move(t);
    ready = true;
    return &table;
}

const PropDef* Lookup(const std::vector<PropDef>& table, uint32_t id) {
    if (id == 0 || id == 0xFFFFFFFFu) return nullptr;
    for (const auto& d : table)
        if (d.id == id) return &d;
    return nullptr;
}

}  // namespace

std::vector<PropText> DescribeProps(const std::vector<Prop>& props) {
    std::vector<PropText> out;
    const auto* table = PropTable();
    if (!table) return out;
    for (const Prop& p : props) {
        const PropDef* d = Lookup(*table, p.id);
        if (!d || d->name.empty()) continue;
        PropText t;
        t.name = d->name;
        t.value = p.value < d->values.size() ? d->values[p.value] : std::to_string(p.value);
        out.push_back(std::move(t));
    }
    return out;
}

void ClassifyRoom(const std::vector<Prop>& props, bool* cruise, bool* lan) {
    *cruise = false;
    *lan = true;
    for (const auto& t : DescribeProps(props)) {
        if (t.name == "GameMode") *cruise = t.value == "Cruise";
        if (t.name == "NetMode") *lan = t.value == "LAN";
    }
}

}  // namespace larecomp::browser

// ---------------------------------------------------------------- hooks ---

using larecomp::browser::g_target_vip;

void Hook_BrowserLanResult(PPCRegister& r1, PPCRegister& r27) {
    const uint32_t target = g_target_vip.load(std::memory_order_relaxed);
    if (!target || (r27.u64 & 0xFF) == 0) return;
    auto* mem = larecomp::browser::Mem();
    if (!mem) return;
    const uint32_t from = GuestRead32(mem, r1.u32 + 0x58);
    if (from == target) return;
    r27.u64 = 0;  // treated like a duplicate: not kept
    static uint32_t last_logged = 0;
    if (from != last_logged) {
        last_logged = from;
        LARECOMP_APP_INFO("[browser] System Link answer from {} skipped (want {})",
                          larecomp::browser::VipText(from), larecomp::browser::VipText(target));
    }
}

void Hook_BrowserLiveResult(PPCRegister& r30, PPCRegister& r3) {
    const uint32_t target = g_target_vip.load(std::memory_order_relaxed);
    if (!target || (r3.u64 & 0xFF) == 0) return;
    auto* mem = larecomp::browser::Mem();
    if (!mem || !IsGuestPtr(r30.u32)) return;
    const uint32_t vip = GuestRead32(mem, r30.u32 + 8);
    if (vip == target) return;
    r3.u64 = 0;
    LARECOMP_APP_INFO("[browser] Live result {} skipped (want {})", larecomp::browser::VipText(vip),
                      larecomp::browser::VipText(target));
}

bool Hook_BrowserCreateGameVeto(PPCRegister& r27) {
    (void)r27;
    if (!g_target_vip.load(std::memory_order_relaxed)) return false;
    LARECOMP_APP_INFO("[browser] the picked room was not found; not hosting a game instead");
    larecomp::browser::Finish(larecomp::browser::JoinPhase::kFailed, larecomp::browser::reason::kRoomGone,
                              "Room not found (it closed, is full, or is in another mode)");
    return true;
}

void Hook_BrowserJoinSession(PPCRegister& r3) {
    (void)r3;
    if (larecomp::browser::g_phase != larecomp::browser::JoinPhase::kSearching) return;
    larecomp::browser::g_phase = larecomp::browser::JoinPhase::kJoining;
    larecomp::browser::g_phase_deadline =
        larecomp::browser::Clock::now() + larecomp::browser::kJoinWait;
    larecomp::browser::SetStatus(larecomp::browser::JoinPhase::kJoining, "Joining...");
    LARECOMP_APP_INFO("[browser] the game is joining the picked room");
}
