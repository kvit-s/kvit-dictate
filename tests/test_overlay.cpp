// tests/test_overlay.cpp — the overlay's one pure decision.
//
// Everything else in src/overlay.cpp is Direct2D, DirectWrite and a layered window, and is
// verified by looking at it. `overlay_mode_for` is not: it is plan.md §1.5's deterministic
// degradation rule — "below a confidence threshold the overlay degrades deterministically:
// floating pill near the target, or no preview at all, configurable. **This is a defined state,
// not a bug**" — and it is a decision, so it is testable and belongs here.
//
// It is worth saying why this file exists at all. Until it did, the rule was implemented
// *nowhere*. `AnchorConfig::min_confidence` and `below_threshold` parsed, validated and
// round-tripped through settings.json; `Anchor::is_usable()` was written and unit-tested;
// `AnchorEstimator::current_fallback()` was written and documented as "consulted when current()
// is absent or fails is_usable(cfg.min_confidence)"; and no line in src/ called any of the
// three. src/main.cpp handed every anchor to Overlay::set_anchor() and the overlay drew it
// inline, whatever confidence the estimator had attached to it.
//
// It surfaced from a measurement rather than from reading the code: A09/S07 (docs/
// supported-targets.md §5.9.2) produced an anchor 48 line heights from the truth which the
// estimator had *correctly* scored at 0.4953 — below the 0.5 default — and the product would
// have drawn the preview at it regardless. The scoring was doing its job; nothing was listening.
//
// ⚠ These tests cover a function `src/main.cpp` does NOT yet call. That is deliberate, and it is
// recorded in three other places — `App::apply_overlay_mode`, the declaration in overlay.h, and
// docs/supported-targets.md §9 question 7 — because "tested but uncalled" is the exact shape of the
// twenty defects this project has already had, and it must not be mistaken for another one.
//
// The gate was wired and then unwired on evidence. The rule below is right; what is wrong is
// `AnchorConfig::min_confidence`'s default of 0.50, which sits **above the range `score_diff`
// actually produces**, so enforcing it would degrade every commit on every target. The measured
// A09 row (§5.9.2.1) separates cleanly — accurate estimates 0.3742–0.5000, inaccurate ones
// 0.1858–0.2941 — so any threshold in (0.2941, 0.3742] classifies all ten cells correctly. The
// scorer discriminates; picking that number on one application's evidence is what is deferred.
//
// Note the two cases below that quote **0.4953**: that is A09/S07's confidence from the sweep that
// scored against no incumbent anchor. It is kept as a test input because it is a real number the
// function must handle, not because it is still the published figure for that cell — see §5.9.4.

#include "kd_test.h"

#include "anchor.h"
#include "overlay.h"
#include "util/utf8.h"

#include <optional>
#include <string>

namespace {

using kd::OverlayMode;
using kd::overlay_mode_for;
using kd::anchor::Anchor;
using kd::anchor::AnchorConfig;
using kd::anchor::Fallback;
using kd::anchor::Source;

Anchor anchor_at(float confidence, Source source = Source::CommitDiff) {
    Anchor a;
    a.insertion_point = kd::anchor::Point{100, 200};
    a.line_height = 15;
    a.confidence = confidence;
    a.source = source;
    a.timestamp_ms = 1000;
    return a;
}

}  // namespace

KD_TEST(overlay, a_confident_anchor_keeps_the_configured_inline_mode) {
    AnchorConfig cfg;
    KD_CHECK_EQ(static_cast<int>(overlay_mode_for(OverlayMode::Inline,
                                                  anchor_at(kd::anchor::kCaretConfidence), cfg)),
                static_cast<int>(OverlayMode::Inline));

    // Exactly at the threshold counts as usable — is_usable() is `>=`, and the boundary is the
    // one value a reader will assume goes the other way.
    KD_CHECK_EQ(static_cast<int>(
                    overlay_mode_for(OverlayMode::Inline, anchor_at(cfg.min_confidence), cfg)),
                static_cast<int>(OverlayMode::Inline));
}

KD_TEST(overlay, a_low_confidence_anchor_degrades_to_the_configured_fallback) {
    AnchorConfig cfg;
    cfg.below_threshold = Fallback::FloatingPill;

    // 0.4953 is not an invented number: it is what score_diff returned for A09/S07, the wrap that
    // moved the true insertion point outside the captured region. This is the case the whole
    // mechanism exists for, and the case that was silently drawn inline.
    KD_CHECK_EQ(static_cast<int>(overlay_mode_for(OverlayMode::Inline, anchor_at(0.4953f), cfg)),
                static_cast<int>(OverlayMode::FloatingPill));

    cfg.below_threshold = Fallback::NoPreview;
    // StatusOnly, not Hidden. The fallback is named "no *preview*"; Hidden suppresses the
    // Unsupported and Warning states too, which §1.1 of the supported-targets doc requires be
    // surfaced before the user speaks.
    KD_CHECK_EQ(static_cast<int>(overlay_mode_for(OverlayMode::Inline, anchor_at(0.4953f), cfg)),
                static_cast<int>(OverlayMode::StatusOnly));
}

KD_TEST(overlay, no_anchor_at_all_degrades_rather_than_drawing_inline) {
    // Session start, and every target that has never produced one. anchor.h's contract for
    // current_fallback() names this case first: "consulted only when current() is absent".
    AnchorConfig cfg;
    KD_CHECK_EQ(static_cast<int>(overlay_mode_for(OverlayMode::Inline, std::nullopt, cfg)),
                static_cast<int>(OverlayMode::FloatingPill));
}

KD_TEST(overlay, a_sourceless_anchor_degrades_however_high_its_confidence) {
    // is_usable() requires both a source and the threshold. A confidence field carrying 1.0 with
    // Source::None is not a usable anchor, and reading only the float would draw at whatever
    // insertion_point happened to hold.
    AnchorConfig cfg;
    KD_CHECK_EQ(
        static_cast<int>(overlay_mode_for(OverlayMode::Inline, anchor_at(1.0f, Source::None), cfg)),
        static_cast<int>(OverlayMode::FloatingPill));
}

KD_TEST(overlay, reset_confidence_takes_the_overlay_out_of_inline) {
    // anchor.h §7.4: a `seq` gap means a hypothesis was missed, so "the change on screen was
    // produced by text we injected" is no longer sound. reset_confidence() zeroes the confidence
    // and deliberately KEEPS the anchor, because a stale position beats no position — which only
    // means anything if something downstream reads the confidence. This is that something.
    AnchorConfig cfg;
    KD_CHECK_EQ(static_cast<int>(overlay_mode_for(OverlayMode::Inline, anchor_at(0.0f), cfg)),
                static_cast<int>(OverlayMode::FloatingPill));
}

KD_TEST(overlay, degrading_is_the_only_direction_this_moves_in) {
    // A user who chose the pill, the status indicator or nothing is never *upgraded* to inline by
    // a confident anchor. settings.overlay_mode is a preference, not a starting bid.
    AnchorConfig cfg;
    const Anchor good = anchor_at(kd::anchor::kCaretConfidence, Source::Caret);
    for (const OverlayMode m :
         {OverlayMode::Hidden, OverlayMode::FloatingPill, OverlayMode::StatusOnly}) {
        KD_CHECK_EQ(static_cast<int>(overlay_mode_for(m, good, cfg)), static_cast<int>(m));
        KD_CHECK_EQ(static_cast<int>(overlay_mode_for(m, std::nullopt, cfg)), static_cast<int>(m));
    }
}

KD_TEST(overlay, the_threshold_is_read_from_the_config_not_hardcoded) {
    // settings.anchor.min_confidence has to actually do something, which was the whole defect.
    AnchorConfig strict;
    strict.min_confidence = 1.0f;
    KD_CHECK_EQ(static_cast<int>(overlay_mode_for(OverlayMode::Inline,
                                                  anchor_at(kd::anchor::kCaretConfidence), strict)),
                static_cast<int>(OverlayMode::FloatingPill));

    AnchorConfig lax;
    lax.min_confidence = 0.1f;
    KD_CHECK_EQ(static_cast<int>(overlay_mode_for(OverlayMode::Inline, anchor_at(0.2f), lax)),
                static_cast<int>(OverlayMode::Inline));
}

// ═══ layout_overlay — the screen-edge margin ════════════════════════════════════════════════
//
// From the first human run against a real model. The report was "the pill is all the way down,
// mostly below screen", and the placement log said why::
//
//     overlay placed: mode=FloatingPill rect=[1906,2135 28x19] work=[0,0 3840x2160] dpi=96
//
// 19 px tall, because a finalize-only session has no preview text and the pill is a bare status
// dot; bottom edge at 2154 on a 2160-pixel monitor, i.e. **six pixels of clearance**, which is
// inside the strip an auto-hide taskbar reveals itself in. The margin was `gap * 2` and `gap`
// derives from the overlay's own height, so the smaller the pill the closer to the edge it went —
// exactly backwards.

namespace {

kd::anchor::Anchor plain_anchor() {
    Anchor a;
    a.insertion_point = kd::anchor::Point{400, 400};
    a.line_height = 15;
    a.confidence = 0.9f;
    a.source = Source::Caret;
    return a;
}

// The reporter's monitor.
constexpr kd::anchor::Rect k4k{0, 0, 3840, 2160};

}  // namespace

KD_TEST(overlay, a_tiny_pill_still_clears_the_screen_edge) {
    // The exact case that was reported: a 28x19 status dot on a 4K monitor.
    const kd::anchor::Rect r =
        kd::layout_overlay(plain_anchor(), 28, 19, k4k, OverlayMode::FloatingPill);
    KD_REQUIRE(!r.empty());

    // Comfortably clear of the edge rather than technically inside it. The old behaviour put the
    // bottom at 2154; anything under ~40 px of clearance is inside auto-hide taskbar territory.
    const int32_t clearance = k4k.bottom - r.bottom;
    KD_CHECK(clearance >= 40);
    KD_CHECK(r.bottom <= k4k.bottom);   // still on the monitor
    KD_CHECK(r.top >= k4k.top);
}

KD_TEST(overlay, the_edge_margin_does_not_shrink_with_the_overlay) {
    // The defect in one assertion: a shorter overlay must not sit closer to the edge. Deriving
    // the margin from text_height_px is what made it do so.
    const kd::anchor::Rect small =
        kd::layout_overlay(plain_anchor(), 28, 19, k4k, OverlayMode::FloatingPill);
    const kd::anchor::Rect large =
        kd::layout_overlay(plain_anchor(), 600, 120, k4k, OverlayMode::FloatingPill);
    KD_CHECK_EQ(k4k.bottom - small.bottom, k4k.bottom - large.bottom);
}

KD_TEST(overlay, the_edge_margin_scales_with_the_monitor_not_the_dpi_argument) {
    // layout_overlay is given no DPI on purpose. The margin still has to be bigger on a bigger
    // display, which it gets from the work area — physical pixels, so a denser monitor has
    // proportionally more of them.
    constexpr kd::anchor::Rect k1080{0, 0, 1920, 1080};
    const int32_t c4k =
        k4k.bottom - kd::layout_overlay(plain_anchor(), 28, 19, k4k, OverlayMode::FloatingPill).bottom;
    const int32_t c1080 =
        k1080.bottom -
        kd::layout_overlay(plain_anchor(), 28, 19, k1080, OverlayMode::FloatingPill).bottom;
    KD_CHECK(c4k > c1080);
    KD_CHECK(c1080 >= 20);  // still clear of the edge on a smaller screen
}

KD_TEST(overlay, the_status_indicator_clears_two_edges) {
    // StatusOnly is a corner indicator, so it has to clear the right edge as well as the bottom.
    const kd::anchor::Rect r =
        kd::layout_overlay(plain_anchor(), 28, 19, k4k, OverlayMode::StatusOnly);
    KD_CHECK(k4k.bottom - r.bottom >= 40);
    KD_CHECK(k4k.right - r.right >= 40);
}

KD_TEST(overlay, an_unknown_work_area_still_produces_a_rect) {
    // No monitor info: the final clamp is skipped entirely, which is the one path by which the
    // pill can genuinely leave the screen. It must still return something placeable rather than
    // an empty rect, and the placement log now records that this happened.
    const kd::anchor::Rect none{};
    const kd::anchor::Rect r =
        kd::layout_overlay(plain_anchor(), 28, 19, none, OverlayMode::FloatingPill);
    KD_CHECK(!r.empty());
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  clamp_scroll_line — docs/editing.md §7.3
// ═══════════════════════════════════════════════════════════════════════════════════════════
//
// Scrollback is a prerequisite, not a nicety: the pill shows the newest eight lines with
// front-elision, so a session-long buffer sits behind that window unseen, and
// editable-but-unreachable text is not a feature but a trap. The off-by-one at each end is the
// kind of thing that is obvious in a table of cases and invisible inside a paint function, which
// is why the arithmetic is out here.

KD_TEST(overlay, everything_that_fits_starts_at_the_top) {
    KD_CHECK_EQ(kd::clamp_scroll_line(0, 0, false, 3, 8), uint32_t{0});
    KD_CHECK_EQ(kd::clamp_scroll_line(5, 0, false, 3, 8), uint32_t{0});
    KD_CHECK_EQ(kd::clamp_scroll_line(0, 2, true, 8, 8), uint32_t{0});
    // A visible count of zero has no valid non-zero start, whatever was asked for.
    KD_CHECK_EQ(kd::clamp_scroll_line(4, 4, true, 40, 0), uint32_t{0});
}

KD_TEST(overlay, the_requested_start_is_clamped_to_the_last_full_screen) {
    // A buffer that shrank under a stale offset must not leave the pill showing blank space
    // below its own text.
    KD_CHECK_EQ(kd::clamp_scroll_line(0, 0, false, 40, 8), uint32_t{0});
    KD_CHECK_EQ(kd::clamp_scroll_line(10, 0, false, 40, 8), uint32_t{10});
    KD_CHECK_EQ(kd::clamp_scroll_line(32, 0, false, 40, 8), uint32_t{32});
    KD_CHECK_EQ(kd::clamp_scroll_line(33, 0, false, 40, 8), uint32_t{32});
    KD_CHECK_EQ(kd::clamp_scroll_line(9999, 0, false, 40, 8), uint32_t{32});
}

KD_TEST(overlay, any_caret_movement_scrolls_the_caret_into_view) {
    // The rule that makes the arrow keys able to reach the top of a long buffer at all. It is
    // applied *after* the clamp rather than before it, because it overrides the requested
    // position and not the other way round.
    KD_CHECK_EQ(kd::clamp_scroll_line(30, 2, true, 40, 8), uint32_t{2});   // caret above: scroll back
    KD_CHECK_EQ(kd::clamp_scroll_line(0, 20, true, 40, 8), uint32_t{13});  // caret below: caret on the last line
    KD_CHECK_EQ(kd::clamp_scroll_line(10, 12, true, 40, 8), uint32_t{10}); // already visible: left alone
    KD_CHECK_EQ(kd::clamp_scroll_line(10, 17, true, 40, 8), uint32_t{10}); // the last visible line
    KD_CHECK_EQ(kd::clamp_scroll_line(10, 18, true, 40, 8), uint32_t{11}); // one past it
}

KD_TEST(overlay, the_caret_on_the_last_line_pins_the_view_to_the_bottom) {
    KD_CHECK_EQ(kd::clamp_scroll_line(0, 39, true, 40, 8), uint32_t{32});
    // ...and never past it, however far the caret is claimed to be.
    KD_CHECK_EQ(kd::clamp_scroll_line(0, 500, true, 40, 8), uint32_t{32});
}

KD_TEST(overlay, only_a_caret_the_user_moved_unpins_the_view) {
    // The bug this closes, in one line: during dictation the caret moves once per utterance,
    // because EditBuffer::append leaves it at the end of the text, and App::push_edit_view sends
    // that caret to the overlay with `active` false. Unpinning on it stopped the view following
    // the text — a long dictation grew to eight lines and then froze there while the newest
    // words accumulated off the bottom, unreachable and unseen.
    //
    // The other half of why it froze rather than merely lagged: with `active` false paint_once()
    // never hit-tests the caret, so `caret_known` is false and clamp_scroll_line's rescue — the
    // rule directly above — cannot fire. An unpinned view with no known caret has nothing left
    // to move it.
    KD_CHECK(!kd::caret_move_unpins_scroll(false, true));   // speech appended; the view stays pinned
    KD_CHECK(kd::caret_move_unpins_scroll(true, true));     // the user arrowed away; it does not
    KD_CHECK(!kd::caret_move_unpins_scroll(true, false));   // a selection change is not a move
    KD_CHECK(!kd::caret_move_unpins_scroll(false, false));
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  key_legend — docs/editing.md §7.5
// ═══════════════════════════════════════════════════════════════════════════════════════════
//
// The pill's standing line of key names. It is tested for one property above all: **it may not
// name a key that does nothing in the state it is shown in.** A legend that is merely decorative
// is harmless; a legend that promises Enter will finish, in a session where Enter is an ordinary
// keystroke that ends the session and lands in the document, teaches the user something false
// about a destructive key. The two session shapes — docs/editing.md's compose buffer and
// plan.md §1.8's stop-on-input — disagree about exactly that, which is why `composing` is a
// parameter rather than an assumption.

namespace {

using kd::OverlayState;
using kd::key_legend;

bool mentions(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

KD_TEST(overlay, the_legend_is_silent_where_there_is_no_session_to_steer) {
    // Finalizing is the one worth stating: `stop` has gone out, and Enter, Esc and the hotkey no
    // longer do what a legend drawn there would have just promised.
    KD_CHECK(key_legend(OverlayState::Finalizing, true, "Win+Y").empty());
    KD_CHECK(key_legend(OverlayState::Idle, true, "Win+Y").empty());
    KD_CHECK(key_legend(OverlayState::ModelLoading, true, "Win+Y").empty());
    KD_CHECK(key_legend(OverlayState::Warning, true, "Win+Y").empty());
    KD_CHECK(key_legend(OverlayState::Unsupported, true, "Win+Y").empty());
    KD_CHECK(key_legend(OverlayState::Error, true, "Win+Y").empty());
}

KD_TEST(overlay, a_composing_session_names_enter_and_esc_in_every_state_it_shows) {
    for (OverlayState s : {OverlayState::Listening, OverlayState::Speaking,
                           OverlayState::Editing}) {
        const std::string line = key_legend(s, true, "Win+Y");
        KD_CHECK(mentions(line, "Enter to finish"));
        KD_CHECK(mentions(line, "Esc to cancel"));
    }
}

KD_TEST(overlay, the_hotkey_is_named_only_where_it_does_something) {
    // docs/editing.md §4.5: the hotkey means "dictate" and nothing else, so while the microphone
    // is already open it is deliberately a no-op. Naming it there would advertise a dead key.
    KD_CHECK(!mentions(key_legend(OverlayState::Listening, true, "Win+Y"), "Win+Y"));
    KD_CHECK(!mentions(key_legend(OverlayState::Speaking, true, "Win+Y"), "Win+Y"));
    KD_CHECK(mentions(key_legend(OverlayState::Editing, true, "Win+Y"), "Win+Y to resume"));
    // §3.2: the microphone is closed while editing and open otherwise, so the invitation to edit
    // belongs to the states that are not editing yet.
    KD_CHECK(mentions(key_legend(OverlayState::Listening, true, "Win+Y"), "type or click to edit"));
    KD_CHECK(!mentions(key_legend(OverlayState::Editing, true, "Win+Y"), "type or click to edit"));
}

KD_TEST(overlay, without_the_compose_buffer_enter_is_not_a_commit_and_is_not_named) {
    // plan.md §1.8's session, which is what runs with `editing.enabled` false or any cadence but
    // end-of-session. Enter is an ordinary key there: it passes through to the target and ends
    // the session. The legend must not say it finishes.
    const std::string line = key_legend(OverlayState::Listening, false, "Win+Y");
    KD_CHECK(!mentions(line, "Enter"));
    KD_CHECK(mentions(line, "Win+Y to finish"));
    KD_CHECK(mentions(line, "Esc to cancel"));
    KD_CHECK(mentions(line, "any key ends it"));
}

KD_TEST(overlay, the_separator_is_one_middle_dot_and_the_line_is_valid_utf8) {
    // It shipped as `\xC3\x82\xC2\xB7` — the UTF-8 encoding of U+00B7 encoded a second time —
    // and the pill drew a capital A-circumflex before every dot. Nothing above caught it:
    // every check here was a substring search, and mojibake in the glue between substrings
    // passes all of them. So the bytes are asserted, and so is the property that would have caught any such
    // corruption anywhere in the line — `to_utf16_or_empty` answers an empty string for invalid
    // UTF-8, so a legend that failed this would not be mis-drawn but absent.
    const std::string line = key_legend(OverlayState::Editing, true, "Win+Y");
    KD_CHECK(kd::utf8::validate(line));
    KD_CHECK(mentions(line, " \xC2\xB7 "));
    KD_CHECK(!mentions(line, "\xC3\x82"));  // the double-encoding, in any position
}

KD_TEST(overlay, an_unspellable_binding_drops_its_item_rather_than_printing_a_blank) {
    // HotkeySpec::to_display_string() has no failure mode today, but a legend reading
    // " to resume" would be worse than one item shorter, and the caller may push an empty string
    // before a binding is installed at all.
    const std::string editing = key_legend(OverlayState::Editing, true, "");
    KD_CHECK(mentions(editing, "Enter to finish"));
    KD_CHECK(!mentions(editing, "to resume"));

    const std::string plain = key_legend(OverlayState::Listening, false, "");
    KD_CHECK(!mentions(plain, "to finish"));
    KD_CHECK(mentions(plain, "Esc to cancel"));
}

// ─── The colour scheme ──────────────────────────────────────────────────────────────────────
//
// `resolve_overlay_style` is the second pure decision in this file: which five colours the pill
// paints with, given the user's ThemeMode and what Windows currently says. The Windows half —
// reading the registry, WM_SETTINGCHANGE, the brushes — is verified by looking at it. This half
// is arithmetic on a struct, and it is where the two failures worth testing live: a scheme that
// ignores the user's own colours, and a scheme change that quietly rewrites their layout.

KD_TEST(overlay, system_follows_windows_in_both_directions) {
    kd::OverlayStyle s;
    s.theme = kd::ThemeMode::System;
    KD_CHECK_EQ(kd::resolve_overlay_style(s, false).color_background,
                kd::overlay_palette(false).background);
    KD_CHECK_EQ(kd::resolve_overlay_style(s, true).color_background,
                kd::overlay_palette(true).background);
    KD_CHECK(kd::theme_paints_dark(kd::ThemeMode::System, true));
    KD_CHECK(!kd::theme_paints_dark(kd::ThemeMode::System, false));
}

KD_TEST(overlay, light_and_dark_ignore_what_windows_says) {
    // The two explicit choices are choices, not preferences to be overridden by the desktop.
    kd::OverlayStyle light;
    light.theme = kd::ThemeMode::Light;
    KD_CHECK_EQ(kd::resolve_overlay_style(light, true).color_pending,
                kd::overlay_palette(false).pending);

    kd::OverlayStyle dark;
    dark.theme = kd::ThemeMode::Dark;
    KD_CHECK_EQ(kd::resolve_overlay_style(dark, false).color_pending,
                kd::overlay_palette(true).pending);
}

KD_TEST(overlay, custom_colours_survive_every_scheme_windows_is_in) {
    // The whole reason ThemeMode::Custom exists. A user who set five colours keeps five colours,
    // and `theme_paints_dark` answers false for them so that nothing downstream — the state dot,
    // the selection wash, the microphone button — picks a set to match a scheme they are not in.
    kd::OverlayStyle s;
    s.theme = kd::ThemeMode::Custom;
    s.color_pending = 0xFF112233;
    s.color_unstable = 0x80112233;
    s.color_background = 0xE6445566;
    s.color_warning = 0xFF778899;
    s.color_border = 0x33AABBCC;

    for (bool system_dark : {false, true}) {
        const kd::OverlayStyle out = kd::resolve_overlay_style(s, system_dark);
        KD_CHECK_EQ(out.color_pending, s.color_pending);
        KD_CHECK_EQ(out.color_unstable, s.color_unstable);
        KD_CHECK_EQ(out.color_background, s.color_background);
        KD_CHECK_EQ(out.color_warning, s.color_warning);
        KD_CHECK_EQ(out.color_border, s.color_border);
        KD_CHECK(!kd::theme_paints_dark(kd::ThemeMode::Custom, system_dark));
    }
}

KD_TEST(overlay, the_light_palette_is_exactly_the_shipped_defaults) {
    // Not a tautology: it is what makes a settings file written before the pill had a scheme
    // paint identically after it, and what makes the settings.cpp migration to `custom` a
    // no-op for anyone who never touched a colour. If a default here is ever changed alone,
    // this is the test that says which of the two spellings is now wrong.
    const kd::OverlayStyle shipped;
    const kd::OverlayPalette& light = kd::overlay_palette(false);
    KD_CHECK_EQ(shipped.color_pending, light.pending);
    KD_CHECK_EQ(shipped.color_unstable, light.unstable);
    KD_CHECK_EQ(shipped.color_background, light.background);
    KD_CHECK_EQ(shipped.color_warning, light.warning);
    KD_CHECK_EQ(shipped.color_border, light.border);
}

KD_TEST(overlay, a_scheme_never_touches_the_layout) {
    // The scheme owns colours and nothing else. A dark pill in a different font, at a different
    // size, or at a different width would be a scheme that rewrote settings the user set.
    kd::OverlayStyle s;
    s.theme = kd::ThemeMode::System;
    s.font_family = "Cascadia Code";
    s.font_size_dip = 21.0f;
    s.corner_radius_dip = 2.0f;
    s.padding_dip = 11.0f;
    s.max_width_dip = 480.0f;

    const kd::OverlayStyle out = kd::resolve_overlay_style(s, true);
    KD_CHECK_EQ(out.font_family, s.font_family);
    KD_CHECK_EQ(out.font_size_dip, s.font_size_dip);
    KD_CHECK_EQ(out.corner_radius_dip, s.corner_radius_dip);
    KD_CHECK_EQ(out.padding_dip, s.padding_dip);
    KD_CHECK_EQ(out.max_width_dip, s.max_width_dip);
    KD_CHECK(out.theme == s.theme);
}

KD_TEST(overlay, the_dark_pill_is_still_translucent_and_still_two_inks) {
    // Two properties the dark palette shares with the light one by construction, and both are
    // load-bearing rather than decorative. The pill is drawn over the user's document, so an
    // opaque background would read as a hole cut in it; and docs/editing.md's committed/unstable
    // split is *only* visible as the difference between those two alphas.
    const kd::OverlayPalette& dark = kd::overlay_palette(true);
    KD_CHECK((dark.background >> 24) < 0xFFu);
    KD_CHECK((dark.border >> 24) < 0xFFu);
    KD_CHECK((dark.pending >> 24) > (dark.unstable >> 24));
    KD_CHECK_EQ(dark.pending & 0x00FFFFFFu, dark.unstable & 0x00FFFFFFu);

    // And the property that makes it dark at all: the ink is lighter than the ground, which is
    // the reverse of the light scheme. Compared on the green channel, which is where perceived
    // lightness mostly lives.
    const kd::OverlayPalette& light = kd::overlay_palette(false);
    KD_CHECK(((dark.pending >> 8) & 0xFFu) > ((dark.background >> 8) & 0xFFu));
    KD_CHECK(((light.pending >> 8) & 0xFFu) < ((light.background >> 8) & 0xFFu));
}
