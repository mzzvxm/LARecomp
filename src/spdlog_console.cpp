#ifndef REXGLUE_HAS_XEO3_TARGET
// larecomp - ReXGlue Recompiled Project
// This is just an spdlog thing
#include "spdlog_console.h"

#include <rex/logging.h>
#include <spdlog/spdlog.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

void InitLarecompLogging() {
    static bool initialized = false;
    if (initialized) {
        return;
    }

    initialized = true;

    // Ensure logs directory exists
    std::filesystem::create_directories("logs");

    static std::string log_filename = "logs/larecomp_001.log";
    if (const char* custom_log = std::getenv("LARECOMP_LOG_FILE"); custom_log && *custom_log) {
        log_filename = custom_log;
    } else {
        for (int i = 1; i <= 999; ++i) {
            char name[64];
            std::snprintf(name, sizeof(name), "logs/larecomp_%03d.log", i);
            if (!std::filesystem::exists(name)) {
                log_filename = name;
                break;
            }
        }
    }

    spdlog::level::level_enum default_level = spdlog::level::debug;
    if (const char* env_level = std::getenv("REX_LOG_LEVEL")) {
        std::string lvl = env_level;
        for (char& c : lvl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lvl == "trace") default_level = spdlog::level::trace;
        else if (lvl == "debug") default_level = spdlog::level::debug;
        else if (lvl == "info") default_level = spdlog::level::info;
        else if (lvl == "warn" || lvl == "warning") default_level = spdlog::level::warn;
        else if (lvl == "err" || lvl == "error") default_level = spdlog::level::err;
        else if (lvl == "critical" || lvl == "off") default_level = spdlog::level::off;
    }

    rex::LogConfig config;
    config.default_level = default_level;
    config.log_to_console = true;
    config.log_file = log_filename.c_str();

    config.console_pattern = "[%^%l%$] [%n] [t%t] %v";
    config.file_pattern    = "[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] [t%t] %v";

    config.flush_level = spdlog::level::warn;

    config.category_levels["core"]           = default_level;
    config.category_levels["cpu"]            = spdlog::level::info;
    config.category_levels["gpu"]            = spdlog::level::info;
    config.category_levels["krnl"]           = default_level;
    config.category_levels["mc"]             = default_level;
    config.category_levels["larecomp.app"]   = default_level;
    config.category_levels["larecomp.crash"] = spdlog::level::trace;

    rex::InitLogging(config);
    rex::RegisterLogLevelCallback();

    spdlog::flush_on(spdlog::level::warn);
    spdlog::flush_every(std::chrono::seconds(1));
}

void ShutdownLarecompLogging() {
    spdlog::shutdown();
}

#endif // REXGLUE_HAS_XEO3_TARGET
