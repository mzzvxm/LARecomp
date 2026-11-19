// A check for the two-button box of the compatibility renderer offer (gpu/choice_box.h, Windows): it builds the real dialog but never shows it
// (TwoButtonBox::show = false: no window on the screen, no focus taken, no other window touched), looks at its controls from its own message
// loop and presses its buttons by the ways a player or a test tool has: BM_CLICK, Enter, Esc, the X. What it checks:
//   * the buttons are real BUTTON windows, the action first (left), the way out last (right), the action the default (DM_GETDEFID and
//     BS_DEFPUSHBUTTON), none cut: each is at least as wide as its label needs and the label's text extent fits inside it;
//   * the text is there whole, the box holds everything, the buttons do not overlap, all are of one height;
//   * the answers: the action button and Enter give 0, the way-out button, Esc and the X (WM_CLOSE) give 1;
//   * the same at 100 %, and with a long refusal text as in the real box (the box grows, nothing is cut).
// Build (the msvc-target clang of labuild, from the source folder):
//   clang++ -std=c++23 -Isrc/gpu src/gpu/choice_box_test.cpp src/gpu/choice_box.cpp -luser32 -lgdi32 -o choice_box_test.exe
// `--aware` makes the process per-monitor DPI aware first (as the game is). With `--shot <folder>` it also writes the dialog as a picture (<folder>\two-button-box-<n>.bmp, from WM_PRINT: nothing shows on screen).
// Exit 0 = passed. Not Windows: it passes without checking anything.

#define _CRT_SECURE_NO_WARNINGS 1
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "choice_box.h"
#include "choice_box_layout.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <thread>
#endif

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failed;                                                            \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
    }                                                                        \
  } while (0)

}  // namespace

#if defined(_WIN32)

namespace {

using mcla::gpu::fallback::BoxIcon;
using mcla::gpu::fallback::TwoButtonBox;

std::string g_shot_folder;
bool g_dpi_aware = false;
int g_shot_count = 0;

struct Child {
  HWND hwnd = nullptr;
  std::string cls;
  std::wstring text;
  int id = 0;
  LONG style = 0;
  RECT rect{};  // in the dialog's client coordinates
};

std::vector<Child> Children(HWND dlg) {
  std::vector<Child> out;
  for (HWND c = GetWindow(dlg, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
    Child child;
    child.hwnd = c;
    char name[64] = {};
    GetClassNameA(c, name, sizeof(name));
    child.cls = name;
    wchar_t text[2048] = {};
    GetWindowTextW(c, text, 2048);
    child.text = text;
    child.id = GetDlgCtrlID(c);
    child.style = static_cast<LONG>(GetWindowLongPtrW(c, GWL_STYLE));
    GetWindowRect(c, &child.rect);
    MapWindowPoints(nullptr, dlg, reinterpret_cast<POINT*>(&child.rect), 2);
    out.push_back(child);
  }
  return out;
}

// The width of `text` in the control's own font, as the control would draw it on one line.
int TextWidth(HWND control, const std::wstring& text) {
  HDC dc = GetDC(control);
  HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0)));
  SIZE size{};
  GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
  SelectObject(dc, old);
  ReleaseDC(control, dc);
  return size.cx;
}

void WriteBitmap(HWND dlg, const std::string& path) {
  RECT window{};
  GetWindowRect(dlg, &window);
  const int w = static_cast<int>(window.right - window.left), h = static_cast<int>(window.bottom - window.top);
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = w;
  info.bmiHeader.biHeight = -h;  // top-down
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ old = SelectObject(mem, bmp);
  // The whole window as it is drawn: the frame and title, the dialog's own background (WM_ERASEBKGND), then its children (WM_PRINT with
  // PRF_CHILDREN draws them even when the dialog is hidden).
  SendMessageW(dlg, WM_PRINT, reinterpret_cast<WPARAM>(mem), PRF_NONCLIENT | PRF_CLIENT | PRF_ERASEBKGND | PRF_CHILDREN);
  BITMAPFILEHEADER file{};
  file.bfType = 0x4D42;
  file.bfOffBits = sizeof(file) + sizeof(info.bmiHeader);
  file.bfSize = file.bfOffBits + static_cast<DWORD>(w * h * 4);
  if (FILE* out = std::fopen(path.c_str(), "wb")) {
    BITMAPINFOHEADER header = info.bmiHeader;
    header.biHeight = -h;
    std::fwrite(&file, sizeof(file), 1, out);
    std::fwrite(&header, sizeof(header), 1, out);
    std::fwrite(bits, 1, static_cast<size_t>(w) * static_cast<size_t>(h) * 4, out);
    std::fclose(out);
  }
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
}

enum class How { kClickAction, kClickWayOut, kEnter, kEscape, kClose, kNone };

int g_timeout_s = 10;  // how long Run waits for an answer

TwoButtonBox RefusalBox() {
  TwoButtonBox box;
  box.icon = BoxIcon::kError;
  box.title = "LARecomp";
  box.body =
      "LARecomp could not start on this PC's graphics card.\n\n"
      "Direct3D 12: refused on purpose (mcla_gpu_force_refusal, a test)\n"
      "Vulkan: refused on purpose (mcla_gpu_force_refusal, a test)\n\n"
      "The game needs a graphics card and driver with Direct3D 12 (shader model 6.0 and resource binding tier 2) or Vulkan "
      "(descriptor indexing and buffer device address). Updating the graphics driver may help. The details are in the log.\n\n"
      "The compatibility renderer runs on more graphics cards, with fewer graphics options. Start the game with it now?";
  box.action = "Use the compatibility renderer";
  box.way_out = "Close";
  return box;
}

TwoButtonBox UnfinishedBox() {
  TwoButtonBox box;
  box.icon = BoxIcon::kWarning;
  box.title = "LARecomp";
  box.body =
      "LARecomp did not finish starting the last 2 times.\n\nThe graphics card or its driver may not agree with the game's renderer. "
      "The compatibility renderer runs on more graphics cards, with fewer graphics options.\n\nStart the game with it now?";
  box.action = "Use the compatibility renderer";
  box.way_out = "Keep trying";
  return box;
}

// Shows `box` (hidden), looks at it, presses it `how` (kNone: leaves it for another process to press). The answer, or -1 when it did not answer within
// g_timeout_s seconds (a watchdog closes it then).
int Run(TwoButtonBox box, How how, const char* label) {
  std::atomic<bool> timed_out{false};
  std::atomic<HWND> dialog{nullptr};
  std::atomic<bool> done{false};
  std::thread watchdog([&] {
    for (int i = 0; i < g_timeout_s * 10 && !done.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!done.load()) {
      timed_out = true;
      PostMessageW(dialog.load(), WM_CLOSE, 0, 0);
    }
  });

  const std::wstring want_action(box.action.begin(), box.action.end());
  const std::wstring want_way_out(box.way_out.begin(), box.way_out.end());
  const std::wstring want_body(box.body.begin(), box.body.end());
  box.show = false;
  box.on_ready = [&](void* handle) {
    HWND dlg = static_cast<HWND>(handle);
    dialog = dlg;
    std::vector<Child> kids = Children(dlg);
    std::vector<Child> buttons, statics;
    for (const Child& c : kids) {
      (c.cls == "Button" ? buttons : statics).push_back(c);
    }
    CHECK(buttons.size() == 2);
    if (buttons.size() == 2) {
      // Z order lists the last-made first: read them as the eye does, left to right.
      if (buttons[0].rect.left > buttons[1].rect.left) {
        std::swap(buttons[0], buttons[1]);
      }
      CHECK(buttons[0].text == want_action);
      CHECK(buttons[1].text == want_way_out);
      CHECK(buttons[0].rect.right <= buttons[1].rect.left);  // side by side, no overlap
      CHECK(buttons[0].rect.top == buttons[1].rect.top);
      CHECK(buttons[0].rect.bottom - buttons[0].rect.top == buttons[1].rect.bottom - buttons[1].rect.top);
      // The default: the first button, by its style and by what the dialog says (DM_GETDEFID: DC_HASDEFID in the high word).
      CHECK((buttons[0].style & BS_TYPEMASK) == BS_DEFPUSHBUTTON);
      CHECK((buttons[1].style & BS_TYPEMASK) == BS_PUSHBUTTON);
      const LRESULT def = SendMessageW(dlg, DM_GETDEFID, 0, 0);
      CHECK(HIWORD(def) == DC_HASDEFID && LOWORD(def) == static_cast<WORD>(buttons[0].id));
      // Nothing is cut: every label fits inside its button with room on both sides.
      for (const Child& b : buttons) {
        RECT own{};
        GetClientRect(b.hwnd, &own);
        const int width = TextWidth(b.hwnd, b.text);
        CHECK(own.right - own.left >= width + 16);
        CHECK(own.bottom - own.top >= 20);
      }
      CHECK(GetDlgItem(dlg, IDOK) == buttons[0].hwnd);
      CHECK(GetDlgItem(dlg, IDCANCEL) == buttons[1].hwnd);
      RECT client{};
      GetClientRect(dlg, &client);
      CHECK(buttons[0].rect.left >= 0);
      CHECK(buttons[1].rect.right <= client.right);
      CHECK(buttons[1].rect.bottom <= client.bottom);
      // The first button is the far left of the pair and the pair sits at the right edge, as Windows' own boxes do.
      CHECK(buttons[1].rect.right >= client.right - 24);
    }
    // The text: one STATIC window with the whole body (no cut: its box is as tall as the wrapped text needs).
    bool body_found = false;
    for (const Child& s : statics) {
      if (s.text == want_body) {
        body_found = true;
        RECT own{};
        GetClientRect(s.hwnd, &own);
        HDC dc = GetDC(s.hwnd);
        HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(s.hwnd, WM_GETFONT, 0, 0)));
        RECT need = own;
        need.bottom = 0;
        DrawTextW(dc, s.text.c_str(), -1, &need, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS);
        SelectObject(dc, old);
        ReleaseDC(s.hwnd, dc);
        CHECK(own.bottom >= need.bottom);
        CHECK(s.rect.right <= [&] { RECT c{}; GetClientRect(dlg, &c); return c.right; }());
      }
    }
    CHECK(body_found);
    CHECK(statics.size() == 2);  // the icon and the text
    if (!g_shot_folder.empty()) {
      RECT client{};
      GetClientRect(dlg, &client);
      RECT outer{};
      GetWindowRect(dlg, &outer);
      std::printf("%s: dpi %u, client %ldx%ld, window %ldx%ld\n", label, GetDpiForWindow(dlg), client.right, client.bottom,
                  outer.right - outer.left, outer.bottom - outer.top);
      for (const Child& c : kids) {
        std::printf("   %-7s id %d  %ld,%ld %ldx%ld  %.30ls\n", c.cls.c_str(), c.id, c.rect.left, c.rect.top, c.rect.right - c.rect.left,
                    c.rect.bottom - c.rect.top, c.text.c_str());
      }
      WriteBitmap(dlg, g_shot_folder + "\\two-button-box-" + std::to_string(++g_shot_count) + "-" + label + ".bmp");
    }

    switch (how) {
      case How::kClickAction:
        PostMessageW(buttons.size() == 2 ? buttons[0].hwnd : dlg, BM_CLICK, 0, 0);
        break;
      case How::kClickWayOut:
        PostMessageW(buttons.size() == 2 ? buttons[1].hwnd : dlg, BM_CLICK, 0, 0);
        break;
      case How::kEnter:
        PostMessageW(dlg, WM_KEYDOWN, VK_RETURN, 0);
        PostMessageW(dlg, WM_KEYUP, VK_RETURN, 0);
        break;
      case How::kEscape:
        PostMessageW(dlg, WM_KEYDOWN, VK_ESCAPE, 0);
        PostMessageW(dlg, WM_KEYUP, VK_ESCAPE, 0);
        break;
      case How::kClose:
        PostMessageW(dlg, WM_CLOSE, 0, 0);
        break;
      case How::kNone:
        std::printf("ready: the box is up (hidden), pid %lu\n", GetCurrentProcessId());
        std::fflush(stdout);
        break;
    }
  };
  const std::optional<int> answer = mcla::gpu::fallback::ShowTwoButtonBox(box);
  done = true;
  watchdog.join();
  CHECK(!timed_out.load());
  CHECK(answer.has_value());
  return answer.value_or(-1);
}

void CheckBoxes() {
  CHECK(Run(RefusalBox(), How::kClickAction, "refusal-action") == 0);
  CHECK(Run(RefusalBox(), How::kClickWayOut, "refusal-close") == 1);
  CHECK(Run(RefusalBox(), How::kEnter, "refusal-enter") == 0);
  CHECK(Run(RefusalBox(), How::kEscape, "refusal-esc") == 1);
  CHECK(Run(RefusalBox(), How::kClose, "refusal-x") == 1);
  CHECK(Run(UnfinishedBox(), How::kClickAction, "unfinished-action") == 0);
  CHECK(Run(UnfinishedBox(), How::kClickWayOut, "unfinished-keep") == 1);
  CHECK(Run(UnfinishedBox(), How::kEnter, "unfinished-enter") == 0);
  CHECK(Run(UnfinishedBox(), How::kEscape, "unfinished-esc") == 1);

  // A short text: the box is still as wide as the two buttons need, and the one line sits beside the icon.
  TwoButtonBox short_text = UnfinishedBox();
  short_text.body = "Start it now?";
  CHECK(Run(short_text, How::kClickAction, "short-action") == 0);
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == "--shot") {
      g_shot_folder = argv[i + 1];
    }
  }
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--aware") {  // as the game is (SDL makes the process per-monitor DPI aware): the box is laid out at the monitor's DPI
      g_dpi_aware = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
      std::printf("per-monitor DPI aware: %s\n", g_dpi_aware ? "yes" : "no");
    }
  }
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == "--serve") {  // one box, hidden, left for another process to read and press (xs_run.py's Dialog, by hand): --serve refusal|unfinished
      g_timeout_s = 40;
      const int answer = Run(std::string(argv[i + 1]) == "unfinished" ? UnfinishedBox() : RefusalBox(), How::kNone, "serve");
      std::printf("answer %d\nchoice box: %d checks, %d failed\n", answer, g_checks, g_failed);
      return g_failed == 0 ? 0 : 1;
    }
  }
  CheckBoxes();
  std::printf("choice box: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

#else

int main() {
  std::printf("choice box: %d checks, %d failed (Windows only)\n", g_checks, g_failed);
  return 0;
}

#endif
