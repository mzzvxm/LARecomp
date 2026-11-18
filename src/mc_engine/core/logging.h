#pragma once
#include <rex/logging.h>

namespace mc::log {
inline const rex::LogCategoryId mc = rex::RegisterLogCategory("mc");
}

#define MC_TRACE(...)    REXLOG_CAT_TRACE(::mc::log::mc, __VA_ARGS__)
#define MC_DEBUG(...)    REXLOG_CAT_DEBUG(::mc::log::mc, __VA_ARGS__)
#define MC_INFO(...)     REXLOG_CAT_INFO(::mc::log::mc, __VA_ARGS__)
#define MC_WARN(...)     REXLOG_CAT_WARN(::mc::log::mc, __VA_ARGS__)
#define MC_ERROR(...)    REXLOG_CAT_ERROR(::mc::log::mc, __VA_ARGS__)
#define MC_CRITICAL(...) REXLOG_CAT_CRITICAL(::mc::log::mc, __VA_ARGS__)
