#include "identity_client.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "settings/larecomp_settings.h"

#include "larecomp_log.h"
#include "../rate_my_ride/rmr_net.h"
#include "../relay_info.h"
#include "../protocol/identity_protocol.h"
#include "identity_store.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#endif

namespace larecomp::identity {

namespace {

using Clock = std::chrono::steady_clock;
using Fields = net::Fields;

std::mutex g_mtx;
std::condition_variable g_cv;
Status g_status;
bool g_started = false;
bool g_kick = false;
std::string g_recover_code;
bool g_cancel = false;
// What the last cycle talked to, for recover/cancel outside a cycle.
std::string g_master_host;
uint16_t g_master_port = 0;
uint8_t g_master_key[32] = {};

void Publish(const Status& s) {
    std::lock_guard<std::mutex> lk(g_mtx);
    const uint64_t gen = g_status.generation + 1;
    g_status = s;
    g_status.generation = gen;
}

bool OnlineOn() { return larecomp::cvar::GetFlagByName("mcla_online_enable") == "true"; }

// Where the pass finds and keeps what the master hands out: the game's one identity (identity_store.h).
class GameTicketHooks final : public net::TicketHooks {
public:
    bool Cached(const uint8_t (&master_key)[32], Ticket& out) override { return CachedTicket(master_key, out); }
    void Put(const uint8_t (&master_key)[32], const Ticket& ticket) override { StoreTicket(master_key, ticket); }
    void SetRecoveryCode(const std::string& code) override { SetPendingRecoveryCode(code); }
    void Note(const std::string& line) override { LARECOMP_APP_INFO("[identity] {}", line); }
};

// One pass: challenge, ticket, proof (identity_net.cpp). Returns how long until the next pass.
std::chrono::seconds Cycle(Status& s) {
    std::string host;
    uint16_t port = 0;
    relay_info::ConfiguredServer(host, port);
    const KeyPair keys = Keys();
    net::CycleInput in;
    in.host = host;
    in.port = port;
    in.nick = Nick();
    in.keys = &keys;
    in.claim = net::ClaimMode::kAlways;
    GameTicketHooks hooks;
    net::CycleOutput out;
    const std::chrono::seconds next = net::Cycle(s, in, hooks, &out);
    if (out.master_known) {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_master_host = out.master_host;
        g_master_port = out.master_port;
        std::memcpy(g_master_key, out.master_key, 32);
    }
    return next;
}

void HandleRequests(Status& s) {
    std::string code;
    bool cancel;
    std::string mhost;
    uint16_t mport;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        code.swap(g_recover_code);
        cancel = g_cancel;
        g_cancel = false;
        mhost = g_master_host;
        mport = g_master_port;
    }
    if (mhost.empty() || (code.empty() && !cancel)) return;
    if (!code.empty()) {
        const Fields f = net::MasterCall(mhost, mport, "recover", Nick(), Keys(), code, net::Options{});
        const std::string st = f.count("status") ? f.at("status") : "";
        if (st == "pending") {
            s.check = Check::kClaimPending;
            s.claim_until = std::atoll(f.at("until").c_str());
            s.detail = "recovery code accepted";
        } else if (st == "bad_code") {
            s.detail = "that recovery code is wrong";
        } else if (st == "rate_limited") {
            s.detail = "too many tries; wait an hour";
        } else if (st == "already_yours") {
            s.detail = "this nick is already yours";
        } else {
            s.detail = "recovery failed: " + (st.empty() ? std::string("no answer") : st);
        }
        LARECOMP_APP_INFO("[identity] recover \"{}\": {}", Nick(), st);
    }
    if (cancel) {
        const Fields f = net::MasterCall(mhost, mport, "cancel", Nick(), Keys(), "", net::Options{});
        const std::string st = f.count("status") ? f.at("status") : "";
        if (st == "ok") {
            s.incoming_until = 0;
            if (f.count("recovery")) SetPendingRecoveryCode(f.at("recovery"));
            s.detail = "transfer cancelled; you have a new recovery code";
        } else {
            s.detail = "cancel failed: " + (st.empty() ? std::string("no answer") : st);
        }
        LARECOMP_APP_INFO("[identity] cancel transfer of \"{}\": {}", Nick(), st);
    }
}

void Worker() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);  // counted: harmless next to the SDK's own
#endif
    std::string last_server;
    bool last_online = false;
    auto next = Clock::now();
    // Whether the last pass was online with the identity loaded. A recover or
    // cancel request only wakes the loop then: offline it stays queued until
    // the game goes online, instead of waking the loop on every pass.
    bool serving = false;
    Status s;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cv.wait_until(lk, (std::min)(next, Clock::now() + std::chrono::seconds(2)),
                            [serving] {
                                return g_kick ||
                                       (serving && (!g_recover_code.empty() || g_cancel));
                            });
        }
        const bool online = OnlineOn();
        bool kick;
        bool requests;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            kick = g_kick;
            g_kick = false;
            requests = !g_recover_code.empty() || g_cancel;
        }
        if (!online || !Loaded()) {
            if (last_online || s.check != Check::kOff) {
                s = Status{};
                Publish(s);
            }
            last_online = false;
            serving = false;
            // Offline the next look is 2 s away; `next` left in the past made
            // the wait return at once and the loop spin a core for as long as
            // the game stayed offline.
            next = Clock::now() + std::chrono::seconds(2);
            continue;
        }
        serving = true;
        const std::string server = larecomp::cvar::GetFlagByName("mcla_online_server") + "|" +
                                   larecomp::cvar::GetFlagByName("mcla_online_port");
        if (requests) {
            HandleRequests(s);
            Publish(s);
            kick = true;
        }
        if (!kick && online == last_online && server == last_server && Clock::now() < next) continue;
        last_online = online;
        last_server = server;
        Status fresh;
        fresh.incoming_until = s.incoming_until;
        fresh.check = Check::kWorking;
        fresh.detail = "checking the nick with the server...";
        fresh.relay = s.relay;
        Publish(fresh);
        next = Clock::now() + Cycle(fresh);
        const bool changed = fresh.check != s.check || fresh.detail != s.detail;
        s = fresh;
        Publish(s);
        if (changed)
            LARECOMP_APP_INFO("[identity] {} ({})", s.detail, s.relay);
    }
}

}  // namespace

void StartClient() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_started) return;
    g_started = true;
    std::thread(Worker).detach();
}

Status GetStatus() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_status;
}

void CheckNow() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_kick = true;
    g_cv.notify_all();
}

void RequestRecover(const std::string& code) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_recover_code = code;
    g_cv.notify_all();
}

void RequestCancelTransfer() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_cancel = true;
    g_cv.notify_all();
}

}  // namespace larecomp::identity
