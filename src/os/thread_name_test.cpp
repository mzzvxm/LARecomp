// Unit test of the Linux thread name rule (os/thread_name.h).
//   clang++ -std=c++20 -I src src/os/thread_name_test.cpp -o thread_name_test && ./thread_name_test
// Exit code 0 = passed.
#include "os/thread_name.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& what) {
  if (!ok) {
    ++g_failures;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

}  // namespace

int main() {
  using larecomp::os::kLinuxThreadNameMax;
  using larecomp::os::ThreadNameFor;

  Check(kLinuxThreadNameMax == 15, "pthread_setname_np takes 15 bytes and the NUL");

  // Where the OS has no limit (Windows) every name is kept, and the profilers' scripts keep finding it.
  Check(ThreadNameFor("larecomp gpu render", false) == "larecomp gpu render", "Windows keeps the render thread's name");
  Check(ThreadNameFor("larecomp texture decode", false) == "larecomp texture decode", "Windows keeps the decode pool's name");

  // Linux: the two names of ours were both refused (19 and 23 bytes); each loses its program prefix and then fits.
  const std::string gpu = ThreadNameFor("larecomp gpu render", true);
  const std::string decode = ThreadNameFor("larecomp texture decode", true);
  Check(gpu == "gpu render", "the render thread is 'gpu render' on Linux, got '" + gpu + "'");
  Check(decode == "texture decode", "the decode pool is 'texture decode' on Linux, got '" + decode + "'");
  Check(gpu.size() <= kLinuxThreadNameMax && decode.size() <= kLinuxThreadNameMax, "both fit pthread_setname_np");

  // A name that already fits is not touched, prefix or not.
  Check(ThreadNameFor("larecomp", true) == "larecomp", "a short name is kept");
  Check(ThreadNameFor("larecomp wheel", true) == "larecomp wheel", "14 bytes fit, the prefix stays");
  Check(ThreadNameFor("123456789012345", true) == "123456789012345", "exactly 15 bytes fit");

  // One byte too many with no prefix is cut to 15.
  Check(ThreadNameFor("1234567890123456", true) == "123456789012345", "16 bytes are cut to 15");
  // After the prefix goes it may still be too long: cut.
  Check(ThreadNameFor("larecomp a-very-long-worker-name", true) == "a-very-long-wor", "prefix dropped, then cut to 15");
  // The prefix alone is not a name: kept as it is, then cut.
  Check(ThreadNameFor("larecomp ", true) == "larecomp ", "just the prefix is kept");
  Check(ThreadNameFor("", true).empty(), "an empty name stays empty");

  // A cut never ends inside a UTF-8 sequence: 'e' with acute accent (C3 A9) straddling the limit goes whole.
  const std::string accent = "12345678901234\xC3\xA9tail";  // the accent starts at byte 14: bytes 14 and 15
  const std::string cut = ThreadNameFor(accent, true);
  Check(cut == "12345678901234", "a UTF-8 character that straddles the limit is dropped whole, got " + std::to_string(cut.size()) + " bytes");
  // ... and one that ends exactly at the limit stays.
  const std::string fits = "1234567890123\xC3\xA9tail";  // the accent is bytes 13 and 14: complete inside 15
  Check(ThreadNameFor(fits, true) == "1234567890123\xC3\xA9", "a UTF-8 character that ends at the limit stays");

  // The compiled-in rule agrees with the OS it is built for.
#if defined(__linux__)
  Check(larecomp::os::kThreadNameLimited, "this is Linux: names are limited");
  Check(ThreadNameFor("larecomp gpu render") == "gpu render", "the one-argument form applies the Linux rule on Linux");
#else
  Check(!larecomp::os::kThreadNameLimited, "this is not Linux: names are not limited");
  Check(ThreadNameFor("larecomp gpu render") == "larecomp gpu render", "the one-argument form keeps the name off Linux");
#endif

  if (g_failures == 0) std::printf("thread_name_test: all checks passed\n");
  return g_failures == 0 ? 0 : 1;
}
