#pragma once

#include <rex/logging.h>

namespace larecomp::log {
inline const rex::LogCategoryId App =
    rex::RegisterLogCategory("larecomp.app");

inline const rex::LogCategoryId Crash =
    rex::RegisterLogCategory("larecomp.crash");
}

#define LARECOMP_APP_DEBUG(...)   REXLOG_CAT_DEBUG(::larecomp::log::App, __VA_ARGS__)
#define LARECOMP_APP_INFO(...)    REXLOG_CAT_INFO(::larecomp::log::App, __VA_ARGS__)
#define LARECOMP_APP_WARN(...)    REXLOG_CAT_WARN(::larecomp::log::App, __VA_ARGS__)
#define LARECOMP_APP_ERROR(...)   REXLOG_CAT_ERROR(::larecomp::log::App, __VA_ARGS__)

#define LARECOMP_CRASH_INFO(...)  REXLOG_CAT_INFO(::larecomp::log::Crash, __VA_ARGS__)
#define LARECOMP_CRASH_WARN(...)  REXLOG_CAT_WARN(::larecomp::log::Crash, __VA_ARGS__)
#define LARECOMP_CRASH_ERROR(...) REXLOG_CAT_ERROR(::larecomp::log::Crash, __VA_ARGS__)
