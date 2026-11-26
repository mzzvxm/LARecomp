#pragma once

#include <algorithm>
#include <array>
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

// Keep the ranked snapshot order for restoration. Only the optional index is sorted.
template <size_t Capacity>
class MirrorCarLookup {
public:
    template <typename CarState>
    void Build(const std::array<CarState, Capacity>& cars, size_t count, bool indexed) {
        indexed_ = indexed;
        if (indexed_) {
            std::transform(cars.begin(), cars.begin() + count, index_.begin(), [](const CarState& car) { return car.item; });
            std::sort(index_.begin(), index_.begin() + count);
        }
    }

    template <typename CarState>
    bool Contains(const std::array<CarState, Capacity>& cars, size_t count, uint32_t item) const {
        if (indexed_) {
            return std::binary_search(index_.begin(), index_.begin() + count, item);
        }
        return std::any_of(cars.begin(), cars.begin() + count, [item](const CarState& car) { return car.item == item; });
    }

private:
    std::array<uint32_t, Capacity> index_{};
    bool indexed_ = false;
};

}  // namespace mc::guest_work
