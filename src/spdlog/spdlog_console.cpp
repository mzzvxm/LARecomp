#ifndef REXGLUE_HAS_XEO3_TARGET
// larecomp - ReXGlue Recompiled Project
// This is just an spdlog thing
#include "spdlog_console.h"

#include <rex/logging/log_category.h>
#include <spdlog/spdlog.h>

#include <chrono>

void InitLarecompLogging() {
    static bool initialized = false;
    if (initialized) {
        return;
    }

    initialized = true;

    rex::LogConfig config;
    config.default_level = spdlog::level::debug;
    config.log_to_console = true;
    config.log_file = "debug_la.txt";

    config.console_pattern = "[%^%l%$] [%n] [t%t] %v";
    config.file_pattern    = "[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] [t%t] %v";

    config.flush_level = spdlog::level::warn;

    config.category_levels["core"]           = spdlog::level::debug;
    config.category_levels["cpu"]            = spdlog::level::info;
    config.category_levels["gpu"]            = spdlog::level::info;
    config.category_levels["krnl"]           = spdlog::level::debug;

    rex::InitLogging(config);
    rex::RegisterLogLevelCallback();

    spdlog::flush_on(spdlog::level::warn);
    spdlog::flush_every(std::chrono::seconds(1));
}
#endif // REXGLUE_HAS_XEO3_TARGET
