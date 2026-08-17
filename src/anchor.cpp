// Anchor estimation — plan.md §1.5.
//
// A BEST-EFFORT ESTIMATOR WITH A CONFIDENCE SCORE. Revision 1 of the plan called this mechanism
// "correct by construction", "universal", and a source of "free failure detection"; the review
// rejected all three and was right to. Nothing in this file is correct by construction. Every
// number in the scorer below is a hand-chosen prior that Phase 0B exists to replace with a
// measurement, and where the code is guessing the comment says so.
//
// What the module is allowed to do is be wrong. What it is not allowed to do is be wrong
// *silently* (hence the confidence score and the deterministic fallback) or *flappingly* (hence
// the hysteresis).
//
// **Injection success is a separate signal.** An empty diff does not prove the injection failed;
// a non-empty diff does not prove the expected text arrived. injector.h owns that verdict.
// Nothing here returns one, and nothing may be added that does.

#define KD_LOG_MODULE "anchor"

#include "anchor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <system_error>
#include <thread>

#include "util/clock.h"
#include "util/log.h"

// windows.h is included in the .cpp only. anchor.h stays Windows-free on purpose: it is what
// lets the diff scorer and the DPI arithmetic be unit-tested with no window, no desktop and no
// display. Do not "fix" a compile error by moving this include into the header.
#include <windows.h>

#include <shellscalingapi.h>  // GetDpiForMonitor (shcore), for monitor_geometry_for()

namespace kd::anchor {

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Pure half: geometry, differencing, scoring. No Windows calls below this line until the
//  "Windows-facing" banner.
// ═══════════════════════════════════════════════════════════════════════════════════════════

namespace {

// A linear ramp from 1.0 at `lo` down to 0.0 at `hi`. Used everywhere in the scorer so the shape
// of every penalty is the same shape, and tuning one is changing two numbers rather than a
// formula.
float ramp_down(float v, float lo, float hi) noexcept {
    if (!(hi > lo)) return v <= lo ? 1.0f : 0.0f;
    if (v <= lo) return 1.0f;
    if (v >= hi) return 0.0f;
    return (hi - v) / (hi - lo);
}

float clamp01(float v) noexcept { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// ─── Scoring constants ──────────────────────────────────────────────────────────────────────
//
// All of these are priors, not measurements. They are named and gathered here so Phase 0B can
// replace them from recorded captures without reading the algorithm.

// Vertical compactness. One commit should repaint about one text line. The estimator has no font
// metrics — it never sees the target's DPI, font size or line spacing — so "one line" is
// approximated as a fraction of the capture height. With the default 220 px capture window, 12 %
// is ~26 px, which is roughly one line at 100 % scale and roughly half a line at 200 %. That
// asymmetry is a known weakness of scoring on fractions instead of on measured line height.
constexpr float kCompactRowsGood = 0.12f;
constexpr float kCompactRowsBad = 0.55f;

// Horizontal extent of the last changed text row. A long commit legitimately spans the capture,
// so the penalty bottoms out well above zero instead of rejecting it.
constexpr float kWidthGood = 0.50f;
constexpr float kWidthBad = 1.00f;
constexpr float kWidthFloor = 0.25f;

// Fragmentation. The penalty starts at *two* regions, not three: two is already ambiguous — a
// wrapped line, or a caret/underline drawn separately from the glyphs — and syntax highlighting,
// spellcheck squiggles, autocomplete popups and animation push it much higher.
constexpr float kFragPerRegion = 0.35f;

// Proximity to the previous anchor, measured in line heights. Never falls below 0.5: a genuine
// jump (the user pressed Enter, or the field scrolled by one line) is not evidence of garbage.
constexpr float kProxNearLines = 3.0f;
constexpr float kProxFarLines = 15.0f;
constexpr float kProxFloor = 0.50f;
constexpr float kProxNoPrevious = 0.85f;  // no incumbent to corroborate against; neither reward
                                          // nor punish, but do not claim the top of the range

// RTL/bidi is a guess (see anchor_from_diff's contract), so it costs confidence.
constexpr float kRtlPenalty = 0.65f;
constexpr int32_t kMinLeftGrowthPx = 4;  // below this, "grew leftward" is antialiasing noise

// Scroll / full-viewport-repaint shape.
constexpr float kScrollRowFraction = 0.70f;
constexpr float kScrollBoxFraction = 0.70f;
constexpr float kScrollDensity = 0.20f;

// ─── The insertion point implied by a diff ──────────────────────────────────────────────────

struct DiffPoint {
    Point p;
    int32_t line_height = 0;
    bool rtl = false;
    bool valid = false;
};

// Shared by score_diff() and anchor_from_diff() so the position that is scored is provably the
// position that is published. Two copies of this arithmetic would drift.
DiffPoint diff_insertion_point(const DiffResult& d, const Rect& region,
                               const std::optional<Anchor>& previous) noexcept {
    DiffPoint out;
    if (d.changed_pixels <= 0 || d.last_changed_row_extent.empty()) return out;

    const Rect& band = d.last_changed_row_extent;
    out.valid = true;
    out.line_height = band.height();

    // RTL/bidi: we can see that the changed region grew *leftward* from the previous anchor on
    // the same line, and that is all we can see. A right-aligned LTR paragraph and a selection
    // replacement produce the same shape, so this sets a flag and costs confidence rather than
    // claiming to have detected bidi text. With no previous anchor on the same line there is
    // nothing to compare against and the estimator assumes LTR — which is simply wrong for an
    // RTL target, and is on Phase 0B's scenario matrix for exactly that reason.
    if (previous) {
        const int32_t prev_x = previous->insertion_point.x - region.left;
        const int32_t prev_top = previous->insertion_point.y - region.top;
        const int32_t prev_h = previous->line_height > 0 ? previous->line_height : band.height();
        const bool same_line = band.top < prev_top + prev_h && prev_top < band.bottom;
        if (same_line) {
            const int32_t left_growth = prev_x - band.left;
            const int32_t right_growth = band.right - prev_x;
            if (left_growth > right_growth && left_growth > kMinLeftGrowthPx) out.rtl = true;
        }
    }

    // The line-wrap fix, and the whole reason DiffResult carries the last changed *text row*
    // separately from the bounding box: when a commit wraps, the bounding box's right edge is the
    // end of the previous line, which is nowhere near the insertion point.
    out.p.x = region.left + (out.rtl ? band.left : band.right);
    out.p.y = region.top + band.top;  // top of the line box, matching Anchor's convention
    return out;
}

bool is_ascii_whitespace_only(std::string_view s) noexcept {
    if (s.empty()) return true;
    for (const char c : s) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return false;
    }
    return true;
}

}  // namespace

bool BitmapView::is_valid() const noexcept {
    return pixels != nullptr && width > 0 && height > 0 && stride >= width * 4;
}

const char* to_string(Source s) noexcept {
    switch (s) {
        case Source::None: return "none";
        case Source::Caret: return "caret";
        case Source::LastClick: return "last_click";
        case Source::CommitDiff: return "commit_diff";
    }
    return "none";
}

const char* to_string(Fallback f) noexcept {
    switch (f) {
        case Fallback::FloatingPill: return "floating_pill";
        case Fallback::NoPreview: return "no_preview";
    }
    return "floating_pill";
}

DiffResult diff_bitmaps(const BitmapView& before, const BitmapView& after, int tolerance,
                        int merge_gap_px) {
    DiffResult d;
    if (!before.is_valid() || !after.is_valid()) return d;
    // Mismatched dimensions mean the two captures are not of the same rectangle, which is a
    // caller bug — but reading past the end of one of them would be worse than reporting nothing.
    if (before.width != after.width || before.height != after.height) return d;

    const int32_t w = before.width;
    const int32_t h = before.height;
    const int tol = tolerance < 0 ? 0 : tolerance;
    const int32_t gap = merge_gap_px < 0 ? 0 : merge_gap_px;

    bool black_before = true;
    bool black_after = true;

    // Per-row summaries, so the band arithmetic below is a second pass over h entries rather than
    // a second pass over w*h pixels.
    std::vector<int32_t> row_min(static_cast<size_t>(h), 0);
    std::vector<int32_t> row_max(static_cast<size_t>(h), -1);

    // Run-length connected-component labelling. Counting components pixel-by-pixel would count
    // every *glyph* as its own region — "the" would be three — which is why runs are merged
    // across gaps smaller than `merge_gap_px` before being labelled.
    struct Run {
        int32_t x0 = 0;
        int32_t x1 = 0;  // inclusive
        int32_t label = -1;
    };
    std::vector<Run> raw_runs;
    std::vector<Run> cur_runs;
    std::vector<Run> prev_runs;
    std::vector<int32_t> parent;
    parent.reserve(256);

    const auto find_root = [&parent](int32_t x) noexcept {
        while (parent[static_cast<size_t>(x)] != x) {
            parent[static_cast<size_t>(x)] =
                parent[static_cast<size_t>(parent[static_cast<size_t>(x)])];
            x = parent[static_cast<size_t>(x)];
        }
        return x;
    };
    const auto unite = [&parent, &find_root](int32_t a, int32_t b) noexcept {
        a = find_root(a);
        b = find_root(b);
        if (a != b) parent[static_cast<size_t>(b)] = a;
    };

    int32_t min_x = w, max_x = -1, min_y = h, max_y = -1;

    for (int32_t y = 0; y < h; ++y) {
        const uint8_t* rb = before.pixels + static_cast<size_t>(y) * static_cast<size_t>(before.stride);
        const uint8_t* ra = after.pixels + static_cast<size_t>(y) * static_cast<size_t>(after.stride);

        raw_runs.clear();
        int32_t run_start = -1;
        int32_t row_count = 0;
        int32_t rmin = w;
        int32_t rmax = -1;

        for (int32_t x = 0; x < w; ++x) {
            const uint8_t* pb = rb + static_cast<size_t>(x) * 4u;
            const uint8_t* pa = ra + static_cast<size_t>(x) * 4u;

            // Alpha is deliberately ignored on both sides: BitBlt into a 32-bit DIB leaves the
            // fourth byte undefined, and comparing it produces changes on pixels nobody painted.
            if (black_before && (pb[0] | pb[1] | pb[2]) != 0) black_before = false;
            if (black_after && (pa[0] | pa[1] | pa[2]) != 0) black_after = false;

            const int db = std::abs(static_cast<int>(pa[0]) - static_cast<int>(pb[0]));
            const int dg = std::abs(static_cast<int>(pa[1]) - static_cast<int>(pb[1]));
            const int dr = std::abs(static_cast<int>(pa[2]) - static_cast<int>(pb[2]));
            const bool changed = (db > tol) || (dg > tol) || (dr > tol);

            if (changed) {
                ++row_count;
                if (x < rmin) rmin = x;
                rmax = x;
                if (run_start < 0) run_start = x;
            } else if (run_start >= 0) {
                raw_runs.push_back(Run{run_start, x - 1, -1});
                run_start = -1;
            }
        }
        if (run_start >= 0) raw_runs.push_back(Run{run_start, w - 1, -1});

        if (row_count > 0) {
            d.changed_pixels += row_count;
            ++d.changed_rows;
            row_min[static_cast<size_t>(y)] = rmin;
            row_max[static_cast<size_t>(y)] = rmax;
            if (rmin < min_x) min_x = rmin;
            if (rmax > max_x) max_x = rmax;
            if (y < min_y) min_y = y;
            max_y = y;
        }

        // Merge runs separated by less than the gap, then label.
        cur_runs.clear();
        for (const Run& r : raw_runs) {
            if (!cur_runs.empty() && r.x0 - cur_runs.back().x1 - 1 <= gap) {
                cur_runs.back().x1 = r.x1;
            } else {
                cur_runs.push_back(r);
            }
        }

        // Two-pointer sweep: both lists are sorted by x0, so a previous-row run that ends before
        // this run starts can never touch a later run either. 8-connected (the +1s), because
        // antialiased glyph edges are diagonal and 4-connectivity splits them.
        size_t pi = 0;
        for (Run& r : cur_runs) {
            while (pi < prev_runs.size() && prev_runs[pi].x1 + 1 < r.x0) ++pi;
            size_t pj = pi;
            int32_t label = -1;
            while (pj < prev_runs.size() && prev_runs[pj].x0 <= r.x1 + 1) {
                if (label < 0) {
                    label = find_root(prev_runs[pj].label);
                } else {
                    unite(label, prev_runs[pj].label);
                }
                ++pj;
            }
            if (label < 0) {
                label = static_cast<int32_t>(parent.size());
                parent.push_back(label);
            }
            r.label = label;
        }
        prev_runs.swap(cur_runs);
    }

    d.all_black_before = black_before;
    d.all_black_after = black_after;

    if (d.changed_pixels == 0) return d;  // empty diff: not a failure, just no information

    d.changed_bounds = Rect{min_x, min_y, max_x + 1, max_y + 1};

    // Count components. Only roots survive the union-find, so this is one pass over the labels.
    int32_t regions = 0;
    for (size_t i = 0; i < parent.size(); ++i) {
        if (parent[i] == static_cast<int32_t>(i)) {
            ++regions;
            if (regions >= kMaxTrackedRegions) break;
        }
    }
    d.changed_regions = regions;

    // The last changed *text row*: walk up from the last changed pixel row while rows keep
    // changing. The separator is a fully unchanged pixel row, which ordinary line spacing
    // provides; tight leading or a descender touching the next ascender merges two lines into one
    // band, and then the wrap case is missed. That is a limit, not a handled case.
    int32_t band_bottom = max_y;
    int32_t band_top = band_bottom;
    while (band_top > 0 && row_max[static_cast<size_t>(band_top - 1)] >= 0) --band_top;

    int32_t bmin = w;
    int32_t bmax = -1;
    for (int32_t y = band_top; y <= band_bottom; ++y) {
        if (row_max[static_cast<size_t>(y)] < 0) continue;
        bmin = std::min(bmin, row_min[static_cast<size_t>(y)]);
        bmax = std::max(bmax, row_max[static_cast<size_t>(y)]);
    }
    d.last_changed_row_extent = Rect{bmin, band_top, bmax + 1, band_bottom + 1};

    // Scroll / full-viewport repaint. A scroll moves every line, so the change is wide, tall,
    // spread over most rows, and reasonably solid. The density term is what keeps scattered
    // syntax highlighting out of this bucket — that case is caught by the fragmentation penalty
    // instead, and lands at a low score rather than at exactly zero.
    //
    // Honest limit: a scroll of sparse text on a plain background can fall under the density
    // threshold and be missed here. The fragmentation penalty then scores it low anyway, so the
    // outcome — do not trust this diff — is the same, but the reason recorded is different.
    const double region_area = static_cast<double>(w) * static_cast<double>(h);
    const double box_area = static_cast<double>(d.changed_bounds.width()) *
                            static_cast<double>(d.changed_bounds.height());
    const bool wide = d.changed_bounds.width() >= static_cast<int32_t>(kScrollBoxFraction * static_cast<float>(w));
    const bool tall = d.changed_bounds.height() >= static_cast<int32_t>(kScrollBoxFraction * static_cast<float>(h));
    const bool rows_dense = static_cast<double>(d.changed_rows) >= kScrollRowFraction * static_cast<double>(h);
    const bool solid = box_area > 0.0 && static_cast<double>(d.changed_pixels) >= kScrollDensity * box_area;
    d.looks_like_scroll = wide && tall && rows_dense && solid && region_area > 0.0;

    return d;
}

float score_diff(const DiffResult& d, const Rect& capture_region,
                 const std::optional<Anchor>& previous) {
    if (capture_region.empty()) return 0.0f;

    // An all-black capture is the documented symptom of an HDR / multi-plane-overlay path that
    // BitBlt cannot see. It carries no information at all, so it scores zero — and the estimator
    // separately raises the WGC-fallback flag, because "score zero forever" is not a diagnosis.
    if (d.all_black_before || d.all_black_after) return 0.0f;

    // Empty diff. **Not a failure signal** — whitespace-only and newline-only commits produce
    // little or no visible change, and text can land outside the visible region. Zero means "no
    // information", the previous anchor is kept, and injector.h is still the only thing that says
    // whether the injection worked.
    if (d.changed_pixels <= 0) return 0.0f;

    // Full-viewport change: treated as a scroll. Discard, keep the previous anchor.
    if (d.looks_like_scroll) return 0.0f;

    const float w = static_cast<float>(capture_region.width());
    const float h = static_cast<float>(capture_region.height());

    const float rows_frac = static_cast<float>(d.changed_rows) / h;
    const float compact = ramp_down(rows_frac, kCompactRowsGood, kCompactRowsBad);
    if (compact <= 0.0f) return 0.0f;

    const float width_frac = static_cast<float>(d.last_changed_row_extent.width()) / w;
    const float width_term =
        kWidthFloor + (1.0f - kWidthFloor) * ramp_down(width_frac, kWidthGood, kWidthBad);

    const int32_t regions = d.changed_regions > 0 ? d.changed_regions : 1;
    const float frag = 1.0f / (1.0f + kFragPerRegion * static_cast<float>(regions - 1));

    const DiffPoint dp = diff_insertion_point(d, capture_region, previous);
    if (!dp.valid) return 0.0f;

    float prox = kProxNoPrevious;
    if (previous) {
        const int32_t scale_px =
            std::max(std::max(previous->line_height, dp.line_height), int32_t{8});
        const float dx = static_cast<float>(dp.p.x - previous->insertion_point.x);
        const float dy = static_cast<float>(dp.p.y - previous->insertion_point.y);
        const float lines = std::sqrt(dx * dx + dy * dy) / static_cast<float>(scale_px);
        prox = kProxFloor + (1.0f - kProxFloor) * ramp_down(lines, kProxNearLines, kProxFarLines);
    }

    float s = compact * width_term * frag * prox;
    if (dp.rtl) s *= kRtlPenalty;

    // Source C never outranks a real caret rectangle, however good the diff looks. It is an
    // inference from pixels; source A is the target telling us directly.
    return std::min(clamp01(s), kMaxDiffConfidence);
}

std::optional<Anchor> anchor_from_diff(const DiffResult& d, const Rect& capture_region,
                                       const std::optional<Anchor>& previous, uint64_t now_ms) {
    const float confidence = score_diff(d, capture_region, previous);
    if (confidence <= 0.0f) return std::nullopt;

    const DiffPoint dp = diff_insertion_point(d, capture_region, previous);
    if (!dp.valid) return std::nullopt;

    Anchor a;
    a.insertion_point = dp.p;
    a.line_height = dp.line_height;
    a.rtl = dp.rtl;
    a.confidence = confidence;
    a.source = Source::CommitDiff;
    a.timestamp_ms = now_ms;
    return a;
}

bool should_replace_anchor(const std::optional<Anchor>& current, const Anchor& candidate,
                           const AnchorConfig& cfg, uint64_t now_ms) {
    if (candidate.source == Source::None) return false;
    if (!current || current->source == Source::None) return true;

    // Same source: this is a refresh, not a switch. Hysteresis is about *which source wins*, not
    // about freezing a source's own updates — otherwise the caret could never move.
    if (candidate.source == current->source) return true;

    // A stale incumbent loses to anything current. The subtraction is guarded because a manual
    // clock in a test, or a timestamp from a different clock, can run backwards.
    if (now_ms >= current->timestamp_ms &&
        now_ms - current->timestamp_ms > static_cast<uint64_t>(cfg.stale_after_ms)) {
        return true;
    }

    return candidate.confidence >= current->confidence + cfg.source_switch_margin;
}

// ─── Source B ───────────────────────────────────────────────────────────────────────────────

void ClickAnchor::on_click(MouseButton b, Point p, uintptr_t w, uint64_t t) noexcept {
    // Non-left buttons are not a weaker signal, they are a *different* signal: a right-click
    // position is where a context menu is about to open, and middle-click pastes on X11 (which is
    // what a WSLg target is). Ignoring them leaves the previous left click cached, which is the
    // better estimate.
    if (b != MouseButton::Left) return;
    position = p;
    window = w;
    timestamp_ms = t;
    valid = true;
    downgraded = false;
}

void ClickAnchor::on_wheel() noexcept {
    // Scrolling moves the caret with no click at all, so the cached point is not stale — it is
    // meaningless. Invalidate rather than downgrade.
    valid = false;
    downgraded = false;
}

void ClickAnchor::on_keyboard_navigation() noexcept {
    // Arrow keys / Home / End / Page: the caret moved, but probably not far, and a stale position
    // beats no position. Downgrade.
    if (valid) downgraded = true;
}

void ClickAnchor::clear() noexcept { *this = ClickAnchor{}; }

std::optional<Anchor> ClickAnchor::evaluate(uintptr_t foreground_window, uint64_t now_ms,
                                            int32_t line_height_hint) const {
    if (!valid || window == 0) return std::nullopt;

    // The click window must still be foreground. `window` is the *top-level* window under the
    // click — AnchorEstimator::set_last_click normalises it with GetAncestor(GA_ROOT), because
    // the raw hit-test result is a child control and would never compare equal to
    // GetForegroundWindow().
    if (foreground_window == 0 || foreground_window != window) return std::nullopt;

    Anchor a;
    a.insertion_point = position;
    a.line_height = line_height_hint > 0 ? line_height_hint : 0;

    // Vertical placement is the reliable half: the click landed *inside* the target line, so with
    // a line height to work with the line box is centred on the click. X is only a hint —
    // clicking past end-of-line snaps the caret left, and clicking an empty field says nothing
    // about X at all — but there is nothing better available, so it is used as-is and the
    // confidence carries the doubt.
    if (a.line_height > 0) a.insertion_point.y = position.y - a.line_height / 2;

    a.confidence = downgraded ? kClickDowngradedConfidence : kClickConfidence;
    a.source = Source::LastClick;
    a.timestamp_ms = now_ms;
    return a;
}

// ─── DPI and multi-monitor ──────────────────────────────────────────────────────────────────

int32_t monitor_index_for(const std::vector<MonitorGeometry>& monitors, Point p) noexcept {
    for (size_t i = 0; i < monitors.size(); ++i) {
        const Rect& b = monitors[i].bounds;
        if (p.x >= b.left && p.x < b.right && p.y >= b.top && p.y < b.bottom) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

float dpi_scale(uint32_t dpi) noexcept {
    return dpi == 0 ? 1.0f : static_cast<float>(dpi) / 96.0f;
}

float px_to_dip(int32_t px, uint32_t dpi) noexcept {
    return static_cast<float>(px) / dpi_scale(dpi);
}

int32_t dip_to_px(float dip, uint32_t dpi) noexcept {
    const float v = dip * dpi_scale(dpi);
    // Round half away from zero rather than truncating: truncation biases every converted
    // coordinate one pixel toward the origin, which on a 200 % monitor is half a DIP of drift per
    // conversion and shows up as an overlay that sits slightly high and slightly left.
    return static_cast<int32_t>(v >= 0.0f ? std::floor(v + 0.5f) : std::ceil(v - 0.5f));
}

AnchorDip to_monitor_dips(const Anchor& a, const std::vector<MonitorGeometry>& monitors) noexcept {
    AnchorDip out;
    const int32_t idx = monitor_index_for(monitors, a.insertion_point);
    if (idx < 0) return out;  // on no monitor: caller must degrade, not index with -1
    const MonitorGeometry& m = monitors[static_cast<size_t>(idx)];
    out.monitor = idx;
    out.x = px_to_dip(a.insertion_point.x - m.bounds.left, m.dpi);
    out.y = px_to_dip(a.insertion_point.y - m.bounds.top, m.dpi);
    out.line_height = px_to_dip(a.line_height, m.dpi);
    return out;
}

Rect capture_region_for(Point insertion_point, const AnchorConfig& cfg,
                        const MonitorGeometry& monitor) noexcept {
    const float s = dpi_scale(monitor.dpi);
    int32_t w = std::max(1, static_cast<int32_t>(std::lround(static_cast<double>(cfg.capture_width) * s)));
    int32_t h = std::max(1, static_cast<int32_t>(std::lround(static_cast<double>(cfg.capture_height) * s)));

    const Rect& b = monitor.bounds;
    const bool clamp = !b.empty();
    if (clamp) {
        w = std::min(w, b.width());
        h = std::min(h, b.height());
    }

    // The point sits right-of-centre because text grows rightward from the insertion point: the
    // interesting pixels are mostly *behind* it, and the room in front only has to cover one
    // commit. (For an RTL target this is backwards, which is one more reason RTL is scored down
    // rather than claimed as supported.)
    Rect r;
    r.left = insertion_point.x - (w * 65) / 100;
    r.top = insertion_point.y - h / 2;
    r.right = r.left + w;
    r.bottom = r.top + h;

    if (clamp) {
        if (r.right > b.right) { r.left -= r.right - b.right; r.right = b.right; }
        if (r.left < b.left) { r.right += b.left - r.left; r.left = b.left; }
        if (r.bottom > b.bottom) { r.top -= r.bottom - b.bottom; r.bottom = b.bottom; }
        if (r.top < b.top) { r.bottom += b.top - r.top; r.top = b.top; }
    }
    return r;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Windows-facing half. Everything below touches the desktop; nothing below is unit-tested,
//  which is precisely why the interesting logic is all above.
// ═══════════════════════════════════════════════════════════════════════════════════════════

namespace {

MonitorGeometry geometry_of_monitor(HMONITOR mon) noexcept {
    MonitorGeometry g;
    g.dpi = USER_DEFAULT_SCREEN_DPI;
    if (mon == nullptr) return g;

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        // rcMonitor, not rcWork: this rectangle bounds a *capture*, and the taskbar's pixels are
        // as capturable as any other. rcWork is the overlay's business (overlay.cpp), because a
        // preview under the taskbar is unreadable — a diff under it is not.
        g.bounds.left = mi.rcMonitor.left;
        g.bounds.top = mi.rcMonitor.top;
        g.bounds.right = mi.rcMonitor.right;
        g.bounds.bottom = mi.rcMonitor.bottom;
    }

    UINT dx = 0;
    UINT dy = 0;
    if (SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy)) && dx != 0) {
        // dy is read and ignored rather than averaged: Windows reports square DPI for every
        // display it supports, and silently averaging a non-square value would hide something we
        // do not model.
        g.dpi = static_cast<uint32_t>(dx);
    }
    return g;
}

}  // namespace

MonitorGeometry monitor_geometry_for(Point p) noexcept {
    const POINT pt{p.x, p.y};
    return geometry_of_monitor(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST));
}

MonitorGeometry monitor_geometry_for_window(uintptr_t window) noexcept {
    HWND h = reinterpret_cast<HWND>(window);
    if (h == nullptr || IsWindow(h) == FALSE) {
        return geometry_of_monitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    }
    return geometry_of_monitor(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST));
}

namespace {

// GetDC(nullptr) must be released with ReleaseDC(nullptr, dc), not DeleteDC. Acquired and
// released per capture rather than cached for the object's lifetime: a display-mode change, a
// remote-session transition or a monitor being unplugged can invalidate a cached screen DC, and
// re-acquiring a few times a second costs nothing measurable.
struct ScopedScreenDc {
    HDC dc = nullptr;
    ScopedScreenDc() : dc(GetDC(nullptr)) {}
    ~ScopedScreenDc() { if (dc != nullptr) ReleaseDC(nullptr, dc); }
    ScopedScreenDc(const ScopedScreenDc&) = delete;
    ScopedScreenDc& operator=(const ScopedScreenDc&) = delete;
};

struct ScopedDc {
    HDC dc = nullptr;
    ~ScopedDc() { if (dc != nullptr) DeleteDC(dc); }
    ScopedDc() = default;
    ScopedDc(const ScopedDc&) = delete;
    ScopedDc& operator=(const ScopedDc&) = delete;
    HDC release() noexcept { HDC d = dc; dc = nullptr; return d; }
};

struct ScopedBitmap {
    HBITMAP bmp = nullptr;
    ~ScopedBitmap() { if (bmp != nullptr) DeleteObject(bmp); }
    ScopedBitmap() = default;
    ScopedBitmap(const ScopedBitmap&) = delete;
    ScopedBitmap& operator=(const ScopedBitmap&) = delete;
    HBITMAP release() noexcept { HBITMAP b = bmp; bmp = nullptr; return b; }
};

Rect window_rect_of(uintptr_t window) noexcept {
    Rect r;
    HWND hwnd = reinterpret_cast<HWND>(window);
    RECT rc{};
    if (hwnd != nullptr && IsWindow(hwnd) && GetWindowRect(hwnd, &rc)) {
        r = Rect{rc.left, rc.top, rc.right, rc.bottom};
    }
    return r;
}

class BitBltCapture final : public IScreenCapture {
public:
    explicit BitBltCapture(bool use_captureblt) noexcept : captureblt_(use_captureblt) {}
    ~BitBltCapture() override { release_surface(); }

    BitBltCapture(const BitBltCapture&) = delete;
    BitBltCapture& operator=(const BitBltCapture&) = delete;

    Result<BitmapView> capture(const Rect& region) override {
        if (region.empty()) {
            return Status{StatusCode::InvalidArgument, "capture region is empty"};
        }
        warn_once_if_not_per_monitor_v2();

        ScopedScreenDc screen;
        if (screen.dc == nullptr) {
            return Status{StatusCode::OsError, "GetDC(NULL) failed", GetLastError()};
        }

        if (Status s = ensure_surface(screen.dc, region.width(), region.height()); !s) return s;

        // CAPTUREBLT is what makes layered windows appear in the result; without it tooltips, IME
        // candidate lists and some Electron popups are simply absent from the capture. It is also
        // documented to cause a visible flicker in some configurations, which is why it is a
        // config knob (AnchorConfig::use_captureblt) and a Phase 0B measurement rather than a
        // decision made from the documentation.
        //
        // Our own overlay is excluded from every capture surface by SetWindowDisplayAffinity(
        // WDA_EXCLUDEFROMCAPTURE) in overlay.cpp. Without that, the estimator would see the
        // pending text it just drew and score it as the target's response to an injection.
        const DWORD rop = SRCCOPY | (captureblt_ ? CAPTUREBLT : 0u);
        if (!BitBlt(mem_dc_, 0, 0, w_, h_, screen.dc, region.left, region.top, rop)) {
            return Status{StatusCode::OsError, "BitBlt from the desktop DC failed", GetLastError()};
        }

        // A DIB section's bits are not guaranteed current until the GDI batch is flushed. Reading
        // them without this yields intermittently stale pixels, which presents as a diff that is
        // empty exactly when the machine is busy.
        GdiFlush();

        last_black_ = surface_is_black();

        BitmapView v;
        v.pixels = bits_;
        v.width = w_;
        v.height = h_;
        v.stride = w_ * 4;
        return v;
    }

    [[nodiscard]] bool last_capture_was_black() const override { return last_black_; }
    [[nodiscard]] const char* name() const override { return "bitblt"; }

private:
    Status ensure_surface(HDC reference_dc, int32_t w, int32_t h) {
        if (mem_dc_ != nullptr && dib_ != nullptr && w == w_ && h == h_) return Status::ok();
        release_surface();

        ScopedDc dc;
        dc.dc = CreateCompatibleDC(reference_dc);
        if (dc.dc == nullptr) {
            return Status{StatusCode::OsError, "CreateCompatibleDC failed", GetLastError()};
        }

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        // Negative height = top-down DIB, which is what BitmapView documents. A bottom-up DIB
        // would make every row index in the diff wrong in a way that still produces plausible
        // pictures, so it is not worth "supporting".
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        ScopedBitmap bmp;
        bmp.bmp = CreateDIBSection(reference_dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp.bmp == nullptr || bits == nullptr) {
            return Status{StatusCode::OsError, "CreateDIBSection failed", GetLastError()};
        }

        HGDIOBJ old = SelectObject(dc.dc, bmp.bmp);
        if (old == nullptr || old == HGDI_ERROR) {
            return Status{StatusCode::OsError, "SelectObject on the capture DIB failed",
                          GetLastError()};
        }

        // Commit only after every step succeeded; the scoped wrappers above have released
        // everything on each early return.
        old_bitmap_ = old;
        mem_dc_ = dc.release();
        dib_ = bmp.release();
        bits_ = static_cast<uint8_t*>(bits);
        w_ = w;
        h_ = h;
        return Status::ok();
    }

    void release_surface() noexcept {
        if (mem_dc_ != nullptr && old_bitmap_ != nullptr) SelectObject(mem_dc_, old_bitmap_);
        if (dib_ != nullptr) DeleteObject(dib_);
        if (mem_dc_ != nullptr) DeleteDC(mem_dc_);
        old_bitmap_ = nullptr;
        dib_ = nullptr;
        mem_dc_ = nullptr;
        bits_ = nullptr;
        w_ = 0;
        h_ = 0;
    }

    [[nodiscard]] bool surface_is_black() const noexcept {
        if (bits_ == nullptr) return false;
        const size_t count = static_cast<size_t>(w_) * static_cast<size_t>(h_);
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* p = bits_ + i * 4u;
            if ((p[0] | p[1] | p[2]) != 0) return false;
        }
        return true;
    }

    void warn_once_if_not_per_monitor_v2() noexcept {
        if (warned_dpi_) return;
        warned_dpi_ = true;
        const DPI_AWARENESS_CONTEXT ctx = GetThreadDpiAwarenessContext();
        if (!AreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
            // Not fatal, but every anchor on a scaled monitor will be wrong: without per-monitor
            // v2 the desktop DC's coordinates are virtualised and BitBlt hands back a stretched
            // approximation of the wrong rectangle. The manifest that fixes this is the TODO in
            // CMakeLists.txt under the kvit-dictate target.
            KD_LOG_WARN("process is not PROCESS_PER_MONITOR_DPI_AWARE_V2; captured rectangles are "
                        "virtualised and anchors on scaled monitors will be wrong");
        }
    }

    HDC mem_dc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ old_bitmap_ = nullptr;
    uint8_t* bits_ = nullptr;
    int32_t w_ = 0;
    int32_t h_ = 0;
    bool captureblt_ = true;
    bool last_black_ = false;
    bool warned_dpi_ = false;
};

class GuiThreadCaretProvider final : public ICaretProvider {
public:
    bool caret_rect(uintptr_t window, Rect& out) override {
        HWND hwnd = reinterpret_cast<HWND>(window);
        if (hwnd == nullptr || !IsWindow(hwnd)) return false;

        const DWORD tid = GetWindowThreadProcessId(hwnd, nullptr);
        if (tid == 0) return false;

        GUITHREADINFO gti{};
        gti.cbSize = sizeof(gti);
        if (!GetGUIThreadInfo(tid, &gti)) return false;

        // rcCaret is in the client coordinates of hwndCaret, not of `window` and not of the
        // screen. With no hwndCaret there is no caret and rcCaret is meaningless — which is the
        // common case, because most modern applications draw their own caret and never create a
        // system one. That is not an error; it is why sources B and C exist.
        if (gti.hwndCaret == nullptr) return false;

        const RECT rc = gti.rcCaret;
        if (rc.right <= rc.left || rc.bottom <= rc.top) return false;

        // Deliberately *not* requiring GUI_CARETBLINKING: a steady (non-blinking) cursor is one of
        // the Phase 0B scenarios, and the flag would reject exactly that case.

        POINT tl{rc.left, rc.top};
        POINT br{rc.right, rc.bottom};
        if (!ClientToScreen(gti.hwndCaret, &tl)) return false;
        if (!ClientToScreen(gti.hwndCaret, &br)) return false;

        // A window with WS_EX_LAYOUTRTL has a mirrored client area, and ClientToScreen mirrors
        // with it — so the converted "left" can land to the right of the converted "right".
        // Normalising here rather than assuming ordering is the difference between an anchor that
        // works in an RTL layout and a negative-width rectangle.
        out.left = std::min(tl.x, br.x);
        out.right = std::max(tl.x, br.x);
        out.top = std::min(tl.y, br.y);
        out.bottom = std::max(tl.y, br.y);
        return !out.empty();
    }
};

}  // namespace

std::unique_ptr<IScreenCapture> make_bitblt_capture(bool use_captureblt) {
    return std::unique_ptr<IScreenCapture>(new BitBltCapture(use_captureblt));
}

Status make_wgc_capture(std::unique_ptr<IScreenCapture>* out) {
    if (out != nullptr) out->reset();
    // Deliberately unimplemented. plan.md §1.5 requires WGC's exact desktop API path *and any
    // user-visible capture indicator* to be prototyped in Phase 0B rather than assumed
    // transparent; a privacy tool that silently lights a "your screen is being recorded" badge is
    // not something to ship on the strength of a documentation reading. Writing an unverifiable
    // implementation here would turn a known gap into an unknown bug.
    return Status{StatusCode::Unsupported,
                  "Windows.Graphics.Capture fallback is not implemented (plan.md §1.5 Phase 0B: "
                  "the desktop capture path and its capture indicator must be prototyped first). "
                  "Source C is unavailable on this display; sources A and B still apply."};
}

std::unique_ptr<ICaretProvider> make_gui_thread_caret_provider() {
    return std::unique_ptr<ICaretProvider>(new GuiThreadCaretProvider());
}

// ─── The estimator ──────────────────────────────────────────────────────────────────────────

struct AnchorEstimator::Impl {
    struct Job {
        enum class Kind : uint8_t { BeforeFrame, AfterFrame, Notify };
        Kind kind = Kind::BeforeFrame;
        Rect region;
        uintptr_t window = 0;
        std::string text;   // copied: submit_after_frame's view does not outlive the call
        Anchor anchor;
    };

    // Bounded queue. If the UI thread outruns the worker the oldest job is dropped and counted,
    // in the same spirit as util/log.h: falling behind must not turn into unbounded memory, and
    // the estimator has nothing so precious that blocking a caller for it would be right.
    static constexpr size_t kMaxJobs = 16;

    Impl(AnchorConfig c, IScreenCapture* cap, ICaretProvider* car, IAnchorListener* l) noexcept
        : cfg(c), capture(cap), caret(car), listener(l) {}

    AnchorConfig cfg;
    IScreenCapture* capture = nullptr;
    ICaretProvider* caret = nullptr;
    IAnchorListener* listener = nullptr;

    mutable std::mutex m;
    std::condition_variable cv;
    std::thread worker;
    bool running = false;
    bool stopping = false;
    std::deque<Job> jobs;
    uint64_t dropped_jobs = 0;

    // Shared state, guarded by `m`.
    std::optional<Anchor> current;
    ClickAnchor click;
    bool wgc_required = false;
    bool invalidate_pending = false;

    // Worker-thread-only state. No lock, because nothing else touches it.
    std::vector<uint8_t> before_pixels;
    int32_t before_w = 0;
    int32_t before_h = 0;
    Rect before_region;
    uintptr_t before_window = 0;
    Rect before_window_rect;
    bool have_before = false;

    static uint64_t now_ms() noexcept { return SteadyClock::instance().now_ms(); }

    void push(Job job) {
        uint64_t dropped = 0;
        {
            std::lock_guard<std::mutex> lk(m);
            if (!running) return;  // "Until start(), submit_* calls are ignored" (anchor.h)
            if (jobs.size() >= kMaxJobs) {
                jobs.pop_front();
                ++dropped_jobs;
                dropped = dropped_jobs;
            }
            jobs.push_back(std::move(job));
        }
        if (dropped != 0) {
            KD_LOG_WARN("anchor job queue full; oldest job dropped (%llu total). The estimator is "
                        "behind the commit rate — anchors will lag, nothing else breaks.",
                        static_cast<unsigned long long>(dropped));
        }
        cv.notify_one();
    }

    [[nodiscard]] bool stop_requested() {
        std::lock_guard<std::mutex> lk(m);
        return stopping;
    }

    // Applies hysteresis and, if the candidate wins, records it and asks the worker to notify.
    // The listener is never called from here: anchor.h promises on_anchor arrives on the worker
    // thread, and sources A and B are read on the UI thread.
    void publish(const Anchor& a) {
        bool notify = false;
        {
            std::lock_guard<std::mutex> lk(m);
            if (!should_replace_anchor(current, a, cfg, a.timestamp_ms)) return;
            current = a;
            notify = running && listener != nullptr;
        }
        if (notify) {
            Job j;
            j.kind = Job::Kind::Notify;
            j.anchor = a;
            push(std::move(j));
        }
    }

    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [this] { return stopping || !jobs.empty(); });
                // Queued captures are worthless once we are stopping — the target is gone or the
                // session ended — so they are dropped rather than drained.
                if (stopping) return;
                job = std::move(jobs.front());
                jobs.pop_front();
            }

            switch (job.kind) {
                case Job::Kind::BeforeFrame: handle_before(job); break;
                case Job::Kind::AfterFrame: handle_after(job); break;
                case Job::Kind::Notify:
                    if (listener != nullptr) listener->on_anchor(job.anchor);
                    break;
            }
        }
    }

    void handle_before(const Job& job) {
        have_before = false;
        if (capture == nullptr) return;

        Result<BitmapView> r = capture->capture(job.region);
        if (!r) {
            // Status::to_string() is declared in util/result.h but is not implemented by any
            // source file in CMakeLists yet, so this logs the parts that are inline accessors.
            KD_LOG_DEBUG("before-frame capture failed (code %u, os 0x%08X): %s",
                         static_cast<unsigned>(r.status().code()), r.status().os_error(),
                         r.status().message().c_str());
            return;
        }
        const BitmapView& v = *r;
        if (!v.is_valid()) return;

        if (capture->last_capture_was_black()) {
            note_black_capture();
            return;
        }

        // **Copy.** IScreenCapture documents the returned view as valid only until the next
        // capture from the same object — and the after-frame poll below performs several. Holding
        // the pointer would diff a buffer against itself and report an empty change on every
        // commit, which is the most plausible-looking bug this module could have.
        copy_frame(v);
        before_region = job.region;
        before_window = job.window;
        before_window_rect = window_rect_of(job.window);
        have_before = true;
    }

    void copy_frame(const BitmapView& v) {
        before_w = v.width;
        before_h = v.height;
        const size_t row_bytes = static_cast<size_t>(before_w) * 4u;
        before_pixels.resize(row_bytes * static_cast<size_t>(before_h));
        for (int32_t y = 0; y < before_h; ++y) {
            std::memcpy(before_pixels.data() + static_cast<size_t>(y) * row_bytes,
                        v.pixels + static_cast<size_t>(y) * static_cast<size_t>(v.stride),
                        row_bytes);
        }
    }

    void note_black_capture() {
        bool first = false;
        {
            std::lock_guard<std::mutex> lk(m);
            first = !wgc_required;
            wgc_required = true;
        }
        if (first) {
            KD_LOG_WARN("capture came back entirely black: HDR / multi-plane overlay, or a "
                        "genuinely black target — either way BitBlt sees nothing here. Source C "
                        "needs the Windows.Graphics.Capture fallback, which is not implemented "
                        "(plan.md §1.5, Phase 0B).");
        }
    }

    void handle_after(const Job& job) {
        if (!have_before) return;
        have_before = false;  // one after-frame per before-frame, always

        {
            std::lock_guard<std::mutex> lk(m);
            if (invalidate_pending) {
                // A seq gap landed between the two frames: we no longer believe we know what text
                // was injected, so the change on screen cannot be attributed to it.
                invalidate_pending = false;
                return;
            }
        }
        if (capture == nullptr) return;

        const bool whitespace_only = is_ascii_whitespace_only(job.text);
        const uint64_t deadline = now_ms() + static_cast<uint64_t>(cfg.after_frame_poll_ms);

        DiffResult best;
        Rect after_region = before_region;
        bool have_diff = false;

        // Async-rendering targets (Electron, WSLg over RDP) do not repaint synchronously with the
        // injection, so the after-frame is polled to a deadline rather than captured once. A
        // single capture here is the difference between "works in Notepad" and "works".
        for (;;) {
            Rect region = before_region;
            if (before_window != 0) {
                const Rect now_rect = window_rect_of(before_window);
                if (now_rect.empty()) break;  // window gone; nothing to compare against
                if (now_rect.width() != before_window_rect.width() ||
                    now_rect.height() != before_window_rect.height()) {
                    // A resize relaid out the text. There is no sound way to align the two
                    // frames, so the pair is discarded rather than diffed into a confident-looking
                    // nonsense rectangle.
                    KD_LOG_DEBUG("target window resized between frames; discarding the pair");
                    return;
                }
                // The window merely moved: shift the capture rectangle by the same delta so both
                // frames cover the same *content*.
                const int32_t dx = now_rect.left - before_window_rect.left;
                const int32_t dy = now_rect.top - before_window_rect.top;
                region.left += dx;
                region.right += dx;
                region.top += dy;
                region.bottom += dy;
            }

            Result<BitmapView> r = capture->capture(region);
            if (!r) {
                KD_LOG_DEBUG("after-frame capture failed (code %u, os 0x%08X): %s",
                             static_cast<unsigned>(r.status().code()), r.status().os_error(),
                             r.status().message().c_str());
                break;
            }
            const BitmapView before_view{before_pixels.data(), before_w, before_h, before_w * 4};
            best = diff_bitmaps(before_view, *r, cfg.diff_tolerance, cfg.diff_merge_gap_px);
            after_region = region;
            have_diff = true;

            if (best.all_black_before || best.all_black_after) break;
            if (best.changed_pixels > 0 && !best.looks_like_scroll) break;
            if (now_ms() >= deadline) break;

            std::unique_lock<std::mutex> lk(m);
            cv.wait_for(lk, std::chrono::milliseconds(cfg.after_frame_poll_interval_ms),
                        [this] { return stopping; });
            if (stopping) return;
        }

        if (!have_diff) return;

        if (best.all_black_before || best.all_black_after) {
            note_black_capture();
            return;
        }

        if (best.changed_pixels == 0) {
            // **An empty diff is not a failure.** A whitespace-only or newline-only commit
            // produces little or no visible change; text can land outside the visible region; the
            // target may simply not have repainted within the deadline. The previous anchor is
            // kept and nothing is inferred about the injection — injector.h owns that verdict.
            KD_LOG_DEBUG("empty diff after a %s commit; keeping the previous anchor (this says "
                         "nothing about whether the injection succeeded)",
                         whitespace_only ? "whitespace-only" : "visible");
            return;
        }

        std::optional<Anchor> previous;
        {
            std::lock_guard<std::mutex> lk(m);
            previous = current;
        }

        const std::optional<Anchor> candidate =
            anchor_from_diff(best, after_region, previous, now_ms());
        if (!candidate) {
            // Scored zero: a scroll, or a shape we cannot place. Discard, keep the previous
            // anchor. This is the defined behaviour, not a dropped update.
            KD_LOG_DEBUG("diff scored zero (rows=%d regions=%d scroll=%d); keeping the previous "
                         "anchor",
                         best.changed_rows, best.changed_regions, best.looks_like_scroll ? 1 : 0);
            return;
        }
        publish(*candidate);
    }
};

AnchorEstimator::AnchorEstimator(AnchorConfig cfg, IScreenCapture* capture, ICaretProvider* caret,
                                 IAnchorListener* listener)
    : impl_(new Impl(cfg, capture, caret, listener)) {}

AnchorEstimator::~AnchorEstimator() {
    stop();
    delete impl_;
}

Status AnchorEstimator::start() {
    {
        std::lock_guard<std::mutex> lk(impl_->m);
        if (impl_->running) return Status::ok();
        impl_->stopping = false;
        impl_->running = true;
    }
    try {
        impl_->worker = std::thread([this] { impl_->run(); });
    } catch (const std::system_error& e) {
        std::lock_guard<std::mutex> lk(impl_->m);
        impl_->running = false;
        return Status{StatusCode::Internal,
                      std::string("could not start the anchor worker thread: ") + e.what()};
    }
    return Status::ok();
}

void AnchorEstimator::stop() {
    {
        std::lock_guard<std::mutex> lk(impl_->m);
        if (!impl_->running && !impl_->worker.joinable()) return;
        impl_->stopping = true;
        impl_->running = false;
    }
    impl_->cv.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->jobs.clear();
    impl_->stopping = false;
}

void AnchorEstimator::set_last_click(Point p, uintptr_t window, uint64_t timestamp_ms) {
    set_last_click(MouseButton::Left, p, window, timestamp_ms);
}

void AnchorEstimator::set_last_click(MouseButton button, Point p, uintptr_t window,
                                     uint64_t timestamp_ms) {
    // The hook hands us the window *under the cursor*, which for a click into a text field is a
    // child control. The foreground window is always a top-level window, so the two would never
    // compare equal without this. GetAncestor is a local lookup — no message is sent to the target
    // and nothing can block on it, which is what plan.md §1.5's no-cross-process rule is about.
    uintptr_t root = window;
    if (window != 0) {
        HWND h = GetAncestor(reinterpret_cast<HWND>(window), GA_ROOT);
        if (h != nullptr) root = reinterpret_cast<uintptr_t>(h);
    }

    std::optional<Anchor> candidate;
    {
        std::lock_guard<std::mutex> lk(impl_->m);
        impl_->click.on_click(button, p, root, timestamp_ms);
        const int32_t hint = impl_->current ? impl_->current->line_height : 0;
        candidate = impl_->click.evaluate(
            reinterpret_cast<uintptr_t>(GetForegroundWindow()), timestamp_ms, hint);
    }
    if (candidate) impl_->publish(*candidate);
}

void AnchorEstimator::invalidate_click() {
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->click.on_wheel();
    // The already-published anchor is deliberately left alone. If it came from the click it is now
    // wrong, but replacing a wrong position with no position mid-session is the flapping this
    // module exists to avoid; it ages out via stale_after_ms, or the next caret/diff replaces it.
}

void AnchorEstimator::downgrade_click_confidence() {
    std::optional<Anchor> candidate;
    {
        std::lock_guard<std::mutex> lk(impl_->m);
        impl_->click.on_keyboard_navigation();
        if (impl_->current && impl_->current->source == Source::LastClick) {
            const int32_t hint = impl_->current->line_height;
            candidate = impl_->click.evaluate(
                reinterpret_cast<uintptr_t>(GetForegroundWindow()), Impl::now_ms(), hint);
        }
    }
    if (candidate) impl_->publish(*candidate);
}

void AnchorEstimator::refresh_caret(uintptr_t window) {
    const uint64_t now = Impl::now_ms();

    Rect rc;
    if (impl_->caret != nullptr && impl_->caret->caret_rect(window, rc) && !rc.empty()) {
        Anchor a;
        // The caret rectangle *is* the insertion point: its left edge is where the next glyph
        // goes and its height is the line height, measured rather than guessed. This is the only
        // source in the module that is not an inference.
        a.insertion_point = Point{rc.left, rc.top};
        a.line_height = rc.height();
        a.confidence = kCaretConfidence;
        a.source = Source::Caret;
        a.timestamp_ms = now;
        impl_->publish(a);
        return;
    }

    // No system caret — the common case, because most modern applications draw their own. Fall
    // back to the cached click so that losing source A does not leave the overlay with nothing.
    std::optional<Anchor> candidate;
    {
        std::lock_guard<std::mutex> lk(impl_->m);
        const int32_t hint = impl_->current ? impl_->current->line_height : 0;
        candidate = impl_->click.evaluate(reinterpret_cast<uintptr_t>(GetForegroundWindow()), now,
                                          hint);
    }
    if (candidate) impl_->publish(*candidate);
}

void AnchorEstimator::submit_before_frame(const Rect& region) {
    submit_before_frame(region, 0);
}

void AnchorEstimator::submit_before_frame(const Rect& region, uintptr_t target_window) {
    Impl::Job j;
    j.kind = Impl::Job::Kind::BeforeFrame;
    j.region = region;
    j.window = target_window;
    impl_->push(std::move(j));
}

void AnchorEstimator::submit_after_frame(std::string_view injected_text) {
    Impl::Job j;
    j.kind = Impl::Job::Kind::AfterFrame;
    // Copy: the caller's view is into the commit engine's buffer and does not outlive this call.
    j.text.assign(injected_text);
    impl_->push(std::move(j));
}

void AnchorEstimator::reset_confidence() {
    std::lock_guard<std::mutex> lk(impl_->m);
    // docs/protocol.md §7.4: a seq gap means a hypothesis was missed, so "the change on screen was
    // produced by text we injected" is no longer sound. The position is kept — a stale position
    // beats no position — and only the belief in it is dropped.
    if (impl_->current) impl_->current->confidence = 0.0f;
    impl_->invalidate_pending = true;
}

std::optional<Anchor> AnchorEstimator::current() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    return impl_->current;
}

Fallback AnchorEstimator::current_fallback() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    return impl_->cfg.below_threshold;
}

bool AnchorEstimator::wgc_fallback_required() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    return impl_->wgc_required;
}

}  // namespace kd::anchor
