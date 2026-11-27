#pragma once

#include <cstdint>
#include <optional>

namespace mc::cinematic::director {

// Count guest presents only after observing readiness. A streaming interruption
// discards the accumulated history. Leave two presents to drain in-flight work.
struct SessionSettle {
    static constexpr uint64_t kDrain = 2;
    std::optional<uint64_t> start;
    uint64_t generation = 0;

    bool Observe(bool ready, uint64_t present, uint64_t frames, uint64_t current_generation = 0) {
        if (generation != current_generation) {
            start.reset();
            generation = current_generation;
        }
        if (!ready) {
            start.reset();
            return false;
        }
        if (!start || present < *start) start = present;
        return present - *start >= frames + kDrain;
    }
};

}  // namespace mc::cinematic::director
