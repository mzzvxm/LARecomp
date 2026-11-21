#pragma once
//
// Which local address a LARecomp server listens on: the --bind <ip> option of larecomp_master_server,
// larecomp_rmr_server and larecomp_relay_server, and LARECOMP_BIND, the same thing from the environment.
//
// The default is every interface (0.0.0.0), the same as before this option: a server somebody hosts for other
// players must be reachable from outside. --bind is for the opposite wish: a server that stays on one adapter. The
// test trio runs on --bind 127.0.0.1 (nothing reaches it from the network, and Windows Firewall has nothing to ask
// about); a host with several adapters can pin a server to its LAN or VPN address.
//
// Order: --bind, then LARECOMP_BIND, then every interface. A value that is given but wrong stops the server with
// a message; it never falls back to every interface (a typo must not open the port wider than asked).
//
// Plain C++ with no sockets, so the rules have a check of their own (bind_address_test.cpp).
//

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace larecomp::online {

inline constexpr const char* kBindEnvVar = "LARECOMP_BIND";

struct BindAddress {
  uint32_t host_order = 0;       // the IPv4 address as a number (127.0.0.1 = 0x7F000001); 0 = every interface
  bool any = true;               // every interface (0.0.0.0)
  bool loopback = false;         // 127.0.0.0/8
  std::string text = "0.0.0.0";  // dotted form, for logs and messages
  std::string source = "default (all interfaces)";  // "--bind", "LARECOMP_BIND" or the default
};

namespace bind_detail {

inline std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
  return s;
}

inline std::string Lower(std::string_view s) {
  std::string r(s);
  for (char& c : r)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return r;
}

// Four decimal numbers of 1 to 3 digits, each 0..255, separated by single dots. "010" is refused (some parsers read it
// as octal, some as ten; nobody should have to know), and so is everything else (a name, "1.2.3", "1.2.3.4.5", a sign,
// a space, a port, IPv6).
inline bool ParseDottedQuad(std::string_view s, uint32_t* out) {
  uint32_t value = 0;
  int parts = 0;
  size_t i = 0;
  while (i <= s.size()) {
    size_t j = i;
    uint32_t n = 0;
    while (j < s.size() && s[j] >= '0' && s[j] <= '9') {
      n = n * 10 + uint32_t(s[j] - '0');
      if (++j - i > 3) return false;
    }
    const size_t digits = j - i;
    if (digits == 0 || n > 255 || (digits > 1 && s[i] == '0')) return false;
    value = (value << 8) | n;
    ++parts;
    if (j == s.size()) break;
    if (s[j] != '.' || parts == 4) return false;
    i = j + 1;
  }
  if (parts != 4) return false;
  *out = value;
  return true;
}

}  // namespace bind_detail

// Reads one address. "0.0.0.0", "any", "all" and "*" mean every interface; "localhost" means 127.0.0.1; anything else
// must be an IPv4 address of this machine in dotted form (the check that it IS this machine's is bind() itself).
// Multicast and broadcast addresses (224.0.0.0 and up) and the rest of 0.x.x.x are refused: a server needs a unicast
// address. Fills `out` (source untouched) and returns true, or sets `error` and returns false.
inline bool ParseBindAddress(std::string_view in, BindAddress* out, std::string* error) {
  const std::string_view s = bind_detail::Trim(in);
  auto fail = [&](const std::string& why) {
    if (error) *error = "\"" + std::string(s) + "\": " + why;
    return false;
  };
  if (s.empty()) return fail("empty (give an IPv4 address such as 127.0.0.1, or 0.0.0.0 for every interface)");
  const std::string word = bind_detail::Lower(s);
  uint32_t v = 0;
  if (word == "any" || word == "all" || word == "*") {
    v = 0;
  } else if (word == "localhost") {
    v = 0x7F000001u;
  } else if (s.find(':') != std::string_view::npos) {
    return fail("IPv6 and host:port are not supported; give one IPv4 address such as 127.0.0.1 (the port is --port)");
  } else if (!bind_detail::ParseDottedQuad(s, &v)) {
    return fail("not an IPv4 address (four numbers 0..255 with dots, such as 127.0.0.1; names other than localhost "
                "are not looked up)");
  } else if (v != 0 && ((v >> 24) == 0 || (v >> 24) >= 224)) {
    return fail("not an address a server can listen on (multicast, broadcast and 0.x.x.x are refused)");
  }
  out->host_order = v;
  out->any = v == 0;
  out->loopback = (v >> 24) == 127;
  char text[16];
  std::snprintf(text, sizeof(text), "%u.%u.%u.%u", unsigned(v >> 24), unsigned((v >> 16) & 255u),
                unsigned((v >> 8) & 255u), unsigned(v & 255u));
  out->text = text;
  return true;
}

// The rule: `option_value` (the text after --bind; nullptr when the option was not given) wins over `env_value` (the
// LARECOMP_BIND variable; nullptr or empty = not set), which wins over every interface. On failure `error` names the
// source that was wrong.
inline bool ResolveBindAddress(const char* option_value, const char* env_value, BindAddress* out, std::string* error) {
  BindAddress r;
  std::string why;
  if (option_value) {
    if (!ParseBindAddress(option_value, &r, &why)) {
      if (error) *error = "--bind " + why;
      return false;
    }
    r.source = "--bind";
  } else if (env_value && *env_value) {
    if (!ParseBindAddress(env_value, &r, &why)) {
      if (error) *error = std::string(kBindEnvVar) + " " + why;
      return false;
    }
    r.source = kBindEnvVar;
  }
  *out = r;
  return true;
}

// The same, reading the real environment.
inline bool ResolveBindAddress(const char* option_value, BindAddress* out, std::string* error) {
  return ResolveBindAddress(option_value, std::getenv(kBindEnvVar), out, error);
}

}  // namespace larecomp::online
