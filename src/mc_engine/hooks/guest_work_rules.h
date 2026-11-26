#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>

namespace mc::guest_work {

// One canonical state per guest address. map node addresses stay stable while
// constructors add zones. Both A/B paths see the same originals.
template <typename State, size_t Zones>
class ZoneLookup {
public:
    State& At(uint32_t address) { return states_[address]; }

    State& At(uint32_t address, size_t index, bool indexed) {
        if (!indexed) return At(address);
        State*& state = slots_[index];
        if (!state) state = &At(address);
        return *state;
    }

    void Reset() {
        slots_.fill(nullptr);
        states_.clear();
    }

private:
    std::map<uint32_t, State> states_;
    std::array<State*, Zones> slots_{};
};

// The same 32 saved slots on either A/B path. The mask accelerates lookup and
// preserves the old first-free rule when the table has a hole.
class SavedSlots {
public:
    static constexpr size_t kCapacity = 32;

    template <typename KeyAt>
    size_t Find(uint32_t key, const KeyAt& key_at, bool indexed) const {
        if (indexed) {
            for (uint32_t bits = occupied_; bits; bits &= bits - 1) {
                const size_t index = std::countr_zero(bits);
                if (key_at(index) == key) return index;
            }
        } else {
            for (size_t index = 0; index < kCapacity; ++index) {
                if (key_at(index) == key) return index;
            }
        }
        return kCapacity;
    }

    template <typename KeyAt>
    size_t Place(uint32_t key, const KeyAt& key_at, bool indexed) const {
        if (indexed) return Find(key, key_at, true) == kCapacity ? std::countr_zero(~occupied_) : kCapacity;
        size_t free = kCapacity;
        for (size_t index = 0; index < kCapacity; ++index) {
            const uint32_t saved = key_at(index);
            if (saved == key) return kCapacity;
            if (!saved && free == kCapacity) free = index;
        }
        return free;
    }

    void Save(size_t index) { occupied_ |= uint32_t{1} << index; }
    void Restore(size_t index) { occupied_ &= ~(uint32_t{1} << index); }

private:
    uint32_t occupied_ = 0;
};

}  // namespace mc::guest_work
