// Discord Rich Presence IPC client, vendored from the ReXGlue SDK
// (src/discord/discord_rpc.cpp) so larecomp builds against the stock SDK,
// which has no rex/discord_rpc.h. Namespace and cvar are renamed so this
// coexists with a fork SDK that still exports its own copy.

#include "rpc_client.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#if REX_PLATFORM_LINUX
#include <vector>
#endif

// The SDK compiled this only on Windows and Linux (its CMakeLists bailed out
// elsewhere); larecomp adds the file unconditionally, so the guard lives here.
#if REX_PLATFORM_WIN32 || REX_PLATFORM_LINUX

REXCVAR_DEFINE_BOOL(larecomp_discord_activity, true, "Discord", "Enable Discord Rich Presence activity");

#if REX_PLATFORM_WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#elif REX_PLATFORM_LINUX
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace larecomp::discord_rpc {

namespace {

// IPC frame opcodes defined by the Discord RPC protocol.
enum : uint32_t {
  kOpHandshake = 0,
  kOpFrame = 1,
  kOpClose = 2,
  kOpPing = 3,
  kOpPong = 4,
};

std::atomic<bool> g_running{false};
std::thread g_thread;
std::mutex g_mutex;
Presence g_presence;
std::string g_client_id;
std::atomic<bool> g_dirty{false};

#if REX_PLATFORM_WIN32
HANDLE g_pipe = INVALID_HANDLE_VALUE;
bool IsConnected() {
  return g_pipe != INVALID_HANDLE_VALUE;
}
#elif REX_PLATFORM_LINUX
int g_sock = -1;
bool IsConnected() {
  return g_sock != -1;
}
#endif

#if REX_PLATFORM_WIN32

bool ConnectToDiscord() {
  for (int i = 0; i < 10; ++i) {
    char name[64];
    std::snprintf(name, sizeof(name), "\\\\.\\pipe\\discord-ipc-%d", i);
    HANDLE h =
        ::CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      g_pipe = h;
      return true;
    }
  }
  REXLOG_WARN("DiscordRPC: no Discord IPC pipe found (is Discord running?)");
  return false;
}

void CloseConnection() {
  if (g_pipe != INVALID_HANDLE_VALUE) {
    ::CloseHandle(g_pipe);
    g_pipe = INVALID_HANDLE_VALUE;
  }
}

#elif REX_PLATFORM_LINUX

std::vector<std::string> GetSocketDirs() {
  std::vector<std::string> dirs;
  const char* xdg = std::getenv("XDG_RUNTIME_DIR");
  if (xdg && *xdg)
    dirs.push_back(xdg);
  const char* tmp = std::getenv("TMPDIR");
  if (tmp && *tmp)
    dirs.push_back(tmp);
  dirs.push_back("/run/user/" + std::to_string(::getuid()));
  dirs.push_back("/tmp");
  return dirs;
}

bool ConnectToDiscord() {
  for (const auto& dir : GetSocketDirs()) {
    for (int i = 0; i < 10; ++i) {
      std::string path = dir + "/discord-ipc-" + std::to_string(i);
      int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd == -1)
        continue;
      sockaddr_un addr{};
      addr.sun_family = AF_UNIX;
      std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
      if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        g_sock = fd;
        return true;
      }
      ::close(fd);
    }
  }
  REXLOG_WARN("DiscordRPC: no Discord IPC socket found (is Discord running?)");
  return false;
}

void CloseConnection() {
  if (g_sock != -1) {
    ::close(g_sock);
    g_sock = -1;
  }
}

#endif  // connect/close

#if REX_PLATFORM_WIN32

bool WriteFrame(uint32_t op, const std::string& payload) {
  if (!IsConnected())
    return false;
  uint32_t header[2] = {op, static_cast<uint32_t>(payload.size())};
  DWORD written = 0;
  if (!::WriteFile(g_pipe, header, sizeof(header), &written, nullptr) || written != sizeof(header))
    return false;
  if (!payload.empty()) {
    if (!::WriteFile(g_pipe, payload.data(), static_cast<DWORD>(payload.size()), &written,
                     nullptr) ||
        written != payload.size())
      return false;
  }
  return true;
}

#elif REX_PLATFORM_LINUX

bool WriteAll(const void* buf, size_t len) {
  const char* ptr = static_cast<const char*>(buf);
  while (len > 0) {
    ssize_t n = ::write(g_sock, ptr, len);
    if (n <= 0)
      return false;
    ptr += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

bool WriteFrame(uint32_t op, const std::string& payload) {
  if (!IsConnected())
    return false;
  uint32_t header[2] = {op, static_cast<uint32_t>(payload.size())};
  if (!WriteAll(header, sizeof(header)))
    return false;
  if (!payload.empty())
    if (!WriteAll(payload.data(), payload.size()))
      return false;
  return true;
}

#endif  // WriteFrame

#if REX_PLATFORM_WIN32

void DrainMessages(bool* out_ready = nullptr) {
  if (!IsConnected())
    return;
  while (true) {
    DWORD avail = 0;
    if (!::PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &avail, nullptr))
      return;
    if (avail < 8)
      return;
    uint32_t header[2] = {0, 0};
    DWORD read = 0;
    if (!::ReadFile(g_pipe, header, sizeof(header), &read, nullptr) || read != sizeof(header))
      return;
    std::string buf;
    if (header[1] > 0) {
      buf.resize(header[1]);
      DWORD total = 0;
      while (total < header[1]) {
        DWORD n = 0;
        if (!::ReadFile(g_pipe, buf.data() + total, header[1] - total, &n, nullptr) || n == 0)
          return;
        total += n;
      }
    }
    if (header[0] == kOpPing) {
      WriteFrame(kOpPong, buf);
    } else if (header[0] == kOpFrame) {
      if (out_ready && buf.find("\"evt\":\"READY\"") != std::string::npos)
        *out_ready = true;
    } else if (header[0] == kOpClose) {
      REXLOG_WARN("DiscordRPC: server closed: {}", buf);
      CloseConnection();
      return;
    }
  }
}

#elif REX_PLATFORM_LINUX

bool ReadAll(void* buf, size_t len) {
  char* ptr = static_cast<char*>(buf);
  while (len > 0) {
    ssize_t n = ::recv(g_sock, ptr, len, 0);
    if (n <= 0)
      return false;
    ptr += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

bool HasData() {
  if (!IsConnected())
    return false;
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(g_sock, &fds);
  timeval tv{0, 0};
  return ::select(g_sock + 1, &fds, nullptr, nullptr, &tv) > 0;
}

void DrainMessages(bool* out_ready = nullptr) {
  if (!IsConnected())
    return;
  while (HasData()) {
    uint32_t header[2] = {0, 0};
    if (!ReadAll(header, sizeof(header))) {
      CloseConnection();
      return;
    }
    std::string buf;
    if (header[1] > 0) {
      buf.resize(header[1]);
      if (!ReadAll(buf.data(), header[1])) {
        CloseConnection();
        return;
      }
    }
    if (header[0] == kOpPing) {
      WriteFrame(kOpPong, buf);
    } else if (header[0] == kOpFrame) {
      if (out_ready && buf.find("\"evt\":\"READY\"") != std::string::npos)
        *out_ready = true;
    } else if (header[0] == kOpClose) {
      REXLOG_WARN("DiscordRPC: server closed: {}", buf);
      CloseConnection();
      return;
    }
  }
}

#endif  // DrainMessages

#if REX_PLATFORM_WIN32
uint32_t GetPid() {
  return static_cast<uint32_t>(::GetCurrentProcessId());
}
std::string GetNonce() {
  return std::to_string(::GetTickCount());
}
#elif REX_PLATFORM_LINUX
uint32_t GetPid() {
  return static_cast<uint32_t>(::getpid());
}
std::string GetNonce() {
  return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());
}
#endif

std::string JsonEscape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 2);
  for (char c : in) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char hex[8];
          std::snprintf(hex, sizeof(hex), "\\u%04x", c);
          out += hex;
        } else {
          out += c;
        }
    }
  }
  return out;
}

bool SendHandshake() {
  std::string payload = std::string("{\"v\":1,\"client_id\":\"") + g_client_id + "\"}";
  return WriteFrame(kOpHandshake, payload);
}

bool SendClearActivity() {
  std::string payload = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":";
  payload += std::to_string(GetPid());
  payload += ",\"activity\":null},\"nonce\":\"";
  payload += GetNonce();
  payload += "\"}";
  return WriteFrame(kOpFrame, payload);
}

bool SendActivity(const Presence& p, int64_t start_timestamp) {
  auto appendStr = [](std::string& dst, bool& first, const char* key, const std::string& val) {
    if (val.empty())
      return;
    if (!first)
      dst += ",";
    dst += "\"";
    dst += key;
    dst += "\":\"" + JsonEscape(val) + "\"";
    first = false;
  };

  std::string assets;
  {
    bool first_a = true;
    assets = "{";
    appendStr(assets, first_a, "large_image", p.large_image_key_);
    appendStr(assets, first_a, "large_text", p.large_image_text_);
    appendStr(assets, first_a, "small_image", p.small_image_key_);
    appendStr(assets, first_a, "small_text", p.small_image_text_);
    assets += "}";
    if (first_a)
      assets.clear();
  }

  std::string activity = "{";
  bool first = true;
  appendStr(activity, first, "details", p.details_);
  appendStr(activity, first, "state", p.state_);
  if (start_timestamp > 0) {
    if (!first)
      activity += ",";
    activity += "\"timestamps\":{\"start\":";
    activity += std::to_string(start_timestamp);
    activity += "}";
    first = false;
  }
  if (!assets.empty()) {
    if (!first)
      activity += ",";
    activity += "\"assets\":";
    activity += assets;
    first = false;
  }
  if (!first)
    activity += ",";
  activity += "\"instance\":false";
  activity += "}";

  std::string payload = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":";
  payload += std::to_string(GetPid());
  payload += ",\"activity\":";
  payload += activity;
  payload += "},\"nonce\":\"";
  payload += GetNonce();
  payload += "\"}";
  return WriteFrame(kOpFrame, payload);
}

void WorkerThread() {
  using clock = std::chrono::steady_clock;
  auto next_reconnect = clock::now();
  int64_t start_timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
  bool handshake_sent = false;
  bool ready = false;
  bool last_activity_enabled = REXCVAR_GET(larecomp_discord_activity);

  while (g_running.load()) {
    if (!IsConnected()) {
      if (clock::now() < next_reconnect) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;
      }
      if (!ConnectToDiscord()) {
        next_reconnect = clock::now() + std::chrono::seconds(15);
        continue;
      }
      handshake_sent = false;
      ready = false;
    }

    if (!handshake_sent) {
      if (!SendHandshake()) {
        CloseConnection();
        next_reconnect = clock::now() + std::chrono::seconds(15);
        continue;
      }
      handshake_sent = true;
    }

    DrainMessages(&ready);

    const bool activity_enabled = REXCVAR_GET(larecomp_discord_activity);
    if (activity_enabled != last_activity_enabled) {
      last_activity_enabled = activity_enabled;
      g_dirty = true;
    }

    if (ready && g_dirty.exchange(false)) {
      if (activity_enabled) {
        Presence snap;
        {
          std::lock_guard<std::mutex> lock(g_mutex);
          snap = g_presence;
        }
        if (!SendActivity(snap, start_timestamp)) {
          CloseConnection();
          next_reconnect = clock::now() + std::chrono::seconds(5);
          continue;
        }
      } else {
        if (!SendClearActivity()) {
          CloseConnection();
          next_reconnect = clock::now() + std::chrono::seconds(5);
          continue;
        }
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }

  if (IsConnected()) {
    SendClearActivity();
    WriteFrame(kOpClose, "{}");
    CloseConnection();
  }
}

void SyncClearAndClose() {
  if (!IsConnected())
    return;
  SendClearActivity();
  WriteFrame(kOpClose, "{}");
  CloseConnection();
}

#if REX_PLATFORM_WIN32
BOOL WINAPI ConsoleCtrlHandler(DWORD) {
  SyncClearAndClose();
  return FALSE;
}
#endif

struct AtExitInstaller {
  AtExitInstaller() {
    std::atexit([] { SyncClearAndClose(); });
#if REX_PLATFORM_WIN32
    ::SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#endif
  }
} g_atExitInstaller;

}  // namespace

void Start(const char* client_id, const Presence& initial) {
  if (!client_id || *client_id == '\0') {
    REXLOG_WARN("DiscordRPC: Start() called with null/empty client_id — skipping");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_client_id = client_id;
    g_presence = initial;
  }
  g_dirty = true;
  if (g_running.exchange(true))
    return;
  g_thread = std::thread(WorkerThread);
}

void SetPresence(const Presence& p) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_presence = p;
  }
  g_dirty = true;
}

void SetDetails(const std::string& details) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_presence.details_ = details;
  }
  g_dirty = true;
}

void SetState(const std::string& state) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_presence.state_ = state;
  }
  g_dirty = true;
}

void SetLargeImage(const std::string& key, const std::string& text) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_presence.large_image_key_ = key;
    g_presence.large_image_text_ = text;
  }
  g_dirty = true;
}

void SetSmallImage(const std::string& key, const std::string& text) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_presence.small_image_key_ = key;
    g_presence.small_image_text_ = text;
  }
  g_dirty = true;
}

void Stop() {
  if (!g_running.exchange(false))
    return;
  if (g_thread.joinable())
    g_thread.join();
}

}  // namespace larecomp::discord_rpc

#endif  // REX_PLATFORM_WIN32 || REX_PLATFORM_LINUX
