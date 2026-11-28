// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TaskDialogIndirect. comctl32 version 6, Vista.
//
// The message box that took over from MessageBoxW: a caption, an icon, a
// heading, body text, buttons with the caller's own labels, a checkbox. XP's
// comctl32 has no such export in either of its versions, and a program that
// calls it there never gets as far as a dialog -- consumers delay-import
// comctl32, so the first call ends in the delay-load failure hook. For
// Electron that is every dialog.showMessageBox and dialog.showErrorBox, and
// with them the box that reports an uncaught exception.
//
// MessageBoxW cannot stand in. It has a fixed set of button combinations and
// no checkbox, and the callers of this function are the ones that wanted
// something it does not offer. So where the system has no task dialog, this
// builds one: an ordinary modal dialog, made of the button and static
// controls user32 has always had, laid out by measuring the text.
//
// What it is not is a redrawing of Vista's. There is no white panel over a
// grey button strip, a command link is a wide left-aligned button rather than
// an arrow and two sizes of text, and the heading is the message font in bold
// rather than a larger blue one. On XP that is the right look anyway: it is
// what that system's own dialogs look like. The parts that are missing
// outright -- the progress bar, the expander, hyperlinks -- are listed in
// INCOMPLETE_FUNCS.md, and none of them loses the caller its text.
//
// === Finding the system's ===
//
// Not through dispatch.h, for two reasons that both come down to there being
// two comctl32s. Version 5 is the file in the system directory and version 6
// lives in the side-by-side store, chosen for a process whose manifest asks
// for it. Only version 6 has task dialogs, on any Windows.
//
// So LoadSystemLibrary is the wrong tool: it loads by full path from the
// system directory, which is version 5 by construction, and would report the
// function absent on Windows 11. The lookup has to be by bare name, so the
// activation context can answer it -- which is what the delay-load thunk this
// redirect displaces would have done. And the answer is not cached, because
// the activation context is a property of the calling thread at the moment
// of the call, not of the process: a plugin with its own manifest and its
// host can legitimately get different comctl32s. Two loader calls are nothing
// beside putting a modal dialog on screen.
//
// A bare name has a cost, and this is the one place winwrap pays it: comctl32
// is not a known DLL, so in a process with no version 6 manifest the loader
// looks beside the executable before it looks in the system directory, and a
// comctl32.dll planted there would be loaded. The thunk had the same exposure,
// so a consumer is no worse off than without winwrap -- but it is why the
// lookup is not made at all before Vista. No comctl32 XP ever shipped has
// task dialogs, so there the question has one answer and nothing is loaded
// to ask it.
//
// A consequence worth knowing: a program with no version 6 manifest gets
// winwrap's dialog on every Windows, where without winwrap it would have got
// the delay-load failure. The unit test relies on that to exercise this code
// on systems that do have task dialogs.
//
// === user32 and gdi32 ===
//
// Resolved at run time, about forty of them, so that winwrap goes on linking
// against kernel32 and advapi32 alone. Loaded rather than looked up, on the
// reasoning shell32.cc gives for its own DLL: asking for a dialog is asking
// for user32. The table is built once and published with a compare-exchange,
// so two threads opening their first dialogs together both end up with a
// complete one and no lock is involved.

#include <stddef.h> // offsetof

#include "dispatch.h"
#include "public/winwrap_public.h" // GetRealNTVersion
#include "winwrap.h"

namespace {

  // --- The SDK's declarations, restated -------------------------------------
  //
  // commctrl.h hides all of this below NTDDI_VISTA, which is where these
  // sources are compiled. Byte-packed, as the header has them: that is not
  // incidental on x64, where the natural layout would put padding after the
  // first UINT and every pointer after it would be read from the wrong place.

#pragma pack(push, 1)
  struct TaskButton {
    int id;
    PCWSTR text;
  };

  using TaskCallback = HRESULT(CALLBACK*)(HWND, UINT, WPARAM, LPARAM, LONG_PTR);

  struct TaskConfig {
    UINT cbSize;
    HWND hwndParent;
    HINSTANCE hInstance;
    DWORD dwFlags;
    DWORD dwCommonButtons;
    PCWSTR pszWindowTitle;
    union {
      HICON hMainIcon;
      PCWSTR pszMainIcon;
    };
    PCWSTR pszMainInstruction;
    PCWSTR pszContent;
    UINT cButtons;
    const TaskButton* pButtons;
    int nDefaultButton;
    UINT cRadioButtons;
    const TaskButton* pRadioButtons;
    int nDefaultRadioButton;
    PCWSTR pszVerificationText;
    PCWSTR pszExpandedInformation;
    PCWSTR pszExpandedControlText;
    PCWSTR pszCollapsedControlText;
    union {
      HICON hFooterIcon;
      PCWSTR pszFooterIcon;
    };
    PCWSTR pszFooter;
    TaskCallback pfCallback;
    LONG_PTR lpCallbackData;
    UINT cxWidth;
  };
#pragma pack(pop)

  // The sizes the SDK's own structures have. A mismatch here would mean every
  // member after the first was being read from somewhere else.
#if defined(_WIN64)
  static_assert(sizeof(TaskConfig) == 160, "TASKDIALOGCONFIG is 160 bytes on x64");
  static_assert(sizeof(TaskButton) == 12, "TASKDIALOG_BUTTON is 12 bytes on x64");
#else
  static_assert(sizeof(TaskConfig) == 96, "TASKDIALOGCONFIG is 96 bytes on x86");
  static_assert(sizeof(TaskButton) == 8, "TASKDIALOG_BUTTON is 8 bytes on x86");
#endif

  // TASKDIALOG_FLAGS. The ones with no entry here are accepted and ignored.
  constexpr DWORD kUseHiconMain           = 0x0002;
  constexpr DWORD kAllowCancellation      = 0x0008;
  constexpr DWORD kUseCommandLinks        = 0x0010;
  constexpr DWORD kUseCommandLinksNoIcon  = 0x0020;
  constexpr DWORD kVerificationChecked    = 0x0100;
  constexpr DWORD kCallbackTimer          = 0x0800;
  constexpr DWORD kPositionRelativeWindow = 0x1000;
  constexpr DWORD kRtlLayout              = 0x2000;
  constexpr DWORD kNoDefaultRadioButton   = 0x4000;
  constexpr DWORD kCanBeMinimized         = 0x8000;
  constexpr DWORD kSizeToContent          = 0x01000000;

  // TASKDIALOG_NOTIFICATIONS.
  constexpr UINT kNotifyCreated             = 0;
  constexpr UINT kNotifyButtonClicked       = 2;
  constexpr UINT kNotifyTimer               = 4;
  constexpr UINT kNotifyDestroyed           = 5;
  constexpr UINT kNotifyRadioButtonClicked  = 6;
  constexpr UINT kNotifyDialogConstructed   = 7;
  constexpr UINT kNotifyVerificationClicked = 8;

  // TASKDIALOG_MESSAGES, the ones answered.
  constexpr UINT kMessageClickButton       = WM_USER + 102;
  constexpr UINT kMessageSetElementText    = WM_USER + 108;
  constexpr UINT kMessageClickRadioButton  = WM_USER + 110;
  constexpr UINT kMessageEnableButton      = WM_USER + 111;
  constexpr UINT kMessageEnableRadioButton = WM_USER + 112;
  constexpr UINT kMessageClickVerification = WM_USER + 113;
  constexpr UINT kMessageUpdateElementText = WM_USER + 114;

  // TASKDIALOG_ELEMENTS, which is also the order the texts are stacked in,
  // except that the footer goes under the buttons.
  enum Element { kContent, kExpandedInformation, kFooter, kMainInstruction, kElementCount };

  // The common buttons, in the order the system lays them out, with the index
  // user32's MB_GetString knows each label by and the English for a user32
  // that will not say.
  struct CommonButton {
    DWORD flag;
    int id;
    UINT string_index;
    const wchar_t* fallback;
  };
  const CommonButton kCommonButtons[] = {
      {0x01, IDOK, 0, L"OK"},         {0x02, IDYES, 5, L"&Yes"},
      {0x04, IDNO, 6, L"&No"},        {0x10, IDRETRY, 3, L"&Retry"},
      {0x08, IDCANCEL, 1, L"Cancel"}, {0x20, 8 /* IDCLOSE */, 7, L"&Close"},
  };

  // Control identifiers. The caller's button IDs cannot be used for these: a
  // control ID travels in the low word of WM_COMMAND and theirs are ints, and
  // 1 and 2 have to stay free for what the dialog manager sends by itself on
  // Enter and Escape.
  constexpr int kFirstButtonControl  = 0x100;
  constexpr int kFirstRadioControl   = 0x1000;
  constexpr int kVerificationControl = 0x2000;

  constexpr UINT_PTR kTimerId      = 1;
  constexpr UINT kTimerPeriod      = 200; // What the system's dialog uses.
  constexpr INT_PTR kEndedNormally = 1;
  constexpr INT_PTR kEndedFailed   = 2;

  // --- user32 and gdi32 -----------------------------------------------------

#define WW_UI_USER32(X)                                                                         \
  X(AdjustWindowRectEx)                                                                         \
  X(BeginPaint) X(CheckDlgButton) X(CreateWindowExW) X(DialogBoxIndirectParamW) X(DrawIconEx)   \
      X(DrawTextW) X(EnableWindow) X(EndDialog) X(EndPaint) X(GetDC) X(GetDlgItem)              \
          X(GetMonitorInfoW) X(GetSystemMetrics) X(GetWindowLongPtrW) X(GetWindowRect)          \
              X(IsDlgButtonChecked) X(IsWindowEnabled) X(KillTimer) X(LoadIconW) X(LoadStringW) \
                  X(MonitorFromWindow) X(ReleaseDC) X(SendMessageW) X(SetFocus) X(SetTimer)     \
                      X(SetWindowLongPtrW) X(SetWindowPos) X(SetWindowTextW)                    \
                          X(SystemParametersInfoW)

#define WW_UI_GDI32(X) X(CreateFontIndirectW) X(DeleteObject) X(GetStockObject) X(SelectObject)

  // Two levels, so a name that is itself a macro is expanded before it is
  // made a string. GetWindowLongPtrW is one on x86, where the function is
  // called GetWindowLongW, and asking user32 for the former by name there
  // finds nothing.
#define WW_UI_STRING_(name) #name
#define WW_UI_STRING(name)  WW_UI_STRING_(name)
#define WW_UI_MEMBER(name)  decltype(&::name) name;

  using MbGetStringFn = LPCWSTR(WINAPI*)(UINT);

  struct Ui {
    WW_UI_USER32(WW_UI_MEMBER)
    WW_UI_GDI32(WW_UI_MEMBER)
    // user32's own table of button labels, in the user's language. Exported
    // by name since XP and documented long after; optional here.
    MbGetStringFn MB_GetString;
  };

  std::atomic<void*> g_user32{nullptr};
  std::atomic<void*> g_gdi32{nullptr};
  std::atomic<Ui*> g_ui{nullptr};

  const Ui* LoadUi() {
    if (Ui* ready = g_ui.load(std::memory_order_acquire)) {
      return ready;
    }
    const HMODULE user32 = ::winwrap::LoadSystemLibrary(g_user32, L"user32.dll");
    const HMODULE gdi32  = ::winwrap::LoadSystemLibrary(g_gdi32, L"gdi32.dll");
    if (!user32 || !gdi32) {
      return nullptr;
    }
    Ui* ui = static_cast<Ui*>(::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Ui)));
    if (!ui) {
      return nullptr;
    }
    bool complete = true;
#define WW_UI_LOAD(module, name)                                              \
  ui->name = reinterpret_cast<decltype(ui->name)>(                            \
      reinterpret_cast<void*>(::GetProcAddress(module, WW_UI_STRING(name)))); \
  complete = complete && ui->name != nullptr;
#define WW_UI_LOAD_USER32(name) WW_UI_LOAD(user32, name)
#define WW_UI_LOAD_GDI32(name)  WW_UI_LOAD(gdi32, name)
    WW_UI_USER32(WW_UI_LOAD_USER32)
    WW_UI_GDI32(WW_UI_LOAD_GDI32)
#undef WW_UI_LOAD_GDI32
#undef WW_UI_LOAD_USER32
#undef WW_UI_LOAD
    ui->MB_GetString = reinterpret_cast<MbGetStringFn>(
        reinterpret_cast<void*>(::GetProcAddress(user32, "MB_GetString")));
    if (!complete) {
      ::HeapFree(::GetProcessHeap(), 0, ui);
      return nullptr;
    }
    Ui* expected = nullptr;
    if (!g_ui.compare_exchange_strong(expected, ui, std::memory_order_acq_rel)) {
      // Another thread published first. Theirs is the same table.
      ::HeapFree(::GetProcessHeap(), 0, ui);
      return expected;
    }
    return ui;
  }

  // --- The system's own function --------------------------------------------

  using TaskDialogIndirectFn = decltype(&ww_TaskDialogIndirect);

  // By bare name and not cached; the top of the file says why.
  TaskDialogIndirectFn SystemTaskDialog() {
    if (GetRealNTVersion() < 0x0600) {
      return nullptr;
    }
    HMODULE comctl32 = ::GetModuleHandleW(L"comctl32.dll");
    if (!comctl32) {
      comctl32 = ::LoadLibraryW(L"comctl32.dll");
    }
    if (!comctl32) {
      return nullptr;
    }
    return reinterpret_cast<TaskDialogIndirectFn>(
        reinterpret_cast<void*>(::GetProcAddress(comctl32, "TaskDialogIndirect")));
  }

  // --- The dialog -----------------------------------------------------------

  struct Button {
    int id;
    const wchar_t* text;
    wchar_t* owned; // `text`, when it had to be copied out of a string table.
    bool link;      // Stacked in the text column rather than in the bottom row.
    HWND window;
    int width;
  };

  struct Dialog {
    const Ui* ui;
    const TaskConfig* config;
    HWND window;
    HFONT font;
    HFONT bold;
    bool own_font; // False when `font` is a stock object.
    HICON icon;
    int icon_x;
    int icon_y;
    int icon_size;
    Button* buttons;
    UINT button_count;
    HWND* radios;
    HWND verification;
    HWND elements[kElementCount];
    wchar_t* owned_text[kElementCount + 2];
    bool cancellable;
    bool failed;
    DWORD timer_start;
    int result;
    int radio_result;
    BOOL verified;
  };

  bool HasText(const wchar_t* text) {
    return text != nullptr && text[0] != L'\0';
  }

  // A string member of the configuration as text. Each may be a pointer or,
  // made with MAKEINTRESOURCE, the number of a string in hInstance's table;
  // the second kind is copied, since a string resource is not terminated.
  const wchar_t* ResolveText(const Dialog& d, PCWSTR text, wchar_t** owned) {
    *owned = nullptr;
    if (!text) {
      return nullptr;
    }
    if (!IS_INTRESOURCE(text)) {
      return text;
    }
    const wchar_t* resource = nullptr;
    const int length =
        d.ui->LoadStringW(d.config->hInstance, LOWORD(reinterpret_cast<ULONG_PTR>(text)),
                          reinterpret_cast<LPWSTR>(&resource), 0);
    if (length <= 0 || !resource) {
      return nullptr;
    }
    wchar_t* copy = static_cast<wchar_t*>(
        ::HeapAlloc(::GetProcessHeap(), 0, (static_cast<size_t>(length) + 1) * sizeof(wchar_t)));
    if (!copy) {
      return nullptr;
    }
    memcpy(copy, resource, static_cast<size_t>(length) * sizeof(wchar_t));
    copy[length] = L'\0';
    *owned       = copy;
    return copy;
  }

  HRESULT Notify(const Dialog& d, UINT notification, WPARAM w_param) {
    if (!d.config->pfCallback || !d.window) {
      return S_OK;
    }
    return d.config->pfCallback(d.window, notification, w_param, 0, d.config->lpCallbackData);
  }

  // The size `text` takes in `font`: wrapped to `width` if one is given, and
  // otherwise as wide as its longest line. `prefix` is for button labels,
  // where an ampersand is an underline and not a character.
  SIZE Measure(const Dialog& d, HDC dc, HFONT font, const wchar_t* text, int width, bool prefix) {
    SIZE size = {0, 0};
    if (!HasText(text)) {
      return size;
    }
    RECT bounds = {0, 0, width > 0 ? width : 0, 0};
    UINT format = DT_CALCRECT | DT_EXPANDTABS | DT_EDITCONTROL;
    if (width > 0) {
      format |= DT_WORDBREAK;
    }
    if (!prefix) {
      format |= DT_NOPREFIX;
    }
    const HGDIOBJ previous = d.ui->SelectObject(dc, font);
    d.ui->DrawTextW(dc, text, -1, &bounds, format);
    d.ui->SelectObject(dc, previous);
    size.cx = bounds.right - bounds.left;
    size.cy = bounds.bottom - bounds.top;
    return size;
  }

  HWND CreateControl(const Dialog& d,
                     const wchar_t* window_class,
                     const wchar_t* text,
                     DWORD style,
                     int id,
                     HFONT font,
                     int x,
                     int y,
                     int width,
                     int height) {
    const HWND control = d.ui->CreateWindowExW(
        0, window_class, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height, d.window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), ::GetModuleHandleW(nullptr), nullptr);
    if (control) {
      d.ui->SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    }
    return control;
  }

  void CreateFonts(Dialog& d) {
    // Sized by its members rather than sizeof, so it is the structure XP
    // knows whatever WINVER this happens to be compiled with: Vista appended
    // a member, and XP refuses the size that includes it.
    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize =
        static_cast<UINT>(offsetof(NONCLIENTMETRICSW, lfMessageFont) + sizeof(LOGFONTW));
    if (d.ui->SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, metrics.cbSize, &metrics, 0)) {
      d.font                         = d.ui->CreateFontIndirectW(&metrics.lfMessageFont);
      metrics.lfMessageFont.lfWeight = FW_BOLD;
      d.bold                         = d.ui->CreateFontIndirectW(&metrics.lfMessageFont);
    }
    d.own_font = d.font != nullptr;
    if (!d.font) {
      d.font = static_cast<HFONT>(d.ui->GetStockObject(DEFAULT_GUI_FONT));
    }
    if (!d.bold) {
      d.bold = d.font;
    }
  }

  void LoadMainIcon(Dialog& d) {
    const TaskConfig& config = *d.config;
    if (config.dwFlags & kUseHiconMain) {
      d.icon = config.hMainIcon;
      return;
    }
    if (!config.pszMainIcon) {
      return;
    }
    if (IS_INTRESOURCE(config.pszMainIcon)) {
      // TD_WARNING_ICON and its neighbours are MAKEINTRESOURCE of -1 to -4.
      // The fourth is the elevation shield, which XP has no picture of and no
      // concept for.
      int system_icon = 0;
      switch (LOWORD(reinterpret_cast<ULONG_PTR>(config.pszMainIcon))) {
        case 0xFFFF:
          system_icon = 32515; // IDI_WARNING
          break;
        case 0xFFFE:
          system_icon = 32513; // IDI_ERROR
          break;
        case 0xFFFD:
          system_icon = 32516; // IDI_INFORMATION
          break;
        case 0xFFFC:
          return;
        default:
          break;
      }
      if (system_icon) {
        d.icon = d.ui->LoadIconW(nullptr, MAKEINTRESOURCEW(system_icon));
        return;
      }
    }
    if (config.hInstance) {
      d.icon = d.ui->LoadIconW(config.hInstance, config.pszMainIcon);
    }
  }

  // The buttons, the caller's first and then the common ones, which is the
  // order the system shows them in. With neither there is an OK, as there is
  // in the system's.
  bool CollectButtons(Dialog& d) {
    const TaskConfig& config = *d.config;
    const size_t capacity =
        static_cast<size_t>(config.cButtons) + sizeof(kCommonButtons) / sizeof(kCommonButtons[0]);
    d.buttons = static_cast<Button*>(
        ::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, capacity * sizeof(Button)));
    if (!d.buttons) {
      return false;
    }
    const bool links = (config.dwFlags & (kUseCommandLinks | kUseCommandLinksNoIcon)) != 0;
    for (UINT i = 0; i < config.cButtons; ++i) {
      Button& button = d.buttons[d.button_count++];
      button.id      = config.pButtons[i].id;
      button.text    = ResolveText(d, config.pButtons[i].text, &button.owned);
      button.link    = links;
    }
    DWORD common = config.dwCommonButtons;
    if (d.button_count == 0 && common == 0) {
      common = 0x01; // TDCBF_OK_BUTTON
    }
    for (const CommonButton& entry : kCommonButtons) {
      if (!(common & entry.flag)) {
        continue;
      }
      Button& button = d.buttons[d.button_count++];
      button.id      = entry.id;
      button.text    = d.ui->MB_GetString ? d.ui->MB_GetString(entry.string_index) : nullptr;
      if (!HasText(button.text)) {
        button.text = entry.fallback;
      }
    }
    return true;
  }

  Button* FindButton(const Dialog& d, int id) {
    for (UINT i = 0; i < d.button_count; ++i) {
      if (d.buttons[i].id == id) {
        return &d.buttons[i];
      }
    }
    return nullptr;
  }

  // Creates every control and sizes and places the window. False only for a
  // failure that leaves no usable dialog.
  bool Build(Dialog& d) {
    const Ui& ui             = *d.ui;
    const TaskConfig& config = *d.config;

    CreateFonts(d);
    LoadMainIcon(d);
    if (!CollectButtons(d)) {
      return false;
    }
    if (config.cRadioButtons) {
      d.radios = static_cast<HWND*>(
          ::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, config.cRadioButtons * sizeof(HWND)));
      if (!d.radios) {
        return false;
      }
    }

    const wchar_t* texts[kElementCount] = {};
    texts[kMainInstruction] =
        ResolveText(d, config.pszMainInstruction, &d.owned_text[kMainInstruction]);
    texts[kContent] = ResolveText(d, config.pszContent, &d.owned_text[kContent]);
    texts[kExpandedInformation] =
        ResolveText(d, config.pszExpandedInformation, &d.owned_text[kExpandedInformation]);
    texts[kFooter] = ResolveText(d, config.pszFooter, &d.owned_text[kFooter]);
    const wchar_t* verification =
        ResolveText(d, config.pszVerificationText, &d.owned_text[kElementCount]);
    const wchar_t* title = ResolveText(d, config.pszWindowTitle, &d.owned_text[kElementCount + 1]);

    // The caption, or the program's file name where the caller gave none,
    // which is what the system's dialog shows.
    wchar_t module_path[MAX_PATH] = {};
    if (!HasText(title)) {
      const DWORD length = ::GetModuleFileNameW(nullptr, module_path, MAX_PATH - 1);
      title              = module_path;
      for (DWORD i = 0; i < length; ++i) {
        if (module_path[i] == L'\\' || module_path[i] == L'/') {
          title = module_path + i + 1;
        }
      }
    }
    ui.SetWindowTextW(d.window, title);

    const HDC dc = ui.GetDC(d.window);
    if (!dc) {
      return false;
    }

    // Everything is in units of the font's line height, so the dialog scales
    // with the user's font and DPI settings without being told about either.
    const int em            = (Measure(d, dc, d.font, L"Mg", 0, false).cy > 0)
                                  ? Measure(d, dc, d.font, L"Mg", 0, false).cy
                                  : 13;
    const int margin        = em;
    const int gap           = em * 2 / 3;
    const int button_height = em * 7 / 4;
    const int button_gap    = em / 2;
    const int row_height    = em * 3 / 2;

    d.icon_size      = d.icon ? ui.GetSystemMetrics(SM_CXICON) : 0;
    d.icon_x         = margin;
    d.icon_y         = margin;
    const int text_x = margin + (d.icon ? d.icon_size + margin : 0);

    // The work area of the monitor the dialog will be on, which bounds the
    // dialog and is what it is centred in when not centred on its parent.
    RECT work = {0, 0, ui.GetSystemMetrics(SM_CXSCREEN), ui.GetSystemMetrics(SM_CYSCREEN)};
    MONITORINFO monitor = {};
    monitor.cbSize      = sizeof(monitor);
    if (ui.GetMonitorInfoW(ui.MonitorFromWindow(config.hwndParent ? config.hwndParent : d.window,
                                                MONITOR_DEFAULTTOPRIMARY),
                           &monitor)) {
      work = monitor.rcWork;
    }
    const int work_width  = work.right - work.left;
    const int work_height = work.bottom - work.top;

    // --- Widths ---

    // The bottom row: the buttons that are not links.
    int row_width = 0;
    for (UINT i = 0; i < d.button_count; ++i) {
      Button& button = d.buttons[i];
      if (button.link) {
        continue;
      }
      const int natural = Measure(d, dc, d.font, button.text, 0, true).cx + em * 2;
      button.width      = natural > em * 6 ? natural : em * 6;
      row_width += button.width + (row_width ? button_gap : 0);
    }
    const int verification_width =
        HasText(verification) ? Measure(d, dc, d.font, verification, 0, true).cx + em * 2 : 0;

    // The text column: as wide as its widest line wants, within limits.
    int natural = 0;
    for (int element = 0; element < kElementCount; ++element) {
      const HFONT font = element == kMainInstruction ? d.bold : d.font;
      const int width  = Measure(d, dc, font, texts[element], 0, false).cx;
      natural          = width > natural ? width : natural;
    }
    for (UINT i = 0; i < config.cRadioButtons; ++i) {
      wchar_t* owned = nullptr;
      const int width =
          Measure(d, dc, d.font, ResolveText(d, config.pRadioButtons[i].text, &owned), 0, true).cx +
          em * 2;
      natural = width > natural ? width : natural;
      if (owned) {
        ::HeapFree(::GetProcessHeap(), 0, owned);
      }
    }
    for (UINT i = 0; i < d.button_count; ++i) {
      if (d.buttons[i].link) {
        const int width = Measure(d, dc, d.font, d.buttons[i].text, 0, true).cx + em * 2;
        natural         = width > natural ? width : natural;
      }
    }
    int limit = em * 30;
    if ((config.dwFlags & kSizeToContent) && work_width / 2 > limit) {
      limit = work_width / 2;
    }
    int text_width = natural;
    text_width     = text_width < em * 16 ? em * 16 : text_width;
    text_width     = text_width > limit ? limit : text_width;

    int client_width = text_x + text_width + margin;
    if (config.cxWidth) {
      // In dialog units, a quarter of the font's average character width,
      // found the way the dialog manager finds it.
      const int alphabet =
          Measure(d, dc, d.font, L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", 0, false)
              .cx;
      const int average = (alphabet / 26 + 1) / 2;
      client_width      = static_cast<int>(config.cxWidth) * average / 4;
    }
    const int bottom_width =
        margin * 2 + row_width + (verification_width ? verification_width + gap : 0);
    client_width = client_width < bottom_width ? bottom_width : client_width;
    // And never so narrow that the text column has no width, which a small
    // cxWidth beside an icon would otherwise ask for.
    const int narrowest = text_x + em * 8 + margin;
    client_width        = client_width < narrowest ? narrowest : client_width;
    client_width        = client_width > work_width ? work_width : client_width;
    text_width          = client_width - text_x - margin;

    // --- Heights, creating each control where it lands ---

    bool created           = true;
    int y                  = margin;
    const Element column[] = {kMainInstruction, kContent, kExpandedInformation};

    // A line or two of text beside an icon is centred on it, as a message
    // box's is, rather than hanging from its top edge.
    if (d.icon && !config.cRadioButtons) {
      int column_height = 0;
      bool only_text    = true;
      for (UINT i = 0; i < d.button_count; ++i) {
        only_text = only_text && !d.buttons[i].link;
      }
      for (const Element element : column) {
        if (HasText(texts[element])) {
          const HFONT font = element == kMainInstruction ? d.bold : d.font;
          column_height += Measure(d, dc, font, texts[element], text_width, false).cy +
                           (column_height ? gap : 0);
        }
      }
      if (only_text && column_height < d.icon_size) {
        y += (d.icon_size - column_height) / 2;
      }
    }
    for (const Element element : column) {
      if (!HasText(texts[element])) {
        continue;
      }
      const HFONT font = element == kMainInstruction ? d.bold : d.font;
      const int height = Measure(d, dc, font, texts[element], text_width, false).cy;
      d.elements[element] =
          CreateControl(d, L"STATIC", texts[element], SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL, -1,
                        font, text_x, y, text_width, height);
      created = created && d.elements[element];
      y += height + gap;
    }

    bool group_start = true;
    for (UINT i = 0; i < config.cRadioButtons; ++i) {
      wchar_t* owned      = nullptr;
      const wchar_t* text = ResolveText(d, config.pRadioButtons[i].text, &owned);
      d.radios[i] = CreateControl(d, L"BUTTON", text ? text : L"",
                                  BS_AUTORADIOBUTTON | (group_start ? WS_GROUP | WS_TABSTOP : 0),
                                  kFirstRadioControl + static_cast<int>(i), d.font, text_x, y,
                                  text_width, row_height);
      created     = created && d.radios[i];
      group_start = false;
      y += row_height;
      if (owned) {
        ::HeapFree(::GetProcessHeap(), 0, owned);
      }
    }
    if (config.cRadioButtons) {
      y += gap;
    }

    // WS_GROUP on the first control after the radio buttons ends their group,
    // so the arrow keys stay among them.
    group_start = true;
    bool linked = false;
    for (UINT i = 0; i < d.button_count; ++i) {
      Button& button = d.buttons[i];
      if (!button.link) {
        continue;
      }
      // A command link's label is "title\nnote". Both lines are kept.
      const int height = Measure(d, dc, d.font, button.text, text_width - em * 2, true).cy + em;
      button.window    = CreateControl(
          d, L"BUTTON", button.text ? button.text : L"",
          BS_PUSHBUTTON | BS_LEFT | BS_MULTILINE | WS_TABSTOP | (group_start ? WS_GROUP : 0),
          kFirstButtonControl + static_cast<int>(i), d.font, text_x, y, text_width, height);
      created     = created && button.window;
      group_start = false;
      linked      = true;
      y += height + button_gap;
    }
    if (linked) {
      y += gap - button_gap;
    }

    // The text column ended with a gap it does not need; the icon may reach
    // lower than the text does.
    y = y > margin ? y - gap : y;
    if (d.icon && y < margin + d.icon_size) {
      y = margin + d.icon_size;
    }

    if (row_width || verification_width) {
      y += margin;
      int x = client_width - margin - row_width;
      for (UINT i = 0; i < d.button_count; ++i) {
        Button& button = d.buttons[i];
        if (button.link) {
          continue;
        }
        button.window = CreateControl(d, L"BUTTON", button.text ? button.text : L"",
                                      BS_PUSHBUTTON | WS_TABSTOP | (group_start ? WS_GROUP : 0),
                                      kFirstButtonControl + static_cast<int>(i), d.font, x, y,
                                      button.width, button_height);
        created       = created && button.window;
        group_start   = false;
        x += button.width + button_gap;
      }
      if (verification_width) {
        d.verification = CreateControl(
            d, L"BUTTON", verification, BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP,
            kVerificationControl, d.font, margin, y, verification_width, button_height);
        created = created && d.verification;
        if (d.verification && (config.dwFlags & kVerificationChecked)) {
          ui.CheckDlgButton(d.window, kVerificationControl, BST_CHECKED);
        }
      }
      y += button_height;
    }

    if (HasText(texts[kFooter])) {
      y += gap;
      const int width  = client_width - margin * 2;
      const int height = Measure(d, dc, d.font, texts[kFooter], width, false).cy;
      d.elements[kFooter] =
          CreateControl(d, L"STATIC", texts[kFooter], SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL, -1,
                        d.font, margin, y, width, height);
      created = created && d.elements[kFooter];
      y += height;
    }
    int client_height = y + margin;

    ui.ReleaseDC(d.window, dc);
    if (!created) {
      return false;
    }

    // --- The window ---

    RECT frame = {0, 0, client_width, client_height};
    ui.AdjustWindowRectEx(&frame, static_cast<DWORD>(ui.GetWindowLongPtrW(d.window, GWL_STYLE)),
                          FALSE, static_cast<DWORD>(ui.GetWindowLongPtrW(d.window, GWL_EXSTYLE)));
    int width  = frame.right - frame.left;
    int height = frame.bottom - frame.top;
    // Text too long for the screen is cut off at the bottom rather than
    // running off it with the buttons.
    height = height > work_height ? work_height : height;

    RECT anchor = work;
    if ((config.dwFlags & kPositionRelativeWindow) && config.hwndParent) {
      ui.GetWindowRect(config.hwndParent, &anchor);
    }
    int left = anchor.left + ((anchor.right - anchor.left) - width) / 2;
    int top  = anchor.top + ((anchor.bottom - anchor.top) - height) / 2;
    left     = left + width > work.right ? work.right - width : left;
    top      = top + height > work.bottom ? work.bottom - height : top;
    left     = left < work.left ? work.left : left;
    top      = top < work.top ? work.top : top;
    ui.SetWindowPos(d.window, nullptr, left, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);

    // --- Initial state ---

    if (config.cRadioButtons && !(config.dwFlags & kNoDefaultRadioButton)) {
      UINT selected = 0;
      for (UINT i = 0; i < config.cRadioButtons; ++i) {
        if (config.pRadioButtons[i].id == config.nDefaultRadioButton) {
          selected = i;
        }
      }
      ui.CheckDlgButton(d.window, kFirstRadioControl + static_cast<int>(selected), BST_CHECKED);
    }

    Button* initial = FindButton(d, config.nDefaultButton);
    if (!initial) {
      initial = &d.buttons[0];
    }
    ui.SendMessageW(initial->window, BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);
    ui.SendMessageW(d.window, DM_SETDEFID,
                    static_cast<WPARAM>(kFirstButtonControl + (initial - d.buttons)), 0);
    ui.SetFocus(initial->window);

    if (config.dwFlags & kCallbackTimer) {
      d.timer_start = ::GetTickCount();
      ui.SetTimer(d.window, kTimerId, kTimerPeriod, nullptr);
    }
    return true;
  }

  // A button was pressed, by the user or by a message. The callback may
  // refuse, which leaves the dialog open.
  void Click(Dialog& d, int id) {
    if (Notify(d, kNotifyButtonClicked, static_cast<WPARAM>(id)) == S_FALSE) {
      return;
    }
    d.result = id;
    if (d.verification) {
      d.verified = d.ui->IsDlgButtonChecked(d.window, kVerificationControl) == BST_CHECKED;
    }
    for (UINT i = 0; i < d.config->cRadioButtons; ++i) {
      if (d.ui->IsDlgButtonChecked(d.window, kFirstRadioControl + static_cast<int>(i)) ==
          BST_CHECKED) {
        d.radio_result = d.config->pRadioButtons[i].id;
      }
    }
    d.ui->EndDialog(d.window, kEndedNormally);
  }

  // Escape, Alt+F4, the close box, or a WM_CLOSE posted from elsewhere --
  // which is how Electron dismisses a message box from code.
  void Cancel(Dialog& d) {
    if (d.cancellable) {
      Click(d, IDCANCEL);
    }
  }

  void OnCommand(Dialog& d, int control) {
    const Ui& ui = *d.ui;
    if (control == IDCANCEL) {
      Cancel(d);
    } else if (control == kVerificationControl) {
      Notify(d, kNotifyVerificationClicked,
             ui.IsDlgButtonChecked(d.window, control) == BST_CHECKED);
    } else if (control >= kFirstRadioControl &&
               control < kFirstRadioControl + static_cast<int>(d.config->cRadioButtons)) {
      Notify(d, kNotifyRadioButtonClicked,
             static_cast<WPARAM>(d.config->pRadioButtons[control - kFirstRadioControl].id));
    } else if (control >= kFirstButtonControl &&
               control < kFirstButtonControl + static_cast<int>(d.button_count)) {
      Click(d, d.buttons[control - kFirstButtonControl].id);
    }
  }

  HWND RadioWindow(const Dialog& d, int id) {
    for (UINT i = 0; i < d.config->cRadioButtons; ++i) {
      if (d.config->pRadioButtons[i].id == id) {
        return d.radios[i];
      }
    }
    return nullptr;
  }

  // The TDM_ messages a caller may send to the window it was given in its
  // callback. True if this was one.
  bool OnTaskMessage(Dialog& d, UINT message, WPARAM w_param, LPARAM l_param) {
    const Ui& ui = *d.ui;
    switch (message) {
      case kMessageClickButton: {
        const int id         = static_cast<int>(w_param);
        const Button* button = FindButton(d, id);
        if (button) {
          if (ui.IsWindowEnabled(button->window)) {
            Click(d, id);
          }
        } else if (id == IDCANCEL) {
          Cancel(d);
        }
        return true;
      }
      case kMessageEnableButton:
        if (const Button* button = FindButton(d, static_cast<int>(w_param))) {
          ui.EnableWindow(button->window, l_param != 0);
        }
        return true;
      case kMessageClickRadioButton:
        if (const HWND radio = RadioWindow(d, static_cast<int>(w_param))) {
          ui.SendMessageW(radio, BM_CLICK, 0, 0);
        }
        return true;
      case kMessageEnableRadioButton:
        if (const HWND radio = RadioWindow(d, static_cast<int>(w_param))) {
          ui.EnableWindow(radio, l_param != 0);
        }
        return true;
      case kMessageClickVerification:
        if (d.verification) {
          ui.CheckDlgButton(d.window, kVerificationControl, w_param ? BST_CHECKED : BST_UNCHECKED);
          if (l_param) {
            ui.SetFocus(d.verification);
          }
          Notify(d, kNotifyVerificationClicked, w_param != 0);
        }
        return true;
      case kMessageSetElementText:
      case kMessageUpdateElementText:
        // The new text goes into the space the old text had. Nothing is laid
        // out again, which is all TDM_UPDATE_ELEMENT_TEXT promises and less
        // than TDM_SET_ELEMENT_TEXT does.
        if (w_param < kElementCount && d.elements[w_param] && l_param &&
            !IS_INTRESOURCE(reinterpret_cast<PCWSTR>(l_param))) {
          ui.SetWindowTextW(d.elements[w_param], reinterpret_cast<PCWSTR>(l_param));
        }
        return true;
      default:
        return false;
    }
  }

  INT_PTR CALLBACK DialogProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    // Published before any dialog is created, so never null here.
    const Ui& ui = *g_ui.load(std::memory_order_acquire);

    if (message == WM_INITDIALOG) {
      Dialog& d = *reinterpret_cast<Dialog*>(l_param);
      d.window  = window;
      ui.SetWindowLongPtrW(window, DWLP_USER, static_cast<LONG_PTR>(l_param));
      if (!Build(d)) {
        d.failed = true;
        ui.EndDialog(window, kEndedFailed);
        return FALSE;
      }
      Notify(d, kNotifyDialogConstructed, 0);
      Notify(d, kNotifyCreated, 0);
      // FALSE: the focus has been set, and is not the dialog manager's to
      // give to the first control.
      return FALSE;
    }

    Dialog* d = reinterpret_cast<Dialog*>(ui.GetWindowLongPtrW(window, DWLP_USER));
    if (!d || d->failed) {
      return FALSE;
    }
    switch (message) {
      case WM_COMMAND:
        if (HIWORD(w_param) != BN_CLICKED) {
          return FALSE;
        }
        OnCommand(*d, LOWORD(w_param));
        return TRUE;
      case WM_CLOSE:
        Cancel(*d);
        return TRUE;
      case WM_PAINT: {
        if (!d->icon) {
          return FALSE;
        }
        // Drawn rather than given to a static control, which would show an
        // icon of another size at that size and not at the system's.
        PAINTSTRUCT paint;
        const HDC dc = ui.BeginPaint(window, &paint);
        ui.DrawIconEx(dc, d->icon_x, d->icon_y, d->icon, d->icon_size, d->icon_size, 0, nullptr,
                      DI_NORMAL);
        ui.EndPaint(window, &paint);
        return TRUE;
      }
      case WM_TIMER:
        if (w_param == kTimerId) {
          // S_FALSE from the callback restarts the count.
          if (Notify(*d, kNotifyTimer, ::GetTickCount() - d->timer_start) == S_FALSE) {
            d->timer_start = ::GetTickCount();
          }
          return TRUE;
        }
        return FALSE;
      case WM_DESTROY:
        ui.KillTimer(window, kTimerId);
        Notify(*d, kNotifyDestroyed, 0);
        return FALSE;
      default:
        return OnTaskMessage(*d, message, w_param, l_param) ? TRUE : FALSE;
    }
  }

  void Release(Dialog& d) {
    const HANDLE heap = ::GetProcessHeap();
    if (d.bold && d.bold != d.font) {
      d.ui->DeleteObject(d.bold);
    }
    if (d.font && d.own_font) {
      d.ui->DeleteObject(d.font);
    }
    for (UINT i = 0; i < d.button_count; ++i) {
      if (d.buttons[i].owned) {
        ::HeapFree(heap, 0, d.buttons[i].owned);
      }
    }
    if (d.buttons) {
      ::HeapFree(heap, 0, d.buttons);
    }
    if (d.radios) {
      ::HeapFree(heap, 0, d.radios);
    }
    for (wchar_t* text : d.owned_text) {
      if (text) {
        ::HeapFree(heap, 0, text);
      }
    }
  }

} // namespace

extern "C" {

// winwrap's own dialog, whatever the system has. Split out of
// ww_TaskDialogIndirect for the reason winwrap_RegGetValueFallbackW is split
// out of ww_RegGetValueW: the public name hands over to comctl32 wherever
// comctl32 answers, so a test calling it on such a system tests comctl32, and
// this code would be exercised only where nobody runs the tests.
HRESULT WINAPI winwrap_TaskDialogIndirectFallback(const struct _TASKDIALOGCONFIG* pTaskConfig,
                                                  int* pnButton,
                                                  int* pnRadioButton,
                                                  BOOL* pfVerificationFlagChecked) {
  const TaskConfig* config = reinterpret_cast<const TaskConfig*>(pTaskConfig);
  if (!config || config->cbSize != sizeof(TaskConfig) || (config->cButtons && !config->pButtons) ||
      (config->cRadioButtons && !config->pRadioButtons)) {
    return E_INVALIDARG;
  }
  const Ui* ui = LoadUi();
  if (!ui) {
    return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
  }

  Dialog d = {};
  d.ui     = ui;
  d.config = config;

  // Whether Escape and the close box do anything: when the caller said so,
  // or when there is a Cancel button for them to mean.
  d.cancellable = (config->dwFlags & kAllowCancellation) || (config->dwCommonButtons & 0x08);
  for (UINT i = 0; i < config->cButtons; ++i) {
    d.cancellable = d.cancellable || config->pButtons[i].id == IDCANCEL;
  }

  // A dialog template with no controls in it: the header, then an empty
  // menu, class and title. The controls are created in WM_INITDIALOG, once
  // there is a window to measure text for. This way round the dialog manager
  // still supplies the modal loop, the disabled owner, Tab, Enter and Escape.
  alignas(DWORD) WORD storage[16] = {};
  DLGTEMPLATE* dialog_template    = reinterpret_cast<DLGTEMPLATE*>(storage);
  dialog_template->style          = WS_POPUP | WS_CAPTION | DS_MODALFRAME | DS_SETFOREGROUND;
  if (d.cancellable || (config->dwFlags & kCanBeMinimized)) {
    dialog_template->style |= WS_SYSMENU;
  }
  if (config->dwFlags & kCanBeMinimized) {
    dialog_template->style |= WS_MINIMIZEBOX;
  }
  if (config->dwFlags & kRtlLayout) {
    dialog_template->dwExtendedStyle |= WS_EX_LAYOUTRTL;
  }

  const INT_PTR ended =
      ui->DialogBoxIndirectParamW(::GetModuleHandleW(nullptr), dialog_template, config->hwndParent,
                                  DialogProc, reinterpret_cast<LPARAM>(&d));
  const bool failed = d.failed;
  Release(d);
  if (ended != kEndedNormally) {
    return failed ? E_OUTOFMEMORY : E_FAIL;
  }

  if (pnButton) {
    *pnButton = d.result;
  }
  if (pnRadioButton) {
    *pnRadioButton = d.radio_result;
  }
  if (pfVerificationFlagChecked) {
    *pfVerificationFlagChecked = d.verified;
  }
  return S_OK;
}

HRESULT WINAPI ww_TaskDialogIndirect(const struct _TASKDIALOGCONFIG* pTaskConfig,
                                     int* pnButton,
                                     int* pnRadioButton,
                                     BOOL* pfVerificationFlagChecked) {
  // Hand straight over where Windows has this itself.
  if (const TaskDialogIndirectFn system_fn = SystemTaskDialog()) {
    return system_fn(pTaskConfig, pnButton, pnRadioButton, pfVerificationFlagChecked);
  }
  return winwrap_TaskDialogIndirectFallback(pTaskConfig, pnButton, pnRadioButton,
                                            pfVerificationFlagChecked);
}

} // extern "C"
