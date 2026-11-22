// The compare keys (F1 flips the selected effect, F12 selects the next one). A developer tool: the whole file is
// compiled only with LARECOMP_DEV_TOOLS (CMakeLists.txt adds it to the sources then). See effect_compare.h.

#if defined(LARECOMP_DEV_TOOLS)

#include "effect_compare.h"
#include "settings/immortal.h"

#include <imgui.h>

#include "settings/larecomp_settings.h"
#include <rex/ui/ui_system.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "gpu/effect_compare_rules.h"
#include "gpu/gpu_class.h"
#include "mc_engine/core/logging.h"
#include "mc_engine/ui/imgui_theme.h"
#include "mc_engine/ui/overlay_support.h"

namespace mcla::compare {

namespace {

using Clock = std::chrono::steady_clock;
namespace cmp = mcla::gpu::compare;

constexpr char kBindName[] = "mcla_compare";  // ui.bind.mcla_compare (the 0.10 cvar was bind_mcla_compare)
constexpr char kDefaultKey[] = "F1";
constexpr char kNextBindName[] = "mcla_compare_next";  // ui.bind.mcla_compare_next (the 0.10 cvar was bind_mcla_compare_next)
// F12 is the last of the free function keys: F2 to F11 are taken (the texture dump, the telemetry card, the settings panel, the mod
// browser, the server browser, the achievements, the online identity, the map warp, the frame trace and the RenderDoc capture), and
// F1 was already the compare key.
constexpr char kNextDefaultKey[] = "F12";
constexpr char kSettingName[] = "mcla_compare_effect";

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

// What the label shows and since when: written by the key (and by the changes that drop a flip), read by the
// dialog's draw, both on the UI thread in practice; the mutex is for a change from another thread.
struct Shown {
  std::mutex mutex;
  cmp::Label label;
  Clock::time_point at{};
  bool visible = false;
};
Shown& g_shown = larecomp::Leak<Shown>();

void Show(const cmp::Label& label) {
  std::lock_guard lock(g_shown.mutex);
  g_shown.label = label;
  g_shown.at = Clock::now();
  g_shown.visible = !label.name.empty();
}

void Hide() {
  std::lock_guard lock(g_shown.mutex);
  g_shown.visible = false;
}

// The settings as they are saved: read through the registry's own locked accessor (the pause menu writes
// them from another thread). The flip is not in them.
cmp::Saved ReadSaved() {
  cmp::Saved saved;
  saved.cls = gpu::gpu_class::Current();
  saved.class_known = gpu::gpu_class::Known();
  saved.handheld = gpu::gpu_class::Handheld();
  saved.occlusion = larecomp::cvar::GetFlagByName("mcla_gpu_ao");
  saved.contact = larecomp::cvar::GetFlagByName("mcla_gpu_contact_shadows");
  saved.shafts = larecomp::cvar::GetFlagByName("mcla_gpu_sun_shafts");
  saved.bounce = larecomp::cvar::GetFlagByName("mcla_gpu_ssgi");
  saved.filter = larecomp::cvar::GetFlagByName("mcla_gpu_shadow_filter");
  saved.car_refl = larecomp::cvar::GetFlagByName("mcla_gpu_car_selfrefl");
  const std::string bloom = larecomp::cvar::GetFlagByName("mcla_gpu_bloom_scale");
  saved.bloom = bloom != "false" && bloom != "0";
  return saved;
}

void OnKey() {
  static int64_t last_press_ms = -1;  // the UI thread's
  const int64_t now = NowMs();
  if (!cmp::AcceptPress(now, last_press_ms)) {
    return;
  }
  last_press_ms = now;

  const cmp::Effect effect = cmp::ParseEffect(larecomp::cvar::GetFlagByName(kSettingName));
  cmp::State& state = cmp::Global();
  if (state.Flipped() != cmp::Effect::kNone && state.Flipped() != effect) {
    state.Clear();  // another effect was selected while one was flipped
  }
  const cmp::Press press = cmp::PressOf(effect, state.ForcedFor(effect), ReadSaved());
  if (!press.blocked) {
    state.Set(effect, press.force);
  }
  Show(cmp::LabelOf(effect, press.shown_on, press.blocked));
  MC_INFO("[compare] {} ({}; {} is not changed)", cmp::LabelText(effect, press.shown_on, press.blocked),
          press.blocked                         ? "nothing to flip"
          : press.force == cmp::Force::kNone ? "back to your setting"
                                              : "for a look",
          std::string(cmp::Info(effect).cvar));
}

// The select key: the next of the six effects, written into mcla_compare_effect (the one place the selection is kept, so F4 and the
// command line see it). The write runs the change callback below, which drops a flip that was on and hides the label; the note comes
// after it. It says what your setting shows of the effect now, which is what F1 flips away from.
void OnSelectKey() {
  static int64_t last_press_ms = -1;  // the UI thread's
  const int64_t now = NowMs();
  if (!cmp::AcceptPress(now, last_press_ms)) {
    return;
  }
  last_press_ms = now;

  const cmp::Effect next = cmp::NextEffect(cmp::ParseEffect(larecomp::cvar::GetFlagByName(kSettingName)));
  larecomp::cvar::SetFlagByName(kSettingName, cmp::Info(next).choice);
  const cmp::Standing standing = cmp::StandingOf(next, ReadSaved());
  Show(cmp::SelectedLabelOf(next, standing.on, standing.blocked));
  MC_INFO("[compare] selected {} ({}={}; your setting shows it {})", std::string(cmp::Info(next).label), kSettingName,
          std::string(cmp::Info(next).choice),
          standing.blocked ? "with nothing to flip: it needs the ambient occlusion" : standing.on ? "on" : "off");
}

// A setting that was flipped was changed (the pause menu, F4, the console): the flip goes, and with it the label.
void OnSettingChanged(cmp::Effect effect) {
  if (cmp::Global().Flipped() != effect) {
    return;
  }
  cmp::Global().Clear();
  Hide();
  MC_INFO("[compare] {}: the setting {} was changed, the flip is dropped", std::string(cmp::Info(effect).label),
          std::string(cmp::Info(effect).cvar));
}

// The label, top centre, in the game's panel style (imgui_theme.h): the effect in the menu's yellow, what it is
// doing under it, teal when on.
class LabelDialog final : public mcla::ui::PanelDialog {
 protected:
  void OnDraw(ImGuiIO& io) override {
    cmp::Label label;
    float age = 0.0f;
    {
      std::lock_guard lock(g_shown.mutex);
      if (!g_shown.visible) {
        return;
      }
      label = g_shown.label;
      age = std::chrono::duration<float>(Clock::now() - g_shown.at).count();
    }
    const float hold = float(std::clamp(larecomp::settings::mcla_compare_label_seconds.Get(), 0.5, 30.0));
    const float alpha = cmp::LabelAlpha(age, hold);
    if (alpha <= 0.0f) {
      Hide();
      return;
    }

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 16.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    // As wide as the words need, at least as wide as a short effect's name.
    ImGui::SetNextWindowSizeConstraints(ImVec2(280.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    if (mcla::ui::BeginPanel("##mcla_compare_label", label.name.c_str(), mcla::ui::PanelIcon::kNone,
                             ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_AlwaysAutoResize,
                             {}, 25.0f)) {
      ImGui::TextColored(label.good ? mcla::ui::kColorGood : mcla::ui::kColorWarn, "%s", label.state.c_str());
    }
    mcla::ui::EndPanel();
    ImGui::PopStyleVar();
  }
};

bool g_started = false;

}  // namespace

rex::ui::OverlayConfig KeyOverlayConfig() {
  return {kBindName, {rex::VirtualKey::kF1}, &mcla::ui::MakeHotkey<&OnKey>};
}

rex::ui::OverlayConfig SelectKeyOverlayConfig() {
  return {kNextBindName, {rex::VirtualKey::kF12}, &mcla::ui::MakeHotkey<&OnSelectKey>};
}

namespace {

// The key a bind is on, for the log (capitals, digits, space and '+'): the default unless it was rebound.
std::string KeyLabel(const char* bind, const char* fallback) {
  const std::string key = mcla::ui::BindText(bind, fallback);
  std::string out;
  for (const char c : key) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u)) {
      out += char(std::toupper(u));
    } else if (c == '+' || c == ' ') {
      out += c;
    }
  }
  return out.empty() ? std::string(fallback) : out;
}

}  // namespace

void Start(rex::ui::IUISystem& ui) {
  if (g_started) {
    return;
  }
  g_started = true;
  ui.AddDialog(std::make_unique<LabelDialog>());
  // A flip ends when the setting it flips is changed, and when another effect is selected.
  for (int i = 0; i < cmp::kEffectCount; ++i) {
    const cmp::Effect effect = static_cast<cmp::Effect>(i);
    larecomp::cvar::RegisterChangeCallback(cmp::Info(effect).cvar, [effect](std::string_view, std::string_view) {
      OnSettingChanged(effect);
    });
  }
  larecomp::cvar::RegisterChangeCallback(kSettingName, [](std::string_view, std::string_view) {
    if (cmp::Global().Flipped() != cmp::Effect::kNone) {
      cmp::Global().Clear();
      Hide();
    }
  });
  MC_INFO("[compare] the compare key is {} ({}): it flips {} (mcla_compare_effect); {} ({}) selects the next effect; the label stays {:.1f} s",
          KeyLabel(kBindName, kDefaultKey), kBindName,
          std::string(cmp::Info(cmp::ParseEffect(larecomp::cvar::GetFlagByName(kSettingName))).label),
          KeyLabel(kNextBindName, kNextDefaultKey), kNextBindName, larecomp::settings::mcla_compare_label_seconds.Get());
}

}  // namespace mcla::compare

#endif  // LARECOMP_DEV_TOOLS
