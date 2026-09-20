#pragma once

#include <atomic>
#include <cstdint>

namespace mcla::draw_stats {

inline std::atomic<uint64_t> g_draw_calls{0};
inline std::atomic<uint64_t> g_vertices{0};

inline void RecordDraw(uint32_t vertex_count) {
    g_draw_calls.fetch_add(1, std::memory_order_relaxed);
    g_vertices.fetch_add(vertex_count, std::memory_order_relaxed);
}

}  // namespace mcla::draw_stats
