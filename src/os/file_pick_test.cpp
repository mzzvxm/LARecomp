// Standalone check of the file chooser answers and the typed path fallback (os/file_pick.h): what the SDK's dialog gave becomes a file, a cancel or "no
// chooser"; the sentence over the path box; and what a typed or pasted path means (quotes, a file:// address, ~, a shell's backslashes) and whether it
// names a file. No game, no SDK, no GTK, no window.
//
//   cmake --build <dir> --target larecomp_file_pick_test, then run it (exit 0 = passed).
//   Or by hand:  clang++ -std=c++23 -I src src/os/file_pick_test.cpp src/os/file_pick.cpp

#include "os/file_pick.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define LARECOMP_GETPID _getpid
#else
#include <unistd.h>
#define LARECOMP_GETPID getpid
#endif

namespace os = larecomp::os;
namespace fs = std::filesystem;

namespace {

int g_failed = 0;
int g_checked = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    ++g_checked;                                                   \
    if (!(cond)) {                                                 \
      ++g_failed;                                                  \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
    }                                                              \
  } while (0)

bool Contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

std::string Utf8(const fs::path& path) {
  const std::u8string u8 = path.u8string();
  return std::string(u8.begin(), u8.end());
}

void TestDialogAnswer() {
  // The dialog could not open: no chooser, with the system's own words.
  {
    const os::FilePick pick = os::FromDialogAnswer(false, {}, "no portal answered and zenity is not installed");
    CHECK(pick.unavailable == "no portal answered and zenity is not installed");
    CHECK(pick.path.empty());
    CHECK(!pick.cancelled);
  }
  // No message at all still says something (the wizard puts it in a sentence).
  {
    const os::FilePick pick = os::FromDialogAnswer(false, {}, "");
    CHECK(!pick.unavailable.empty());
    CHECK(!pick.cancelled);
    CHECK(pick.path.empty());
  }
  // It opened and the player closed it: a cancel, never "no chooser" (the wizard must not offer a path box for a cancel).
  {
    const os::FilePick pick = os::FromDialogAnswer(true, {}, "");
    CHECK(pick.cancelled);
    CHECK(pick.unavailable.empty());
    CHECK(pick.path.empty());
  }
  {
    const os::FilePick pick = os::FromDialogAnswer(true, {fs::path()}, "");  // an empty path is no choice
    CHECK(pick.cancelled);
    CHECK(pick.path.empty());
  }
  // A file: the first of the paths.
  {
    const os::FilePick pick = os::FromDialogAnswer(true, {fs::path("a.iso"), fs::path("b.iso")}, "");
    CHECK(pick.path == fs::path("a.iso"));
    CHECK(!pick.cancelled);
    CHECK(pick.unavailable.empty());
  }
  // The message of an error that was not one (the dialog opened) is not kept.
  {
    const os::FilePick pick = os::FromDialogAnswer(true, {fs::path("a.iso")}, "left over");
    CHECK(pick.unavailable.empty());
    CHECK(pick.path == fs::path("a.iso"));
  }
}

void TestUnavailableText() {
  const std::string text = os::UnavailableText("no portal answered", "the Xbox 360 ISO");
  CHECK(Contains(text, "No file chooser could be opened"));
  CHECK(Contains(text, "(no portal answered)"));
  CHECK(Contains(text, "the Xbox 360 ISO"));
  CHECK(Contains(text, "Type the path"));
  CHECK(text.size() > 4 && text.compare(text.size() - 8, 8, "instead.") == 0);
  // No reason, no empty brackets; a reason with the whitespace of a system message is trimmed.
  const std::string bare = os::UnavailableText("", "the save");
  CHECK(!Contains(bare, "()"));
  CHECK(!Contains(bare, " ("));
  CHECK(Contains(bare, "Type the path of the save below instead."));
  CHECK(Contains(os::UnavailableText("  the portal is gone\n", "x"), "(the portal is gone)"));
}

void TestTypedPath(const fs::path& dir) {
  const fs::path file = dir / "plain.iso";
  const fs::path spaced = dir / "my game.iso";
  const fs::path folder = dir / "folder";
  const std::string home = Utf8(dir);

  const auto ok = [&](const std::string& typed, const fs::path& want) {
    const os::TypedPath parsed = os::ParseTypedPath(typed, home);
    if (!parsed.error.empty() || parsed.path != want) {
      std::printf("  typed '%s': error '%s', path '%s'\n", typed.c_str(), parsed.error.c_str(), Utf8(parsed.path).c_str());
    }
    CHECK(parsed.error.empty());
    CHECK(parsed.path == want);
  };
  const auto fails = [&](const std::string& typed, const std::string& says) {
    const os::TypedPath parsed = os::ParseTypedPath(typed, home);
    if (parsed.error.empty() || !Contains(parsed.error, says)) {
      std::printf("  typed '%s': error '%s' (wanted '%s')\n", typed.c_str(), parsed.error.c_str(), says.c_str());
    }
    CHECK(!parsed.error.empty());
    CHECK(Contains(parsed.error, says));
    CHECK(parsed.path.empty());
  };

  // A plain path, with the whitespace a paste leaves around it.
  ok(Utf8(file), file);
  ok("  " + Utf8(file) + "  ", file);
  ok(Utf8(file) + "\n", file);
  ok("\t" + Utf8(file) + "\r\n", file);
  ok(Utf8(spaced), spaced);
  // One pair of quotes (a path copied from a shell or a file manager's "copy as path").
  ok("'" + Utf8(spaced) + "'", spaced);
  ok("\"" + Utf8(spaced) + "\"", spaced);
  ok("  \"" + Utf8(spaced) + "\"  ", spaced);
  ok("\" " + Utf8(spaced) + " \"", spaced);  // spaces inside the quotes go too
  // Quotes that do not match are part of the text, so the file is not found.
  fails("'" + Utf8(file), "There is no file at");
  fails("'" + Utf8(file) + "\"", "There is no file at");

  // Nothing typed.
  fails("", "Type the path");
  fails("   ", "Type the path");
  fails("\n", "Type the path");
  fails("''", "Type the path");
  // A folder, a missing file.
  fails(Utf8(folder), "is a folder, not a file");
  fails(Utf8(dir / "missing.iso"), "There is no file at");
  fails(Utf8(dir / "missing.iso"), "missing.iso");

#ifndef _WIN32
  // A file:// address, as a file manager gives it; its %20 and %25 are decoded, a % that starts no escape stays.
  ok("file://" + Utf8(file), file);
  ok("file://localhost" + Utf8(file), file);
  ok("file://" + home + "/my%20game.iso", spaced);
  ok("  file://" + home + "/my%20game.iso\n", spaced);
  ok("'file://" + home + "/my%20game.iso'", spaced);
  ok("file://" + home + "/100%25.iso", dir / "100%.iso");
  ok("file://" + home + "/100%.iso", dir / "100%.iso");     // a % that is not an escape
  ok("file://" + home + "/100%zz.iso", dir / "100%zz.iso");  // two characters after it that are not hex
  ok(home + "/100%.iso", dir / "100%.iso");                  // a plain path is never decoded
  ok(home + "/my%20game.iso", dir / "my%20game.iso");
  fails("file://" + home + "/nothing.iso", "There is no file at");
  // A shell's escapes.
  ok(home + "/my\\ game.iso", spaced);
  ok("'" + home + "/my\\ game.iso'", spaced);
  ok(home + "/plain\\.iso", file);
  fails(home + "/missing\\", "There is no file at");  // a backslash at the end stays
  // ~ is the home folder (here the test's folder).
  ok("~/plain.iso", file);
  ok("~/my\\ game.iso", spaced);
  ok("'~/my game.iso'", spaced);
  fails("~", "is a folder, not a file");
  fails("~/nothing.iso", "There is no file at");
  // ~ without a home is a name like any other.
  {
    const os::TypedPath parsed = os::ParseTypedPath("~/plain.iso", "");
    CHECK(!parsed.error.empty());
    CHECK(Contains(parsed.error, "~/plain.iso"));
  }
  // A path with a ~ inside it is not a home.
  fails(home + "/~/plain.iso", "There is no file at");
#endif

  // The error names the path as it was understood, for the player to see what was read.
  {
    const os::TypedPath parsed = os::ParseTypedPath("  '" + Utf8(dir / "gone.iso") + "'  ", home);
    CHECK(Contains(parsed.error, Utf8(dir / "gone.iso")));
    CHECK(!Contains(parsed.error, "'"));
  }
}

}  // namespace

int main() {
  TestDialogAnswer();
  TestUnavailableText();

  // The files the typed path is checked against: a scratch folder with a plain file, one with a space in its name, a folder and (off Windows) one with a %.
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path() / ("larecomp_file_pick_test_" + std::to_string(LARECOMP_GETPID()));
  fs::remove_all(dir, ec);
  fs::create_directories(dir / "folder", ec);
  for (const char* name : {"plain.iso", "my game.iso"
#ifndef _WIN32
                           ,
                           "100%.iso", "100%zz.iso", "my%20game.iso"
#endif
       }) {
    std::ofstream(dir / name) << "x";
  }
  CHECK(!ec);
  TestTypedPath(dir);
  fs::remove_all(dir, ec);

  if (g_failed != 0) {
    std::printf("file_pick_test: %d of %d checks FAILED\n", g_failed, g_checked);
    return 1;
  }
  std::printf("file_pick_test: %d checks passed\n", g_checked);
  return 0;
}
