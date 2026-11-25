#pragma once
//
// Whether this build can play online at all.
//
// The relay client that carries the game's network traffic to an online server is the SDK's (src/xam/mcla_online.cpp in LAR-RXSDK),
// and it is written on Winsock: off Windows its sockets, its receive thread and its heartbeat are empty stubs. Nothing on the
// LARecomp side can make up for that, so on Linux online play does not exist yet, and everything of ours that offers it says so
// instead of looking like it works and failing later with "Server is offline":
//   * the log line at start (InitOnlineSettings in menus/pause_menu.cpp), and an online setting from larecomp.toml or the command
//     line is ignored for the run, so the game never routes its sockets through a relay it cannot reach;
//   * the SYSTEM LINK menu's ONLINE row stays, greyed, and its label says so; the SERVER BROWSER rows are not added;
//   * the pop-up that shows the connection (online/online_toast.cpp) and the F6 server browser say it in a sentence.
// When the SDK gets a POSIX client, this is the one line to change.
//
namespace larecomp {

#if defined(_WIN32)
inline constexpr bool kOnlineAvailable = true;
#else
inline constexpr bool kOnlineAvailable = false;
#endif

// The sentence every place uses (no full stop: a caller adds its own).
inline constexpr const char* kOnlineUnavailableText = "Online play is not available on Linux yet";

}  // namespace larecomp
