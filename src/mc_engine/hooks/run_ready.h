#pragma once

#include <atomic>
#include <cstdint>

namespace mc::run_ready {

enum class Event : uint8_t { Title, Save, Roam, Race };

constexpr const char* Name(Event event) {
    switch (event) {
        case Event::Title: return "title_ready";
        case Event::Save: return "save_loaded";
        case Event::Roam: return "free_roam";
        case Event::Race: return "race_started";
    }
    return "";
}

class Marks {
public:
    bool Pending(Event event) const {
        return (seen_.load(std::memory_order_relaxed) & Bit(event)) == 0;
    }

    bool Claim(Event event) {
        const auto bit = Bit(event);
        return (seen_.fetch_or(bit, std::memory_order_relaxed) & bit) == 0;
    }

private:
    static constexpr unsigned Bit(Event event) { return 1u << static_cast<unsigned>(event); }
    std::atomic<unsigned> seen_{0};
};

class TitleGate {
public:
    void Draw() { state_.fetch_or(kDrawn, std::memory_order_release); }
    void Enter() { state_.fetch_or(kEntered, std::memory_order_release); }
    bool Entered() const { return (state_.load(std::memory_order_acquire) & kEntered) != 0; }
    bool ReadyOnPoll() const { return state_.load(std::memory_order_acquire) == kDrawn; }

private:
    static constexpr unsigned kDrawn = 1;
    static constexpr unsigned kEntered = 2;
    std::atomic<unsigned> state_{0};
};

// race.tour's existing arcade countdown boundary. Require a sample of this race before it.
class RaceClock {
public:
    bool Tick(uint32_t race, float timer) {
        if (race != race_) {
            race_ = race;
            countdown_seen_ = false;
        }
        if (!race || timer < 0.0f) return false;
        if (timer < 9.3f) countdown_seen_ = true;
        return countdown_seen_ && timer >= 9.3f;
    }

private:
    uint32_t race_ = 0;
    bool countdown_seen_ = false;
};

bool Pending(Event event);
void Note(Event event);
void NoteTitleDraw();
void NotePadPoll();
void NoteEntered();
bool Entered();

}  // namespace mc::run_ready
