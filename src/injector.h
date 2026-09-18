// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// Text injection into the focused window, and the guards that decide when not to.
//
// Implements: plan.md §1.7 (text injection and target safety), and the parts of plan.md §7 that
// gate on it — "no wrong-window injection under automated focus-race tests", "exact Unicode
// injection in every declared supported target", "clipboard restored only when we still own the
// state we set".
//
// ─── Both mechanisms ship ───────────────────────────────────────────────────────────────────
//
// Neither is obviously better and the answer is likely per-target, so both are implemented and
// both are selectable globally and per-app:
//
//   1. **SendInput with KEYEVENTF_UNICODE** — universal across Win32/UWP/Electron/games. Slow
//      for long text (~1–2 ms/char with pacing), and some apps drop fast synthetic input.
//   2. **Clipboard + Ctrl+V** — fast and atomic, but clobbers the clipboard and fails where
//      Ctrl+V is bound elsewhere (terminals want Ctrl+Shift+V).
//
// **UI Automation TextPattern/ValuePattern is not implemented.** It is absent in the targets
// that matter most here, and plan.md §1.5 commits to a no-UIA design. Do not add it as a third
// method without revisiting that decision in the plan first.
//
// ─── UIPI is a hard boundary, not a detection problem ───────────────────────────────────────
//
// SendInput cannot inject into a higher-integrity target: an elevated window silently ignores
// input from a non-elevated process, and Microsoft notes the return value does not identify
// UIPI as the cause. So elevated targets get an **explicit unsupported state**, detected up
// front by comparing integrity levels at session start and surfaced in the overlay and tray.
// Never a generic "injection seemed not to work" guess, and never inferred from pixels — see
// anchor.h, which is forbidden from returning an injection verdict.
//
// ─── Target identity is validated before every injection ────────────────────────────────────
//
// Target identity = top-level foreground HWND at hotkey press, plus focused thread and focused
// control where obtainable (GetGUIThreadInfo().hwndFocus). Any change ends the session rather
// than continuing into a new target. This is the rule that plan.md §7's focus-race gate tests,
// and it is why `inject()` takes the identity captured at session start rather than looking up
// the foreground window itself.
//
// ─── Refusals ───────────────────────────────────────────────────────────────────────────────
//
// Refuse to run on the secure desktop, during UAC prompts, on the lock screen, and into
// password fields (ES_PASSWORD and the heuristics that work where it is not detectable).
// Remote-session behaviour is declared in docs/supported-targets.md, not discovered by users.
//
// ─── Threading ──────────────────────────────────────────────────────────────────────────────
//
// **Everything here runs on the UI thread**, the one that owns the message loop, and never on
// the hook thread or the IPC thread.
//
// And for the clipboard method, "the one that owns the message loop" is load-bearing rather than
// descriptive: that thread **must keep pumping** after inject() returns, because the restore that
// puts the user's clipboard back arrives as a WM_TIMER. A caller that injects and then stops
// pumping keeps the user's clipboard until it pumps again or calls
// finish_pending_clipboard_restore(). The application's message loop satisfies this without
// trying; the harnesses (tools/probe, tests/test_injector.cpp) had to be taught, and both were.
//
// Two reasons, both concrete. The clipboard path calls OpenClipboard/SetClipboardData, which are
// tied to the calling thread and to a window handle, and interleaving them from two threads
// produces failures that reproduce once a week. And the SendInput path is paced with waits
// between chunks; running it on the low-level hook thread would stall the hook, which Windows
// responds to by silently unhooking it (plan.md §1.8) — losing the hotkey while typing.
//
// A consequence implementers get wrong: **our own SendInput traffic arrives back through our own
// hooks.** Every synthetic event we generate is flagged LLKHF_INJECTED and must be ignored by
// the stop-on-input logic, or the first character we type ends the session that produced it.
// The filter lives in hotkey.h; `InjectionMark` below is how this module tells it what is ours.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "util/result.h"

namespace kd {

// ─── Method ─────────────────────────────────────────────────────────────────────────────────

enum class InjectMethod : uint8_t {
    SendInput,  // KEYEVENTF_UNICODE, one WM_CHAR-equivalent per UTF-16 code unit
    Clipboard,  // set CF_UNICODETEXT, send the paste chord, restore
};

const char* to_string(InjectMethod m) noexcept;
bool inject_method_from_string(std::string_view s, InjectMethod& out) noexcept;

// ─── Outcome ────────────────────────────────────────────────────────────────────────────────

enum class InjectStatus : uint8_t {
    Ok,
    TargetChanged,        // foreground or focus moved since the identity was captured
    ElevatedUnsupported,  // UIPI: our integrity level is below the target's (explicit state)
    SecureDesktop,        // UAC prompt or lock screen is up
    PasswordField,        // refused by policy
    ClipboardUnavailable, // OpenClipboard failed, or another process holds it
    PasteChordRejected,   // clipboard method, the target never consumed the paste
    PartialWrite,         // some events were accepted and some were not — the document now
                          // contains a prefix of the intended text. Reported, never repaired.
    Failed,
};

const char* to_string(InjectStatus s) noexcept;

struct InjectResult {
    InjectStatus status = InjectStatus::Failed;
    size_t bytes_injected = 0;   // UTF-8 bytes actually sent; < input size on PartialWrite
    uint32_t os_error = 0;
    uint64_t elapsed_us = 0;     // feeds the UI/commit-delay term of plan.md §3 Phase 2 item 4
    std::string detail;
};

// ─── Target identity ────────────────────────────────────────────────────────────────────────

// Windows integrity levels, coarse. Compared numerically: injection into a target whose level
// exceeds ours is refused up front.
enum class IntegrityLevel : uint8_t { Untrusted, Low, Medium, MediumPlus, High, System, Unknown };

const char* to_string(IntegrityLevel l) noexcept;

struct TargetIdentity {
    uintptr_t top_level = 0;   // HWND, as an opaque value so this header stays Windows-free
    uintptr_t focus = 0;       // GetGUIThreadInfo().hwndFocus, 0 when unobtainable
    uint32_t thread_id = 0;
    uint32_t process_id = 0;
    IntegrityLevel integrity = IntegrityLevel::Unknown;
    bool is_password_field = false;
    bool is_console = false;     // conhost / Windows Terminal: paste chord is Ctrl+Shift+V
    std::string process_name;    // for per-app policy lookup and for the supported-target matrix

    [[nodiscard]] bool is_valid() const noexcept { return top_level != 0; }

    // Identity comparison used by validate(). Deliberately compares the focused control too:
    // tabbing between fields inside one window is a target change, because text meant for a
    // search box does not belong in the document behind it.
    [[nodiscard]] bool same_as(const TargetIdentity& other) const noexcept;
};

// Capture the current foreground target. Called once at hotkey press.
Result<TargetIdentity> capture_foreground_target();

// Re-check that the captured identity is still the foreground target. Called immediately before
// **every** injection, not once per session.
bool validate_target(const TargetIdentity& t);

// True when the secure desktop is active (UAC / lock screen). Injection is refused outright.
bool secure_desktop_active();

// Put text on the clipboard and leave it there. **Not** part of the injection path.
//
// The clipboard *injection* method sets, pastes and then restores the previous contents
// (ClipboardGuardState). This does the opposite deliberately: it is the last resort for text that
// has nowhere else to go — a whole session accumulated under Cadence::EndOfSession when the
// target window has since lost focus — so the user can press Ctrl+V and get it back. Restoring
// would defeat the entire purpose, which is why this is a separate entry point rather than a flag
// on the other one.
//
// Overwrites whatever the user had copied. That is a real cost, and it is the lesser one against
// silently discarding minutes of dictation; the caller is expected to say so in the log and the
// tray. UI thread only, like everything else here.
bool set_clipboard_text(std::string_view utf8);

// Read CF_UNICODETEXT off the clipboard. Returns false when the clipboard holds no text, cannot
// be opened, or holds text that is not well-formed once converted — never a partial or repaired
// answer, because util/utf8.h forbids U+FFFD substitution anywhere and this text is on its way
// into a buffer bound for the user's document.
//
// Added for Ctrl+V inside the compose buffer (docs/editing.md §4.3): EditBuffer is pure and has
// no Windows API, so the UI thread reads the clipboard here and hands the result to it as an
// ordinary insertion. That keeps one insertion path rather than two. UI thread only.
bool get_clipboard_text(std::string& out);

// Run any deferred clipboard restore now instead of when its timer fires, and return whether one
// was outstanding. Call it on the way out of the process: a pending restore whose timer never
// gets dispatched leaves the user's clipboard holding our dictation, which is the failure the
// restore exists to prevent. Safe to call when nothing is pending. UI thread only.
bool finish_pending_clipboard_restore();

// ─── Self-injection marking ─────────────────────────────────────────────────────────────────
//
// The handshake between this module and the hooks. Before injecting, the injector publishes a
// mark; the hook filter (hotkey.h) ignores input events carrying it, plus everything flagged
// LLKHF_INJECTED / LLMHF_INJECTED. plan.md §1.8: "Our own SendInput traffic must never stop the
// session producing it."
struct InjectionMark {
    // dwExtraInfo value stamped on every synthetic event this process generates. A per-process
    // random value rather than a constant, so that a second copy of this program (or another
    // tool that copied the constant) cannot cause us to ignore genuine user input.
    uintptr_t extra_info = 0;
};

const InjectionMark& injection_mark();

// ─── Clipboard preservation ─────────────────────────────────────────────────────────────────
//
// plan.md §1.7: restore only if we still own the clipboard state we set, compared through
// GetClipboardSequenceNumber against the value recorded at set time. If the user or another app
// changed the clipboard meanwhile, **leave it alone** — silently overwriting a user's copy is
// worse than leaving our text there. plan.md §7 gates on this.
//
// ─── The restore is deferred, and why ───────────────────────────────────────────────────────
//
// The restore does not happen inside inject(). A target reads the clipboard asynchronously when
// it processes our Ctrl+V, so a restore that runs before it has done so hands it the *previous*
// contents — which is exactly what happened: on this machine VS Code (Electron, whose paste
// crosses into the renderer process and back) routinely read later than the 60 ms we waited, and
// pasted whatever the user had copied before. Reported 2026-09-17.
//
// The wait is now a WM_TIMER rather than a sleep, so it costs nobody anything and can be long
// enough to stop being a race: `paste_settle_ms`, floored at a value no observed target has
// beaten. The clipboard API offers no completion signal to do better with; injector.cpp records
// what was tried instead (delayed rendering) and why it was rejected.
//
// The consequence for callers: when inject() returns, `restore_pending` is true and `restored`
// is not yet meaningful. A caller that needs to know whether our text is still on the clipboard
// must treat pending as "it is, and it is about to be taken back".
struct ClipboardGuardState {
    uint32_t sequence_at_set = 0;
    bool had_previous = false;
    bool restored = false;
    bool skipped_because_changed = false;  // the honest outcome; surfaced in diagnostics

    // A restore is scheduled and has not run yet. Our text is on the clipboard *now* and will not
    // be in a moment; see above.
    bool restore_pending = false;

    // How long our text was actually on the clipboard, measured rather than assumed: the wait is
    // the whole guarantee here, and a message loop that was busy makes the real figure longer
    // than the configured one. Zero until the restore has run.
    uint32_t on_clipboard_ms = 0;
};

// ─── Configuration ──────────────────────────────────────────────────────────────────────────

struct InjectConfig {
    InjectMethod method = InjectMethod::SendInput;

    // SendInput pacing. Zero means "send in one batch", which the fastest targets accept and
    // the flakiest drop. Measured per target in Phase 0B.
    uint32_t chars_per_batch = 32;
    uint32_t inter_batch_delay_us = 1000;

    // Clipboard path: how long our text stays on the clipboard after the paste chord before the
    // user's own contents are put back. It is a wait on an application we do not control, so it
    // is generous by default and floored in injector.cpp — 60 ms was the old default and it is
    // what the 2026-09-17 bug was made of. Nothing blocks for it; see ClipboardGuardState.
    uint32_t paste_settle_ms = 1200;

    // Send Ctrl+Shift+V rather than Ctrl+V, for every target rather than only the detected ones.
    //
    // Consoles are already found by TargetIdentity::is_console, and the two are OR'd: this raises
    // the floor, it does not replace the detection. It exists for the terminal that detection
    // misses, when the user would rather not write a per-app policy for it — and it is a blunt
    // instrument, because in most non-terminal applications Ctrl+Shift+V means "paste without
    // formatting" and in some it means nothing at all.
    bool paste_ctrl_shift_v = false;

    // Refuse rather than degrade. Both default true; both are settings a user can see.
    bool refuse_password_fields = true;
    bool refuse_elevated_targets = true;
};

// ─── The injector ───────────────────────────────────────────────────────────────────────────

class Injector {
public:
    explicit Injector(InjectConfig cfg);
    ~Injector();
    Injector(const Injector&) = delete;
    Injector& operator=(const Injector&) = delete;

    // Inject UTF-8 text into `target`. Validates the target first and refuses on any mismatch.
    // The text is converted to UTF-16 exactly once, through utf8::to_utf16 — the only place the
    // boundary is crossed (util/utf8.h).
    //
    // Append-only by construction: there is no delete, no backspace, no selection replacement
    // in this API, and none may be added. See commit_engine.h for why.
    InjectResult inject(const TargetIdentity& target, std::string_view utf8_text);

    // Up-front support check, run once at session start so the unsupported state is explicit
    // rather than discovered after the user has spoken a sentence.
    InjectStatus check_supported(const TargetIdentity& target);

    [[nodiscard]] const InjectConfig& config() const noexcept;
    void set_config(const InjectConfig& cfg);

    // Diagnostics from the last clipboard-method injection, for the plan.md §7 gate.
    [[nodiscard]] const ClipboardGuardState& last_clipboard_state() const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

// ─── Per-app policy ─────────────────────────────────────────────────────────────────────────
//
// plan.md §4 sweeps inject_method per app, and plan.md §3 Phase 4 ships "per-app injection
// policies". Keyed on the executable name, lowercased — not on the window title, which changes
// with the document, and not on the HWND class, which Electron apps share.
struct AppPolicy {
    std::string process_name;  // "code.exe", "windowsterminal.exe"
    InjectMethod method = InjectMethod::SendInput;
    bool use_ctrl_shift_v = false;  // terminals
    bool supported = true;          // false => explicit refusal with a reason in the UI
    std::string note;
};

// Returns nullptr when no policy is registered; the caller falls back to the global setting.
const AppPolicy* find_app_policy(const std::vector<AppPolicy>& policies,
                                 std::string_view process_name);

}  // namespace kd
