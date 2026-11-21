// An HTTP or HTTPS GET on Linux, through the system's libcurl. See curl_get.h.

#include "os/curl_get.h"

#if defined(__linux__)

#include <dlfcn.h>

#include <mutex>

namespace larecomp::os {

namespace {

// libcurl's ABI (curl.h): the option and info numbers are part of it and never change.
constexpr int kCurlOptWriteData = 10001;        // CURLOPT_WRITEDATA
constexpr int kCurlOptUrl = 10002;              // CURLOPT_URL
constexpr int kCurlOptUserAgent = 10018;        // CURLOPT_USERAGENT
constexpr int kCurlOptFailOnError = 45;         // CURLOPT_FAILONERROR
constexpr int kCurlOptNoSignal = 99;            // CURLOPT_NOSIGNAL
constexpr int kCurlOptWriteFunction = 20011;    // CURLOPT_WRITEFUNCTION
constexpr int kCurlOptTimeoutMs = 155;          // CURLOPT_TIMEOUT_MS
constexpr int kCurlOptConnectTimeoutMs = 156;   // CURLOPT_CONNECTTIMEOUT_MS
constexpr int kCurlInfoResponseCode = 0x200002; // CURLINFO_RESPONSE_CODE
constexpr long kCurlGlobalDefault = 3;          // CURL_GLOBAL_DEFAULT (SSL and Win32)
constexpr int kCurlOk = 0;                      // CURLE_OK

struct Curl {
  void* handle = nullptr;
  void* (*easy_init)() = nullptr;
  int (*easy_setopt)(void*, int, ...) = nullptr;
  int (*easy_perform)(void*) = nullptr;
  int (*easy_getinfo)(void*, int, ...) = nullptr;
  void (*easy_cleanup)(void*) = nullptr;
  const char* (*easy_strerror)(int) = nullptr;
  std::string why;  // when the library could not be used
  bool usable() const { return handle != nullptr; }
};

template <typename Fn>
bool Bind(void* handle, const char* name, Fn* out) {
  *out = reinterpret_cast<Fn>(dlsym(handle, name));
  return *out != nullptr;
}

const Curl& Library() {
  static Curl curl;
  static std::once_flag once;
  std::call_once(once, [] {
    const char* const kNames[] = {"libcurl.so.4", "libcurl-gnutls.so.4"};
    for (const char* name : kNames) {
      curl.handle = dlopen(name, RTLD_NOW | RTLD_LOCAL);
      if (curl.handle) break;
    }
    if (!curl.handle) {
      curl.why = "libcurl.so.4 is not installed";
      return;
    }
    int (*global_init)(long) = nullptr;
    const bool bound = Bind(curl.handle, "curl_global_init", &global_init) && Bind(curl.handle, "curl_easy_init", &curl.easy_init) &&
                       Bind(curl.handle, "curl_easy_setopt", &curl.easy_setopt) && Bind(curl.handle, "curl_easy_perform", &curl.easy_perform) &&
                       Bind(curl.handle, "curl_easy_getinfo", &curl.easy_getinfo) && Bind(curl.handle, "curl_easy_cleanup", &curl.easy_cleanup) &&
                       Bind(curl.handle, "curl_easy_strerror", &curl.easy_strerror);
    if (!bound) {
      curl.why = "libcurl has not the functions of its easy interface";
      dlclose(curl.handle);
      curl.handle = nullptr;
      return;
    }
    global_init(kCurlGlobalDefault);
  });
  return curl;
}

struct Sink {
  std::string* body;
  size_t limit;
  bool over = false;
};

size_t Collect(char* data, size_t size, size_t count, void* user) {
  Sink* sink = static_cast<Sink*>(user);
  const size_t bytes = size * count;
  if (sink->body->size() + bytes > sink->limit) {
    sink->over = true;
    return 0;  // a short write: libcurl ends the transfer with CURLE_WRITE_ERROR
  }
  sink->body->append(data, bytes);
  return bytes;
}

}  // namespace

bool HttpGetAvailable() { return Library().usable(); }

HttpGetResult HttpGet(const std::string& url, std::chrono::milliseconds timeout, size_t max_body_bytes) {
  HttpGetResult result;
  const Curl& curl = Library();
  if (!curl.usable()) {
    result.why = curl.why;
    return result;
  }
  void* easy = curl.easy_init();
  if (!easy) {
    result.why = "libcurl could not start a request";
    return result;
  }
  Sink sink{&result.body, max_body_bytes};
  const long total_ms = static_cast<long>(timeout.count() > 0 ? timeout.count() : 1);
  curl.easy_setopt(easy, kCurlOptUrl, url.c_str());
  curl.easy_setopt(easy, kCurlOptUserAgent, "larecomp/1.0");
  curl.easy_setopt(easy, kCurlOptWriteFunction, &Collect);
  curl.easy_setopt(easy, kCurlOptWriteData, &sink);
  curl.easy_setopt(easy, kCurlOptTimeoutMs, total_ms);
  curl.easy_setopt(easy, kCurlOptConnectTimeoutMs, total_ms);
  curl.easy_setopt(easy, kCurlOptNoSignal, 1L);       // no SIGALRM for name lookups: this runs on a thread of a program with its own signal handlers
  curl.easy_setopt(easy, kCurlOptFailOnError, 1L);    // an HTTP status of 400 or more is a failure, not a body
  // No redirects are followed (CURLOPT_FOLLOWLOCATION stays off): the URL the caller names is the one that answers.
  const int code = curl.easy_perform(easy);
  long status = 0;
  curl.easy_getinfo(easy, kCurlInfoResponseCode, &status);
  curl.easy_cleanup(easy);
  result.status = status;
  if (code == kCurlOk) {
    // The transfer worked; only a 2xx is an answer (a 3xx is not followed, FAILONERROR has already failed the 4xx and 5xx).
    if (status >= 200 && status < 300) {
      result.ok = true;
      return result;
    }
    result.body.clear();
    result.why = "HTTP " + std::to_string(status) + " (redirects are not followed)";
    return result;
  }
  result.body.clear();
  if (sink.over) {
    result.why = "the answer is longer than " + std::to_string(max_body_bytes) + " bytes";
  } else {
    const char* text = curl.easy_strerror(code);
    result.why = std::string("libcurl: ") + (text ? text : "an error") + " (" + std::to_string(code) + ")" +
                 (status ? ", HTTP " + std::to_string(status) : std::string());
  }
  return result;
}

}  // namespace larecomp::os

#else  // not Linux

namespace larecomp::os {

bool HttpGetAvailable() { return false; }

HttpGetResult HttpGet(const std::string&, std::chrono::milliseconds, size_t) {
  HttpGetResult result;
  result.why = "no libcurl client on this platform";
  return result;
}

}  // namespace larecomp::os

#endif
