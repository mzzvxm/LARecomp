// The two-button box (choice_box.h): the Windows dialog, built from measured sizes (choice_box_layout.h).
#include "choice_box.h"

#include <algorithm>
#include <string>
#include <vector>

#include "choice_box_layout.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace mcla::gpu::fallback {

#if defined(_WIN32)

namespace {

constexpr int kIdAction = IDOK;
constexpr int kIdWayOut = IDCANCEL;

std::wstring Wide(const std::string& utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (count <= 0) {
    return std::wstring(utf8.begin(), utf8.end());
  }
  std::wstring out(static_cast<size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), count);
  return out;
}

// The functions of Windows 10 1607 and later are looked up, so the exe starts (and the box falls back to a plain layout at the
// system's DPI) where they are missing.
struct DpiApi {
  UINT(WINAPI* get_dpi_for_window)(HWND) = nullptr;
  BOOL(WINAPI* system_parameters_info_for_dpi)(UINT, UINT, PVOID, UINT, UINT) = nullptr;
  BOOL(WINAPI* adjust_window_rect_ex_for_dpi)(LPRECT, DWORD, BOOL, DWORD, UINT) = nullptr;
  int(WINAPI* get_system_metrics_for_dpi)(int, UINT) = nullptr;
};

const DpiApi& Api() {
  static const DpiApi api = [] {
    DpiApi a;
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
      const auto find = [user32](const char* name) { return reinterpret_cast<void*>(GetProcAddress(user32, name)); };
      a.get_dpi_for_window = reinterpret_cast<decltype(a.get_dpi_for_window)>(find("GetDpiForWindow"));
      a.system_parameters_info_for_dpi = reinterpret_cast<decltype(a.system_parameters_info_for_dpi)>(find("SystemParametersInfoForDpi"));
      a.adjust_window_rect_ex_for_dpi = reinterpret_cast<decltype(a.adjust_window_rect_ex_for_dpi)>(find("AdjustWindowRectExForDpi"));
      a.get_system_metrics_for_dpi = reinterpret_cast<decltype(a.get_system_metrics_for_dpi)>(find("GetSystemMetricsForDpi"));
    }
    return a;
  }();
  return api;
}

struct State {
  const TwoButtonBox* box = nullptr;
  std::wstring title, body, action, way_out;
  HFONT font = nullptr;
  HICON icon = nullptr;
  BoxLayout layout;
  int result = 0;  // IDOK, IDCANCEL, or 0 when the dialog could not be built
};

UINT DpiOf(HWND window) {
  const DpiApi& api = Api();
  if (api.get_dpi_for_window) {
    if (const UINT dpi = api.get_dpi_for_window(window)) {
      return dpi;
    }
  }
  UINT dpi = 96;
  if (HDC screen = GetDC(nullptr)) {
    dpi = static_cast<UINT>(GetDeviceCaps(screen, LOGPIXELSX));
    ReleaseDC(nullptr, screen);
  }
  return dpi ? dpi : 96;
}

// The font Windows' own message boxes use, at this DPI.
HFONT MessageFont(UINT dpi) {
  NONCLIENTMETRICSW metrics{};
  metrics.cbSize = sizeof(metrics);
  const DpiApi& api = Api();
  const bool got = api.system_parameters_info_for_dpi
                       ? api.system_parameters_info_for_dpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi) != FALSE
                       : SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) != FALSE;
  if (!got) {
    return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));  // stock objects are never deleted
  }
  return CreateFontIndirectW(&metrics.lfMessageFont);
}

HICON IconFor(BoxIcon kind, int edge) {
  // The system's icons by number (IDI_ERROR, IDI_WARNING, IDI_INFORMATION are the ANSI names without UNICODE).
  LPCWSTR id = MAKEINTRESOURCEW(32516);
  switch (kind) {
    case BoxIcon::kError:
      id = MAKEINTRESOURCEW(32513);
      break;
    case BoxIcon::kWarning:
      id = MAKEINTRESOURCEW(32515);
      break;
    case BoxIcon::kInfo:
      break;
  }
  return static_cast<HICON>(LoadImageW(nullptr, id, IMAGE_ICON, edge, edge, LR_SHARED));
}

// The monitor's work area the box is centred in: the primary one (the box is created there, so its DPI is that monitor's).
RECT PrimaryWorkArea() {
  RECT work{0, 0, 1280, 720};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  return work;
}

// Measures, builds the controls and sizes the box. False when a control could not be made.
bool Build(HWND dlg, State& st) {
  const UINT dpi = DpiOf(dlg);
  const int dpi_i = static_cast<int>(dpi);
  st.font = MessageFont(dpi);
  const DpiApi& api = Api();
  const int icon_edge = api.get_system_metrics_for_dpi ? api.get_system_metrics_for_dpi(SM_CXICON, dpi) : Dip(32, dpi_i);
  st.icon = IconFor(st.box->icon, icon_edge);

  const RECT work = PrimaryWorkArea();
  BoxMeasure measure;
  measure.dpi = dpi_i;
  measure.icon = st.icon ? icon_edge : 0;

  if (HDC dc = GetDC(dlg)) {
    HGDIOBJ old_font = SelectObject(dc, st.font);
    RECT text{0, 0, MaxBodyWidth(dpi_i, static_cast<int>(work.right - work.left), measure.icon), 0};
    DrawTextW(dc, st.body.c_str(), -1, &text, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS);
    measure.body_w = static_cast<int>(text.right - text.left) + 2;  // two pixels for the glyph overhang
    measure.body_h = static_cast<int>(text.bottom - text.top) + 2;
    for (const std::wstring* label : {&st.action, &st.way_out}) {
      SIZE size{};
      GetTextExtentPoint32W(dc, label->c_str(), static_cast<int>(label->size()), &size);
      measure.label_w.push_back(size.cx);
      measure.label_h = std::max(measure.label_h, static_cast<int>(size.cy));
    }
    SelectObject(dc, old_font);
    ReleaseDC(dlg, dc);
  } else {
    return false;
  }
  st.layout = PlanBox(measure);

  const HINSTANCE instance = GetModuleHandleW(nullptr);
  const auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, const BoxRect& r, int id) {
    HWND child = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, r.x, r.y, r.w, r.h, dlg,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    if (child) {
      SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(st.font), FALSE);
    }
    return child;
  };
  bool ok = true;
  if (st.icon) {
    HWND icon = make(L"STATIC", L"", SS_ICON, st.layout.icon, -1);
    ok = ok && icon != nullptr;
    if (icon) {
      SendMessageW(icon, STM_SETICON, reinterpret_cast<WPARAM>(st.icon), 0);
    }
  }
  ok = ok && make(L"STATIC", st.body.c_str(), SS_LEFT | SS_NOPREFIX, st.layout.body, -1) != nullptr;
  HWND action = make(L"BUTTON", st.action.c_str(), WS_TABSTOP | BS_DEFPUSHBUTTON, st.layout.buttons[0], kIdAction);
  HWND way_out = make(L"BUTTON", st.way_out.c_str(), WS_TABSTOP | BS_PUSHBUTTON, st.layout.buttons[1], kIdWayOut);
  if (!ok || !action || !way_out) {
    return false;
  }
  SendMessageW(dlg, DM_SETDEFID, kIdAction, 0);  // DM_GETDEFID (and Enter) say the first button

  // The window around that client area, centred on the primary monitor's work area.
  const LONG_PTR style = GetWindowLongPtrW(dlg, GWL_STYLE);
  const LONG_PTR ex_style = GetWindowLongPtrW(dlg, GWL_EXSTYLE);
  RECT frame{0, 0, st.layout.client_w, st.layout.client_h};
  if (api.adjust_window_rect_ex_for_dpi) {
    api.adjust_window_rect_ex_for_dpi(&frame, static_cast<DWORD>(style), FALSE, static_cast<DWORD>(ex_style), dpi);
  } else {
    AdjustWindowRectEx(&frame, static_cast<DWORD>(style), FALSE, static_cast<DWORD>(ex_style));
  }
  const int width = static_cast<int>(frame.right - frame.left);
  const int height = static_cast<int>(frame.bottom - frame.top);
  const int x = static_cast<int>(work.left) + (static_cast<int>(work.right - work.left) - width) / 2;
  const int y = static_cast<int>(work.top) + (static_cast<int>(work.bottom - work.top) - height) / 2;
  SetWindowPos(dlg, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
  SetWindowTextW(dlg, st.title.c_str());
  SetFocus(action);
  return true;
}

INT_PTR CALLBACK Proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  State* st = reinterpret_cast<State*>(GetWindowLongPtrW(dlg, DWLP_USER));
  switch (msg) {
    case WM_INITDIALOG: {
      st = reinterpret_cast<State*>(lp);
      SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(st));
      if (!Build(dlg, *st)) {
        st->result = 0;
        EndDialog(dlg, 0);
        return TRUE;
      }
      if (st->box->on_ready) {
        st->box->on_ready(dlg);
      }
      return FALSE;  // the focus is set (the first button)
    }
    case WM_COMMAND:
      if (st && HIWORD(wp) == BN_CLICKED && (LOWORD(wp) == kIdAction || LOWORD(wp) == kIdWayOut)) {
        st->result = LOWORD(wp);
        EndDialog(dlg, st->result);
        return TRUE;
      }
      break;
    case WM_CLOSE:  // the X, Alt+F4, Esc without a Close button: the way out
      if (st) {
        st->result = kIdWayOut;
      }
      EndDialog(dlg, kIdWayOut);
      return TRUE;
    case WM_ERASEBKGND:  // as Windows' own boxes: a white block for the text, the button face colour for the strip
      if (st) {
        const HDC dc = reinterpret_cast<HDC>(wp);
        RECT client{};
        GetClientRect(dlg, &client);
        FillRect(dc, &client, GetSysColorBrush(COLOR_3DFACE));
        RECT upper{0, 0, client.right, st->layout.text_area_h};
        FillRect(dc, &upper, GetSysColorBrush(COLOR_WINDOW));
        SetWindowLongPtrW(dlg, DWLP_MSGRESULT, TRUE);
        return TRUE;
      }
      break;
    case WM_CTLCOLORSTATIC: {
      const HDC dc = reinterpret_cast<HDC>(wp);
      SetBkColor(dc, GetSysColor(COLOR_WINDOW));
      SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
      return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_WINDOW));
    }
    default:
      break;
  }
  return FALSE;
}

struct MinimizedWindow {
  HWND hwnd;
  bool own_thread;
};

BOOL CALLBACK MinimizeProcessWindow(HWND hwnd, LPARAM param) {
  DWORD pid = 0;
  const DWORD thread_id = GetWindowThreadProcessId(hwnd, &pid);
  if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && !IsIconic(hwnd) && ::GetWindow(hwnd, GW_OWNER) == nullptr) {
    const bool own_thread = thread_id == GetCurrentThreadId();
    if (own_thread) {
      ShowWindow(hwnd, SW_FORCEMINIMIZE);
    } else {
      ShowWindowAsync(hwnd, SW_FORCEMINIMIZE);
    }
    reinterpret_cast<std::vector<MinimizedWindow>*>(param)->push_back({hwnd, own_thread});
  }
  return TRUE;
}

}  // namespace

std::optional<int> ShowTwoButtonBox(const TwoButtonBox& box) {
  State st;
  st.box = &box;
  st.title = Wide(box.title);
  st.body = Wide(box.body);
  st.action = Wide(box.action);
  st.way_out = Wide(box.way_out);

  // An empty dialog: the title bar, the X and the frame. The controls are made in WM_INITDIALOG, from measured sizes.
  // The header is followed by a menu, a class and a title (each one WORD: none, the default class, none).
  struct alignas(4) Template {
    DLGTEMPLATE header;
    WORD menu;
    WORD window_class;
    WORD title;
  } tmpl{};
  tmpl.header.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | (box.show ? WS_VISIBLE : 0u);
  tmpl.header.dwExtendedStyle = box.show ? WS_EX_TOPMOST : 0u;
  tmpl.header.cx = 100;
  tmpl.header.cy = 50;

  std::vector<MinimizedWindow> minimized;
  if (box.show) {
    EnumWindows(MinimizeProcessWindow, reinterpret_cast<LPARAM>(&minimized));
  }
  const INT_PTR shown = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &tmpl.header, nullptr, Proc, reinterpret_cast<LPARAM>(&st));
  for (const MinimizedWindow& window : minimized) {
    if (window.own_thread) {
      ShowWindow(window.hwnd, SW_RESTORE);
    } else {
      ShowWindowAsync(window.hwnd, SW_RESTORE);
    }
  }
  if (st.font && st.font != GetStockObject(DEFAULT_GUI_FONT)) {
    DeleteObject(st.font);
  }
  if (shown == -1 || st.result == 0) {
    return std::nullopt;
  }
  return st.result == kIdAction ? 0 : 1;
}

#else

// Not Windows: the SDK's own box does the job.
std::optional<int> ShowTwoButtonBox(const TwoButtonBox&) { return std::nullopt; }

#endif

}  // namespace mcla::gpu::fallback
