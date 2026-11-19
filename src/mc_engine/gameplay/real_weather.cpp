#ifndef REXGLUE_HAS_XEO3_TARGET
//
// Real-world weather, mapped onto MCLA's four skies.
//
// Two free, key-less services, one hour apart:
//   1. https://ipapi.co/json/                  IP -> latitude, longitude, city
//   2. https://api.open-meteo.com/v1/forecast  lat/lon -> WMO weather code
//
// The WMO code is folded onto the engine's weather indices (0 Nice, 1 Cloudy,
// 2 Stormy, 3 Foggy — name table at 0x827E1B00). MCLA has no snow sky, so snow
// codes land on Stormy, which is the closest the game can render.
//
// Everything happens on one background thread that sleeps between polls, so the
// render thread never blocks on the network. The thread is created the first
// time the feature is enabled and idles when it is switched off — with
// weather != "real" nothing is ever sent.
//
// Note the obvious: resolving the location by IP means the request itself
// carries the machine's public IP to those two services. That is the whole
// mechanism, not a side effect.
//

#include "real_weather.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "larecomp_log.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace {

constexpr int kPollMinutes = 60;
// Retry sooner than the next full poll when a fetch fails, so a transient
// network hiccup at boot does not cost an hour of stock weather.
constexpr int kRetryMinutes = 5;

std::atomic<int> g_index{-1};
std::atomic<bool> g_enabled{false};

std::mutex g_loc_mutex;
std::string g_location;

#if defined(_WIN32)

// Minimal HTTPS GET. Returns the body, or an empty string on any failure.
std::string HttpsGet(const wchar_t* host, const wchar_t* path) {
    std::string body;

    HINTERNET session = WinHttpOpen(L"larecomp/1.0",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return body;
    // Never let a stalled socket hold the thread for long.
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 10000);

    HINTERNET connect = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return body;
    }

    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path, nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return body;
    }

    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) break;
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, &chunk[0], avail, &read) || read == 0) break;
            chunk.resize(read);
            body += chunk;
            if (body.size() > 256 * 1024) break;  // these responses are tiny
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return body;
}

#else  // !_WIN32

std::string HttpsGet(const wchar_t*, const wchar_t*) { return {}; }

#endif

// Pulls "key": <number> out of a JSON body. Good enough for two flat responses
// with no nesting ambiguity, and avoids dragging in a JSON dependency.
bool JsonNumber(const std::string& body, const char* key, double* out) {
    std::string needle = std::string("\"") + key + "\"";
    size_t at = body.find(needle);
    if (at == std::string::npos) return false;
    at = body.find(':', at + needle.size());
    if (at == std::string::npos) return false;
    ++at;
    while (at < body.size() && (body[at] == ' ' || body[at] == '"')) ++at;
    char* end = nullptr;
    double v = std::strtod(body.c_str() + at, &end);
    if (end == body.c_str() + at) return false;
    *out = v;
    return true;
}

// Same, but only inside the object that follows `"<section>":`. Open-Meteo
// answers with both "current_units" and "current", and both carry a
// "weather_code" — the units one as the string "wmo code". Parsing the first
// match found anywhere picks that string and fails.
bool JsonNumberIn(const std::string& body, const char* section, const char* key,
                  double* out) {
    std::string needle = std::string("\"") + section + "\":";
    size_t at = body.find(needle);
    if (at == std::string::npos) return false;
    return JsonNumber(body.substr(at), key, out);
}

// Locale-independent fixed-point formatting. Never use printf's %f here: with a
// locale that writes decimal commas (pt-BR among them) the URL comes out as
// "latitude=-23,5500" and the service rejects it.
std::wstring FormatCoord(double v) {
    bool neg = v < 0.0;
    if (neg) v = -v;
    long long scaled = static_cast<long long>(v * 10000.0 + 0.5);
    wchar_t buf[32];
    std::swprintf(buf, 32, L"%ls%lld.%04lld", neg ? L"-" : L"", scaled / 10000,
                  scaled % 10000);
    return buf;
}

bool JsonString(const std::string& body, const char* key, std::string* out) {
    std::string needle = std::string("\"") + key + "\"";
    size_t at = body.find(needle);
    if (at == std::string::npos) return false;
    at = body.find(':', at + needle.size());
    if (at == std::string::npos) return false;
    at = body.find('"', at);
    if (at == std::string::npos) return false;
    size_t end = body.find('"', at + 1);
    if (end == std::string::npos) return false;
    *out = body.substr(at + 1, end - at - 1);
    return true;
}

// WMO weather interpretation codes -> MCLA weather index.
int WmoToWeatherIndex(int code) {
    switch (code) {
        case 0:            // clear sky
        case 1:            // mainly clear
            return 0;      // Nice
        case 45:           // fog
        case 48:           // depositing rime fog
            return 3;      // Foggy
        case 2:            // partly cloudy
        case 3:            // overcast
            return 1;      // Cloudy
        default:
            break;
    }
    // 51-67 drizzle/rain, 71-77 snow, 80-86 showers, 95-99 thunderstorm.
    if (code >= 51 && code <= 99) return 2;  // Stormy
    return 1;                                // anything unexpected: Cloudy
}

bool FetchOnce() {
    std::string geo = HttpsGet(L"ipapi.co", L"/json/");
    double lat = 0.0, lon = 0.0;
    if (geo.empty() || !JsonNumber(geo, "latitude", &lat) ||
        !JsonNumber(geo, "longitude", &lon)) {
        LARECOMP_APP_INFO("[RealWeather] location lookup failed (body: {})",
                          geo.empty() ? std::string("<empty>") : geo.substr(0, 200));
        return false;
    }

    std::string city;
    JsonString(geo, "city", &city);

    std::wstring path = L"/v1/forecast?latitude=" + FormatCoord(lat) +
                        L"&longitude=" + FormatCoord(lon) +
                        L"&current=weather_code";

    std::string wx = HttpsGet(L"api.open-meteo.com", path.c_str());
    double code = 0.0;
    if (wx.empty() || !JsonNumberIn(wx, "current", "weather_code", &code)) {
        LARECOMP_APP_INFO("[RealWeather] forecast fetch failed (body: {})",
                          wx.empty() ? std::string("<empty>") : wx.substr(0, 200));
        return false;
    }

    int idx = WmoToWeatherIndex(static_cast<int>(code));
    g_index.store(idx, std::memory_order_relaxed);

    static const char* kNames[] = {"NICE", "CLOUDY", "STORMY", "FOGGY"};
    {
        std::lock_guard<std::mutex> lock(g_loc_mutex);
        g_location = city.empty() ? "unknown" : city;
    }
    LARECOMP_APP_INFO("[RealWeather] {} ({:.2f}, {:.2f}): WMO {} -> {}",
                      city.empty() ? "unknown" : city, lat, lon,
                      static_cast<int>(code), kNames[idx]);
    return true;
}

// Owns the polling thread and shuts it down cleanly at process exit, so a fetch
// in flight cannot outlive the runtime it logs into.
class Poller {
public:
    void Start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (thread_.joinable()) return;
        thread_ = std::thread([this] { Run(); });
    }

    void Wake() { cv_.notify_all(); }

    ~Poller() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

private:
    void Run() {
        for (;;) {
            int wait_minutes = kPollMinutes;
            if (g_enabled.load(std::memory_order_relaxed)) {
                if (!FetchOnce()) wait_minutes = kRetryMinutes;
            }

            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::minutes(wait_minutes),
                         [this] { return stop_; });
            if (stop_) return;
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    bool stop_ = false;
};

Poller& GetPoller() {
    static Poller poller;
    return poller;
}

}  // namespace

void RealWeather_SetEnabled(bool enabled) {
    bool was = g_enabled.exchange(enabled, std::memory_order_relaxed);
    if (enabled && !was) {
        GetPoller().Start();  // first enable creates the thread
        GetPoller().Wake();   // and fetches straight away instead of waiting
    }
}

int RealWeather_GetIndex() {
    return g_index.load(std::memory_order_relaxed);
}

const char* RealWeather_GetLocation() {
    static thread_local std::string copy;
    std::lock_guard<std::mutex> lock(g_loc_mutex);
    copy = g_location;
    return copy.c_str();
}

#else  // REXGLUE_HAS_XEO3_TARGET

#include "real_weather.h"

void RealWeather_SetEnabled(bool) {}
int RealWeather_GetIndex() { return -1; }
const char* RealWeather_GetLocation() { return ""; }

#endif  // REXGLUE_HAS_XEO3_TARGET
