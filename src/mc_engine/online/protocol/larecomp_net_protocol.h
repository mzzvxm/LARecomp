// LARecomp — online multiplayer wire protocol (SERVER copy).
//
// MUST stay byte-identical to the client copy at
// rexglue-sdk/src/kernel/xam/mcla_online_protocol.h. Plain UDP, little-endian,
// packed structs.
//
// Two jobs: (1) session DIRECTORY (hosts register, clients query), (2) packet
// RELAY so emulated LAN broadcast/unicast crosses processes/machines. Each
// instance gets a server-assigned VIRTUAL IP used as its peer identity.

#pragma once
#include <cstdint>

namespace larecomp::net {

constexpr uint32_t kMagic = 0x3230434D;  // "MC02"
constexpr uint16_t kServerPort = 31000;
// Bumping is mandatory whenever SessionInfo changes size: peers and server
// validate MsgHeader::version and drop mismatches, which is what keeps an old
// build from misreading the struct.
//   v3 added SessionInfo::gamertag.
//   v4 added SessionInfo::props (host session properties/contexts).
//   v5 added SessionInfo::xuid, and kPing/kPong for server-measured RTT.
constexpr uint32_t kProtocolVersion = 5;
constexpr uint32_t kSessionTimeoutSec = 15;

enum class MsgType : uint8_t {
  kRegister = 1,
  kUnregister = 2,
  kHeartbeat = 3,
  kQuery = 4,
  kList = 5,
  kRegisterAck = 6,
  kRelay = 7,
  kRelayDeliver = 8,
  kPing = 9,   // server -> peer: echo the token back (RTT probe)
  kPong = 10,  // peer -> server: token echoed
};

#pragma pack(push, 1)

struct MsgHeader {
  uint32_t magic;
  uint32_t version;
  uint8_t type;
};

// One host session property/context, {id, 32-bit value}. Must match the SDK
// copy in mcla_online_protocol.h byte-for-byte.
struct SessionProp {
  uint32_t id;
  uint32_t value;
};
constexpr uint16_t kMaxSessionProps = 40;

struct SessionInfo {
  uint64_t session_id;
  uint32_t virtual_ip;  // server-assigned peer identity (host order)
  uint32_t host_ip;     // real source IP (network order)
  uint16_t host_port;
  uint32_t gamemode;
  uint32_t map;
  uint8_t max_players;
  uint8_t cur_players;
  uint8_t is_host;
  char name[32];
  // Profile gamertag of this peer, NUL terminated. Sent by every peer, host or
  // not, so the server can identify who is connected (name[] is the session
  // name, which only a host sets). Xbox gamertags are at most 15 chars.
  // NEVER printed raw -- it is attacker controlled; see SafeTag() in server.cpp.
  char gamertag[16];
  // Profile XUID of this peer (0 if not signed in / unavailable). Identifies a
  // user across reconnects and IP changes; used by the admin panel.
  uint64_t xuid;
  // Host session properties/contexts (event definition). prop_count valid; rest
  // zero. Relayed verbatim; the server does not interpret them.
  uint16_t prop_count;
  SessionProp props[kMaxSessionProps];
};

// RTT probe. Server sends kPing with an opaque token; peer echoes it back in a
// kPong. Server matches the token to the send time to compute round-trip.
struct PingMsg {
  MsgHeader hdr;
  uint64_t token;
};

struct RegisterMsg {
  MsgHeader hdr;
  SessionInfo session;
};

struct QueryMsg {
  MsgHeader hdr;
};

struct ListMsg {
  MsgHeader hdr;
  uint16_t count;
  // SessionInfo entries[count] follow.
};

struct RelayMsg {
  MsgHeader hdr;
  uint32_t addr;      // dest vip (kRelay) or src vip (kRelayDeliver), host order
  uint16_t src_port;
  uint16_t inner_len;
  // uint8_t inner[inner_len] follows.
};
constexpr uint16_t kMaxInner = 1408;

#pragma pack(pop)

}  // namespace larecomp::net

