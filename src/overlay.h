#pragma once
//
// The overlay window: layered, click-through, capture-excluded, Direct2D/DirectWrite painted.
//
// Implements: plan.md §1.3 (UI toolkit and the window flags), plan.md §1.5 (what it renders and
// how it degrades), plan.md §1.4 (flagging an utterance whose correction could not be applied).
//
// ─── Every window flag is load-bearing ──────────────────────────────────────────────────────
//
//   WS_EX_LAYERED    — per-pixel alpha, so the pending text sits over the target's own pixels.
//   WS_EX_TOPMOST    — above the target.
//   WS_EX_TOOLWINDOW — no taskbar button, no Alt-Tab entry.
//   WS_EX_NOACTIVATE — **if the overlay takes focus, the target loses it and injection lands in
//                      our own window.** This is the flag whose absence produces the single
//                      worst failure mode in the product.
//   WS_EX_TRANSPARENT— clicks pass through to the target.
//
// Additionally `SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)` (Windows 10 2004+): DWM
// then excludes the overlay from all capture surfaces. **This is a correctness measure, not a
// security one** — it is what stops our own overlay from contaminating the anchor captures in
// anchor.h. Without it, the estimator sees the pending text it just drew and scores it as the
// target's response to an injection.
//
// The process must be PROCESS_PER_MONITOR_DPI_AWARE_V2: the target can be on a different
// monitor at a different scale, and captured rectangles are physical pixels. Note that the
// _V2 form is the DPI_AWARENESS_CONTEXT API / the application manifest, not the older
// SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE) which is v1; the manifest is preferred
// because it applies before any window exists. All coordinates crossing this interface are
// **physical pixels**, matching anchor.h.
//
// ─── Appearance is fixed, and that is a decision, not an omission ───────────────────────────
//
// plan.md §1.5: the overlay's appearance is set in settings — one font, size and colour used
// everywhere — not sampled from the target. Style-matching was abandoned because font family is
// unrecoverable from pixels and colour sampling breaks on gradients and themed backgrounds, and
// because a consistent appearance makes pending text legible *as* pending. Do not add a
// "match the target's font" feature; it was considered and rejected.
//
// ─── Threading ──────────────────────────────────────────────────────────────────────────────
//
// **The overlay is created, painted and destroyed on the UI thread only** — the thread that
// owns the message loop. Direct2D device contexts here are single-threaded (created without
// D2D1_FACTORY_TYPE_MULTI_THREADED, deliberately: the multi-threaded factory takes a lock per
// call, and there is no second thread that needs it).
//
// The anchor estimator publishes from a worker thread; those updates are posted to the UI
// thread before reaching set_anchor(). Calling any method here from another thread is a bug
// that will present as intermittent device-lost errors rather than as an obvious crash.
//
// ─── Device loss ────────────────────────────────────────────────────────────────────────────
//
// D2DERR_RECREATE_TARGET happens on GPU driver updates, remote-session transitions and
// display-mode changes — all routine on this machine. The render target and every device-bound
// resource must be recreated on that error and the frame re-painted, not just logged.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "anchor.h"
#include "util/result.h"

namespace kd {

// ─── Presentation modes ─────────────────────────────────────────────────────────────────────

enum class OverlayMode : uint8_t {
    Hidden,
    Inline,        // at the anchor, in the target's text flow. The baseline UX if Phase 0B's
                   // gate is met (plan.md §3 Phase 0B).
    FloatingPill,  // near the target window, position independent of the caret. The
                   // deterministic degradation when anchor confidence is low — a defined state,
                   // not a bug (plan.md §1.5).
    StatusOnly,    // a small listening indicator, no text preview at all
};

const char* to_string(OverlayMode m) noexcept;

// plan.md §1.5: "Below a confidence threshold the overlay degrades deterministically: floating
// pill near the target, or no preview at all, configurable. **This is a defined state, not a
// bug.**" anchor.h states the same rule from the other end — `current_fallback()` is "consulted
// only when `current()` is absent or fails `is_usable(cfg.min_confidence)`".
//
// ⚠ **NOT CALLED BY THE PRODUCT YET (2026-08-18).** `App::apply_overlay_mode` in main.cpp is
// where it goes and carries the full reason. In short: the rule below is right and
// `AnchorConfig::min_confidence`'s default of 0.50 is wrong — it sits above the range
// `score_diff` actually produces, so enforcing it would degrade every commit on every target.
// Measured in docs/supported-targets.md §5.9.2.1: accurate estimates score 0.3742–0.5000 and
// inaccurate ones 0.1858–0.3415, so any threshold in (0.3415, 0.3742] gets all twelve measured
// cells right. Picking that number is what is being deferred, not the rule — the margin fell from
// 0.080 to 0.033 when two cells were added. This function is kept, and tested, so choosing the
// threshold is the only work left.
//
// This is that decision, and it is a free function rather than a few lines inside `App::on_tick`
// because `src/main.cpp` has no tests. Until this existed, nothing in `src/` called
// `is_usable()`, `min_confidence` or `current_fallback()` at all: the threshold parsed,
// validated and round-tripped through settings.json and then did nothing, and an anchor the
// estimator had itself scored at 0.4953 was handed to the overlay as if it were trustworthy.
// See docs/supported-targets.md §5.9.2's `S07` row for the measurement that exposed it.
//
// `configured` is the user's `settings.overlay_mode`. Only `Inline` depends on the anchor, so a
// user who asked for the pill is never *upgraded* to inline by a confident estimate — degrading
// is the only direction this function moves in.
[[nodiscard]] OverlayMode overlay_mode_for(OverlayMode configured,
                                           const std::optional<anchor::Anchor>& current,
                                           const anchor::AnchorConfig& cfg) noexcept;

// What the session is doing, so the overlay can be unambiguous about the microphone. plan.md
// §1.6: "The mic stays open across a long pause, so listening state must be unambiguous."
enum class OverlayState : uint8_t {
    Idle,
    Listening,
    Speaking,        // VAD says speech is in progress
    Finalizing,      // `stop` sent, awaiting the last `final`
    ModelLoading,
    Warning,         // a stability violation or a degraded anchor; the utterance is flagged
    Unsupported,     // elevated target, password field, secure desktop — an explicit state
    Error,
};

const char* to_string(OverlayState s) noexcept;

// ─── Appearance ─────────────────────────────────────────────────────────────────────────────

struct OverlayStyle {
    std::string font_family = "Segoe UI";
    float font_size_dip = 14.0f;

    // 0xAARRGGBB. Committed-but-not-yet-typed text and unstable text are drawn differently so
    // the user can see which part is still moving.
    uint32_t color_pending = 0xFF3C3C3C;
    uint32_t color_unstable = 0x993C3C3C;
    uint32_t color_background = 0xE6FFFFFF;
    uint32_t color_warning = 0xFFB34700;
    uint32_t color_border = 0x33000000;

    float corner_radius_dip = 6.0f;
    float padding_dip = 6.0f;

    // Cap on rendered width, in DIPs, before the text scrolls within the pill. An overlay that
    // grows without bound covers the document it is annotating.
    float max_width_dip = 720.0f;
};

// ─── The window ─────────────────────────────────────────────────────────────────────────────

class Overlay {
public:
    Overlay();
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    // `instance` is an HINSTANCE as an opaque value, keeping this header Windows-free so that
    // style/layout arithmetic can be unit-tested without creating a window.
    Status create(uintptr_t instance, const OverlayStyle& style);
    void destroy();

    [[nodiscard]] bool is_created() const noexcept;
    [[nodiscard]] uintptr_t hwnd() const noexcept;

    // False when SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) was refused — i.e. on a build
    // older than Windows 10 2004. **The anchor estimator must not trust source C (commit
    // differencing) when this is false**: the overlay then appears in its own after-frames and
    // the estimator scores our glyphs as the target's response to an injection, producing a
    // high-confidence anchor that tracks the overlay instead of the caret. That is worse than
    // having no source C at all, so the degradation is exposed here rather than only logged.
    [[nodiscard]] bool capture_excluded() const noexcept;

    // ── Content ─────────────────────────────────────────────────────────────────────────────

    // `pending` is text the commit engine has accepted but not yet injected; `unstable` is the
    // tail above the eligible prefix, which is what revisions rewrite. Both UTF-8.
    void set_text(std::string_view pending, std::string_view unstable);

    void set_state(OverlayState s);
    void set_mode(OverlayMode m);

    // Position. An absent anchor means "no usable estimate", which drives the fallback the
    // config in anchor.h selected — the overlay never invents a position from a stale anchor.
    void set_anchor(const std::optional<anchor::Anchor>& a);

    // plan.md §1.4 violation handling: "the overlay flags the utterance". This is a visible,
    // persistent mark on the affected utterance — not a toast that disappears — because the
    // user needs to know which sentence in their document may be wrong.
    void flag_violation(std::string_view reason);
    void clear_violation();

    // ── Presentation ────────────────────────────────────────────────────────────────────────

    void show();
    void hide();

    // Repaint. Cheap when nothing changed. Called from the UI thread's idle handling or from a
    // timer; the overlay does not run its own render loop, because a compositor-rate loop for
    // text that changes a few times a second wastes power for no visible benefit.
    void render();

    void set_style(const OverlayStyle& s);
    [[nodiscard]] const OverlayStyle& style() const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

// ─── Pure layout helper ─────────────────────────────────────────────────────────────────────
//
// Given an anchor, a measured text size and the work area of the monitor the anchor is on,
// compute the overlay's rectangle: flipped above the line when there is no room below, clamped
// horizontally to the monitor, and never covering the anchor's own line. Pure, so the
// mixed-DPI and multi-monitor cases plan.md §7 gates on are unit-tested rather than eyeballed.
anchor::Rect layout_overlay(const anchor::Anchor& a, int32_t text_width_px, int32_t text_height_px,
                            const anchor::Rect& monitor_work_area, OverlayMode mode);

}  // namespace kd
