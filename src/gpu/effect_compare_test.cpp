// Check of the compare key's rules (effect_compare.h: the effects and the flip; effect_compare_rules.h: what a
// press does and says). A program of its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when
// every check holds.
//
//   cmake --build <build dir> --target larecomp_effect_compare_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_effect_compare_test
//
// The expected values are written out here again on purpose, as host_plan_test.cpp does: the setting names
// and the `auto` table of gpu_class.h are the other side of the check.

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "effect_compare_rules.h"

using namespace mcla::gpu::compare;
using mcla::gpu::gpu_class::GpuClass;
namespace gc = mcla::gpu::gpu_class;
namespace hp = mcla::gpu::host_plan;
namespace sf = mcla::gpu::shadow_filter;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

void CheckEq(const std::string& got, const std::string& want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::printf("FAILED: %s: got '%s', want '%s'\n", what.c_str(), got.c_str(), want.c_str());
  }
}

const GpuClass kClasses[] = {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh};
const char* ClassWord(GpuClass cls) {
  return cls == GpuClass::kLow ? "low" : cls == GpuClass::kMid ? "mid" : "high";
}

Saved Make(GpuClass cls, bool known, const char* ao, const char* contact, const char* shafts, const char* bounce,
           const char* filter, bool bloom) {
  Saved saved;
  saved.cls = cls;
  saved.class_known = known;
  saved.occlusion = ao;
  saved.contact = contact;
  saved.shafts = shafts;
  saved.bounce = bounce;
  saved.filter = filter;
  saved.bloom = bloom;
  return saved;
}

// ---- the table -------------------------------------------------------------------------------------------

void TestTable() {
  struct Want {
    Effect effect;
    const char* choice;
    const char* cvar;
    const char* label;
  };
  const Want want[] = {
      {Effect::kBounceLight, "bounce", "mcla_gpu_ssgi", "BOUNCE LIGHT"},
      {Effect::kContactShadows, "contact", "mcla_gpu_contact_shadows", "CONTACT SHADOWS"},
      {Effect::kSunShafts, "shafts", "mcla_gpu_sun_shafts", "SUN SHAFTS"},
      {Effect::kAmbientOcclusion, "occlusion", "mcla_gpu_ao", "AMBIENT OCCLUSION"},
      {Effect::kShadowFilter, "filter", "mcla_gpu_shadow_filter", "SHADOW FILTER"},
      {Effect::kBloomFix, "bloom", "mcla_gpu_bloom_scale", "BLOOM FIX"},
  };
  Check(kEffectCount == 6, "six effects");
  for (const Want& w : want) {
    const std::string name = w.choice;
    CheckEq(std::string(Info(w.effect).choice), w.choice, name + ": choice");
    CheckEq(std::string(Info(w.effect).cvar), w.cvar, name + ": cvar");
    CheckEq(std::string(Info(w.effect).label), w.label, name + ": label");
    Check(ParseEffect(w.choice) == w.effect, name + ": the choice parses to the effect");
    Check(EffectOfCvar(w.cvar) == w.effect, name + ": the cvar maps back to the effect");
    // The label is capitals and spaces (the game's own face), and short enough for the pause row: "COMPARE KEY: "
    // is 13 characters and a page takes 41.
    bool plain = true;
    for (const char ch : std::string(Info(w.effect).label)) {
      plain = plain && ((ch >= 'A' && ch <= 'Z') || ch == ' ');
    }
    Check(plain, name + ": the label is capitals and spaces");
    Check(Info(w.effect).label.size() <= 17, name + ": the label fits the pause row");
    // A string setting has a text to be forced on with, the switch has none.
    Check((w.effect == Effect::kBloomFix) == Info(w.effect).on_text.empty(), name + ": on_text only for the switch");
  }
  Check(ParseEffect("") == Effect::kBounceLight, "an empty choice is the bounce light");
  Check(ParseEffect("nonsense") == Effect::kBounceLight, "a text that is no choice is the bounce light");
  Check(ParseEffect("BOUNCE") == Effect::kBounceLight, "a capitalised choice falls back to the bounce light");
  Check(EffectOfCvar("mcla_gpu_shadow_resolution") == Effect::kNone, "the shadow detail is not a compare effect");
  Check(EffectOfCvar("") == Effect::kNone, "an empty cvar name is no effect");
  // The names the owners use (gpu_class.h's table has the same cvar for the four that have an `auto`).
  CheckEq(std::string(gc::CvarName(gc::Effect::kContactShadows)), std::string(Info(Effect::kContactShadows).cvar), "contact cvar agrees with gpu_class.h");
  CheckEq(std::string(gc::CvarName(gc::Effect::kSunShafts)), std::string(Info(Effect::kSunShafts).cvar), "shafts cvar agrees with gpu_class.h");
  CheckEq(std::string(gc::CvarName(gc::Effect::kSsgi)), std::string(Info(Effect::kBounceLight).cvar), "bounce cvar agrees with gpu_class.h");
  CheckEq(std::string(gc::CvarName(gc::Effect::kShadowFilter)), std::string(Info(Effect::kShadowFilter).cvar), "filter cvar agrees with gpu_class.h");
}

// ---- the flip --------------------------------------------------------------------------------------------

void TestState() {
  State state;
  for (int i = 0; i < kEffectCount; ++i) {
    Check(state.ForcedFor(static_cast<Effect>(i)) == Force::kNone, "a new state forces nothing");
  }
  Check(state.Flipped() == Effect::kNone, "a new state has no flip");
  state.Set(Effect::kBounceLight, Force::kOff);  // effect 0 with force 1: not the empty word
  Check(state.ForcedFor(Effect::kBounceLight) == Force::kOff, "the bounce light is forced off");
  Check(state.Flipped() == Effect::kBounceLight, "the flip is on the bounce light");
  for (int i = 1; i < kEffectCount; ++i) {
    Check(state.ForcedFor(static_cast<Effect>(i)) == Force::kNone, "only the chosen effect is forced");
  }
  Check(state.ForcedFor(Effect::kNone) == Force::kNone, "no effect is forced for kNone");
  state.Set(Effect::kBloomFix, Force::kOn);
  Check(state.ForcedFor(Effect::kBloomFix) == Force::kOn, "the bloom fix is forced on");
  Check(state.ForcedFor(Effect::kBounceLight) == Force::kNone, "setting another flip drops the first");
  Check(state.Apply(Effect::kBloomFix, false), "a switch forced on is on");
  state.Set(Effect::kBloomFix, Force::kOff);
  Check(!state.Apply(Effect::kBloomFix, true), "a switch forced off is off");
  Check(state.Apply(Effect::kShadowFilter, true), "another effect's switch is as it was");
  state.Set(Effect::kBloomFix, Force::kNone);
  Check(state.Flipped() == Effect::kNone, "forcing nothing is no flip");
  Check(state.Apply(Effect::kBloomFix, true) && !state.Apply(Effect::kBloomFix, false),
        "no flip leaves a switch as it was");
  state.Set(Effect::kSunShafts, Force::kOn);
  state.Clear();
  Check(state.Flipped() == Effect::kNone && state.ForcedFor(Effect::kSunShafts) == Force::kNone,
        "Clear drops the flip");
  state.Set(Effect::kNone, Force::kOn);
  Check(state.Flipped() == Effect::kNone, "a flip of no effect is none");
}

// ---- what the saved settings come to ---------------------------------------------------------------------

void TestStanding() {
  for (const GpuClass cls : kClasses) {
    const std::string c = ClassWord(cls);
    // The occlusion: off, low, high; no `auto`; anything else is off.
    for (const char* text : {"off", "low", "high", "garbage", ""}) {
      const bool on = std::string(text) == "low" || std::string(text) == "high";
      const Standing s = StandingOf(Effect::kAmbientOcclusion, Make(cls, true, text, "auto", "auto", "auto", "auto", true));
      Check(s.on == on && !s.blocked, "occlusion '" + std::string(text) + "' on " + c);
    }
    // The contact shadows and the sun shafts: `auto` is the class's cell (gpu_class.h): off, low, high.
    const bool auto_on = cls != GpuClass::kLow;
    for (const Effect effect : {Effect::kContactShadows, Effect::kSunShafts}) {
      const std::string name = std::string(Info(effect).choice) + " " + c;
      auto with = [&](const char* text) {
        return effect == Effect::kContactShadows
                   ? Make(cls, true, "high", text, "off", "off", "game", true)
                   : Make(cls, true, "high", "off", text, "off", "game", true);
      };
      Check(StandingOf(effect, with("auto")).on == auto_on, name + ": auto");
      Check(StandingOf(effect, with("off")).on == false, name + ": off");
      Check(StandingOf(effect, with("low")).on == true, name + ": low");
      Check(StandingOf(effect, with("high")).on == true, name + ": high");
      Check(StandingOf(effect, with("HIGH")).on == true, name + ": capitals are read as lower case");
      Check(!StandingOf(effect, with("auto")).blocked, name + ": never blocked");
    }
    // The shafts read a text they do not know as `auto`; the contact shadows as off.
    Check(StandingOf(Effect::kSunShafts, Make(cls, true, "high", "off", "hgih", "off", "game", true)).on == auto_on,
          "shafts " + c + ": an unknown text counts as auto");
    Check(!StandingOf(Effect::kContactShadows, Make(cls, true, "high", "hgih", "off", "off", "game", true)).on,
          "contact " + c + ": an unknown text is off");
    // The shadow filter: auto is the class's cell (game on Low, soft above), game, soft and pcss (which is soft).
    Check(StandingOf(Effect::kShadowFilter, Make(cls, true, "off", "off", "off", "off", "auto", true)).on == auto_on,
          "filter " + c + ": auto");
    Check(!StandingOf(Effect::kShadowFilter, Make(cls, true, "off", "off", "off", "off", "game", true)).on,
          "filter " + c + ": game");
    Check(StandingOf(Effect::kShadowFilter, Make(cls, true, "off", "off", "off", "off", "soft", true)).on,
          "filter " + c + ": soft");
    Check(StandingOf(Effect::kShadowFilter, Make(cls, true, "off", "off", "off", "off", "pcss", true)).on,
          "filter " + c + ": pcss is soft");
    Check(StandingOf(Effect::kShadowFilter, Make(cls, true, "off", "off", "off", "off", "SOFT", true)).on,
          "filter " + c + ": SOFT");
    // The bloom fix is the switch.
    Check(StandingOf(Effect::kBloomFix, Make(cls, true, "off", "off", "off", "off", "game", true)).on, "bloom on");
    Check(!StandingOf(Effect::kBloomFix, Make(cls, true, "off", "off", "off", "off", "game", false)).on, "bloom off");
    // The bounce light: off on every class for `auto` (gpu_class.h), by hand low or high; it needs the occlusion.
    Check(!StandingOf(Effect::kBounceLight, Make(cls, true, "high", "off", "off", "auto", "game", true)).on,
          "bounce " + c + ": auto is off");
    for (const char* text : {"low", "high"}) {
      const Standing with_ao = StandingOf(Effect::kBounceLight, Make(cls, true, "high", "off", "off", text, "game", true));
      Check(with_ao.on && !with_ao.blocked, "bounce " + c + " " + text + " with the occlusion");
      const Standing without = StandingOf(Effect::kBounceLight, Make(cls, true, "off", "off", "off", text, "game", true));
      Check(!without.on && without.blocked, "bounce " + c + " " + text + " without the occlusion is held and blocked");
    }
    Check(!StandingOf(Effect::kBounceLight, Make(cls, true, "high", "off", "off", "off", "game", true)).on,
          "bounce " + c + ": off");
    const Standing none = StandingOf(Effect::kBounceLight, Make(cls, true, "off", "off", "off", "off", "game", true));
    Check(!none.on && none.blocked, "bounce " + c + ": off with the occlusion off is blocked too");
  }
  // A class that is not known acts as Low for the shadow filter and the contact shadows (gpu_class::Known()).
  Check(!StandingOf(Effect::kShadowFilter, Make(GpuClass::kHigh, false, "off", "off", "off", "off", "auto", true)).on,
        "filter: auto on an unknown class acts as Low (game)");
  Check(!StandingOf(Effect::kShadowFilter, Make(GpuClass::kHigh, false, "off", "off", "off", "off", "game", true)).on,
        "filter: game on an unknown class");
  Check(StandingOf(Effect::kShadowFilter, Make(GpuClass::kHigh, false, "off", "off", "off", "off", "soft", true)).on,
        "filter: soft on an unknown class");
  Check(!StandingOf(Effect::kBounceLight, Make(GpuClass::kHigh, false, "high", "off", "off", "auto", "game", true)).on,
        "bounce: auto on an unknown class is off");
  Check(!StandingOf(Effect::kNone, Make(GpuClass::kHigh, true, "high", "high", "high", "high", "soft", true)).on,
        "no effect is never on");
}

// ---- the text a flip forces on is an on value of its setting, on every class ------------------------------

void TestForcedOnText() {
  for (const GpuClass cls : kClasses) {
    const std::string c = ClassWord(cls);
    for (const bool known : {true, false}) {
      const std::string on_text_contact(Info(Effect::kContactShadows).on_text);
      Check(hp::Resolve(hp::Pass::kContact, cls, known, on_text_contact).level != hp::Level::kOff, "contact forced-on text is on " + c);
      Check(hp::Resolve(hp::Pass::kShafts, cls, known, std::string(Info(Effect::kSunShafts).on_text)).level != hp::Level::kOff,
            "shafts forced-on text is on " + c);
      Check(hp::Resolve(hp::Pass::kBounce, cls, known, std::string(Info(Effect::kBounceLight).on_text)).level != hp::Level::kOff,
            "bounce forced-on text is on " + c);
      Check(hp::Resolve(hp::Pass::kOcclusion, cls, known, std::string(Info(Effect::kAmbientOcclusion).on_text)).level != hp::Level::kOff,
            "occlusion forced-on text is on " + c);
    }
    Check(sf::ModeFor(cls, sf::ParseChoice(std::string(Info(Effect::kShadowFilter).on_text))) == sf::Mode::kSoft,
          "filter forced-on text is soft " + c);
    // And `off` is off for each of them (what a flip forces off is read as the word `off`).
    Check(hp::Resolve(hp::Pass::kContact, cls, true, "off").level == hp::Level::kOff, "contact off is off");
    Check(hp::Resolve(hp::Pass::kShafts, cls, true, "off").level == hp::Level::kOff, "shafts off is off");
    Check(hp::Resolve(hp::Pass::kBounce, cls, true, "off").level == hp::Level::kOff, "bounce off is off");
    Check(hp::Resolve(hp::Pass::kOcclusion, cls, true, "off").level == hp::Level::kOff, "occlusion off is off");
    Check(sf::ModeFor(cls, sf::ParseChoice("game")) == sf::Mode::kGame, "filter game is the game's");
  }
}

// ---- a press ---------------------------------------------------------------------------------------------

void TestPress() {
  for (const GpuClass cls : kClasses) {
    for (int i = 0; i < kEffectCount; ++i) {
      const Effect effect = static_cast<Effect>(i);
      for (const bool saved_on : {true, false}) {
        // Settings that make the effect on, or off, whatever it is.
        // (the occlusion is what the bounce light needs, so it stays on unless the occlusion is the effect)
        const Saved saved = saved_on ? Make(cls, true, "high", "high", "high", "high", "soft", true)
                                     : Make(cls, true, effect == Effect::kAmbientOcclusion ? "off" : "high", "off", "off", "off",
                                            "game", false);
        const std::string name = std::string(Info(effect).choice) + (saved_on ? " saved on " : " saved off ") + ClassWord(cls);
        Check(StandingOf(effect, saved).on == saved_on, name + ": the settings are what the test says");
        const Press first = PressOf(effect, Force::kNone, saved);
        Check(!first.blocked, name + ": not blocked");
        Check(first.force == (saved_on ? Force::kOff : Force::kOn), name + ": the first press goes away from the saved setting");
        Check(first.shown_on == !saved_on, name + ": the first press shows the opposite");
        const Press second = PressOf(effect, first.force, saved);
        Check(second.force == Force::kNone, name + ": the second press puts the saved setting back");
        Check(second.shown_on == saved_on, name + ": the second press shows the saved setting");
        const Press third = PressOf(effect, second.force, saved);
        Check(third.force == first.force && third.shown_on == first.shown_on, name + ": the third press is the first again");
      }
    }
    // The bounce light with the occlusion off: nothing to show, the flip is left as it was.
    const Saved held = Make(cls, true, "off", "off", "off", "high", "game", true);
    const Press blocked = PressOf(Effect::kBounceLight, Force::kNone, held);
    Check(blocked.blocked && blocked.force == Force::kNone && !blocked.shown_on, std::string("bounce blocked while the occlusion is off ") + ClassWord(cls));
    const Press kept = PressOf(Effect::kBounceLight, Force::kOn, held);
    Check(kept.blocked && kept.force == Force::kOn, "a blocked press leaves the flip as it was");
    // The occlusion itself is never blocked, also with the bounce light on.
    const Press ao = PressOf(Effect::kAmbientOcclusion, Force::kNone, Make(cls, true, "high", "off", "off", "high", "game", true));
    Check(!ao.blocked && ao.force == Force::kOff, "the occlusion flips off");
  }
}

// ---- the label -------------------------------------------------------------------------------------------

void TestLabel() {
  CheckEq(LabelText(Effect::kBounceLight, false, false), "BOUNCE LIGHT: OFF", "bounce light off");
  CheckEq(LabelText(Effect::kBounceLight, true, false), "BOUNCE LIGHT: ON", "bounce light on");
  CheckEq(LabelText(Effect::kContactShadows, false, false), "CONTACT SHADOWS: OFF", "contact shadows off");
  CheckEq(LabelText(Effect::kSunShafts, true, false), "SUN SHAFTS: ON", "sun shafts on");
  CheckEq(LabelText(Effect::kAmbientOcclusion, false, false), "AMBIENT OCCLUSION: OFF", "occlusion off");
  CheckEq(LabelText(Effect::kShadowFilter, true, false), "SHADOW FILTER: ON", "shadow filter on");
  CheckEq(LabelText(Effect::kBloomFix, false, false), "BLOOM FIX: OFF", "bloom fix off");
  CheckEq(LabelText(Effect::kBounceLight, false, true), "BOUNCE LIGHT NEEDS AMBIENT OCCLUSION", "blocked bounce light");
  CheckEq(LabelText(Effect::kNone, true, false), "", "no effect, no label");
  // The two parts the panel draws.
  const Label on = LabelOf(Effect::kContactShadows, true, false);
  CheckEq(on.name, "CONTACT SHADOWS", "label name");
  CheckEq(on.state, "ON", "label state on");
  Check(on.good, "on is drawn in the good colour");
  const Label off = LabelOf(Effect::kContactShadows, false, false);
  CheckEq(off.state, "OFF", "label state off");
  Check(!off.good, "off is not drawn in the good colour");
  const Label blocked = LabelOf(Effect::kBounceLight, true, true);
  CheckEq(blocked.state, "NEEDS AMBIENT OCCLUSION", "label state blocked");
  Check(!blocked.good, "blocked is not good, whatever shown_on says");
  Check(LabelOf(Effect::kNone, true, false).name.empty(), "no effect has no label name");
}

// ---- how often, and how the label fades ------------------------------------------------------------------

void TestTiming() {
  Check(AcceptPress(1000, -1), "the first press counts");
  Check(AcceptPress(0, -1), "the first press counts at time 0");
  Check(!AcceptPress(1100, 1000), "a press 100 ms after the last is dropped");
  Check(!AcceptPress(1249, 1000), "249 ms is dropped");
  Check(AcceptPress(1250, 1000), "250 ms counts");
  Check(AcceptPress(5000, 1000), "a press seconds later counts");
  const float hold = kLabelHoldDefault;
  Check(LabelAlpha(-0.1f, hold) == 0.0f, "no label before the press");
  Check(LabelAlpha(0.0f, hold) == 0.0f, "the label starts from nothing");
  Check(LabelAlpha(kLabelFadeIn * 0.5f, hold) > 0.4f && LabelAlpha(kLabelFadeIn * 0.5f, hold) < 0.6f, "half way in");
  Check(LabelAlpha(kLabelFadeIn, hold) == 1.0f, "fully in");
  Check(LabelAlpha(0.5f, hold) == 1.0f, "held");
  Check(LabelAlpha(hold, hold) == 1.0f, "held until the hold time");
  const float mid = LabelAlpha(hold + kLabelFadeOut * 0.5f, hold);
  Check(mid > 0.4f && mid < 0.6f, "half way out");
  Check(LabelAlpha(hold + kLabelFadeOut, hold) == 0.0f, "gone");
  Check(LabelAlpha(10.0f, hold) == 0.0f, "long gone");
  // A long hold (the chain's, for a picture) keeps it up.
  Check(LabelAlpha(5.0f, 6.0f) == 1.0f, "a 6 s hold is still up at 5 s");
  // The alpha never leaves 0..1 and never rises while it fades out.
  float last = 1.0f;
  bool ok = true;
  for (float t = hold; t < hold + kLabelFadeOut + 0.1f; t += 0.01f) {
    const float a = LabelAlpha(t, hold);
    ok = ok && a >= 0.0f && a <= 1.0f && a <= last + 1e-6f;
    last = a;
  }
  Check(ok, "the fade out is monotone and inside 0..1");
}

}  // namespace

int main() {
  TestTable();
  TestState();
  TestStanding();
  TestForcedOnText();
  TestPress();
  TestLabel();
  TestTiming();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
