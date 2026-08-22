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

#include <optional>

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
