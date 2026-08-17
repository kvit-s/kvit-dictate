// Global hotkey and stop-on-input — WH_KEYBOARD_LL + WH_MOUSE_LL.
//
// Specification: hotkey.h. Implements plan.md §1.8 in full and the source-B half of §1.5.
//
// ─── How this file is split, and why the split is load-bearing ──────────────────────────────
//
//   1. InputFilter — a pure state machine. Given a sequence of InputEvents it decides: chord
//      matched / key-up to ignore / self-injected / real user input that stops the session. No
//      Windows types, no syscalls, no allocation, no locks. This is the part with tests
//      (tests/test_hotkey.cpp) and it is the only part where the interesting mistakes live.
//   2. The hook callbacks — translate a KBDLLHOOKSTRUCT/MSLLHOOKSTRUCT into an InputEvent, call
//      InputFilter, and PostMessage the answer. Nothing else happens there. Ever.
//   3. The hook thread's message loop — everything that could take more than a microsecond:
//      SendInput for the Start-menu tap, re-installing a hook Windows removed, applying a new
//      binding. The callback asks for these by posting to its own thread queue and returning.
//
// The reason for (3) rather than "just do it in the callback": plan.md §1.8, and it is not a
// style preference. Windows measures how long a low-level hook takes; past
// HKEY_CURRENT_USER\Control Panel\Desktop\LowLevelHooksTimeout (default 300 ms) it removes the
// hook, with no notification, no error code, and no way to ask whether you are still installed.
// The symptom is "the hotkey stopped working", hours later, unreproducibly. So the callback is
// structurally incapable of blocking: the only calls it makes are into InputFilter,
// PostMessage/PostThreadMessage (which never wait on a receiver) and CallNextHookEx.
//
// ─── What cannot be intercepted ─────────────────────────────────────────────────────────────
//
// Ctrl+Alt+Del is a Secure Attention Sequence handled by winlogon below the hook chain, and
// Win+L is handled by the same path. Neither is visible here and neither ever will be. That is
// deliberate Windows security, not a gap to work around.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>
#include <utility>

#include "hotkey.h"

#define KD_LOG_MODULE "hotkey"
#include "util/log.h"

namespace kd {

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Virtual-key constants: hotkey.h's copies, checked against the SDK
// ═══════════════════════════════════════════════════════════════════════════════════════════
//
// hotkey.h cannot include <windows.h> (the rule that keeps InputFilter unit-testable), so it
// carries its own copies of the VK_* values. These assertions are the entire justification for
// that duplication being safe.

static_assert(vk::kBack == VK_BACK && vk::kTab == VK_TAB && vk::kReturn == VK_RETURN, "VK drift");
static_assert(vk::kShift == VK_SHIFT && vk::kControl == VK_CONTROL && vk::kMenu == VK_MENU, "VK drift");
static_assert(vk::kCapsLock == VK_CAPITAL && vk::kEscape == VK_ESCAPE && vk::kSpace == VK_SPACE, "VK drift");
static_assert(vk::kLeft == VK_LEFT && vk::kUp == VK_UP && vk::kRight == VK_RIGHT && vk::kDown == VK_DOWN, "VK drift");
static_assert(vk::kLWin == VK_LWIN && vk::kRWin == VK_RWIN, "VK drift");
static_assert(vk::kF1 == VK_F1 && vk::kF13 == VK_F13 && vk::kF24 == VK_F24, "VK drift");
static_assert(vk::kLShift == VK_LSHIFT && vk::kRShift == VK_RSHIFT, "VK drift");
static_assert(vk::kLControl == VK_LCONTROL && vk::kRControl == VK_RCONTROL, "VK drift");
static_assert(vk::kLMenu == VK_LMENU && vk::kRMenu == VK_RMENU, "VK drift");
static_assert(vk::kF24 - vk::kF1 == 23, "VK_F1..VK_F24 must stay contiguous; the F-key naming relies on it");
static_assert(kMsgHotkey == WM_APP + 0x11u, "kMsgHotkey must be inside the WM_APP range");

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  StopInputReason
// ═══════════════════════════════════════════════════════════════════════════════════════════

const char* to_string(StopInputReason r) noexcept {
    switch (r) {
        case StopInputReason::Keystroke: return "keystroke";
        case StopInputReason::MouseClick: return "mouse_click";
        case StopInputReason::HotkeyToggle: return "hotkey_toggle";
        case StopInputReason::Escape: return "escape";
    }
    return "unknown";
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  HotkeySpec — display and parse
// ═══════════════════════════════════════════════════════════════════════════════════════════

namespace {

char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string lowered(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(ascii_lower(c));
    return out;
}

bool all_digits(std::string_view s) noexcept {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

// The display name of a key. Deliberately not GetKeyNameTextW: that is layout-dependent and
// localised, so a settings file written on a German layout would not parse on a US one. The
// hotkey strings in settings.json are a stable identifier, not UI prose.
std::string vk_display_name(uint32_t v) {
    if (v >= vk::k0 && v <= vk::k9) return std::string(1, static_cast<char>(v));
    if (v >= vk::kA && v <= vk::kZ) return std::string(1, static_cast<char>(v));
    if (v >= vk::kF1 && v <= vk::kF24) {
        return "F" + std::to_string(v - vk::kF1 + 1);
    }
    switch (v) {
        case vk::kSpace: return "Space";
        case vk::kEscape: return "Esc";
        case vk::kTab: return "Tab";
        case vk::kReturn: return "Enter";
        case vk::kBack: return "Backspace";
        case vk::kCapsLock: return "CapsLock";
        case vk::kLControl: return "LCtrl";
        case vk::kRControl: return "RCtrl";
        case vk::kLMenu: return "LAlt";
        case vk::kRMenu: return "RAlt";
        case vk::kLShift: return "LShift";
        case vk::kRShift: return "RShift";
        case vk::kLWin: return "LWinKey";
        case vk::kRWin: return "RWinKey";
        default: break;
    }
    // An unnamed code round-trips as a number rather than becoming unrepresentable. A settings
    // file is allowed to name a key this module has no friendly name for.
    char buf[16];
    std::snprintf(buf, sizeof(buf), "VK_0x%02X", static_cast<unsigned>(v & 0xFFu));
    return buf;
}

// Not noexcept: lowered() allocates. Neither this nor mod_from_name() is ever on the hook path —
// they run when settings are parsed or the dialog is opened.
bool vk_from_name(std::string_view token, uint32_t& out) {
    const std::string t = lowered(token);
    if (t.empty()) return false;
    if (t.size() == 1) {
        const char c = t[0];
        if (c >= '0' && c <= '9') {
            out = static_cast<uint32_t>(c);
            return true;
        }
        if (c >= 'a' && c <= 'z') {
            out = static_cast<uint32_t>(c - 'a') + vk::kA;
            return true;
        }
        return false;
    }
    if (t[0] == 'f' && all_digits(std::string_view(t).substr(1))) {
        const unsigned long n = std::strtoul(t.c_str() + 1, nullptr, 10);
        if (n >= 1 && n <= 24) {
            out = vk::kF1 + static_cast<uint32_t>(n - 1);
            return true;
        }
        return false;
    }
    if (t == "space") { out = vk::kSpace; return true; }
    if (t == "esc" || t == "escape") { out = vk::kEscape; return true; }
    if (t == "tab") { out = vk::kTab; return true; }
    if (t == "enter" || t == "return") { out = vk::kReturn; return true; }
    if (t == "backspace") { out = vk::kBack; return true; }
    if (t == "capslock" || t == "caps") { out = vk::kCapsLock; return true; }
    if (t == "lctrl" || t == "lcontrol") { out = vk::kLControl; return true; }
    if (t == "rctrl" || t == "rcontrol") { out = vk::kRControl; return true; }
    if (t == "lalt" || t == "lmenu") { out = vk::kLMenu; return true; }
    if (t == "ralt" || t == "rmenu") { out = vk::kRMenu; return true; }
    if (t == "lshift") { out = vk::kLShift; return true; }
    if (t == "rshift") { out = vk::kRShift; return true; }
    if (t == "lwinkey") { out = vk::kLWin; return true; }
    if (t == "rwinkey") { out = vk::kRWin; return true; }
    if (t.size() > 5 && t.compare(0, 5, "vk_0x") == 0) {
        char* end = nullptr;
        const unsigned long n = std::strtoul(t.c_str() + 5, &end, 16);
        if (end != nullptr && *end == '\0' && n < vk::kCount) {
            out = static_cast<uint32_t>(n);
            return true;
        }
    }
    return false;
}

// Returns false if the token is not a modifier name.
bool mod_from_name(std::string_view token, uint32_t& mods, bool& require_left_win) {
    const std::string t = lowered(token);
    if (t == "ctrl" || t == "control") { mods |= kModCtrl; return true; }
    if (t == "alt" || t == "menu") { mods |= kModAlt; return true; }
    if (t == "shift") { mods |= kModShift; return true; }
    if (t == "win" || t == "meta" || t == "super") { mods |= kModWin; return true; }
    if (t == "lwin") { mods |= kModWin; require_left_win = true; return true; }
    return false;
}

}  // namespace

std::string HotkeySpec::to_display_string() const {
    // Modifier order is fixed at Ctrl, Alt, Shift, Win so that the string is canonical and the
    // round-trip in tests/test_hotkey.cpp is meaningful. It also happens to spell the preset
    // plan.md §1.8 names as "Ctrl+Win+Space".
    std::string s;
    if (mods & kModCtrl) s += "Ctrl+";
    if (mods & kModAlt) s += "Alt+";
    if (mods & kModShift) s += "Shift+";
    if (mods & kModWin) s += require_left_win ? "LWin+" : "Win+";
    s += vk_display_name(vk);
    return s;
}

bool HotkeySpec::parse(std::string_view s, HotkeySpec& out) {
    if (s.empty()) return false;

    // Every token but the last is a modifier; the last is the key. That rule is what makes
    // "RCtrl" (a modifier key used *as* the binding) unambiguous — as the only token it is the
    // key, never the Ctrl modifier.
    HotkeySpec spec{};
    spec.mods = kModNone;
    spec.require_left_win = false;

    size_t pos = 0;
    for (;;) {
        const size_t plus = s.find('+', pos);
        const std::string_view token = s.substr(pos, plus == std::string_view::npos ? plus : plus - pos);
        if (token.empty()) return false;  // "Win+", "+Y", "Ctrl++Y"

        if (plus == std::string_view::npos) {
            if (!vk_from_name(token, spec.vk)) return false;
            break;
        }
        if (!mod_from_name(token, spec.mods, spec.require_left_win)) return false;
        pos = plus + 1;
        if (pos >= s.size()) return false;  // trailing '+'
    }

    out = spec;
    return true;
}

std::vector<std::pair<std::string, HotkeySpec>> hotkey_presets() {
    // plan.md §1.8's list, as data. The name is the canonical display string, so the settings
    // dialog, the settings file and this table cannot disagree about spelling.
    std::vector<std::pair<std::string, HotkeySpec>> out;
    out.reserve(3 + 1 + 12 + 3);

    auto add = [&out](uint32_t v, uint32_t mods) {
        HotkeySpec s{};
        s.vk = v;
        s.mods = mods;
        out.emplace_back(s.to_display_string(), s);
    };

    // Win-key combos: the default and its two same-class alternatives. All three need the
    // Start-menu suppression hack.
    add(vk::kY, kModWin);  // the default — nominally Mixed Reality, dead without a headset
    add(vk::kJ, kModWin);
    add(vk::kO, kModWin);
    add(vk::kSpace, kModCtrl | kModWin);

    // F13–F24: no physical key on a standard keyboard sends these, nothing in Windows binds
    // them, and — the point — they need no Start-menu hack at all, because no Win key is
    // involved. This is the escape route when the hack below stops working.
    for (uint32_t v = vk::kF13; v <= vk::kF24; ++v) add(v, kModNone);

    // Modifier keys used as the whole binding. Honest limitation, stated here rather than
    // discovered: binding one of these means the key stops working as a modifier, because the
    // hook swallows it. Right Alt is additionally AltGr on many non-US layouts, where the
    // keyboard driver emits a synthetic left-Ctrl alongside it — the exact-modifier match in
    // chord_matches() means the binding simply never fires there rather than misfiring.
    add(vk::kRControl, kModNone);
    add(vk::kRMenu, kModNone);
    add(vk::kCapsLock, kModNone);

    return out;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  InputFilter — the pure classifier
// ═══════════════════════════════════════════════════════════════════════════════════════════

namespace {

constexpr bool bit_test(const uint64_t* words, uint32_t v) noexcept {
    return v < vk::kCount && (words[v >> 6] & (uint64_t{1} << (v & 63u))) != 0;
}
constexpr void bit_set(uint64_t* words, uint32_t v) noexcept {
    if (v < vk::kCount) words[v >> 6] |= (uint64_t{1} << (v & 63u));
}
constexpr void bit_clear(uint64_t* words, uint32_t v) noexcept {
    if (v < vk::kCount) words[v >> 6] &= ~(uint64_t{1} << (v & 63u));
}

}  // namespace

InputFilter::InputFilter() = default;

void InputFilter::set_hotkey(const HotkeySpec& spec) {
    spec_ = spec;
    // The old binding's key may be physically down right now. Forgetting that we consumed it
    // would leave its key-up unswallowed; remembering it would swallow a key that is no longer
    // ours. Clearing is the honest choice: the worst case is one stray key-up reaching the
    // target at the exact moment the user changed the binding in the settings dialog.
    hotkey_chord_consumed_ = false;
}

const HotkeySpec& InputFilter::hotkey() const noexcept { return spec_; }

void InputFilter::set_injection_mark(uintptr_t extra_info) noexcept { injection_mark_ = extra_info; }

void InputFilter::begin_session_state(uint64_t now_ms) noexcept {
    for (size_t i = 0; i < kVkWords; ++i) down_at_activation_[i] = down_[i];
    session_active_ = true;
    session_started_ms_ = now_ms;
}

void InputFilter::note_session_started(uint64_t now_ms) {
    // Ordinarily the filter has already done this, in on_event(), at the instant it recognised
    // the chord. That matters: the owner learns about the activation through a PostMessage, and
    // the chord's own key-ups reach this hook *before* the UI thread gets round to answering.
    // Snapshotting only here would mean the snapshot was taken after the keys had been released,
    // and the very first key-up of the chord would stop the session it just started — the
    // regression plan.md §1.8 exists to prevent.
    //
    // So this entry point is a confirmation, not the trigger. It still does the full job when a
    // session is started by some path other than the hotkey (the tray menu, a test).
    if (!session_active_) {
        begin_session_state(now_ms);
    } else {
        session_started_ms_ = now_ms;
    }
}

void InputFilter::note_session_ended() {
    session_active_ = false;
    session_started_ms_ = 0;
    for (size_t i = 0; i < kVkWords; ++i) down_at_activation_[i] = 0;
    // hotkey_chord_consumed_ is deliberately NOT cleared: the binding key may still be held, and
    // its key-up must still be swallowed to match the key-down we swallowed.
}

uint32_t InputFilter::current_mods(uint32_t ignore_vk) const noexcept {
    const auto held = [&](uint32_t v) noexcept { return v != ignore_vk && bit_test(down_, v); };

    uint32_t m = kModNone;
    // Both the sided codes (what a low-level hook reports for real keystrokes) and the neutral
    // ones (what SendInput-generated input carries) count.
    if (held(vk::kLControl) || held(vk::kRControl) || held(vk::kControl)) m |= kModCtrl;
    if (held(vk::kLMenu) || held(vk::kRMenu) || held(vk::kMenu)) m |= kModAlt;
    if (held(vk::kLShift) || held(vk::kRShift) || held(vk::kShift)) m |= kModShift;
    if (held(vk::kLWin) || (!spec_.require_left_win && held(vk::kRWin))) m |= kModWin;
    return m;
}

bool InputFilter::is_binding_modifier(uint32_t vk) const noexcept {
    // Sided and neutral codes both count, for the same reason current_mods() accepts both: a real
    // keystroke reports the sided code and SendInput-generated input carries the neutral one.
    if ((spec_.mods & kModCtrl) != 0 &&
        (vk == vk::kLControl || vk == vk::kRControl || vk == vk::kControl)) {
        return true;
    }
    if ((spec_.mods & kModAlt) != 0 && (vk == vk::kLMenu || vk == vk::kRMenu || vk == vk::kMenu)) {
        return true;
    }
    if ((spec_.mods & kModShift) != 0 &&
        (vk == vk::kLShift || vk == vk::kRShift || vk == vk::kShift)) {
        return true;
    }
    if ((spec_.mods & kModWin) != 0 &&
        (vk == vk::kLWin || (!spec_.require_left_win && vk == vk::kRWin))) {
        return true;
    }
    return false;
}

bool InputFilter::chord_matches(const InputEvent& e) const noexcept {
    if (e.vk != spec_.vk) return false;
    // EXACT match, not "at least these". Win+Shift+Y is somebody else's shortcut and must not
    // fire a Win+Y binding; likewise the AltGr case noted in hotkey_presets().
    return current_mods(spec_.vk) == spec_.mods;
}

FilterOutcome InputFilter::on_event(const InputEvent& e) {
    FilterOutcome out;

    // ── Rule 0: our own input is not user input ──────────────────────────────────────────────
    //
    // Two independent signals, both checked, because neither alone is sufficient. LLKHF_INJECTED
    // catches everything SendInput produces including the Start-menu tap below; the extra_info
    // mark additionally identifies *which* process injected it, so a second copy of this program
    // (or a legitimate accessibility tool) does not get silently filtered out as ours.
    //
    // The key-state bitset is deliberately not updated for these. It models what the *user* is
    // physically holding, and a synthetic Ctrl tap does not put the user's finger on Ctrl.
    if (e.injected) return out;
    if (injection_mark_ != 0 && e.extra_info == injection_mark_) return out;

    switch (e.kind) {
        case InputEvent::Kind::KeyDown: return on_key_event(e, true);
        case InputEvent::Kind::KeyUp: return on_key_event(e, false);
        case InputEvent::Kind::MouseDown:
        case InputEvent::Kind::MouseUp:
        case InputEvent::Kind::MouseWheel:
        case InputEvent::Kind::MouseMove: return on_mouse_event(e);
    }
    return out;
}

FilterOutcome InputFilter::on_key_event(const InputEvent& e, bool down) {
    FilterOutcome out;

    if (!down) {
        bit_clear(down_, e.vk);

        // Leaving the down-at-activation set is unconditional, and must happen BEFORE any of the
        // early returns below. The binding key's own release takes one of those returns, so
        // clearing further down left the binding key in the set permanently — contradicting the
        // rule stated there ("once a key is released it must leave the set") and, because the set
        // is what suppresses auto-repeat, quietly making the binding key un-repeatable for the
        // rest of the process's life.
        bit_clear(down_at_activation_, e.vk);

        // The binding key's release. We swallowed its press, so we must swallow its release too:
        // a target that saw no key-down but does see a key-up can get its own modifier tracking
        // stuck (this is how "my Shift key is stuck on" bugs are born).
        if (e.vk == spec_.vk && hotkey_chord_consumed_) {
            hotkey_chord_consumed_ = false;
            out.swallow = true;
            return out;
        }

        // plan.md §1.8's rule, stated there as "ignore all key-up events for keys already down
        // at activation". This implementation is deliberately stronger: **no key-up ever stops
        // the session**, whether or not the key was held at activation. A key-up cannot express
        // an intention the matching key-down did not already express, and the key-down path
        // below is where stop-on-input is decided.
        //
        // The down-at-activation set is still tracked, for two reasons that do bite: it is what
        // suppresses auto-repeat of the held chord keys (see below), and once a key is released
        // it must leave the set, so that pressing it *again* counts as real input. The clearing
        // itself happens at the top of this branch, so that the early returns cannot skip it.
        return out;
    }

    const bool was_down = bit_test(down_, e.vk);
    bit_set(down_, e.vk);

    // ── The chord ────────────────────────────────────────────────────────────────────────────
    if (chord_matches(e)) {
        out.swallow = true;  // including auto-repeats: the target must never see this key
        if (!hotkey_chord_consumed_) {
            hotkey_chord_consumed_ = true;  // one action per physical press, not per repeat
            if (session_active_) {
                out.toggle_finalize = true;
            } else {
                out.activate = true;
                begin_session_state(e.time_ms);
            }
            // False for every F13–F24 and modifier-key binding, which is the whole point of
            // offering them: those paths never call SendInput at all.
            out.suppress_start_menu = spec_.needs_start_menu_suppression();
        }
        return out;
    }

    // Still the binding key, but the modifiers no longer match — the user released Win while
    // still holding Y. It stays ours until it goes up, or the target receives a lone 'Y'.
    if (e.vk == spec_.vk && hotkey_chord_consumed_) {
        out.swallow = true;
        return out;
    }

    if (!session_active_) return out;

    // ── Esc cancels ──────────────────────────────────────────────────────────────────────────
    //
    // Not swallowed, and that is a judgement call rather than something plan.md settles. Esc
    // means "get me out of this" in the target application too, and eating it would mean a
    // dialog that refused to close while dictation happened to be running. The cost is that the
    // target also sees the Esc; the alternative cost is worse and less explicable.
    if (e.vk == vk::kEscape) {
        out.cancel = true;
        return out;
    }

    // ── Keys held since activation ───────────────────────────────────────────────────────────
    //
    // A key held down auto-repeats, and a low-level hook sees every repeat as a fresh key-down —
    // including for modifiers on most keyboards. Without this, holding Win+Y for half a second
    // would activate the session and then immediately stop it with the Win key's own repeat.
    // plan.md §1.8 states the rule for key-ups; this is the same rule where it actually bites.
    if (was_down && bit_test(down_at_activation_, e.vk)) return out;

    // ── The first half of the binding chord ──────────────────────────────────────────────────
    //
    // Win+Y reaches the hook as LWIN down, *then* Y down — and the user pressing the binding a
    // second time is asking to finish the session, not typing into it. Reading that LWIN as
    // ordinary input ended the session on the modifier, and the Y a few milliseconds later then
    // found no session running and started a new one. Observed as: press the hotkey to finish,
    // and a second block of text appears instead.
    //
    // Only the modifiers the binding actually uses are excused, and only while a session is
    // running. If the user goes on to press some other key (Win+E), that key still stops the
    // session on the line below, which is the outcome plan.md §1.8 asks for.
    if (is_binding_modifier(e.vk)) return out;

    // ── Anything else is the user doing something else ───────────────────────────────────────
    out.stop_on_input = true;
    out.stop_reason = StopInputReason::Keystroke;
    return out;
}

FilterOutcome InputFilter::on_mouse_event(const InputEvent& e) {
    FilterOutcome out;

    switch (e.kind) {
        case InputEvent::Kind::MouseWheel:
            // plan.md §1.5, source B: scrolling moves the caret with no click, so the cached
            // click is no longer where the insertion point is. Invalidate rather than downgrade
            // — a stale anchor that looks confident is worse than no anchor.
            out.click_invalidated = true;
            // Not stop-on-input: plan.md §1.8 says "any real click or keystroke", and a wheel is
            // neither. Scrolling to re-read what you dictated is a normal thing to do mid-session.
            return out;

        case InputEvent::Kind::MouseDown:
            // Left button only for the anchor cache: a right-click opens a context menu and a
            // middle-click pastes on X11, so neither indicates where the insertion point is.
            if (e.left_button) out.click_captured = true;
            // Every button stops the session, though — plan.md §1.8's "any real click".
            if (session_active_) {
                out.stop_on_input = true;
                out.stop_reason = StopInputReason::MouseClick;
            }
            return out;

        case InputEvent::Kind::MouseUp:
            // The matching button-down already stopped the session and already updated the
            // cache. Acting on the up as well would double-report.
            return out;

        case InputEvent::Kind::MouseMove:
        case InputEvent::Kind::KeyDown:
        case InputEvent::Kind::KeyUp:
        default:
            // Moving the mouse is not an intention. It must not stop a session, or dictating
            // while resting a hand on the mouse would be impossible.
            return out;
    }
}

bool InputFilter::session_active() const noexcept { return session_active_; }

bool InputFilter::is_key_down(uint32_t v) const noexcept { return bit_test(down_, v); }

size_t InputFilter::keys_held_at_activation() const noexcept {
    size_t n = 0;
    for (size_t i = 0; i < kVkWords; ++i) n += static_cast<size_t>(std::popcount(down_at_activation_[i]));
    return n;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  The Start-menu suppression hack — quarantined, exactly as plan.md §1.8 requires
// ═══════════════════════════════════════════════════════════════════════════════════════════

bool suppress_start_menu(uintptr_t injection_mark) {
    // ── READ THIS BEFORE CHANGING ANYTHING HERE ──────────────────────────────────────────────
    //
    // This is a hack against undocumented shell behaviour and a plausible casualty of any
    // Windows update. Nothing about it is contractual.
    //
    // What it works around: we swallow the Y of Win+Y, so the shell never sees the second key of
    // the chord. What it does see is a Win press followed by a Win release with nothing in
    // between, which is the gesture that opens the Start menu. Injecting any other key event
    // into that window makes the shell treat the Win press as part of a combination and drop the
    // Start-menu action.
    //
    // VK_CONTROL specifically, and not just any key: a lone Ctrl tap does nothing in essentially
    // every application. Shift dismisses IME candidate windows and toggles sticky-keys prompts;
    // Alt moves focus to the menu bar; a letter would be typed into the document.
    //
    // When this breaks, the symptom is the Start menu appearing on every activation. **The
    // answer is to switch the binding to F13–F24, not to escalate the hack** — those bindings
    // involve no Win key and reach this function never. plan.md §1.8 keeps them one click away
    // in settings for exactly this reason.
    //
    // Called from the hook thread's message loop, never from the hook callback: SendInput is a
    // kernel transition that re-enters the input pipeline, and a hook callback that calls it is
    // a hook callback that can be slow enough for Windows to remove.

    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_CONTROL;
    in[0].ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_CONTROL, MAPVK_VK_TO_VSC));
    in[0].ki.dwExtraInfo = static_cast<ULONG_PTR>(injection_mark);

    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;

    const UINT sent = SendInput(2, in, sizeof(INPUT));
    if (sent != 2) {
        // The usual cause is UIPI: a higher-integrity window has the foreground, and SendInput
        // from a medium-integrity process is blocked. Same boundary as plan.md §1.7's elevated
        // targets, and equally not something to work around.
        KD_LOG_WARN("start-menu suppression: SendInput sent %u of 2 (err %lu)", sent, GetLastError());
        return false;
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  HotkeyManager — hooks, dedicated thread, watchdog
// ═══════════════════════════════════════════════════════════════════════════════════════════

namespace {

// Private messages, posted by the hook callback to its OWN thread queue. Everything here is work
// that must not happen inside the callback.
constexpr UINT kHookMsgSetSpec = WM_USER + 1;        // lParam: new HotkeySpec*, owned by the loop
constexpr UINT kHookMsgSessionStarted = WM_USER + 2;
constexpr UINT kHookMsgSessionEnded = WM_USER + 3;
constexpr UINT kHookMsgSuppressStart = WM_USER + 4;
constexpr UINT kHookMsgSetMark = WM_USER + 5;        // wParam: dwExtraInfo mark

// How often the watchdog checks whether we are still in the hook chain, and how far behind the
// system's last-input time we tolerate being before concluding we are not.
constexpr UINT kWatchdogPeriodMs = 2000;
constexpr int32_t kHookSilenceGraceMs = 5000;

// RAII for a hook handle. Not "UnhookWindowsHookEx at the end of the function": the thread body
// has several early returns and the reinstall path replaces the handle in place.
class ScopedHook {
public:
    ScopedHook() = default;
    ~ScopedHook() { reset(); }
    ScopedHook(const ScopedHook&) = delete;
    ScopedHook& operator=(const ScopedHook&) = delete;

    bool install(int id, HOOKPROC proc) {
        reset();
        // hMod may be the current module for a low-level hook — unlike the other hook types,
        // WH_KEYBOARD_LL and WH_MOUSE_LL are not injected into other processes, so no DLL is
        // required and the callback runs in this process on the installing thread.
        h_ = SetWindowsHookExW(id, proc, GetModuleHandleW(nullptr), 0);
        return h_ != nullptr;
    }
    void reset() noexcept {
        if (h_ != nullptr) {
            UnhookWindowsHookEx(h_);
            h_ = nullptr;
        }
    }
    [[nodiscard]] HHOOK get() const noexcept { return h_; }
    explicit operator bool() const noexcept { return h_ != nullptr; }

private:
    HHOOK h_ = nullptr;
};

struct HookRuntime {
    // ── Owner-thread state ───────────────────────────────────────────────────────────────────
    std::thread thread;
    std::atomic<DWORD> thread_id{0};
    std::atomic<bool> running{false};
    uintptr_t owner_window = 0;
    IHotkeyListener* listener = nullptr;
    uintptr_t injection_mark = 0;

    // ── Hook-thread-only state ───────────────────────────────────────────────────────────────
    //
    // No lock guards these, and none may be added: a lock the UI thread can hold is a lock the
    // hook callback can wait on, which is the failure mode this whole file is arranged to avoid.
    // Configuration changes reach them by PostThreadMessage instead.
    ScopedHook kbd;
    ScopedHook mouse;
    InputFilter filter;
    DWORD last_event_tick = 0;

    // ── Statistics, read from any thread ─────────────────────────────────────────────────────
    std::atomic<uint64_t> key_events{0};
    std::atomic<uint64_t> mouse_events{0};
    std::atomic<uint64_t> swallowed{0};
    std::atomic<uint64_t> reinstalls{0};
    std::atomic<uint64_t> max_callback_us{0};

    // ── The last left click, as a seqlock ────────────────────────────────────────────────────
    //
    // The click's x, y and time do not fit in a PostMessage's two parameters, so they live in a
    // single slot the hook writes and the UI thread reads. A seqlock rather than a mutex because
    // the writer is the hook callback and must never wait; an even counter means "stable", odd
    // means "being written". The fields are atomics with relaxed ordering so this is a race in
    // the informal sense only, not in the undefined-behaviour sense.
    std::atomic<uint32_t> click_seq{0};
    std::atomic<int32_t> click_x{0};
    std::atomic<int32_t> click_y{0};
    std::atomic<uint64_t> click_time{0};
};

std::atomic<HookRuntime*> g_runtime{nullptr};

// QPC frequency, primed on the hook thread before the hooks go in so that the magic-static guard
// is never taken for the first time inside a callback.
uint64_t qpc_frequency() {
    static const uint64_t f = [] {
        LARGE_INTEGER v{};
        QueryPerformanceFrequency(&v);
        return v.QuadPart > 0 ? static_cast<uint64_t>(v.QuadPart) : uint64_t{1};
    }();
    return f;
}

uint64_t qpc_now() noexcept {
    LARGE_INTEGER v{};
    QueryPerformanceCounter(&v);
    return static_cast<uint64_t>(v.QuadPart);
}

void record_callback_time(HookRuntime* rt, uint64_t ticks) noexcept {
    const uint64_t us = (ticks * 1000000ull) / qpc_frequency();
    uint64_t prev = rt->max_callback_us.load(std::memory_order_relaxed);
    while (us > prev && !rt->max_callback_us.compare_exchange_weak(prev, us, std::memory_order_relaxed)) {
    }
}

void post_to_owner(HookRuntime* rt, HotkeyPost code, intptr_t detail) noexcept {
    if (rt->owner_window == 0) return;
    // PostMessage never waits for the receiver — that is the entire reason it, and not
    // SendMessage, is what a hook callback is allowed to call. A full queue makes it fail; we
    // drop the notification rather than retry, because retrying is waiting.
    PostMessageW(reinterpret_cast<HWND>(rt->owner_window), kMsgHotkey, static_cast<WPARAM>(code),
                 static_cast<LPARAM>(detail));
}

void write_click(HookRuntime* rt, int32_t x, int32_t y, uint64_t t) noexcept {
    const uint32_t s = rt->click_seq.load(std::memory_order_relaxed);
    rt->click_seq.store(s + 1, std::memory_order_release);  // odd: a write is in progress
    rt->click_x.store(x, std::memory_order_relaxed);
    rt->click_y.store(y, std::memory_order_relaxed);
    rt->click_time.store(t, std::memory_order_relaxed);
    rt->click_seq.store(s + 2, std::memory_order_release);
}

bool read_click(HookRuntime* rt, int32_t& x, int32_t& y, uint64_t& t) noexcept {
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t s0 = rt->click_seq.load(std::memory_order_acquire);
        if (s0 == 0 || (s0 & 1u) != 0) continue;  // never written, or mid-write
        x = rt->click_x.load(std::memory_order_relaxed);
        y = rt->click_y.load(std::memory_order_relaxed);
        t = rt->click_time.load(std::memory_order_relaxed);
        if (rt->click_seq.load(std::memory_order_acquire) == s0) return true;
    }
    return false;
}

// Turn a FilterOutcome into posts. Called from the hook callback, so: no allocation, no blocking
// call, and nothing that can re-enter the input pipeline.
void deliver(HookRuntime* rt, const FilterOutcome& o, const InputEvent& e) noexcept {
    if (o.swallow) rt->swallowed.fetch_add(1, std::memory_order_relaxed);

    if (o.suppress_start_menu) {
        // To our own message loop, not SendInput here. See suppress_start_menu().
        PostThreadMessageW(rt->thread_id.load(std::memory_order_relaxed), kHookMsgSuppressStart, 0, 0);
    }
    if (o.activate) post_to_owner(rt, HotkeyPost::Activate, 0);
    if (o.toggle_finalize) post_to_owner(rt, HotkeyPost::Finalize, 0);
    if (o.cancel) post_to_owner(rt, HotkeyPost::Cancel, 0);
    if (o.stop_on_input) post_to_owner(rt, HotkeyPost::StopInput, static_cast<intptr_t>(o.stop_reason));
    if (o.click_invalidated) post_to_owner(rt, HotkeyPost::Wheel, 0);
    if (o.click_captured) {
        write_click(rt, e.x, e.y, e.time_ms);
        post_to_owner(rt, HotkeyPost::LeftClick, 0);
    }
}

LRESULT CALLBACK keyboard_proc(int code, WPARAM wparam, LPARAM lparam) {
    HookRuntime* rt = g_runtime.load(std::memory_order_acquire);
    if (code != HC_ACTION || rt == nullptr) {
        return CallNextHookEx(nullptr, code, wparam, lparam);
    }

    const uint64_t t0 = qpc_now();

    InputEvent e;
    switch (static_cast<UINT>(wparam)) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: e.kind = InputEvent::Kind::KeyDown; break;
        case WM_KEYUP:
        case WM_SYSKEYUP: e.kind = InputEvent::Kind::KeyUp; break;
        default: return CallNextHookEx(rt->kbd.get(), code, wparam, lparam);
    }

    const KBDLLHOOKSTRUCT* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
    e.vk = k->vkCode;
    e.injected = (k->flags & LLKHF_INJECTED) != 0;
    e.extra_info = static_cast<uintptr_t>(k->dwExtraInfo);
    e.time_ms = k->time;

    // k->time is already the tick count of the event, so the watchdog's clock costs no call.
    rt->last_event_tick = k->time;
    rt->key_events.fetch_add(1, std::memory_order_relaxed);

    const FilterOutcome o = rt->filter.on_event(e);
    deliver(rt, o, e);

    record_callback_time(rt, qpc_now() - t0);

    // Returning 1 without CallNextHookEx is what swallows the combo (plan.md §1.8). It is also
    // the only way to do it: there is no "modify the event" for a low-level keyboard hook.
    if (o.swallow) return 1;
    return CallNextHookEx(rt->kbd.get(), code, wparam, lparam);
}

LRESULT CALLBACK mouse_proc(int code, WPARAM wparam, LPARAM lparam) {
    HookRuntime* rt = g_runtime.load(std::memory_order_acquire);
    if (code != HC_ACTION || rt == nullptr) {
        return CallNextHookEx(nullptr, code, wparam, lparam);
    }

    const uint64_t t0 = qpc_now();

    InputEvent e;
    switch (static_cast<UINT>(wparam)) {
        case WM_LBUTTONDOWN:
            e.kind = InputEvent::Kind::MouseDown;
            e.left_button = true;
            break;
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN: e.kind = InputEvent::Kind::MouseDown; break;
        case WM_LBUTTONUP:
            e.kind = InputEvent::Kind::MouseUp;
            e.left_button = true;
            break;
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
        case WM_XBUTTONUP: e.kind = InputEvent::Kind::MouseUp; break;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: e.kind = InputEvent::Kind::MouseWheel; break;
        case WM_MOUSEMOVE: e.kind = InputEvent::Kind::MouseMove; break;
        default: return CallNextHookEx(rt->mouse.get(), code, wparam, lparam);
    }

    const MSLLHOOKSTRUCT* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam);
    e.injected = (m->flags & LLMHF_INJECTED) != 0;
    e.extra_info = static_cast<uintptr_t>(m->dwExtraInfo);
    e.x = m->pt.x;  // physical pixels; the process is per-monitor DPI aware (plan.md §1.3)
    e.y = m->pt.y;
    e.time_ms = m->time;

    rt->last_event_tick = m->time;
    rt->mouse_events.fetch_add(1, std::memory_order_relaxed);

    const FilterOutcome o = rt->filter.on_event(e);
    deliver(rt, o, e);

    record_callback_time(rt, qpc_now() - t0);

    // The mouse hook never swallows. Stop-on-input observes a click; it does not eat it. Eating
    // it would move the caret in the target without the target knowing, which is the one thing
    // that would make the anchor worse rather than better.
    return CallNextHookEx(rt->mouse.get(), code, wparam, lparam);
}

bool install_hooks(HookRuntime* rt) {
    return rt->kbd.install(WH_KEYBOARD_LL, keyboard_proc) &&
           rt->mouse.install(WH_MOUSE_LL, mouse_proc);
}

// Windows removes a hook that exceeded LowLevelHooksTimeout and tells nobody: there is no
// callback, no error, and no API that answers "am I still installed?". UnhookWindowsHookEx on a
// removed hook still succeeds.
//
// So this is an inference, not a detection, and it is written as one. GetLastInputInfo reports
// when the system last saw input; our hook records when *we* last saw some. If the system has
// seen input well after we did, we are no longer in the chain — reinstall.
//
// Honest false positives, both harmless because reinstalling is idempotent and costs a syscall:
// a low-integrity process cannot observe input directed at an elevated window (UIPI), and input
// on the secure desktop is invisible to us by design. Both look like silence from here. That is
// why a reinstall is silent and only a *failed* reinstall reaches the listener.
void watchdog(HookRuntime* rt) {
    LASTINPUTINFO lii{};
    lii.cbSize = sizeof(lii);
    if (!GetLastInputInfo(&lii)) return;

    // Signed difference, because tick counts wrap every 49.7 days and an unsigned compare would
    // declare the hook dead for the whole second half of that cycle.
    const int32_t behind = static_cast<int32_t>(lii.dwTime - rt->last_event_tick);
    if (behind <= kHookSilenceGraceMs) return;

    rt->reinstalls.fetch_add(1, std::memory_order_relaxed);
    const bool ok = install_hooks(rt);
    rt->last_event_tick = GetTickCount();

    if (!ok) {
        KD_LOG_ERROR("hook reinstall failed (err %lu); the hotkey is now dead", GetLastError());
        post_to_owner(rt, HotkeyPost::HookLost, 0);
    } else {
        KD_LOG_WARN("low-level hooks were reinstalled after %d ms of unexplained silence", behind);
    }
}

void hook_thread_main(HookRuntime* rt, std::promise<Status> ready) {
    // A thread has no message queue until it asks for one, and PostThreadMessage to a thread
    // without a queue fails. Forcing the queue into existence *before* start() returns is what
    // makes set_hotkey()/stop() safe to call the instant start() succeeds.
    MSG msg{};
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    rt->thread_id.store(GetCurrentThreadId(), std::memory_order_release);

    qpc_frequency();  // prime the magic static off the callback path
    rt->last_event_tick = GetTickCount();

    if (!install_hooks(rt)) {
        const DWORD err = GetLastError();
        rt->kbd.reset();
        rt->mouse.reset();
        ready.set_value(Status{StatusCode::OsError,
                               "SetWindowsHookEx failed; the global hotkey will not work",
                               static_cast<uint32_t>(err)});
        return;
    }
    ready.set_value(Status::ok());

    const UINT_PTR timer = SetTimer(nullptr, 0, kWatchdogPeriodMs, nullptr);

    // GetMessage returns 0 on WM_QUIT and -1 on error; `> 0` handles both without the classic
    // `while (GetMessage(...))` bug of looping forever on -1.
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        switch (msg.message) {
            case kHookMsgSetSpec: {
                HotkeySpec* spec = reinterpret_cast<HotkeySpec*>(msg.lParam);
                if (spec != nullptr) {
                    rt->filter.set_hotkey(*spec);
                    delete spec;  // allocated by the caller's thread, freed here — see set_hotkey()
                }
                break;
            }
            case kHookMsgSetMark:
                rt->injection_mark = static_cast<uintptr_t>(msg.wParam);
                rt->filter.set_injection_mark(rt->injection_mark);
                break;
            case kHookMsgSessionStarted: rt->filter.note_session_started(GetTickCount64()); break;
            case kHookMsgSessionEnded: rt->filter.note_session_ended(); break;
            case kHookMsgSuppressStart: suppress_start_menu(rt->injection_mark); break;
            case WM_TIMER: watchdog(rt); break;
            default: break;
        }
    }

    if (timer != 0) KillTimer(nullptr, timer);
    // Explicit, on the installing thread, before the thread dies. ScopedHook's destructor is the
    // safety net for the early-return path above, not the normal route.
    rt->kbd.reset();
    rt->mouse.reset();
}

}  // namespace

struct HotkeyManager::Impl {
    HookRuntime rt;
};

HotkeyManager::HotkeyManager() : impl_(new Impl()) {}

HotkeyManager::~HotkeyManager() {
    stop();
    delete impl_;
}

Status HotkeyManager::start(const HotkeySpec& spec, uintptr_t owner_window,
                            IHotkeyListener* listener) {
    HookRuntime* rt = &impl_->rt;

    if (rt->running.load(std::memory_order_acquire)) {
        return Status{StatusCode::InvalidArgument, "HotkeyManager::start called while running"};
    }
    if (owner_window == 0) {
        return Status{StatusCode::InvalidArgument,
                      "owner_window must be the message-only window that receives kMsgHotkey"};
    }

    rt->owner_window = owner_window;
    rt->listener = listener;
    rt->filter.set_hotkey(spec);
    rt->filter.set_injection_mark(rt->injection_mark);

    // The hook procedures are plain C callbacks with no user parameter, so they find their state
    // through this single global. One HotkeyManager per process, enforced rather than assumed.
    HookRuntime* expected = nullptr;
    if (!g_runtime.compare_exchange_strong(expected, rt, std::memory_order_acq_rel)) {
        return Status{StatusCode::Unavailable, "another HotkeyManager already owns the hooks"};
    }

    std::promise<Status> ready;
    std::future<Status> ready_future = ready.get_future();
    rt->thread = std::thread(hook_thread_main, rt, std::move(ready));

    // Blocking here is fine: this is the UI thread at startup, not a hook callback, and the wait
    // is bounded by SetWindowsHookEx returning.
    Status st = ready_future.get();
    if (!st.is_ok()) {
        rt->thread.join();
        rt->thread_id.store(0, std::memory_order_release);
        g_runtime.store(nullptr, std::memory_order_release);
        return st;
    }

    rt->running.store(true, std::memory_order_release);
    KD_LOG_INFO("hotkey installed: %s", spec.to_display_string().c_str());
    return Status::ok();
}

void HotkeyManager::stop() {
    HookRuntime* rt = &impl_->rt;
    if (!rt->thread.joinable()) return;

    const DWORD tid = rt->thread_id.load(std::memory_order_acquire);
    if (tid != 0) PostThreadMessageW(tid, WM_QUIT, 0, 0);
    rt->thread.join();

    rt->running.store(false, std::memory_order_release);
    rt->thread_id.store(0, std::memory_order_release);
    rt->listener = nullptr;
    rt->owner_window = 0;

    HookRuntime* expected = rt;
    g_runtime.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
}

void HotkeyManager::set_hotkey(const HotkeySpec& spec) {
    HookRuntime* rt = &impl_->rt;
    if (!rt->running.load(std::memory_order_acquire)) {
        rt->filter.set_hotkey(spec);  // not started yet: the hook thread cannot be racing us
        return;
    }
    // Heap-allocated on the caller's thread and freed on the hook thread's message loop. The
    // allocation never happens on the hook path, and the spec does not fit in a message parameter.
    HotkeySpec* copy = new HotkeySpec(spec);
    if (!PostThreadMessageW(rt->thread_id.load(std::memory_order_acquire), kHookMsgSetSpec, 0,
                            reinterpret_cast<LPARAM>(copy))) {
        delete copy;
        KD_LOG_ERROR("set_hotkey: PostThreadMessage failed (err %lu)", GetLastError());
    }
}

void HotkeyManager::set_injection_mark(uintptr_t extra_info) {
    HookRuntime* rt = &impl_->rt;
    if (!rt->running.load(std::memory_order_acquire)) {
        rt->injection_mark = extra_info;
        rt->filter.set_injection_mark(extra_info);
        return;
    }
    // While running, `injection_mark` is hook-thread-only state like the filter itself: it is
    // assigned by the message handler, not here. Writing it from this thread would be a plain
    // data race against the suppression path that reads it.
    PostThreadMessageW(rt->thread_id.load(std::memory_order_acquire), kHookMsgSetMark,
                       static_cast<WPARAM>(extra_info), 0);
}

void HotkeyManager::note_session_started() {
    HookRuntime* rt = &impl_->rt;
    if (!rt->running.load(std::memory_order_acquire)) {
        rt->filter.note_session_started(GetTickCount64());
        return;
    }
    PostThreadMessageW(rt->thread_id.load(std::memory_order_acquire), kHookMsgSessionStarted, 0, 0);
}

void HotkeyManager::note_session_ended() {
    HookRuntime* rt = &impl_->rt;
    if (!rt->running.load(std::memory_order_acquire)) {
        rt->filter.note_session_ended();
        return;
    }
    PostThreadMessageW(rt->thread_id.load(std::memory_order_acquire), kHookMsgSessionEnded, 0, 0);
}

bool HotkeyManager::is_running() const noexcept {
    return impl_->rt.running.load(std::memory_order_acquire);
}

HotkeyManager::Stats HotkeyManager::stats() const noexcept {
    const HookRuntime* rt = &impl_->rt;
    Stats s;
    s.key_events = rt->key_events.load(std::memory_order_relaxed);
    s.mouse_events = rt->mouse_events.load(std::memory_order_relaxed);
    s.swallowed = rt->swallowed.load(std::memory_order_relaxed);
    s.hook_reinstalls = rt->reinstalls.load(std::memory_order_relaxed);
    s.max_callback_us = rt->max_callback_us.load(std::memory_order_relaxed);
    return s;
}

bool dispatch_hotkey_message(uint32_t msg, uintptr_t wparam, intptr_t lparam) {
    if (msg != kMsgHotkey) return false;

    HookRuntime* rt = g_runtime.load(std::memory_order_acquire);
    if (rt == nullptr || rt->listener == nullptr) return true;  // ours, but nobody is listening

    switch (static_cast<HotkeyPost>(wparam)) {
        case HotkeyPost::Activate: rt->listener->on_activate(); break;
        case HotkeyPost::Finalize: rt->listener->on_finalize(); break;
        case HotkeyPost::Cancel: rt->listener->on_cancel(); break;
        case HotkeyPost::StopInput:
            rt->listener->on_stop_input(static_cast<StopInputReason>(lparam));
            break;
        case HotkeyPost::Wheel: rt->listener->on_wheel(); break;
        case HotkeyPost::HookLost: rt->listener->on_hook_lost(); break;
        case HotkeyPost::LeftClick: {
            int32_t x = 0, y = 0;
            uint64_t t = 0;
            if (!read_click(rt, x, y, t)) break;
            // WindowFromPoint is resolved HERE, on the UI thread, not in the hook callback. It
            // walks the window tree and the callback's budget is measured in microseconds; the
            // window under a point one message-loop iteration later is the same window in every
            // realistic case, and anchor.h re-checks that it is still the foreground window
            // anyway (plan.md §1.5, source B validity rules).
            const POINT pt{x, y};
            const HWND w = WindowFromPoint(pt);
            rt->listener->on_left_click(x, y, reinterpret_cast<uintptr_t>(w), t);
            break;
        }
    }
    return true;
}

}  // namespace kd
