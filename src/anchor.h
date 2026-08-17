#pragma once
//
// Anchor estimation: where, on screen, to draw the pending-text overlay.
//
// Implements: plan.md §1.5. Three sources combined into one confidence-scored estimate, with a
// deterministic fallback below threshold.
//
// ─── What this module claims, and what revision 1 wrongly claimed ───────────────────────────
//
// Revision 1 of the plan called commit differencing "correct by construction", "universal", and
// a source of "free failure detection". None of that survives contact with the counterexamples,
// and the review was right to reject it. **This is a best-effort estimator with a confidence
// score.** It is allowed to be wrong; what it is not allowed to do is be wrong *silently* or
// *flappingly*.
//
// Only one property is recovered from the target: **where the insertion point is.** The
// overlay's appearance is fixed in settings — one font, size and colour everywhere — because
// font family is unrecoverable from pixels, colour sampling breaks on gradients and themed
// backgrounds, and a consistent appearance makes pending text legible *as* pending.
//
// **No accessibility APIs.** UIA is cross-process COM that can block for seconds on a busy
// target — the same target receiving our synthetic keystrokes — and it is absent in the targets
// that matter most here. WSLg is the sharpest case: those windows expose no UIA text pattern at
// all, which is precisely why Win+H refuses to dictate into them, and they are ordinary Windows
// windows for SendInput and BitBlt purposes.
//
// ─── The three sources (plan.md §1.5) ───────────────────────────────────────────────────────
//
//   A. GetGUIThreadInfo().rcCaret — high confidence when non-empty. Free, synchronous, no
//      capture. Correct for classic Win32 edit controls; empty for apps that draw their own
//      caret (which is most of the interesting ones).
//   B. Last left-click before the hotkey — medium. The mouse hook is already installed for
//      stop-on-input, so this is one cached point, and it is pure coordinates: it works in
//      WSLg, terminals, Electron, games. Validity rules are in hotkey.h, where the click is
//      captured; the important half is that **vertical placement is the reliable half** — the
//      click lands within the target line — so the line box is anchored on click Y and X is
//      treated as a hint.
//   C. Commit differencing — variable, scored per commit. Capture before and after an
//      injection; the changed region locates the text we just wrote.
//
// ─── Source C's failure cases, enumerated rather than discovered ────────────────────────────
//
// The changed region's right edge is *not* generally the insertion point:
//   * whitespace-only or newline-only commits produce little or no visible change;
//   * **line wrap** — the bounding box's right edge is on the previous line, so the estimator
//     must use the rightmost changed pixel of the *last changed text row*, not of the box;
//   * **RTL and bidirectional text** — the insertion point is at the *left* edge;
//   * target auto-scroll or horizontal scroll repaints the whole viewport;
//   * selection replacement changes a region that includes deleted text;
//   * syntax highlighting, spellcheck underlines and lint decorations repaint older text;
//   * block cursors, autocomplete popups, animation and caret blink all add changed regions;
//   * password fields render dots; some content is deliberately capture-excluded.
//
// A full-viewport change scores zero and is treated as a scroll: discard, keep the previous
// anchor. Hysteresis prevents flapping between sources.
//
// ─── Injection success is a separate signal ─────────────────────────────────────────────────
//
// An empty diff does **not** prove injection failed — whitespace can be accepted invisibly, and
// text can land outside the visible region. A non-empty diff does not prove the expected text
// arrived. Injection success is determined by injector.h, never inferred from pixels. Nothing
// in this header returns a success/failure verdict about injection, and nothing may be added
// that does.
//
// ─── Capture mechanics ──────────────────────────────────────────────────────────────────────
//
// BitBlt from the desktop DC — post-composition, so it sees Chromium, WSLg, everything DWM
// draws. Our own overlay is excluded via SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)
// (overlay.h), which is what stops the overlay contaminating its own input.
//
// An all-black capture indicates an HDR / multi-plane-overlay path, and the fallback is
// Windows.Graphics.Capture — **whose exact desktop API path and any user-visible capture
// indicator must be prototyped in Phase 0B rather than assumed transparent.** This header
// declares the seam (IScreenCapture) and does not pretend the fallback is already understood.
//
// ─── Threading ──────────────────────────────────────────────────────────────────────────────
//
// **Capture and diff run on a worker thread. No COM, no cross-process calls, ever.** That is
// plan.md §1.5's rule and it is what keeps a slow or hung target from freezing the UI.
//
//   * `AnchorEstimator::submit_*` is called from the UI thread and returns immediately.
//   * `IAnchorListener::on_anchor` is invoked on the worker thread; post it to the UI thread.
//   * `diff_bitmaps()` and `score_diff()` are pure and callable from anywhere — they are the
//     unit-tested part, fed synthetic bitmaps (plan.md §7: "capture/diff fixtures").
//   * Source A (GetGUIThreadInfo) is cheap and synchronous and is read on the UI thread.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "util/result.h"

namespace kd::anchor {

// ─── Pure geometry and pixels ───────────────────────────────────────────────────────────────
//
// Plain structs rather than RECT/POINT so the diff logic compiles and tests without Windows
// types leaking into the pure half. Conversion happens at the Win32 call sites.

struct Point {
    int32_t x = 0;
    int32_t y = 0;
};

struct Rect {
    int32_t left = 0, top = 0, right = 0, bottom = 0;
    [[nodiscard]] int32_t width() const noexcept { return right - left; }
    [[nodiscard]] int32_t height() const noexcept { return bottom - top; }
    [[nodiscard]] bool empty() const noexcept { return right <= left || bottom <= top; }
};

// A 32-bit BGRA top-down bitmap. Non-owning; `pixels` must outlive the call.
// **All coordinates in this module are physical pixels**, not DIPs: the process is
// per-monitor-DPI-aware v2 and captured rectangles are physical (plan.md §1.3).
struct BitmapView {
    const uint8_t* pixels = nullptr;
    int32_t width = 0;
    int32_t height = 0;
    int32_t stride = 0;  // bytes per row, may exceed width * 4

    [[nodiscard]] bool is_valid() const noexcept;
};

// The result of comparing a before/after capture of the same screen rectangle.
//
// **All rectangles here are bitmap-local** (origin at the top-left of the captured region), not
// screen coordinates. anchor_from_diff() is what places them on the screen. Rects are half-open:
// `right` is one past the rightmost changed pixel, which is also where the next glyph would
// start — convenient, and the reason the insertion point is `right` and not `right - 1`.
struct DiffResult {
    Rect changed_bounds;            // bounding box of all changed pixels, empty if none
    int32_t changed_rows = 0;       // number of pixel rows containing any change
    int32_t changed_pixels = 0;
    Rect last_changed_row_extent;   // the LAST changed *text* row — the line-wrap fix: the
                                    // bounding box's right edge can belong to the previous line,
                                    // this one does not. "Text row" here means a maximal run of
                                    // consecutive changed pixel rows; the separator is a fully
                                    // unchanged pixel row, which normal line spacing provides.
                                    // Tight leading, or a descender that touches the next line's
                                    // ascender, defeats the separation and merges two lines into
                                    // one band — a known limit, not a handled case.
    bool looks_like_scroll = false; // change covers most of the region: treat as scroll
    bool all_black_before = false;  // HDR / multi-plane-overlay indicator; BitBlt returned black
    bool all_black_after = false;

    // Number of disjoint changed regions, 8-connected, after horizontally merging runs closer
    // than `merge_gap_px` (so the letters of one word do not count as three regions). Capped at
    // kMaxTrackedRegions — past that the exact count carries no more information than "many".
    //
    // This is the fragmentation signal: syntax highlighting, spellcheck underlines, lint
    // decorations, autocomplete popups, block cursors, caret blink and animation all add regions,
    // and a diff with many of them is not telling us where one commit landed.
    int32_t changed_regions = 0;
};

inline constexpr int32_t kMaxTrackedRegions = 64;

// Pure. `before` and `after` must have identical dimensions; returns an empty diff otherwise
// rather than reading out of bounds.
//
// `tolerance` is a per-channel absolute difference below which a pixel counts as unchanged.
// It is not zero because subpixel antialiasing and DWM's own dithering produce ±1 noise on
// pixels nobody touched, and a zero tolerance turns every capture into a full-viewport change.
//
// `merge_gap_px` is the horizontal gap below which two changed runs on the same row count as one
// region. Without it every glyph is its own connected component and the fragmentation signal
// above would fire on a perfectly ordinary one-word commit. It is in physical pixels, so on a
// 200 % monitor the default is effectively half as generous — see dpi_scale().
DiffResult diff_bitmaps(const BitmapView& before, const BitmapView& after, int tolerance = 8,
                        int merge_gap_px = 8);

// ─── Anchors ────────────────────────────────────────────────────────────────────────────────

enum class Source : uint8_t {
    None,        // no estimate at all
    Caret,       // A: GetGUIThreadInfo().rcCaret
    LastClick,   // B: cached left-click position
    CommitDiff,  // C: screen differencing
};

const char* to_string(Source s) noexcept;

struct Anchor {
    Point insertion_point;      // physical pixels, screen coordinates
    int32_t line_height = 0;    // estimated; drives the overlay's text size box
    bool rtl = false;           // the insertion point is at the LEFT edge of the changed region
    float confidence = 0.0f;    // 0.0 .. 1.0
    Source source = Source::None;
    uint64_t timestamp_ms = 0;

    [[nodiscard]] bool is_usable(float threshold) const noexcept {
        return source != Source::None && confidence >= threshold;
    }
};

// Confidence each source claims when it has something to say. These are *priors chosen by hand*,
// not measurements: Phase 0B is what will replace them with numbers. They live here rather than
// in the .cpp because the overlay's threshold and the tests both compare against them, and three
// copies of 0.95 in three files is how a threshold silently stops meaning anything.
inline constexpr float kCaretConfidence = 0.95f;            // source A, rcCaret non-empty
inline constexpr float kClickConfidence = 0.60f;            // source B, click still valid
inline constexpr float kClickDowngradedConfidence = 0.35f;  // source B after keyboard navigation
inline constexpr float kMaxDiffConfidence = 0.90f;          // source C never outranks a real caret

// Pure: score a diff as an anchor candidate. A compact single-row change near `previous` scores
// high; a full-viewport change scores zero. Isolated here so the scoring policy can be tuned
// against recorded Phase 0B captures without touching the capture code.
//
// `capture_region` must be the screen rectangle the two bitmaps were captured from: its width and
// height are what "compact" is measured against.
float score_diff(const DiffResult& d, const Rect& capture_region,
                 const std::optional<Anchor>& previous);

// Pure: place a scored diff on the screen. Returns nullopt when the diff carries no usable
// position at all — an empty diff, an all-black capture, or a scroll.
//
// **An empty diff is not a failure signal.** Whitespace-only and newline-only commits produce
// little or no visible change, text can land outside the visible region, and the target may
// simply not have repainted yet. The caller keeps its previous anchor and asks injector.h — never
// these pixels — whether the injection worked.
//
// RTL/bidi is *guessed*, not determined: if the changed region grew leftward from `previous` on
// the same line, `rtl` is set and the confidence is cut, because a right-aligned LTR paragraph
// and a selection replacement produce the same shape. With no previous anchor on the same line
// there is nothing to compare against and the estimator assumes LTR, which is simply wrong for an
// RTL target and is one of the things Phase 0B has to measure.
std::optional<Anchor> anchor_from_diff(const DiffResult& d, const Rect& capture_region,
                                       const std::optional<Anchor>& previous, uint64_t now_ms);

// ─── Degradation policy (plan.md §1.5) ──────────────────────────────────────────────────────
//
// "Below a confidence threshold the overlay degrades deterministically: floating pill near the
// target, or no preview at all, configurable. **This is a defined state, not a bug.**"

enum class Fallback : uint8_t {
    FloatingPill,  // a pill near the target window, position independent of the caret
    NoPreview,     // commit-only; nothing is drawn
};

const char* to_string(Fallback f) noexcept;

struct AnchorConfig {
    float min_confidence = 0.5f;
    Fallback below_threshold = Fallback::FloatingPill;

    // Hysteresis: a new source must beat the current one by this margin before the anchor
    // switches to it. Without it the estimate flaps between sources on alternate commits, which
    // is more distracting than a slightly wrong but stable position.
    float source_switch_margin = 0.15f;

    // How long an anchor stays usable without corroboration.
    uint32_t stale_after_ms = 3000;

    // The rectangle captured around the expected insertion point, in physical pixels. Larger
    // catches more line wrap and scroll; smaller is faster and less likely to include unrelated
    // repaints.
    int32_t capture_width = 900;
    int32_t capture_height = 220;

    // Async-rendering targets (Electron, WSLg over RDP) do not repaint synchronously with the
    // injection, so the after-frame is polled to a deadline rather than captured once.
    uint32_t after_frame_poll_ms = 120;
    uint32_t after_frame_poll_interval_ms = 16;

    int diff_tolerance = 8;
    int diff_merge_gap_px = 8;  // see diff_bitmaps()

    // SRCCOPY | CAPTUREBLT. CAPTUREBLT is what makes layered windows (tooltips, some Electron
    // popups, IME candidate lists) appear in the capture — but it is also documented to cause a
    // visible flicker on some configurations, and a flicker on every commit would be worse than
    // missing a tooltip. Left on by default and left as a knob because which way this goes is a
    // Phase 0B measurement, not something to decide from the documentation.
    bool use_captureblt = true;
};

// ─── Hysteresis (pure, so "it does not flap" is a test and not a claim) ──────────────────────
//
// True when `candidate` should replace `current`. Same source: always (that is just a refresh).
// Different source: only if it beats the incumbent by cfg.source_switch_margin, or the incumbent
// has gone stale. One bad frame from a rival source therefore cannot move the anchor.
bool should_replace_anchor(const std::optional<Anchor>& current, const Anchor& candidate,
                           const AnchorConfig& cfg, uint64_t now_ms);

// ─── Source B: the last left-click, as a pure state machine ─────────────────────────────────
//
// plan.md §1.5's validity rules, all of them, in one testable object. The mouse hook in hotkey.h
// already filters to left clicks before calling the estimator; this repeats the rule rather than
// trusting that, because the rule is cheap and the consequence of getting it wrong is the overlay
// parked over a context menu.

enum class MouseButton : uint8_t { Left, Right, Middle, Other };

struct ClickAnchor {
    Point position;
    uintptr_t window = 0;      // the HWND under the click, opaque
    uint64_t timestamp_ms = 0;
    bool valid = false;
    bool downgraded = false;   // keyboard navigation since the click

    // Non-left buttons are ignored outright and leave the cached click untouched: right-click
    // opens a context menu (so the click position is a menu, not a caret) and middle-click pastes
    // on X11, which WSLg targets are.
    void on_click(MouseButton b, Point p, uintptr_t w, uint64_t t) noexcept;

    // Scrolling moves the caret with no click, so the cached position is now meaningless.
    // Invalidate, do not downgrade.
    void on_wheel() noexcept;

    // Arrow keys / Home / End since the click: the caret has moved but is probably still in the
    // same region. Downgrade rather than discard — a stale position beats no position.
    void on_keyboard_navigation() noexcept;

    void clear() noexcept;

    // `foreground_window` is the *current* foreground HWND: the click is only usable if the window
    // it landed in is still the foreground one. `line_height_hint` is whatever the caller knows
    // about text size (0 if nothing).
    //
    // Vertical placement is the reliable half — the click lands inside the target line — so the
    // line box is anchored on click Y and X is only a hint (clicking past end-of-line snaps the
    // caret left, clicking an empty field says nothing about X at all).
    [[nodiscard]] std::optional<Anchor> evaluate(uintptr_t foreground_window, uint64_t now_ms,
                                                 int32_t line_height_hint) const;
};

// ─── DPI and multi-monitor (plan.md §1.3, §7) ───────────────────────────────────────────────
//
// The process is PROCESS_PER_MONITOR_DPI_AWARE_V2, so everything captured and every coordinate
// crossing this module is a **physical pixel**. The target may be on a different monitor at a
// different scale from the one the overlay's font size was authored for, which is why these
// conversions exist as pure functions with tests rather than as scattered multiplications.

struct MonitorGeometry {
    Rect bounds;        // physical pixels, virtual-desktop coordinates (may be negative)
    uint32_t dpi = 96;  // effective DPI: 96 = 100 %, 120 = 125 %, 144 = 150 %, 192 = 200 %
};

// -1 when the point is on no monitor at all, which happens for real: a window can be dragged
// partly off the desktop, and a cached click can outlive a monitor being unplugged. Callers must
// degrade rather than index with -1.
int32_t monitor_index_for(const std::vector<MonitorGeometry>& monitors, Point p) noexcept;

float dpi_scale(uint32_t dpi) noexcept;                // dpi / 96
float px_to_dip(int32_t px, uint32_t dpi) noexcept;
int32_t dip_to_px(float dip, uint32_t dpi) noexcept;   // rounds half away from zero

// The anchor in DIPs relative to its monitor's top-left — what a Direct2D render target on that
// monitor wants. `monitor` is -1 (and the rest zero) if the anchor is on no monitor.
struct AnchorDip {
    float x = 0.0f;
    float y = 0.0f;
    float line_height = 0.0f;
    int32_t monitor = -1;
};

AnchorDip to_monitor_dips(const Anchor& a, const std::vector<MonitorGeometry>& monitors) noexcept;

// The rectangle to capture around an expected insertion point, clamped to the monitor.
//
// cfg.capture_width/height are physical pixels *at 96 dpi* and are scaled by the monitor's DPI,
// so the captured region covers the same amount of text on every monitor. Without that scaling a
// 200 % display would capture half as much text and lose the line-wrap case the width is there to
// catch. The point sits right-of-centre because text grows rightward from it.
Rect capture_region_for(Point insertion_point, const AnchorConfig& cfg,
                        const MonitorGeometry& monitor) noexcept;

// ─── The Windows-facing seams ───────────────────────────────────────────────────────────────
//
// Both are interfaces so the estimator can be driven from recorded captures in tests, and so
// the WGC fallback can be introduced as a second implementation rather than a branch inside
// the estimator.

class IScreenCapture {
public:
    virtual ~IScreenCapture() = default;

    // Capture a screen rectangle in physical pixels. The returned view is owned by the
    // implementation and remains valid until the next capture from the same object.
    virtual Result<BitmapView> capture(const Rect& region) = 0;

    // True when the last capture came back entirely black, the documented symptom of an
    // HDR/MPO path that BitBlt cannot see. Callers switch to the WGC implementation.
    [[nodiscard]] virtual bool last_capture_was_black() const = 0;

    [[nodiscard]] virtual const char* name() const = 0;  // "bitblt" | "wgc"
};

// Source A, behind an interface so tests need no real window.
class ICaretProvider {
public:
    virtual ~ICaretProvider() = default;
    // `window` is an opaque HWND, passed as uintptr_t so this header stays Windows-free.
    // Returns false when the target draws its own caret, which is the common case.
    virtual bool caret_rect(uintptr_t window, Rect& out) = 0;
};

class IAnchorListener {
public:
    virtual ~IAnchorListener() = default;
    // Called on the worker thread. Post to the UI thread; do not draw here.
    virtual void on_anchor(const Anchor& a) = 0;
};

// ─── Concrete implementations of the two seams ──────────────────────────────────────────────

// BitBlt from the desktop DC. Post-composition, so it sees Chromium, WSLg and everything else
// DWM draws. Not thread-safe: it owns a memory DC and a DIB section and is driven from the
// estimator's worker thread only.
std::unique_ptr<IScreenCapture> make_bitblt_capture(bool use_captureblt = true);

// **Windows.Graphics.Capture is NOT implemented, and this function says so instead of pretending
// otherwise.** It returns StatusCode::Unsupported.
//
// plan.md §1.5 is explicit that WGC's exact desktop API path *and any user-visible capture
// indicator* must be prototyped in Phase 0B rather than assumed transparent — a fallback that
// silently lights a "your screen is being recorded" badge on a privacy tool is not a fallback we
// can ship unmeasured. Writing an unverifiable implementation here would convert a known gap into
// an unknown bug, so the seam exists and the body does not.
//
// The path that leads here is real and detected: an all-black BitBlt capture (HDR / multi-plane
// overlay), reported by AnchorEstimator::wgc_fallback_required().
Status make_wgc_capture(std::unique_ptr<IScreenCapture>* out);

// Source A over GetGUIThreadInfo. Cheap, synchronous, no capture, no COM.
std::unique_ptr<ICaretProvider> make_gui_thread_caret_provider();

// ─── The estimator ──────────────────────────────────────────────────────────────────────────

class AnchorEstimator {
public:
    AnchorEstimator(AnchorConfig cfg, IScreenCapture* capture, ICaretProvider* caret,
                    IAnchorListener* listener);
    ~AnchorEstimator();
    AnchorEstimator(const AnchorEstimator&) = delete;
    AnchorEstimator& operator=(const AnchorEstimator&) = delete;

    // Starts the worker thread. Until then, submit_* calls are ignored.
    Status start();
    void stop();

    // Source B. Called from the UI thread when the mouse hook reports a left click, and when a
    // wheel event invalidates it (plan.md §1.5: scrolling moves the caret with no click).
    void set_last_click(Point p, uintptr_t window, uint64_t timestamp_ms);
    void invalidate_click();

    // The same rule with the button still attached, for callers that have not already filtered to
    // left clicks. Non-left buttons leave the cached click alone.
    void set_last_click(MouseButton button, Point p, uintptr_t window, uint64_t timestamp_ms);

    // Downgrade rather than discard: keyboard navigation since the click means the position is
    // probably stale but not necessarily wrong.
    void downgrade_click_confidence();

    // Source A. Cheap and synchronous; call at session start and after each commit.
    void refresh_caret(uintptr_t window);

    // Source C. Call immediately *before* an injection with the expected text, then
    // `submit_after_frame()` once the injection has been issued. The estimator captures,
    // polls for the after-frame to its deadline, diffs, scores and publishes.
    void submit_before_frame(const Rect& region);
    void submit_after_frame(std::string_view injected_text);

    // Same, but naming the target window so the worker can compensate for the window moving
    // between the before and after frames: the delta is applied to the capture rectangle so the
    // two frames still cover the same *content*. A window that changed size, rather than moving,
    // has relaid out its text and the pair is discarded — there is no sound way to align it.
    void submit_before_frame(const Rect& region, uintptr_t target_window);

    // True once a capture came back entirely black, which is the documented symptom of an
    // HDR / multi-plane-overlay path that BitBlt cannot see. Source C is dead until the process
    // switches to Windows.Graphics.Capture — which is not implemented (see make_wgc_capture).
    // Note this cannot distinguish "BitBlt saw an MPO plane" from "the target really is black
    // there", e.g. a full-screen black terminal; both yield no usable diff, and the conservative
    // response is the same.
    [[nodiscard]] bool wgc_fallback_required() const;

    // §7.4 — a `seq` gap means a hypothesis was missed, so the belief that "the change on screen
    // was produced by text we injected" is no longer sound. Resets confidence to zero; the
    // anchor itself is kept, because a stale position beats no position.
    void reset_confidence();

    [[nodiscard]] std::optional<Anchor> current() const;

    // What the overlay must degrade to. Consulted only when `current()` is absent or fails
    // is_usable(cfg.min_confidence) — there is deliberately no `Fallback::Inline`, because
    // "inline at the anchor" is not a fallback and OverlayMode already spells it.
    [[nodiscard]] Fallback current_fallback() const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace kd::anchor
