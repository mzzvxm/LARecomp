#include "browser_client.h"

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "settings/larecomp_settings.h"

#include "../relay_info.h"

// The network side (who is asked, how, what the answers mean) is browser_query.cpp, shared with
// larecomp_online_tool; this file is the game's: it reads the three addresses from the settings and keeps the
// snapshot the window reads.

namespace larecomp::browser {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kAutoRefreshEvery = std::chrono::seconds(8);

}  // namespace

struct Client::State {
    mutable std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;
    bool refresh_asked = true;
    bool auto_refresh = false;
    Snapshot snapshot;
    std::map<std::string, uint32_t> icon_crcs;  // address -> crc already fetched
    std::vector<IconUpdate> icons;
};

namespace {

void Worker(std::shared_ptr<Client::State> st) {
    Clock::time_point last{};
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(st->mutex);
            st->wake.wait_for(lk, std::chrono::milliseconds(500), [&] {
                return st->stop || st->refresh_asked ||
                       (st->auto_refresh && Clock::now() - last >= kAutoRefreshEvery);
            });
            if (st->stop) return;
            if (!st->refresh_asked && !(st->auto_refresh && Clock::now() - last >= kAutoRefreshEvery))
                continue;
            st->refresh_asked = false;
            st->snapshot.refreshing = true;
            ++st->snapshot.generation;
        }
        last = Clock::now();

        // Who to ask: the master's list, the player's own, the current relay.
        Sources sources;
        sources.master = larecomp::cvar::GetFlagByName("online_master_server");
        sources.favorites = larecomp::cvar::GetFlagByName("server_browser_servers");
        {
            std::string host;
            uint16_t port = 0;
            relay_info::ConfiguredServer(host, port);
            if (!larecomp::cvar::GetFlagByName("mcla_online_server").empty())
                sources.current = host + ":" + std::to_string(port);
        }

        std::map<std::string, uint32_t> known;
        {
            std::lock_guard<std::mutex> lk(st->mutex);
            known = st->icon_crcs;
        }
        Collected got = Collect(sources, Options{}, known);
        std::vector<ServerView>& results = got.servers;
        std::vector<std::vector<uint8_t>>& icons = got.icons;
        std::string master_status = got.master.text;
        if (sources.master.empty() && !got.master.via_favorites)
            master_status = "no server list set (online_master_server)";

        std::lock_guard<std::mutex> lk(st->mutex);
        for (size_t k = 0; k < results.size(); ++k) {
            if (icons[k].empty()) continue;
            st->icon_crcs[results[k].address] = results[k].icon_crc;
            st->icons.push_back({results[k].address, results[k].icon_crc, std::move(icons[k])});
        }
        st->snapshot.servers = std::move(results);
        st->snapshot.master_status = master_status;
        st->snapshot.refreshing = false;
        ++st->snapshot.generation;
    }
}

}  // namespace

Client::Client() : state_(std::make_shared<State>()) { std::thread(Worker, state_).detach(); }

Client::~Client() {
    std::lock_guard<std::mutex> lk(state_->mutex);
    state_->stop = true;
    state_->wake.notify_all();
}

void Client::Refresh() {
    std::lock_guard<std::mutex> lk(state_->mutex);
    state_->refresh_asked = true;
    state_->wake.notify_all();
}

void Client::SetAutoRefresh(bool on) {
    std::lock_guard<std::mutex> lk(state_->mutex);
    if (on && !state_->auto_refresh) state_->refresh_asked = true;
    state_->auto_refresh = on;
    state_->wake.notify_all();
}

Snapshot Client::Take() const {
    std::lock_guard<std::mutex> lk(state_->mutex);
    return state_->snapshot;
}

std::vector<IconUpdate> Client::TakeIcons() {
    std::vector<IconUpdate> out;
    std::lock_guard<std::mutex> lk(state_->mutex);
    out.swap(state_->icons);
    return out;
}

}  // namespace larecomp::browser
