#include "mod_download.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <thread>

#include <rex/filesystem.h>

#include "larecomp_log.h"
#include "../../online/protocol/browser_protocol.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
using SocketHandle = SOCKET;
constexpr SocketHandle kNoSocket = INVALID_SOCKET;
static void CloseSocket(SocketHandle s) { closesocket(s); }
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kNoSocket = -1;
static void CloseSocket(SocketHandle s) { close(s); }
#endif

namespace fs = std::filesystem;

namespace larecomp::browser {
namespace {

constexpr uint64_t kMaxTotalBytes = 4ull << 30;
constexpr uint64_t kMaxFileBytes = 2ull << 30;
constexpr size_t kMaxManifestBytes = 8u << 20;
constexpr int kMaxMods = 64;
constexpr int kTimeoutMs = 15000;
constexpr uint64_t kMaxImageBytes = 1u << 20;

std::string Utf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// A path from UTF-8 text (fs::u8path is deprecated in C++20).
fs::path FromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

fs::path ModelsDir() {
    const fs::path base = rex::filesystem::GetExecutableFolder();
    std::error_code ec;
    const fs::path modloader = base / "modloader";
    if (fs::is_directory(modloader, ec)) return modloader;
    const fs::path models = base / "models";
    if (fs::is_directory(models, ec)) return models;
    return modloader;
}

bool ValidModName(const std::string& n) {
    if (n.empty() || n.size() > 64 || n[0] == '.') return false;
    for (char c : n)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) return false;
    return true;
}

// A server-supplied path must stay inside its mod folder and must not be
// something Windows would run.
bool ValidModPath(const std::string& p) {
    if (p.empty() || p.size() > 400 || p[0] == '/') return false;
    if (p.find_first_of("\\:") != std::string::npos) return false;
    size_t start = 0;
    while (start <= p.size()) {
        size_t slash = p.find('/', start);
        if (slash == std::string::npos) slash = p.size();
        const std::string seg = p.substr(start, slash - start);
        if (seg.empty() || seg[0] == '.') return false;
        for (unsigned char c : seg)
            if (c < 0x20) return false;
        start = slash + 1;
    }
    std::string ext = fs::path(p).extension().string();
    for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    static const char* kBlocked[] = {".exe", ".dll", ".bat", ".cmd", ".com", ".ps1",
                                     ".vbs", ".js",  ".msi", ".scr", ".lnk", ".sh"};
    for (const char* b : kBlocked)
        if (ext == b) return false;
    return true;
}

fs::path LocalPath(const fs::path& root, const std::string& rel) {
    fs::path out = root;
    size_t start = 0;
    while (start <= rel.size()) {
        size_t slash = rel.find('/', start);
        if (slash == std::string::npos) slash = rel.size();
        out /= FromUtf8(rel.substr(start, slash - start));
        start = slash + 1;
    }
    return out;
}

// Hash of a local file, remembered by (path, size, mtime): tokyo alone is
// ~275 MB, and the check runs on every join.
bool LocalHash(const fs::path& p, uint64_t size, uint64_t* out) {
    static std::mutex m;
    static std::map<std::string, std::pair<std::pair<uint64_t, int64_t>, uint64_t>> cache;
    std::error_code ec;
    const auto mtime = fs::last_write_time(p, ec);
    if (ec) return false;
    const int64_t t = int64_t(mtime.time_since_epoch().count());
    const std::string key = Utf8(p);
    {
        std::lock_guard<std::mutex> lk(m);
        auto it = cache.find(key);
        if (it != cache.end() && it->second.first == std::make_pair(size, t)) {
            *out = it->second.second;
            return true;
        }
    }
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    Fnv1a64 h;
    std::vector<char> buf(1 << 20);
    while (f) {
        f.read(buf.data(), std::streamsize(buf.size()));
        h.Add(buf.data(), size_t(f.gcount()));
    }
    std::lock_guard<std::mutex> lk(m);
    cache[key] = {{size, t}, h.h};
    *out = h.h;
    return true;
}

bool LocalFileMatches(const fs::path& p, const ModFile& f) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || fs::file_size(p, ec) != f.bytes || ec) return false;
    uint64_t h = 0;
    return LocalHash(p, f.bytes, &h) && h == f.hash;
}

void EnsureWinsock() {
#ifdef _WIN32
    static bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
#endif
}

// GET http://host:port/path, body handed to `sink` as it arrives (sink returns
// false to stop). Plain HTTP/1.1 with Content-Length, which is what the relay
// sends; anything else is an error.
bool HttpGet(const std::string& host, uint16_t port, const std::string& path,
             const std::function<bool(const char*, size_t)>& sink, uint64_t* length, std::string* error) {
    EnsureWinsock();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        *error = "cannot resolve " + host;
        return false;
    }
    SocketHandle s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
    DWORD tv = kTimeoutMs;
#else
    timeval tv{kTimeoutMs / 1000, 0};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    const bool connected = s != kNoSocket && connect(s, res->ai_addr, int(res->ai_addrlen)) == 0;
    freeaddrinfo(res);
    if (!connected) {
        if (s != kNoSocket) CloseSocket(s);
        *error = "cannot connect to " + host + ":" + std::to_string(port);
        return false;
    }
    const std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    if (send(s, req.data(), int(req.size()), 0) != int(req.size())) {
        CloseSocket(s);
        *error = "send failed";
        return false;
    }
    std::string head;
    std::vector<char> buf(256 * 1024);
    uint64_t want = UINT64_MAX, got = 0;
    bool in_body = false, ok = true;
    for (;;) {
        const int n = int(recv(s, buf.data(), int(buf.size()), 0));
        if (n <= 0) break;
        const char* p = buf.data();
        size_t left = size_t(n);
        if (!in_body) {
            head.append(p, left);
            const size_t end = head.find("\r\n\r\n");
            if (end == std::string::npos) {
                if (head.size() > 16384) break;
                continue;
            }
            const int status = std::atoi(head.c_str() + (head.find(' ') == std::string::npos ? 0 : head.find(' ') + 1));
            if (status != 200) {
                *error = "server answered HTTP " + std::to_string(status);
                ok = false;
                break;
            }
            std::string lower = head.substr(0, end);
            for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
            const size_t cl = lower.find("content-length:");
            if (cl == std::string::npos) {
                *error = "no Content-Length";
                ok = false;
                break;
            }
            want = std::strtoull(lower.c_str() + cl + 15, nullptr, 10);
            if (length) *length = want;
            in_body = true;
            const std::string body = head.substr(end + 4);
            head.clear();
            if (!body.empty()) {
                got += body.size();
                if (!sink(body.data(), body.size())) {
                    ok = false;
                    *error = "cancelled";
                    break;
                }
            }
        } else {
            got += left;
            if (!sink(p, left)) {
                ok = false;
                *error = "cancelled";
                break;
            }
        }
        if (in_body && got >= want) break;
    }
    CloseSocket(s);
    if (ok && (!in_body || got != want)) {
        *error = in_body ? "connection closed early" : "no answer";
        ok = false;
    }
    return ok;
}

std::string UrlEncodePath(const std::string& p) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : p) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            o += char(c);
        } else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

}  // namespace

bool ModCheck::RequiredReady() const {
    for (const auto& m : mods)
        if (m.required && m.state != ModState::kInstalled) return false;
    return ok;
}

bool ModCheck::AllReady() const {
    for (const auto& m : mods)
        if (m.state != ModState::kInstalled) return false;
    return ok;
}

ModInfo ParseModInfo(const std::string& text) {
    ModInfo info;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        const size_t eq = line.find('=');
        const size_t start = line.find_first_not_of(" \t\xEF\xBB\xBF");
        if (eq == std::string::npos || start == std::string::npos || line[start] == '#' || start >= eq) continue;
        std::string key = line.substr(start, eq - start);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        std::string value;
        const size_t v = line.find_first_not_of(" \t", eq + 1);
        if (v == std::string::npos) continue;
        if (line[v] == '"') {
            for (size_t i = v + 1; i < line.size() && line[i] != '"'; ++i) {
                if (line[i] == '\\' && i + 1 < line.size()) {
                    const char e = line[++i];
                    value += e == 'n' ? '\n' : e;
                } else {
                    value += line[i];
                }
            }
        } else {
            value = line.substr(v);
            while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
        }
        // Text from someone else: no control characters but newlines, capped.
        std::string clean;
        for (char c : value)
            if (static_cast<unsigned char>(c) >= 0x20 || c == '\n') clean += c;
        if (clean.size() > 600) clean.resize(600);
        if (key == "title") info.title = clean.substr(0, 64);
        else if (key == "description") info.description = clean;
        else if (key == "author") info.author = clean.substr(0, 48);
        else if (key == "version") info.version = clean.substr(0, 24);
        else if (key == "image") info.image = clean.substr(0, 120);
    }
    if (!info.image.empty() && !ValidModPath(info.image)) info.image.clear();
    return info;
}

ModInfo ReadLocalModInfo(const std::string& folder) {
    std::ifstream f(ModelsDir() / FromUtf8(folder) / "mod.toml", std::ios::binary);
    if (!f) return {};
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (text.size() > 16384) text.resize(16384);
    return ParseModInfo(text);
}

std::vector<uint8_t> ReadLocalModImage(const std::string& folder, const ModInfo& info) {
    std::vector<uint8_t> out;
    if (info.image.empty()) return out;
    const fs::path p = LocalPath(ModelsDir() / FromUtf8(folder), info.image);
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    if (ec || size == 0 || size > kMaxImageBytes) return out;
    std::ifstream f(p, std::ios::binary);
    out.resize(size_t(size));
    if (!f.read(reinterpret_cast<char*>(out.data()), std::streamsize(size))) out.clear();
    return out;
}

int CompareVersions(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        auto piece = [](const std::string& s, size_t& k) {
            const size_t start = k;
            while (k < s.size() && s[k] != '.' && s[k] != '-') ++k;
            std::string p = s.substr(start, k - start);
            if (k < s.size()) ++k;
            return p;
        };
        const std::string pa = piece(a, i), pb = piece(b, j);
        const bool na = !pa.empty() && std::all_of(pa.begin(), pa.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
        const bool nb = !pb.empty() && std::all_of(pb.begin(), pb.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
        if (na && nb) {
            const unsigned long long x = std::strtoull(pa.c_str(), nullptr, 10);
            const unsigned long long y = std::strtoull(pb.c_str(), nullptr, 10);
            if (x != y) return x < y ? -1 : 1;
        } else if (pa != pb) {
            return pa < pb ? -1 : 1;
        }
    }
    return 0;
}

std::string FormatBytes(uint64_t b) {
    char out[32];
    if (b >= (1ull << 30))
        std::snprintf(out, sizeof(out), "%.1f GB", double(b) / double(1ull << 30));
    else if (b >= (1ull << 20))
        std::snprintf(out, sizeof(out), "%.1f MB", double(b) / double(1ull << 20));
    else
        std::snprintf(out, sizeof(out), "%.0f KB", double(b) / 1024.0);
    return out;
}

ModCheck CheckServerMods(const std::string& host, uint16_t http_port, bool with_info, bool with_images) {
    ModCheck check;
    std::string text;
    uint64_t length = 0;
    if (!HttpGet(host, http_port, kModManifestPath,
                 [&](const char* d, size_t n) {
                     text.append(d, n);
                     return text.size() <= kMaxManifestBytes;
                 },
                 &length, &check.error))
        return check;

    std::map<std::string, size_t> index;
    uint64_t total = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::vector<std::string> f;
        size_t a = pos;
        while (a <= nl) {
            size_t t = text.find('\t', a);
            if (t == std::string::npos || t > nl) t = nl;
            f.push_back(text.substr(a, t - a));
            a = t + 1;
        }
        pos = nl + 1;
        if (f.size() == 5 && f[0] == "mod") {
            if (!ValidModName(f[1]) || index.count(f[1]) || int(check.mods.size()) >= kMaxMods) continue;
            ServerMod m;
            m.name = f[1];
            m.required = f[2] == "1";
            index[m.name] = check.mods.size();
            check.mods.push_back(std::move(m));
        } else if (f.size() == 5 && f[0] == "file") {
            auto it = index.find(f[1]);
            if (it == index.end() || !ValidModPath(f[2])) {
                LARECOMP_APP_WARN("[browser] server mod file rejected: {}/{}", f[1], f[2]);
                continue;
            }
            ModFile mf;
            mf.path = f[2];
            mf.bytes = std::strtoull(f[3].c_str(), nullptr, 10);
            mf.hash = std::strtoull(f[4].c_str(), nullptr, 16);
            if (mf.bytes > kMaxFileBytes || total + mf.bytes > kMaxTotalBytes) {
                check.error = "the server's mods are larger than this game accepts";
                return check;
            }
            total += mf.bytes;
            check.mods[it->second].files.push_back(std::move(mf));
        }
    }

    const fs::path models = ModelsDir();
    for (auto& m : check.mods) {
        const fs::path dir = models / FromUtf8(m.name);
        std::error_code ec;
        const bool exists = fs::is_directory(dir, ec);
        for (auto& f : m.files) {
            m.bytes += f.bytes;
            f.have = exists && LocalFileMatches(LocalPath(dir, f.path), f);
            if (!f.have) m.need_bytes += f.bytes;
        }
        m.state = m.need_bytes == 0 && exists ? ModState::kInstalled : exists ? ModState::kOutdated : ModState::kMissing;
    }
    if (with_info) {
        for (auto& m : check.mods) {
            auto has = [&](const std::string& path) -> const ModFile* {
                for (const auto& f : m.files)
                    if (f.path == path) return &f;
                return nullptr;
            };
            const ModFile* toml = has("mod.toml");
            if (!toml || toml->bytes > 16384) continue;
            std::string text, err;
            if (!HttpGet(host, http_port, std::string(kModFilePrefix) + UrlEncodePath(m.name + "/mod.toml"),
                         [&](const char* d, size_t n) {
                             text.append(d, n);
                             return text.size() <= 16384;
                         },
                         nullptr, &err))
                continue;
            m.info = ParseModInfo(text);
            const ModFile* img = m.info.image.empty() ? nullptr : has(m.info.image);
            if (!with_images || !img || img->bytes > kMaxImageBytes) continue;
            std::vector<uint8_t> bytes;
            if (HttpGet(host, http_port, std::string(kModFilePrefix) + UrlEncodePath(m.name + "/" + img->path),
                        [&](const char* d, size_t n) {
                            bytes.insert(bytes.end(), d, d + n);
                            return bytes.size() <= kMaxImageBytes;
                        },
                        nullptr, &err))
                m.image = std::move(bytes);
        }
    }
    check.ok = true;
    return check;
}

// ------------------------------------------------------------ download ---

struct ModDownload::State {
    mutable std::mutex mutex;
    Progress progress;
    std::atomic<bool> cancel{false};
};

namespace {

void SetProgress(ModDownload::State& st, const std::function<void(ModDownload::Progress&)>& fn) {
    std::lock_guard<std::mutex> lk(st.mutex);
    fn(st.progress);
}

bool DownloadMod(const std::string& host, uint16_t port, const ServerMod& mod, ModDownload::State& st,
                 std::string* error) {
    const fs::path models = ModelsDir();
    const fs::path final_dir = models / FromUtf8(mod.name);
    const fs::path tmp = models / FromUtf8(".download_" + mod.name);
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    if (ec) {
        *error = "cannot create " + Utf8(tmp);
        return false;
    }
    for (const auto& f : mod.files) {
        if (st.cancel.load()) {
            *error = "cancelled";
            return false;
        }
        const fs::path dst = LocalPath(tmp, f.path);
        fs::create_directories(dst.parent_path(), ec);
        SetProgress(st, [&](ModDownload::Progress& p) { p.current = mod.name + "/" + f.path; });
        if (f.have) {
            // Same file here already: copy it instead of downloading it again.
            fs::copy_file(LocalPath(final_dir, f.path), dst, fs::copy_options::overwrite_existing, ec);
            if (!ec) {
                SetProgress(st, [&](ModDownload::Progress& p) { p.done += f.bytes; });
                continue;
            }
        }
        std::ofstream out(dst, std::ios::binary | std::ios::trunc);
        if (!out) {
            *error = "cannot write " + Utf8(dst);
            return false;
        }
        Fnv1a64 h;
        uint64_t got = 0;
        const bool ok = HttpGet(
            host, port, std::string(kModFilePrefix) + UrlEncodePath(mod.name + "/" + f.path),
            [&](const char* d, size_t n) {
                if (st.cancel.load() || got + n > f.bytes) return false;
                out.write(d, std::streamsize(n));
                h.Add(d, n);
                got += n;
                SetProgress(st, [&](ModDownload::Progress& p) { p.done += n; });
                return bool(out);
            },
            nullptr, error);
        out.close();
        if (!ok) return false;
        if (got != f.bytes || h.h != f.hash) {
            *error = mod.name + "/" + f.path + " arrived damaged (size or hash differ)";
            return false;
        }
    }
    // All there: the old folder steps aside (kept, the loader skips dot names).
    if (fs::exists(final_dir, ec)) {
        char stamp[32];
        std::snprintf(stamp, sizeof(stamp), "%lld", static_cast<long long>(std::time(nullptr)));
        fs::rename(final_dir, models / FromUtf8(".old_" + mod.name + "_" + stamp), ec);
        if (ec) {
            *error = "cannot move the old " + mod.name + " aside (is a file of it open?)";
            return false;
        }
    }
    fs::rename(tmp, final_dir, ec);
    if (ec) {
        *error = "cannot put " + mod.name + " in place";
        return false;
    }
    LARECOMP_APP_INFO("[browser] mod {} installed ({} files, {})", mod.name, mod.files.size(), FormatBytes(mod.bytes));
    return true;
}

}  // namespace

ModDownload::ModDownload(std::string host, uint16_t http_port, std::vector<ServerMod> mods)
    : state_(std::make_shared<State>()) {
    uint64_t total = 0;
    for (const auto& m : mods) total += m.bytes;
    state_->progress.total = total;
    auto st = state_;
    std::thread([st, host = std::move(host), http_port, mods = std::move(mods)] {
        std::string error;
        bool ok = true;
        for (const auto& m : mods) {
            if (!DownloadMod(host, http_port, m, *st, &error)) {
                ok = false;
                break;
            }
        }
        if (!ok) LARECOMP_APP_WARN("[browser] mod download failed: {}", error);
        SetProgress(*st, [&](Progress& p) {
            p.finished = true;
            p.failed = !ok;
            p.error = error;
        });
    }).detach();
}

ModDownload::~ModDownload() { state_->cancel.store(true); }

void ModDownload::Cancel() { state_->cancel.store(true); }

ModDownload::Progress ModDownload::Get() const {
    std::lock_guard<std::mutex> lk(state_->mutex);
    return state_->progress;
}

bool RestartGame() {
#ifdef _WIN32
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    // The arguments this run got, without the program name.
    std::wstring cmd = GetCommandLineW();
    size_t i = 0;
    if (!cmd.empty() && cmd[0] == L'"') {
        i = cmd.find(L'"', 1);
        i = i == std::wstring::npos ? cmd.size() : i + 1;
    } else {
        while (i < cmd.size() && cmd[i] != L' ' && cmd[i] != L'\t') ++i;
    }
    while (i < cmd.size() && (cmd[i] == L' ' || cmd[i] == L'\t')) ++i;
    const std::wstring args = cmd.substr(i);
    auto quote = [](const std::wstring& s) {
        std::wstring o = L"'";
        for (wchar_t c : s) {
            if (c == L'\'') o += L'\'';
            o += c;
        }
        return o + L"'";
    };
    const std::wstring dir = fs::path(exe).parent_path().wstring();
    // A helper waits for this process to be gone (two games on one folder
    // fight over the mod archive), then starts the new one.
    std::wstring ps = L"Wait-Process -Id " + std::to_wstring(GetCurrentProcessId()) +
                      L" -ErrorAction SilentlyContinue; Start-Process -FilePath " + quote(exe) +
                      L" -WorkingDirectory " + quote(dir);
    if (!args.empty()) ps += L" -ArgumentList " + quote(args);
    std::wstring line = L"powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden -Command \"" + ps + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(),
                        &si, &pi)) {
        LARECOMP_APP_WARN("[browser] could not start the restart helper ({})", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    // Close this game the normal way; it saves on its way out.
    struct Ctx {
        DWORD pid;
        HWND hwnd;
    } ctx{GetCurrentProcessId(), nullptr};
    EnumWindows(
        [](HWND h, LPARAM p) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(p);
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid == c->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) {
                c->hwnd = h;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    if (ctx.hwnd) PostMessageW(ctx.hwnd, WM_CLOSE, 0, 0);
    LARECOMP_APP_INFO("[browser] restarting the game");
    return true;
#else
    return false;
#endif
}

}  // namespace larecomp::browser
