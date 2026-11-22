#pragma once
// A stand-in for the SDK's runtime: REX_RUNTIME() gives the profile the check made (identity_adapter_test.cpp).
#include <rex/xam/user_profile.h>

namespace rex::kernel {

class TitleRuntime {
public:
    rex::xam::UserProfile& GetUserProfile() { return *profile; }
    rex::xam::UserProfile* profile = nullptr;
};

inline TitleRuntime g_runtime;

}  // namespace rex::kernel

#define REX_RUNTIME() (&::rex::kernel::g_runtime)
