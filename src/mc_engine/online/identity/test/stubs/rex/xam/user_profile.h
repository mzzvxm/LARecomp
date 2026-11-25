#pragma once
// A stand-in for the SDK's UserProfile with the four calls identity_store.cpp makes (for identity_adapter_test.cpp, which
// compiles the game's identity files without the SDK).
#include <cstdint>
#include <string>

namespace rex::xam {

class UserProfile {
public:
    UserProfile(std::string name, uint64_t xuid) : name_(std::move(name)), xuid_(xuid) {}
    uint64_t GetXUID() const { return xuid_; }
    std::string GetName() const { return name_; }
    void set_xuid(uint64_t xuid) { xuid_ = xuid; }

private:
    std::string name_;
    uint64_t xuid_;
};

}  // namespace rex::xam
