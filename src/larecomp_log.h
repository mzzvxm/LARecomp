#pragma once

#include <rex/logging/log_category.h>
#include <rex/logging/logger.h>

// Log categories on rexglue 0.11: a LogCategory is a constinit object (no RegisterLogCategory), added to the logger once
// with AddCategories (AddLarecompLogCategories in spdlog_console.cpp adds App, Crash and mc::log::mc, from InitLarecompLogging, the
// app constructor's first step: a category written to before it is added, or never added, opens with a "not registered" warning), which
// also gives each a log.categories.<name> setting.
// CHECK: category names with a dot (the 0.10 names were "larecomp.app" and "larecomp.crash"): the plan says "a lowercase
// identifier", so the underscore form is used; log.categories.larecomp_app is the setting a player would write.
namespace larecomp::log {
inline constinit rex::log::LogCategory App{"larecomp_app", "LARecomp application"};
inline constinit rex::log::LogCategory Crash{"larecomp_crash", "LARecomp crash handler"};
}  // namespace larecomp::log

#define LARECOMP_APP_DEBUG(...)   REXLOG_CAT_DEBUG(::larecomp::log::App, __VA_ARGS__)
#define LARECOMP_APP_INFO(...)    REXLOG_CAT_INFO(::larecomp::log::App, __VA_ARGS__)
#define LARECOMP_APP_WARN(...)    REXLOG_CAT_WARN(::larecomp::log::App, __VA_ARGS__)
#define LARECOMP_APP_ERROR(...)   REXLOG_CAT_ERROR(::larecomp::log::App, __VA_ARGS__)

#define LARECOMP_CRASH_INFO(...)  REXLOG_CAT_INFO(::larecomp::log::Crash, __VA_ARGS__)
#define LARECOMP_CRASH_WARN(...)  REXLOG_CAT_WARN(::larecomp::log::Crash, __VA_ARGS__)
#define LARECOMP_CRASH_ERROR(...) REXLOG_CAT_ERROR(::larecomp::log::Crash, __VA_ARGS__)
