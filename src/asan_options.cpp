#include <sanitizer/common_interface_defs.h>
#include <windows.h>

// ASAN_OPTIONS can override these defaults, including the per-run log path.
extern "C" [[clang::no_sanitize("address")]] const char* __asan_default_options() {
    return "detect_leaks=0:halt_on_error=0:symbolize=1:log_path=asan";
}

namespace {

struct AsanLogPath {
    AsanLogPath() {
        // On Windows the runtime DLL initializes its log before it reads the
        // executable's default options. Give direct launches a file too.
        if (GetEnvironmentVariableA("ASAN_OPTIONS", nullptr, 0) == 0) {
            __sanitizer_set_report_path("asan");
        }
    }
};

const AsanLogPath kAsanLogPath;

}  // namespace
