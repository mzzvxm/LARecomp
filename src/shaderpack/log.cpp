#include "shaderpack/log.h"

#include <cstdio>
#include <mutex>
#include <utility>

namespace larecomp::shaderpack {

namespace {

std::mutex& CallbackMutex() {
  static std::mutex mutex;
  return mutex;
}

LogCallback& Callback() {
  static LogCallback callback;
  return callback;
}

}  // namespace

LogCallback SetLogCallback(LogCallback callback) {
  std::lock_guard<std::mutex> lock(CallbackMutex());
  std::swap(callback, Callback());
  return callback;  // the previous one
}

void LogText(LogLevel level, const std::string& text) {
  LogCallback callback;
  {
    std::lock_guard<std::mutex> lock(CallbackMutex());
    callback = Callback();
  }
  if (callback) {
    callback(level, text);
    return;
  }
  const char* prefix = level == LogLevel::kError ? "error: " : level == LogLevel::kWarn ? "warning: " : "";
  std::fprintf(stderr, "[shaderpack] %s%s\n", prefix, text.c_str());
}

}  // namespace larecomp::shaderpack
