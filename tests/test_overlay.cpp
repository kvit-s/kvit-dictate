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
// ⚠ These tests cover a function `src/main.cpp` does NOT yet call. That is deliberate and it is
// recorded in three places — `App::apply_overlay_mode`, the declaration in overlay.h, and
// handover.md §8 — because "tested but uncalled" is the exact shape of the twenty defects this
// project has already had. The gate was wired, and unwiring it was a decision taken on evidence:
// enforcing this threshold against today's `score_diff` makes the inline preview unreachable on
// every target that has no system caret, because the scorer penalises ordinary typing. See
// tests/test_anchor.cpp, `s10_first_commit_falls_a_hair_under_the_default_threshold` and
// `s10_the_second_commit_is_scored_WORSE_not_better`. The rule below is right; the numbers it
// would be fed are not calibrated yet, and calibrating them is what turns this on.

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
