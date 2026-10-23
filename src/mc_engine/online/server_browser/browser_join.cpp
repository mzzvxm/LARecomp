#include "browser_join.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>

#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

#include "../online_common.h"
#include "../system_link.h"

namespace larecomp::browser {
namespace {

using Clock = std::chrono::steady_clock;
constexpr uint32_t kUiManagerPtr = 0x8286D804;      // *: mcUIManager; +52 -> root node
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
JoinRequest g_request;      // guarded by g_mutex
JoinStatus g_status;        // guarded by g_mutex
bool g_request_new = false;  // guarded by g_mutex
// What the search hooks compare against; 0 = no filter. Written by the tick,
// read by the hooks (both on the guest thread, but the UI reads it too).
std::atomic<uint32_t> g_target_vip{0};
std::atomic<bool> g_world_ready{false};

constexpr uint32_t kPlayerManagerPtr = 0x82874374;  // *: mcPlayerManager
constexpr uint32_t kGetPlayerFn = 0x822A3998;       // (manager, index) -> player
constexpr auto kWorldWait = std::chrono::seconds(120);

// Guest thread only.
JoinPhase g_phase = JoinPhase::kIdle;
Clock::time_point g_phase_deadline{};
int g_start_step = 0;

rex::memory::Memory* Mem() {
    auto* rt = rex::Runtime::instance();
    return rt ? rt->memory() : nullptr;
}

template <typename... Args>
uint32_t CallGuest(uint32_t fn_addr, Args... args) {
    auto* rt = rex::Runtime::instance();
    auto* dispatcher = rt ? rt->function_dispatcher() : nullptr;
    PPCFunc* fn = dispatcher ? dispatcher->GetFunction(fn_addr) : nullptr;
    if (!fn) return 0;
    return rex::ppc::GuestToHostFunction<uint32_t>(fn, static_cast<uint32_t>(args)...);
}

void SetStatus(JoinPhase phase, std::string text) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_status.phase = phase;
    g_status.text = std::move(text);
}

// A guest copy of `name`, made once: state names are looked up every join.
uint32_t GuestName(const char* name) {
    static std::map<std::string, uint32_t> cache;
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

void Finish(JoinPhase phase, std::string text) {
    g_target_vip.store(0);
    g_phase = phase;
    g_start_step = 0;
    SetStatus(phase, std::move(text));
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

void CancelJoin() {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_request = JoinRequest{};
    g_request_new = true;
}

JoinStatus GetJoinStatus() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_status;
}

// Mirrors what the online menus' fill dereferences (sub_822A5680):
// player -> +880 -> +16 -> +48, an object with a vtable.
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
    return IsGuestPtr(c) && IsGuestPtr(GuestRead32(mem, c));
}

bool WorldReady() { return g_world_ready.load(std::memory_order_relaxed); }

void TickServerBrowserJoin() {
    // A few times a second is plenty; it is a guest call.
    static int frames = 0;
    if (++frames >= 15 || g_phase == JoinPhase::kStarting) {
        frames = 0;
        g_world_ready.store(ProbeWorld(), std::memory_order_relaxed);
    }
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
        if (!req.host_vip && !req.any_room) {
            g_phase = JoinPhase::kIdle;
            SetStatus(JoinPhase::kIdle, {});
            return;
        }
        LARECOMP_APP_INFO("[browser] join {} ({}, {}, {})", req.label,
                          req.any_room ? std::string("any session") : "host " + VipText(req.host_vip),
                          req.lan ? "System Link" : "Live", req.cruise ? "cruise" : "match");
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
            Finish(JoinPhase::kFailed, "The server did not answer: online could not be switched on");
        }
        break;

    case JoinPhase::kStarting: {
        const uint32_t hsm = Hsm();
        if (!hsm || !g_world_ready.load(std::memory_order_relaxed)) {
            // Boot screens, a load, the garage: wait for the city.
            if (g_start_step == 0) {
                SetStatus(JoinPhase::kStarting, "Waiting for the city to load to join " + req.label + "...");
                if (now > g_phase_deadline) Finish(JoinPhase::kFailed, "Join cancelled: the city never loaded");
            }
            break;
        }
        if (g_start_step == 0) {
            // The menu's onenter picks the net mode and clears the options.
            // Already in it (the browser opened from the SYSTEM LINK / XBOX
            // LIVE menu's own row): no goto. Re-entering it reloads the pause
            // movie, and the goto's tail (sub_82221BE0) then walks the parents
            // of the UIObject that reload freed (read of guest 0x50).
            const char* net_state = req.lan ? "LAN" : "Online";
            if (InState(hsm, net_state)) {
                LARECOMP_APP_INFO("[browser] already in '{}'; searching from it", net_state);
            } else if (!GotoState(hsm, net_state)) {
                Finish(JoinPhase::kFailed, "The game's online menu could not be opened");
                break;
            }
            g_start_step = 1;
            break;
        }
        // Next frame: the options the confirm screen would set, then the
        // search. The script's GameMode state names are swapped:
        // OLGameMode_Cruise sets 'Single' and OLGameMode_Single sets 'Cruise'.
        ActivateState(hsm, "OLGameType_Standard");
        ActivateState(hsm, req.cruise ? "OLGameMode_Single" : "OLGameMode_Cruise");
        // A picked room: only its host passes the search, and the game may
        // not host instead. Any room: the game's own rules, hosting included.
        g_target_vip.store(req.any_room ? 0 : req.host_vip);
        GotoState(hsm, "QuickMatchSearching");
        g_phase = JoinPhase::kSearching;
        g_phase_deadline = now + kSearchWait;
        SetStatus(JoinPhase::kSearching,
                  req.any_room ? "Looking for a session on " + req.label + " (none: you host it)..."
                               : "Searching for " + req.label + "...");
        break;
    }

    case JoinPhase::kSearching:
        if (now > g_phase_deadline) {
            if (req.any_room)
                Finish(JoinPhase::kIdle, {});  // joined or hosting by now
            else
                Finish(JoinPhase::kFailed, req.label + " did not answer the search");
        }
        break;

    case JoinPhase::kJoining:
        if (now > g_phase_deadline) Finish(JoinPhase::kIdle, {});
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
    static std::vector<PropDef> table;
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
    larecomp::browser::Finish(larecomp::browser::JoinPhase::kFailed,
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
