// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// kd_testtarget — a controlled application to inject into.
//
// Implements: plan.md §3 Phase 0B (the critical gate), plan.md §7 ("tools/testtarget exercising
// classic edit, rich edit, multiline and password controls"), and the `A09` row of
// docs/supported-targets.md §5.9 — the **calibration target**. Every other application's error
// figures are only interpretable relative to this one: if A09 cannot hit one line height, the
// estimator is broken, not the target.
//
// ─── What makes this different from "a window with an edit box" ─────────────────────────────
//
// Three properties, each of which exists because a measurement is impossible without it:
//
//   1. **It logs every WM_KEYDOWN / WM_CHAR / WM_UNICHAR / WM_PASTE it receives, with the exact
//      code point.** plan.md §1.5 is explicit that "injection success and anchor estimation are
//      separate signals" and that success is "never inferred from pixels". A test that asserts
//      what *arrived* needs the target to say what arrived; reading the control's text back is
//      not enough, because a control can normalise, reject or reflow what it was handed and the
//      difference between "the keystroke never arrived" and "the control ate it" is exactly the
//      distinction Phase 0B has to draw.
//
//   2. **A pane that draws its own caret and never calls CreateCaret.** Anchor source A
//      (GetGUIThreadInfo().rcCaret) is empty for apps that draw their own caret — which is most
//      of the interesting ones (VS Code, Slack, WSLg). Without a local target in that state, the
//      empty-rcCaret path is only ever exercised against applications we cannot instrument.
//
//   3. **A side channel** (WM_COPYDATA) reporting the true caret rectangle, the field's text, the
//      line height, the theme and the DPI. docs/supported-targets.md §3.4: this is the only cell
//      family with *programmatic* ground truth.
//
// ─── Honest limits of this target ───────────────────────────────────────────────────────────
//
// It is a Win32 target. It cannot tell us how Electron, Chromium's compositor or WSLg's RDP
// pipe behave — that is what the other eight applications in the matrix are for. Passing here is
// necessary, not sufficient.
//
// Threading: one thread, one message loop. The side channel is answered synchronously inside the
// WM_COPYDATA handler; nothing here blocks for longer than a clipboard read.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// commctrl.h must follow windows.h; SetWindowSubclass lives here and comctl32 is already linked
// by kd_core (see CMakeLists.txt).
#include <commctrl.h>
#include <richedit.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <deque>
#include <string>
#include <vector>

#include "util/json.h"
#include "util/utf8.h"

namespace {

// ─── Side-channel contract (shared with tools/probe/main.cpp) ───────────────────────────────
//
// Deliberately duplicated as literals in both tools rather than promoted into a header under
// src/: this is a Phase-0B instrument contract between two tools, not a product interface, and
// src/ is the wrong place for it. If a third consumer ever appears, that is the moment to give
// it a home.
//
// The reply is sent from inside the request handler with SendMessageTimeout back to the
// requester's window. That is a re-entrant SendMessage — the probe is blocked inside its own
// SendMessageTimeout — and it works because Windows dispatches inbound sent messages to a thread
// that is itself blocked in SendMessage. PostMessage is not an option: WM_COPYDATA's buffer is
// only valid for the duration of the send.
constexpr DWORD kSideChannelRequest = 0x4B440101u;  // 'KD' 01 01
constexpr DWORD kSideChannelReply = 0x4B440102u;
constexpr wchar_t kWindowClass[] = L"KdTestTargetWindow";
constexpr wchar_t kPaneClass[] = L"KdTestTargetSelfCaretPane";

// ─── Control identity ───────────────────────────────────────────────────────────────────────

enum : int {
    kIdEditSingle = 101,
    kIdEditMulti = 102,
    kIdRichEdit = 103,
    kIdEditPassword = 104,
    kIdEditReadOnly = 105,
    kIdEditRtl = 106,
    kIdSelfCaret = 107,
    kIdLog = 108,
};

enum : UINT {
    kIdmThemeDark = 40001,
    kIdmThemeLight = 40002,
    kIdmFontLarge = 40003,
    kIdmFontSmall = 40004,
    kIdmCaretBlink = 40010,
    kIdmCaretSteady = 40011,
    kIdmCaretFocusPane = 40012,
    kIdmScrollNone = 40020,
    kIdmScrollOnInject = 40021,
    kIdmScrollTimer = 40022,
    kIdmLogClear = 40030,
    kIdmLogCopy = 40031,
    kIdmTextClear = 40040,
    kIdmTextMidLine = 40041,
    kIdmTextLongLine = 40042,
    kIdmTextManyLines = 40043,
    kIdmTextRtl = 40044,
};

const wchar_t* control_name(int id) noexcept {
    switch (id) {
        case kIdEditSingle: return L"edit-single";
        case kIdEditMulti: return L"edit-multiline";
        case kIdRichEdit: return L"richedit";
        case kIdEditPassword: return L"edit-password";
        case kIdEditReadOnly: return L"edit-readonly";
        case kIdEditRtl: return L"edit-rtl";
        case kIdSelfCaret: return L"self-caret-pane";
        case kIdLog: return L"log";
        default: return L"?";
    }
}

// ─── Event log ──────────────────────────────────────────────────────────────────────────────
//
// Bounded on purpose. A run of the probe's `--repeat 20` against REF-1 produces a few thousand
// WM_CHARs; an unbounded log would grow until the ListBox redraw became the slowest thing in the
// measurement, which would then be measuring us.

struct LogEntry {
    uint64_t ms = 0;         // milliseconds since this process started
    int control_id = 0;
    std::wstring message;    // "WM_CHAR"
    std::wstring detail;     // "U+0041 'A' extra=0x00000000"
};

constexpr size_t kMaxLogEntries = 4096;

struct AppState {
    HWND main = nullptr;
    HWND edit_single = nullptr;
    HWND edit_multi = nullptr;
    HWND rich = nullptr;
    HWND edit_password = nullptr;
    HWND edit_readonly = nullptr;
    HWND edit_rtl = nullptr;
    HWND self_caret = nullptr;
    HWND log_list = nullptr;
    HFONT font = nullptr;
    HFONT log_font = nullptr;
    HBRUSH bg_brush = nullptr;

    bool dark = false;
    bool large_font = false;

    // Whether the log LISTBOX is updated. The log itself always accumulates - `log_total` and the
    // side channel's `events` are unaffected - because this switch exists to stop the pane
    // REPAINTING, not to stop recording.
    //
    // Why it has to exist: this is the calibration target (§5.9), and its own instrumentation was
    // contaminating the measurement it exists to enable. The pane takes an LB_ADDSTRING and an
    // LB_SETTOPINDEX per received keystroke, so a 207-byte REF-1 repaints it several hundred
    // times, and any capture region that overlaps it sees a diff dominated by scrolling log text
    // rather than by the injected characters. Measured against A09/S01: 14-17 changed regions and
    // ~40 line heights of apparent anchor error, none of it the estimator's doing.
    bool log_visual = true;
    UINT dpi = 96;

    std::deque<LogEntry> log;
    uint64_t log_total = 0;   // never reset by a trim; the probe uses it to detect drops
    ULONGLONG start_tick = 0;

    // Set by the probe before an injection so that a log line can be attributed to a cell.
    std::wstring marker;
};

AppState g;

uint64_t now_ms() noexcept { return static_cast<uint64_t>(GetTickCount64() - g.start_tick); }

std::wstring hex32(uint32_t v) {
    wchar_t buf[16];
    swprintf(buf, 16, L"0x%08X", v);
    return buf;
}

// dwExtraInfo is a ULONG_PTR — 64 bits here — and injector.h's InjectionMark is a full-width
// random value. Truncating it to 32 bits would make two different marks look identical, which is
// precisely the confusion the per-process random mark exists to avoid.
std::wstring hex64(uint64_t v) {
    wchar_t buf[24];
    swprintf(buf, 24, L"0x%016llX", static_cast<unsigned long long>(v));
    return buf;
}

std::wstring hex_ptr(const void* p) {
    wchar_t buf[24];
    swprintf(buf, 24, L"0x%p", p);
    return buf;
}

std::wstring codepoint_label(uint32_t cp) {
    wchar_t buf[64];
    if (cp >= 0xD800u && cp <= 0xDBFFu) {
        swprintf(buf, 64, L"U+%04X (high surrogate)", cp);
    } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
        swprintf(buf, 64, L"U+%04X (low surrogate)", cp);
    } else if (cp < 0x20u) {
        swprintf(buf, 64, L"U+%04X (control)", cp);
    } else if (cp <= 0xFFFFu) {
        swprintf(buf, 64, L"U+%04X '%lc'", cp, static_cast<wchar_t>(cp));
    } else {
        swprintf(buf, 64, L"U+%05X", cp);
    }
    return buf;
}

void log_event(int control_id, const wchar_t* message, std::wstring detail);

// ─── Fonts, colours, DPI ────────────────────────────────────────────────────────────────────

COLORREF text_colour() { return g.dark ? RGB(228, 228, 228) : RGB(16, 16, 16); }
COLORREF back_colour() { return g.dark ? RGB(24, 24, 28) : RGB(252, 252, 252); }

void rebuild_fonts() {
    if (g.font) DeleteObject(g.font);
    if (g.log_font) DeleteObject(g.log_font);

    const int pt = g.large_font ? 20 : 10;
    const int height = -MulDiv(pt, static_cast<int>(g.dpi), 72);
    // A fixed-pitch face on purpose: the anchor error is measured in line heights, and a
    // proportional face makes "the caret is one character to the left" a different number of
    // pixels on every measurement.
    g.font = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         FIXED_PITCH | FF_MODERN, L"Consolas");
    g.log_font = CreateFontW(-MulDiv(9, static_cast<int>(g.dpi), 72), 0, 0, 0, FW_NORMAL, FALSE,
                             FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN,
                             L"Consolas");

    const HWND targets[] = {g.edit_single, g.edit_multi, g.rich,     g.edit_password,
                            g.edit_readonly, g.edit_rtl, g.self_caret};
    for (HWND h : targets) {
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    }
    if (g.log_list) SendMessageW(g.log_list, WM_SETFONT, reinterpret_cast<WPARAM>(g.log_font), TRUE);
}

void apply_theme() {
    if (g.bg_brush) DeleteObject(g.bg_brush);
    g.bg_brush = CreateSolidBrush(back_colour());

    if (g.rich) {
        // RichEdit does not honour WM_CTLCOLOREDIT for its background; it has its own message,
        // and its text colour has to be set through a character format over the whole range.
        SendMessageW(g.rich, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(back_colour()));
        CHARFORMAT2W cf{};
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_COLOR;
        cf.crTextColor = text_colour();
        SendMessageW(g.rich, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&cf));
    }
    if (g.main) InvalidateRect(g.main, nullptr, TRUE);
}

int line_height_of(HWND control) {
    if (!control) return 0;
    HDC dc = GetDC(control);
    if (!dc) return 0;
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    HGDIOBJ old = font ? SelectObject(dc, font) : nullptr;
    TEXTMETRICW tm{};
    const BOOL ok = GetTextMetricsW(dc, &tm);
    if (old) SelectObject(dc, old);
    ReleaseDC(control, dc);
    return ok ? static_cast<int>(tm.tmHeight + tm.tmExternalLeading) : 0;
}

// ─── The self-drawn-caret pane ──────────────────────────────────────────────────────────────
//
// The whole point of this control is what it does **not** do: it never calls CreateCaret,
// SetCaretPos or ShowCaret, so GetGUIThreadInfo().rcCaret is empty while it has focus. That is
// anchor source A's failure mode (plan.md §1.5, "empty for apps drawing their own caret") and it
// is otherwise only reproducible against applications we cannot instrument.
//
// It also owns the scroll scenario (S07): in `scroll_on_inject` mode every inserted character
// shifts the content by a line, so the after-frame differs everywhere and source C must score it
// as a scroll and discard it rather than pointing at the biggest changed rectangle.

enum class ScrollMode { None, OnInject, Timer };

struct PaneState {
    std::wstring text;
    size_t caret = 0;
    int scroll_y = 0;
    bool steady_caret = false;   // S10: a non-blinking cursor is the control case
    bool caret_visible = true;
    ScrollMode scroll = ScrollMode::None;
};

PaneState pane;

constexpr UINT_PTR kBlinkTimer = 1;
constexpr UINT_PTR kScrollTimer = 2;

std::vector<std::wstring> pane_lines() {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : pane.text) {
        if (c == L'\n') {
            out.push_back(cur);
            cur.clear();
        } else if (c != L'\r') {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

// Line index and byte-free column of the caret, in UTF-16 code units.
void pane_caret_line_col(size_t& line, size_t& col) {
    line = 0;
    col = 0;
    for (size_t i = 0; i < pane.caret && i < pane.text.size(); ++i) {
        if (pane.text[i] == L'\n') {
            ++line;
            col = 0;
        } else if (pane.text[i] != L'\r') {
            ++col;
        }
    }
}

// The pane's caret rectangle in client coordinates. This is programmatic ground truth: we know
// exactly where we drew it, which is what makes A09 the calibration row.
RECT pane_caret_rect(HWND h) {
    RECT r{0, 0, 0, 0};
    HDC dc = GetDC(h);
    if (!dc) return r;
    HGDIOBJ old = SelectObject(dc, g.font);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    const int lh = static_cast<int>(tm.tmHeight + tm.tmExternalLeading);

    size_t line = 0, col = 0;
    pane_caret_line_col(line, col);
    const std::vector<std::wstring> lines = pane_lines();
    const std::wstring& text = lines[line < lines.size() ? line : lines.size() - 1];
    SIZE sz{0, 0};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(col < text.size() ? col : text.size()),
                          &sz);
    SelectObject(dc, old);
    ReleaseDC(h, dc);

    const int x = 4 + sz.cx;
    const int y = 4 + static_cast<int>(line) * lh - pane.scroll_y;
    r.left = x;
    r.top = y;
    r.right = x + 2;
    r.bottom = y + lh;
    return r;
}

void pane_insert(HWND h, const std::wstring& s) {
    pane.text.insert(pane.caret, s);
    pane.caret += s.size();
    if (pane.scroll == ScrollMode::OnInject) {
        // Deliberately scroll by a whole line per insertion: the resulting after-frame differs in
        // every row, which is the input source C is required to reject.
        HDC dc = GetDC(h);
        int lh = 16;
        if (dc) {
            HGDIOBJ old = SelectObject(dc, g.font);
            TEXTMETRICW tm{};
            GetTextMetricsW(dc, &tm);
            lh = static_cast<int>(tm.tmHeight + tm.tmExternalLeading);
            SelectObject(dc, old);
            ReleaseDC(h, dc);
        }
        pane.scroll_y += lh;
    }
    InvalidateRect(h, nullptr, TRUE);
}

void pane_paint(HWND h) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(h, &ps);
    RECT client{};
    GetClientRect(h, &client);

    HBRUSH bg = CreateSolidBrush(back_colour());
    FillRect(dc, &client, bg);
    DeleteObject(bg);

    HGDIOBJ old_font = SelectObject(dc, g.font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text_colour());
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    const int lh = static_cast<int>(tm.tmHeight + tm.tmExternalLeading);

    const std::vector<std::wstring> lines = pane_lines();
    for (size_t i = 0; i < lines.size(); ++i) {
        const int y = 4 + static_cast<int>(i) * lh - pane.scroll_y;
        if (y + lh < 0 || y > client.bottom) continue;
        TextOutW(dc, 4, y, lines[i].c_str(), static_cast<int>(lines[i].size()));
    }

    if (GetFocus() == h && (pane.steady_caret || pane.caret_visible)) {
        const RECT cr = pane_caret_rect(h);
        HBRUSH caret = CreateSolidBrush(text_colour());
        FillRect(dc, &cr, caret);
        DeleteObject(caret);
    }

    SelectObject(dc, old_font);
    EndPaint(h, &ps);
}

std::wstring clipboard_text_for_log(HWND owner) {
    std::wstring out;
    if (!OpenClipboard(owner)) return out;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h));
        if (p) {
            out.assign(p);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

LRESULT CALLBACK PaneProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            pane_paint(h);
            return 0;

        case WM_ERASEBKGND:
            return 1;  // painted in WM_PAINT; erasing here doubles the changed-pixel count

        case WM_SETFOCUS:
            // No CreateCaret. That absence is the feature — see the header comment.
            if (!pane.steady_caret) SetTimer(h, kBlinkTimer, GetCaretBlinkTime(), nullptr);
            pane.caret_visible = true;
            InvalidateRect(h, nullptr, TRUE);
            return 0;

        case WM_KILLFOCUS:
            KillTimer(h, kBlinkTimer);
            InvalidateRect(h, nullptr, TRUE);
            return 0;

        case WM_TIMER:
            if (wp == kBlinkTimer) {
                pane.caret_visible = !pane.caret_visible;
                InvalidateRect(h, nullptr, TRUE);
            } else if (wp == kScrollTimer) {
                pane.scroll_y += 1;
                InvalidateRect(h, nullptr, TRUE);
            }
            return 0;

        case WM_LBUTTONDOWN:
            SetFocus(h);
            return 0;

        case WM_GETDLGCODE:
            // Without this the dialog manager eats Tab, Return and the arrow keys before the
            // pane ever sees them, and the log would show characters vanishing that in fact
            // never arrived.
            return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS;

        case WM_KEYDOWN: {
            log_event(kIdSelfCaret, L"WM_KEYDOWN",
                      L"vk=" + hex32(static_cast<uint32_t>(wp)) + L" lparam=" +
                          hex32(static_cast<uint32_t>(lp)) + L" extra=" +
                          hex64(static_cast<uint64_t>(GetMessageExtraInfo())));
            if (wp == VK_LEFT && pane.caret > 0) --pane.caret;
            if (wp == VK_RIGHT && pane.caret < pane.text.size()) ++pane.caret;
            InvalidateRect(h, nullptr, TRUE);
            return 0;
        }

        case WM_CHAR: {
            const uint32_t cp = static_cast<uint32_t>(wp);
            if (cp == 0x16u) {
                // Ctrl+V reaches a plain window as WM_CHAR 0x16 (SYN), never as WM_PASTE: only
                // the edit controls synthesise WM_PASTE. Handling it here is what lets the
                // clipboard injection path be measured against a self-drawn target at all.
                const std::wstring clip = clipboard_text_for_log(h);
                log_event(kIdSelfCaret, L"WM_CHAR(0x16=Ctrl+V)",
                          L"clipboard CF_UNICODETEXT, " + std::to_wstring(clip.size()) +
                              L" utf-16 units");
                pane_insert(h, clip);
                return 0;
            }
            log_event(kIdSelfCaret, L"WM_CHAR",
                      codepoint_label(cp) + L" extra=" +
                          hex64(static_cast<uint64_t>(GetMessageExtraInfo())));
            if (cp == L'\r' || cp == L'\n') {
                pane_insert(h, L"\n");
            } else if (cp >= 0x20u || cp == L'\t') {
                pane_insert(h, std::wstring(1, static_cast<wchar_t>(cp)));
            }
            return 0;
        }

        case WM_UNICHAR: {
            if (wp == UNICODE_NOCHAR) return TRUE;  // "yes, this window accepts WM_UNICHAR"
            log_event(kIdSelfCaret, L"WM_UNICHAR", codepoint_label(static_cast<uint32_t>(wp)));
            return 0;
        }

        case WM_PASTE: {
            const std::wstring clip = clipboard_text_for_log(h);
            log_event(kIdSelfCaret, L"WM_PASTE",
                      std::to_wstring(clip.size()) + L" utf-16 units from clipboard");
            pane_insert(h, clip);
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ─── Subclass procedure for the stock controls ──────────────────────────────────────────────
//
// The stock controls handle their own input; we only observe. Returning DefSubclassProc for
// every message is load-bearing — swallowing anything here would make the target lie about what
// a real EDIT control does with our injected input, which is the one thing it must not do.

LRESULT CALLBACK ControlSubclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                                     DWORD_PTR ref) {
    (void)ref;
    const int control_id = static_cast<int>(id);
    switch (msg) {
        case WM_KEYDOWN:
            log_event(control_id, L"WM_KEYDOWN",
                      L"vk=" + hex32(static_cast<uint32_t>(wp)) + L" lparam=" +
                          hex32(static_cast<uint32_t>(lp)) + L" extra=" +
                          hex64(static_cast<uint64_t>(GetMessageExtraInfo())));
            break;
        case WM_CHAR:
            // GetMessageExtraInfo() here is the extra info of the last message this thread
            // retrieved, which for a WM_CHAR produced by TranslateMessage is the originating
            // WM_KEYDOWN's dwExtraInfo. That propagation is not something the documentation
            // promises, so it is logged as an observation rather than relied upon: if it turns
            // out to be stable, the injector's InjectionMark becomes visible at the target too.
            log_event(control_id, L"WM_CHAR",
                      codepoint_label(static_cast<uint32_t>(wp)) + L" extra=" +
                          hex64(static_cast<uint64_t>(GetMessageExtraInfo())));
            break;
        case WM_UNICHAR:
            if (wp != UNICODE_NOCHAR) {
                log_event(control_id, L"WM_UNICHAR", codepoint_label(static_cast<uint32_t>(wp)));
            }
            break;
        case WM_IME_CHAR:
            log_event(control_id, L"WM_IME_CHAR", codepoint_label(static_cast<uint32_t>(wp)));
            break;
        case WM_PASTE:
            log_event(control_id, L"WM_PASTE", L"(control asked to paste)");
            break;
        case WM_SETFOCUS:
            log_event(control_id, L"WM_SETFOCUS", L"");
            break;
        default:
            break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// ─── Log plumbing ───────────────────────────────────────────────────────────────────────────

void log_event(int control_id, const wchar_t* message, std::wstring detail) {
    LogEntry e;
    e.ms = now_ms();
    e.control_id = control_id;
    e.message = message;
    e.detail = std::move(detail);

    wchar_t line[512];
    swprintf(line, 512, L"[%7llu ms] %-16s %-22s %s", static_cast<unsigned long long>(e.ms),
             control_name(control_id), message, e.detail.c_str());

    g.log.push_back(std::move(e));
    ++g.log_total;
    while (g.log.size() > kMaxLogEntries) g.log.pop_front();

    if (g.log_visual && g.log_list) {
        const LRESULT count = SendMessageW(g.log_list, LB_GETCOUNT, 0, 0);
        if (count != LB_ERR && count > static_cast<LRESULT>(kMaxLogEntries)) {
            SendMessageW(g.log_list, LB_DELETESTRING, 0, 0);
        }
        const LRESULT idx =
            SendMessageW(g.log_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line));
        if (idx != LB_ERR) SendMessageW(g.log_list, LB_SETTOPINDEX, static_cast<WPARAM>(idx), 0);
    }
}

void clear_log() {
    g.log.clear();
    if (g.log_list) SendMessageW(g.log_list, LB_RESETCONTENT, 0, 0);
}

void copy_log_to_clipboard(HWND owner) {
    std::wstring all;
    for (const LogEntry& e : g.log) {
        wchar_t line[512];
        swprintf(line, 512, L"[%7llu ms] %s %s %s\r\n", static_cast<unsigned long long>(e.ms),
                 control_name(e.control_id), e.message.c_str(), e.detail.c_str());
        all += line;
    }
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    const size_t bytes = (all.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        void* p = GlobalLock(mem);
        if (p) {
            memcpy(p, all.c_str(), bytes);
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}

// ─── Reading a control's text without lying about it ────────────────────────────────────────
//
// Returns UTF-8. The conversion goes through kd::utf8::from_utf16, the *checked* form, because a
// clipboard payload or a mangled surrogate pair from a broken injection can leave an unpaired
// surrogate in the control — and "the text came back with an unpaired surrogate" is one of the
// exact defects docs/supported-targets.md §3.2 asks the `lossy` grade to record. Silently
// substituting U+FFFD here would erase the finding.
bool control_text_utf8(HWND control, std::string& out, bool& valid_utf16) {
    valid_utf16 = true;
    out.clear();
    if (!control) return false;
    const int len = GetWindowTextLengthW(control);
    if (len <= 0) {
        // GetWindowTextLength returns 0 for both "empty" and "cannot read"; an empty field is a
        // legitimate S01 state, so this is not an error.
        return true;
    }
    std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
    const int got = GetWindowTextW(control, buf.data(), len + 1);
    buf.resize(static_cast<size_t>(got < 0 ? 0 : got));
    valid_utf16 = kd::utf8::from_utf16(buf, out, nullptr);
    return true;
}

// ─── Side channel ───────────────────────────────────────────────────────────────────────────

HWND control_by_name(const std::string& name) {
    if (name == "edit-single") return g.edit_single;
    if (name == "edit-multiline") return g.edit_multi;
    if (name == "richedit") return g.rich;
    if (name == "edit-password") return g.edit_password;
    if (name == "edit-readonly") return g.edit_readonly;
    if (name == "edit-rtl") return g.edit_rtl;
    if (name == "self-caret-pane") return g.self_caret;
    return nullptr;
}

int control_id_of(HWND h) {
    if (!h) return 0;
    return static_cast<int>(GetDlgCtrlID(h));
}

kd::json::Value caret_report() {
    kd::json::Value caret = kd::json::Value::object();

    HWND focus = GetFocus();
    if (focus == g.self_caret) {
        // Programmatic ground truth: we drew this caret, so we know where it is to the pixel.
        RECT r = pane_caret_rect(g.self_caret);
        POINT tl{r.left, r.top};
        POINT br{r.right, r.bottom};
        ClientToScreen(g.self_caret, &tl);
        ClientToScreen(g.self_caret, &br);
        caret.set("available", kd::json::Value::boolean(true));
        caret.set("self_drawn", kd::json::Value::boolean(true));
        caret.set("system_caret_present", kd::json::Value::boolean(false));
        kd::json::Value rect = kd::json::Value::array();
        rect.push_back(kd::json::Value::number(static_cast<int64_t>(tl.x)));
        rect.push_back(kd::json::Value::number(static_cast<int64_t>(tl.y)));
        rect.push_back(kd::json::Value::number(static_cast<int64_t>(br.x)));
        rect.push_back(kd::json::Value::number(static_cast<int64_t>(br.y)));
        caret.set("screen_rect", std::move(rect));
        return caret;
    }

    // Source A, read on our own thread. Two things the probe needs and cannot get from its own
    // side: the raw rcCaret *before* any coordinate conversion, and hwndCaret. Reporting both
    // closes docs/supported-targets.md §9 open question 2 ("rcCaret coordinate space")
    // empirically rather than by reading the documentation twice.
    GUITHREADINFO gti{};
    gti.cbSize = sizeof(gti);
    const BOOL ok = GetGUIThreadInfo(GetCurrentThreadId(), &gti);
    caret.set("available", kd::json::Value::boolean(ok != 0 && (gti.rcCaret.right > gti.rcCaret.left ||
                                                               gti.rcCaret.bottom > gti.rcCaret.top)));
    caret.set("self_drawn", kd::json::Value::boolean(false));
    caret.set("system_caret_present", kd::json::Value::boolean(ok != 0 && gti.hwndCaret != nullptr));
    if (ok) {
        kd::json::Value raw = kd::json::Value::array();
        raw.push_back(kd::json::Value::number(static_cast<int64_t>(gti.rcCaret.left)));
        raw.push_back(kd::json::Value::number(static_cast<int64_t>(gti.rcCaret.top)));
        raw.push_back(kd::json::Value::number(static_cast<int64_t>(gti.rcCaret.right)));
        raw.push_back(kd::json::Value::number(static_cast<int64_t>(gti.rcCaret.bottom)));
        caret.set("raw_rc_caret", std::move(raw));
        caret.set("hwnd_caret", kd::json::Value::string(
                                    kd::utf8::from_utf16_or_empty(hex_ptr(gti.hwndCaret))));

        RECT r = gti.rcCaret;
        if (gti.hwndCaret) {
            MapWindowPoints(gti.hwndCaret, HWND_DESKTOP, reinterpret_cast<POINT*>(&r), 2);
        }
        kd::json::Value rect = kd::json::Value::array();
        rect.push_back(kd::json::Value::number(static_cast<int64_t>(r.left)));
        rect.push_back(kd::json::Value::number(static_cast<int64_t>(r.top)));
        rect.push_back(kd::json::Value::number(static_cast<int64_t>(r.right)));
        rect.push_back(kd::json::Value::number(static_cast<int64_t>(r.bottom)));
        caret.set("screen_rect", std::move(rect));
    }
    return caret;
}

std::string build_state_json() {
    kd::json::Value root = kd::json::Value::object();
    root.set("tool", kd::json::Value::string("kd_testtarget"));
    root.set("schema", kd::json::Value::string("kd-testtarget-state/1"));
    root.set("hwnd", kd::json::Value::string(kd::utf8::from_utf16_or_empty(hex_ptr(g.main))));
    root.set("pid", kd::json::Value::number(static_cast<int64_t>(GetCurrentProcessId())));
    root.set("uptime_ms", kd::json::Value::number(static_cast<int64_t>(now_ms())));
    root.set("marker", kd::json::Value::string(kd::utf8::from_utf16_or_empty(g.marker)));

    HWND focus = GetFocus();
    const int fid = control_id_of(focus);
    kd::json::Value f = kd::json::Value::object();
    f.set("hwnd", kd::json::Value::string(kd::utf8::from_utf16_or_empty(hex_ptr(focus))));
    f.set("id", kd::json::Value::number(static_cast<int64_t>(fid)));
    f.set("name", kd::json::Value::string(kd::utf8::from_utf16_or_empty(control_name(fid))));
    f.set("is_password", kd::json::Value::boolean(fid == kIdEditPassword));
    f.set("is_readonly", kd::json::Value::boolean(fid == kIdEditReadOnly));
    f.set("is_rtl", kd::json::Value::boolean(fid == kIdEditRtl));
    f.set("is_self_drawn_caret", kd::json::Value::boolean(fid == kIdSelfCaret));
    f.set("line_height_px",
          kd::json::Value::number(static_cast<int64_t>(fid == kIdSelfCaret
                                                           ? line_height_of(g.self_caret)
                                                           : line_height_of(focus))));
    // Where the caret is IN THE TEXT, in UTF-16 code units, as distinct from where it is on
    // screen. Four scenarios were arranged with EM_SETSEL(-1, -1) in the belief that it moved
    // the caret to the end; it does not, and because nothing reported the offset, the only
    // evidence that they had all injected at offset 0 instead was buried in a readback string
    // inside a result file nobody re-read. A harness that reports where it put the caret can be
    // checked before it measures anything.
    {
        int64_t sel_start = 0, sel_end = 0;
        if (fid == kIdSelfCaret) {
            sel_start = sel_end = static_cast<int64_t>(pane.caret);
        } else if (focus) {
            DWORD a = 0, b = 0;
            SendMessageW(focus, EM_GETSEL, reinterpret_cast<WPARAM>(&a),
                         reinterpret_cast<LPARAM>(&b));
            sel_start = static_cast<int64_t>(a);
            sel_end = static_cast<int64_t>(b);
        }
        f.set("sel_start", kd::json::Value::number(sel_start));
        f.set("sel_end", kd::json::Value::number(sel_end));
        f.set("text_length", kd::json::Value::number(static_cast<int64_t>(
                                 fid == kIdSelfCaret ? pane.text.size()
                                 : focus             ? static_cast<size_t>(GetWindowTextLengthW(focus))
                                                     : 0u)));
    }
    root.set("focus", std::move(f));

    std::string text;
    bool valid = true;
    if (focus == g.self_caret) {
        valid = kd::utf8::from_utf16(pane.text, text, nullptr);
    } else {
        control_text_utf8(focus, text, valid);
    }
    root.set("text", kd::json::Value::string(text));
    root.set("text_valid_utf16", kd::json::Value::boolean(valid));

    root.set("caret", caret_report());
    root.set("theme", kd::json::Value::string(g.dark ? "dark" : "light"));
    root.set("log_visual", kd::json::Value::boolean(g.log_visual));
    root.set("font_pt", kd::json::Value::number(static_cast<int64_t>(g.large_font ? 20 : 10)));
    root.set("dpi", kd::json::Value::number(static_cast<int64_t>(g.dpi)));
    root.set("scroll_mode",
             kd::json::Value::string(pane.scroll == ScrollMode::None      ? "none"
                                     : pane.scroll == ScrollMode::OnInject ? "on-inject"
                                                                           : "timer"));
    root.set("caret_blink", kd::json::Value::string(pane.steady_caret ? "steady" : "blinking"));
    root.set("system_caret_blink_ms",
             kd::json::Value::number(static_cast<int64_t>(GetCaretBlinkTime())));

    kd::json::Value events = kd::json::Value::array();
    for (const LogEntry& e : g.log) {
        kd::json::Value ev = kd::json::Value::object();
        ev.set("ms", kd::json::Value::number(static_cast<int64_t>(e.ms)));
        ev.set("control",
               kd::json::Value::string(kd::utf8::from_utf16_or_empty(control_name(e.control_id))));
        ev.set("msg", kd::json::Value::string(kd::utf8::from_utf16_or_empty(e.message)));
        ev.set("detail", kd::json::Value::string(kd::utf8::from_utf16_or_empty(e.detail)));
        events.push_back(std::move(ev));
    }
    root.set("events", std::move(events));
    // Total ever logged, so a probe that sees fewer events than it injected characters can tell
    // "the target dropped them" from "the bounded log trimmed them".
    root.set("events_total", kd::json::Value::number(static_cast<int64_t>(g.log_total)));

    return kd::json::serialize(root);
}

void handle_command(const std::string& cmd) {
    if (cmd == "reset-log") {
        clear_log();
        return;
    }
    if (cmd.rfind("marker:", 0) == 0) {
        g.marker = kd::utf8::to_utf16_or_empty(cmd.substr(7));
        return;
    }
    if (cmd.rfind("log-visual:", 0) == 0) {
        // "log-visual:off" during a measurement, "on" to get the pane back. The probe sets this
        // automatically for A09; a human debugging by eye wants it on.
        const bool on = cmd.substr(11) != "off";
        if (on != g.log_visual) {
            g.log_visual = on;
            if (!on && g.log_list) {
                // Clear what is already drawn as well: leaving several hundred stale lines on
                // screen would still be in the capture, just not changing.
                SendMessageW(g.log_list, LB_RESETCONTENT, 0, 0);
                InvalidateRect(g.log_list, nullptr, TRUE);
                UpdateWindow(g.log_list);
            }
        }
        return;
    }
    if (cmd.rfind("focus:", 0) == 0) {
        HWND h = control_by_name(cmd.substr(6));
        if (h) SetFocus(h);
        return;
    }
    if (cmd.rfind("set-text:", 0) == 0) {
        HWND h = GetFocus();
        const std::wstring w = kd::utf8::to_utf16_or_empty(cmd.substr(9));
        if (h == g.self_caret) {
            pane.text = w;
            pane.caret = w.size();
            pane.scroll_y = 0;
            InvalidateRect(h, nullptr, TRUE);
        } else if (h) {
            SetWindowTextW(h, w.c_str());
            SendMessageW(h, EM_SETSEL, static_cast<WPARAM>(w.size()),
                         static_cast<LPARAM>(w.size()));
        }
        return;
    }
    if (cmd.rfind("select:", 0) == 0) {
        // "select:<start>:<end>" in UTF-16 code units — scenario S03 (selection replacement)
        // needs a selection that is set the same way every repetition.
        const size_t colon = cmd.find(':', 7);
        if (colon != std::string::npos) {
            const int a = std::atoi(cmd.substr(7, colon - 7).c_str());
            const int b = std::atoi(cmd.substr(colon + 1).c_str());
            HWND h = GetFocus();
            if (h && h != g.self_caret) {
                SendMessageW(h, EM_SETSEL, static_cast<WPARAM>(a), static_cast<LPARAM>(b));
            }
        }
        return;
    }
    // "state" and anything unrecognised fall through to a plain state report.
}

LRESULT handle_copydata(HWND self, WPARAM wp, LPARAM lp) {
    const COPYDATASTRUCT* cds = reinterpret_cast<const COPYDATASTRUCT*>(lp);
    if (!cds || cds->dwData != kSideChannelRequest) return 0;

    std::string cmd;
    if (cds->lpData && cds->cbData > 0) {
        cmd.assign(static_cast<const char*>(cds->lpData), cds->cbData);
        while (!cmd.empty() && cmd.back() == '\0') cmd.pop_back();
    }
    handle_command(cmd);

    const std::string reply = build_state_json();
    HWND requester = reinterpret_cast<HWND>(wp);
    if (requester && IsWindow(requester)) {
        COPYDATASTRUCT out{};
        out.dwData = kSideChannelReply;
        out.cbData = static_cast<DWORD>(reply.size());
        out.lpData = const_cast<char*>(reply.c_str());
        DWORD_PTR unused = 0;
        SendMessageTimeoutW(requester, WM_COPYDATA, reinterpret_cast<WPARAM>(self),
                            reinterpret_cast<LPARAM>(&out), SMTO_ABORTIFHUNG, 2000, &unused);
    }
    return 1;
}

// ─── Window construction and layout ─────────────────────────────────────────────────────────

HWND make_label(HWND parent, HINSTANCE inst, const wchar_t* text) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10,
                           parent, nullptr, inst, nullptr);
}

struct Row {
    HWND label = nullptr;
    HWND control = nullptr;
    int height = 0;
};

std::vector<Row> rows;

void layout(HWND h) {
    RECT c{};
    GetClientRect(h, &c);
    const int margin = MulDiv(8, static_cast<int>(g.dpi), 96);
    const int label_h = MulDiv(16, static_cast<int>(g.dpi), 96);
    int y = margin;
    const int x = margin;
    const int w = c.right - c.left - 2 * margin;

    for (const Row& r : rows) {
        if (r.label) {
            MoveWindow(r.label, x, y, w, label_h, TRUE);
            y += label_h;
        }
        const int hh = MulDiv(r.height, static_cast<int>(g.dpi), 96);
        MoveWindow(r.control, x, y, w, hh, TRUE);
        y += hh + margin / 2;
    }

    if (g.log_list) {
        const int rest = c.bottom - y - margin;
        MoveWindow(g.log_list, x, y, w, rest > 40 ? rest : 40, TRUE);
    }
}

void build_menu(HWND h) {
    HMENU bar = CreateMenu();

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, kIdmThemeDark, L"&Dark background\tS11");
    AppendMenuW(view, MF_STRING, kIdmThemeLight, L"&Light background\tS12");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, kIdmFontLarge, L"L&arge font (20pt)");
    AppendMenuW(view, MF_STRING, kIdmFontSmall, L"S&mall font (10pt)");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");

    HMENU caret = CreatePopupMenu();
    AppendMenuW(caret, MF_STRING, kIdmCaretBlink, L"Self-drawn caret: &blinking");
    AppendMenuW(caret, MF_STRING, kIdmCaretSteady, L"Self-drawn caret: &steady\tS10");
    AppendMenuW(caret, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(caret, MF_STRING, kIdmCaretFocusPane, L"&Focus the self-drawn-caret pane");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(caret), L"&Caret");

    HMENU scroll = CreatePopupMenu();
    AppendMenuW(scroll, MF_STRING, kIdmScrollNone, L"Pane scroll: &none");
    AppendMenuW(scroll, MF_STRING, kIdmScrollOnInject, L"Pane scroll: on each &insertion\tS07");
    AppendMenuW(scroll, MF_STRING, kIdmScrollTimer, L"Pane scroll: continuous &timer");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(scroll), L"&Scroll");

    HMENU text = CreatePopupMenu();
    AppendMenuW(text, MF_STRING, kIdmTextClear, L"&Clear every field\tS01");
    AppendMenuW(text, MF_STRING, kIdmTextMidLine, L"&Mid-line text + caret in the middle\tS02");
    AppendMenuW(text, MF_STRING, kIdmTextLongLine, L"&Long unwrapped line\tS08");
    AppendMenuW(text, MF_STRING, kIdmTextManyLines, L"Many lines (&vertical scroll)\tS07");
    AppendMenuW(text, MF_STRING, kIdmTextRtl, L"&RTL sample into the RTL field\tS16");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(text), L"&Text");

    HMENU logm = CreatePopupMenu();
    AppendMenuW(logm, MF_STRING, kIdmLogClear, L"&Clear log");
    AppendMenuW(logm, MF_STRING, kIdmLogCopy, L"C&opy log to clipboard");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(logm), L"&Log");

    SetMenu(h, bar);
}

// EM_SETSEL(-1, -1) does NOT put the caret at the end of an edit control. wParam = -1 means
// "deselect"; the caret is left wherever the control already had it, which straight after
// SetWindowTextW is offset 0. Four scenarios were arranged that way and every one of them
// injected at the START of the text: S04 never approached the wrap point, S07 and S08 never
// scrolled (the control was already showing offset 0, so there was nothing to scroll), and S16
// inserted its LTR run before the RTL text rather than after it. Ask for the length and select
// an empty range at it; that is unambiguous.
void caret_to_end(HWND h) {
    if (!h) return;
    const int len = GetWindowTextLengthW(h);
    SendMessageW(h, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
}

// Every branch must re-arm the pane completely: text, caret AND scroll offset. Leaving
// pane.scroll_y alone is how a scroll leaked out of one scenario into the next — S07 turns on
// ScrollMode::OnInject, S10 ran after it without turning it off, and 25 injected characters
// walked the pane's content 25 line heights off the top of the window.
void pane_set(const std::wstring& text, size_t caret) {
    pane.text = text;
    pane.caret = caret > text.size() ? text.size() : caret;
    pane.scroll_y = 0;
    InvalidateRect(g.self_caret, nullptr, TRUE);
}

// `prefix` padded with 'x' until it very nearly fills one wrapped line of `control`. The margin
// is a handful of average character widths, so that any non-empty injection at the end of it
// wraps, while the line itself does not wrap before the injection arrives.
std::wstring pad_to_near_width(HWND control, const std::wstring& prefix) {
    std::wstring out = prefix;
    if (!control) return out;
    RECT client{};
    GetClientRect(control, &client);
    HDC dc = GetDC(control);
    if (!dc) return out;
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    HGDIOBJ old = font ? SelectObject(dc, font) : nullptr;
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    // The EDIT keeps a small internal margin and, here, a vertical scrollbar; 4 average
    // characters of slack covers both without needing either exactly.
    const int limit = static_cast<int>(client.right - client.left) - tm.tmAveCharWidth * 4;
    // Pad in whole words rather than characters. A single unbroken run would force the EDIT to
    // break mid-word and shove the entire run onto the next line, which is a much larger change
    // than the one visual line S07 is meant to add.
    const std::wstring unit = L"pad ";
    SIZE sz{0, 0};
    while (out.size() < 4096) {
        const std::wstring next = out + unit;
        GetTextExtentPoint32W(dc, next.c_str(), static_cast<int>(next.size()), &sz);
        if (sz.cx >= limit) break;
        out = next;
    }
    if (old) SelectObject(dc, old);
    ReleaseDC(control, dc);
    return out;
}

void fill_scenario_text(UINT which) {
    const wchar_t* mid = L"the quick brown fox | jumps over the lazy dog";
    const wchar_t* long_line =
        L"C:\\apps\\llama-swap\\models\\qwen3-27b-mtp.gguf --n-gpu-layers 999 --ctx-size 32768 "
        L"--flash-attn --parallel 4 --host 127.0.0.1 --port 8080 --alias qwen3-27b-mtp";
    switch (which) {
        case kIdmTextClear: {
            const HWND all[] = {g.edit_single, g.edit_multi, g.rich, g.edit_password,
                                g.edit_rtl};
            for (HWND h : all) {
                if (h) SetWindowTextW(h, L"");
            }
            pane_set(L"", 0);
            break;
        }
        case kIdmTextMidLine: {
            const HWND all[] = {g.edit_single, g.edit_multi, g.rich, g.edit_rtl};
            for (HWND h : all) {
                if (!h) continue;
                SetWindowTextW(h, mid);
                SendMessageW(h, EM_SETSEL, 20, 20);
            }
            pane_set(mid, 20);
            break;
        }
        case kIdmTextLongLine: {
            // Both edits, not only the single-line one. S08 measures the long line in
            // g.edit_single and S04 measures the same line wrapping in g.edit_multi; filling
            // only the former left S04's control holding whatever the previous repetition had
            // typed into it. That is why S04's third repetition drifted to 49.95 line heights
            // while the first two reproduced to the pixel — by then the accumulated text had
            // wrapped, and the scenario under measurement was no longer S04.
            const std::wstring line = long_line;
            SetWindowTextW(g.edit_single, long_line);
            caret_to_end(g.edit_single);
            SetWindowTextW(g.edit_multi, long_line);
            caret_to_end(g.edit_multi);
            pane_set(line, line.size());
            break;
        }
        case kIdmTextManyLines: {
            std::wstring many;
            for (int i = 0; i < 60; ++i) {
                many += L"line ";
                many += std::to_wstring(i);
                many += L" — padding padding padding\r\n";
            }
            // 60 short lines scrolled to the bottom are NOT enough to make S07 a scroll: an
            // insertion at the end of the last one changes that line and nothing else, so the
            // after-frame shows a normal append and `looks_like_scroll` is false — which is what
            // the first three repetitions recorded, and it was the arrangement being wrong
            // rather than the detector. (The ScrollOnInject menu item, which S07 also set, only
            // ever drives the self-drawn pane; it cannot scroll an EDIT.) The last line is
            // therefore padded to just under the control's width, so REF-2 wraps it onto a new
            // visual line and a control already showing its bottom has to scroll up by one.
            many += pad_to_near_width(g.edit_multi, L"last ");
            SetWindowTextW(g.edit_multi, many.c_str());
            caret_to_end(g.edit_multi);
            SendMessageW(g.edit_multi, EM_SCROLLCARET, 0, 0);
            // The pane gets the same content so it is not left holding the previous scenario's,
            // but note it has no EM_SCROLLCARET equivalent: at scroll_y = 0 its caret sits below
            // the visible area. S07 targets g.edit_multi, so that does not affect a measurement
            // today; a pane-targeted scroll scenario would have to scroll to the caret first.
            pane_set(many, many.size());
            break;
        }
        case kIdmTextRtl:
            SetWindowTextW(g.edit_rtl, L"العربية עברית mixed with latin");
            caret_to_end(g.edit_rtl);
            break;
        default:
            break;
    }
}

LRESULT CALLBACK MainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            HINSTANCE inst = reinterpret_cast<LPCREATESTRUCTW>(lp)->hInstance;
            g.main = h;
            g.dpi = GetDpiForWindow(h);

            auto add = [&](const wchar_t* label, HWND control, int height) {
                Row r;
                r.label = make_label(h, inst, label);
                r.control = control;
                r.height = height;
                rows.push_back(r);
            };

            g.edit_single = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10, h,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdEditSingle)), inst, nullptr);
            add(L"classic EDIT, single line (ES_AUTOHSCROLL)", g.edit_single, 24);

            g.edit_multi = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
                    ES_WANTRETURN,
                0, 0, 10, 10, h,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdEditMulti)), inst, nullptr);
            add(L"classic EDIT, multiline + word wrap (line-wrap case S04)", g.edit_multi, 90);

            // Msftedit.dll is loaded at run time and never freed: unloading a module that owns a
            // registered window class while windows of that class exist is a crash, and this
            // process lives exactly as long as the operator needs it.
            if (LoadLibraryW(L"Msftedit.dll")) {
                g.rich = CreateWindowExW(
                    WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE |
                        ES_AUTOVSCROLL,
                    0, 0, 10, 10, h,
                    reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdRichEdit)), inst, nullptr);
                if (g.rich) {
                    // Without ENM_KEYEVENTS a RichEdit still receives WM_CHAR itself; the mask
                    // only governs what it notifies the parent about. We subclass instead, so
                    // this is only here to make the intent explicit for anyone who extends it.
                    SendMessageW(g.rich, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_KEYEVENTS);
                }
            }
            if (g.rich) {
                add(L"RICHEDIT50W (Msftedit.dll)", g.rich, 70);
            }

            g.edit_password = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL, 0, 0, 10, 10, h,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdEditPassword)), inst, nullptr);
            add(L"ES_PASSWORD — the positive control for the refusal path (S17)", g.edit_password,
                24);

            g.edit_readonly = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"read-only: injection must change nothing here",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_READONLY | ES_AUTOHSCROLL, 0, 0, 10, 10, h,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdEditReadOnly)), inst, nullptr);
            add(L"ES_READONLY — accepts focus, must accept no text", g.edit_readonly, 24);

            g.edit_rtl = CreateWindowExW(
                WS_EX_CLIENTEDGE | WS_EX_RTLREADING | WS_EX_RIGHT, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_RIGHT | ES_AUTOHSCROLL, 0, 0, 10, 10, h,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdEditRtl)), inst, nullptr);
            add(L"RTL reading order — insertion point is at the LEFT edge (S16)", g.edit_rtl, 24);

            g.self_caret = CreateWindowExW(
                WS_EX_CLIENTEDGE, kPaneClass, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10,
                h, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdSelfCaret)), inst, nullptr);
            add(L"self-drawn caret pane — CreateCaret is NEVER called, so rcCaret is empty (source A miss)",
                g.self_caret, 110);

            g.log_list = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT |
                    LBS_NOSEL,
                0, 0, 10, 10, h, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdLog)), inst,
                nullptr);

            const int subclassed[] = {kIdEditSingle,   kIdEditMulti,    kIdRichEdit,
                                      kIdEditPassword, kIdEditReadOnly, kIdEditRtl};
            for (int id : subclassed) {
                HWND c = GetDlgItem(h, id);
                if (c) {
                    SetWindowSubclass(c, ControlSubclassProc, static_cast<UINT_PTR>(id), 0);
                }
            }

            build_menu(h);
            rebuild_fonts();
            apply_theme();
            layout(h);
            SetFocus(g.edit_single);
            log_event(0, L"startup",
                      L"kd_testtarget ready; side channel class=" + std::wstring(kWindowClass));
            return 0;
        }

        case WM_SIZE:
            layout(h);
            return 0;

        case WM_DPICHANGED: {
            g.dpi = HIWORD(wp);
            const RECT* suggested = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(h, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            rebuild_fonts();
            layout(h);
            log_event(0, L"WM_DPICHANGED", L"dpi=" + std::to_wstring(g.dpi));
            return 0;
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORLISTBOX: {
            if (!g.bg_brush) break;
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, text_colour());
            SetBkColor(dc, back_colour());
            return reinterpret_cast<LRESULT>(g.bg_brush);
        }

        case WM_ERASEBKGND: {
            if (!g.bg_brush) break;  // before apply_theme() ran; let DefWindowProc handle it
            RECT c{};
            GetClientRect(h, &c);
            FillRect(reinterpret_cast<HDC>(wp), &c, g.bg_brush);
            return 1;
        }

        case WM_COMMAND: {
            const UINT id = LOWORD(wp);
            switch (id) {
                case kIdmThemeDark:
                case kIdmThemeLight:
                    g.dark = (id == kIdmThemeDark);
                    apply_theme();
                    log_event(0, L"theme", g.dark ? L"dark" : L"light");
                    return 0;
                case kIdmFontLarge:
                case kIdmFontSmall:
                    g.large_font = (id == kIdmFontLarge);
                    rebuild_fonts();
                    layout(h);
                    InvalidateRect(h, nullptr, TRUE);
                    log_event(0, L"font",
                              std::to_wstring(g.large_font ? 20 : 10) + L"pt, line height " +
                                  std::to_wstring(line_height_of(g.edit_single)) + L"px");
                    return 0;
                case kIdmCaretBlink:
                case kIdmCaretSteady:
                    pane.steady_caret = (id == kIdmCaretSteady);
                    if (pane.steady_caret) {
                        KillTimer(g.self_caret, kBlinkTimer);
                        pane.caret_visible = true;
                    } else if (GetFocus() == g.self_caret) {
                        SetTimer(g.self_caret, kBlinkTimer, GetCaretBlinkTime(), nullptr);
                    }
                    InvalidateRect(g.self_caret, nullptr, TRUE);
                    return 0;
                case kIdmCaretFocusPane:
                    SetFocus(g.self_caret);
                    return 0;
                case kIdmScrollNone:
                case kIdmScrollOnInject:
                case kIdmScrollTimer:
                    KillTimer(g.self_caret, kScrollTimer);
                    pane.scroll = (id == kIdmScrollNone)      ? ScrollMode::None
                                  : (id == kIdmScrollOnInject) ? ScrollMode::OnInject
                                                               : ScrollMode::Timer;
                    if (pane.scroll == ScrollMode::Timer) {
                        SetTimer(g.self_caret, kScrollTimer, 33, nullptr);
                    }
                    return 0;
                case kIdmLogClear:
                    clear_log();
                    return 0;
                case kIdmLogCopy:
                    copy_log_to_clipboard(h);
                    return 0;
                case kIdmTextClear:
                case kIdmTextMidLine:
                case kIdmTextLongLine:
                case kIdmTextManyLines:
                case kIdmTextRtl:
                    fill_scenario_text(id);
                    return 0;
                default:
                    break;
            }
            return 0;
        }

        case WM_COPYDATA:
            return handle_copydata(h, wp, lp);

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Per-monitor DPI awareness v2, resolved at run time.
//
// The product gets this from an application manifest (see the TODO in CMakeLists.txt), which is
// the right mechanism because it applies before any window exists. This tool has no manifest, so
// it calls the API — and it resolves the entry point dynamically rather than linking it, so that
// a build against an older SDK still produces a running tool instead of a link error. Without
// v2, every rectangle this target reports would be in virtualised coordinates and the probe's
// mixed-DPI row (S13) would measure the DPI virtualisation layer rather than the anchor.
void enable_per_monitor_dpi_v2() {
    using SetCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;
    auto fn = reinterpret_cast<SetCtxFn>(
        reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
    if (fn) fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

}  // namespace

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance,
                      _In_ LPWSTR lpCmdLine, _In_ int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    enable_per_monitor_dpi_v2();
    g.start_tick = GetTickCount64();

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW pane_cls{};
    pane_cls.cbSize = sizeof(pane_cls);
    pane_cls.lpfnWndProc = PaneProc;
    pane_cls.hInstance = hInstance;
    pane_cls.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    pane_cls.lpszClassName = kPaneClass;
    if (!RegisterClassExW(&pane_cls)) return 1;

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = MainProc;
    cls.hInstance = hInstance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&cls)) return 1;

    HWND h = CreateWindowExW(0, kWindowClass, L"kd_testtarget — Phase 0B calibration target",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 980, 820, nullptr,
                             nullptr, hInstance, nullptr);
    if (!h) return 1;

    ShowWindow(h, nCmdShow);
    UpdateWindow(h);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // IsDialogMessage is deliberately NOT used: it would consume Tab and Return before the
        // controls saw them, and this target exists to report exactly what it was sent.
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g.font) DeleteObject(g.font);
    if (g.log_font) DeleteObject(g.log_font);
    if (g.bg_brush) DeleteObject(g.bg_brush);
    return 0;
}
