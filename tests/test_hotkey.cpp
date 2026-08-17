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

KD_TEST(hotkey, second_chord_press_finalises_rather_than_activating_again) {
    kd::InputFilter f;
    activate_win_y(f, 100);
    release_win_y(f, 120);

    f.on_event(kdown(kd::vk::kLWin, 500));
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
    // Not swallowed — a judgement call documented in hotkey.cpp, not something plan.md settles.
    KD_CHECK(!o.swallow);
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
