// Unit test of the libcurl GET (os/curl_get.h) against a tiny HTTP server on 127.0.0.1 in this process. No network, no game.
//   clang++ -std=c++20 -I src src/os/curl_get_test.cpp src/os/curl_get.cpp -ldl -pthread -o curl_get_test && ./curl_get_test
// Exit code 0 = passed (also when there is no libcurl on this machine: the test says so and skips). Off Linux it only checks the "not here" answer.
#include "os/curl_get.h"

#include <chrono>
#include <cstdio>
#include <string>

#if defined(__linux__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <thread>
#endif

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& what) {
  if (!ok) {
    ++g_failures;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

#if defined(__linux__)

// One connection, one canned answer: reads the request up to its blank line, remembers it, answers, closes. `answer` empty: reads the request and
// then says nothing (a server that never answers); `hold_ms` keeps the connection open that long before closing.
class OneShotServer {
 public:
  explicit OneShotServer(std::string answer, int hold_ms = 0) : answer_(std::move(answer)), hold_ms_(hold_ms) {
    listen_ = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(listen_, 1);
    socklen_t len = sizeof(addr);
    getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { Serve(); });
  }
  ~OneShotServer() {
    if (listen_ >= 0) shutdown(listen_, SHUT_RDWR);  // wakes an accept that nobody came to
    if (thread_.joinable()) thread_.join();
    if (listen_ >= 0) close(listen_);
  }
  int port() const { return port_; }
  std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port_) + path; }
  std::string request() const { return request_; }

 private:
  void Serve() {
    sockaddr_in peer{};
    socklen_t len = sizeof(peer);
    const int s = accept(listen_, reinterpret_cast<sockaddr*>(&peer), &len);
    if (s < 0) return;
    char buf[2048];
    std::string in;
    while (in.find("\r\n\r\n") == std::string::npos) {
      const ssize_t n = recv(s, buf, sizeof(buf), 0);
      if (n <= 0) break;
      in.append(buf, size_t(n));
    }
    request_ = in;
    if (!answer_.empty()) send(s, answer_.data(), answer_.size(), MSG_NOSIGNAL);
    std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms_));
    close(s);
  }

  std::string answer_;
  int hold_ms_;
  int listen_ = -1;
  int port_ = 0;
  std::thread thread_;
  std::string request_;
};

std::string Http(int status, const char* reason, const std::string& body) {
  return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\nConnection: close\r\n\r\n" + body;
}

#endif

}  // namespace

int main() {
  using larecomp::os::HttpGet;
  using larecomp::os::HttpGetAvailable;
  using std::chrono::milliseconds;

#if !defined(__linux__)
  Check(!HttpGetAvailable(), "off Linux there is no libcurl client");
  const auto answer = HttpGet("http://127.0.0.1:1/", milliseconds(100));
  Check(!answer.ok && !answer.why.empty(), "off Linux a GET says why it did not run");
#else
  if (!HttpGetAvailable()) {
    std::printf("curl_get_test: skipped: no libcurl.so.4 on this machine (a request answers !ok with a reason)\n");
    const auto answer = HttpGet("http://127.0.0.1:1/", milliseconds(100));
    Check(!answer.ok && !answer.why.empty(), "without libcurl a GET says why it did not run");
    return g_failures == 0 ? 0 : 1;
  }

  // A 200 with a JSON body: the body comes back whole, the request line and the user agent are what the weather code sends.
  {
    const std::string body = R"({"latitude":-23.55,"longitude":-46.63,"city":"Sao Paulo"})";
    OneShotServer server(Http(200, "OK", body));
    const auto got = HttpGet(server.url("/json/"), milliseconds(5000));
    Check(got.ok, "a 200 answer is ok: " + got.why);
    Check(got.status == 200, "the status is 200");
    Check(got.body == body, "the body arrives whole, got '" + got.body + "'");
    const std::string request = server.request();
    Check(request.rfind("GET /json/ HTTP/1.1", 0) == 0, "the request line is GET /json/ HTTP/1.1, got '" + request.substr(0, 40) + "'");
    Check(request.find("User-Agent: larecomp/1.0") != std::string::npos, "the user agent is larecomp/1.0");
  }
  // A query string goes through as it is.
  {
    OneShotServer server(Http(200, "OK", "{}"));
    const auto got = HttpGet(server.url("/v1/forecast?latitude=-23.5500&longitude=-46.6300&current=weather_code"), milliseconds(5000));
    Check(got.ok, "a request with a query string is ok: " + got.why);
    Check(server.request().rfind("GET /v1/forecast?latitude=-23.5500&longitude=-46.6300&current=weather_code HTTP/1.1", 0) == 0, "the query string is sent as given");
  }
  // An HTTP error is a failure with its status, and the body is not handed back.
  {
    OneShotServer server(Http(429, "Too Many Requests", "{\"error\":true}"));
    const auto got = HttpGet(server.url("/json/"), milliseconds(5000));
    Check(!got.ok && got.status == 429 && got.body.empty(), "a 429 is a failure with its status and no body");
    Check(got.why.find("429") != std::string::npos, "the reason names the status: " + got.why);
  }
  // A redirect is not followed: the 302 is reported and is not an answer (nothing is fetched from where it points).
  {
    OneShotServer server("HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:1/elsewhere\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    const auto got = HttpGet(server.url("/json/"), milliseconds(5000));
    Check(!got.ok && got.status == 302, "a redirect is reported with its status and is not ok");
    Check(got.body.empty() && got.why.find("302") != std::string::npos, "with no body and its status in the reason: " + got.why);
  }
  // An answer longer than the cap fails.
  {
    OneShotServer server(Http(200, "OK", std::string(5000, 'x')));
    const auto got = HttpGet(server.url("/big"), milliseconds(5000), 1000);
    Check(!got.ok && got.body.empty(), "an answer over the cap fails");
    Check(got.why.find("1000") != std::string::npos, "and says the cap: " + got.why);
  }
  // Nobody listens: the connection is refused, with a reason, quickly.
  {
    int port = 0;
    {
      OneShotServer closed("");  // takes a free port; the server's destructor frees it
      port = closed.port();
    }
    const auto began = std::chrono::steady_clock::now();
    const auto got = HttpGet("http://127.0.0.1:" + std::to_string(port) + "/", milliseconds(3000));
    const auto took = std::chrono::duration_cast<milliseconds>(std::chrono::steady_clock::now() - began);
    Check(!got.ok && !got.why.empty(), "a refused connection fails with a reason");
    Check(took < milliseconds(2500), "and does not wait for the time limit (" + std::to_string(took.count()) + " ms)");
  }
  // A server that never answers: the time limit ends it.
  {
    OneShotServer server("", 1500);
    const auto began = std::chrono::steady_clock::now();
    const auto got = HttpGet(server.url("/"), milliseconds(300));
    const auto took = std::chrono::duration_cast<milliseconds>(std::chrono::steady_clock::now() - began);
    Check(!got.ok && !got.why.empty(), "a server that says nothing fails with a reason");
    Check(took >= milliseconds(250) && took < milliseconds(1400), "after about the time limit (" + std::to_string(took.count()) + " ms)");
  }
  // A URL that is not one.
  {
    const auto got = HttpGet("not a url", milliseconds(500));
    Check(!got.ok && !got.why.empty(), "a malformed URL fails with a reason");
  }
#endif

  if (g_failures == 0) std::printf("curl_get_test: all checks passed\n");
  return g_failures == 0 ? 0 : 1;
}
