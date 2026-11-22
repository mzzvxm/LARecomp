#pragma once

#include <filesystem>

#include <rex/result.h>

// LARecomp's logging set-up. The file name is historical: up to rexglue 0.10 this configured spdlog (console and file
// sinks, category levels); on 0.11 the SDK's own logger (rex/logging/logger.h, REX_LOGGER()) writes the console, the
// debugger and the numbered log file, so this only sets LARecomp's defaults on the SDK's log.* settings and finds the
// file. The in-game console and its log capture are the SDK's (ui_system.cpp adds a rex::log::LogCaptureSink to the
// logger); nothing here adds a sink, and nothing links spdlog (a second copy would be a second registry that never sees
// the SDK's lines).

// Adds LARecomp's three log categories (larecomp_app, larecomp_crash and mc) to the process logger, which also gives each its
// log.categories.<name> setting. InitLarecompLogging does it before its first line; it is not safe to call twice (the SDK refuses a
// category it has added: the error comes back). A category that is written to without it opens with a "not registered" warning.
rex::Result<void> AddLarecompLogCategories();

// Call first thing in the app constructor, before the settings apply. Adds the categories (above), then sets, at the TITLE layer (a value from larecomp.toml,
// the environment or the command line wins): log.level = debug, the 0.10 line layout (log.pattern), cpu and gpu at info
// and the crash handler's category at trace. LARECOMP_LOG_FILE (a path, relative to the working folder) and the 0.10
// spellings of REX_LOG_LEVEL ("warning", "err") are translated to the SDK's settings. Safe to call twice.
void InitLarecompLogging();

// Flushes every sink (the SDK ends the logging session itself, after the app stops).
void ShutdownLarecompLogging();

// The file the logger writes, as an absolute path. Exact when log.file_name is an absolute path (LARECOMP_LOG_FILE, or a
// player's log_file line); otherwise the newest <app>_NNN.log in app.log_root: the SDK numbers a run one above the highest
// in the folder when it opens the file (App::Init, before OnInit), so from OnInit on that is this run's file. Earlier it is
// the previous run's file or empty: do not use it before OnInit. Empty when there is none.
std::filesystem::path LarecompLogFile();

// LarecompLogFile() as a C string, for the callers that predate it (valid until the same thread calls it again).
const char* LarecompLogFilePath();

// The folder the log files are in: the folder of LarecompLogFile(), else app.log_root, else <exe folder>/logs. A file kept
// next to the log (shader_build.txt, Streamline's sl.log) should ask here instead of assuming ./logs.
std::filesystem::path LarecompLogFolder();
