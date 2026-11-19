#pragma once
#include <array>

#include <rex/logging/log_category.h>
#include <rex/logging/logger.h>

// The "mc" category (rexglue 0.11: a constinit LogCategory, added to the logger with larecomp::log::Register's table).
namespace mc::log {
inline constinit rex::log::LogCategory mc{"mc", "Midnight Club game code and hooks"};
}

#define MC_TRACE(...)    REXLOG_CAT_TRACE(::mc::log::mc, __VA_ARGS__)
#define MC_DEBUG(...)    REXLOG_CAT_DEBUG(::mc::log::mc, __VA_ARGS__)
#define MC_INFO(...)     REXLOG_CAT_INFO(::mc::log::mc, __VA_ARGS__)
#define MC_WARN(...)     REXLOG_CAT_WARN(::mc::log::mc, __VA_ARGS__)
#define MC_ERROR(...)    REXLOG_CAT_ERROR(::mc::log::mc, __VA_ARGS__)
#define MC_CRITICAL(...) REXLOG_CAT_CRITICAL(::mc::log::mc, __VA_ARGS__)
