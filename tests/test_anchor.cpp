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
