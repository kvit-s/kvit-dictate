// tests/test_anchor.cpp — the pure half of anchor estimation, on synthetic bitmaps.
//
// No screen capture, no windows, no DWM. Every bitmap here is drawn by the test, which is what
// makes these assertions stable enough to be worth having: a test that captures the real desktop
// measures whatever happened to be on screen.
//
// plan.md §1.5 is unusually careful about this module because revision 1 of the plan overclaimed
// it — "correct by construction", "universal", "free failure detection" — and the review rejected
// all three. What survived is a best-effort estimator with a confidence score, and the specific
// counterexamples the plan enumerates are what this file tests. Each case below is one of them.

#include "kd_test.h"

#include "anchor.h"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

using kd::anchor::Anchor;
using kd::anchor::BitmapView;
using kd::anchor::DiffResult;
using kd::anchor::MonitorGeometry;
using kd::anchor::Point;
using kd::anchor::Rect;

// A BGRA canvas the tests draw on.
class Canvas {
public:
    Canvas(int32_t w, int32_t h, uint8_t fill = 0xFF)
        : w_(w), h_(h), px_(static_cast<size_t>(w) * h * 4, fill) {}

    void fill_rect(const Rect& r, uint8_t v) {
        for (int32_t y = r.top; y < r.bottom; ++y) {
            if (y < 0 || y >= h_) continue;
            for (int32_t x = r.left; x < r.right; ++x) {
                if (x < 0 || x >= w_) continue;
                uint8_t* p = &px_[(static_cast<size_t>(y) * w_ + x) * 4];
                p[0] = p[1] = p[2] = v;
                p[3] = 0xFF;
            }
        }
    }

    void fill_all(uint8_t v) { fill_rect(Rect{0, 0, w_, h_}, v); }

    [[nodiscard]] BitmapView view() const {
        BitmapView b;
        b.pixels = px_.data();
        b.width = w_;
        b.height = h_;
        b.stride = w_ * 4;
        return b;
    }

private:
    int32_t w_;
    int32_t h_;
    std::vector<uint8_t> px_;
};

constexpr int32_t kW = 400;
constexpr int32_t kH = 200;
const Rect kRegion{0, 0, kW, kH};

}  // namespace

// ═══ diff_bitmaps: the geometry ═════════════════════════════════════════════════════════════

KD_TEST(anchor, identical_bitmaps_produce_an_empty_diff) {
    Canvas a(kW, kH);
    Canvas b(kW, kH);
    const DiffResult d = kd::anchor::diff_bitmaps(a.view(), b.view());
    KD_CHECK(d.changed_bounds.empty());
    KD_CHECK_EQ(d.changed_pixels, 0);
    KD_CHECK(!d.looks_like_scroll);
}

KD_TEST(anchor, a_compact_single_row_change_is_located_and_scores_high) {
    // The good case: a word appeared on one text line, which is what a successful commit into a
    // normal editor looks like.
    Canvas before(kW, kH);
    Canvas after(kW, kH);
    after.fill_rect(Rect{100, 80, 160, 96}, 0x00);

    const DiffResult d = kd::anchor::diff_bitmaps(before.view(), after.view());
    KD_CHECK_EQ(d.changed_bounds.left, 100);
    KD_CHECK_EQ(d.changed_bounds.right, 160);
    KD_CHECK_EQ(d.changed_bounds.top, 80);
    KD_CHECK_EQ(d.changed_bounds.bottom, 96);
    KD_CHECK(!d.looks_like_scroll);

    const float score = kd::anchor::score_diff(d, kRegion, std::nullopt);
    KD_CHECK_GT(score, 0.5f);
}

KD_TEST(anchor, line_wrap_uses_the_last_changed_row_not_the_bounding_box) {
    // THE line-wrap counterexample from plan.md §1.5. Text wrapped: a long run on the first line
    // and a short run on the second. The insertion point is at the END OF THE SECOND LINE — but
    // the bounding box's right edge belongs to the FIRST line, which is further right. A reader
    // of the bounding box alone puts the overlay a whole line up and most of a line across.
    Canvas before(kW, kH);
    Canvas after(kW, kH);
    after.fill_rect(Rect{40, 60, 380, 76}, 0x00);  // full first line
    after.fill_rect(Rect{40, 80, 120, 96}, 0x00);  // short second line

    const DiffResult d = kd::anchor::diff_bitmaps(before.view(), after.view());
    KD_CHECK_EQ(d.changed_bounds.right, 380);           // the trap
    KD_CHECK_EQ(d.last_changed_row_extent.right, 120);  // the answer
    KD_CHECK_GT(d.last_changed_row_extent.top, 76);
}

KD_TEST(anchor, a_full_viewport_change_is_a_scroll_and_scores_zero) {
    // plan.md: "a full-viewport change scores zero (treated as scroll — discard, keep previous
    // anchor)". Auto-scroll on commit is common and the repaint tells us nothing about where the
    // caret went.
    Canvas before(kW, kH, 0xFF);
    Canvas after(kW, kH, 0x40);

    const DiffResult d = kd::anchor::diff_bitmaps(before.view(), after.view());
    KD_CHECK(d.looks_like_scroll);
    KD_CHECK_EQ(kd::anchor::score_diff(d, kRegion, std::nullopt), 0.0f);
}

KD_TEST(anchor, many_disjoint_regions_score_below_one_compact_region) {
    // Syntax highlighting, spellcheck underlines and lint decorations repaint older text all over
    // the viewport. The change is real; it just does not locate the caret.
    Canvas before(kW, kH);
    Canvas scattered(kW, kH);
    for (int32_t i = 0; i < 12; ++i) {
        scattered.fill_rect(Rect{20 + i * 30, 20 + (i % 5) * 30, 30 + i * 30, 32 + (i % 5) * 30},
                            0x00);
    }
    Canvas compact(kW, kH);
    compact.fill_rect(Rect{100, 80, 160, 96}, 0x00);

    const DiffResult scattered_d = kd::anchor::diff_bitmaps(before.view(), scattered.view());
    const DiffResult compact_d = kd::anchor::diff_bitmaps(before.view(), compact.view());

    KD_CHECK_GT(scattered_d.changed_regions, compact_d.changed_regions);
    KD_CHECK_GT(kd::anchor::score_diff(compact_d, kRegion, std::nullopt),
                kd::anchor::score_diff(scattered_d, kRegion, std::nullopt));
}

KD_TEST(anchor, an_empty_diff_is_not_an_injection_failure) {
    // plan.md is explicit and it matters: "An empty diff does *not* prove injection failed —
    // whitespace can be accepted invisibly, and text can land outside the visible region."
    // Injection success is decided by the injector (§1.7), never inferred from pixels. So the
    // scorer's answer for "nothing changed" must be "I have no estimate", not "it failed".
    Canvas before(kW, kH);
    Canvas after(kW, kH);
    const DiffResult d = kd::anchor::diff_bitmaps(before.view(), after.view());
    KD_CHECK_EQ(d.changed_pixels, 0);
    KD_CHECK_EQ(kd::anchor::score_diff(d, kRegion, std::nullopt), 0.0f);
    // ...and nothing in DiffResult claims a failure occurred.
    KD_CHECK(!d.looks_like_scroll);
}

KD_TEST(anchor, an_all_black_capture_is_reported_rather_than_used) {
    // BitBlt from the desktop DC returns black under HDR / multi-plane overlay. Treating that as
    // "the screen is black" would produce a confident, wrong anchor every frame; the plan's
    // answer is to detect it and fall back to Windows.Graphics.Capture, whose desktop path is a
    // Phase 0B open question rather than something to assume works.
    Canvas black(kW, kH, 0x00);
    Canvas normal(kW, kH, 0xFF);

    const DiffResult d = kd::anchor::diff_bitmaps(black.view(), normal.view());
    KD_CHECK(d.all_black_before);
    KD_CHECK(!d.all_black_after);

    const DiffResult d2 = kd::anchor::diff_bitmaps(normal.view(), black.view());
    KD_CHECK(d2.all_black_after);
}

KD_TEST(anchor, tolerance_ignores_subpixel_and_compression_noise) {
    // Font smoothing and DWM's own compositing move pixel values by a couple of levels between
    // otherwise identical frames. Without a tolerance every frame is "changed" and the scorer
    // drowns.
    Canvas before(kW, kH, 0xFF);
    Canvas after(kW, kH, 0xFD);
    const DiffResult d = kd::anchor::diff_bitmaps(before.view(), after.view(), /*tolerance=*/8);
    KD_CHECK_EQ(d.changed_pixels, 0);

    const DiffResult strict = kd::anchor::diff_bitmaps(before.view(), after.view(), /*tolerance=*/0);
    KD_CHECK_GT(strict.changed_pixels, 0);
}

KD_TEST(anchor, an_invalid_bitmap_is_refused_rather_than_read) {
    BitmapView empty;
    Canvas ok(kW, kH);
    const DiffResult d = kd::anchor::diff_bitmaps(empty, ok.view());
    KD_CHECK(d.changed_bounds.empty());
    KD_CHECK_EQ(kd::anchor::score_diff(d, kRegion, std::nullopt), 0.0f);
}

// ═══ score_diff: proximity to the previous anchor ═══════════════════════════════════════════

KD_TEST(anchor, a_change_near_the_previous_anchor_scores_above_a_distant_one) {
    // Hysteresis' raw material: text normally appears where text last appeared. A change on the
    // other side of the screen is more likely to be some other application repainting.
    Canvas before(kW, kH);
    Canvas near_after(kW, kH);
    near_after.fill_rect(Rect{160, 80, 200, 96}, 0x00);
    Canvas far_after(kW, kH);
    far_after.fill_rect(Rect{10, 10, 50, 26}, 0x00);

    Anchor previous;
    previous.insertion_point = Point{150, 88};
    previous.line_height = 16;
    previous.confidence = 0.9f;
    previous.source = kd::anchor::Source::CommitDiff;

    const float near_score =
        kd::anchor::score_diff(kd::anchor::diff_bitmaps(before.view(), near_after.view()), kRegion,
                               previous);
    const float far_score =
        kd::anchor::score_diff(kd::anchor::diff_bitmaps(before.view(), far_after.view()), kRegion,
                               previous);
    KD_CHECK_GT(near_score, far_score);
}

KD_TEST(anchor, every_score_is_a_probability) {
    // A score outside [0,1] would break every threshold comparison downstream, including the
    // one that decides whether the overlay degrades to a floating pill.
    Canvas before(kW, kH);
    Canvas after(kW, kH);
    after.fill_rect(Rect{100, 80, 160, 96}, 0x00);
    const DiffResult cases[] = {
        kd::anchor::diff_bitmaps(before.view(), before.view()),
        kd::anchor::diff_bitmaps(before.view(), after.view()),
        kd::anchor::diff_bitmaps(before.view(), Canvas(kW, kH, 0x00).view()),
    };
    for (const DiffResult& d : cases) {
        const float s = kd::anchor::score_diff(d, kRegion, std::nullopt);
        KD_CHECK(s >= 0.0f);
        KD_CHECK(s <= 1.0f);
    }
}

// ═══ DPI and multi-monitor ══════════════════════════════════════════════════════════════════

KD_TEST(anchor, dpi_conversion_round_trips) {
    const uint32_t dpis[] = {96, 120, 144, 192};
    for (uint32_t dpi : dpis) {
        KD_CHECK_NEAR(kd::anchor::dpi_scale(dpi), static_cast<float>(dpi) / 96.0f, 0.001);
        for (int32_t px : {0, 1, 17, 100, 1234}) {
            const float dip = kd::anchor::px_to_dip(px, dpi);
            KD_CHECK_EQ(kd::anchor::dip_to_px(dip, dpi), px);
        }
    }
}

KD_TEST(anchor, a_point_is_assigned_to_the_monitor_that_contains_it) {
    // The mixed-DPI case plan.md §1.3 requires: a 100 % primary and a 150 % secondary placed to
    // its right, with the secondary's origin at a negative Y so the test cannot pass by
    // accidentally assuming a top-left-at-origin layout.
    std::vector<MonitorGeometry> monitors;
    MonitorGeometry primary;
    primary.bounds = Rect{0, 0, 1920, 1080};
    primary.dpi = 96;
    MonitorGeometry secondary;
    secondary.bounds = Rect{1920, -200, 4480, 1240};
    secondary.dpi = 144;
    monitors.push_back(primary);
    monitors.push_back(secondary);

    KD_CHECK_EQ(kd::anchor::monitor_index_for(monitors, Point{10, 10}), 0);
    KD_CHECK_EQ(kd::anchor::monitor_index_for(monitors, Point{2000, -100}), 1);
    KD_CHECK_EQ(kd::anchor::monitor_index_for(monitors, Point{1919, 500}), 0);
    KD_CHECK_EQ(kd::anchor::monitor_index_for(monitors, Point{1920, 500}), 1);
    // A point on no monitor must say so rather than silently answering 0, which would place the
    // overlay on the primary display for a target that is not there.
    KD_CHECK_EQ(kd::anchor::monitor_index_for(monitors, Point{-500, -500}), -1);
}

KD_TEST(anchor, an_anchor_on_the_scaled_monitor_converts_with_that_monitors_dpi) {
    // The bug this pins: using the PRIMARY monitor's DPI for a target on the secondary. It is
    // invisible on a single-monitor machine and wrong by 50 % on this one.
    std::vector<MonitorGeometry> monitors;
    MonitorGeometry primary;
    primary.bounds = Rect{0, 0, 1920, 1080};
    primary.dpi = 96;
    MonitorGeometry secondary;
    secondary.bounds = Rect{1920, 0, 4480, 1440};
    secondary.dpi = 144;
    monitors.push_back(primary);
    monitors.push_back(secondary);

    Anchor a;
    a.insertion_point = Point{2400, 300};
    a.line_height = 24;
    a.confidence = 0.8f;
    a.source = kd::anchor::Source::CommitDiff;

    const kd::anchor::AnchorDip dip = kd::anchor::to_monitor_dips(a, monitors);
    KD_CHECK_EQ(dip.monitor, 1);
    // 24 physical pixels at 150 % is 16 DIPs. At the primary's 96 dpi it would read as 24 — the
    // overlay would render half again too large.
    KD_CHECK_NEAR(dip.line_height, 16.0f, 0.01);
}

// ═══ is_usable ══════════════════════════════════════════════════════════════════════════════

KD_TEST(anchor, usability_requires_both_a_source_and_the_threshold) {
    // The deterministic degradation gate (plan.md §1.5): below the threshold the overlay drops to
    // its fallback. "No source" must never pass, whatever the confidence field happens to hold.
    Anchor none;
    none.confidence = 1.0f;
    none.source = kd::anchor::Source::None;
    KD_CHECK(!none.is_usable(0.5f));

    Anchor weak;
    weak.confidence = 0.4f;
    weak.source = kd::anchor::Source::CommitDiff;
    KD_CHECK(!weak.is_usable(0.5f));

    Anchor good;
    good.confidence = 0.5f;
    good.source = kd::anchor::Source::Caret;
    KD_CHECK(good.is_usable(0.5f));  // at the threshold, not merely above it
}

// ═══ The estimator, end to end ══════════════════════════════════════════════════════════════
//
// Everything above this line tests pure functions. That left `AnchorEstimator` itself — the
// worker thread, the before/after polling, and `publish()` — with no coverage at all, and it is
// the half that decides whether source C is alive.
//
// That gap had a cost. After several real dictation sessions the diagnostics log held eight
// "empty diff" and four "capture failed" lines from source C and *not one* recording a success,
// because the success path logged nothing — so "source C works and the caret simply outranks it"
// and "source C has never once produced an anchor" were indistinguishable, and they imply very
// different Phase 0B work (docs/supported-targets.md §5.10). A log line was added for the field;
// this is the part that settles it here instead, where it cannot regress unnoticed.

class ScriptedCapture : public kd::anchor::IScreenCapture {
public:
    ScriptedCapture(Canvas before, Canvas after) : before_(std::move(before)), after_(std::move(after)) {}

    kd::Result<BitmapView> capture(const Rect&) override {
        // First call is the before-frame, every later one the after-frame — which is exactly the
        // order the estimator uses.
        return kd::Result<BitmapView>{(calls_++ == 0 ? before_ : after_).view()};
    }
    [[nodiscard]] bool last_capture_was_black() const override { return false; }
    [[nodiscard]] const char* name() const override { return "scripted"; }

private:
    Canvas before_;
    Canvas after_;
    int calls_ = 0;
};

class NoCaret : public kd::anchor::ICaretProvider {
public:
    bool caret_rect(uintptr_t, Rect&) override { return false; }
};

class RecordingListener : public kd::anchor::IAnchorListener {
public:
    void on_anchor(const Anchor& a) override {
        std::lock_guard<std::mutex> lk(m_);
        anchors_.push_back(a);
        cv_.notify_all();
    }

    // The listener fires on the worker thread, so the test waits rather than sleeping a guessed
    // interval — a fixed sleep here would be flaky on a loaded machine and slow on an idle one.
    bool wait_for_one(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, timeout, [this] { return !anchors_.empty(); });
    }

    std::vector<Anchor> anchors() {
        std::lock_guard<std::mutex> lk(m_);
        return anchors_;
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::vector<Anchor> anchors_;
};

KD_TEST(anchor, the_estimator_publishes_an_anchor_from_a_commit_diff) {
    // A blank region, then the same region with a short dark run on one row: the shape a
    // committed word actually makes.
    Canvas before(200, 60, 0xFF);
    Canvas after(200, 60, 0xFF);
    after.fill_rect(Rect{20, 24, 96, 38}, 0x10);

    ScriptedCapture capture(std::move(before), std::move(after));
    NoCaret caret;
    RecordingListener listener;

    kd::anchor::AnchorConfig cfg;
    cfg.after_frame_poll_ms = 200;  // bounded: the scripted after-frame differs on the first poll
    kd::anchor::AnchorEstimator est(cfg, &capture, &caret, &listener);
    KD_REQUIRE(est.start().ok());

    est.submit_before_frame(Rect{100, 100, 300, 160});
    est.submit_after_frame("hello world");

    KD_REQUIRE(listener.wait_for_one(std::chrono::seconds(5)));
    est.stop();

    const std::vector<Anchor> got = listener.anchors();
    KD_REQUIRE(!got.empty());
    const Anchor& a = got.front();
    // The whole point: source C, on its own, with no caret available, produced a usable anchor.
    KD_CHECK_EQ(static_cast<int>(a.source), static_cast<int>(kd::anchor::Source::CommitDiff));
    KD_CHECK(a.confidence > 0.0f);
    KD_CHECK(a.confidence <= kd::anchor::kMaxDiffConfidence);
    KD_CHECK(a.is_usable(cfg.min_confidence));
    // Placed inside the region that was actually submitted, not in bitmap-local coordinates —
    // getting this wrong would put the overlay at the top-left of the screen.
    KD_CHECK(a.insertion_point.x >= 100 && a.insertion_point.x <= 300);
    KD_CHECK(a.insertion_point.y >= 100 && a.insertion_point.y <= 160);
}

KD_TEST(anchor, an_unchanged_after_frame_publishes_nothing) {
    // The "empty diff" branch, which is what every real session has logged so far. It must stay
    // silent rather than publish a degenerate anchor — keeping the previous one is the defined
    // behaviour and the overlay's fallback depends on it.
    Canvas before(200, 60, 0xFF);
    Canvas after(200, 60, 0xFF);  // identical

    ScriptedCapture capture(std::move(before), std::move(after));
    NoCaret caret;
    RecordingListener listener;

    kd::anchor::AnchorConfig cfg;
    cfg.after_frame_poll_ms = 50;
    kd::anchor::AnchorEstimator est(cfg, &capture, &caret, &listener);
    KD_REQUIRE(est.start().ok());

    est.submit_before_frame(Rect{100, 100, 300, 160});
    est.submit_after_frame("hello world");

    KD_CHECK(!listener.wait_for_one(std::chrono::milliseconds(400)));
    est.stop();
    KD_CHECK(listener.anchors().empty());
}

// ═══ The A09/S07 measurement, replayed ══════════════════════════════════════════════════════
//
// docs/supported-targets.md §5.9.2 measured 48.07 line heights of anchor error on a line wrap at
// the bottom of a scrolling control, reproducibly, three times. These tests replay that exact
// DiffResult and capture region so the consequences are pinned in code rather than argued from a
// JSON file. Every number below is copied from
// bench/results/phase0b/A09/S07/sendinput/*/result.json.
//
// The point is not that the estimator is inaccurate here — it cannot be accurate, because the
// true insertion point is outside the rectangle that was captured. The point is what the rest of
// the pipeline does with an answer it has itself scored as not good enough.

namespace {

// The measured region: capture_region_for() around the pre-injection insertion point (1157, 442),
// 900 x 220 at 96 dpi with the point at 65 % of the width.
constexpr Rect kS07Region{572, 332, 1472, 552};
// Where the caret really was after the wrap. 728 px left of where it started, because the wrap
// moved it to the beginning of a new visual line and the control scrolled up to compensate.
constexpr kd::anchor::Point kS07Truth{429, 442};
constexpr int32_t kS07LineHeight = 15;

kd::anchor::DiffResult s07_diff() {
    kd::anchor::DiffResult d;
    d.changed_bounds = Rect{0, 98, 606, 125};
    d.last_changed_row_extent = Rect{0, 113, 578, 125};
    d.changed_rows = 24;
    d.changed_regions = 2;
    // Not recorded per-cell by the probe; any positive value drives the same branches, and the
    // fraction-of-region term in score_diff is dominated by changed_rows.
    d.changed_pixels = 4000;
    d.looks_like_scroll = false;
    return d;
}

}  // namespace

KD_TEST(anchor, s07_wrap_puts_the_truth_outside_the_captured_region) {
    // The measurement's own explanation, asserted rather than asserted-in-prose: the region
    // reaches 585 px behind the previous insertion point, and the new one is further than that.
    KD_CHECK(kS07Truth.x < kS07Region.left);
    KD_CHECK_EQ(kS07Region.left - kS07Truth.x, 143);

    const std::optional<Anchor> a =
        kd::anchor::anchor_from_diff(s07_diff(), kS07Region, std::nullopt, 1000);
    KD_REQUIRE(a.has_value());

    // What the estimator reported, and it is not a wild jump: the right edge of the last changed
    // text row, which is 7 px from where the caret was BEFORE the injection. So the failure mode
    // is not "the overlay leaps across the screen" — it is "the overlay stays put while the text
    // walks away from it". Those need different fixes, and the difference is only visible here.
    KD_CHECK_EQ(a->insertion_point.x, 1150);
    KD_CHECK_EQ(a->insertion_point.y, 445);

    const double dx = static_cast<double>(a->insertion_point.x - kS07Truth.x);
    const double dy = static_cast<double>(a->insertion_point.y - kS07Truth.y);
    const double err_lh = std::sqrt(dx * dx + dy * dy) / kS07LineHeight;
    KD_CHECK_NEAR(err_lh, 48.07, 0.02);  // the figure published in §5.9.2
}

KD_TEST(anchor, s07_scores_below_min_confidence_and_is_published_anyway) {
    const std::optional<Anchor> a =
        kd::anchor::anchor_from_diff(s07_diff(), kS07Region, std::nullopt, 1000);
    KD_REQUIRE(a.has_value());

    // The estimator's own verdict on this answer: BELOW the default threshold. §5.9.2 quotes it
    // as 0.50 because the probe prints two decimals; it is 0.4953, and the distinction is the
    // whole of this test.
    kd::anchor::AnchorConfig cfg;
    KD_CHECK_NEAR(a->confidence, 0.4953f, 0.001);
    KD_CHECK(a->confidence < cfg.min_confidence);
    KD_CHECK(!a->is_usable(cfg.min_confidence));

    // And yet it replaces a previous source-C anchor, because should_replace_anchor treats a
    // candidate from the SAME source as a refresh and returns true without consulting
    // min_confidence at all. Nothing downstream re-checks: AnchorEstimator::publish() stores
    // whatever should_replace_anchor accepts, and Overlay::set_anchor() takes whatever it is
    // given.
    Anchor previous;
    previous.insertion_point = kd::anchor::Point{1157, 442};
    previous.line_height = kS07LineHeight;
    previous.confidence = 0.90f;  // a much better earlier diff
    previous.source = kd::anchor::Source::CommitDiff;
    previous.timestamp_ms = 900;

    KD_CHECK(kd::anchor::should_replace_anchor(previous, *a, cfg, 1000));

    // A caret, by contrast, is not displaced by it — the source ranking does hold.
    Anchor caret_anchor = previous;
    caret_anchor.source = kd::anchor::Source::Caret;
    caret_anchor.confidence = kd::anchor::kCaretConfidence;
    KD_CHECK(!kd::anchor::should_replace_anchor(caret_anchor, *a, cfg, 1000));
}

// ═══ The A09/S10 measurement, and what the gate does to it ══════════════════════════════════
//
// S10 is the good result — 0.12 line heights with no system caret at all, the VS Code / Slack /
// terminal case (docs/supported-targets.md §5.9.2). Its recorded confidence is 0.4999999702,
// which sits a hair BELOW the default min_confidence of 0.5, so once the degradation gate exists
// the obvious worry is that the best thing source C ever measured is still not good enough to
// draw inline.
//
// It is not, on the first commit of a session, and it is on every one after. The reason is that
// `kd_probe` always passes `previous = nullopt`, so every published figure carries
// kProxNoPrevious (0.85) rather than the full proximity term a corroborated estimate earns. The
// probe therefore *understates* what the product scores in its steady state, and no number in
// §5.9.2 should be read as the confidence a user would actually get after the first commit.

namespace {

// region [416,416 980x820] — the whole-window fallback, because a self-drawn caret gives
// capture_region_for() nothing to centre on.
constexpr Rect kS10Region{416, 416, 1396, 1236};

kd::anchor::DiffResult s10_diff() {
    kd::anchor::DiffResult d;
    d.changed_bounds = Rect{22, 457, 199, 472};
    d.last_changed_row_extent = Rect{22, 457, 199, 472};
    d.changed_rows = 15;
    d.changed_regions = 3;
    d.changed_pixels = 900;
    d.looks_like_scroll = false;
    return d;
}

}  // namespace

KD_TEST(anchor, s10_first_commit_falls_a_hair_under_the_default_threshold) {
    // Exactly the figure in the record, to the float. If the scorer is retuned this is the test
    // that says the published table needs re-reading.
    const float first = kd::anchor::score_diff(s10_diff(), kS10Region, std::nullopt);
    KD_CHECK_NEAR(first, 0.4999999702f, 1e-6);

    kd::anchor::AnchorConfig cfg;
    KD_CHECK(first < cfg.min_confidence);  // by 3e-8 — arithmetic, not judgement
}

KD_TEST(anchor, s10_the_second_commit_is_scored_WORSE_not_better) {
    // The state the second commit of a session is actually in: the previous anchor is where the
    // FIRST commit left the caret (615, 873 — region.left + band.right of the diff above), and
    // this commit's text starts there and runs the same width to the right.
    kd::anchor::DiffResult second = s10_diff();
    second.changed_bounds = Rect{199, 457, 376, 472};
    second.last_changed_row_extent = Rect{199, 457, 376, 472};

    Anchor previous;
    previous.insertion_point = kd::anchor::Point{615, 873};
    previous.line_height = 16;
    previous.confidence = 0.49f;
    previous.source = kd::anchor::Source::CommitDiff;
    previous.timestamp_ms = 900;

    const float first_commit = kd::anchor::score_diff(s10_diff(), kS10Region, std::nullopt);
    const float second_commit = kd::anchor::score_diff(second, kS10Region, previous);

    // Having an incumbent to corroborate against makes the score WORSE, not better. The proximity
    // term measures how far the new estimate is from the old one in units of LINE HEIGHT
    // (kProxNearLines = 3, kProxFarLines = 15), and a commit of 25 characters moves the insertion
    // point ~177 px = ~11 line heights *horizontally*. So ordinary typing reads as "the anchor
    // jumped", and every score is compressed toward the bottom of the range.
    //
    // That is a real weakness and it is NOT the reason the degradation gate is unwired — the
    // measured sweep (docs/supported-targets.md §5.9.2.1) shows the compression hits *inaccurate*
    // estimates harder, so accurate ones still separate cleanly at 0.3742–0.5000 against
    // 0.1858–0.2941. What is wrong is min_confidence's 0.50 default, which is above both ranges.
    // This test pins the compression so that a later fix to the units is visibly a change.
    KD_CHECK(second_commit < first_commit);
    KD_CHECK(second_commit < 0.5f);

    // Recorded rather than asserted loosely, so retuning kProx* is visible here.
    KD_CHECK_NEAR(second_commit, 0.3904f, 0.005);
}
