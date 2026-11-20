#pragma once
//
// The error codes of the Director's wire format (DIRECTOR-DESIGN.md section 2.3; documentation/DIRECTOR-PROTOCOL.md). Frozen: the names are
// the wire's.
//

#include <cstdint>
#include <string_view>

namespace mc::cinematic::director {

enum class ErrorCode : uint8_t {
    kBadArgs,     // "bad_args": a missing, unknown or mistyped argument, bad JSON, a value out of range
    kUnknownCmd,  // "unknown_cmd"
    kNotReady,    // "not_ready": the thing exists but is not usable yet (a car that is still being made, a scene that is loading)
    kNoWorld,     // "no_world": no world to act on (a menu, a loading screen, a race)
    kStale,       // "stale": the scene's revision or the `expect` text of an edit is not what the caller saw
    kRefused,     // "refused": a path or a setting the Director does not allow, a bad token, a full client table
    kTimeout,     // "timeout": a deferred answer did not come in `timeout_s`
    kFailed,      // "failed": anything else (the message says what)
};

constexpr const char* ErrorCodeName(ErrorCode code) {
    switch (code) {
        case ErrorCode::kBadArgs: return "bad_args";
        case ErrorCode::kUnknownCmd: return "unknown_cmd";
        case ErrorCode::kNotReady: return "not_ready";
        case ErrorCode::kNoWorld: return "no_world";
        case ErrorCode::kStale: return "stale";
        case ErrorCode::kRefused: return "refused";
        case ErrorCode::kTimeout: return "timeout";
        case ErrorCode::kFailed: return "failed";
    }
    return "failed";
}

inline bool ErrorCodeFromName(std::string_view name, ErrorCode* out) {
    for (int i = 0; i <= static_cast<int>(ErrorCode::kFailed); ++i) {
        const ErrorCode c = static_cast<ErrorCode>(i);
        if (name == ErrorCodeName(c)) {
            *out = c;
            return true;
        }
    }
    return false;
}

}  // namespace mc::cinematic::director
