#pragma once
// ===========================================================================
// An HTTP or HTTPS GET on Linux, through the system's libcurl
// ===========================================================================
// Windows has WinHTTP, which the callers use themselves (the real-world weather's HttpsGet). Linux has no HTTPS client in this program and
// the build has no TLS library, so the one place that needs HTTPS (the weather lookup, which asks two services over HTTPS) had nothing to
// call: the setting WEATHER: REAL did nothing on Linux and the Steam Deck.
//
// libcurl is on every system the game runs on (SteamOS and its Game Mode container, Arch, Ubuntu, Fedora: Steam itself needs it), so it is
// found at run time, not linked: libcurl.so.4, else libcurl-gnutls.so.4. A PC without it only lacks the features that need the network;
// the program still starts. The few option numbers libcurl's ABI fixes (curl.h) are in curl_get.cpp.
//
// HttpGet is for one small answer (a JSON line): no redirects are followed, the body is capped, the whole request has a time limit. Safe from
// any thread; the first call loads the library (and calls curl_global_init once). Off Linux it returns !ok with a reason.
// ===========================================================================

#include <chrono>
#include <cstddef>
#include <string>

namespace larecomp::os {

struct HttpGetResult {
  bool ok = false;  // the request ran and the server answered with a 2xx status
  long status = 0;  // the HTTP status when the server answered (0: no answer)
  std::string body;
  std::string why;  // what went wrong, when !ok (one line, for a log)
};

// Whether libcurl can be loaded here (loads it on the first call).
bool HttpGetAvailable();

HttpGetResult HttpGet(const std::string& url, std::chrono::milliseconds timeout, size_t max_body_bytes = 1u << 20);

}  // namespace larecomp::os
