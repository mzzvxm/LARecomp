#pragma once
// ===========================================================================
// The shader pack code's log lines
// ===========================================================================
// The pack builder (src/shaderpack) has no dependency on the ReXGlue runtime, so
// that the same objects build into larecomp.exe and into the command line tool
// (tools/shaderpack_tool). Its log lines go through one callback instead:
//
//   * BuildShaderPacks (builder.h) points it at the request's ProgressSink for
//     the length of the call, so the readers (rpf_shaders, image_scan) log into
//     the same place as the builder.
//   * Outside a build the default prints to stderr. The app can install its own
//     callback once (SetLogCallback) to send the lines to its logger.
//
// SHADERPACK_LOG_* take a std::format string. Called from the thread that runs the
// build, not from its workers.
// ===========================================================================

#include <format>
#include <functional>
#include <string>

namespace larecomp::shaderpack {

enum class LogLevel { kInfo, kWarn, kError };

using LogCallback = std::function<void(LogLevel level, const std::string& text)>;

// Replaces the callback (an empty one restores the stderr default). Returns the one that was
// installed (empty when it was the default), so a caller can put it back.
LogCallback SetLogCallback(LogCallback callback);

void LogText(LogLevel level, const std::string& text);

}  // namespace larecomp::shaderpack

#define SHADERPACK_LOG_INFO(...) \
  ::larecomp::shaderpack::LogText(::larecomp::shaderpack::LogLevel::kInfo, std::format(__VA_ARGS__))
#define SHADERPACK_LOG_WARN(...) \
  ::larecomp::shaderpack::LogText(::larecomp::shaderpack::LogLevel::kWarn, std::format(__VA_ARGS__))
#define SHADERPACK_LOG_ERROR(...) \
  ::larecomp::shaderpack::LogText(::larecomp::shaderpack::LogLevel::kError, std::format(__VA_ARGS__))
