// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Text injection into the focused window, and the guards that decide when not to.
//
// Specification: injector.h. Implements plan.md §1.7, and the plan.md §7 gates that read
// "no wrong-window injection under automated focus-race tests", "exact Unicode injection in every
// declared supported target; explicit failure state for unsupported and elevated ones", and
// "clipboard restored only when we still own the state we set".
//
// ─── Why so much of this file is refusal ────────────────────────────────────────────────────
//
// The dangerous operation here is not typing; it is typing into the *wrong* place. Every guard
// below exists because the failure it prevents is silent: UIPI drops our input without saying so,
// a focus change redirects a sentence into a chat box, a password field renders dots while
// happily accepting synthetic keystrokes, and a clipboard restore run one moment too late
// destroys something the user copied. None of those announce themselves, so all of them are
// checked before the fact rather than detected after it.
//
// ─── Threading ──────────────────────────────────────────────────────────────────────────────
//
// Everything in this file runs on the UI thread (injector.h's threading section says why:
// clipboard calls are thread- and window-affine, and pacing waits would stall a hook thread into
// being silently unhooked). The one process-wide static with thread affinity is the clipboard
// owner window; see clipboard_owner_window().

#define KD_LOG_MODULE "inject"

#include "injector.h"

#include <windows.h>

#include <bcrypt.h>

#include <cstring>
#include <string>
#include <vector>

#include "util/log.h"
#include "util/utf8.h"

namespace kd {

// ════════════════════════════════════════════════════════════════════════════════════════════
//  injector_detail — the parts that are testable without a foreign window
// ════════════════════════════════════════════════════════════════════════════════════════════
//
// These are not in injector.h on purpose: injector.h is a Windows-free header (project rule) and
// widening it during a parallel implementation pass would mean editing a file another agent may
// hold. tests/test_injector.cpp therefore re-declares this block verbatim.
//
// **Keep the two copies token-identical.** At integration time the right home for this block is
// either injector.h itself (the types below are already Windows-free) or a new
// src/injector_detail.h; see the note in the module report.

namespace injector_detail {

// One synthetic keyboard event, in Windows-free terms. `flags` carries the KEYEVENTF_* bits and
// `extra` the dwExtraInfo mark, so the whole event sequence can be asserted on in a unit test
// without SendInput ever being called.
struct SynthKey {
    uint16_t vk = 0;
    uint16_t scan = 0;
    uint32_t flags = 0;
    uintptr_t extra = 0;
};

// The events produced by one input character, kept as a unit. Two invariants depend on this
// grouping: a pacing batch boundary must never fall inside a surrogate pair or between a key-down
// and its key-up, and a partially-accepted SendInput must be attributed to whole characters so
// that InjectResult::bytes_injected is a real UTF-8 prefix length and not an estimate.
struct SynthAtom {
    uint32_t first_event = 0;
    uint32_t event_count = 0;
    uint32_t utf8_bytes = 0;
};

// Bits for build_paste_chord()'s `stuck_mods`. There is deliberately no Win bit — see the comment
// in build_paste_chord().
inline constexpr uint32_t kStuckShift = 1u << 0;
inline constexpr uint32_t kStuckAlt = 1u << 1;

bool build_text_events(std::string_view utf8, uintptr_t extra, std::vector<SynthKey>& events,
                       std::vector<SynthAtom>& atoms, size_t* bad_offset, const char** reason);

void build_paste_chord(bool ctrl_shift_v, uint32_t stuck_mods, uintptr_t extra,
                       std::vector<SynthKey>& events);

IntegrityLevel integrity_from_rid(uint32_t rid) noexcept;
int integrity_rank(IntegrityLevel l) noexcept;
bool integrity_allows_injection(IntegrityLevel ours, IntegrityLevel target) noexcept;
IntegrityLevel process_integrity(uint32_t pid, uint32_t* os_error) noexcept;
IntegrityLevel current_process_integrity() noexcept;

struct ClipboardBackup;
ClipboardBackup* clipboard_backup_create();
void clipboard_backup_destroy(ClipboardBackup* b) noexcept;
size_t clipboard_backup_format_count(const ClipboardBackup* b) noexcept;
size_t clipboard_backup_lost_format_count(const ClipboardBackup* b) noexcept;

bool clipboard_save(ClipboardBackup* b, ClipboardGuardState& st, uint32_t* os_error);
bool clipboard_set_utf8(std::string_view utf8, ClipboardGuardState& st, uint32_t* os_error);
bool clipboard_set_delayed(std::string_view utf8, std::wstring& out_utf16,
                           ClipboardGuardState& st, uint32_t* os_error);
void clipboard_restore(ClipboardBackup* b, ClipboardGuardState& st, std::wstring_view ours);

// The deferred restore: armed when a paste goes out, disarmed by whichever comes first — the
// target reading our text, the fallback timeout, a second paste, or a caller that wants our text
// to stay on the clipboard. See "Delayed rendering" below.
void paste_watch_arm(ClipboardBackup* backup, std::wstring text, ClipboardGuardState* report_to,
                     uint32_t sequence_at_set, uint32_t target_pid, uint32_t settle_ms,
                     uint32_t fallback_ms);
ClipboardBackup* paste_watch_adopt();
void paste_watch_cancel(const char* why);
bool paste_watch_run_now(const char* why, bool may_defer);
bool paste_watch_active();
void paste_watch_forget_reporter(const ClipboardGuardState* st);

uintptr_t clipboard_owner_window();

}  // namespace injector_detail

// ════════════════════════════════════════════════════════════════════════════════════════════
//  Small local helpers
// ════════════════════════════════════════════════════════════════════════════════════════════

namespace {

// Ordinal, case-insensitive. Not _wcsicmp: that folds case per the CRT locale, and a window class
// name is an ordinal token, not text in a language.
bool iequals_w(const wchar_t* a, const wchar_t* b) noexcept {
    if (a == nullptr || b == nullptr) return a == b;
    return CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
}

bool istarts_with_w(const wchar_t* s, const wchar_t* prefix) noexcept {
    if (s == nullptr || prefix == nullptr) return false;
    const int n = static_cast<int>(wcslen(prefix));
    if (static_cast<int>(wcslen(s)) < n) return false;
    return CompareStringOrdinal(s, n, prefix, n, TRUE) == CSTR_EQUAL;
}

std::string ascii_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// RAII. Every acquired resource in this file is released through one of these, never through a
// CloseHandle at the bottom of a function that can early-return.
struct HandleGuard {
    HANDLE h = nullptr;
    explicit HandleGuard(HANDLE handle = nullptr) noexcept : h(handle) {}
    ~HandleGuard() {
        if (h != nullptr && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    [[nodiscard]] bool valid() const noexcept { return h != nullptr && h != INVALID_HANDLE_VALUE; }
};

struct DesktopGuard {
    HDESK h = nullptr;
    explicit DesktopGuard(HDESK d = nullptr) noexcept : h(d) {}
    ~DesktopGuard() {
        if (h != nullptr) CloseDesktop(h);
    }
    DesktopGuard(const DesktopGuard&) = delete;
    DesktopGuard& operator=(const DesktopGuard&) = delete;
};

// Closes the clipboard on every path out of the scope, including the ones that return early
// because SetClipboardData failed. Leaving the clipboard open hangs every other application on
// the desktop that tries to read it, which is a far louder bug than the one that caused it.
struct ClipboardCloser {
    bool open = false;
    ~ClipboardCloser() {
        if (open) CloseClipboard();
    }
    ClipboardCloser(const ClipboardCloser&) = delete;
    ClipboardCloser& operator=(const ClipboardCloser&) = delete;
    ClipboardCloser() = default;
};

// GMEM_MOVEABLE block that is freed unless ownership is explicitly released to the clipboard.
// SetClipboardData transfers ownership *only on success*; on failure the caller still owns the
// block and leaking it is a real, reported Windows bug pattern.
struct GlobalBlock {
    HGLOBAL h = nullptr;
    ~GlobalBlock() {
        if (h != nullptr) GlobalFree(h);
    }
    HGLOBAL release() noexcept {
        HGLOBAL out = h;
        h = nullptr;
        return out;
    }
    GlobalBlock() = default;
    GlobalBlock(const GlobalBlock&) = delete;
    GlobalBlock& operator=(const GlobalBlock&) = delete;
};

uint64_t qpc_frequency() noexcept {
    static const uint64_t f = [] {
        LARGE_INTEGER li{};
        QueryPerformanceFrequency(&li);
        return static_cast<uint64_t>(li.QuadPart);
    }();
    return f;
}

uint64_t qpc_now() noexcept {
    LARGE_INTEGER li{};
    QueryPerformanceCounter(&li);
    return static_cast<uint64_t>(li.QuadPart);
}

uint64_t qpc_elapsed_us(uint64_t t0, uint64_t t1) noexcept {
    const uint64_t f = qpc_frequency();
    if (f == 0) return 0;
    return (t1 - t0) * 1000000ull / f;
}

// SendInput pacing.
//
// A plain Sleep() rounds up to the system timer resolution, historically ~15.6 ms, which would
// turn a 1000 µs inter-batch delay into a visible stutter. CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
// (Windows 10 1803+) gets ~0.5 ms; below that granularity nothing user-mode can do better without
// spinning, and spinning on the UI thread is worse than being 500 µs late.
//
// We deliberately do NOT pump messages while waiting. Re-entering the message loop between two
// halves of a keystroke sequence is how a second injection interleaves with the first, and the
// commit-prefix invariant does not survive interleaved injections.
class PrecisionSleeper {
public:
    PrecisionSleeper() {
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (timer_ == nullptr) {
            // Pre-1803, or the flag was rejected. Fall back to a normal timer; the delay is then
            // quantised to the system timer tick, which is a pacing accuracy problem and not a
            // correctness one.
            timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_MODIFY_STATE | SYNCHRONIZE);
            high_resolution_ = false;
        }
    }
    ~PrecisionSleeper() {
        if (timer_ != nullptr) CloseHandle(timer_);
    }
    PrecisionSleeper(const PrecisionSleeper&) = delete;
    PrecisionSleeper& operator=(const PrecisionSleeper&) = delete;

    [[nodiscard]] bool high_resolution() const noexcept { return high_resolution_; }

    void wait_us(uint32_t microseconds) const {
        if (microseconds == 0) return;
        if (timer_ == nullptr) {
            Sleep((microseconds + 999u) / 1000u);
            return;
        }
        LARGE_INTEGER due{};
        // Negative = relative, in 100 ns units.
        due.QuadPart = -static_cast<LONGLONG>(microseconds) * 10;
        if (!SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
            Sleep((microseconds + 999u) / 1000u);
            return;
        }
        WaitForSingleObject(timer_, INFINITE);
    }

private:
    HANDLE timer_ = nullptr;
    bool high_resolution_ = true;
};

void fill_input(INPUT& in, const injector_detail::SynthKey& k) noexcept {
    in = INPUT{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = k.vk;
    in.ki.wScan = k.scan;
    in.ki.dwFlags = k.flags;
    in.ki.time = 0;
    in.ki.dwExtraInfo = static_cast<ULONG_PTR>(k.extra);
}

uint16_t scan_for_vk(uint16_t vk) noexcept {
    return static_cast<uint16_t>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
}

// ─── Password-field detection, and an honest statement of its reach ─────────────────────────
//
// ES_PASSWORD lives in the window style of a classic EDIT or RichEdit control, so this works for
// Win32 dialogs, MFC apps, WinForms, and the Windows credential prompts that are ordinary edit
// controls.
//
// It CANNOT see:
//   * a browser <input type="password"> — Chromium draws one HWND for the whole page;
//   * Electron, UWP/WinUI, WPF and Qt password boxes — all custom-drawn, no ES_PASSWORD anywhere;
//   * a WSLg or X11 password prompt — one HWND for the whole X client;
//   * `sudo`, `ssh`, `Read-Host -AsSecureString` and every other terminal password prompt — a
//     console has no per-field HWND at all, and this is the case most likely to bite, because
//     dictating into a terminal is a headline use case.
// Recovering those would require UI Automation, which plan.md §1.5/§1.7 rules out.
//
// So this guard is partial by construction and is documented as partial in
// docs/supported-targets.md rather than being presented as a safety net.
bool class_is_edit_like(const wchar_t* cls) noexcept {
    return iequals_w(cls, L"Edit") || istarts_with_w(cls, L"RichEdit") ||
           istarts_with_w(cls, L"RICHEDIT");
}

bool detect_password_field(HWND focus) noexcept {
    if (focus == nullptr || IsWindow(focus) == FALSE) return false;

    wchar_t cls[64] = {};
    if (GetClassNameW(focus, cls, static_cast<int>(std::size(cls))) == 0) return false;
    if (!class_is_edit_like(cls)) return false;

    const LONG_PTR style = GetWindowLongPtrW(focus, GWL_STYLE);
    if ((style & ES_PASSWORD) != 0) return true;

    // plan.md §1.7's "IsWindowVisible heuristic": a focused but invisible edit control is the
    // classic shape of a hidden input backing a custom-drawn password box. Narrow on purpose —
    // a *visible* edit control without ES_PASSWORD is an ordinary field and must not be refused.
    if (IsWindowVisible(focus) == FALSE) return true;

    return false;
}

bool detect_console(HWND top_level, std::string_view process_name) noexcept {
    wchar_t cls[64] = {};
    if (top_level != nullptr && GetClassNameW(top_level, cls, static_cast<int>(std::size(cls))) != 0) {
        if (iequals_w(cls, L"ConsoleWindowClass") ||                  // legacy conhost
            iequals_w(cls, L"PseudoConsoleWindow") ||                 // ConPTY host
            iequals_w(cls, L"CASCADIA_HOSTING_WINDOW_CLASS") ||       // Windows Terminal
            iequals_w(cls, L"mintty")) {                              // Git Bash / MSYS2
            return true;
        }
    }
    // Terminals that use an ordinary class name. Kept as a list rather than a heuristic because a
    // false positive here sends Ctrl+Shift+V into a text editor, where it is usually "paste
    // without formatting" and occasionally something else entirely.
    static constexpr const char* kConsoleExes[] = {
        "windowsterminal.exe", "wt.exe",       "conhost.exe",     "cmd.exe",
        "powershell.exe",      "pwsh.exe",     "mintty.exe",      "alacritty.exe",
        "wezterm-gui.exe",     "putty.exe",    "conemu64.exe",    "conemu.exe",
    };
    for (const char* e : kConsoleExes) {
        if (process_name == e) return true;
    }
    return false;
}

}  // namespace

// ════════════════════════════════════════════════════════════════════════════════════════════
//  Enum spellings
// ════════════════════════════════════════════════════════════════════════════════════════════
//
// Lowercase snake_case, matching the `"method": "sendinput"` form that
// docs/supported-targets.md §"Measurement record" already uses. settings.cpp round-trips
// InjectMethod through these two functions, so the pair must stay inverse.

const char* to_string(InjectMethod m) noexcept {
    switch (m) {
        case InjectMethod::SendInput: return "sendinput";
        case InjectMethod::Clipboard: return "clipboard";
    }
    return "sendinput";
}

bool inject_method_from_string(std::string_view s, InjectMethod& out) noexcept {
    // Accepts the canonical spelling plus the forms that appear in plan.md prose and on a
    // command line ("SendInput", "send_input", "send-input"), because a settings file a human
    // edited by hand should not fail to load over a hyphen.
    std::string v = ascii_lower(s);
    for (char& c : v) {
        if (c == '-' || c == '_' || c == ' ') c = '\0';
    }
    v.erase(std::remove(v.begin(), v.end(), '\0'), v.end());
    if (v == "sendinput") {
        out = InjectMethod::SendInput;
        return true;
    }
    if (v == "clipboard" || v == "paste" || v == "clipboardpaste") {
        out = InjectMethod::Clipboard;
        return true;
    }
    return false;
}

const char* to_string(InjectStatus s) noexcept {
    switch (s) {
        case InjectStatus::Ok: return "ok";
        case InjectStatus::TargetChanged: return "target_changed";
        case InjectStatus::ElevatedUnsupported: return "elevated_unsupported";
        case InjectStatus::SecureDesktop: return "secure_desktop";
        case InjectStatus::PasswordField: return "password_field";
        case InjectStatus::ClipboardUnavailable: return "clipboard_unavailable";
        case InjectStatus::PasteChordRejected: return "paste_chord_rejected";
        case InjectStatus::PartialWrite: return "partial_write";
        case InjectStatus::Failed: return "failed";
    }
    return "failed";
}

const char* to_string(IntegrityLevel l) noexcept {
    switch (l) {
        case IntegrityLevel::Untrusted: return "untrusted";
        case IntegrityLevel::Low: return "low";
        case IntegrityLevel::Medium: return "medium";
        case IntegrityLevel::MediumPlus: return "medium_plus";
        case IntegrityLevel::High: return "high";
        case IntegrityLevel::System: return "system";
        case IntegrityLevel::Unknown: return "unknown";
    }
    return "unknown";
}

// ════════════════════════════════════════════════════════════════════════════════════════════
//  injector_detail implementation
// ════════════════════════════════════════════════════════════════════════════════════════════

namespace injector_detail {

// ─── Integrity levels ───────────────────────────────────────────────────────────────────────

IntegrityLevel integrity_from_rid(uint32_t rid) noexcept {
    // The RIDs are band *starts*, not an enumeration: AppContainer and protected-process labels
    // sit between them (e.g. 0x2100 medium-plus, 0x3100 for some AV products). Rounding down to
    // the band below is the conservative reading — it can only make us think a target is *lower*
    // than it is... which would be unsafe. So round down but treat anything at or above the
    // system band as System, and anything above High-but-below-System as High.
    if (rid >= SECURITY_MANDATORY_SYSTEM_RID) return IntegrityLevel::System;
    if (rid >= SECURITY_MANDATORY_HIGH_RID) return IntegrityLevel::High;
    if (rid >= SECURITY_MANDATORY_MEDIUM_PLUS_RID) return IntegrityLevel::MediumPlus;
    if (rid >= SECURITY_MANDATORY_MEDIUM_RID) return IntegrityLevel::Medium;
    if (rid >= SECURITY_MANDATORY_LOW_RID) return IntegrityLevel::Low;
    return IntegrityLevel::Untrusted;
}

int integrity_rank(IntegrityLevel l) noexcept {
    switch (l) {
        case IntegrityLevel::Untrusted: return 0;
        case IntegrityLevel::Low: return 1;
        case IntegrityLevel::Medium: return 2;
        case IntegrityLevel::MediumPlus: return 3;
        case IntegrityLevel::High: return 4;
        case IntegrityLevel::System: return 5;
        case IntegrityLevel::Unknown: return -1;
    }
    return -1;
}

bool integrity_allows_injection(IntegrityLevel ours, IntegrityLevel target) noexcept {
    // Fail closed. Unknown means we could not read a token — which, for a window on our own
    // desktop owned by our own user, is itself evidence of a privilege boundary (a protected
    // process, a service, another user's process). Guessing "probably fine" here produces exactly
    // the silent no-op plan.md §1.7 exists to eliminate.
    const int a = integrity_rank(ours);
    const int b = integrity_rank(target);
    if (a < 0 || b < 0) return false;
    return b <= a;
}

namespace {

IntegrityLevel token_integrity(HANDLE token, uint32_t* os_error) noexcept {
    DWORD needed = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &needed);
    if (needed == 0) {
        if (os_error != nullptr) *os_error = GetLastError();
        return IntegrityLevel::Unknown;
    }
    // TOKEN_MANDATORY_LABEL contains a SID_AND_ATTRIBUTES with a pointer member, so the buffer
    // needs pointer alignment. A vector<uint64_t> guarantees 8, where vector<BYTE> only promises
    // 1 — the same alignment argument docs/protocol.md §4 makes about the audio payload.
    std::vector<uint64_t> buf((needed + 7u) / 8u);
    if (GetTokenInformation(token, TokenIntegrityLevel, buf.data(), needed, &needed) == FALSE) {
        if (os_error != nullptr) *os_error = GetLastError();
        return IntegrityLevel::Unknown;
    }
    auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf.data());
    PSID sid = label->Label.Sid;
    if (sid == nullptr || IsValidSid(sid) == FALSE) return IntegrityLevel::Unknown;
    const PUCHAR count = GetSidSubAuthorityCount(sid);
    if (count == nullptr || *count == 0) return IntegrityLevel::Unknown;
    const DWORD rid = *GetSidSubAuthority(sid, static_cast<DWORD>(*count - 1));
    return integrity_from_rid(static_cast<uint32_t>(rid));
}

}  // namespace

IntegrityLevel current_process_integrity() noexcept {
    // Cached: our own integrity level cannot change without the process restarting.
    static const IntegrityLevel level = [] {
        HANDLE raw = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw) == FALSE) {
            return IntegrityLevel::Unknown;
        }
        HandleGuard tok(raw);
        return token_integrity(tok.h, nullptr);
    }();
    return level;
}

IntegrityLevel process_integrity(uint32_t pid, uint32_t* os_error) noexcept {
    if (os_error != nullptr) *os_error = 0;
    if (pid == 0) return IntegrityLevel::Unknown;

    // PROCESS_QUERY_LIMITED_INFORMATION rather than PROCESS_QUERY_INFORMATION: the limited right
    // is granted across an integrity boundary for a same-user elevated process, which is exactly
    // the case we need to read. The full right is not, so asking for it would turn "elevated
    // target" into "unopenable target" and lose the distinction.
    HANDLE raw = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (raw == nullptr) {
        if (os_error != nullptr) *os_error = GetLastError();
        return IntegrityLevel::Unknown;
    }
    HandleGuard proc(raw);

    HANDLE rawtok = nullptr;
    if (OpenProcessToken(proc.h, TOKEN_QUERY, &rawtok) == FALSE) {
        if (os_error != nullptr) *os_error = GetLastError();
        return IntegrityLevel::Unknown;
    }
    HandleGuard tok(rawtok);
    return token_integrity(tok.h, os_error);
}

// ─── Event synthesis ────────────────────────────────────────────────────────────────────────

namespace {

void push_unicode_unit(std::vector<SynthKey>& ev, wchar_t unit, uintptr_t extra, bool down) {
    SynthKey k;
    k.vk = 0;  // MUST be 0 for KEYEVENTF_UNICODE; a non-zero wVk makes the flag a no-op.
    k.scan = static_cast<uint16_t>(unit);
    k.flags = KEYEVENTF_UNICODE | (down ? 0u : static_cast<uint32_t>(KEYEVENTF_KEYUP));
    k.extra = extra;
    ev.push_back(k);
}

void push_vk(std::vector<SynthKey>& ev, uint16_t vk, uintptr_t extra, bool down) {
    SynthKey k;
    k.vk = vk;
    k.scan = scan_for_vk(vk);
    k.flags = down ? 0u : static_cast<uint32_t>(KEYEVENTF_KEYUP);
    k.extra = extra;
    ev.push_back(k);
}

}  // namespace

bool build_text_events(std::string_view utf8, uintptr_t extra, std::vector<SynthKey>& events,
                       std::vector<SynthAtom>& atoms, size_t* bad_offset, const char** reason) {
    events.clear();
    atoms.clear();
    if (bad_offset != nullptr) *bad_offset = utf8::kNpos;
    if (reason != nullptr) *reason = nullptr;

    size_t bad = 0;
    if (!utf8::validate(utf8, &bad)) {
        if (bad_offset != nullptr) *bad_offset = bad;
        if (reason != nullptr) *reason = "malformed-utf8";
        return false;
    }

    // ─── Control characters are rejected, not filtered ──────────────────────────────────────
    //
    // injector.h: "Append-only by construction: there is no delete, no backspace, no selection
    // replacement in this API, and none may be added." A U+0008 delivered as KEYEVENTF_UNICODE
    // arrives as WM_CHAR 0x08, which every classic edit control treats as a backspace — i.e. it
    // *is* the delete path, smuggled in as text. U+007F is the same argument, and U+001B (Esc)
    // dismisses dialogs. Silently dropping them would be worse than refusing: the caller would
    // believe the whole string landed.
    //
    // Tab, CR and LF are the three exceptions, and only because they have unambiguous,
    // non-destructive meanings that a dictation stream can legitimately contain.
    for (size_t i = 0; i < utf8.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        if (c >= 0x20 && c != 0x7F) continue;
        if (c == '\t' || c == '\r' || c == '\n') continue;
        if (bad_offset != nullptr) *bad_offset = i;
        if (reason != nullptr) *reason = "forbidden-control";
        return false;
    }

    std::wstring w;
    if (!utf8::to_utf16(utf8, w, &bad)) {
        // Unreachable given validate() above, but a Result-free helper that trusts a previous
        // check is how the two drift apart later.
        if (bad_offset != nullptr) *bad_offset = bad;
        if (reason != nullptr) *reason = "malformed-utf8";
        return false;
    }

    // Walk the UTF-16 and the UTF-8 in lockstep so each atom can record how many UTF-8 bytes it
    // consumed; bytes_injected on a partial write is a UTF-8 prefix length and must be exact.
    size_t byte_pos = 0;
    for (size_t i = 0; i < w.size();) {
        const wchar_t unit = w[i];
        SynthAtom atom;
        atom.first_event = static_cast<uint32_t>(events.size());

        if (unit == L'\r' || unit == L'\n') {
            // CR, LF and CRLF all collapse to exactly one Enter. Not KEYEVENTF_UNICODE U+000D:
            // that produces a WM_CHAR only, which the many targets that watch for VK_RETURN in
            // WM_KEYDOWN never see. A real VK_RETURN gets both, because TranslateMessage
            // synthesises the WM_CHAR from it.
            //
            // Honest limitation: in a chat client Enter sends the message. The commit engine is
            // not supposed to emit newlines mid-utterance, and if that ever changes this is the
            // line that needs a per-app policy rather than a different key.
            size_t consumed = 1;
            if (unit == L'\r' && i + 1 < w.size() && w[i + 1] == L'\n') consumed = 2;
            push_vk(events, VK_RETURN, extra, true);
            push_vk(events, VK_RETURN, extra, false);
            atom.utf8_bytes = static_cast<uint32_t>(consumed);  // CR/LF are one byte each in UTF-8
            byte_pos += consumed;
            i += consumed;
        } else if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < w.size() && w[i + 1] >= 0xDC00 &&
                   w[i + 1] <= 0xDFFF) {
            // Astral plane. The two code units are two separate KEYEVENTF_UNICODE events — one per
            // UTF-16 code unit — because SendInput has no notion of a code point.
            //
            // Order is nested (down high, down low, up low, up high) rather than sequential so the
            // two WM_CHAR messages are adjacent in the target's queue with nothing between them. A
            // receiver that buffers a lone high surrogate and pairs it with the *next* WM_CHAR is
            // the normal implementation; whether an intervening WM_KEYUP disturbs that is not
            // something we could verify for every target, so we do not rely on it.
            push_unicode_unit(events, unit, extra, true);
            push_unicode_unit(events, w[i + 1], extra, true);
            push_unicode_unit(events, w[i + 1], extra, false);
            push_unicode_unit(events, unit, extra, false);
            atom.utf8_bytes = 4;  // a surrogate pair is always 4 bytes in UTF-8
            byte_pos += 4;
            i += 2;
        } else {
            // BMP scalar (an unpaired surrogate cannot reach here: to_utf16 rejects them and
            // validate() already refused their UTF-8 encodings).
            push_unicode_unit(events, unit, extra, true);
            push_unicode_unit(events, unit, extra, false);
            size_t len = 1;
            char32_t cp = 0;
            if (utf8::decode(utf8, byte_pos, cp, len)) {
                atom.utf8_bytes = static_cast<uint32_t>(len);
            } else {
                atom.utf8_bytes = 1;
            }
            byte_pos += atom.utf8_bytes;
            i += 1;
        }

        atom.event_count = static_cast<uint32_t>(events.size()) - atom.first_event;
        atoms.push_back(atom);
    }

    return true;
}

void build_paste_chord(bool ctrl_shift_v, uint32_t stuck_mods, uintptr_t extra,
                       std::vector<SynthKey>& events) {
    events.clear();

    // ─── Releasing stuck modifiers, and the one we refuse to touch ──────────────────────────
    //
    // The hotkey chord's own modifiers are frequently still physically down when the first commit
    // lands (plan.md §1.8 is about exactly this overlap). A held Shift turns our Ctrl+V into
    // Ctrl+Shift+V; a held Alt turns it into something no target expects. So we send a key-up for
    // them first, marked as ours so hotkey.h's filter ignores it.
    //
    // **Win is deliberately absent from this list.** Injecting a bare Win key-up is precisely the
    // event that pops the Start menu — the failure plan.md §1.8's suppression hack exists to
    // avoid. If Win is held when a paste is due, the chord goes out as Win+Ctrl+V and the paste
    // is lost; that is a visible, recoverable failure, whereas a Start menu stealing focus
    // mid-session is a wrong-window hazard.
    //
    // We do not restore these key-downs afterwards: re-pressing a modifier the user has since
    // released would leave it stuck, which is far worse than an extra key-up for a key that is
    // already up (which Windows discards).
    if ((stuck_mods & kStuckShift) != 0 && !ctrl_shift_v) {
        push_vk(events, VK_SHIFT, extra, false);
    }
    if ((stuck_mods & kStuckAlt) != 0) {
        push_vk(events, VK_MENU, extra, false);
    }

    push_vk(events, VK_CONTROL, extra, true);
    if (ctrl_shift_v) push_vk(events, VK_SHIFT, extra, true);
    push_vk(events, static_cast<uint16_t>('V'), extra, true);
    push_vk(events, static_cast<uint16_t>('V'), extra, false);
    if (ctrl_shift_v) push_vk(events, VK_SHIFT, extra, false);
    push_vk(events, VK_CONTROL, extra, false);
}

// ─── Clipboard ──────────────────────────────────────────────────────────────────────────────

// Defined with the delayed-rendering machinery at the end of this section, because that is what
// it is for: WM_RENDERFORMAT, WM_DESTROYCLIPBOARD and the deferred restore's timer all arrive at
// the window below.
LRESULT CALLBACK owner_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

// The window that owns the clipboard while we hold it.
//
// OpenClipboard(NULL) looks simpler and is a trap: MSDN's EmptyClipboard documentation states
// that opening with a NULL window sets the clipboard owner to NULL, "which causes
// SetClipboardData to fail". So a real HWND is required. A message-only window is the smallest
// one that works — WM_RENDERFORMAT and WM_DESTROYCLIPBOARD are sent directly to the owner, not
// broadcast, so HWND_MESSAGE costs us nothing.
//
// It carries a real window procedure rather than DefWindowProcW because the delayed-rendering
// signal arrives there, and because the deferred restore's timer hangs on this window — which
// means the restore happens only while the UI thread is pumping messages. That is the same
// thread this module is confined to, and finish_pending_clipboard_restore() covers the exit path
// where the pump stops.
//
// Deliberately never destroyed: DestroyWindow must be called on the creating thread, and static
// destruction gives no such guarantee. The kernel reclaims it at process exit. Creating it lazily
// on first use means it is created on the UI thread, which is the only thread that may call into
// this module.
uintptr_t clipboard_owner_window() {
    static const uintptr_t hwnd = [] {
        static const wchar_t kClass[] = L"kd_injector_clipboard_owner";
        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = owner_wndproc;
        wc.hInstance = inst;
        wc.lpszClassName = kClass;
        // ERROR_CLASS_ALREADY_EXISTS is fine and expected if this ever runs twice.
        RegisterClassExW(&wc);
        HWND h = CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst,
                                 nullptr);
        if (h == nullptr) {
            KD_LOG_ERROR("clipboard owner window creation failed (err=%lu)", GetLastError());
        }
        return reinterpret_cast<uintptr_t>(h);
    }();
    return hwnd;
}

namespace {

// Opening the clipboard fails while another process holds it, and something always does —
// clipboard managers, Office, remote-desktop clipboard bridges. Retry with backoff and give up
// cleanly; blocking the UI thread indefinitely on someone else's clipboard is not acceptable.
// Total worst case here is ~63 ms, which is the same order as paste_settle_ms.
bool open_clipboard_with_retry(uint32_t* os_error) {
    HWND owner = reinterpret_cast<HWND>(clipboard_owner_window());
    if (owner == nullptr) {
        if (os_error != nullptr) *os_error = ERROR_INVALID_WINDOW_HANDLE;
        return false;
    }
    static constexpr DWORD kBackoffMs[] = {0, 1, 2, 4, 8, 16, 32};
    DWORD last = 0;
    for (DWORD delay : kBackoffMs) {
        if (delay != 0) Sleep(delay);
        if (OpenClipboard(owner) != FALSE) return true;
        last = GetLastError();
    }
    if (os_error != nullptr) *os_error = last;
    return false;
}

// Formats we refuse to snapshot, with the reason. All of these are handles that are NOT HGLOBAL
// blocks, so GlobalSize/memcpy would either fail or duplicate a handle we do not own and cannot
// legally re-publish.
bool format_is_copyable(UINT fmt) noexcept {
    switch (fmt) {
        case CF_BITMAP:
        case CF_PALETTE:
        case CF_METAFILEPICT:   // HGLOBAL, but it *contains* an HMETAFILE we would not own
        case CF_ENHMETAFILE:
        case CF_OWNERDISPLAY:
        case CF_DSPBITMAP:
        case CF_DSPENHMETAFILE:
        case CF_DSPMETAFILEPICT:
            return false;
        default:
            break;
    }
    // GDI object and private ranges are freed by the owner, not by the clipboard.
    if (fmt >= CF_GDIOBJFIRST && fmt <= CF_GDIOBJLAST) return false;
    if (fmt >= CF_PRIVATEFIRST && fmt <= CF_PRIVATELAST) return false;
    return true;
}

// A snapshot big enough to hold a pasted document but not a 400 MB screenshot. Formats past the
// budget are counted as lost rather than silently truncated.
constexpr size_t kMaxBackupBytes = 4u * 1024u * 1024u;

}  // namespace

struct SavedFormat {
    UINT format = 0;
    std::vector<uint8_t> bytes;
};

struct ClipboardBackup {
    std::vector<SavedFormat> formats;
    size_t lost = 0;   // formats present but not preserved; surfaced in diagnostics
    bool saved = false;
};

ClipboardBackup* clipboard_backup_create() { return new ClipboardBackup(); }

void clipboard_backup_destroy(ClipboardBackup* b) noexcept { delete b; }

size_t clipboard_backup_format_count(const ClipboardBackup* b) noexcept {
    return b == nullptr ? 0 : b->formats.size();
}

size_t clipboard_backup_lost_format_count(const ClipboardBackup* b) noexcept {
    return b == nullptr ? 0 : b->lost;
}

// ─── What is preserved and what is lost, stated rather than discovered ──────────────────────
//
// PRESERVED: every HGLOBAL-backed format, in the priority order the clipboard reports them, up to
// a 4 MiB total budget. That covers CF_UNICODETEXT, CF_TEXT, CF_HTML, CF_RTF, CF_DIB/CF_DIBV5,
// CF_HDROP and essentially every registered format any application publishes.
//
// LOST: CF_BITMAP, CF_PALETTE, CF_METAFILEPICT, CF_ENHMETAFILE and the GDI/private/owner-display
// ranges (not HGLOBAL — see format_is_copyable); anything beyond the byte budget; and any format
// whose owner declined to render it. In practice an application that publishes CF_BITMAP almost
// always publishes CF_DIB alongside it, so an image copy usually survives in a different format
// rather than vanishing. The count of lost formats is recorded and logged; it is not hidden.
//
// KNOWN HAZARD, unavoidable with this API: GetClipboardData sends WM_RENDERFORMAT to the
// clipboard owner and waits. A hung owner blocks us with no timeout Windows offers. This is one
// more reason InjectMethod::SendInput is the default.
bool clipboard_save(ClipboardBackup* b, ClipboardGuardState& st, uint32_t* os_error) {
    if (b == nullptr) return false;
    b->formats.clear();
    b->lost = 0;
    b->saved = false;
    st.had_previous = false;

    if (!open_clipboard_with_retry(os_error)) return false;
    ClipboardCloser closer;
    closer.open = true;

    size_t budget = kMaxBackupBytes;
    for (UINT fmt = EnumClipboardFormats(0); fmt != 0; fmt = EnumClipboardFormats(fmt)) {
        if (!format_is_copyable(fmt)) {
            ++b->lost;
            continue;
        }
        HANDLE data = GetClipboardData(fmt);
        if (data == nullptr) {  // delayed render declined, or not an HGLOBAL after all
            ++b->lost;
            continue;
        }
        const SIZE_T size = GlobalSize(data);
        if (size == 0 || size > budget) {
            ++b->lost;
            continue;
        }
        void* src = GlobalLock(data);
        if (src == nullptr) {
            ++b->lost;
            continue;
        }
        SavedFormat sf;
        sf.format = fmt;
        sf.bytes.resize(static_cast<size_t>(size));
        std::memcpy(sf.bytes.data(), src, static_cast<size_t>(size));
        GlobalUnlock(data);
        budget -= static_cast<size_t>(size);
        b->formats.push_back(std::move(sf));
    }

    b->saved = true;
    st.had_previous = !b->formats.empty();
    if (b->lost != 0) {
        KD_LOG_WARN("clipboard snapshot: %zu format(s) preserved, %zu not preservable",
                    b->formats.size(), b->lost);
    }
    return true;
}

bool clipboard_set_utf8(std::string_view utf8, ClipboardGuardState& st, uint32_t* os_error) {
    st.restored = false;
    st.skipped_because_changed = false;
    st.sequence_at_set = 0;

    std::wstring w;
    if (!utf8::to_utf16(utf8, w, nullptr)) {
        if (os_error != nullptr) *os_error = ERROR_NO_UNICODE_TRANSLATION;
        return false;
    }

    GlobalBlock block;
    block.h = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
    if (block.h == nullptr) {
        if (os_error != nullptr) *os_error = GetLastError();
        return false;
    }
    {
        void* dst = GlobalLock(block.h);
        if (dst == nullptr) {
            if (os_error != nullptr) *os_error = GetLastError();
            return false;
        }
        std::memcpy(dst, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(block.h);
    }

    {
        if (!open_clipboard_with_retry(os_error)) return false;
        ClipboardCloser closer;
        closer.open = true;
        if (EmptyClipboard() == FALSE) {
            if (os_error != nullptr) *os_error = GetLastError();
            return false;
        }
        if (SetClipboardData(CF_UNICODETEXT, block.h) == nullptr) {
            if (os_error != nullptr) *os_error = GetLastError();
            return false;  // ~GlobalBlock frees it: ownership only transfers on success
        }
        block.release();
        // Windows synthesises CF_TEXT / CF_OEMTEXT / CF_LOCALE from CF_UNICODETEXT, so targets
        // that ask for ANSI text still get it. We publish the Unicode form only, on purpose: an
        // explicit CF_TEXT would be a lossy copy of the same string and could disagree with it.
    }  // CloseClipboard happens here — the sequence number must be read *after* it

    st.sequence_at_set = static_cast<uint32_t>(GetClipboardSequenceNumber());
    return true;
}

// ─── The ownership check plan.md §7 gates on ────────────────────────────────────────────────
//
// Restore only if the clipboard sequence number is still the one we recorded when we set our
// text. If it moved, someone — the user pressing Ctrl+C, a clipboard manager, the target app
// itself — has put something there since, and overwriting it with a snapshot taken before our own
// injection would destroy work the user did in between. Leaving our dictated text on the
// clipboard is the lesser harm and is what plan.md §1.7 chooses.
// Does the clipboard still hold exactly this text? **Called with the clipboard already open**,
// because its one caller is in the middle of deciding whether to empty it and reopening between
// the question and the answer would put a window back into the race the question is about.
//
// A pointer comparison would be wrong: a clipboard manager that re-publishes our string hands
// back a different handle holding the same characters, and that is the case this exists to call
// "still ours".
bool clipboard_text_equals(std::wstring_view ours) {
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h == nullptr) return false;
    const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h));
    if (p == nullptr) return false;
    const size_t units = GlobalSize(h) / sizeof(wchar_t);
    size_t len = 0;
    while (len < units && p[len] != L'\0') ++len;
    const bool same = std::wstring_view(p, len) == ours;
    GlobalUnlock(h);
    return same;
}

void clipboard_restore(ClipboardBackup* b, ClipboardGuardState& st, std::wstring_view ours) {
    st.restored = false;
    st.skipped_because_changed = false;
    if (b == nullptr || !b->saved) return;

    const uint32_t now = static_cast<uint32_t>(GetClipboardSequenceNumber());
    const bool moved = now != st.sequence_at_set;

    if (!open_clipboard_with_retry(nullptr)) return;
    ClipboardCloser closer;
    closer.open = true;

    // ─── The sequence number is necessary and not sufficient ────────────────────────────────
    //
    // plan.md §1.7 asks us to restore "only if we still own the state we set", and the sequence
    // number was the whole test. It is the right question asked too coarsely: it counts *every*
    // modification, including ones that leave our own text sitting there unchanged.
    //
    // Two things do that routinely, and between them they cover most machines. A clipboard
    // manager — Ditto, Windows' own Win+V history, a cloud-clipboard sync — wakes on our write,
    // reads it, and re-publishes normalised formats. And Windows synthesises CF_TEXT,
    // CF_OEMTEXT and CF_LOCALE from CF_UNICODETEXT on demand, which is itself a modification.
    // Measured here on 2026-09-09 with Ditto running: the counter moved by exactly six between
    // the set and the restore, on *every* clipboard injection, so the guard fired every time and
    // the user's own clipboard was never once put back. The setting silently became "kvit-dictate
    // takes your clipboard", which is precisely the outcome the guard exists to prevent.
    //
    // So the sequence number now decides only whether to look. What decides the answer is
    // whether CF_UNICODETEXT still holds the string we put there: if it does, nothing the user
    // would miss has arrived, whoever bumped the counter, and restoring destroys only our own
    // text. If it does not, somebody really did copy something and it stays.
    if (moved && !clipboard_text_equals(ours)) {
        st.skipped_because_changed = true;
        KD_LOG_INFO("clipboard changed under us (seq %u -> %u) and no longer holds our text; "
                    "leaving it alone",
                    st.sequence_at_set, now);
        return;
    }
    if (moved) {
        KD_LOG_DEBUG("clipboard sequence moved (%u -> %u) but the text is still ours — a "
                     "clipboard manager or a synthesised format; restoring",
                     st.sequence_at_set, now);
    }

    // Safe to empty: the checks above prove the clipboard still holds exactly what we put there,
    // so the only thing destroyed here is our own text.
    if (EmptyClipboard() == FALSE) return;

    for (const SavedFormat& sf : b->formats) {
        GlobalBlock block;
        block.h = GlobalAlloc(GMEM_MOVEABLE, sf.bytes.size());
        if (block.h == nullptr) continue;
        void* dst = GlobalLock(block.h);
        if (dst == nullptr) continue;
        std::memcpy(dst, sf.bytes.data(), sf.bytes.size());
        GlobalUnlock(block.h);
        if (SetClipboardData(sf.format, block.h) != nullptr) {
            block.release();
        }
        // On failure ~GlobalBlock frees the copy and that format is simply gone. Continuing is
        // right: losing one of six formats beats abandoning the restore with the clipboard empty.
    }

    // An empty backup means the clipboard was empty before we touched it, and an empty clipboard
    // is what "restored" means in that case.
    st.restored = true;
}

// ════════════════════════════════════════════════════════════════════════════════════════════
//  Delayed rendering — the only honest "the target took it" signal
// ════════════════════════════════════════════════════════════════════════════════════════════
//
// The clipboard path used to publish its text, send Ctrl+V, sleep a fixed `paste_settle_ms` and
// put the user's clipboard back. That sleep was a guess at how long *someone else's* application
// takes to process a keystroke, and on this machine the guess was wrong daily: VS Code is
// Electron, its Ctrl+V crosses into the renderer process and back before anything reads the
// clipboard, and that round trip is regularly slower than 60 ms under load. The restore won the
// race and Code pasted the **previous** clipboard contents — reported 2026-09-17, and the logs
// date its first appearance to the day the restore started actually happening (ea8d22f). Before
// that the ownership guard skipped every restore, so the race was there and invisible.
//
// The clipboard API has no "the paste completed" notification. It has one thing close enough:
// publish CF_UNICODETEXT with a **null handle**, and Windows asks us for the bytes — with
// WM_RENDERFORMAT — at the moment a reader wants them. A reader asking is a reader reading. The
// restore is scheduled from that message instead of from a stopwatch, and `paste_settle_ms`
// stops being a bet on the target and becomes what its name always claimed: a short grace after
// the target has the text.
//
// Three things this does not promise, each a smaller failure than the one it replaces:
//
//   * **The first renderer wins.** Once we answer WM_RENDERFORMAT the data is real and every
//     later reader is served by Windows without telling us. A clipboard manager that pounces on
//     our write — Ditto and Win+V both do; the sequence number moves by exactly six here on
//     every injection — can therefore take our one signal. We do not mistake it for the target
//     (the requester's process id is compared against the target's), but after it the target's
//     own read is invisible and kRestoreFallbackMs is what restores. That is a *late* restore,
//     which costs nothing, rather than an early one, which costs the paste.
//   * **A target may never read at all**, having Ctrl+V bound elsewhere or ignoring it. The
//     fallback bounds how long the user's clipboard stays displaced in that case.
//   * **WM_RENDERFORMAT is a SendMessage into our UI thread**, so a target pasting while that
//     thread is busy waits for us. The handler does no I/O and makes no clipboard call beyond
//     the one SetClipboardData it exists for.

struct PendingPaste {
    ClipboardBackup* backup = nullptr;  // the user's clipboard, owned until the restore runs
    std::wstring text;                  // what we published: rendered on demand, compared later
    ClipboardGuardState* report_to = nullptr;  // the caller's diagnostics, updated in place
    uint32_t sequence_at_set = 0;
    uint32_t target_pid = 0;
    uint32_t settle_ms = 60;
    uint32_t attempts = 0;  // restores tried; a busy clipboard is worth coming back for
    bool active = false;
    bool timer_running = false;
    bool we_are_emptying = false;  // tells the handlers our own restore from somebody else's copy
};

// How long to wait for a target that never asks for the text — because it ignores Ctrl+V, or
// because a clipboard manager answered the render request in its place and the target's own read
// is therefore invisible to us. Long enough to cover a loaded Electron paste by a wide margin,
// short enough that a user who copies something else does not have to wonder where it went.
//
// Passed to paste_watch_arm() rather than read from inside it, so that the tests can exercise
// the fallback without the suite spending a second and a half waiting for real time to pass.
constexpr uint32_t kRestoreFallbackMs = 1200;

namespace {

PendingPaste& pending_paste() {
    static PendingPaste p;
    return p;
}

constexpr UINT_PTR kRestoreTimerId = 1;

// A restore that cannot open the clipboard used to be the end of it: the user's contents stayed
// lost and the log said so. Another process holding the clipboard for a few dozen milliseconds
// is ordinary — clipboard managers, Office, RDP bridges — and the deferred design has somewhere
// to put a second attempt, which the blocking one did not.
constexpr uint32_t kMaxRestoreAttempts = 5;
constexpr uint32_t kRestoreRetryMs = 60;

void arm_restore_timer(uint32_t ms) {
    HWND h = reinterpret_cast<HWND>(clipboard_owner_window());
    if (h == nullptr) return;
    // USER_TIMER_MINIMUM is 10 ms and SetTimer rounds up to it silently; saying so here stops a
    // paste_settle_ms of 0 from reading as "restore synchronously".
    if (ms < USER_TIMER_MINIMUM) ms = USER_TIMER_MINIMUM;
    if (SetTimer(h, kRestoreTimerId, ms, nullptr) == 0) {
        // With no timer, nothing would ever put the clipboard back — the failure this whole
        // mechanism exists to prevent. Restoring now is the worse paste and the better
        // clipboard, and it is exactly what the fixed sleep used to do.
        KD_LOG_ERROR("clipboard restore timer could not be set (err=%lu); restoring now",
                     static_cast<unsigned long>(GetLastError()));
        pending_paste().timer_running = false;
        paste_watch_run_now("the restore timer could not be set", false);
        return;
    }
    pending_paste().timer_running = true;
}

void cancel_restore_timer() {
    PendingPaste& p = pending_paste();
    if (!p.timer_running) return;
    HWND h = reinterpret_cast<HWND>(clipboard_owner_window());
    if (h != nullptr) KillTimer(h, kRestoreTimerId);
    p.timer_running = false;
}

// The bytes for one text format, in the encoding that format means. CF_TEXT and CF_OEMTEXT are
// converted rather than refused: which of the three Windows asks us to render is its choice, not
// the reader's, and answering only CF_UNICODETEXT would leave an ANSI reader with nothing.
HGLOBAL render_block(UINT fmt, const std::wstring& w) {
    if (fmt == CF_UNICODETEXT) {
        const size_t bytes = (w.size() + 1) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (h == nullptr) return nullptr;
        void* dst = GlobalLock(h);
        if (dst == nullptr) {
            GlobalFree(h);
            return nullptr;
        }
        std::memcpy(dst, w.c_str(), bytes);
        GlobalUnlock(h);
        return h;
    }
    if (fmt != CF_TEXT && fmt != CF_OEMTEXT) return nullptr;

    const UINT cp = (fmt == CF_TEXT) ? CP_ACP : CP_OEMCP;
    const int need = WideCharToMultiByte(cp, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (need < 0) return nullptr;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, static_cast<size_t>(need) + 1);
    if (h == nullptr) return nullptr;
    char* dst = static_cast<char*>(GlobalLock(h));
    if (dst == nullptr) {
        GlobalFree(h);
        return nullptr;
    }
    if (need > 0) {
        WideCharToMultiByte(cp, 0, w.c_str(), static_cast<int>(w.size()), dst, need, nullptr,
                            nullptr);
    }
    dst[need] = '\0';
    GlobalUnlock(h);
    return h;
}

// Who is asking? A reader holds the clipboard open for the duration of its GetClipboardData, so
// GetOpenClipboardWindow names it. Returns 0 when the reader opened the clipboard with a null
// window, which is legal for reading — and which counts as "not the target", because guessing
// the other way restores early and that is the bug this exists to fix.
uint32_t clipboard_requester_pid() {
    HWND requester = GetOpenClipboardWindow();
    if (requester == nullptr) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(requester, &pid);
    return static_cast<uint32_t>(pid);
}

void on_render_format(UINT fmt) {
    PendingPaste& p = pending_paste();
    if (!p.active) return;

    HGLOBAL block = render_block(fmt, p.text);
    if (block == nullptr) return;
    // No OpenClipboard here, deliberately: MSDN is explicit that an owner responding to
    // WM_RENDERFORMAT must not open the clipboard — the requester already has it open.
    if (SetClipboardData(fmt, block) == nullptr) {
        GlobalFree(block);  // ownership transfers only on success
        return;
    }

    const uint32_t pid = clipboard_requester_pid();
    if (p.target_pid != 0 && pid == p.target_pid) {
        if (p.report_to != nullptr) p.report_to->target_read_it = true;
        KD_LOG_DEBUG("the target (pid %u) read the pasted text; restoring the clipboard in %u ms",
                     pid, p.settle_ms);
        arm_restore_timer(p.settle_ms);
        return;
    }
    KD_LOG_DEBUG("clipboard format %u rendered for pid %u, which is not the target (pid %u) — "
                 "waiting for the fallback rather than restoring on somebody else's read",
                 fmt, pid, p.target_pid);
}

// Sent to a clipboard owner whose window is going away while it still owes data. Ours is never
// destroyed before the process ends, so this is the disorderly exit: render, so that what the
// user pastes next is our text rather than nothing. finish_pending_clipboard_restore() is the
// orderly one, and it puts the user's own clipboard back instead.
void on_render_all_formats() {
    PendingPaste& p = pending_paste();
    if (!p.active || p.we_are_emptying) return;
    HWND owner = reinterpret_cast<HWND>(clipboard_owner_window());
    if (!open_clipboard_with_retry(nullptr)) return;
    ClipboardCloser closer;
    closer.open = true;
    if (GetClipboardOwner() != owner) return;
    HGLOBAL block = render_block(CF_UNICODETEXT, p.text);
    if (block == nullptr) return;
    if (SetClipboardData(CF_UNICODETEXT, block) == nullptr) GlobalFree(block);
}

// Somebody emptied the clipboard. Not necessarily the user copying something — a clipboard
// manager re-publishing our own string does exactly this — which is why nothing is cancelled
// here. The restore's content check decides whether what is there now is ours, and it runs
// whether this message arrived or not.
void on_destroy_clipboard() {
    PendingPaste& p = pending_paste();
    if (!p.active || p.we_are_emptying) return;
    KD_LOG_DEBUG("another application took ownership of the clipboard while our paste was "
                 "outstanding; the restore will compare contents before touching it");
}

}  // namespace

LRESULT CALLBACK owner_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_RENDERFORMAT:
            on_render_format(static_cast<UINT>(wp));
            return 0;
        case WM_RENDERALLFORMATS:
            on_render_all_formats();
            return 0;
        case WM_DESTROYCLIPBOARD:
            on_destroy_clipboard();
            return 0;
        case WM_TIMER:
            if (wp == kRestoreTimerId) {
                cancel_restore_timer();
                paste_watch_run_now("the target did not read our text in time", true);
                return 0;
            }
            break;
        default:
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Publish `utf8` as a delay-rendered CF_UNICODETEXT. The UTF-16 form is handed back rather than
// thrown away: the ownership check at restore time compares against it, and converting twice
// would be two chances to disagree with what is on the clipboard.
//
// Windows synthesises CF_TEXT / CF_OEMTEXT / CF_LOCALE from CF_UNICODETEXT for readers that ask
// for them, delay-rendered or not, so publishing the Unicode form alone still serves an ANSI
// target — and render_block() above answers whichever of the three we are asked for.
bool clipboard_set_delayed(std::string_view utf8, std::wstring& out_utf16,
                           ClipboardGuardState& st, uint32_t* os_error) {
    st.restored = false;
    st.skipped_because_changed = false;
    st.restore_pending = false;
    st.target_read_it = false;
    st.sequence_at_set = 0;

    if (!utf8::to_utf16(utf8, out_utf16, nullptr)) {
        if (os_error != nullptr) *os_error = ERROR_NO_UNICODE_TRANSLATION;
        return false;
    }

    {
        if (!open_clipboard_with_retry(os_error)) return false;
        ClipboardCloser closer;
        closer.open = true;
        if (EmptyClipboard() == FALSE) {
            if (os_error != nullptr) *os_error = GetLastError();
            return false;
        }
        // A null handle is the delayed-rendering request, and it makes the return value useless
        // as a success test: SetClipboardData returns NULL on this path whether it worked or
        // not. GetLastError is the only answer available, so the error state is cleared first.
        SetLastError(ERROR_SUCCESS);
        SetClipboardData(CF_UNICODETEXT, nullptr);
        const DWORD err = GetLastError();
        if (err != ERROR_SUCCESS) {
            if (os_error != nullptr) *os_error = err;
            return false;
        }
    }  // CloseClipboard happens here — the sequence number must be read *after* it

    st.sequence_at_set = static_cast<uint32_t>(GetClipboardSequenceNumber());
    return true;
}

bool paste_watch_active() { return pending_paste().active; }

// Take ownership of the snapshot and start the clock. From here the restore belongs to the
// message loop: WM_RENDERFORMAT from the target shortens the wait to `settle_ms`, and the
// fallback covers every target that never asks.
void paste_watch_arm(ClipboardBackup* backup, std::wstring text, ClipboardGuardState* report_to,
                     uint32_t sequence_at_set, uint32_t target_pid, uint32_t settle_ms,
                     uint32_t fallback_ms) {
    PendingPaste& p = pending_paste();
    // Arming over a live watch would leak the earlier snapshot and, worse, lose the only copy of
    // the user's clipboard. inject_via_clipboard adopts instead; reaching here with one
    // outstanding is a bug in a caller, so it is logged rather than smoothed over.
    if (p.active) {
        KD_LOG_WARN("a clipboard paste was armed while another was still outstanding; the older "
                    "snapshot is being dropped");
        clipboard_backup_destroy(p.backup);
        if (p.report_to != nullptr) p.report_to->restore_pending = false;
        cancel_restore_timer();
        p = PendingPaste{};
    }

    p.backup = backup;
    p.text = std::move(text);
    p.report_to = report_to;
    p.sequence_at_set = sequence_at_set;
    p.target_pid = target_pid;
    p.settle_ms = settle_ms;
    p.active = true;
    if (report_to != nullptr) report_to->restore_pending = true;
    arm_restore_timer(fallback_ms);
}

namespace {

// One attempt. Returns false only for the one failure worth coming back for: the clipboard could
// not be opened, so nothing was decided and nothing was touched. A restore that ran, and a
// restore deliberately skipped because the contents are no longer ours, are both "settled".
bool try_restore_once(PendingPaste& p, ClipboardGuardState& st) {
    // `active` deliberately stays true across this. clipboard_restore may call GetClipboardData
    // to compare contents, and on a clipboard nobody has rendered yet that comes straight back
    // to on_render_format — which must answer with our text, or the comparison would conclude
    // the clipboard is not ours and refuse to put the user's back.
    p.we_are_emptying = true;
    clipboard_restore(p.backup, st, p.text);
    p.we_are_emptying = false;
    return st.restored || st.skipped_because_changed;
}

}  // namespace

// Run the restore now: from the timer, from a caller on its way out, or from a failure that left
// nothing else to run it. Returns whether there was anything to do.
//
// `may_defer` says whether a busy clipboard can be retried from the message loop. It cannot when
// the caller is on its way out of the process — there will be no more timers — so that path
// spends a little real time on its retries instead, and a few hundred milliseconds at shutdown
// is worth the user's clipboard.
bool paste_watch_run_now(const char* why, bool may_defer) {
    PendingPaste& p = pending_paste();
    if (!p.active) return false;
    cancel_restore_timer();

    ClipboardGuardState scratch;
    ClipboardGuardState& st = (p.report_to != nullptr) ? *p.report_to : scratch;
    st.sequence_at_set = p.sequence_at_set;

    ++p.attempts;
    bool settled = try_restore_once(p, st);

    if (!settled && may_defer && p.attempts < kMaxRestoreAttempts) {
        KD_LOG_DEBUG("the clipboard could not be opened to restore it (attempt %u); trying again "
                     "in %u ms",
                     p.attempts, kRestoreRetryMs);
        arm_restore_timer(kRestoreRetryMs);
        return true;  // still outstanding, and still ours to finish
    }
    while (!settled && !may_defer && p.attempts < kMaxRestoreAttempts) {
        Sleep(kRestoreRetryMs);
        ++p.attempts;
        settled = try_restore_once(p, st);
    }

    st.restore_pending = false;
    if (!settled) {
        // Said plainly, because from the user's side this is "the program kept my clipboard".
        KD_LOG_WARN("the clipboard could not be opened to restore it after %u attempts; the "
                    "dictated text is still on it",
                    p.attempts);
    }
    KD_LOG_DEBUG("deferred clipboard restore ran (%s): restored=%d left_alone=%d target_read=%d "
                 "attempts=%u",
                 why, st.restored ? 1 : 0, st.skipped_because_changed ? 1 : 0,
                 st.target_read_it ? 1 : 0, p.attempts);

    clipboard_backup_destroy(p.backup);
    p = PendingPaste{};
    return true;
}

// Hand the snapshot to a second paste that started before the first one was put back. The user's
// clipboard is what is in that snapshot — taking a fresh one here would snapshot *our own*
// dictation and then "restore" it, which is how a second utterance in one session would leave
// the first one's text on the clipboard for good. main.cpp injects twice in a row routinely (a
// deferred flush and then the suffix), so this is the ordinary path, not the exotic one.
ClipboardBackup* paste_watch_adopt() {
    PendingPaste& p = pending_paste();
    if (!p.active) return nullptr;
    cancel_restore_timer();
    ClipboardBackup* b = p.backup;
    if (p.report_to != nullptr) p.report_to->restore_pending = false;
    p = PendingPaste{};
    KD_LOG_DEBUG("a second paste began before the first restore; the snapshot carries over");
    return b;
}

// Abandon the restore and leave our text where it is. For the one caller that means it:
// set_clipboard_text(), whose whole purpose is to put a dictation on the clipboard and keep it
// there. Running the pending restore instead would wipe the recovery copy a moment after the
// user asked for it.
void paste_watch_cancel(const char* why) {
    PendingPaste& p = pending_paste();
    if (!p.active) return;
    cancel_restore_timer();
    if (p.report_to != nullptr) {
        p.report_to->restore_pending = false;
        p.report_to->skipped_because_changed = true;  // our text is on the clipboard, on purpose
    }
    clipboard_backup_destroy(p.backup);
    p = PendingPaste{};
    KD_LOG_DEBUG("the deferred clipboard restore was abandoned: %s", why);
}

// The diagnostics block a pending restore reports into is about to stop existing. Restore now
// rather than write to freed memory later — and rather than silently keep the user's clipboard.
void paste_watch_forget_reporter(const ClipboardGuardState* st) {
    PendingPaste& p = pending_paste();
    if (!p.active || p.report_to != st) return;
    paste_watch_run_now("the injector was destroyed with a paste outstanding", false);
}

}  // namespace injector_detail

// ════════════════════════════════════════════════════════════════════════════════════════════
//  Target identity
// ════════════════════════════════════════════════════════════════════════════════════════════

bool TargetIdentity::same_as(const TargetIdentity& other) const noexcept {
    return top_level == other.top_level && focus == other.focus && thread_id == other.thread_id &&
           process_id == other.process_id;
}

namespace {

std::string process_name_of(DWORD pid, uint32_t* os_error) {
    HANDLE raw = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (raw == nullptr) {
        if (os_error != nullptr) *os_error = GetLastError();
        return {};
    }
    HandleGuard proc(raw);
    wchar_t path[MAX_PATH * 2] = {};
    DWORD len = static_cast<DWORD>(std::size(path));
    if (QueryFullProcessImageNameW(proc.h, 0, path, &len) == FALSE) {
        if (os_error != nullptr) *os_error = GetLastError();
        return {};
    }
    std::wstring_view full(path, len);
    const size_t slash = full.find_last_of(L"\\/");
    const std::wstring_view base = (slash == std::wstring_view::npos) ? full : full.substr(slash + 1);
    return ascii_lower(utf8::from_utf16_or_empty(base));
}

}  // namespace

Result<TargetIdentity> capture_foreground_target() {
    HWND fg = GetForegroundWindow();
    if (fg == nullptr) {
        // Genuinely happens: during a desktop switch, while a screensaver starts, and for a few
        // hundred ms after a window closes. Not an error worth a stack trace, but not something to
        // paper over with "the last window we saw" either.
        return Status{StatusCode::Unavailable, "no foreground window"};
    }

    TargetIdentity t;
    t.top_level = reinterpret_cast<uintptr_t>(fg);

    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(fg, &pid);
    t.thread_id = static_cast<uint32_t>(tid);
    t.process_id = static_cast<uint32_t>(pid);

    // GUITHREADINFO gives the focused control within the foreground thread. It is empty for every
    // application that draws its own caret and owns no child HWNDs — Electron, WSLg, games,
    // most of Chromium. That is expected, not a failure: `focus == 0` means "this target has no
    // Win32 focus to compare", and validate_target() below says what we do about it.
    GUITHREADINFO gti{};
    gti.cbSize = sizeof(gti);
    if (GetGUIThreadInfo(tid, &gti) != FALSE) {
        t.focus = reinterpret_cast<uintptr_t>(gti.hwndFocus);
    }

    uint32_t name_err = 0;
    t.process_name = process_name_of(pid, &name_err);

    uint32_t integrity_err = 0;
    t.integrity = injector_detail::process_integrity(t.process_id, &integrity_err);
    if (t.integrity == IntegrityLevel::Unknown) {
        // Failing to open the target process is itself evidence of a privilege boundary — a
        // protected process, a service, or another user. It is recorded here and turned into
        // ElevatedUnsupported by check_supported(), never into "we'll try and see".
        KD_LOG_WARN("target pid %u: integrity unreadable (open err=%lu, name err=%lu)",
                    t.process_id, static_cast<unsigned long>(integrity_err),
                    static_cast<unsigned long>(name_err));
    }

    t.is_password_field = detect_password_field(reinterpret_cast<HWND>(t.focus));
    t.is_console = detect_console(fg, t.process_name);

    return t;
}

bool validate_target(const TargetIdentity& t) {
    if (!t.is_valid()) return false;
    HWND want = reinterpret_cast<HWND>(t.top_level);
    if (IsWindow(want) == FALSE) return false;

    HWND fg = GetForegroundWindow();
    if (fg != want) return false;

    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(fg, &pid);
    if (static_cast<uint32_t>(tid) != t.thread_id) return false;
    if (static_cast<uint32_t>(pid) != t.process_id) return false;

    if (t.focus != 0) {
        // The focused control is compared too, deliberately: tabbing between fields inside one
        // window is a target change, because text meant for a search box does not belong in the
        // document behind it (injector.h, TargetIdentity::same_as).
        GUITHREADINFO gti{};
        gti.cbSize = sizeof(gti);
        if (GetGUIThreadInfo(tid, &gti) == FALSE) return false;
        if (reinterpret_cast<uintptr_t>(gti.hwndFocus) != t.focus) return false;
    }
    // When t.focus == 0 the target had no Win32 focus to record, so there is nothing to compare
    // and we do not invent a comparison. Requiring it to *stay* zero would abort in exactly the
    // targets plan.md §1.5 cares most about (WSLg, terminals, Electron), which report no focused
    // child at any time. The top-level + thread + process triple is the guarantee there.

    return true;
}

bool get_clipboard_text(std::string& out) {
    out.clear();
    if (IsClipboardFormatAvailable(CF_UNICODETEXT) == FALSE) return false;

    uint32_t os_error = 0;
    // Reached through injector_detail because the retry ladder lives in an anonymous namespace
    // inside it — one implementation of "open the clipboard without giving up on the first
    // ERROR_ACCESS_DENIED", shared with the injection path.
    if (!injector_detail::open_clipboard_with_retry(&os_error)) {
        // Losing a paste is a small thing next to the failures this file usually reports, so this
        // is a debug line rather than a warning: another process holding the clipboard for a few
        // milliseconds is routine, and the retry ladder above has already waited out the common
        // case.
        KD_LOG_DEBUG("clipboard read: could not open (os_error=%lu)",
                     static_cast<unsigned long>(os_error));
        return false;
    }
    // Every path out from here must close the clipboard, including the early returns below.
    struct Closer {
        ~Closer() { CloseClipboard(); }
    } closer;

    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h == nullptr) return false;
    const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h));
    if (p == nullptr) return false;

    // GlobalSize is a bound, not a length: the block is NUL-terminated and may be padded.
    const size_t units = GlobalSize(h) / sizeof(wchar_t);
    size_t len = 0;
    while (len < units && p[len] != L'\0') ++len;

    size_t bad = 0;
    const bool ok = utf8::from_utf16(std::wstring_view(p, len), out, &bad);
    GlobalUnlock(h);
    if (!ok) {
        // No repair, ever: util/utf8.h forbids U+FFFD substitution anywhere in this program, and
        // this text is on its way into a buffer bound for the user's document.
        KD_LOG_WARN("clipboard holds text that is not well-formed at index %zu; refusing to "
                    "paste it rather than repairing it",
                    bad);
        out.clear();
        return false;
    }
    return !out.empty();
}

bool set_clipboard_text(std::string_view utf8) {
    if (utf8.empty()) return false;
    // A restore outstanding from a paste a moment ago would fire seconds from now and wipe what
    // this call is putting there — which is the recovery copy, reached by a user who has already
    // watched one mechanism fail. Abandoning it is the whole intent of this entry point.
    injector_detail::paste_watch_cancel("the last dictation was copied to the clipboard on "
                                        "purpose");
    // The guard state is required by the shared helper and discarded here: it exists to decide
    // whether a later *restore* is safe, and this path never restores. See the header.
    ClipboardGuardState ignored;
    uint32_t os_error = 0;
    if (injector_detail::clipboard_set_utf8(utf8, ignored, &os_error)) return true;
    KD_LOG_WARN("clipboard rescue failed: os_error=%lu", static_cast<unsigned long>(os_error));
    return false;
}

bool finish_pending_clipboard_restore() {
    return injector_detail::paste_watch_run_now("the process is shutting down", false);
}

bool secure_desktop_active() {
    // ─── What this can and cannot promise ───────────────────────────────────────────────────
    //
    // Input is scoped to a desktop, so synthetic input from our process physically cannot reach
    // the secure desktop; we are not the thing standing between a UAC prompt and a keystroke.
    // This check exists so the *state is explicit and reportable* — the session shows "refused:
    // secure desktop" instead of appearing to work and typing nothing.
    //
    // It is inherently racy: the desktop can switch between this call and SendInput a microsecond
    // later. That race is harmless for the reason above.
    HDESK raw = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (raw == nullptr) {
        // Cannot open the input desktop => the input desktop is not ours. That is exactly what a
        // UAC prompt (Winlogon desktop) or a locked workstation looks like from here. Fail closed.
        return true;
    }
    DesktopGuard desk(raw);

    wchar_t name[128] = {};
    DWORD needed = 0;
    if (GetUserObjectInformationW(desk.h, UOI_NAME, name, sizeof(name), &needed) == FALSE) {
        return true;
    }
    // "Default" is the interactive desktop. "Winlogon" is the secure desktop (UAC, lock screen,
    // Ctrl+Alt+Del); "Screen-saver" is the screensaver desktop.
    return !iequals_w(name, L"Default");
}

namespace {

// Diagnostics only, and deliberately NOT a gate.
//
// WTSGetActiveConsoleSessionId names the session attached to the physical console. Our session
// differing from it means either that another user is at the console (fast user switching) or that
// we are in a remote-desktop session — and this API cannot distinguish those two. Refusing on a
// mismatch would refuse every RDP session, and docs/supported-targets.md declares remote-session
// behaviour rather than making users discover it. So the desktop-name check above is the gate and
// this is a log line.
bool session_is_attached_to_console() noexcept {
    DWORD our_session = 0;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &our_session) == FALSE) return true;
    const DWORD console_session = WTSGetActiveConsoleSessionId();
    if (console_session == 0xFFFFFFFFu) return false;  // no session currently attached
    return our_session == console_session;
}

}  // namespace

// ════════════════════════════════════════════════════════════════════════════════════════════
//  Self-injection mark
// ════════════════════════════════════════════════════════════════════════════════════════════

const InjectionMark& injection_mark() {
    static const InjectionMark mark = [] {
        InjectionMark m;
        uint64_t r = 0;
        const NTSTATUS s = BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&r), sizeof(r),
                                           BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (s < 0 || r == 0) {
            // Fall back to something merely unpredictable rather than cryptographic. The property
            // that matters is *uniqueness across processes*, not secrecy: this value only decides
            // whether our own hook ignores an event.
            r = (static_cast<uint64_t>(GetCurrentProcessId()) << 32) ^ qpc_now();
            if (r == 0) r = 0x6B766974646963ull;  // "kvitdic"
        }
        m.extra_info = static_cast<uintptr_t>(r);
        if (m.extra_info == 0) m.extra_info = 1;
        return m;
    }();
    return mark;
}

// ════════════════════════════════════════════════════════════════════════════════════════════
//  Injector
// ════════════════════════════════════════════════════════════════════════════════════════════

// The grace after the target reads, capped: it is a courtesy to a slow reader, not a window for
// anything, and a mis-set value should not leave the user's clipboard displaced for seconds.
// settings.cpp accepts 0..5000 for the field; anything above this is clamped and said so.
constexpr uint32_t kMaxSettleMs = 500;

struct Injector::Impl {
    InjectConfig cfg;
    ClipboardGuardState last_clipboard;
    PrecisionSleeper sleeper;

    // Reused across calls so a per-word commit does not allocate three vectors every time. The
    // module is single-threaded (injector.h), so shared scratch is safe here.
    std::vector<injector_detail::SynthKey> events;
    std::vector<injector_detail::SynthAtom> atoms;
    std::vector<INPUT> inputs;

    // The two mechanisms live here rather than on Injector because they need the scratch vectors
    // and the sleeper above, and injector.h deliberately exposes neither — the header is
    // Windows-free (no INPUT, no timer handle), so anything touching them has to be behind the
    // pimpl. `t0` is the caller's start timestamp, passed down so InjectResult::elapsed_us
    // measures the whole call including the up-front validation, not just the mechanism.
    InjectResult inject_via_sendinput(const TargetIdentity& target, std::string_view utf8_text,
                                      uint64_t t0);
    InjectResult inject_via_clipboard(const TargetIdentity& target, std::string_view utf8_text,
                                      uint64_t t0);
};

Injector::Injector(InjectConfig cfg) : impl_(new Impl()) {
    impl_->cfg = cfg;
    // Touch the mark now so the random draw happens at construction rather than in the middle of
    // the first injection, and so hotkey.h can be told the value before any input is generated.
    (void)injection_mark();
    KD_LOG_INFO("injector: method=%s pacing=%u chars/%u us paste_settle=%u ms hi-res-timer=%d "
                "console_session=%d our_integrity=%s",
                to_string(impl_->cfg.method), impl_->cfg.chars_per_batch,
                impl_->cfg.inter_batch_delay_us, impl_->cfg.paste_settle_ms,
                impl_->sleeper.high_resolution() ? 1 : 0, session_is_attached_to_console() ? 1 : 0,
                to_string(injector_detail::current_process_integrity()));
    // The settings file accepts a wider range than the clipboard path will honour. It used to
    // clamp in silence, so a user who set 1000 got 500 and had no way to find that out.
    if (impl_->cfg.paste_settle_ms > kMaxSettleMs) {
        KD_LOG_WARN("inject.paste_settle_ms is %u ms; the clipboard path clamps it to %u",
                    impl_->cfg.paste_settle_ms, kMaxSettleMs);
    }
}

Injector::~Injector() {
    // A deferred restore reports into impl_->last_clipboard. Running it now is the only way that
    // pointer is never stale, and it is also the right thing for the user: an injector going
    // away with a paste outstanding must not take the clipboard with it.
    injector_detail::paste_watch_forget_reporter(&impl_->last_clipboard);
    delete impl_;
}

const InjectConfig& Injector::config() const noexcept { return impl_->cfg; }

void Injector::set_config(const InjectConfig& cfg) { impl_->cfg = cfg; }

const ClipboardGuardState& Injector::last_clipboard_state() const noexcept {
    return impl_->last_clipboard;
}

InjectStatus Injector::check_supported(const TargetIdentity& target) {
    if (!target.is_valid()) return InjectStatus::Failed;

    if (secure_desktop_active()) return InjectStatus::SecureDesktop;

    if (impl_->cfg.refuse_password_fields && target.is_password_field) {
        return InjectStatus::PasswordField;
    }

    if (impl_->cfg.refuse_elevated_targets) {
        const IntegrityLevel ours = injector_detail::current_process_integrity();
        if (!injector_detail::integrity_allows_injection(ours, target.integrity)) {
            // This is the explicit unsupported state plan.md §1.7 demands. It is decided here,
            // before a single character is spoken, precisely because SendInput's return value
            // would tell us nothing afterwards.
            return InjectStatus::ElevatedUnsupported;
        }
    }

    return InjectStatus::Ok;
}

namespace {

uint32_t bytes_for_complete_atoms(const std::vector<injector_detail::SynthAtom>& atoms,
                                  size_t first, size_t last, uint32_t events_accepted) noexcept {
    uint32_t consumed = 0;
    uint32_t bytes = 0;
    for (size_t i = first; i < last; ++i) {
        if (consumed + atoms[i].event_count > events_accepted) break;
        consumed += atoms[i].event_count;
        bytes += atoms[i].utf8_bytes;
    }
    return bytes;
}

uint32_t sample_stuck_modifiers() noexcept {
    uint32_t mods = 0;
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) mods |= injector_detail::kStuckShift;
    if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) mods |= injector_detail::kStuckAlt;
    return mods;
}

}  // namespace

InjectResult Injector::inject(const TargetIdentity& target, std::string_view utf8_text) {
    const uint64_t t0 = qpc_now();
    InjectResult r;

    auto finish = [&](InjectStatus s) -> InjectResult {
        r.status = s;
        r.elapsed_us = qpc_elapsed_us(t0, qpc_now());
        return r;
    };

    if (utf8_text.empty()) {
        // Not an error and not a no-op worth logging: the commit engine legitimately produces an
        // empty commit when the stable prefix has not advanced.
        return finish(InjectStatus::Ok);
    }

    const InjectStatus supported = check_supported(target);
    if (supported != InjectStatus::Ok) {
        r.detail = "refused up front: ";
        r.detail += to_string(supported);
        return finish(supported);
    }

    // Revalidated here, immediately before injecting, and again before every pacing batch below.
    // Once per session is not enough: the whole point of plan.md §7's focus-race gate is that the
    // foreground window can change between the check and the keystroke.
    if (!validate_target(target)) {
        r.detail = "target changed between capture and injection";
        return finish(InjectStatus::TargetChanged);
    }

    switch (impl_->cfg.method) {
        case InjectMethod::Clipboard: return impl_->inject_via_clipboard(target, utf8_text, t0);
        case InjectMethod::SendInput: break;
    }
    return impl_->inject_via_sendinput(target, utf8_text, t0);
}

InjectResult Injector::Impl::inject_via_sendinput(const TargetIdentity& target,
                                                  std::string_view utf8_text, uint64_t t0) {
    InjectResult r;
    auto finish = [&](InjectStatus s) -> InjectResult {
        r.status = s;
        r.elapsed_us = qpc_elapsed_us(t0, qpc_now());
        return r;
    };

    size_t bad = 0;
    const char* reason = nullptr;
    if (!injector_detail::build_text_events(utf8_text, injection_mark().extra_info, events, atoms,
                                            &bad, &reason)) {
        r.detail = reason != nullptr ? reason : "bad-text";
        r.detail += " at byte offset ";
        r.detail += std::to_string(bad);
        return finish(InjectStatus::Failed);
    }
    if (atoms.empty()) return finish(InjectStatus::Ok);

    const uint32_t per_batch = cfg.chars_per_batch == 0 ? 0xFFFFFFFFu : cfg.chars_per_batch;

    // A rough warning rather than a silent behaviour change: the user chose this pacing, and
    // quietly overriding it would make the Phase 0B measurements meaningless. But a multi-second
    // stall on the UI thread should not be a surprise either.
    if (cfg.inter_batch_delay_us != 0 && per_batch != 0xFFFFFFFFu) {
        const uint64_t batches = (atoms.size() + per_batch - 1) / per_batch;
        const uint64_t pace_us = batches * cfg.inter_batch_delay_us;
        if (pace_us > 200000ull) {
            KD_LOG_WARN("paced injection will block the UI thread for ~%llu ms (%zu chars)",
                        static_cast<unsigned long long>(pace_us / 1000), atoms.size());
        }
    }

    size_t atom_i = 0;
    uint32_t bytes = 0;
    while (atom_i < atoms.size()) {
        if (!validate_target(target)) {
            // Mid-injection focus change. The document now holds a prefix of the intended text.
            // Reported, never repaired — plan.md §1.4 forbids sending backspaces into someone
            // else's document, and this is exactly the case that would tempt it.
            r.bytes_injected = bytes;
            r.detail = "target changed mid-injection";
            return finish(bytes == 0 ? InjectStatus::TargetChanged : InjectStatus::PartialWrite);
        }

        const size_t first_atom = atom_i;
        const uint32_t ev_begin = atoms[atom_i].first_event;
        uint32_t ev_count = 0;
        uint32_t batch_bytes = 0;
        uint32_t chars = 0;
        while (atom_i < atoms.size() && chars < per_batch) {
            ev_count += atoms[atom_i].event_count;
            batch_bytes += atoms[atom_i].utf8_bytes;
            ++atom_i;
            ++chars;
        }

        inputs.resize(ev_count);
        for (uint32_t i = 0; i < ev_count; ++i) {
            fill_input(inputs[i], events[ev_begin + i]);
        }

        const UINT sent = SendInput(ev_count, inputs.data(), sizeof(INPUT));
        if (sent != ev_count) {
            r.os_error = GetLastError();
            bytes += bytes_for_complete_atoms(atoms, first_atom, atom_i, sent);
            r.bytes_injected = bytes;
            // ERROR_ACCESS_DENIED here means the input was blocked — BlockInput, or a
            // higher-integrity process holding the input. It is NOT how UIPI reports itself:
            // UIPI-blocked input returns success and goes nowhere, which is why the integrity
            // comparison happens up front instead of being inferred from this value.
            r.detail = "SendInput accepted " + std::to_string(sent) + " of " +
                       std::to_string(ev_count) + " events";
            KD_LOG_ERROR("SendInput partial: %u/%u events, err=%u", sent, ev_count, r.os_error);
            return finish(bytes == 0 ? InjectStatus::Failed : InjectStatus::PartialWrite);
        }

        bytes += batch_bytes;

        if (atom_i < atoms.size() && cfg.inter_batch_delay_us != 0) {
            sleeper.wait_us(cfg.inter_batch_delay_us);
        }
    }

    r.bytes_injected = bytes;
    return finish(InjectStatus::Ok);
}

InjectResult Injector::Impl::inject_via_clipboard(const TargetIdentity& target,
                                                  std::string_view utf8_text, uint64_t t0) {
    InjectResult r;
    auto finish = [&](InjectStatus s) -> InjectResult {
        r.status = s;
        r.elapsed_us = qpc_elapsed_us(t0, qpc_now());
        return r;
    };

    last_clipboard = ClipboardGuardState{};

    // Owned for the whole function so every early return still frees the snapshot, and released
    // to the deferred restore on the one path that arms it.
    struct BackupHolder {
        injector_detail::ClipboardBackup* p = nullptr;
        explicit BackupHolder(injector_detail::ClipboardBackup* adopted)
            : p(adopted != nullptr ? adopted : injector_detail::clipboard_backup_create()) {}
        ~BackupHolder() { injector_detail::clipboard_backup_destroy(p); }
        injector_detail::ClipboardBackup* release() {
            injector_detail::ClipboardBackup* q = p;
            p = nullptr;
            return q;
        }
        BackupHolder(const BackupHolder&) = delete;
        BackupHolder& operator=(const BackupHolder&) = delete;
    };

    // A paste whose restore has not run yet still has the user's clipboard in its snapshot, and
    // our own dictation on the clipboard. Snapshotting now would preserve *that* — which is how
    // the second of two injections in one session (main.cpp flushes, then injects the suffix)
    // would end up "restoring" the first one's text and keeping it for good.
    injector_detail::ClipboardBackup* adopted = injector_detail::paste_watch_adopt();
    BackupHolder backup(adopted);

    uint32_t err = 0;
    if (adopted != nullptr) {
        last_clipboard.had_previous =
            injector_detail::clipboard_backup_format_count(backup.p) != 0;
    } else if (!injector_detail::clipboard_save(backup.p, last_clipboard, &err)) {
        r.os_error = err;
        r.detail = "could not open the clipboard to snapshot it";
        return finish(InjectStatus::ClipboardUnavailable);
    }

    // Delay-rendered, so that the target asking Windows for the bytes is the signal that says
    // when the restore may safely run. `ours` comes back in the form the clipboard holds it, so
    // the ownership check compares against exactly what was published rather than converting a
    // second time and giving the two conversions a chance to disagree.
    std::wstring ours;
    if (!injector_detail::clipboard_set_delayed(utf8_text, ours, last_clipboard, &err)) {
        r.os_error = err;
        r.detail = "could not place text on the clipboard";
        return finish(InjectStatus::ClipboardUnavailable);
    }

    // Revalidate as late as possible: everything above took milliseconds during which the user
    // could have alt-tabbed, and a paste chord sent into the wrong window pastes our whole text
    // at once rather than one wrong character.
    if (!validate_target(target)) {
        // Restored inline rather than deferred: no chord went out, so there is nobody to wait
        // for and no reason to leave the user's clipboard displaced for a second and a half.
        injector_detail::clipboard_restore(backup.p, last_clipboard, ours);
        r.detail = "target changed before the paste chord";
        return finish(InjectStatus::TargetChanged);
    }

    // Ctrl+Shift+V for consoles. There is no per-call knob for this in InjectConfig, so the
    // signal is TargetIdentity::is_console — which capture_foreground_target() fills in from the
    // window class and the executable name, and which a caller applying an AppPolicy override
    // (AppPolicy::use_ctrl_shift_v) can set before calling. See the module report: the cleaner
    // home for this is a field on InjectConfig.
    // OR, not override: the detection stands on its own and the setting only raises the floor.
    // A user who ticks the global box has said "this machine's terminals want the longer chord";
    // a target detect_console already recognised wanted it either way.
    injector_detail::build_paste_chord(target.is_console || cfg.paste_ctrl_shift_v,
                                       sample_stuck_modifiers(),
                                       injection_mark().extra_info, events);

    inputs.resize(events.size());
    for (size_t i = 0; i < events.size(); ++i) {
        fill_input(inputs[i], events[i]);
    }
    const UINT want = static_cast<UINT>(inputs.size());
    const UINT sent = SendInput(want, inputs.data(), sizeof(INPUT));
    if (sent != want) {
        r.os_error = GetLastError();
        r.detail = "paste chord: SendInput accepted " + std::to_string(sent) + " of " +
                   std::to_string(want) + " events";
        injector_detail::clipboard_restore(backup.p, last_clipboard, ours);
        return finish(InjectStatus::PasteChordRejected);
    }

    // ─── Handing the restore to the message loop ────────────────────────────────────────────
    //
    // This used to block here for paste_settle_ms and then restore, which was a guess at how
    // long the target takes to process a keystroke it has not even received yet. The guess lost:
    // see the delayed-rendering section above, and the VS Code pastes it silently turned into
    // the user's previous clipboard contents.
    //
    // Nothing is waited for now. The watch below restores when the target has actually read the
    // text, or when the fallback timeout says it never will, and both run on the UI thread from
    // its own message loop — so a long paste no longer stalls the overlay either.
    const uint32_t settle_ms = cfg.paste_settle_ms > kMaxSettleMs ? kMaxSettleMs
                                                                  : cfg.paste_settle_ms;
    injector_detail::paste_watch_arm(backup.release(), std::move(ours), &last_clipboard,
                                     last_clipboard.sequence_at_set, target.process_id, settle_ms,
                                     injector_detail::kRestoreFallbackMs);

    // ─── What Ok means here, honestly ───────────────────────────────────────────────────────
    //
    // Ok means the chord was accepted by the input system and our text was on the clipboard when
    // it went out. It does NOT mean the target pasted anything: an application that binds Ctrl+V
    // to something else, or that ignores it entirely, produces this same result with no text
    // inserted. There is no signal that would distinguish them, so InjectStatus::PasteChordRejected
    // is reserved for the case we *can* observe — SendInput refusing the events. Which targets
    // actually consume the chord is a measured fact recorded in docs/supported-targets.md, not
    // something guessed at run time.
    r.bytes_injected = utf8_text.size();
    // Nothing is said here about the clipboard's fate: at this instant our text is still on it
    // and the restore has not run. ClipboardGuardState::restore_pending is the honest answer for
    // a caller asking "is my dictation sitting on the clipboard?", and it says "not for long".
    r.detail = "clipboard restore deferred until the target reads the text";
    return finish(InjectStatus::Ok);
}

// ════════════════════════════════════════════════════════════════════════════════════════════
//  Per-app policy
// ════════════════════════════════════════════════════════════════════════════════════════════

const AppPolicy* find_app_policy(const std::vector<AppPolicy>& policies,
                                 std::string_view process_name) {
    if (process_name.empty()) return nullptr;
    const std::string want = ascii_lower(process_name);
    for (const AppPolicy& p : policies) {
        if (ascii_lower(p.process_name) == want) return &p;
    }
    return nullptr;
}

}  // namespace kd
