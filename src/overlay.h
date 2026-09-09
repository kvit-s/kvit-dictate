// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
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
#include <vector>

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

    // docs/editing.md §7.4 — and this one is load-bearing rather than decorative. Three separate
    // rules in that document rest on the user being able to see whether the microphone is open:
    // §3.2's privacy promise (a closed device must look closed), §3.3's resume (a gesture nobody
    // makes if they cannot tell it is needed), and §4.5's no-op hotkey (a key that does nothing
    // while dictating is only tolerable if the user can see *why*).
    //
    // **It must reach the tray as well as the pill.** §16 names the failure precisely: a new
    // state the pill renders but `set_tray_state` maps onto Listening because its switch has no
    // case for it is `Effect::SurfaceWarning`'s shape, where one fixed string served seven
    // raisers and a refusal during a model load was reported as a stability violation for
    // months. Appended to the enum rather than inserted, so a value already on the wire keeps
    // meaning what it meant.
    Editing,
};

const char* to_string(OverlayState s) noexcept;

// The colour that stands for `s`, as 0xAARRGGBB, in whichever scheme is painting.
//
// One table, two surfaces. The pill draws it as the state dot and the tray icon tints its
// microphone with it, and they have to agree: docs/editing.md §16 records what it costs when the
// pill and the tray disagree about a state, and "the tray says green while the pill says violet"
// is that failure with the colours instead of the words. The pill overrides exactly one entry —
// Warning, which takes its own border colour so the border and the dot cannot differ.
[[nodiscard]] uint32_t state_dot_color(OverlayState s, bool dark) noexcept;

// ─── Appearance ─────────────────────────────────────────────────────────────────────────────

// Where the pill's five colours come from.
//
// plan.md §1.5 says the overlay's appearance is fixed in settings and never sampled from the
// target — and that is still true, because the *system* colour scheme is not the target. It is a
// setting like any other; the only difference is that Windows owns it and can change it while
// the program runs. The pill sits on top of the user's document, so a light pill on a dark
// desktop is not a cosmetic complaint: it is the brightest thing on the screen.
//
// `Custom` is what the five colour fields below are for, and it exists so that a user who has
// set them keeps them. Everything else ignores those fields and paints from `overlay_palette`.
enum class ThemeMode {
    System,  // follow Windows, and follow it again when Windows changes mid-session
    Light,
    Dark,
    Custom,  // the colour fields below, exactly as written
};

const char* to_string(ThemeMode m) noexcept;

// The five colours of one scheme, in the order OverlayStyle declares them.
struct OverlayPalette {
    uint32_t pending;
    uint32_t unstable;
    uint32_t background;
    uint32_t warning;
    uint32_t border;
};

// `overlay_palette(false)` is exactly OverlayStyle's own defaults, so `Custom` on a file that
// never touched a colour is indistinguishable from `Light` — which is what makes the migration
// in settings.cpp honest rather than a visible change.
[[nodiscard]] const OverlayPalette& overlay_palette(bool dark) noexcept;

// Does `mode` paint dark, given what Windows currently says? `Custom` is neither: it answers
// false, because the user's own colours are not a scheme and nothing may second-guess them.
[[nodiscard]] bool theme_paints_dark(ThemeMode mode, bool system_dark) noexcept;

struct OverlayStyle {
    std::string font_family = "Segoe UI";
    float font_size_dip = 14.0f;

    // Which of the three fixed schemes paints the pill, or Custom for the colours below.
    ThemeMode theme = ThemeMode::System;

    // 0xAARRGGBB. Committed-but-not-yet-typed text and unstable text are drawn differently so
    // the user can see which part is still moving.
    //
    // **Read only under ThemeMode::Custom.** Under the other three these are still parsed,
    // serialised and shown in the dialog — so switching to Custom gets the user's colours back
    // rather than a reset — but the pill paints from `overlay_palette` instead.
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

// `base` with the palette for its ThemeMode folded into its five colour fields. Under `Custom`
// the colours are returned untouched. Font, padding, radius and width are never touched by this:
// they are the user's layout, not a scheme.
//
// `system_dark` is the answer to theme::system_prefers_dark(), passed in rather than read here so
// that this stays pure and testable — overlay.h is the half of this file that has no Windows in
// it, and a registry read in the middle of it would end that.
[[nodiscard]] OverlayStyle resolve_overlay_style(const OverlayStyle& base, bool system_dark);

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

    // How loud the microphone is right now, 0 (silence) to 1 (clipping), already smoothed by the
    // caller. Drawn as a level meter beside the state dot.
    //
    // **This is the one indicator that comes from the microphone rather than from the network**,
    // and that is the whole reason it exists. plan.md §1.6 requires listening state to be
    // unambiguous, and until this the pill's answer was a static six-pixel dot: a stalled
    // transcriber, a muted device and a dead sidecar all looked identical to a speaker who could
    // see no words appearing. A meter that moves with the voice separates "you are not being
    // heard" from "you are being heard and the words have not come back yet" — which are the two
    // halves of what a user actually means by "is it listening?".
    //
    // Quantised by the implementation before it marks anything dirty, so feeding it at capture
    // rate does not repaint the pill on microphone noise.
    void set_input_level(float level01);

    // How far behind the preview is **while the speaker is still talking**, in milliseconds; 0
    // whenever there is nothing worth mentioning. Past a threshold the pill says so in its note
    // line.
    //
    // Deliberately separate from `set_state`: the session is not in a *different* state — the
    // microphone is open and the words are coming — it is a latency the user is entitled to see
    // instead of inferring from a preview that stopped moving. A batch model re-decodes the whole
    // open utterance on every hop, so this grows with the sentence and is at its worst exactly
    // when the speaker is furthest from the last thing they saw.
    //
    // **"While still talking" is the caller's job and is load-bearing.** Measured as plain time
    // since the last text, this fires on every ordinary pause — the endpointer takes
    // `endpoint_silence_ms` to notice you stopped and the flush then re-decodes the utterance, so
    // the words always land seconds after your last syllable, entirely correctly. A pill that
    // announced a stalled transcriber there would be wrong every single time the user paused.
    // See `App::publish_listening_state`.
    void set_transcriber_lag(uint64_t lag_ms);

    // Position. An absent anchor means "no usable estimate", which drives the fallback the
    // config in anchor.h selected — the overlay never invents a position from a stale anchor.
    void set_anchor(const std::optional<anchor::Anchor>& a);

    // plan.md §1.4 violation handling: "the overlay flags the utterance". This is a visible,
    // persistent mark on the affected utterance — not a toast that disappears — because the
    // user needs to know which sentence in their document may be wrong.
    void flag_violation(std::string_view reason);
    void clear_violation();

    // ── The compose buffer (docs/editing.md §7) ─────────────────────────────────────────────

    // Where the caret is and what is selected, as **UTF-8 byte offsets into `pending`** — the
    // same units everything else in this program uses (util/utf8.h §1). The unstable tail is not
    // addressable and offsets into it are clamped away, which is docs/editing.md §3's freeze
    // rule arriving here for free rather than being re-implemented.
    //
    // `active` false draws neither caret nor selection, which is what a session that has not
    // been edited looks like — identical to today.
    void set_edit_view(bool active, size_t caret, size_t selection_begin, size_t selection_end);

    // §4.4's one-line hint: `Ctrl+S — press Enter to finish first`. Deliberately not
    // flag_violation(): a violation is a persistent mark on text that may be wrong (plan.md
    // §1.4), and this is neither persistent nor about the text. The caller expires it on the
    // tick it already runs.
    void set_hint(std::string_view hint);
    void clear_hint();

    // The key legend across the bottom of the pill (docs/editing.md §7.5). The overlay composes
    // the line itself from the state it is already painting; the two things it cannot know are
    // pushed here, once, rather than re-pushed per state change:
    //
    //   * `hotkey` — how the binding chord is spelled for a human ("Win+Y"), from
    //     `HotkeySpec::to_display_string()`. Empty drops the item that names it rather than
    //     printing a blank chord.
    //   * `composing` — whether the pill is a compose buffer at all (`editing.enabled`, which
    //     `Settings::normalise` has already forced false for any cadence but end-of-session).
    //     Enter only finishes, and typing only edits, when it is true; with it false the legend
    //     says what plan.md §1.8 actually does instead.
    //   * `push_to_talk` — whether the binding is held rather than toggled
    //     (`HotkeySpec::push_to_talk`). It changes the verb, not the keys: the gesture that ends
    //     the session is releasing the chord, so the legend has to say "let go" where it would
    //     otherwise say "press".
    void set_key_legend(std::string_view hotkey, bool composing, bool push_to_talk = false);

    // §7.3. Scrollback is a prerequisite, not a nicety: the pill shows the newest `kMaxLines`
    // with front-elision, so a session-long buffer sits behind that window unseen, and
    // editable-but-unreachable text is not a feature but a trap.
    //
    // Negative scrolls back (older text), positive forward. The view is pinned to the bottom
    // until the first call, and any caret movement scrolls the caret into view regardless — which
    // is what makes the arrow keys able to reach the top of a long buffer at all.
    void scroll_by_lines(int32_t delta);
    void scroll_to_bottom();

    // ── The mouse (docs/editing.md §5) ──────────────────────────────────────────────────────

    // Drop or restore WS_EX_TRANSPARENT at runtime, **only for the duration of a composing
    // session** (§5.1). Clicks then land on this window instead of passing through to the target,
    // which is the entire point: a click meant for the pill must not move the target's caret.
    //
    // What makes it safe rather than reckless is that WS_EX_NOACTIVATE stays set. A click on a
    // NOACTIVATE window does not activate it, so the target stays foreground and
    // `validate_target` keeps passing. overlay.cpp already returns MA_NOACTIVATE from
    // WM_MOUSEACTIVATE with a comment saying it is belt-and-braces "if a future change ever drops
    // that flag"; this is that change.
    //
    // **The restore belongs on the one path every session ends through** (§5.3). A pill that
    // stays non-transparent after a session eats clicks meant for the document, forever, and the
    // user's only recovery is to restart the process — the same shape as the
    // `note_session_ended()` bug in progress.md §8.
    void set_click_through(bool click_through);
    [[nodiscard]] bool click_through() const noexcept;

    // Where the pill is on screen, in physical pixels, as of the last paint. Empty when it has
    // never been painted or is hidden. Published to the hook so the mouse filter can tell a click
    // inside the pill from a click that means "the user has gone elsewhere" (§5.2).
    [[nodiscard]] anchor::Rect pill_rect() const noexcept;

    // Hit-test a point given **relative to the pill's top-left, in physical pixels** — the units
    // the hook publishes, because it has the rectangle and the UI thread has the layout.
    //
    // Returns false when the point is not over the text at all. On success `offset` is a UTF-8
    // byte offset into the `pending` half, clamped into it: the unstable tail is not addressable
    // (docs/editing.md §3), so a click on the model's still-moving words puts the caret at the
    // end of what is settled rather than inside what is not.
    [[nodiscard]] bool hit_test_text(int32_t x, int32_t y, size_t& offset) const;

    // True when the point is over the microphone button — the one control that says what it does
    // without anyone having to remember a key (§13 stage 3), drawn only while editing.
    [[nodiscard]] bool hit_test_mic_button(int32_t x, int32_t y) const;

    // ── Post-submission actions ─────────────────────────────────────────────────────────────
    //
    // The buttons the pill offers after a session has delivered its text, when the program has
    // said the injection succeeded and only the user can see whether it did. injector.h is blunt
    // about why they have to exist: the clipboard method returns Ok once the paste chord is
    // accepted, and "an application that binds Ctrl+V to something else, or that ignores it
    // entirely, produces this same result with no text inserted". There is no signal that would
    // separate those, so the recovery is offered rather than triggered.
    //
    // At most two, because there are only two useful answers — put it somewhere I can paste it,
    // or type it again — and a third would be a menu. The caller decides which apply from the
    // method it used and the clipboard state it got back; this only draws what it is given.
    //
    // **Labels only, with nothing written above them.** There was a line of explanatory text
    // here and it earned none of the space it took: the buttons say what they do, and the pill
    // is sitting on top of the document the user is trying to read to decide whether to press
    // one. A sentence that has to be read first is the wrong shape for that.
    //
    // Showing actions makes the pill clickable (WS_EX_TRANSPARENT is dropped) and keeps it up
    // after the session has ended. `clear_actions` puts both back.
    void set_actions(const std::vector<std::string>& labels);
    void clear_actions();
    [[nodiscard]] bool has_actions() const noexcept;

    // Which button the point is over, in the same physical-pixel client coordinates as
    // hit_test_text. False when it is over none of them.
    [[nodiscard]] bool hit_test_action(int32_t x, int32_t y, size_t& index) const;

    // Where to post a click. The overlay posts `message` to `hwnd` with the button index in
    // wParam. A window handle rather than a callback because the click arrives on the UI thread
    // inside a window procedure, and the handlers it runs put up dialogs and inject text — work
    // that must not happen inside somebody else's WM_LBUTTONDOWN.
    void set_action_target(uintptr_t hwnd, uint32_t message) noexcept;

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

// ─── The key legend, pure ───────────────────────────────────────────────────────────────────
//
// docs/editing.md §7.5. The line of key names along the bottom of the pill, as UTF-8, for the
// state being painted: empty for every state where the session is not the user's to steer.
//
// Pure and declared here because it is the one thing a legend must never get wrong — naming a key
// that does nothing in the state it is shown in. `composing` false is plan.md §1.8's session,
// where Enter is not a commit and any key ends the session; true is docs/editing.md's, where it
// is and they do not.
[[nodiscard]] std::string key_legend(OverlayState state, bool composing, std::string_view hotkey,
                                    bool push_to_talk = false);

// ─── Pure layout helper ─────────────────────────────────────────────────────────────────────
//
// Given an anchor, a measured text size and the work area of the monitor the anchor is on,
// compute the overlay's rectangle: flipped above the line when there is no room below, clamped
// horizontally to the monitor, and never covering the anchor's own line. Pure, so the
// mixed-DPI and multi-monitor cases plan.md §7 gates on are unit-tested rather than eyeballed.
anchor::Rect layout_overlay(const anchor::Anchor& a, int32_t text_width_px, int32_t text_height_px,
                            const anchor::Rect& monitor_work_area, OverlayMode mode);

// ─── The scroll arithmetic, also pure ───────────────────────────────────────────────────────
//
// docs/editing.md §7.3's three rules in one function, in the order they override each other:
//
//   * the view shows `visible_lines` of `total_lines`, starting at `desired`;
//   * **any caret movement scrolls the caret into view** — so `caret_line`, when it is known,
//     wins over `desired`;
//   * the result is always a valid start line, so a buffer that shrank under a stale offset
//     cannot leave the pill showing blank space below its own text.
//
// Separated out and tested rather than written inline in paint_once() for the reason
// `layout_overlay` was: the off-by-one at each end is the kind of thing that is obvious in a
// table of cases and invisible in a paint function.
[[nodiscard]] uint32_t clamp_scroll_line(uint32_t desired, uint32_t caret_line, bool caret_known,
                                         uint32_t total_lines, uint32_t visible_lines) noexcept;

// ─── When a moved caret unpins the view ─────────────────────────────────────────────────────
//
// §7.3's second rule — "any caret movement scrolls the caret into view" — is about a caret the
// *user* moved. Outside the compose buffer the caret still moves, once per utterance, because
// EditBuffer::append leaves it at the end of the text; and `edit_active` false is exactly the
// state in which paint_once() does not know where the caret's line is, so an unpin there leaves
// the view frozen wherever it last was while the transcript grows past it — which is the bottom
// of the window for the first eight lines and then, silently, the top of a buffer whose newest
// words are off the bottom. That is the opposite of what §7.3's third rule promises, and it is
// what a long dictation actually looked like.
//
// So the rule needs both halves: the caret moved, *and* the pill is drawing that caret.
[[nodiscard]] constexpr bool caret_move_unpins_scroll(bool edit_active, bool caret_moved) noexcept {
    return edit_active && caret_moved;
}

}  // namespace kd
