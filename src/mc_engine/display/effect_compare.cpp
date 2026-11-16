// The compare keys (F1 flips the selected effect, F12 selects the next one). A developer tool: the whole file is
// compiled only with LARECOMP_DEV_TOOLS (CMakeLists.txt adds it to the sources then). See effect_compare.h.

#if defined(LARECOMP_DEV_TOOLS)

#include "effect_compare.h"

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

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

REXCVAR_DEFINE_STRING(
    mcla_compare_effect, "bounce", "MCLA/GPU",
    "Developer tool (builds with LARECOMP_DEV_TOOLS only): which graphics effect the compare key flips off and on, to see "
    "what it does: bounce (the bounce light, the default), contact (contact shadows), shafts (sun shafts), occlusion "
    "(ambient occlusion), filter (the sun shadow filter), selfrefl (the car's reflection of its own body) or bloom (the glow "
    "size fix: the select key does not walk to it, set it here or on the command line). The select key is F12 and the "
    "compare key F1 until they are rebound in the key bindings of F4 (bind_mcla_compare_next, bind_mcla_compare). F12 "
    "selects the next effect and shows which one in a label at the top of the screen; F1 shows the selected effect the other "
    "way than your setting has it, the label says how, and the next press puts your setting back. The flip is never "
    "saved, and it is dropped when the setting it flips is changed or this one is. Live.")
    .debug_only()
    .allowed({"bounce", "contact", "shafts", "occlusion", "filter", "selfrefl", "bloom"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(
    mcla_compare_label_seconds, 1.5, "MCLA/GPU",
    "Diagnostic: how long the compare key's label stays up before it fades out (about 0.35 s more), 0.5 to 30. "
    "A test run takes its pictures a second after the press and sets it higher. Live.")
    .debug_only()
    .range(0.5, 30.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::compare {

namespace {

using Clock = std::chrono::steady_clock;
namespace cmp = mcla::gpu::compare;

constexpr char kBindName[] = "bind_mcla_compare";
constexpr char kDefaultKey[] = "F1";
constexpr char kNextBindName[] = "bind_mcla_compare_next";
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
Shown g_shown;

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
  saved.occlusion = rex::cvar::GetFlagByName("mcla_gpu_ao");
  saved.contact = rex::cvar::GetFlagByName("mcla_gpu_contact_shadows");
  saved.shafts = rex::cvar::GetFlagByName("mcla_gpu_sun_shafts");
  saved.bounce = rex::cvar::GetFlagByName("mcla_gpu_ssgi");
  saved.filter = rex::cvar::GetFlagByName("mcla_gpu_shadow_filter");
  saved.car_refl = rex::cvar::GetFlagByName("mcla_gpu_car_selfrefl");
  const std::string bloom = rex::cvar::GetFlagByName("mcla_gpu_bloom_scale");
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

  const cmp::Effect effect = cmp::ParseEffect(rex::cvar::GetFlagByName(kSettingName));
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

  const cmp::Effect next = cmp::NextEffect(cmp::ParseEffect(rex::cvar::GetFlagByName(kSettingName)));
  rex::cvar::SetFlagByName(kSettingName, cmp::Info(next).choice);
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
class LabelDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit LabelDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

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
    const float hold = float(std::clamp(REXCVAR_GET(mcla_compare_label_seconds), 0.5, 30.0));
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

std::unique_ptr<LabelDialog> g_label;

}  // namespace

namespace {

// The key a bind is on, for the log (capitals, digits, space and '+'): the default unless it was rebound.
std::string KeyLabel(const char* bind, const char* fallback) {
  const std::string key = rex::cvar::GetFlagByName(bind);
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

void Start(rex::ui::ImGuiDrawer* drawer) {
  if (!drawer || g_label) {
    return;
  }
  g_label = std::make_unique<LabelDialog>(drawer);
  rex::ui::RegisterBind(kBindName, kDefaultKey,
                        "Developer tool: flick the selected graphics effect (mcla_compare_effect) off and on to see the "
                        "difference; the label says what the picture shows. Never saved",
                        [] { OnKey(); });
  rex::ui::RegisterBind(kNextBindName, kNextDefaultKey,
                        "Developer tool: select the next graphics effect for the compare key (bounce light, contact shadows, "
                        "sun shafts, ambient occlusion, shadow filter, self-reflections); the label says which",
                        [] { OnSelectKey(); });
  // A flip ends when the setting it flips is changed, and when another effect is selected.
  for (int i = 0; i < cmp::kEffectCount; ++i) {
    const cmp::Effect effect = static_cast<cmp::Effect>(i);
    rex::cvar::RegisterChangeCallback(cmp::Info(effect).cvar, [effect](std::string_view, std::string_view) {
      OnSettingChanged(effect);
    });
  }
  rex::cvar::RegisterChangeCallback(kSettingName, [](std::string_view, std::string_view) {
    if (cmp::Global().Flipped() != cmp::Effect::kNone) {
      cmp::Global().Clear();
      Hide();
    }
  });
  MC_INFO("[compare] the compare key is {} ({}): it flips {} (mcla_compare_effect); {} ({}) selects the next effect; the label stays {:.1f} s",
          KeyLabel(kBindName, kDefaultKey), kBindName,
          std::string(cmp::Info(cmp::ParseEffect(rex::cvar::GetFlagByName(kSettingName))).label),
          KeyLabel(kNextBindName, kNextDefaultKey), kNextBindName, REXCVAR_GET(mcla_compare_label_seconds));
}

}  // namespace mcla::compare

#endif  // LARECOMP_DEV_TOOLS
