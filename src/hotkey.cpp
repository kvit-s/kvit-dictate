// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
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
static_assert(vk::kPageUp == VK_PRIOR && vk::kPageDown == VK_NEXT, "VK drift");
static_assert(vk::kEnd == VK_END && vk::kHome == VK_HOME, "VK drift");
static_assert(vk::kPrintScreen == VK_SNAPSHOT && vk::kInsert == VK_INSERT, "VK drift");
static_assert(vk::kDelete == VK_DELETE, "VK drift");
static_assert(vk::kC == 'C' && vk::kV == 'V' && vk::kX == 'X', "VK drift");
static_assert(vk::kNumpad0 == VK_NUMPAD0 && vk::kDivide == VK_DIVIDE, "VK drift");
static_assert(vk::kNumLock == VK_NUMLOCK && vk::kScrollLock == VK_SCROLL, "VK drift");
static_assert(vk::kVolumeMute == VK_VOLUME_MUTE && vk::kVolumeUp == VK_VOLUME_UP, "VK drift");
static_assert(vk::kVolumeDown == VK_VOLUME_DOWN, "VK drift");
static_assert(vk::kMediaNextTrack == VK_MEDIA_NEXT_TRACK, "VK drift");
static_assert(vk::kMediaPrevTrack == VK_MEDIA_PREV_TRACK, "VK drift");
static_assert(vk::kMediaStop == VK_MEDIA_STOP && vk::kMediaPlayPause == VK_MEDIA_PLAY_PAUSE,
              "VK drift");
static_assert(vk::kOem1 == VK_OEM_1 && vk::kOem3 == VK_OEM_3, "VK drift");
static_assert(vk::kOem4 == VK_OEM_4 && vk::kOem8 == VK_OEM_8 && vk::kOem102 == VK_OEM_102,
              "VK drift");
static_assert(vk::kF24 - vk::kF1 == 23, "VK_F1..VK_F24 must stay contiguous; the F-key naming relies on it");
static_assert(kMsgHotkey == WM_APP + 0x11u, "kMsgHotkey must be inside the WM_APP range");

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  StopInputReason
// ═══════════════════════════════════════════════════════════════════════════════════════════

uint64_t pack_edit_post(const EditPost& p) noexcept {
    uint64_t v = static_cast<uint64_t>(p.op) & 0xFFull;
    if (p.shift) v |= 1ull << 8;
    if (p.caps_lock) v |= 1ull << 9;
    if (p.needs_char) v |= 1ull << 10;
    v |= (static_cast<uint64_t>(p.vk) & 0xFFull) << 16;
    v |= (static_cast<uint64_t>(p.scan) & 0xFFFFull) << 32;
    v |= (static_cast<uint64_t>(p.mods) & 0xFull) << 48;
    return v;
}

EditPost unpack_edit_post(uint64_t packed) noexcept {
    EditPost p;
    p.op = static_cast<EditOp>(packed & 0xFFull);
    p.shift = (packed & (1ull << 8)) != 0;
    p.caps_lock = (packed & (1ull << 9)) != 0;
    p.needs_char = (packed & (1ull << 10)) != 0;
    p.vk = static_cast<uint32_t>((packed >> 16) & 0xFFull);
    p.scan = static_cast<uint32_t>((packed >> 32) & 0xFFFFull);
    p.mods = static_cast<uint32_t>((packed >> 48) & 0xFull);
    return p;
}

uint64_t pack_pill_post(const PillPost& p) noexcept {
    const auto clamp16 = [](int32_t v) noexcept -> uint64_t {
        if (v < -32768) v = -32768;
        if (v > 32767) v = 32767;
        return static_cast<uint64_t>(static_cast<uint16_t>(static_cast<int16_t>(v)));
    };
    uint64_t v = static_cast<uint64_t>(p.kind) & 0xFFull;
    v |= clamp16(p.x) << 16;
    v |= clamp16(p.y) << 32;
    v |= clamp16(p.wheel) << 48;
    return v;
}

PillPost unpack_pill_post(uint64_t packed) noexcept {
    const auto s16 = [](uint64_t v) noexcept -> int32_t {
        return static_cast<int32_t>(static_cast<int16_t>(static_cast<uint16_t>(v & 0xFFFFull)));
    };
    PillPost p;
    p.kind = static_cast<FilterOutcome::PillPointerKind>(packed & 0xFFull);
    p.x = s16(packed >> 16);
    p.y = s16(packed >> 32);
    p.wheel = s16(packed >> 48);
    return p;
}

const char* to_string(ForeignKeyAction a) noexcept {
    switch (a) {
        case ForeignKeyAction::Ignore: return "ignore";
        case ForeignKeyAction::EndSession: return "end-session";
    }
    return "ignore";
}

bool parse_foreign_key_action(std::string_view s, ForeignKeyAction& out) noexcept {
    if (s == "ignore") {
        out = ForeignKeyAction::Ignore;
        return true;
    }
    if (s == "end-session") {
        out = ForeignKeyAction::EndSession;
        return true;
    }
    return false;
}

const char* to_string(StopInputReason r) noexcept {
    switch (r) {
        case StopInputReason::Keystroke: return "keystroke";
        case StopInputReason::MouseClick: return "mouse_click";
        case StopInputReason::HotkeyToggle: return "hotkey_toggle";
        case StopInputReason::Escape: return "escape";
        case StopInputReason::Commit: return "commit";
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

InputFilter::InputFilter() {
    // See the member's comment in hotkey.h. An unconfigured filter classifies exactly as it did
    // before docs/editing.md existed; HotkeyManager::set_editing() is what turns the compose
    // buffer on, and main.cpp calls it before start() so that the very first chord is classified
    // against the user's configuration.
    editing_.enabled = false;
}

void InputFilter::set_hotkey(const HotkeySpec& spec) {
    spec_ = spec;
    // The old binding's key may be physically down right now. Forgetting that we consumed it
    // would leave its key-up unswallowed; remembering it would swallow a key that is no longer
    // ours. Clearing is the honest choice: the worst case is one stray key-up reaching the
    // target at the exact moment the user changed the binding in the settings dialog.
    hotkey_chord_consumed_ = false;
    // Same argument, and one more: the release we were waiting for belongs to a binding that no
    // longer exists. A held key that is no longer part of the chord must not finalise on its way
    // up — the user changed a setting, they did not finish dictating.
    ptt_held_ = false;
}

const HotkeySpec& InputFilter::hotkey() const noexcept { return spec_; }

void InputFilter::set_editing(const EditingConfig& cfg) noexcept { editing_ = cfg; }

const EditingConfig& InputFilter::editing() const noexcept { return editing_; }

void InputFilter::set_injection_mark(uintptr_t extra_info) noexcept { injection_mark_ = extra_info; }

void InputFilter::begin_session_state(uint64_t now_ms) noexcept {
    for (size_t i = 0; i < kVkWords; ++i) down_at_activation_[i] = down_[i];
    session_active_ = true;
    session_started_ms_ = now_ms;
    // A session starts dictating, never editing (docs/editing.md §1), and the fail-open latch is
    // per-session by definition — §10.1 says the filter stops swallowing "permanently, for that
    // session", which is only meaningful if the next session gets a fresh answer.
    editing_active_ = false;
    ui_fail_open_ = false;
    pill_dragging_ = false;
    // Seeded from the activation, so that a UI thread which is answering *now* is not judged
    // stale on the very first keystroke merely because nobody has stamped a tick yet.
    ui_heartbeat_ms_ = now_ms;
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
    // The session is over by some route other than the release — Esc, a stop-on-input click, the
    // silence timer. The binding may well still be held, and its release must then mean nothing:
    // finalising a session that has already ended is at best a no-op and at worst, once the next
    // one has started, ends a session the user only just began.
    ptt_held_ = false;
    editing_active_ = false;
    ui_fail_open_ = false;
    pill_dragging_ = false;
    for (size_t i = 0; i < kVkWords; ++i) down_at_activation_[i] = 0;
    // hotkey_chord_consumed_ is deliberately NOT cleared: the binding key may still be held, and
    // its key-up must still be swallowed to match the key-down we swallowed. `swallowed_down_`
    // is left alone for exactly the same reason — Enter is very often still down at the instant
    // the commit it triggered ends the session, and its release has to go the same way its press
    // did or the target sees half a keystroke.
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

bool InputFilter::is_passthrough(uint32_t v) const noexcept {
    // docs/editing.md §4.3's two disjoint Passthrough groups. Never swallowed, never committing.

    // 1. A bare modifier expresses no intention on its own, and swallowing a modifier's key-up is
    //    how "my Shift key is stuck on" bugs are born — the reasoning this file already applies
    //    to the binding key, applied to the rest.
    switch (v) {
        case vk::kShift:
        case vk::kLShift:
        case vk::kRShift:
        case vk::kControl:
        case vk::kLControl:
        case vk::kRControl:
        case vk::kMenu:
        case vk::kLMenu:
        case vk::kRMenu:
        case vk::kLWin:
        case vk::kRWin:
            return true;
        default:
            break;
    }

    // 2. The keys that change the machine rather than the document. None of them can put a
    //    character anywhere, so swallowing them is pure loss: turning the volume down
    //    mid-dictation should simply work.
    //
    //    "unless one of them is the binding" — CapsLock is an offered binding (hotkey_presets),
    //    and a bound key must reach chord_matches rather than being excused here.
    if (v == spec_.vk) return false;
    switch (v) {
        case vk::kVolumeMute:
        case vk::kVolumeDown:
        case vk::kVolumeUp:
        case vk::kMediaNextTrack:
        case vk::kMediaPrevTrack:
        case vk::kMediaStop:
        case vk::kMediaPlayPause:
        case vk::kPrintScreen:
        case vk::kCapsLock:
        case vk::kNumLock:
        case vk::kScrollLock:
            return true;
        default:
            return false;
    }
}

namespace {

// A key that carries a character on some layout. Deliberately a range test over the blocks
// Windows reserves for character keys rather than a list of characters: *which* character a key
// produces is a property of the layout, resolved on the UI thread (docs/editing.md §4.2), and a
// filter that tried to know would be wrong on every layout but the one it was written on.
bool is_character_vk(uint32_t v) noexcept {
    if (v >= vk::k0 && v <= vk::k9) return true;
    if (v >= vk::kA && v <= vk::kZ) return true;
    if (v == vk::kSpace) return true;
    if (v >= vk::kNumpad0 && v <= vk::kDivide) return true;   // the numeric keypad block
    if (v >= vk::kOem1 && v <= vk::kOem3) return true;        // ;=,-./` on a US layout
    if (v >= vk::kOem4 && v <= vk::kOem8) return true;        // [\]' on a US layout
    if (v == vk::kOem102) return true;                        // the extra key on 102-key boards
    return false;
}

}  // namespace

EditOp InputFilter::classify_edit(uint32_t v, uint32_t mods, bool& needs_char) const noexcept {
    needs_char = false;

    const bool ctrl = (mods & kModCtrl) != 0;
    const bool alt = (mods & kModAlt) != 0;
    const bool win = (mods & kModWin) != 0;

    // Anything with the Win key belongs to the shell, not to the buffer.
    if (win) return EditOp::None;

    switch (v) {
        case vk::kBack: return ctrl ? EditOp::DeleteWordBack : EditOp::Backspace;
        case vk::kDelete: return ctrl ? EditOp::DeleteWordForward : EditOp::Delete;
        case vk::kLeft: return ctrl ? EditOp::MoveWordLeft : EditOp::MoveLeft;
        case vk::kRight: return ctrl ? EditOp::MoveWordRight : EditOp::MoveRight;
        case vk::kUp: return EditOp::MoveUp;
        case vk::kDown: return EditOp::MoveDown;
        case vk::kHome: return ctrl ? EditOp::MoveDocStart : EditOp::MoveLineStart;
        case vk::kEnd: return ctrl ? EditOp::MoveDocEnd : EditOp::MoveLineEnd;
        default: break;
    }

    // The Ctrl chords the pill claims. An exact match on Ctrl alone: Ctrl+Shift+A is somebody
    // else's shortcut and Ctrl+Alt is AltGr, which produces a character on many layouts and is
    // handled below.
    if (mods == kModCtrl) {
        switch (v) {
            case vk::kA: return EditOp::SelectAll;
            case vk::kC: return EditOp::Copy;
            case vk::kX: return EditOp::Cut;
            case vk::kZ: return EditOp::Undo;
            // Ctrl+V is deliberately absent from this switch: the clipboard is a Windows API and
            // EditBuffer has none, so the UI thread reads it and issues InsertText. Returning
            // InsertText here with an empty payload is what tells it to.
            case vk::kV:
                needs_char = true;
                return EditOp::InsertText;
            default: return EditOp::None;
        }
    }

    // A printable character. Ctrl+Alt together is AltGr — the third level on most European
    // layouts, where it is the only way to type @ or backslash — so it is allowed through;
    // Ctrl alone is not, because a Ctrl chord means a command everywhere.
    if (is_character_vk(v) && (!ctrl || alt)) {
        needs_char = true;
        return EditOp::InsertText;
    }

    return EditOp::None;
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

    // ── Rule 0.5: fail open (docs/editing.md §10.1) ──────────────────────────────────────────
    //
    // Checked here, before any branch can decide to swallow, because the property has to hold
    // for every path rather than for the ones somebody remembered. The UI thread stamps a tick
    // every message-loop iteration; a low-level hook event carries the same clock (both are
    // GetTickCount), so the comparison is a subtraction with no syscall.
    //
    // Signed, and through uint32_t first, because tick counts wrap every 49.7 days: an unsigned
    // compare would declare the UI thread dead for the whole second half of that cycle, which is
    // the one bug this check must not have.
    //
    // Latched, not sampled: §10.1 says the hook stops swallowing "permanently, for that session".
    // A UI thread that recovers mid-session would otherwise re-enter a mode the user has already
    // been thrown out of, halfway through whatever they typed to get out of it.
    if (session_active_ && editing_.enabled && !ui_fail_open_) {
        const int32_t behind = static_cast<int32_t>(static_cast<uint32_t>(e.time_ms) -
                                                    static_cast<uint32_t>(ui_heartbeat_ms_));
        if (behind > static_cast<int32_t>(kUiStaleMs)) ui_fail_open_ = true;
    }

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

        // ── Push-to-talk: the release IS the finalise ────────────────────────────────────────
        //
        // Computed here, above every early return below, and folded into whatever outcome those
        // returns produce — the binding key's release takes one of them, so deciding further
        // down would have covered the modifier and missed the key.
        //
        // "Let go" means letting go of any part of the chord, not specifically the bound key.
        // Somebody holding Win+Y and releasing Win first has stopped holding the hotkey, and a
        // gesture that only ended on the Y would leave them talking into a session they thought
        // was over. Cleared before it fires so the rest of the chord coming up — the Y a few
        // milliseconds after the Win — cannot finalise a second time.
        //
        // It is the same `toggle_finalize` the second press produces: `stop`, a last decode, and
        // the text typed into the target application. Push-to-talk moves *when* that happens,
        // never what it does.
        if (ptt_held_ && (e.vk == spec_.vk || is_binding_modifier(e.vk))) {
            ptt_held_ = false;
            out.toggle_finalize = true;
        }

        // The binding key's release. We swallowed its press, so we must swallow its release too:
        // a target that saw no key-down but does see a key-up can get its own modifier tracking
        // stuck (this is how "my Shift key is stuck on" bugs are born).
        if (e.vk == spec_.vk && hotkey_chord_consumed_) {
            hotkey_chord_consumed_ = false;
            out.swallow = true;
            return out;
        }

        // The same rule, generalised to every key the pill swallowed on its own behalf: Enter,
        // and now Esc. The set is written by the key-down path below and drained here, so a key
        // can only ever have its release swallowed once, and only if its press actually was.
        if (bit_test(swallowed_down_, e.vk)) {
            bit_clear(swallowed_down_, e.vk);
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
            if (!session_active_) {
                out.activate = true;
                begin_session_state(e.time_ms);
                ptt_held_ = spec_.push_to_talk;
            } else if (spec_.push_to_talk) {
                // Push-to-talk: **a press never ends a session.** The release does, in the key-up
                // path above. Reaching here at all means a session was started by some route
                // other than this binding — the tray menu — because the chord's own repeats are
                // stopped by `hotkey_chord_consumed_` and the only way to press the binding
                // again is to release it first, which is the finalise. The user is now holding
                // the binding over someone else's session, and under this gesture that can only
                // mean "keep going, and finish when I let go".
                //
                // Placed before the toggle branch rather than folded into it, because the two
                // are mutually exclusive by definition and interleaving them would make the
                // toggle's own reasoning conditional on a mode it predates.
                if (editing_active_) {
                    editing_active_ = false;
                    out.resume_dictation = true;  // §3.3, exactly as the toggle binding does
                }
                ptt_held_ = true;
            } else if (!composing()) {
                // plan.md §1.8's toggle, exactly as it was. It survives wherever the compose
                // buffer does not exist — `editing.enabled` false, or a cadence other than
                // end-of-session — which is also why session.cpp's finalise-from-Running branch
                // is live code rather than the dead branch docs/editing.md §16 feared.
                out.toggle_finalize = true;
            } else if (editing_active_) {
                // docs/editing.md §3.3 and §4.5. Reopen the microphone; the caret moves to the
                // end of the buffer and the selection is dropped, on the UI thread.
                //
                // Cleared here rather than on a call back from the UI thread, for the same reason
                // begin_session_state() snapshots at chord-recognition time: the answer has to be
                // right for the *next* keystroke, which reaches this hook long before a posted
                // message reaches the UI thread and comes back.
                editing_active_ = false;
                out.resume_dictation = true;
            } else {
                // Already dictating. §4.5: the hotkey means "dictate" and nothing else, ever, so
                // there is deliberately nothing to do — and it is still swallowed, because a
                // target that received the chord's second key would be worse off than one that
                // saw nothing. The obvious alternative (pause the microphone, symmetrically with
                // resuming it) is a one-line change here if the no-op turns out to feel dead in
                // use; it is not the default because "always start or continue" is a rule with no
                // exceptions and "toggle the mic" reintroduces one.
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

    // ── Esc cancels, and is consumed ─────────────────────────────────────────────────────────
    //
    // **This used to pass through, and the reversal is deliberate.** The argument for passing it
    // on was that Esc means "get me out of this" in the target too, so eating it would mean a
    // dialog that refused to close while dictation happened to be running.
    //
    // That traded a common cost for a rare one, and got it backwards. Cancelling is an ordinary
    // gesture — start dictating, change your mind, press Esc — and every one of those leaked an
    // Esc into whatever had focus: it interrupts a running agent in a terminal, leaves vim's
    // insert mode, dismisses an autocomplete, closes a dialog the user was not finished with.
    // Reported from real use within minutes of the first install. The dialog case costs one
    // extra keypress (end the session, then Esc) and only when a dialog appears mid-dictation.
    //
    // It also makes the two session-ending keys symmetrical. Enter commits and is swallowed, for
    // the ordering reason docs/editing.md §2 records; Esc discards, and there was never a reason
    // for it to behave differently.
    //
    // Swallowed on the way down *and* on the way up: a target that saw only the key-up of an Esc
    // it never saw pressed is worse off than one that saw neither, and this is the same
    // `swallowed_down_` bookkeeping the commit key uses.
    if (e.vk == vk::kEscape) {
        out.swallow = true;
        bit_set(swallowed_down_, e.vk);
        if (!was_down) out.cancel = true;
        return out;
    }

    // ── Enter commits, and is consumed ───────────────────────────────────────────────────────
    //
    // docs/editing.md §4.3. **Unmodified only**: Ctrl+Enter, Shift+Enter and Alt+Enter mean
    // something in the target (send-and-keep-open, soft line break, properties) and are not this
    // gesture. Auto-repeat is excluded too — one press, one commit — but the repeats are still
    // swallowed, because a target that received only the *second* Enter of a held key is worse
    // than one that received none.
    //
    // Swallowed rather than passed through, which is the entire fix for the ordering bug in
    // docs/editing.md §2: stop-on-input let the Enter reach the application first and the
    // session's text was injected after it, one line below where the user was typing.
    if (composing() && e.vk == editing_.commit_vk &&
        current_mods(editing_.commit_vk) == kModNone) {
        out.swallow = true;
        bit_set(swallowed_down_, e.vk);
        if (!was_down) out.commit = true;
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

    // ── While composing, the keyboard belongs to the pill ────────────────────────────────────
    //
    // docs/editing.md §4.1 states the price of the feature in one line and it is worth repeating
    // where it is charged: the pill can never be given focus (WS_EX_NOACTIVATE is the flag
    // overlay.h calls the worst failure in the product), so the only way a keystroke can reach it
    // is to be taken from the window that *is* focused. There is no third option.
    //
    // Note what this branch is placed *after*: the fail-open latch in on_event, the chord, Esc
    // and Commit. Every one of those is a way out of the mode, and none of them may be reachable
    // only through a classification that might fail.
    if (composing()) {
        const uint32_t mods = current_mods(e.vk);

        // §4.3's Passthrough class: never swallowed, never committing.
        if (is_passthrough(e.vk)) return out;

        bool needs_char = false;
        const EditOp op = classify_edit(e.vk, mods, needs_char);
        if (op != EditOp::None) {
            out.swallow = true;
            bit_set(swallowed_down_, e.vk);
            out.edit_op = op;
            out.edit_shift = (mods & kModShift) != 0;
            out.edit_needs_char = needs_char;
            out.key_vk = e.vk;
            out.key_scan = e.scan_code;
            out.key_mods = mods;
            out.caps_lock = e.caps_lock;

            // §3.2: the first edit key **stops the capture** — not gates it, not discards its
            // frames; stops it, at the device. Reported once, on the transition, because closing
            // an already-closed microphone is not a state change the session machine should see.
            if (!editing_active_) {
                editing_active_ = true;
                out.editing_began = true;
            }
            return out;
        }

        // §4.4's Ignore class: Ctrl+S, F5, Alt+Tab, Win+E. Swallowed, and nothing happens.
        //
        // An earlier revision of the design had this commit, exactly as Enter does — which is
        // right for a passive preview and wrong for a surface the user is operating: someone
        // editing a paragraph presses Ctrl+S from habit and their half-finished text is typed
        // into the document and the session is over. An irreversible action, from a key that
        // means something else, with no warning. The cost of the retraction is that Ctrl+S
        // silently does nothing, so it is not silent: `foreign_key` puts one line in the pill
        // naming the key and saying what to do about it.
        if (editing_.on_foreign_key == ForeignKeyAction::Ignore) {
            out.swallow = true;
            bit_set(swallowed_down_, e.vk);
            out.foreign_key = true;
            out.key_vk = e.vk;
            out.key_mods = mods;
            return out;
        }
        // ForeignKeyAction::EndSession — today's behaviour, kept one setting away (§12): the key
        // passes through to the target and stop-on-input ends the session, below.
    }

    // ── Anything else is the user doing something else ───────────────────────────────────────
    out.stop_on_input = true;
    out.stop_reason = StopInputReason::Keystroke;
    return out;
}

FilterOutcome InputFilter::on_mouse_event(const InputEvent& e) {
    FilterOutcome out;

    switch (e.kind) {
        case InputEvent::Kind::MouseWheel:
            // Over the pill: scroll it (docs/editing.md §7.3). Scrolling is **not** an edit
            // gesture and does not close the microphone — the comment below already reasons that
            // scrolling to re-read what you dictated is a normal thing to do mid-session, and
            // that stays true when the thing being scrolled is our own pill.
            //
            // The anchor's cached click is deliberately NOT invalidated here: the user scrolled
            // *our* window, so the target's caret has not moved and the cached point is still
            // where the insertion point is.
            if (composing() && editing_.mouse && point_in_pill(e.x, e.y)) {
                out.pill_pointer = true;
                out.pill_kind = FilterOutcome::PillPointerKind::Scroll;
                out.pill_x = e.x - pill_left_;
                out.pill_y = e.y - pill_top_;
                out.pill_wheel = e.wheel_delta;
                return out;
            }
            // plan.md §1.5, source B: scrolling moves the caret with no click, so the cached
            // click is no longer where the insertion point is. Invalidate rather than downgrade
            // — a stale anchor that looks confident is worse than no anchor.
            out.click_invalidated = true;
            // Not stop-on-input: plan.md §1.8 says "any real click or keystroke", and a wheel is
            // neither. Scrolling to re-read what you dictated is a normal thing to do mid-session.
            return out;

        case InputEvent::Kind::MouseDown:
            // Inside the pill: place the caret. **Not** stop-on-input — docs/editing.md §5.2.
            // The click also never reaches the target, because the overlay drops
            // WS_EX_TRANSPARENT for the duration of a composing session and absorbs it; the
            // filter's job here is only to stop reading it as "the user has gone elsewhere".
            //
            // Left button only. A right-click opens a context menu and a middle-click pastes,
            // and neither is a caret placement — they fall through and end the session, which is
            // what they meant before the pill was clickable and still do.
            if (e.left_button && composing() && editing_.mouse && point_in_pill(e.x, e.y)) {
                pill_dragging_ = true;
                out.pill_pointer = true;
                out.pill_kind = FilterOutcome::PillPointerKind::Down;
                out.pill_x = e.x - pill_left_;
                out.pill_y = e.y - pill_top_;
                return out;
            }
            // Left button only for the anchor cache: a right-click opens a context menu and a
            // middle-click pastes on X11, so neither indicates where the insertion point is.
            if (e.left_button) out.click_captured = true;
            // Every other button stops the session — plan.md §1.8's "any real click".
            if (session_active_) {
                out.stop_on_input = true;
                out.stop_reason = StopInputReason::MouseClick;
            }
            return out;

        case InputEvent::Kind::MouseUp:
            // The end of a drag-select. Reported even when the pointer has left the pill, because
            // a selection that started inside it is the user's and letting go anywhere ends it.
            if (pill_dragging_) {
                pill_dragging_ = false;
                out.pill_pointer = true;
                out.pill_kind = FilterOutcome::PillPointerKind::Up;
                out.pill_x = e.x - pill_left_;
                out.pill_y = e.y - pill_top_;
                return out;
            }
            // The matching button-down already stopped the session and already updated the
            // cache. Acting on the up as well would double-report.
            return out;

        case InputEvent::Kind::MouseMove:
            // Only while dragging. A pointer resting over the pill is not an intention, and
            // moving the mouse must never stop a session — dictating with a hand on the mouse
            // would otherwise be impossible.
            if (pill_dragging_) {
                out.pill_pointer = true;
                out.pill_kind = FilterOutcome::PillPointerKind::Move;
                out.pill_x = e.x - pill_left_;
                out.pill_y = e.y - pill_top_;
                return out;
            }
            [[fallthrough]];
        case InputEvent::Kind::KeyDown:
        case InputEvent::Kind::KeyUp:
        default:
            // Moving the mouse is not an intention. It must not stop a session, or dictating
            // while resting a hand on the mouse would be impossible.
            return out;
    }
}

void InputFilter::note_ui_heartbeat(uint64_t tick_ms) noexcept { ui_heartbeat_ms_ = tick_ms; }

bool InputFilter::failed_open() const noexcept { return ui_fail_open_; }

bool InputFilter::composing() const noexcept {
    return session_active_ && editing_.enabled && !ui_fail_open_;
}

bool InputFilter::editing_active() const noexcept { return editing_active_; }

void InputFilter::set_pill_rect(int32_t left, int32_t top, int32_t right, int32_t bottom) noexcept {
    pill_left_ = left;
    pill_top_ = top;
    pill_right_ = right;
    pill_bottom_ = bottom;
}

bool InputFilter::point_in_pill(int32_t x, int32_t y) const noexcept {
    // An empty (or unpublished) rectangle contains nothing, which is what makes the mouse behave
    // exactly as it did before this feature when the UI thread has not published one.
    if (pill_right_ <= pill_left_ || pill_bottom_ <= pill_top_) return false;
    return x >= pill_left_ && x < pill_right_ && y >= pill_top_ && y < pill_bottom_;
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
constexpr UINT kHookMsgSetEditing = WM_USER + 6;     // lParam: new EditingConfig*, owned by the loop

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

    // docs/editing.md §10.1's heartbeat: written by the UI thread every message-loop iteration,
    // read by the hook callback. Relaxed on both sides — it is a staleness estimate, and an
    // acquire/release pair would buy an ordering nothing here depends on.
    std::atomic<uint64_t> ui_heartbeat{0};

    // The pill's rectangle in physical screen pixels, published by the UI thread after every
    // paint and read by the mouse callback (docs/editing.md §5.2). Four independent relaxed
    // atomics rather than a seqlock: the worst a torn read can produce is one click judged
    // against a rectangle that is half old and half new, during a frame in which the pill was
    // moving, and the consequence of getting it wrong in either direction is a single click that
    // ends the session or does not. A seqlock's retry loop would be a worse trade in a callback
    // whose budget is measured in microseconds.
    std::atomic<int32_t> pill_left{0};
    std::atomic<int32_t> pill_top{0};
    std::atomic<int32_t> pill_right{0};
    std::atomic<int32_t> pill_bottom{0};

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
    if (o.commit) post_to_owner(rt, HotkeyPost::Commit, 0);
    // Order matters and is the order the UI thread must see them in: the microphone closes
    // before the keystroke that closed it is applied, so the session machine's flush of the
    // in-flight utterance (docs/editing.md §3.3) happens against a buffer the edit has not yet
    // touched.
    if (o.editing_began) post_to_owner(rt, HotkeyPost::EditingBegan, 0);
    if (o.resume_dictation) post_to_owner(rt, HotkeyPost::ResumeDictation, 0);
    if (o.edit_op != EditOp::None) {
        EditPost ep;
        ep.op = o.edit_op;
        ep.shift = o.edit_shift;
        ep.caps_lock = o.caps_lock;
        ep.needs_char = o.edit_needs_char;
        ep.vk = o.key_vk;
        ep.scan = o.key_scan;
        ep.mods = o.key_mods;
        post_to_owner(rt, HotkeyPost::Edit, static_cast<intptr_t>(pack_edit_post(ep)));
    }
    if (o.pill_pointer) {
        PillPost pp;
        pp.kind = o.pill_kind;
        pp.x = o.pill_x;
        pp.y = o.pill_y;
        pp.wheel = o.pill_wheel;
        post_to_owner(rt, HotkeyPost::PillPointer, static_cast<intptr_t>(pack_pill_post(pp)));
    }
    if (o.foreign_key) {
        post_to_owner(rt, HotkeyPost::ForeignKey,
                      static_cast<intptr_t>(o.key_vk | (o.key_mods << 16)));
    }
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
    e.scan_code = k->scanCode;
    e.injected = (k->flags & LLKHF_INJECTED) != 0;
    e.extra_info = static_cast<uintptr_t>(k->dwExtraInfo);
    e.time_ms = k->time;

    // Caps lock is read here, in the callback, because it is read *at the moment of the
    // keystroke* and the UI thread resolves the character some milliseconds later — by which time
    // the user may have pressed Caps again. GetKeyState is a read of this thread's cached key
    // state: no syscall, no blocking, and nothing the hook budget can notice.
    e.caps_lock = (GetKeyState(VK_CAPITAL) & 1) != 0;

    // k->time is already the tick count of the event, so the watchdog's clock costs no call.
    rt->last_event_tick = k->time;
    rt->key_events.fetch_add(1, std::memory_order_relaxed);

    // docs/editing.md §10.1. A relaxed load of one atomic, handed to the pure filter that owns
    // the decision — the filter is where the comparison lives so that a stale stamp can be
    // injected in a unit test without a hung thread.
    rt->filter.note_ui_heartbeat(rt->ui_heartbeat.load(std::memory_order_relaxed));

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
        case WM_MOUSEHWHEEL:
            e.kind = InputEvent::Kind::MouseWheel;
            // MSLLHOOKSTRUCT packs the delta in the high word of mouseData, signed.
            e.wheel_delta = static_cast<int16_t>(
                (reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam)->mouseData >> 16) & 0xFFFFu);
            break;
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

    // The same two reads the keyboard callback makes: the UI thread's heartbeat, and the pill's
    // rectangle. Both are relaxed loads of atomics the UI thread wrote, and neither can block.
    rt->filter.note_ui_heartbeat(rt->ui_heartbeat.load(std::memory_order_relaxed));
    rt->filter.set_pill_rect(rt->pill_left.load(std::memory_order_relaxed),
                             rt->pill_top.load(std::memory_order_relaxed),
                             rt->pill_right.load(std::memory_order_relaxed),
                             rt->pill_bottom.load(std::memory_order_relaxed));

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
    ready.set_value(Status::success());

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
            case kHookMsgSetEditing: {
                EditingConfig* cfg = reinterpret_cast<EditingConfig*>(msg.lParam);
                if (cfg != nullptr) {
                    rt->filter.set_editing(*cfg);
                    delete cfg;
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
    KD_LOG_INFO("hotkey installed: %s (%s)", spec.to_display_string().c_str(),
                spec.push_to_talk ? "push-to-talk" : "toggle");
    return Status::success();
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

void HotkeyManager::set_editing(const EditingConfig& cfg) {
    HookRuntime* rt = &impl_->rt;
    if (!rt->running.load(std::memory_order_acquire)) {
        rt->filter.set_editing(cfg);
        return;
    }
    EditingConfig* copy = new EditingConfig(cfg);
    if (!PostThreadMessageW(rt->thread_id.load(std::memory_order_acquire), kHookMsgSetEditing, 0,
                            reinterpret_cast<LPARAM>(copy))) {
        delete copy;
        KD_LOG_ERROR("set_editing: PostThreadMessage failed (err %lu)", GetLastError());
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

void HotkeyManager::set_pill_rect(int32_t left, int32_t top, int32_t right,
                                  int32_t bottom) noexcept {
    HookRuntime* rt = &impl_->rt;
    rt->pill_left.store(left, std::memory_order_relaxed);
    rt->pill_top.store(top, std::memory_order_relaxed);
    rt->pill_right.store(right, std::memory_order_relaxed);
    rt->pill_bottom.store(bottom, std::memory_order_relaxed);
}

void HotkeyManager::heartbeat() noexcept {
    // No `running` check and no PostThreadMessage: this is the one piece of hook-thread-visible
    // state the UI thread writes directly, because a heartbeat delivered through the hook
    // thread's message queue would stop being delivered at exactly the moment it matters — a UI
    // thread that is wedged is not posting, and a hook thread that is busy is not draining.
    impl_->rt.ui_heartbeat.store(GetTickCount64() & 0xFFFFFFFFull, std::memory_order_relaxed);
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
        case HotkeyPost::Commit: rt->listener->on_commit(); break;
        case HotkeyPost::Edit:
            rt->listener->on_edit(unpack_edit_post(static_cast<uint64_t>(lparam)));
            break;
        case HotkeyPost::EditingBegan: rt->listener->on_editing_began(); break;
        case HotkeyPost::PillPointer:
            rt->listener->on_pill_pointer(unpack_pill_post(static_cast<uint64_t>(lparam)));
            break;
        case HotkeyPost::ResumeDictation: rt->listener->on_resume_dictation(); break;
        case HotkeyPost::ForeignKey:
            rt->listener->on_foreign_key(static_cast<uint32_t>(lparam) & 0xFFFFu,
                                         static_cast<uint32_t>(lparam) >> 16);
            break;
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
