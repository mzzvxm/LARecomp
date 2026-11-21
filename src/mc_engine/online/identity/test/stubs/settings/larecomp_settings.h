#pragma once
// A stand-in for the game's settings by name (identity_client.cpp and relay_info_config.cpp read three of them).
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace larecomp::cvar {

inline std::mutex& FlagMutex() {
    static std::mutex m;
    return m;
}
inline std::map<std::string, std::string>& Flags() {
    static std::map<std::string, std::string> f;
    return f;
}
inline std::string GetFlagByName(std::string_view name) {
    std::lock_guard<std::mutex> lk(FlagMutex());
    const auto it = Flags().find(std::string(name));
    return it == Flags().end() ? std::string() : it->second;
}
inline bool SetFlagByName(std::string_view name, std::string_view value) {
    std::lock_guard<std::mutex> lk(FlagMutex());
    Flags()[std::string(name)] = std::string(value);
    return true;
}

}  // namespace larecomp::cvar
