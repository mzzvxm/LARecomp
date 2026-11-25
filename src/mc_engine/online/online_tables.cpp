//
// online_tables.json: the game's stats views and session property names, written once for a launcher (see online_tables_core.h
// for what is in it and why). The game is only read: the tables are static data in the title's own memory.
//
// When: the first tick starts a clock; eight seconds later (and then every two seconds) the tables are read. The property table
// is the one the game fills at boot, so the first complete read writes the file; if it never fills, the file is written after
// five minutes with "properties": {"available": false}. Ninety seconds after the first write the tables are read once more
// and the file is rewritten only if the text changed (the game fills some property ids in lazily), then the tick stops.
// A file that already holds the same text is not touched, so a player who runs the game every day does not see it change.
//

#include "online_tables.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "online_common.h"
#include "larecomp_guest.h"
#include "larecomp_version.h"
#include "settings/immortal.h"
#include "spdlog_console.h"  // LarecompLogFolder()

#include <rex/kernel/guest_memory.h>
#include <rex/memory/virtual_memory.h>

namespace larecomp::online_tables {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr auto kFirstRead = 8s;
constexpr auto kRetry = 2s;
constexpr auto kGiveUp = 300s;
constexpr auto kSecondRead = 90s;

class LiveReader final : public GuestReader {
 public:
  bool Read(uint32_t addr, void* out, uint32_t len) const override {
    auto* rt = REX_RUNTIME();
    auto* mem = rt ? larecomp::guest::AddressSpace(rt) : nullptr;
    if (!mem || len == 0 || addr < 0x10000u || addr > 0xFFFFFFFFu - (len - 1)) return false;
    auto* heap = mem->LookupHeap(addr);
    if (!heap) return false;
    const auto access = heap->QueryRangeAccess(addr, addr + len - 1);
    if ((static_cast<uint32_t>(access) & static_cast<uint32_t>(rex::memory::PageAccess::kReadOnly)) == 0) return false;
    std::memcpy(out, mem->TranslateVirtual<const uint8_t*>(addr), len);
    return true;
  }
};

enum class Phase { kNotStarted, kWaiting, kWritten, kDone };

Phase g_phase = Phase::kNotStarted;
Clock::time_point g_start;
Clock::time_point g_next;
std::string& g_last_text = larecomp::Leak<std::string>();  // what this run wrote (or found already there); never destroyed (EXIT-HARD: a guest thread can still run at the exit)

std::string Utf8(const std::filesystem::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}

Tables Collect() {
  Tables t;
  t.game_version = larecomp::kVersionLabel;
  const StatsStatus st = ReadStatsSchema(LiveGuestReader(), kSpaAddress, &t.stats);
  t.stats_available = st == StatsStatus::kOk;
  if (!t.stats_available) t.stats_error = StatsStatusText(st);
  t.properties_available = ReadPropertyTable(LiveGuestReader(), kPropertyTable, &t.properties);
  return t;
}

// True when the file now holds `text` (written, or already the same).
bool WriteFile(const std::string& text) {
  if (text == g_last_text) return true;
  const std::filesystem::path folder = LarecompLogFolder();
  const std::filesystem::path file = folder / kFileName;
  std::error_code ec;
  {
    std::ifstream in(file, std::ios::binary);
    if (in) {
      const std::string old((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      if (old == text) {
        g_last_text = text;
        return true;
      }
    }
  }
  std::filesystem::create_directories(folder, ec);
  const std::filesystem::path tmp = folder / (std::string(kFileName) + ".tmp");
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      LARECOMP_APP_WARN("[online-tables] cannot write {}", Utf8(tmp));
      return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    if (!out) {
      LARECOMP_APP_WARN("[online-tables] cannot write {}", Utf8(tmp));
      out.close();
      std::filesystem::remove(tmp, ec);
      return false;
    }
  }
  std::filesystem::rename(tmp, file, ec);
  if (ec) {
    LARECOMP_APP_WARN("[online-tables] cannot replace {}: {}", Utf8(file), ec.message());
    std::filesystem::remove(tmp, ec);
    return false;
  }
  g_last_text = text;
  return true;
}

void Report(const Tables& t, const char* what) {
  LARECOMP_APP_INFO("[online-tables] {} {}: {} stats view(s) in {} column set(s){}, {} session propert{}{}", what,
                    kFileName, t.stats.views.size(), t.stats.column_sets.size(),
                    t.stats_available ? "" : std::string(" (stats: ") + t.stats_error + ")", t.properties.size(),
                    t.properties.size() == 1 ? "y" : "ies", t.properties_available ? "" : " (property table not filled)");
}

}  // namespace

const GuestReader& LiveGuestReader() {
  static const LiveReader reader;
  return reader;
}

void Tick() {
  if (g_phase == Phase::kDone) return;
  const auto now = Clock::now();
  if (g_phase == Phase::kNotStarted) {
    g_phase = Phase::kWaiting;
    g_start = now;
    g_next = now + kFirstRead;
    return;
  }
  if (now < g_next) return;

  const Tables t = Collect();
  if (g_phase == Phase::kWaiting) {
    if (!t.properties_available && now - g_start < kGiveUp) {
      g_next = now + kRetry;
      return;
    }
    if (WriteFile(ToJson(t))) Report(t, "wrote");
    g_phase = Phase::kWritten;
    g_next = now + kSecondRead;
    return;
  }
  // kWritten: one more look for ids the game filled in late.
  const std::string text = ToJson(t);
  if (text != g_last_text && WriteFile(text)) Report(t, "updated");
  g_phase = Phase::kDone;
}

}  // namespace larecomp::online_tables
