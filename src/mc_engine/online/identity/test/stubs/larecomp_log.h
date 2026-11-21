#pragma once
// A stand-in for the game's log macros: the lines are kept so that identity_adapter_test.cpp can print them and the
// check of the split can compare what the code before and after it logs.
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace idstub {

inline std::mutex& LogMutex() {
    static std::mutex m;
    return m;
}
inline std::vector<std::string>& LogLines() {
    static std::vector<std::string> v;
    return v;
}
template <typename... A>
void Log(const char* level, std::format_string<A...> fmt, A&&... args) {
    std::string line = std::string(level) + " " + std::format(fmt, std::forward<A>(args)...);
    std::lock_guard<std::mutex> lk(LogMutex());
    LogLines().push_back(std::move(line));
}

}  // namespace idstub

#define LARECOMP_APP_DEBUG(...) ::idstub::Log("DEBUG", __VA_ARGS__)
#define LARECOMP_APP_INFO(...) ::idstub::Log("INFO", __VA_ARGS__)
#define LARECOMP_APP_WARN(...) ::idstub::Log("WARN", __VA_ARGS__)
#define LARECOMP_APP_ERROR(...) ::idstub::Log("ERROR", __VA_ARGS__)
