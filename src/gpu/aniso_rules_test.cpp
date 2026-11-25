// Check of the anisotropic filtering rules (aniso_rules.h): what mcla_gpu_anisotropy says and which of the game's samplers take
// it. The setting is live (Samplers::FrameSettingChanged reads it once a frame; a change makes the scene forget its memoized
// sampler slots, and the next draw makes samplers for the new level). A program of its own: it needs no renderer, no ReXGlue and
// no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_aniso_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_aniso_rules_test
//
// The expected values are written out here again on purpose: the fetch constant's AnisoFilter values (0 off, 1 the game's 1:1, 2 to
// 5 for 2x to 16x) are the other side of the check, so a change in the header alone fails it.

#include <cstdint>
#include <cstdio>
#include <string>

#include "aniso_rules.h"

using namespace mcla::gpu::aniso;

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

void CheckN(int64_t got, int64_t want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::printf("FAILED: %s: got %lld, want %lld\n", what.c_str(), (long long)got, (long long)want);
  }
}

void TheWords() {
  struct Case {
    const char* text;
    int32_t level;
  };
  const Case cases[] = {
      {"game", -1},  {"", -1},      {"auto", -1},  {"3", -1},     {"32", -1},   {"x", -1},   {"-1", -1},   {"high", -1},
      {"off", 0},    {"OFF", 0},    {" Off ", 0},  {"0", 0},      {"1", 0},     {"1x", 0},
      {"2", 2},      {"2x", 2},     {"2X", 2},     {" 2 x ", 2},
      {"4", 3},      {"4x", 3},     {"8", 4},      {"8x", 4},     {"16", 5},    {"16x", 5},    {"16X", 5},   {"  16 ", 5},
  };
  for (const Case& c : cases) {
    CheckN(ParseText(c.text), c.level, std::string("the level of '") + c.text + "'");
  }
  // The names the log prints for each level, and back: every level's name parses to itself.
  const int32_t levels[] = {kGame, kDisabled, kMax2, kMax4, kMax8, kMax16};
  const char* names[] = {"game", "off", "2x", "4x", "8x", "16x"};
  for (size_t i = 0; i < 6; ++i) {
    Check(std::string(Name(levels[i])) == names[i], std::string("the name of level ") + std::to_string(levels[i]));
    CheckN(ParseText(Name(levels[i])), levels[i], std::string("'") + names[i] + "' parses back to its level");
  }
  Check(std::string(Name(kMax1)) == "?", "the game's 1:1 has no setting word");
  Check(std::string(Name(7)) == "?", "a level that is none is '?'");
  CheckN(kDisabled, 0, "off is the fetch constant's 0");
  CheckN(kMax1, 1, "1:1 is the fetch constant's 1");
  CheckN(kMax16, 5, "16x is the fetch constant's 5");
}

void TheEligibleSamplers() {
  //                       mip_min mip_max base_map mag   min   mip
  Check(Eligible(0, 10, false, true, true, true), "a trilinear sampler with a mip chain takes the setting");
  Check(Eligible(2, 3, false, true, true, true), "a chain of two levels is enough");
  Check(!Eligible(0, 0, false, true, true, true), "one level: nothing to filter between");
  Check(!Eligible(5, 5, false, true, true, true), "a range of one level in the middle of the chain too");
  Check(!Eligible(5, 3, false, true, true, true), "an inverted range");
  Check(!Eligible(0, 10, true, true, true, true), "base map only (the HUD's lookups, a lookup of the base level)");
  Check(!Eligible(0, 10, false, false, true, true), "a point-sampled magnification (a deliberate nearest) stays as the game made it");
  Check(!Eligible(0, 10, false, true, false, true), "a point-sampled minification stays");
  Check(!Eligible(0, 10, false, true, true, false), "a nearest mip filter (bilinear, not trilinear) stays");
  Check(!Eligible(0, 10, false, false, false, false), "a fully nearest sampler stays");
}

void TheEffectiveLevel() {
  // The setting's level for an eligible sampler; the game's own for every other, and for `game`.
  CheckN(Effective(kGame, true, 0), 0, "game: the game's off stays off");
  CheckN(Effective(kGame, true, 3), 3, "game: the game's 4x stays 4x");
  CheckN(Effective(kGame, false, 5), 5, "game, not eligible: the game's level");
  CheckN(Effective(kMax16, true, 0), 5, "16x over a trilinear sampler the game made without anisotropy");
  CheckN(Effective(kMax16, true, 2), 5, "16x over a game's 2x");
  CheckN(Effective(kMax2, true, 5), 2, "2x over a game's 16x: the setting is what is asked for, it can lower too");
  CheckN(Effective(kDisabled, true, 5), 0, "off over a game's 16x: off");
  CheckN(Effective(kMax16, false, 0), 0, "16x does not reach a sampler that is not eligible");
  CheckN(Effective(kMax16, false, 2), 2, "... and leaves its own level");
  CheckN(Effective(kDisabled, false, 3), 3, "off does not reach it either");
}

void TheHostsMaximum() {
  CheckN(MaxAnisotropy(0), 1, "off: 1");
  CheckN(MaxAnisotropy(1), 1, "1:1: 1");
  CheckN(MaxAnisotropy(2), 2, "2x: 2");
  CheckN(MaxAnisotropy(3), 4, "4x: 4");
  CheckN(MaxAnisotropy(4), 8, "8x: 8");
  CheckN(MaxAnisotropy(5), 16, "16x: 16");
}

void ALiveChange() {
  // The row steps through the words with no state between: the answer for a sampler depends only on the word read now, so the
  // frame after a change makes the samplers of the new level (the description's key holds the level).
  const char* steps[] = {"game", "16", "off", "4x", "game"};
  const uint32_t expect[] = {2, 5, 0, 3, 2};  // for a trilinear sampler the game made 2x
  for (size_t i = 0; i < 5; ++i) {
    const int32_t o = ParseText(steps[i]);
    CheckN(Effective(o, true, 2), expect[i], std::string("a game 2x sampler under '") + steps[i] + "'");
  }
  // A change is seen as a change of the parsed level, not of the word: "4" and "4x" are one setting.
  Check(ParseText("4") == ParseText("4x"), "4 and 4x are the same level: no change is reported");
  Check(ParseText("garbage") == ParseText("game"), "a word that is no level is the game's: no change from `game`");
}

}  // namespace

int main() {
  TheWords();
  TheEligibleSamplers();
  TheEffectiveLevel();
  TheHostsMaximum();
  ALiveChange();
  if (g_failed != 0) {
    std::printf("aniso_rules_test: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("aniso_rules_test: %d checks passed\n", g_checks);
  return 0;
}
