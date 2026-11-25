#pragma once

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

namespace larecomp::os {

// Keep the result owned by the caller. On Windows the CRT copies it under its
// environment lock. On Linux, process environment updates happen at startup.
inline std::optional<std::string> ReadEnvironment(const char* name) {
#if defined(_WIN32)
  char* value = nullptr;
  const auto error = _dupenv_s(&value, nullptr, name);
  const std::unique_ptr<char, decltype(&std::free)> owned(value, &std::free);
  if (error || !owned) return std::nullopt;
  return std::string(owned.get());
#else
  if (const char* value = std::getenv(name)) return std::string(value);
  return std::nullopt;
#endif
}

}  // namespace larecomp::os
