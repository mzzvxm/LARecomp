#include "rtao_setting.h"

#include <atomic>

namespace mcla::gpu::rtao_setting {
namespace {

// RtAo::Initialize publishes RtAo::available() here and RtAo::Shutdown clears it; until then (and on a card that cannot build the pass) the row says NOT READY and the frame is Ultra.
std::atomic<bool> g_pass_built{false};

}  // namespace

bool PassBuilt() { return g_pass_built.load(std::memory_order_relaxed); }

void NotePassBuilt(bool built) { g_pass_built.store(built, std::memory_order_relaxed); }

Decision DecideFor(std::string_view setting_text) {
  return Decide(gc::Current(), gc::Known(), gc::CurrentRtTier(), ParseChoice(setting_text), PassBuilt());
}

}  // namespace mcla::gpu::rtao_setting
