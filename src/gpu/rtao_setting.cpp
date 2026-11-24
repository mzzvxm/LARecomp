#include "rtao_setting.h"

#include <atomic>

namespace mcla::gpu::rtao_setting {
namespace {

// CHECK: R1 publishes RtAo::available() here and clears it on shutdown. R3 uses Ultra until then.
std::atomic<bool> g_pass_built{false};

}  // namespace

bool PassBuilt() { return g_pass_built.load(std::memory_order_relaxed); }

void NotePassBuilt(bool built) { g_pass_built.store(built, std::memory_order_relaxed); }

Decision DecideFor(std::string_view setting_text) {
  return Decide(gc::Current(), gc::Known(), gc::CurrentRtTier(), ParseChoice(setting_text), PassBuilt());
}

}  // namespace mcla::gpu::rtao_setting
