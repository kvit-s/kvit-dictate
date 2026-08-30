// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Suite `hotkey` — InputFilter, the pure half of src/hotkey.cpp.
//
// plan.md §1.8 calls the activation rule "a unit-tested state machine, not an ad-hoc filter", and
// plan.md §7 gates on "hooks survive long sessions and ignore self-injected input". This file is
// where that claim is cashed. Nothing here installs a hook, touches a window, or needs a
// keyboard: InputFilter takes synthetic InputEvent sequences and returns decisions, which is the
// entire reason it was separated from the callbacks.
//
// What is deliberately NOT covered here, and why:
//   * SetWindowsHookEx / the hook thread / the watchdog — they need a real message loop and real
//     input. tools/probe exercises them.
//   * suppress_start_menu() itself calls SendInput, so a unit test could only assert that the
//     call succeeded, not that the Start menu stayed shut. What IS tested here is the part that
//     can break silently: that the tap it injects is classified as ours and never stops the
//     session it was injected to protect.

#include "hotkey.h"
#include "kd_test.h"

namespace {

using kd::InputEvent;

InputEvent key_event(InputEvent::Kind kind, uint32_t vk, uint64_t t) {
    InputEvent e;
    e.kind = kind;
    e.vk = vk;
    e.time_ms = t;
    return e;
}

InputEvent kdown(uint32_t vk, uint64_t t = 100) {
    return key_event(InputEvent::Kind::KeyDown, vk, t);
}

InputEvent kup(uint32_t vk, uint64_t t = 100) {
    return key_event(InputEvent::Kind::KeyUp, vk, t);
}

InputEvent mouse_event(InputEvent::Kind kind, bool left, int32_t x, int32_t y, uint64_t t) {
    InputEvent e;
    e.kind = kind;
    e.left_button = left;
    e.x = x;
    e.y = y;
    e.time_ms = t;
    return e;
}

InputEvent mdown(bool left, int32_t x = 0, int32_t y = 0, uint64_t t = 100) {
    return mouse_event(InputEvent::Kind::MouseDown, left, x, y, t);
}

InputEvent mup(bool left, uint64_t t = 100) {
    return mouse_event(InputEvent::Kind::MouseUp, left, 0, 0, t);
}

InputEvent wheel(uint64_t t = 100) {
    return mouse_event(InputEvent::Kind::MouseWheel, false, 0, 0, t);
}

InputEvent mmove(int32_t x, int32_t y, uint64_t t = 100) {
    return mouse_event(InputEvent::Kind::MouseMove, false, x, y, t);
}

// Press the default Win+Y chord and assert it took. Returns with both keys still held, exactly as
// they are in reality at the moment the session starts.
void activate_win_y(kd::InputFilter& f, uint64_t t = 100) {
    f.on_event(kdown(kd::vk::kLWin, t));
    const kd::FilterOutcome o = f.on_event(kdown(kd::vk::kY, t + 10));
    KD_REQUIRE(o.activate);
}

void release_win_y(kd::InputFilter& f, uint64_t t = 120) {
    f.on_event(kup(kd::vk::kY, t));
    f.on_event(kup(kd::vk::kLWin, t + 10));
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  HotkeySpec: the settings-file contract
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(hotkey, presets_round_trip_through_their_display_strings) {
    const auto presets = kd::hotkey_presets();
    KD_REQUIRE(!presets.empty());

    // The default comes first, because the settings dialog shows the list in order.
    KD_CHECK_EQ(presets.front().first, std::string("Win+Y"));

    for (const auto& p : presets) {
        // The preset's name IS its canonical display string — one spelling, not two.
        KD_CHECK_EQ(p.second.to_display_string(), p.first);

        kd::HotkeySpec parsed{};
        KD_REQUIRE(kd::HotkeySpec::parse(p.first, parsed));
        KD_CHECK_EQ(parsed.vk, p.second.vk);
        KD_CHECK_EQ(parsed.mods, p.second.mods);
        KD_CHECK_EQ(parsed.require_left_win, p.second.require_left_win);
        KD_CHECK_EQ(parsed.to_display_string(), p.first);
    }
}

KD_TEST(hotkey, presets_are_exactly_the_list_in_plan_1_8) {
    const auto presets = kd::hotkey_presets();
    const auto has = [&presets](const char* name) {
        for (const auto& p : presets) {
            if (p.first == name) return true;
        }
        return false;
    };

    // Win+Y (default) · Win+J · Win+O · Ctrl+Win+Space · F13–F24 · right Ctrl / right Alt /
    // CapsLock. 4 + 12 + 3.
    KD_CHECK_EQ(presets.size(), size_t{19});
    KD_CHECK(has("Win+Y"));
    KD_CHECK(has("Win+J"));
    KD_CHECK(has("Win+O"));
    KD_CHECK(has("Ctrl+Win+Space"));
    KD_CHECK(has("F13"));
    KD_CHECK(has("F24"));
    KD_CHECK(has("RCtrl"));
    KD_CHECK(has("RAlt"));
    KD_CHECK(has("CapsLock"));

    // The Win-key options need the Start-menu hack; the rest exist precisely so that they do not.
    for (const auto& p : presets) {
        const bool is_win_combo = p.first.find("Win+") != std::string::npos;
        KD_CHECK_EQ(p.second.needs_start_menu_suppression(), is_win_combo);
    }
}

KD_TEST(hotkey, parse_rejects_malformed_bindings) {
    kd::HotkeySpec s{};
    KD_CHECK(!kd::HotkeySpec::parse("", s));
    KD_CHECK(!kd::HotkeySpec::parse("Win+", s));
    KD_CHECK(!kd::HotkeySpec::parse("+Y", s));
    KD_CHECK(!kd::HotkeySpec::parse("Ctrl++Y", s));
    KD_CHECK(!kd::HotkeySpec::parse("Win+F25", s));   // F13–F24 only
    KD_CHECK(!kd::HotkeySpec::parse("Win+F0", s));
    KD_CHECK(!kd::HotkeySpec::parse("Hyper+Y", s));   // not a modifier we know
    KD_CHECK(!kd::HotkeySpec::parse("Win+Nonsense", s));

    // Case-insensitive, because a settings file is hand-editable.
    KD_REQUIRE(kd::HotkeySpec::parse("win+y", s));
    KD_CHECK_EQ(s.vk, kd::vk::kY);
    KD_CHECK_EQ(s.mods, static_cast<uint32_t>(kd::kModWin));
    KD_CHECK_EQ(s.to_display_string(), std::string("Win+Y"));
}

KD_TEST(hotkey, a_modifier_key_used_as_the_binding_parses_as_the_key) {
    // "RCtrl" must not be read as "the Ctrl modifier with no key". The rule is positional: every
    // token but the last is a modifier, the last is the key.
    kd::HotkeySpec s{};
    KD_REQUIRE(kd::HotkeySpec::parse("RCtrl", s));
    KD_CHECK_EQ(s.vk, kd::vk::kRControl);
    KD_CHECK_EQ(s.mods, static_cast<uint32_t>(kd::kModNone));

    KD_REQUIRE(kd::HotkeySpec::parse("CapsLock", s));
    KD_CHECK_EQ(s.vk, kd::vk::kCapsLock);
    KD_CHECK_EQ(s.mods, static_cast<uint32_t>(kd::kModNone));
}

KD_TEST(hotkey, stop_reason_names) {
    KD_CHECK_EQ(kd::to_string(kd::StopInputReason::Keystroke), "keystroke");
    KD_CHECK_EQ(kd::to_string(kd::StopInputReason::MouseClick), "mouse_click");
    KD_CHECK_EQ(kd::to_string(kd::StopInputReason::HotkeyToggle), "hotkey_toggle");
    KD_CHECK_EQ(kd::to_string(kd::StopInputReason::Escape), "escape");
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  The chord
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(hotkey, win_y_activates_and_is_swallowed) {
    kd::InputFilter f;  // default binding is Win+Y

    kd::FilterOutcome o = f.on_event(kdown(kd::vk::kLWin, 100));
    KD_CHECK(!o.swallow);   // the Win key itself passes through; only the Y is eaten
    KD_CHECK(!o.activate);
    KD_CHECK(!f.session_active());

    o = f.on_event(kdown(kd::vk::kY, 110));
    KD_CHECK(o.activate);
    KD_CHECK(o.swallow);
    KD_CHECK(o.suppress_start_menu);
    KD_CHECK(!o.stop_on_input);
    KD_CHECK(!o.toggle_finalize);
    KD_CHECK(f.session_active());
}

KD_TEST(hotkey, the_bare_binding_key_without_its_modifier_is_left_alone) {
    kd::InputFilter f;
    kd::FilterOutcome o = f.on_event(kdown(kd::vk::kY, 100));
    KD_CHECK(!o.activate);
    KD_CHECK(!o.swallow);  // the user typed a 'y' and must get a 'y'
    KD_CHECK(!f.session_active());

    o = f.on_event(kup(kd::vk::kY, 110));
    KD_CHECK(!o.swallow);  // and no orphaned key-up either
}

KD_TEST(hotkey, modifiers_must_match_exactly) {
    // Win+Shift+Y belongs to somebody else. "At least these modifiers" would steal it.
    kd::InputFilter f;
    f.on_event(kdown(kd::vk::kLShift, 100));
    f.on_event(kdown(kd::vk::kLWin, 110));

    const kd::FilterOutcome o = f.on_event(kdown(kd::vk::kY, 120));
    KD_CHECK(!o.activate);
    KD_CHECK(!o.swallow);
    KD_CHECK(!f.session_active());
}

KD_TEST(hotkey, require_left_win_rejects_the_right_win_key) {
    kd::HotkeySpec spec{};
    spec.vk = kd::vk::kY;
    spec.mods = kd::kModWin;
    spec.require_left_win = true;
    KD_CHECK_EQ(spec.to_display_string(), std::string("LWin+Y"));

    kd::InputFilter f;
    f.set_hotkey(spec);

    f.on_event(kdown(kd::vk::kRWin, 100));
    KD_CHECK(!f.on_event(kdown(kd::vk::kY, 110)).activate);
    f.on_event(kup(kd::vk::kY, 120));
    f.on_event(kup(kd::vk::kRWin, 130));

    f.on_event(kdown(kd::vk::kLWin, 200));
    KD_CHECK(f.on_event(kdown(kd::vk::kY, 210)).activate);
}

KD_TEST(hotkey, ctrl_win_space_requires_both_modifiers) {
    kd::HotkeySpec spec{};
    KD_REQUIRE(kd::HotkeySpec::parse("Ctrl+Win+Space", spec));

    kd::InputFilter f;
    f.set_hotkey(spec);

    f.on_event(kdown(kd::vk::kLWin, 100));
    KD_CHECK(!f.on_event(kdown(kd::vk::kSpace, 110)).activate);  // Win+Space is not the binding
    f.on_event(kup(kd::vk::kSpace, 120));

    f.on_event(kdown(kd::vk::kLControl, 130));
    const kd::FilterOutcome o = f.on_event(kdown(kd::vk::kSpace, 140));
    KD_CHECK(o.activate);
    KD_CHECK(o.swallow);
    KD_CHECK(o.suppress_start_menu);  // a Win key is involved, so the hack still applies
}

KD_TEST(hotkey, the_modifier_of_the_finalising_chord_is_not_user_input) {
    // Regression, found by pressing the hotkey twice in the real application.
    //
    // Win+Y reaches the hook as LWIN down and *then* Y down. The LWIN was classified as the user
    // typing, so the owner ended the session on it — and the Y ten milliseconds later found no
    // session running and started a *new* one. The user saw a second block of text appear instead
    // of the first one finishing.
    //
    // The test below pressed exactly these two keys and did not catch it, because it ignored the
    // modifier's outcome. Only the *owner* ends a session (note_session_ended), so a filter driven
    // without one keeps `session_active_` true and the Y still finalises. The assertion this test
    // adds is the whole difference.
    kd::InputFilter f;
    activate_win_y(f, 100);
    release_win_y(f, 120);

    const kd::FilterOutcome mod = f.on_event(kdown(kd::vk::kLWin, 500));
    KD_CHECK(!mod.stop_on_input);
    KD_CHECK(!mod.cancel);
    KD_CHECK(f.session_active());

    KD_CHECK(f.on_event(kdown(kd::vk::kY, 510)).toggle_finalize);
}

KD_TEST(hotkey, a_different_chord_on_the_same_modifier_still_stops_the_session) {
    // The other half of the rule above: excusing the binding's modifier must not excuse whatever
    // the user pressed it for. Win+A is somebody else's shortcut, and reaching for it while
    // dictating is exactly the "I am doing something else now" that plan.md §1.8 stops on.
    kd::InputFilter f;
    activate_win_y(f, 100);
    release_win_y(f, 120);

    KD_CHECK(!f.on_event(kdown(kd::vk::kLWin, 500)).stop_on_input);
    const kd::FilterOutcome o = f.on_event(kdown(kd::vk::kA, 510));
    KD_CHECK(o.stop_on_input);
    KD_CHECK_EQ(o.stop_reason, kd::StopInputReason::Keystroke);
    KD_CHECK(!o.swallow);  // and the target still gets its shortcut
}

KD_TEST(hotkey, a_modifier_the_binding_does_not_use_is_still_user_input) {
    // The excuse is scoped to the binding. With an F13 binding there is no modifier in the chord,
    // so a Win press during a session is the user reaching for the Start menu and nothing else.
    kd::HotkeySpec f13{};
    f13.vk = kd::vk::kF13;
    f13.mods = kd::kModNone;

    kd::InputFilter f;
    f.set_hotkey(f13);
    KD_REQUIRE(f.on_event(kdown(kd::vk::kF13, 100)).activate);
    f.on_event(kup(kd::vk::kF13, 110));

    KD_CHECK(f.on_event(kdown(kd::vk::kLWin, 200)).stop_on_input);
}

KD_TEST(hotkey, second_chord_press_finalises_rather_than_activating_again) {
    kd::InputFilter f;
    activate_win_y(f, 100);
    release_win_y(f, 120);

    KD_CHECK(!f.on_event(kdown(kd::vk::kLWin, 500)).stop_on_input);
    const kd::FilterOutcome o = f.on_event(kdown(kd::vk::kY, 510));
    KD_CHECK(o.toggle_finalize);
    KD_CHECK(!o.activate);
    KD_CHECK(o.swallow);
    KD_CHECK(!o.stop_on_input);  // the toggle must not also read as "the user did something else"
    KD_CHECK(o.suppress_start_menu);
}

KD_TEST(hotkey, changing_the_binding_takes_effect_immediately) {
    kd::InputFilter f;
    kd::HotkeySpec f13{};
    f13.vk = kd::vk::kF13;
    f13.mods = kd::kModNone;
    f.set_hotkey(f13);
    KD_CHECK_EQ(f.hotkey().vk, kd::vk::kF13);

    f.on_event(kdown(kd::vk::kLWin, 100));
    KD_CHECK(!f.on_event(kdown(kd::vk::kY, 110)).activate);  // the old binding is gone
    f.on_event(kup(kd::vk::kY, 120));
    f.on_event(kup(kd::vk::kLWin, 130));

    KD_CHECK(f.on_event(kdown(kd::vk::kF13, 200)).activate);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Bindings that need no Start-menu hack (plan.md §1.8's escape route)
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(hotkey, f13_binding_never_asks_for_the_start_menu_hack) {
    kd::HotkeySpec spec{};
    spec.vk = kd::vk::kF13;
    spec.mods = kd::kModNone;
    KD_CHECK(!spec.needs_start_menu_suppression());

    kd::InputFilter f;
    f.set_hotkey(spec);

    kd::FilterOutcome o = f.on_event(kdown(kd::vk::kF13, 100));
    KD_CHECK(o.activate);
    KD_CHECK(o.swallow);
    KD_CHECK(!o.suppress_start_menu);  // the whole point: this path calls SendInput never

    o = f.on_event(kup(kd::vk::kF13, 110));
    KD_CHECK(o.swallow);         // we ate the down, so the target must not see a lone up
    KD_CHECK(!o.stop_on_input);
    KD_CHECK(f.session_active());
}

KD_TEST(hotkey, capslock_binding_activates_toggles_and_is_swallowed) {
    kd::HotkeySpec spec{};
    spec.vk = kd::vk::kCapsLock;
    spec.mods = kd::kModNone;

    kd::InputFilter f;
    f.set_hotkey(spec);

    kd::FilterOutcome o = f.on_event(kdown(kd::vk::kCapsLock, 100));
    KD_CHECK(o.activate);
    // Swallowing is also what stops the caps-lock state from flipping on every activation.
    KD_CHECK(o.swallow);
    KD_CHECK(!o.suppress_start_menu);

    KD_CHECK(f.on_event(kup(kd::vk::kCapsLock, 110)).swallow);

    o = f.on_event(kdown(kd::vk::kCapsLock, 500));
    KD_CHECK(o.toggle_finalize);
    KD_CHECK(!o.activate);
    KD_CHECK(!o.stop_on_input);
}

KD_TEST(hotkey, right_ctrl_binding_matches_even_though_it_is_itself_a_modifier) {
    // The trap: pressing the bound key puts kModCtrl into the current modifier mask, which would
    // never equal the spec's kModNone. current_mods() excludes the bound key for this reason.
    kd::HotkeySpec spec{};
    spec.vk = kd::vk::kRControl;
    spec.mods = kd::kModNone;

    kd::InputFilter f;
    f.set_hotkey(spec);

    kd::FilterOutcome o = f.on_event(kdown(kd::vk::kRControl, 100));
    KD_CHECK(o.activate);
    KD_CHECK(o.swallow);

    f.on_event(kup(kd::vk::kRControl, 110));
    f.note_session_ended();

    // Left Ctrl is a different key and must be left entirely alone.
    o = f.on_event(kdown(kd::vk::kLControl, 200));
    KD_CHECK(!o.activate);
    KD_CHECK(!o.swallow);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Activation must not immediately self-stop (plan.md §1.8)
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(hotkey, the_chords_own_key_ups_do_not_stop_the_session_it_started) {
    // The regression that would otherwise make the product appear to do nothing at all.
    kd::InputFilter f;
    f.on_event(kdown(kd::vk::kLWin, 100));
    KD_REQUIRE(f.on_event(kdown(kd::vk::kY, 110)).activate);
    KD_CHECK_EQ(f.keys_held_at_activation(), size_t{2});

    kd::FilterOutcome o = f.on_event(kup(kd::vk::kY, 120));
    KD_CHECK(o.swallow);
    KD_CHECK(!o.stop_on_input);

    o = f.on_event(kup(kd::vk::kLWin, 130));
    KD_CHECK(!o.swallow);
    KD_CHECK(!o.stop_on_input);

    KD_CHECK(f.session_active());
    KD_CHECK_EQ(f.keys_held_at_activation(), size_t{0});
}

KD_TEST(hotkey, held_chord_keys_autorepeat_without_stopping_the_session) {
    // A held key auto-repeats and a low-level hook sees every repeat as a fresh key-down —
    // modifiers included. Without the down-at-activation rule, holding Win+Y for half a second
    // would start a session and kill it in the same breath.
    kd::InputFilter f;
    activate_win_y(f, 100);

    kd::FilterOutcome o = f.on_event(kdown(kd::vk::kLWin, 150));
    KD_CHECK(!o.stop_on_input);

    o = f.on_event(kdown(kd::vk::kY, 160));
    KD_CHECK(o.swallow);
    KD_CHECK(!o.activate);  // one action per physical press, not per repeat
    KD_CHECK(!o.stop_on_input);

    KD_CHECK(f.session_active());
}

KD_TEST(hotkey, a_key_held_across_activation_is_ignored_until_it_is_released) {
    kd::HotkeySpec spec{};
    spec.vk = kd::vk::kF13;
    spec.mods = kd::kModNone;

    kd::InputFilter f;
    f.set_hotkey(spec);

    f.on_event(kdown(kd::vk::kA, 100));  // a key the user happened to be leaning on
    KD_REQUIRE(f.on_event(kdown(kd::vk::kF13, 110)).activate);
    KD_CHECK_EQ(f.keys_held_at_activation(), size_t{2});

    KD_CHECK(!f.on_event(kdown(kd::vk::kA, 200)).stop_on_input);  // its auto-repeat
    KD_CHECK(!f.on_event(kup(kd::vk::kA, 300)).stop_on_input);    // its release
    KD_CHECK_EQ(f.keys_held_at_activation(), size_t{1});          // F13 is still held

    // But pressing it AGAIN is unambiguously the user doing something else.
    KD_CHECK(f.on_event(kdown(kd::vk::kA, 400)).stop_on_input);
}

KD_TEST(hotkey, releasing_the_modifier_mid_chord_still_swallows_the_bound_key) {
    kd::InputFilter f;
    activate_win_y(f, 100);
    f.on_event(kup(kd::vk::kLWin, 120));  // Win released first, Y still held

    // No longer a matching chord, but the target must still never receive a 'y' — we ate its
    // key-down, and an unmatched key-up is how a target's modifier tracking gets stuck.
    kd::FilterOutcome o = f.on_event(kdown(kd::vk::kY, 130));
    KD_CHECK(o.swallow);
    KD_CHECK(!o.activate);

    o = f.on_event(kup(kd::vk::kY, 140));
    KD_CHECK(o.swallow);
    KD_CHECK(!o.stop_on_input);
}

KD_TEST(hotkey, note_session_started_confirms_and_does_not_re_snapshot) {
    // The owner learns about activation through a PostMessage, so note_session_started() arrives
    // well after the hook already saw the chord's key-ups. If it re-snapshotted the held keys,
    // anything pressed in between would be baked into the "ignore me" set — and, worse, the
    // chord's own keys would already have fallen out of it.
    kd::InputFilter f;
    activate_win_y(f, 100);
    KD_CHECK_EQ(f.keys_held_at_activation(), size_t{2});

    f.on_event(kup(kd::vk::kY, 120));  // -> {LWin}

    // Real user input arrives before the UI thread confirms. It must still stop the session.
    KD_CHECK(f.on_event(kdown(kd::vk::kLControl, 125)).stop_on_input);

    f.note_session_started(140);
    KD_CHECK(f.session_active());
    KD_CHECK_EQ(f.keys_held_at_activation(), size_t{1});  // LWin only — not LWin and LControl
}

KD_TEST(hotkey, ending_the_session_clears_the_activation_snapshot) {
    kd::InputFilter f;
    activate_win_y(f, 100);
    f.note_session_ended();

    KD_CHECK(!f.session_active());
    KD_CHECK_EQ(f.keys_held_at_activation(), size_t{0});

    // Idle: a keystroke is nobody's business, and a click is only worth caching.
    KD_CHECK(!f.on_event(kdown(kd::vk::kA, 300)).stop_on_input);
    KD_CHECK(!f.on_event(mdown(true, 5, 6, 310)).stop_on_input);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Self-injected input
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(hotkey, os_flagged_injected_events_are_not_user_input) {
    kd::InputFilter f;
    activate_win_y(f, 100);
    release_win_y(f, 120);

    InputEvent e = kdown(kd::vk::kA, 200);
    e.injected = true;  // LLKHF_INJECTED
    const kd::FilterOutcome o = f.on_event(e);

    KD_CHECK(!o.stop_on_input);
    KD_CHECK(!o.swallow);
    // And it left no trace in the model of what the user is physically holding: a synthetic key
    // does not put a finger on a key, and treating it as one corrupts the modifier mask.
    KD_CHECK(!f.is_key_down(kd::vk::kA));
    KD_CHECK(f.session_active());
}

KD_TEST(hotkey, our_own_dwextrainfo_mark_is_not_user_input) {
    constexpr uintptr_t kMark = 0xDEADBEEFu;

    kd::InputFilter f;
    f.set_injection_mark(kMark);
    activate_win_y(f, 100);
    release_win_y(f, 120);

    // The mark alone is enough: it identifies *which* process injected the event, which the OS
    // flag does not.
    InputEvent ours = kdown(kd::vk::kA, 200);
    ours.injected = false;
    ours.extra_info = kMark;
    KD_CHECK(!f.on_event(ours).stop_on_input);

    // Another process's synthetic input is not ours and is not privileged. This is why the mark
    // is a per-process random value rather than a shared constant (injector.h).
    InputEvent theirs = kdown(kd::vk::kA, 210);
    theirs.extra_info = 0x12345678u;
    KD_CHECK(f.on_event(theirs).stop_on_input);
}

KD_TEST(hotkey, an_unset_injection_mark_does_not_filter_everything) {
    // dwExtraInfo is 0 on ordinary keystrokes. A naive `e.extra_info == injection_mark_` with the
    // mark left at its default would make the filter ignore every key the user ever pressed.
    kd::InputFilter f;
    activate_win_y(f, 100);
    release_win_y(f, 120);

    InputEvent e = kdown(kd::vk::kA, 200);
    e.extra_info = 0;
    KD_CHECK(f.on_event(e).stop_on_input);
}

KD_TEST(hotkey, the_start_menu_suppression_tap_is_never_user_input) {
    // The exact sequence suppress_start_menu() produces: a neutral VK_CONTROL down/up pair,
    // flagged injected and carrying our mark, arriving *after* the session has started and
    // *before* the user has released the Win key.
    constexpr uintptr_t kMark = 0xABCD1234u;

    kd::InputFilter f;
    f.set_injection_mark(kMark);

    f.on_event(kdown(kd::vk::kLWin, 100));
    const kd::FilterOutcome a = f.on_event(kdown(kd::vk::kY, 110));
    KD_REQUIRE(a.activate);
    KD_REQUIRE(a.suppress_start_menu);

    InputEvent tap_down = kdown(kd::vk::kControl, 111);
    tap_down.injected = true;
    tap_down.extra_info = kMark;
    InputEvent tap_up = kup(kd::vk::kControl, 112);
    tap_up.injected = true;
    tap_up.extra_info = kMark;

    kd::FilterOutcome o = f.on_event(tap_down);
    KD_CHECK(!o.stop_on_input);
    KD_CHECK(!o.cancel);
    KD_CHECK(!o.swallow);  // never swallow our own tap: the shell must see it, that is its job

    o = f.on_event(tap_up);
    KD_CHECK(!o.stop_on_input);

    // Critically, the tap must not have polluted the modifier mask: if it had, the Y auto-repeat
    // below would look like Ctrl+Win+Y and stop matching the binding.
    KD_CHECK(!f.is_key_down(kd::vk::kControl));
    KD_CHECK(f.on_event(kdown(kd::vk::kY, 150)).swallow);

    // And the real chord key-ups that follow are still harmless.
    KD_CHECK(!f.on_event(kup(kd::vk::kY, 200)).stop_on_input);
    KD_CHECK(!f.on_event(kup(kd::vk::kLWin, 210)).stop_on_input);
    KD_CHECK(f.session_active());
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Stop-on-input and cancel
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(hotkey, a_real_keystroke_stops_the_session_and_still_reaches_the_target) {
    kd::InputFilter f;
    activate_win_y(f, 100);
    release_win_y(f, 120);

    const kd::FilterOutcome o = f.on_event(kdown(kd::vk::kA, 400));
    KD_CHECK(o.stop_on_input);
    KD_CHECK_EQ(kd::to_string(o.stop_reason), "keystroke");
    KD_CHECK(!o.swallow);  // stop-on-input observes the keystroke; it does not eat it
    KD_CHECK(!o.cancel);
}

KD_TEST(hotkey, escape_cancels_only_while_a_session_is_running) {
    kd::InputFilter f;

    kd::FilterOutcome o = f.on_event(kdown(kd::vk::kEscape, 50));
    KD_CHECK(!o.cancel);  // idle: Esc is entirely the target's business
    KD_CHECK(!o.swallow);
    f.on_event(kup(kd::vk::kEscape, 60));

    activate_win_y(f, 100);
    release_win_y(f, 120);

    o = f.on_event(kdown(kd::vk::kEscape, 200));
    KD_CHECK(o.cancel);
    KD_CHECK(!o.stop_on_input);  // cancel is the specific signal; a generic stop would lose "why"
    // **Swallowed**, and this assertion is the reverse of what it used to be. Esc passed through
    // on the argument that a dialog opened mid-dictation should still close; in practice every
    // ordinary cancel leaked an Esc into whatever had focus, which interrupts a terminal, leaves
    // vim's insert mode and dismisses autocompletes. Reported from real use. See hotkey.cpp.
    KD_CHECK(o.swallow);
}

KD_TEST(hotkey, the_escape_key_up_is_swallowed_too) {
    // A target that saw only the key-*up* of an Esc it never saw pressed is worse off than one
    // that saw neither: an application watching for the release alone still acts on it. Same
    // `swallowed_down_` bookkeeping the commit key uses, and the same reason.
    kd::InputFilter f;
    activate_win_y(f, 100);
    release_win_y(f, 120);

    kd::FilterOutcome down = f.on_event(kdown(kd::vk::kEscape, 200));
    KD_CHECK(down.cancel);
    KD_CHECK(down.swallow);

    kd::FilterOutcome up = f.on_event(kup(kd::vk::kEscape, 220));
    KD_CHECK(up.swallow);
}

KD_TEST(hotkey, an_idle_escape_still_belongs_entirely_to_the_target) {
    // The scope of the change, asserted so it cannot widen by accident: with no session running
    // this filter has no business touching Esc at all, and a build that ate it everywhere would
    // be a keyboard hook that breaks the Escape key.
    kd::InputFilter f;
    kd::FilterOutcome o = f.on_event(kdown(kd::vk::kEscape, 50));
    KD_CHECK(!o.cancel);
    KD_CHECK(!o.swallow);
    KD_CHECK(!f.on_event(kup(kd::vk::kEscape, 60)).swallow);
}

KD_TEST(hotkey, escape_is_not_swallowed_after_the_session_has_ended) {
    // The session ends, and Esc goes back to being the target's. The auto-repeat of a held Esc
    // is the case that would otherwise keep swallowing forever.
    kd::InputFilter f;
    activate_win_y(f, 100);
    release_win_y(f, 120);
    KD_CHECK(f.on_event(kdown(kd::vk::kEscape, 200)).swallow);
    f.on_event(kup(kd::vk::kEscape, 210));

    f.note_session_ended();
    kd::FilterOutcome o = f.on_event(kdown(kd::vk::kEscape, 300));
    KD_CHECK(!o.cancel);
    KD_CHECK(!o.swallow);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  The Commit class — docs/editing.md §4.3, stage 1
// ═══════════════════════════════════════════════════════════════════════════════════════════
//
// The bug this fixes is an *ordering* bug (docs/editing.md §2): Enter used to be stop-on-input,
// which observes a key and lets it through, so the application received the line break before
// the session's text was injected and the dictated words landed one line too low. Every check
// below is on the two properties that fix it — the commit is reported, and the key is eaten.

// A composing session: editing enabled (which Settings::normalise() only permits under
// Cadence::EndOfSession) and the Win+Y chord pressed and released.
namespace {

void activate_composing(kd::InputFilter& f, uint64_t t = 100) {
    kd::EditingConfig cfg;
    cfg.enabled = true;
    f.set_editing(cfg);
    activate_win_y(f, t);
    release_win_y(f, t + 20);
}

}  // namespace

KD_TEST(hotkey, enter_commits_and_is_swallowed) {
    kd::InputFilter f;
    activate_composing(f);

    const kd::FilterOutcome o = f.on_event(kdown(kd::vk::kReturn, 400));
    KD_CHECK(o.commit);
    KD_CHECK(o.swallow);        // the whole point: the target never sees it
    KD_CHECK(!o.stop_on_input);  // "commit" is the specific signal; a generic stop loses why
    KD_CHECK(!o.cancel);

    // And the release goes the same way the press did, so the target is never handed half a
    // keystroke.
    const kd::FilterOutcome up = f.on_event(kup(kd::vk::kReturn, 420));
    KD_CHECK(up.swallow);
    KD_CHECK(!up.commit);
}

KD_TEST(hotkey, a_held_enter_commits_once_and_swallows_every_repeat) {
    kd::InputFilter f;
    activate_composing(f);

    KD_CHECK(f.on_event(kdown(kd::vk::kReturn, 400)).commit);
    for (uint64_t t = 430; t < 600; t += 30) {
        const kd::FilterOutcome o = f.on_event(kdown(kd::vk::kReturn, t));
        // One press, one commit — but the repeats are still eaten, because a target that
        // received only the *second* Enter of a held key is worse off than one that got none.
        KD_CHECK(!o.commit);
        KD_CHECK(o.swallow);
    }
}

KD_TEST(hotkey, only_an_unmodified_enter_commits) {
    // Ctrl+Enter, Shift+Enter and Alt+Enter mean something in the target — send-and-keep-open,
    // soft line break, properties — and are not this gesture. What happens to them instead
    // depends on whether the pill owns the keyboard, and both answers are checked because the
    // difference is exactly docs/editing.md §4.4's retraction.
    for (uint32_t mod : {kd::vk::kLControl, kd::vk::kLShift, kd::vk::kLMenu}) {
        // Composing: a modified Enter is a foreign key. Swallowed, hinted, and — the point of
        // §4.4 — it does **not** commit. An irreversible action must not fire from a key that
        // means something else.
        kd::InputFilter f;
        activate_composing(f);
        f.on_event(kdown(mod, 400));
        kd::FilterOutcome o = f.on_event(kdown(kd::vk::kReturn, 410));
        KD_CHECK(!o.commit);
        KD_CHECK(o.swallow);
        KD_CHECK(o.foreign_key);
        KD_CHECK(!o.stop_on_input);

        // Editing off: today's behaviour, unchanged. The key reaches the target and stop-on-input
        // ends the session.
        kd::InputFilter g;
        activate_win_y(g, 100);
        release_win_y(g, 120);
        g.on_event(kdown(mod, 400));
        o = g.on_event(kdown(kd::vk::kReturn, 410));
        KD_CHECK(!o.commit);
        KD_CHECK(!o.swallow);
        KD_CHECK(o.stop_on_input);
    }
}

KD_TEST(hotkey, enter_is_inert_while_no_session_is_running) {
    kd::InputFilter f;
    kd::EditingConfig cfg;
    cfg.enabled = true;
    f.set_editing(cfg);

    const kd::FilterOutcome o = f.on_event(kdown(kd::vk::kReturn, 50));
    KD_CHECK(!o.commit);
    KD_CHECK(!o.swallow);  // idle: Enter is entirely the target's business
}

KD_TEST(hotkey, with_editing_disabled_enter_keeps_todays_behaviour) {
    // docs/editing.md §6.1 and §16: `editing.enabled` is forced off unless the cadence is
    // end-of-session, and a user who has changed nothing is still on Cadence::Immediate. With the
    // buffer already in the document there is nothing for Enter to commit, so it must go on
    // meaning what it meant — a keystroke that ends the session and reaches the application.
    kd::InputFilter f;
    kd::EditingConfig cfg;
    cfg.enabled = false;
    f.set_editing(cfg);
    activate_win_y(f, 100);
    release_win_y(f, 120);

    const kd::FilterOutcome o = f.on_event(kdown(kd::vk::kReturn, 400));
    KD_CHECK(!o.commit);
    KD_CHECK(!o.swallow);
    KD_CHECK(o.stop_on_input);
}

KD_TEST(hotkey, the_commit_key_comes_from_configuration) {
    // Not a VK buried in a switch: docs/editing.md §12 makes it `editing.commit_key`, and a test
    // that only ever exercises the default cannot tell the two apart.
    kd::InputFilter f;
    kd::EditingConfig cfg;
    cfg.enabled = true;
    cfg.commit_vk = kd::vk::kF13;
    f.set_editing(cfg);
    activate_win_y(f, 100);
    release_win_y(f, 120);

    KD_CHECK(f.on_event(kdown(kd::vk::kF13, 400)).commit);
    KD_CHECK(!f.on_event(kdown(kd::vk::kReturn, 500)).commit);
}

KD_TEST(hotkey, foreign_key_action_round_trips_through_its_spelling) {
    kd::ForeignKeyAction a = kd::ForeignKeyAction::EndSession;
    KD_CHECK(kd::parse_foreign_key_action("ignore", a));
    KD_CHECK_EQ(static_cast<int>(a), static_cast<int>(kd::ForeignKeyAction::Ignore));
    KD_CHECK(kd::parse_foreign_key_action("end-session", a));
    KD_CHECK_EQ(static_cast<int>(a), static_cast<int>(kd::ForeignKeyAction::EndSession));
    KD_CHECK(!kd::parse_foreign_key_action("", a));
    KD_CHECK(!kd::parse_foreign_key_action("Ignore", a));  // the file's spelling is exact
    KD_CHECK_EQ(kd::to_string(kd::ForeignKeyAction::Ignore), "ignore");
    KD_CHECK_EQ(kd::to_string(kd::ForeignKeyAction::EndSession), "end-session");
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  The key classes — docs/editing.md §4.3
// ═══════════════════════════════════════════════════════════════════════════════════════════

namespace {

// Press `mods` then `vk`, with the UI thread answering, and hand back the outcome for the vk.
kd::FilterOutcome press(kd::InputFilter& f, uint32_t vk, std::initializer_list<uint32_t> mods = {},
                        uint64_t t = 400) {
    for (uint32_t m : mods) f.on_event(kdown(m, t++));
    return f.on_event(kdown(vk, t));
}

}  // namespace

KD_TEST(hotkey, the_editing_keys_are_classified_and_swallowed) {
    struct Case {
        uint32_t vk;
        std::initializer_list<uint32_t> mods;
        kd::EditOp op;
    };
    const Case cases[] = {
        {kd::vk::kBack, {}, kd::EditOp::Backspace},
        {kd::vk::kBack, {kd::vk::kLControl}, kd::EditOp::DeleteWordBack},
        {kd::vk::kDelete, {}, kd::EditOp::Delete},
        {kd::vk::kDelete, {kd::vk::kLControl}, kd::EditOp::DeleteWordForward},
        {kd::vk::kLeft, {}, kd::EditOp::MoveLeft},
        {kd::vk::kRight, {}, kd::EditOp::MoveRight},
        {kd::vk::kLeft, {kd::vk::kLControl}, kd::EditOp::MoveWordLeft},
        {kd::vk::kRight, {kd::vk::kLControl}, kd::EditOp::MoveWordRight},
        {kd::vk::kUp, {}, kd::EditOp::MoveUp},
        {kd::vk::kDown, {}, kd::EditOp::MoveDown},
        {kd::vk::kHome, {}, kd::EditOp::MoveLineStart},
        {kd::vk::kEnd, {}, kd::EditOp::MoveLineEnd},
        {kd::vk::kHome, {kd::vk::kLControl}, kd::EditOp::MoveDocStart},
        {kd::vk::kEnd, {kd::vk::kLControl}, kd::EditOp::MoveDocEnd},
        {kd::vk::kA, {kd::vk::kLControl}, kd::EditOp::SelectAll},
        {kd::vk::kC, {kd::vk::kLControl}, kd::EditOp::Copy},
        {kd::vk::kX, {kd::vk::kLControl}, kd::EditOp::Cut},
        {kd::vk::kZ, {kd::vk::kLControl}, kd::EditOp::Undo},
        {kd::vk::kA, {}, kd::EditOp::InsertText},
        {kd::vk::kSpace, {}, kd::EditOp::InsertText},
    };

    for (const Case& c : cases) {
        kd::InputFilter f;
        activate_composing(f);
        const kd::FilterOutcome o = press(f, c.vk, c.mods);
        KD_CHECK_EQ(kd::to_string(o.edit_op), kd::to_string(c.op));
        KD_CHECK(o.swallow);
        KD_CHECK(!o.stop_on_input);   // docs/editing.md §4.4: no stray key ends the session
        KD_CHECK(!o.commit);
    }
}

KD_TEST(hotkey, shift_extends_the_selection_rather_than_typing) {
    kd::InputFilter f;
    activate_composing(f);
    const kd::FilterOutcome o = press(f, kd::vk::kLeft, {kd::vk::kLShift});
    KD_CHECK_EQ(kd::to_string(o.edit_op), kd::to_string(kd::EditOp::MoveLeft));
    KD_CHECK(o.edit_shift);
}

KD_TEST(hotkey, a_printable_key_asks_the_ui_thread_for_its_character) {
    // docs/editing.md §4.2: the hook classifies, the UI thread resolves. Turning a virtual key
    // into a character means asking the keyboard layout, which the callback may not do — so what
    // travels is the key, the scan code, the modifiers and the caps state, and the answer is
    // produced where a layout call is legal.
    kd::InputFilter f;
    activate_composing(f);

    kd::InputEvent e = kdown(kd::vk::kA, 400);
    e.scan_code = 0x1E;
    e.caps_lock = true;
    const kd::FilterOutcome o = f.on_event(e);
    KD_CHECK(o.edit_needs_char);
    KD_CHECK_EQ(o.key_vk, kd::vk::kA);
    KD_CHECK_EQ(o.key_scan, uint32_t{0x1E});
    KD_CHECK(o.caps_lock);
}

KD_TEST(hotkey, an_edit_post_survives_the_round_trip_through_one_lparam) {
    kd::EditPost p;
    p.op = kd::EditOp::MoveWordRight;
    p.shift = true;
    p.caps_lock = true;
    p.needs_char = true;
    p.vk = 0xDE;
    p.scan = 0xABCD;
    p.mods = kd::kModCtrl | kd::kModShift;

    const kd::EditPost q = kd::unpack_edit_post(kd::pack_edit_post(p));
    KD_CHECK_EQ(kd::to_string(q.op), kd::to_string(p.op));
    KD_CHECK_EQ(q.shift, p.shift);
    KD_CHECK_EQ(q.caps_lock, p.caps_lock);
    KD_CHECK_EQ(q.needs_char, p.needs_char);
    KD_CHECK_EQ(q.vk, p.vk);
    KD_CHECK_EQ(q.scan, p.scan);
    KD_CHECK_EQ(q.mods, p.mods);
}

KD_TEST(hotkey, the_machine_keys_pass_straight_through) {
    // docs/editing.md §4.3's second Passthrough group: none of them can put a character
    // anywhere, so swallowing them is pure loss. Turning the volume down mid-dictation should
    // simply work.
    for (uint32_t v : {kd::vk::kVolumeUp, kd::vk::kVolumeDown, kd::vk::kVolumeMute,
                       kd::vk::kMediaPlayPause, kd::vk::kMediaNextTrack, kd::vk::kMediaPrevTrack,
                       kd::vk::kMediaStop, kd::vk::kPrintScreen, kd::vk::kNumLock,
                       kd::vk::kScrollLock, kd::vk::kCapsLock}) {
        kd::InputFilter f;
        activate_composing(f);
        const kd::FilterOutcome o = press(f, v);
        KD_CHECK(!o.swallow);
        KD_CHECK(!o.stop_on_input);
        KD_CHECK(!o.commit);
        KD_CHECK_EQ(kd::to_string(o.edit_op), kd::to_string(kd::EditOp::None));
    }
}

KD_TEST(hotkey, a_bare_modifier_expresses_no_intention) {
    // Swallowing a modifier's key-up is how "my Shift key is stuck on" bugs are born, and
    // hotkey.cpp already reasons this way for the binding key.
    for (uint32_t v : {kd::vk::kLShift, kd::vk::kRShift, kd::vk::kLControl, kd::vk::kRControl,
                       kd::vk::kLMenu, kd::vk::kRMenu, kd::vk::kLWin, kd::vk::kRWin}) {
        kd::InputFilter f;
        activate_composing(f);
        const kd::FilterOutcome o = press(f, v);
        KD_CHECK(!o.swallow);
        KD_CHECK(!o.commit);
        KD_CHECK_EQ(kd::to_string(o.edit_op), kd::to_string(kd::EditOp::None));
    }
}

KD_TEST(hotkey, a_foreign_key_is_swallowed_and_does_not_commit) {
    // docs/editing.md §4.4, and this is the retraction it records. An earlier revision had every
    // non-editing key commit, exactly as Enter does — right for a passive preview, and wrong the
    // moment the pill is something the user is operating: Ctrl+S from habit would type a
    // half-finished paragraph into the document and end the session. An irreversible action,
    // fired by a key that means something else, with no warning.
    kd::InputFilter f;
    activate_composing(f);
    const kd::FilterOutcome o = press(f, 0x53 /* S */, {kd::vk::kLControl});
    KD_CHECK(o.foreign_key);
    KD_CHECK(o.swallow);
    KD_CHECK(!o.commit);
    KD_CHECK(!o.stop_on_input);
    KD_CHECK_EQ(o.key_vk, uint32_t{0x53});
    KD_CHECK_EQ(o.key_mods, kd::kModCtrl);
}

KD_TEST(hotkey, end_session_restores_todays_behaviour_for_a_foreign_key) {
    // docs/editing.md §12: the setting is an enum and not a boolean precisely because the two
    // values name two different things that happen.
    kd::InputFilter f;
    kd::EditingConfig cfg;
    cfg.enabled = true;
    cfg.on_foreign_key = kd::ForeignKeyAction::EndSession;
    f.set_editing(cfg);
    activate_win_y(f, 100);
    release_win_y(f, 120);

    const kd::FilterOutcome o = press(f, 0x53 /* S */, {kd::vk::kLControl});
    KD_CHECK(!o.foreign_key);
    KD_CHECK(!o.swallow);
    KD_CHECK(o.stop_on_input);
}

KD_TEST(hotkey, an_editing_key_swallows_its_own_release) {
    kd::InputFilter f;
    activate_composing(f);
    KD_REQUIRE(f.on_event(kdown(kd::vk::kBack, 400)).swallow);
    KD_CHECK(f.on_event(kup(kd::vk::kBack, 420)).swallow);
    // Once only: the set is drained on the release, so a second release cannot swallow again.
    KD_CHECK(!f.on_event(kup(kd::vk::kBack, 440)).swallow);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  The two states, and the hotkey's one meaning — docs/editing.md §3.2, §4.5
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(hotkey, the_first_edit_key_closes_the_microphone_and_only_the_first) {
    kd::InputFilter f;
    activate_composing(f);
    KD_CHECK(!f.editing_active());

    const kd::FilterOutcome first = f.on_event(kdown(kd::vk::kLeft, 400));
    KD_CHECK(first.editing_began);
    KD_CHECK(f.editing_active());

    // Reported once, on the transition: closing an already-closed microphone is not a state
    // change the session machine should be told about.
    const kd::FilterOutcome second = f.on_event(kdown(kd::vk::kLeft, 450));
    KD_CHECK(!second.editing_began);
    KD_CHECK(f.editing_active());
}

KD_TEST(hotkey, the_hotkey_resumes_from_editing_and_does_nothing_while_dictating) {
    // docs/editing.md §4.5: the hotkey means "dictate", and nothing else, ever. No destructive
    // gesture is state-dependent — which is the objection §4.4 raises against Ctrl+S, applied to
    // a key the user presses far more often.
    kd::InputFilter f;
    activate_composing(f);

    // While dictating: nothing at all. Still swallowed — a target that received the chord's
    // second key would be worse off than one that saw nothing.
    kd::FilterOutcome o = f.on_event(kdown(kd::vk::kLWin, 300));
    o = f.on_event(kdown(kd::vk::kY, 310));
    KD_CHECK(o.swallow);
    KD_CHECK(!o.toggle_finalize);   // the toggle is gone while composing
    KD_CHECK(!o.resume_dictation);
    KD_CHECK(!o.commit);
    KD_CHECK(!o.cancel);
    release_win_y(f, 320);

    // Now edit, then press it again: it resumes.
    KD_REQUIRE(f.on_event(kdown(kd::vk::kBack, 400)).editing_began);
    f.on_event(kup(kd::vk::kBack, 410));
    f.on_event(kdown(kd::vk::kLWin, 500));
    o = f.on_event(kdown(kd::vk::kY, 510));
    KD_CHECK(o.resume_dictation);
    KD_CHECK(o.swallow);
    KD_CHECK(!o.toggle_finalize);
    KD_CHECK(!f.editing_active());
}

KD_TEST(hotkey, with_editing_off_the_hotkey_keeps_plan_1_8s_toggle) {
    // The cost §4.5 accepts is confined to sessions the compose buffer actually runs in.
    // `editing.enabled` is forced false for any cadence but end-of-session, so the muscle memory
    // survives everywhere the pill is still a passive preview — and session.cpp's
    // finalise-from-Running branch is live code rather than the dead branch §16 feared.
    kd::InputFilter f;
    activate_win_y(f, 100);
    release_win_y(f, 120);
    f.on_event(kdown(kd::vk::kLWin, 300));
    const kd::FilterOutcome o = f.on_event(kdown(kd::vk::kY, 310));
    KD_CHECK(o.toggle_finalize);
    KD_CHECK(!o.resume_dictation);
}

KD_TEST(hotkey, ending_a_session_leaves_no_editing_state_behind) {
    kd::InputFilter f;
    activate_composing(f);
    f.on_event(kdown(kd::vk::kBack, 400));
    KD_REQUIRE(f.editing_active());
    f.note_session_ended();
    KD_CHECK(!f.editing_active());
    KD_CHECK(!f.composing());
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Fail open — docs/editing.md §10.1
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(hotkey, a_wedged_ui_thread_gives_the_keyboard_back) {
    // The single most important property of the feature. The hook swallows keys on a thread that
    // stays healthy whatever happens to the UI thread; without this the user's keyboard is dead
    // in every application with no way out short of Ctrl+Alt+Del, which is strictly worse than
    // any failure this product can produce today.
    //
    // Testable at the filter level by injecting a stale stamp, without a hung thread — which is
    // the whole reason the comparison lives in the pure classifier.
    kd::InputFilter f;
    activate_composing(f, 100);

    // Answering: the pill has the keyboard.
    f.note_ui_heartbeat(500);
    KD_CHECK(f.on_event(kdown(kd::vk::kBack, 520)).swallow);
    KD_CHECK(f.composing());
    KD_CHECK(!f.failed_open());
    f.on_event(kup(kd::vk::kBack, 530));

    // Two seconds behind: it stops swallowing and stops posting.
    const kd::FilterOutcome o = f.on_event(kdown(kd::vk::kBack, 500 + 2001));
    KD_CHECK(f.failed_open());
    KD_CHECK(!f.composing());
    KD_CHECK(!o.swallow);
    KD_CHECK_EQ(kd::to_string(o.edit_op), kd::to_string(kd::EditOp::None));

    // Enter goes with it. Nothing may be swallowed on the pill's behalf once the consumer is
    // gone, including the one key that would otherwise commit.
    KD_CHECK(!f.on_event(kdown(kd::vk::kReturn, 500 + 2100)).swallow);
}

KD_TEST(hotkey, failing_open_is_latched_for_the_session_and_cleared_by_the_next_one) {
    // Latched: a UI thread that recovers mid-session would otherwise re-enter a mode the user has
    // already been thrown out of, halfway through whatever they typed to get out of it.
    kd::InputFilter f;
    activate_composing(f, 100);
    f.note_ui_heartbeat(500);
    f.on_event(kdown(kd::vk::kBack, 500 + 3000));
    KD_REQUIRE(f.failed_open());

    f.note_ui_heartbeat(500 + 3100);
    KD_CHECK(f.failed_open());
    KD_CHECK(!f.on_event(kdown(kd::vk::kLeft, 500 + 3200)).swallow);

    // A new session starts from a clean answer — §10.1 says "permanently, for that session",
    // which is only meaningful if the next one gets to try again.
    f.note_session_ended();
    activate_win_y(f, 500 + 4000);
    KD_CHECK(!f.failed_open());
    KD_CHECK(f.composing());
}

KD_TEST(hotkey, failing_open_records_how_far_behind_the_ui_thread_was) {
    // The latch used to trip in silence, and the log then showed a session the user ended with
    // Enter as `stop-on-input: keystroke` with no hint of why. deliver() posts this number to the
    // UI thread so the warning can name it; it is zero until the latch trips and zero again for
    // the next session.
    kd::InputFilter f;
    activate_composing(f, 100);
    f.note_ui_heartbeat(500);
    KD_CHECK_EQ(f.failed_open_behind_ms(), 0u);

    f.on_event(kdown(kd::vk::kBack, 500 + 3000));
    KD_REQUIRE(f.failed_open());
    KD_CHECK_EQ(f.failed_open_behind_ms(), 3000u);

    f.note_session_ended();
    KD_CHECK_EQ(f.failed_open_behind_ms(), 0u);
}

KD_TEST(hotkey, the_stale_check_survives_a_tick_count_wrap) {
    // Tick counts wrap every 49.7 days. An unsigned compare would declare the UI thread dead for
    // the whole second half of that cycle, which is the one bug this check must not have.
    kd::InputFilter f;
    activate_composing(f, 0xFFFFFF00u);
    f.note_ui_heartbeat(0xFFFFFF00u);
    // 0x100 ms later, having wrapped past 2^32.
    const kd::FilterOutcome o = f.on_event(kdown(kd::vk::kBack, 0x100000000ull));
    KD_CHECK(!f.failed_open());
    KD_CHECK(o.swallow);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  The mouse over the pill — docs/editing.md §5.2
// ═══════════════════════════════════════════════════════════════════════════════════════════

namespace {

// A composing session with a pill at [100,200 .. 400,300] in physical screen pixels.
void activate_with_pill(kd::InputFilter& f) {
    activate_composing(f);
    f.set_pill_rect(100, 200, 400, 300);
}

kd::InputEvent wheel_at(int32_t x, int32_t y, int32_t delta, uint64_t t = 400) {
    kd::InputEvent e = mouse_event(kd::InputEvent::Kind::MouseWheel, false, x, y, t);
    e.wheel_delta = delta;
    return e;
}

}  // namespace

KD_TEST(hotkey, a_click_inside_the_pill_places_a_caret_rather_than_ending_the_session) {
    kd::InputFilter f;
    activate_with_pill(f);

    const kd::FilterOutcome o = f.on_event(mdown(true, 150, 250, 400));
    KD_CHECK(o.pill_pointer);
    KD_CHECK_EQ(static_cast<int>(o.pill_kind),
                static_cast<int>(kd::FilterOutcome::PillPointerKind::Down));
    KD_CHECK(!o.stop_on_input);
    // Relative to the pill's top-left, because that is what the UI thread hit-tests against.
    KD_CHECK_EQ(o.pill_x, 50);
    KD_CHECK_EQ(o.pill_y, 50);
    // Not cached as an anchor either: the user clicked our window, not the document, so the
    // insertion point in the target has not moved.
    KD_CHECK(!o.click_captured);
}

KD_TEST(hotkey, a_click_outside_the_pill_still_ends_the_session) {
    kd::InputFilter f;
    activate_with_pill(f);

    const kd::FilterOutcome o = f.on_event(mdown(true, 500, 250, 400));
    KD_CHECK(!o.pill_pointer);
    KD_CHECK(o.stop_on_input);
    KD_CHECK_EQ(kd::to_string(o.stop_reason), "mouse_click");
    KD_CHECK(o.click_captured);
}

KD_TEST(hotkey, an_unpublished_rectangle_contains_nothing) {
    // docs/editing.md §16 names this as the `last_voice_ms` shape: a value the tests supply and
    // the product may not. If the UI thread never publishes the rectangle the slot reads zero,
    // every click is outside the pill, and the mouse reverts to today's behaviour — which is the
    // safe direction, and is pinned here so it stays deliberate rather than accidental.
    kd::InputFilter f;
    activate_composing(f);   // no set_pill_rect
    const kd::FilterOutcome o = f.on_event(mdown(true, 150, 250, 400));
    KD_CHECK(!o.pill_pointer);
    KD_CHECK(o.stop_on_input);
}

KD_TEST(hotkey, the_pill_rectangle_is_half_open_at_its_far_edges) {
    kd::InputFilter f;
    activate_with_pill(f);
    KD_CHECK(f.point_in_pill(100, 200));
    KD_CHECK(f.point_in_pill(399, 299));
    KD_CHECK(!f.point_in_pill(400, 250));
    KD_CHECK(!f.point_in_pill(250, 300));
    KD_CHECK(!f.point_in_pill(99, 250));
}

KD_TEST(hotkey, only_the_left_button_places_a_caret) {
    // A right-click opens a context menu and a middle-click pastes. Neither is a caret placement,
    // and both meant "the user has gone elsewhere" before the pill was clickable.
    kd::InputFilter f;
    activate_with_pill(f);
    const kd::FilterOutcome o = f.on_event(mdown(false, 150, 250, 400));
    KD_CHECK(!o.pill_pointer);
    KD_CHECK(o.stop_on_input);
}

KD_TEST(hotkey, a_drag_that_starts_in_the_pill_is_followed_out_of_it) {
    kd::InputFilter f;
    activate_with_pill(f);

    KD_REQUIRE(f.on_event(mdown(true, 150, 250, 400)).pill_pointer);

    kd::FilterOutcome o = f.on_event(mmove(300, 260, 410));
    KD_CHECK(o.pill_pointer);
    KD_CHECK_EQ(static_cast<int>(o.pill_kind),
                static_cast<int>(kd::FilterOutcome::PillPointerKind::Move));

    // Out of the rectangle, still dragging: a selection that started inside the pill is the
    // user's, and letting go anywhere ends it.
    o = f.on_event(mmove(900, 900, 420));
    KD_CHECK(o.pill_pointer);
    KD_CHECK(!o.stop_on_input);

    o = f.on_event(mup(true, 430));
    KD_CHECK(o.pill_pointer);
    KD_CHECK_EQ(static_cast<int>(o.pill_kind),
                static_cast<int>(kd::FilterOutcome::PillPointerKind::Up));

    // And once it is over, an ordinary move is an ordinary move again.
    KD_CHECK(!f.on_event(mmove(900, 900, 440)).pill_pointer);
}

KD_TEST(hotkey, a_move_with_no_drag_is_still_not_an_intention) {
    kd::InputFilter f;
    activate_with_pill(f);
    const kd::FilterOutcome o = f.on_event(mmove(150, 250, 400));
    KD_CHECK(!o.pill_pointer);
    KD_CHECK(!o.stop_on_input);   // dictating with a hand resting on the mouse must work
}

KD_TEST(hotkey, the_wheel_over_the_pill_scrolls_it_and_spares_the_anchor) {
    kd::InputFilter f;
    activate_with_pill(f);

    const kd::FilterOutcome o = f.on_event(wheel_at(150, 250, 120));
    KD_CHECK(o.pill_pointer);
    KD_CHECK_EQ(static_cast<int>(o.pill_kind),
                static_cast<int>(kd::FilterOutcome::PillPointerKind::Scroll));
    KD_CHECK_EQ(o.pill_wheel, 120);
    // The user scrolled *our* window, so the target's caret has not moved and the cached click
    // is still where the insertion point is.
    KD_CHECK(!o.click_invalidated);
    KD_CHECK(!o.stop_on_input);
}

KD_TEST(hotkey, the_wheel_elsewhere_still_invalidates_the_cached_click) {
    kd::InputFilter f;
    activate_with_pill(f);
    const kd::FilterOutcome o = f.on_event(wheel_at(900, 900, 120));
    KD_CHECK(!o.pill_pointer);
    KD_CHECK(o.click_invalidated);
    KD_CHECK(!o.stop_on_input);
}

KD_TEST(hotkey, the_mouse_setting_turns_the_clickable_pill_off) {
    // docs/editing.md §12's `editing.mouse`. With it off the keyboard half is unaffected and the
    // mouse behaves exactly as it did before the pill was clickable — which is also what the
    // overlay does, because main.cpp leaves WS_EX_TRANSPARENT set in that case.
    kd::InputFilter f;
    kd::EditingConfig cfg;
    cfg.enabled = true;
    cfg.mouse = false;
    f.set_editing(cfg);
    activate_win_y(f, 100);
    release_win_y(f, 120);
    f.set_pill_rect(100, 200, 400, 300);

    const kd::FilterOutcome o = f.on_event(mdown(true, 150, 250, 400));
    KD_CHECK(!o.pill_pointer);
    KD_CHECK(o.stop_on_input);
    KD_CHECK(f.on_event(kdown(kd::vk::kBack, 500)).swallow);  // the keyboard still works
}

KD_TEST(hotkey, failing_open_gives_the_mouse_back_too) {
    // §10.1's latch is about the pill's claim on input, not specifically about keys. A UI thread
    // that will never process a caret placement must not be the reason a click stops meaning
    // "the user has gone elsewhere".
    kd::InputFilter f;
    activate_with_pill(f);
    f.note_ui_heartbeat(500);
    f.on_event(kdown(kd::vk::kBack, 500 + 3000));
    KD_REQUIRE(f.failed_open());

    const kd::FilterOutcome o = f.on_event(mdown(true, 150, 250, 500 + 3100));
    KD_CHECK(!o.pill_pointer);
    KD_CHECK(o.stop_on_input);
}

KD_TEST(hotkey, a_pill_post_survives_the_round_trip_through_one_lparam) {
    kd::PillPost p;
    p.kind = kd::FilterOutcome::PillPointerKind::Move;
    p.x = -1234;
    p.y = 4321;
    p.wheel = -240;
    const kd::PillPost q = kd::unpack_pill_post(kd::pack_pill_post(p));
    KD_CHECK_EQ(static_cast<int>(q.kind), static_cast<int>(p.kind));
    KD_CHECK_EQ(q.x, p.x);
    KD_CHECK_EQ(q.y, p.y);
    KD_CHECK_EQ(q.wheel, p.wheel);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Anchor source B (plan.md §1.5)
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(hotkey, only_the_left_button_press_updates_the_anchor_cache) {
    kd::InputFilter f;

    // Left press: cached. This is the "last left-click before the hotkey" of plan.md §1.5.
    KD_CHECK(f.on_event(mdown(true, 10, 20, 100)).click_captured);

    // Right and middle look identical to the filter and neither is cached: a right-click opens a
    // context menu and a middle-click pastes on X11, so neither locates an insertion point.
    KD_CHECK(!f.on_event(mdown(false, 30, 40, 110)).click_captured);

    // The release is not a second click.
    KD_CHECK(!f.on_event(mup(true, 120)).click_captured);

    // Moving is not clicking, and must never stop a session either.
    const kd::FilterOutcome o = f.on_event(mmove(50, 60, 130));
    KD_CHECK(!o.click_captured);
    KD_CHECK(!o.stop_on_input);
}

KD_TEST(hotkey, the_wheel_invalidates_the_cached_click_without_stopping_the_session) {
    kd::InputFilter f;
    KD_CHECK(f.on_event(mdown(true, 400, 300, 100)).click_captured);

    kd::FilterOutcome o = f.on_event(wheel(110));
    KD_CHECK(o.click_invalidated);  // scrolling moves the caret with no click
    KD_CHECK(!o.click_captured);
    KD_CHECK(!o.stop_on_input);

    // Same during a session: a wheel is neither a click nor a keystroke, and scrolling back to
    // re-read what you dictated is a normal thing to do mid-session.
    activate_win_y(f, 200);
    release_win_y(f, 220);
    o = f.on_event(wheel(300));
    KD_CHECK(o.click_invalidated);
    KD_CHECK(!o.stop_on_input);
    KD_CHECK(f.session_active());
}

KD_TEST(hotkey, any_click_stops_a_running_session_and_a_left_one_is_still_cached) {
    kd::InputFilter f;
    activate_win_y(f, 100);
    release_win_y(f, 120);

    // Right-click: stops, not cached.
    kd::FilterOutcome o = f.on_event(mdown(false, 1, 2, 300));
    KD_CHECK(o.stop_on_input);
    KD_CHECK_EQ(kd::to_string(o.stop_reason), "mouse_click");
    KD_CHECK(!o.click_captured);

    f.note_session_ended();
    activate_win_y(f, 400);
    release_win_y(f, 420);

    // Left click: stops AND is cached — the user clicked somewhere, so that is where the next
    // session's anchor should start from.
    o = f.on_event(mdown(true, 7, 8, 500));
    KD_CHECK(o.stop_on_input);
    KD_CHECK(o.click_captured);
}

KD_TEST(hotkey, injected_mouse_events_neither_stop_nor_cache) {
    // LLMHF_INJECTED — our own clipboard-paste chord and any accessibility tool. plan.md §1.8:
    // "Our own SendInput traffic must never stop the session producing it."
    kd::InputFilter f;
    activate_win_y(f, 100);
    release_win_y(f, 120);

    InputEvent e = mdown(true, 11, 12, 300);
    e.injected = true;
    const kd::FilterOutcome o = f.on_event(e);

    KD_CHECK(!o.stop_on_input);
    KD_CHECK(!o.click_captured);
    KD_CHECK(f.session_active());
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Push-to-talk: the release finalises (HotkeySpec::push_to_talk)
// ═══════════════════════════════════════════════════════════════════════════════════════════

namespace {

// The default binding with the gesture flipped. Returned by value so each test owns its spec.
kd::HotkeySpec ptt_win_y() {
    kd::HotkeySpec s{};
    s.push_to_talk = true;
    return s;
}

kd::HotkeySpec ptt_f13() {
    kd::HotkeySpec s{};
    s.vk = kd::vk::kF13;
    s.mods = kd::kModNone;
    s.push_to_talk = true;
    return s;
}

}  // namespace

KD_TEST(hotkey, push_to_talk_is_off_by_default_and_survives_a_display_string_round_trip) {
    // The spelling names the keys and nothing else: a mode carried in the string would make
    // parse() and to_display_string() stop being inverses.
    const kd::HotkeySpec fresh{};
    KD_CHECK(!fresh.push_to_talk);

    kd::HotkeySpec parsed{};
    KD_REQUIRE(kd::HotkeySpec::parse(ptt_win_y().to_display_string(), parsed));
    KD_CHECK_EQ(parsed.vk, kd::vk::kY);
    KD_CHECK(!parsed.push_to_talk);
}

KD_TEST(hotkey, holding_the_binding_starts_and_releasing_it_finalises) {
    kd::InputFilter f;
    f.set_hotkey(ptt_win_y());

    f.on_event(kdown(kd::vk::kLWin, 100));
    kd::FilterOutcome o = f.on_event(kdown(kd::vk::kY, 110));
    KD_REQUIRE(o.activate);
    KD_CHECK(!o.toggle_finalize);
    KD_CHECK(o.swallow);

    // Still held: the session is running and the chord's own repeats change nothing.
    o = f.on_event(kdown(kd::vk::kY, 200));
    KD_CHECK(!o.activate);
    KD_CHECK(!o.toggle_finalize);
    KD_CHECK(o.swallow);
    KD_CHECK(f.session_active());

    // Let go. This is the same finalise the second press produces under the toggle — `stop`, a
    // last decode, and the text typed into the target.
    o = f.on_event(kup(kd::vk::kY, 900));
    KD_CHECK(o.toggle_finalize);
    KD_CHECK(o.swallow);        // still our key: its press was swallowed, so its release is too
    KD_CHECK(!o.stop_on_input);
    KD_CHECK(!o.activate);
}

KD_TEST(hotkey, releasing_the_modifier_first_also_finalises_and_only_once) {
    // Win+Y released Win-first. Somebody who has let go of the Win key has stopped holding the
    // hotkey, and a gesture that waited for the Y would leave them talking into a dead session.
    kd::InputFilter f;
    f.set_hotkey(ptt_win_y());
    f.on_event(kdown(kd::vk::kLWin, 100));
    KD_REQUIRE(f.on_event(kdown(kd::vk::kY, 110)).activate);

    kd::FilterOutcome o = f.on_event(kup(kd::vk::kLWin, 800));
    KD_CHECK(o.toggle_finalize);
    KD_CHECK(!o.stop_on_input);

    // The Y coming up a few milliseconds later must not finalise a second session.
    o = f.on_event(kup(kd::vk::kY, 810));
    KD_CHECK(!o.toggle_finalize);
    KD_CHECK(o.swallow);
}

KD_TEST(hotkey, a_modifier_free_binding_finalises_on_its_own_release) {
    kd::InputFilter f;
    f.set_hotkey(ptt_f13());

    kd::FilterOutcome o = f.on_event(kdown(kd::vk::kF13, 100));
    KD_REQUIRE(o.activate);
    KD_CHECK(!o.suppress_start_menu);  // no Win key, no hack — the point of offering F13–F24

    o = f.on_event(kup(kd::vk::kF13, 700));
    KD_CHECK(o.toggle_finalize);
    KD_CHECK(o.swallow);
}

KD_TEST(hotkey, under_push_to_talk_a_press_never_finalises) {
    // The whole difference between the two gestures, stated as one property: with the binding
    // held, every further press of it — repeat, or a fresh one after the owner started a session
    // some other way — leaves the session running.
    kd::InputFilter f;
    f.set_hotkey(ptt_f13());
    f.note_session_started(50);  // as the tray menu would

    kd::FilterOutcome o = f.on_event(kdown(kd::vk::kF13, 100));
    KD_CHECK(!o.toggle_finalize);
    KD_CHECK(!o.activate);
    KD_CHECK(o.swallow);
    KD_CHECK(f.session_active());

    // ...and the release ends it, because holding it is what the user was told to do.
    KD_CHECK(f.on_event(kup(kd::vk::kF13, 600)).toggle_finalize);
}

KD_TEST(hotkey, the_release_is_inert_once_the_session_has_ended_some_other_way) {
    // Esc, a click, or the silence timer ends the session while the key is still down. The
    // release must then mean nothing: finalising here would, once the next session has started,
    // end one the user only just began.
    kd::InputFilter f;
    f.set_hotkey(ptt_f13());
    KD_REQUIRE(f.on_event(kdown(kd::vk::kF13, 100)).activate);

    KD_REQUIRE(f.on_event(kdown(kd::vk::kEscape, 200)).cancel);
    f.note_session_ended();

    const kd::FilterOutcome o = f.on_event(kup(kd::vk::kF13, 300));
    KD_CHECK(!o.toggle_finalize);
    KD_CHECK(o.swallow);  // its press was swallowed; its release still has to be
}

KD_TEST(hotkey, rebinding_mid_hold_does_not_finalise_on_the_stale_release) {
    // The settings dialog closed while the binding was held. The key going up now belongs to a
    // binding that no longer exists; the user changed a setting, they did not finish dictating.
    kd::InputFilter f;
    f.set_hotkey(ptt_f13());
    KD_REQUIRE(f.on_event(kdown(kd::vk::kF13, 100)).activate);

    f.set_hotkey(ptt_win_y());
    const kd::FilterOutcome o = f.on_event(kup(kd::vk::kF13, 200));
    KD_CHECK(!o.toggle_finalize);
}

KD_TEST(hotkey, push_to_talk_still_ends_the_session_on_a_foreign_keystroke) {
    // plan.md §1.8 is unchanged: the gesture that *finishes* moved, and nothing else did.
    kd::InputFilter f;
    f.set_hotkey(ptt_f13());
    KD_REQUIRE(f.on_event(kdown(kd::vk::kF13, 100)).activate);

    const kd::FilterOutcome o = f.on_event(kdown(kd::vk::kA, 200));
    KD_CHECK(o.stop_on_input);
    KD_CHECK_EQ(static_cast<int>(o.stop_reason), static_cast<int>(kd::StopInputReason::Keystroke));
    KD_CHECK(!o.swallow);
}

KD_TEST(hotkey, an_edit_key_closes_the_microphone_and_the_release_still_finalises) {
    // Editing is reachable under push-to-talk — an edit key pressed with the other hand closes
    // the microphone while the binding is still held — and it must not change what letting go
    // means. Resuming from editing is not: the only way to press the binding again is to release
    // it first, and that is the finalise.
    kd::InputFilter f;
    kd::EditingConfig cfg;
    cfg.enabled = true;
    f.set_editing(cfg);
    f.set_hotkey(ptt_f13());
    KD_REQUIRE(f.on_event(kdown(kd::vk::kF13, 100)).activate);

    const kd::FilterOutcome edit = f.on_event(kdown(kd::vk::kBack, 200));
    KD_CHECK(edit.editing_began);
    KD_CHECK(edit.swallow);
    KD_CHECK(f.editing_active());
    KD_CHECK(f.session_active());

    const kd::FilterOutcome o = f.on_event(kup(kd::vk::kF13, 400));
    KD_CHECK(o.toggle_finalize);
    KD_CHECK(o.swallow);
}
