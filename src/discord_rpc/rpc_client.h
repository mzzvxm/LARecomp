/**
 * @file        rpc_client.h
 * @brief       Lightweight Discord Rich Presence client (vendored from the ReXGlue SDK)
 *
 * Communicates with the local Discord client over a named pipe (IPC) using a
 * JSON payload — no third-party dependencies.
 *
 * To enable Rich Presence, create an Application at
 * https://discord.com/developers/applications and pass its Application ID to
 * Start(). Upload artwork there and reference asset names via SetLargeImage /
 * SetSmallImage.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <string>

#include <rex/platform.h>

#if REX_PLATFORM_WIN32 || REX_PLATFORM_LINUX

namespace larecomp::discord_rpc {

struct Presence {
  std::string details_;           // first line under the title, e.g. "In Main Menu"
  std::string state_;             // second line, e.g. "Level 196"
  std::string large_image_key_;   // asset name from the Developer Portal
  std::string large_image_text_;  // tooltip shown on hover
  std::string small_image_key_;
  std::string small_image_text_;
};

// Spawns a background worker thread (idempotent). Sets the initial presence.
void Start(const char* client_id, const Presence& initial = {});

// Replaces the entire presence
void SetPresence(const Presence& p);

// Per-field setters for incremental updates (avoids constructing a full Presence).
void SetDetails(const std::string& details);
void SetState(const std::string& state);
void SetLargeImage(const std::string& key, const std::string& text = "");
void SetSmallImage(const std::string& key, const std::string& text = "");

// Stops the worker thread and clears the presence.
void Stop();

}  // namespace larecomp::discord_rpc

#endif  // REX_PLATFORM_WIN32 || REX_PLATFORM_LINUX
