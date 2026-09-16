#include "mc_engine/boot_progress.h"

#include <mutex>

namespace mc::boot {
namespace {

std::mutex g_lock;
std::vector<Phase> g_phases;

// Enough to fill the popup without it scrolling away from what is happening.
constexpr size_t kRecentKept = 6;
std::vector<std::string> g_recent;

}  // namespace

void BeginPhase(std::string name, int total) {
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_phases.empty()) g_phases.back().finished = true;
    g_phases.push_back(Phase{std::move(name), std::string(), 0, total, false});
}

void SetTotal(int total) {
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_phases.empty()) g_phases.back().total = total;
}

void Step(std::string item) {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_phases.empty()) return;
    ++g_phases.back().done;
    if (!item.empty()) {
        g_recent.push_back(item);
        if (g_recent.size() > kRecentKept) g_recent.erase(g_recent.begin());
    }
    g_phases.back().item = std::move(item);
}

void EndPhase() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_phases.empty()) return;
    g_phases.back().finished = true;
    g_phases.back().item.clear();
}

std::vector<Phase> Read() {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_phases;
}

std::vector<std::string> Recent() {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_recent;
}

bool Any() {
    std::lock_guard<std::mutex> guard(g_lock);
    return !g_phases.empty();
}

void Reset() {
    std::lock_guard<std::mutex> guard(g_lock);
    g_phases.clear();
    g_recent.clear();
}

}  // namespace mc::boot
