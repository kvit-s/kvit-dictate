// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// The overlay window — layered, click-through, capture-excluded, Direct2D/DirectWrite painted.
//
// Specification: overlay.h. Implements plan.md §1.3 (window flags, DPI), §1.5 (what it renders
// and how it degrades), §1.4 (flagging an utterance whose correction could not be applied).
//
// ═══ Manual verification procedure ══════════════════════════════════════════════════════════
//
// None of this file is covered by an automated test: it is pixels, window management and DWM
// behaviour, and a test that asserted on any of it would assert on the mock, not on Windows.
// layout_overlay() at the bottom is the part that *is* pure and testable; everything above it
// is verified by hand, with this procedure. Run it whenever this file, the manifest's DPI
// awareness, or the Windows build changes.
//
// Monitor configuration required: **two monitors at different scale factors** — e.g. the
// primary at 150 % and the secondary at 100 %, arranged side by side. A single-monitor pass
// proves nothing about the cases plan.md §7 gates on ("Mixed-DPI and multi-monitor overlay
// tests pass, with deterministic low-confidence fallback").
//
//  1. **Activation.** Put the caret in Notepad, trigger the overlay, then type. Look at: the
//     Notepad title bar (must stay active) and where the characters land.
//     Failure looks like: the Notepad title bar greys out the moment the overlay appears, or
//     the keystrokes go nowhere. That is WS_EX_NOACTIVATE or SW_SHOWNOACTIVATE lost — the worst
//     failure in the product, because injection then lands in our own window.
//
//  2. **Click-through.** With the overlay visible over a browser, click a link *underneath* it.
//     Failure looks like: the click does nothing, or the cursor changes over the overlay. That
//     is WS_EX_TRANSPARENT lost. The converse is step 9: *during* a composing session the cursor
//     is supposed to change, and only then.
//
//  3. **Capture exclusion.** With the overlay visible, run `tools/probe` in its anchor mode (or
//     press PrtSc and paste into Paint). Look at the captured image where the overlay is.
//     Failure looks like: the overlay's pending text appears in the capture. The anchor
//     estimator then scores its own pixels as the target's response to an injection, and the
//     anchor chases the overlay across the screen. Check the log for the
//     "SetWindowDisplayAffinity failed" warning first — on a build older than Windows 10 2004
//     this degradation is expected and is reported, not silent.
//
//  4. **Mixed DPI.** Drag the target window from the 150 % monitor to the 100 % one with the
//     overlay showing. Look at: the overlay's physical text size (must stay the same physical
//     size on both, i.e. more pixels on the 150 % monitor) and its position relative to the
//     caret.
//     Failure looks like: text half-size or double-size on the second monitor (the render
//     target's DPI is not being re-read per monitor), or the overlay landing on the wrong
//     monitor entirely (a coordinate treated as logical when it is physical).
//
//  5. **Flip-above.** Click into the last visible line of a maximised editor, at the very
//     bottom of the screen. The overlay must appear *above* the caret line, never off-screen
//     and never covering the line being dictated into.
//     Failure looks like: a clipped overlay at the screen edge, or the overlay sitting on top
//     of the text it is annotating.
//
//  6. **Fallback determinism.** Force a low-confidence anchor (dictate into a WSLg window, or
//     set anchor.min_confidence to 1.0 in settings). The overlay must move to exactly one
//     defined place — bottom-centre of the target's monitor — and stay there.
//     Failure looks like: flapping between the inline position and the pill on alternate
//     commits. plan.md §1.5 calls that out specifically; the hysteresis that prevents it lives
//     in anchor.cpp, but this is where it is visible.
//
//  7. **Violation flag.** Trigger a committed-prefix violation (the probe has a switch for it).
//     The overlay must show a persistent warning-coloured border and the reason, and keep
//     showing it until the next utterance. Failure looks like: a mark that fades by itself —
//     plan.md §1.4 wants a persistent mark, because the user needs to know which sentence in
//     their document may be wrong.
//
//  8. **Device loss.** With the overlay visible, start and end a Remote Desktop session to the
//     machine, or update the display driver. The overlay must repaint normally afterwards.
//     Failure looks like: the overlay freezes on its last frame or disappears — that is
//     D2DERR_RECREATE_TARGET being logged instead of handled.
//
//  9. **Pointer shape.** With `editing.mouse` on, start a session and move the pointer over the
//     pill. Over the text it must be an I-beam and over the microphone button a hand; move off
//     the pill and it must go back to whatever the window underneath asks for, immediately.
//     Failure looks like: a busy or app-starting cursor over the pill (the class cursor is back
//     to nullptr, or WM_SETCURSOR is not answered), or an I-beam that persists after the session
//     ends — which is not a cursor bug but §5.3's: WS_EX_TRANSPARENT was never restored, and the
//     pill is eating clicks meant for the document.
//
// ═══════════════════════════════════════════════════════════════════════════════════════════

#define KD_LOG_MODULE "overlay"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <utility>

#include "overlay.h"
#include "theme.h"
#include "util/log.h"
#include "util/utf8.h"

// Windows 10 2004 (build 19041) added this. The SDK on this machine (10.0.26100) defines it,
// but the guard keeps the file compiling against an older SDK — and the *runtime* degradation
// on an older OS is handled separately, where the call is made.
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace kd {
namespace {

// ─── Small RAII helpers ─────────────────────────────────────────────────────────────────────
//
// Written here rather than pulled from WRL/ATL: this project has no third-party dependencies
// and the SDK's ComPtr would be the only place we reached into the C++ support libraries. These
// are twenty lines and every acquisition in this file is released on every path, including the
// early returns that D2D's HRESULT-per-call style produces in quantity.

template <typename T>
class ComRef {
public:
    ComRef() = default;
    ~ComRef() { reset(); }
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;
    ComRef(ComRef&& other) noexcept : p_(other.p_) { other.p_ = nullptr; }
    ComRef& operator=(ComRef&& other) noexcept {
        if (this != &other) {
            reset();
            p_ = other.p_;
            other.p_ = nullptr;
        }
        return *this;
    }

    // Out-parameter for a CreateXxx() call. Releases whatever was held first, so a re-create
    // cannot leak the previous interface.
    T** put() noexcept {
        reset();
        return &p_;
    }

    // A second owning reference to the same interface. COM's own answer — AddRef — rather than
    // a std::shared_ptr, because the interface is already reference-counted and wrapping it in a
    // second counter is how an object ends up released while somebody still holds it. Used by
    // paint_once() to keep the layout it just drew, so a mouse hit test asks the same layout the
    // user is looking at rather than building a second one that could disagree with it.
    [[nodiscard]] ComRef clone() const noexcept {
        ComRef c;
        c.p_ = p_;
        if (c.p_ != nullptr) c.p_->AddRef();
        return c;
    }

    [[nodiscard]] T* get() const noexcept { return p_; }
    T* operator->() const noexcept { return p_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }
    void reset() noexcept {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }

private:
    T* p_ = nullptr;
};

// A screen DC borrowed for the lifetime of a scope. GetDC(nullptr) must be paired with
// ReleaseDC, and the pairing is easy to lose across the HRESULT early returns below.
class ScreenDC {
public:
    ScreenDC() : dc_(GetDC(nullptr)) {}
    ~ScreenDC() {
        if (dc_) {
            ReleaseDC(nullptr, dc_);
        }
    }
    ScreenDC(const ScreenDC&) = delete;
    ScreenDC& operator=(const ScreenDC&) = delete;
    [[nodiscard]] HDC get() const noexcept { return dc_; }

private:
    HDC dc_ = nullptr;
};

// The 32-bit top-down premultiplied-BGRA DIB section that UpdateLayeredWindow consumes.
struct DibSurface {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    void* bits = nullptr;
    int width = 0;
    int height = 0;

    ~DibSurface() { reset(); }

    void reset() noexcept {
        if (dc && previous) {
            SelectObject(dc, previous);
        }
        previous = nullptr;
        if (bitmap) {
            DeleteObject(bitmap);
            bitmap = nullptr;
        }
        if (dc) {
            DeleteDC(dc);
            dc = nullptr;
        }
        bits = nullptr;
        width = 0;
        height = 0;
    }

    // Recreates only on a size change: the DIB is the single largest allocation this module
    // makes and the overlay resizes only when the text does.
    bool ensure(int w, int h) {
        if (w <= 0 || h <= 0) {
            return false;
        }
        if (dc && bitmap && w == width && h == height) {
            return true;
        }
        reset();

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        // Negative height = top-down. D2D and UpdateLayeredWindow both want top-down; a
        // bottom-up DIB renders the overlay upside down, which is a memorable five minutes.
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        ScreenDC screen;
        bitmap = CreateDIBSection(screen.get(), &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap) {
            reset();
            return false;
        }
        dc = CreateCompatibleDC(nullptr);
        if (!dc) {
            reset();
            return false;
        }
        previous = SelectObject(dc, bitmap);
        width = w;
        height = h;
        return true;
    }
};

// ─── Fixed appearance constants ─────────────────────────────────────────────────────────────
//
// plan.md §1.5: the overlay's appearance is fixed in settings and is never sampled from the
// target. These are the few colours OverlayStyle does not carry — the state dot. They are
// deliberately constants and not another six settings fields: the dot exists to make the
// microphone state unambiguous (plan.md §1.6), and a user-recolourable "is the mic on" light is
// a way to make it ambiguous again.

// Two sets, indexed by whether the pill is painting dark. The *hues* are identical in both and
// only the lightness moves: a dot that changed colour with the desktop scheme would break the
// one thing it is for. These are the mid-500 tones of each hue on light, and the 300s on dark,
// which is the smallest change that keeps them legible on a near-black pill.
struct DotColors {
    uint32_t idle;
    uint32_t listening;
    uint32_t speaking;
    uint32_t finalizing;
    uint32_t loading;
    uint32_t unsupported;
    uint32_t error;
    // Editing: a violet, deliberately not the green of Listening. The user checks this against
    // the microphone light on their machine (docs/editing.md §7.4), so the two must never
    // disagree — and green over a closed device is exactly that disagreement.
    uint32_t editing;
};

constexpr DotColors kDots[2] = {
    // light
    {0xFF9E9E9E, 0xFF2E7D32, 0xFF43A047, 0xFF1565C0, 0xFFB8860B, 0xFF757575, 0xFFC62828,
     0xFF6A1B9A},
    // dark
    {0xFFBDBDBD, 0xFF66BB6A, 0xFF81C784, 0xFF64B5F6, 0xFFFFD54F, 0xFF9E9E9E, 0xFFEF5350,
     0xFFBA68C8},
};

// Upper bound on the number of wrapped lines the preview may occupy. An overlay that grows with
// the utterance covers the document it is annotating; OverlayStyle::max_width_dip caps the
// width, this caps the height, and text beyond both is elided from the *front* because the
// newest words are the ones the user is checking.
//
// Front-elision is what makes this a scrolled view rather than a truncated one: the box always
// shows the newest `kMaxLines` worth of text, which is the bottom of a transcript that only ever
// grows. Scrollback (docs/editing.md §7.3) reaches the rest, and the indicator down the right
// padding strip is what says the rest is there — for a while this file said "there is no
// scrollbar and no way to look further back" and only the second half stopped being true.
//
// Three lines suited a preview that emptied into the document every sentence. It does not suit a
// buffer that holds a whole session, so this is eight — about a paragraph. Note the interaction
// with overlay mode: eight lines *at the caret* covers the document being dictated into, which is
// what the paragraph above warns about, so the floating pill is the mode that pairs with a large
// value here.
constexpr uint32_t kMaxLines = 8;

// Hard cap on the buffer laid out for display, in UTF-16 code units.
//
// This used to be a *starting* budget for a loop that shrank it until the text fitted
// `kMaxLines`. Scrollback (docs/editing.md §7.3) replaces that: the whole buffer is laid out and
// the view shows a window onto it, so the loop's job — pick a length that happens to be eight
// lines — no longer exists. What remains is a ceiling, because a twenty-minute session laid out
// on every repaint is a cost nobody asked for and docs/editing.md §15 question 5 records that
// the right size for it is unmeasured.
//
// 32768 units is roughly 5000 words, well past any session anyone has run, and the one time it
// bites it is logged rather than silently truncating.
constexpr size_t kMaxDisplayUnits = 32768;

// ─── The level meter (overlay.h, set_input_level) ──────────────────────────────────────────
//
// The state dot has always sat in a gutter `dot_r * 2` wide, and below the dot that gutter is
// empty for the whole height of the pill — which under scrollback is up to `kMaxLines`. The
// meter is drawn there: a track down the gutter and a fill rising from the bottom of it with the
// microphone's level. It costs no width, it is next to the thing whose colour already says which
// state the session is in, and it moves.
//
// One line of pill is not enough gutter to read a bar in, so below this the meter is not drawn
// and the dot's own radius carries the level instead (see kDotLevelGain). Both, above it.
constexpr float kMeterMinGutterDip = 26.0f;

// The dot breathes with the level even when the meter is drawn, because the dot is the part that
// is visible at every pill height and in `StatusOnly`. Silence is not zero — a dot that vanished
// would read as "off", which is the opposite of what it means.
constexpr float kDotLevelFloor = 0.62f;
constexpr float kDotLevelGain = 0.55f;

// How faint the meter's unfilled track is against the pill, light scheme then dark.
constexpr uint32_t kMeterTrack[2] = {0x18000000, 0x20FFFFFF};

// ─── When a stalled transcriber gets said out loud (overlay.h, set_transcriber_lag) ─────────
//
// Below this the pill says nothing: a batch model behind a hop takes a second or two to answer
// on a good day, and a caption that appeared every time it did would be noise. Past it the
// speaker has been talking into a preview that has not moved for long enough to wonder whether
// anything is on, and the honest answer — it is listening, the words have not come back — is
// better than leaving them to guess from a dot.
constexpr uint64_t kLagNoteMs = 4000;

// A caret is a solid 1-DIP bar and does not blink (docs/editing.md §7.1). overlay.h is explicit
// that this window runs no render loop, and a blinking caret needs a ~530 ms timer for the whole
// session to buy nothing but familiarity.
constexpr float kCaretWidthDip = 1.0f;

// The selection's fill, as 0xAARRGGBB, light scheme then dark. Painted *under* the text, so the
// glyphs keep their own colours and the committed/unstable split stays readable through it —
// which is why the dark one is lighter rather than merely the same green: a wash that darkens a
// near-black pill would hide the selection instead of showing it.
constexpr uint32_t kSelectionFill[2] = {0x552E7D32, 0x6643A047};

// docs/editing.md §13 stage 3: "the microphone button — the one control that says what it does
// without anyone having to remember a key, and the answer for whoever finds §4.5's no-op hotkey
// more confusing than the toggle it replaced."
constexpr const char* kMicButtonLabel = "\xF0\x9F\x8E\xA4 Resume dictating";  // U+1F3A4
// Light scheme then dark. A wash of the *opposite* colour to the pill in each, because the
// button has to read as a raised patch on the background it is actually sitting on.
constexpr uint32_t kMicButtonFill[2] = {0x22000000, 0x22FFFFFF};
constexpr float kMicButtonPadDip = 6.0f;

D2D1_COLOR_F argb(uint32_t c) noexcept {
    return D2D1::ColorF(c & 0x00FFFFFFu, static_cast<float>((c >> 24) & 0xFFu) / 255.0f);
}

// `style` is the **resolved** style — the one the palette has already been folded into — and
// `dark` says which of the two dot sets goes with it. Warning is the exception that reads its
// colour from the style, because the warning border and the warning dot are one signal and must
// not be able to differ.
uint32_t dot_color(OverlayState s, const OverlayStyle& style, bool dark) noexcept {
    // Warning is the one state that does not read the shared table: the warning border and the
    // warning dot are one signal on the pill and must not be able to differ, so the dot takes
    // the border's colour. Everywhere else this is state_dot_color verbatim, which is what keeps
    // the pill and the tray icon in agreement.
    if (s == OverlayState::Warning) return style.color_warning;
    return state_dot_color(s, dark);
}

// Deliberately *not* to_string(OverlayState). to_string is the programmatic spelling used in
// logs; this is prose shown to a user, and the two must be free to diverge — "Unsupported"
// tells a developer which enumerator it is and tells a user nothing about why their dictation
// is not appearing.
const char* state_label(OverlayState s) noexcept {
    switch (s) {
        case OverlayState::Idle: return "";
        // The microphone glyph is what the user checks against the hardware light on their
        // machine (docs/editing.md §7.4), so it is on every state where the device is open and
        // struck through on the one where it is not. The words carry the same information, for
        // the case where the font has no colour emoji and draws a box.
        case OverlayState::Listening: return "\xF0\x9F\x8E\xA4 Listening";  // U+1F3A4
        case OverlayState::Speaking: return "\xF0\x9F\x8E\xA4 Listening \xE2\x80\xA2";  // + U+2022
        case OverlayState::Finalizing: return "Finishing\xE2\x80\xA6";
        case OverlayState::ModelLoading: return "Loading model\xE2\x80\xA6";
        case OverlayState::Warning: return "Correction could not be applied";
        case OverlayState::Unsupported: return "Dictation is not supported in this target";
        case OverlayState::Editing:
            // U+1F3A4 MICROPHONE + U+0338 COMBINING LONG SOLIDUS OVERLAY. The words are what
            // carry the meaning if the font declines to compose the two.
            return "\xF0\x9F\x8E\xA4\xCC\xB8 Editing \xE2\x80\x94 microphone off";
        case OverlayState::Error: return "Dictation stopped";
    }
    return "";
}

// Display-only elision. `cut_out` receives how many leading UTF-16 units were dropped, so the
// caller can shift the pending/unstable colour split by the same amount.
//
// **No offset derived from this ever reaches the injector.** This is the one place in the
// program that shortens text, and it is safe only because the result is drawn and discarded;
// the committed-prefix invariant (plan.md §1.4) is about what is injected, which comes from
// commit_engine.cpp and never from here.
std::wstring elide_front(const std::wstring& s, size_t keep, size_t& cut_out) {
    cut_out = 0;
    if (s.size() <= keep) {
        return s;
    }
    size_t cut = s.size() - keep;
    // Never cut between a surrogate pair. A lone low surrogate is not text; DirectWrite would
    // draw a replacement glyph and utf8::from_utf16 would reject it if it ever travelled back.
    if (cut < s.size() && s[cut] >= 0xDC00 && s[cut] <= 0xDFFF) {
        ++cut;
    }
    cut_out = cut;
    std::wstring out;
    out.reserve(keep + 1);
    out.push_back(L'\x2026');  // U+2026 HORIZONTAL ELLIPSIS — the mark that text was dropped
    out.append(s, cut, std::wstring::npos);
    return out;
}

// How many UTF-16 code units the first `byte_off` bytes of `text` occupy.
//
// This is the one conversion the caret needs: EditBuffer speaks UTF-8 byte offsets (util/utf8.h
// §1: "every offset in this program is a UTF-8 byte offset"), DirectWrite's hit-testing speaks
// UTF-16 code-unit positions, and the boundary between them is here — the same boundary utf8.h
// §2 says is the only place UTF-16 is crossed.
size_t utf16_prefix_units(std::string_view text, size_t byte_off) {
    if (byte_off > text.size()) byte_off = text.size();
    std::wstring w;
    if (!utf8::to_utf16(text.substr(0, byte_off), w, nullptr)) return 0;
    return w.size();
}

// The inverse of utf16_prefix_units: the UTF-8 byte offset whose prefix occupies `units` UTF-16
// code units. Used by the mouse hit test, which gets a UTF-16 position out of DirectWrite and has
// to hand a byte offset to EditBuffer.
//
// Walks code points rather than doing arithmetic on lengths, because the relationship is not a
// ratio: ASCII is 1:1, most of the BMP is 3 bytes to 1 unit, and everything above it is 4 bytes
// to 2 units. The result always lands on a code-point boundary; EditBuffer floors it to a
// grapheme boundary, which is the clamp that keeps a click from splitting a combining sequence.
size_t utf8_offset_for_utf16_units(std::string_view text, size_t units) {
    size_t off = 0;
    size_t seen = 0;
    while (off < text.size() && seen < units) {
        char32_t cp = 0;
        size_t len = 0;
        if (!utf8::decode(text, off, cp, len)) break;
        seen += cp >= 0x10000 ? 2u : 1u;
        off += len;
    }
    return off;
}

// UTF-8 -> UTF-16 at the Windows API boundary, per util/utf8.h. Malformed input is refused
// rather than repaired: utf8.h forbids U+FFFD substitution anywhere, and this text is on its way
// to (a preview of) the user's document.
std::wstring wide_or_log(std::string_view s, const char* what) {
    std::wstring out;
    size_t bad = 0;
    if (!utf8::to_utf16(s, out, &bad)) {
        KD_LOG_WARN("refusing to render malformed UTF-8 %s at offset %zu (%zu bytes)", what, bad,
                    s.size());
        return std::wstring{};
    }
    return out;
}

const wchar_t* kWindowClass = L"kd_overlay_window";

}  // namespace

// ─── to_string ──────────────────────────────────────────────────────────────────────────────
//
// These spell the enumerator, not the prose. Log lines and settings files are the consumers.

const char* to_string(OverlayMode m) noexcept {
    switch (m) {
        case OverlayMode::Hidden: return "Hidden";
        case OverlayMode::Inline: return "Inline";
        case OverlayMode::FloatingPill: return "FloatingPill";
        case OverlayMode::StatusOnly: return "StatusOnly";
    }
    return "Hidden";
}

OverlayMode overlay_mode_for(OverlayMode configured, const std::optional<anchor::Anchor>& current,
                             const anchor::AnchorConfig& cfg) noexcept {
    // Only the inline mode is anchored, so it is the only one a bad anchor can invalidate.
    if (configured != OverlayMode::Inline) return configured;

    // is_usable() requires BOTH a real source and the threshold, so a reset_confidence() anchor
    // (§7.4 — a `seq` gap, meaning we can no longer believe the pixels came from our text) drops
    // out here too, without this function needing to know why the confidence went to zero.
    if (current.has_value() && current->is_usable(cfg.min_confidence)) return OverlayMode::Inline;

    switch (cfg.below_threshold) {
        case anchor::Fallback::NoPreview:
            // StatusOnly rather than Hidden. The fallback is named "no *preview*", and Hidden
            // suppresses everything — including the Unsupported and Warning states that §1.1 of
            // the supported-targets doc requires be surfaced before the user speaks. Removing
            // the text preview is the degradation; going silent is a different thing.
            return OverlayMode::StatusOnly;
        case anchor::Fallback::FloatingPill:
            break;
    }
    return OverlayMode::FloatingPill;
}

std::string key_legend(OverlayState state, bool composing, std::string_view hotkey,
                       bool push_to_talk) {
    // Only the three states where the session is the user's to steer. Finalizing is deliberately
    // not one of them: by then `stop` has gone out and none of these keys would do what the line
    // promised. Idle, ModelLoading, Warning, Unsupported and Error have no session to steer at
    // all, and the pill in those states is already saying the one thing that matters.
    if (state != OverlayState::Listening && state != OverlayState::Speaking &&
        state != OverlayState::Editing) {
        return std::string();
    }

    std::string out;
    const auto item = [&out](std::string_view text) {
        if (!out.empty()) out.append(" \xC2\xB7 ");  // U+00B7 MIDDLE DOT
        out.append(text);
    };

    if (composing) {
        // docs/editing.md §4.3's classes, in the order a user needs them: the way out that keeps
        // the text, the way out that discards it, then what this state offers that the other does
        // not. The hotkey is named only while editing, because §4.5 gives it exactly one meaning
        // and while the microphone is already open that meaning is a no-op — a legend that named
        // it there would be advertising a key that does nothing.
        //
        // Under push-to-talk the first item is the gesture the user is *already making*: their
        // finger is on the key, and letting go is what finishes. Enter still commits — the filter
        // does not change — but naming it first would put the second-best way out in front of the
        // one the user chose in settings.
        item(push_to_talk ? "let go to finish" : "Enter to finish");
        item("Esc to cancel");
        if (state == OverlayState::Editing) {
            if (!hotkey.empty()) {
                // Editing is reachable under push-to-talk — an edit key pressed with the other
                // hand closes the microphone while the binding is still held — and the verb has
                // to match the gesture. "Win+Y to resume" would be asking for a press from
                // someone who has never let go.
                std::string resume;
                if (push_to_talk) resume.append("hold ");
                resume.append(hotkey);
                resume.append(" to resume");
                item(resume);
            }
        } else {
            item("type or click to edit");
        }
        return out;
    }

    // plan.md §1.8's session, which is what runs whenever the compose buffer does not exist.
    // Enter is not a commit here and must not be named as one; the hotkey finishes, and every
    // other key ends the session by passing through to the target.
    if (!hotkey.empty()) {
        std::string finish;
        if (push_to_talk) finish.append("let go of ");
        finish.append(hotkey);
        finish.append(" to finish");
        item(finish);
    }
    item("Esc to cancel");
    item("any key ends it");
    return out;
}

const char* to_string(OverlayState s) noexcept {
    switch (s) {
        case OverlayState::Idle: return "Idle";
        case OverlayState::Listening: return "Listening";
        case OverlayState::Speaking: return "Speaking";
        case OverlayState::Finalizing: return "Finalizing";
        case OverlayState::ModelLoading: return "ModelLoading";
        case OverlayState::Warning: return "Warning";
        case OverlayState::Unsupported: return "Unsupported";
        case OverlayState::Editing: return "editing";
        case OverlayState::Error: return "Error";
    }
    return "Idle";
}

const char* to_string(ThemeMode m) noexcept {
    switch (m) {
        case ThemeMode::System: return "system";
        case ThemeMode::Light: return "light";
        case ThemeMode::Dark: return "dark";
        case ThemeMode::Custom: return "custom";
    }
    return "system";
}

uint32_t state_dot_color(OverlayState s, bool dark) noexcept {
    const DotColors& d = kDots[dark ? 1 : 0];
    switch (s) {
        case OverlayState::Idle: return d.idle;
        case OverlayState::Listening: return d.listening;
        case OverlayState::Speaking: return d.speaking;
        case OverlayState::Finalizing: return d.finalizing;
        case OverlayState::ModelLoading: return d.loading;
        // The pill overrides this one with its own border colour — see dot_color below. The
        // shared answer is the amber of the table, which is what the tray shows.
        case OverlayState::Warning: return d.loading;
        case OverlayState::Unsupported: return d.unsupported;
        case OverlayState::Editing: return d.editing;
        case OverlayState::Error: return d.error;
    }
    return d.idle;
}

const OverlayPalette& overlay_palette(bool dark) noexcept {
    // The light half is OverlayStyle's declared defaults, character for character. That is not a
    // coincidence to be tidied away: it is what makes a settings file written before this
    // existed paint identically after it, and what makes `Custom` on an untouched file look like
    // `Light` rather than like a reset.
    static constexpr OverlayPalette kLight{
        0xFF3C3C3C,  // pending    — near-black text on a near-white pill
        0x993C3C3C,  // unstable   — the same ink, 60 % opaque
        0xE6FFFFFF,  // background — white, slightly transparent, so the document shows through
        0xFFB34700,  // warning
        0x33000000,  // border     — a wash of the text colour, not a line
    };
    // The dark half is the same construction with the ink and the ground exchanged, at the same
    // opacities: the transparency is what makes the pill read as sitting *over* the document
    // rather than as a hole cut in it, and that has to survive the scheme change.
    static constexpr OverlayPalette kDark{
        0xFFE8E8E8,  // pending    — not pure white; white on near-black vibrates
        0x99E8E8E8,  // unstable
        0xE61E1E1E,  // background
        0xFFFFB74D,  // warning    — the same amber, lightened until it carries on near-black
        0x33FFFFFF,  // border
    };
    return dark ? kDark : kLight;
}

bool theme_paints_dark(ThemeMode mode, bool system_dark) noexcept {
    switch (mode) {
        case ThemeMode::System: return system_dark;
        case ThemeMode::Light: return false;
        case ThemeMode::Dark: return true;
        // Not "false because Custom is light" — false because nothing about a hand-picked
        // palette may be second-guessed, and every consumer of this answer (the dot set, the
        // selection wash, the microphone button) would be doing exactly that.
        case ThemeMode::Custom: return false;
    }
    return system_dark;
}

OverlayStyle resolve_overlay_style(const OverlayStyle& base, bool system_dark) {
    OverlayStyle out = base;
    if (base.theme == ThemeMode::Custom) {
        return out;
    }
    const OverlayPalette& p = overlay_palette(theme_paints_dark(base.theme, system_dark));
    out.color_pending = p.pending;
    out.color_unstable = p.unstable;
    out.color_background = p.background;
    out.color_warning = p.warning;
    out.color_border = p.border;
    return out;
}

namespace {

// ─── The window's state ─────────────────────────────────────────────────────────────────────
//
// Overlay::Impl derives from this so that the free WndProc — which cannot name the private
// nested type Overlay::Impl — has something to cast GWLP_USERDATA back to. That is the whole
// reason for the split; there is no second implementation.
//
// **UI thread only.** overlay.h states it and it is not defensive: the D2D factory below is
// created SINGLE_THREADED on purpose (the multi-threaded factory takes a lock per call and
// there is no second thread that needs it), so a call from the anchor worker thread is a data
// race that presents as an intermittent device-lost error rather than as a crash.
struct OverlayCore {
    HWND hwnd = nullptr;
    HINSTANCE instance = nullptr;

    // Two styles, and the distinction is load-bearing. `style` is what the caller configured and
    // what style() hands back — it is the user's settings and this file never edits it.
    // `painted` is that style with the colour scheme folded in, recomputed whenever either half
    // moves, and it is what every brush and every colour in paint_once() is built from. Reading
    // `style` for a colour would paint the *custom* palette in every mode.
    OverlayStyle style{};
    OverlayStyle painted{};
    bool system_dark = false;  // the OS's answer, refreshed on WM_SETTINGCHANGE
    bool painting_dark = false;

    OverlayMode mode = OverlayMode::Inline;
    OverlayState state = OverlayState::Idle;

    std::string pending;
    std::string unstable;
    std::optional<anchor::Anchor> anchor;

    bool violation = false;
    std::string violation_reason;

    // ── The compose buffer (docs/editing.md §7) ─────────────────────────────────────────────
    //
    // All three offsets are UTF-8 byte offsets into `pending`, clamped into it on the way in.
    bool edit_active = false;
    size_t caret = 0;
    size_t sel_begin = 0;
    size_t sel_end = 0;

    // §4.4's one-line hint, expired by the caller on the tick it already runs.
    std::string hint;

    // §7.5's key legend. Pushed once by the caller (the binding's spelling, and whether the pill
    // is a compose buffer this session); the line itself is composed per paint from `state`.
    std::string legend_hotkey;
    bool legend_composing = false;
    bool legend_push_to_talk = false;

    // §7.3. `scroll_pinned` is the default and is what makes a session with no editing look and
    // behave exactly as it does today: the view follows the bottom of a transcript that only ever
    // grows. The wheel and any caret movement unpin it.
    uint32_t scroll_line = 0;
    bool scroll_pinned = true;

    // What the last paint laid out, kept so a mouse hit test can ask the same layout the user is
    // looking at rather than building a second one that might disagree with it (§5.2). Cleared
    // whenever the layout is rebuilt.
    ComRef<IDWriteTextLayout> hit_layout;
    float hit_origin_x = 0.0f;   // DIPs, relative to the pill's own top-left
    float hit_origin_y = 0.0f;
    size_t hit_cut_units = 0;    // leading UTF-16 units dropped by the front-elision
    uint32_t hit_dpi = USER_DEFAULT_SCREEN_DPI;
    anchor::Rect painted_rect{};   // physical screen pixels, as of the last paint

    // The microphone button, in DIPs relative to the pill's top-left. Empty unless the last paint
    // drew one, which it does only while editing.
    float mic_left = 0.0f, mic_top = 0.0f, mic_right = 0.0f, mic_bottom = 0.0f;

    // Is a point — physical pixels, relative to the pill's top-left, the units the hook publishes
    // — over that button? Asked by two callers that must agree: the click filter, which resumes
    // dictation, and WM_SETCURSOR, which promises with the pointer shape that the click will do
    // something. A cursor that said "button" where the click lands on text would be a lie.
    [[nodiscard]] bool point_in_mic(int32_t x, int32_t y) const noexcept {
        if (mic_right <= mic_left || mic_bottom <= mic_top) {
            return false;  // the last paint drew no button, which means we are not editing
        }
        const float scale =
            static_cast<float>(hit_dpi) / static_cast<float>(USER_DEFAULT_SCREEN_DPI);
        if (scale <= 0.0f) {
            return false;
        }
        const float px = static_cast<float>(x) / scale;
        const float py = static_cast<float>(y) / scale;
        return px >= mic_left && px < mic_right && py >= mic_top && py < mic_bottom;
    }

    // docs/editing.md §5.1. False for the whole of a normal session and true only while a
    // composing session is running; WS_EX_NOACTIVATE is untouched either way.
    bool click_through = true;

    // overlay.h: the microphone's own level, and how long the transcriber has been quiet. The
    // two halves of "is it listening?" that a static dot could not tell apart.
    float input_level = 0.0f;
    uint64_t transcriber_lag_ms = 0;

    bool want_visible = false;
    bool dirty = true;

    // False when SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) failed — see create().
    bool capture_excluded = false;

    uint32_t dpi = USER_DEFAULT_SCREEN_DPI;
    // Last rectangle reported to the log, so placement is logged on change rather than on
    // every frame. See paint_once().
    anchor::Rect last_logged_rect{};

    DibSurface surface;
    ComRef<ID2D1Factory> d2d;
    ComRef<IDWriteFactory> dwrite;
    ComRef<ID2D1DCRenderTarget> rt;
    ComRef<IDWriteTextFormat> format_body;
    ComRef<IDWriteTextFormat> format_note;
    ComRef<ID2D1SolidColorBrush> brush_pending;
    ComRef<ID2D1SolidColorBrush> brush_unstable;
    ComRef<ID2D1SolidColorBrush> brush_background;
    ComRef<ID2D1SolidColorBrush> brush_warning;
    ComRef<ID2D1SolidColorBrush> brush_border;
    ComRef<ID2D1SolidColorBrush> brush_dot;

    // ── The colour scheme ───────────────────────────────────────────────────────────────────

    // Recompute `painted` from `style` and the OS's current answer. Called from create(), from
    // set_style(), and from the WM_SETTINGCHANGE that says the scheme moved.
    //
    // Returns true when the colours actually changed, because the brushes are device resources
    // and throwing them away is not free: WM_SETTINGCHANGE arrives for a great many things that
    // are not the colour scheme, and under any mode but System the answer is the same one every
    // time. A repaint that rebuilt the render target on every taskbar move would be a cost paid
    // for nothing.
    bool refresh_theme(bool ask_the_os) {
        if (ask_the_os) {
            system_dark = theme::system_prefers_dark();
        }
        const OverlayStyle next = resolve_overlay_style(style, system_dark);
        const bool changed = next.color_pending != painted.color_pending ||
                             next.color_unstable != painted.color_unstable ||
                             next.color_background != painted.color_background ||
                             next.color_warning != painted.color_warning ||
                             next.color_border != painted.color_border;
        painted = next;
        painting_dark = theme_paints_dark(style.theme, system_dark);
        if (changed) {
            release_device_resources();
            dirty = true;
        }
        return changed;
    }

    // ── Device resources ────────────────────────────────────────────────────────────────────

    void release_device_resources() noexcept {
        hit_layout.reset();
        brush_dot.reset();
        brush_border.reset();
        brush_warning.reset();
        brush_background.reset();
        brush_unstable.reset();
        brush_pending.reset();
        rt.reset();
    }

    void release_text_formats() noexcept {
        hit_layout.reset();  // a layout outlives neither the format it was built from nor the DPI
        format_note.reset();
        format_body.reset();
    }

    HRESULT ensure_factories();
    HRESULT ensure_text_formats();
    HRESULT ensure_render_target();

    // ── Geometry ────────────────────────────────────────────────────────────────────────────

    [[nodiscard]] OverlayMode effective_mode() const noexcept;
    [[nodiscard]] anchor::Rect monitor_work_area_and_dpi(uint32_t& dpi_out) const;

    // ── Painting ────────────────────────────────────────────────────────────────────────────

    [[nodiscard]] bool has_anything_to_show() const noexcept;
    HRESULT paint_once();
    void render();
    void apply_visibility();
};

HRESULT OverlayCore::ensure_factories() {
    if (!d2d) {
        // SINGLE_THREADED: see the threading note above.
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.put());
        if (FAILED(hr)) {
            return hr;
        }
    }
    if (!dwrite) {
        HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                         reinterpret_cast<IUnknown**>(dwrite.put()));
        if (FAILED(hr)) {
            return hr;
        }
    }
    return S_OK;
}

HRESULT OverlayCore::ensure_text_formats() {
    if (format_body && format_note) {
        return S_OK;
    }
    release_text_formats();

    std::wstring family = utf8::to_utf16_or_empty(style.font_family);
    if (family.empty()) {
        family = L"Segoe UI";  // OverlayStyle's own default; a malformed setting must not un-draw
    }

    // The locale drives font fallback and shaping. An empty locale string is legal but leaves
    // DirectWrite to guess, which produces visibly different CJK glyph choices between machines.
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) == 0) {
        wcscpy_s(locale, L"en-us");
    }

    // Font size is in DIPs and stays in DIPs: per-monitor scaling is applied by the render
    // target's DPI (see paint_once), not by rewriting the size here. Doing it here would round
    // the size per monitor and make the same text reflow differently on each.
    const float size = std::clamp(style.font_size_dip, 6.0f, 96.0f);

    HRESULT hr = dwrite->CreateTextFormat(family.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                          size, locale, format_body.put());
    if (FAILED(hr)) {
        return hr;
    }
    format_body->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

    hr = dwrite->CreateTextFormat(family.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                  size * 0.85f, locale, format_note.put());
    if (FAILED(hr)) {
        release_text_formats();
        return hr;
    }
    format_note->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    return S_OK;
}

HRESULT OverlayCore::ensure_render_target() {
    if (rt) {
        return S_OK;
    }

    // ── Why UpdateLayeredWindow over a D2D-painted DIB, and not a swap chain ────────────────
    //
    // A layered (non-composited) window has no swap chain of its own; the D2D-on-swap-chain
    // route means a DXGI composition surface, a DirectComposition visual tree, and a
    // WS_EX_NOREDIRECTIONBITMAP window — three more moving parts, two more link libraries
    // (d3d11, dxgi) that CMakeLists deliberately does not list, and a different set of
    // device-loss cases. UpdateLayeredWindow with a D2D-rendered DIB section is the simpler,
    // well-trodden path, and the frame rate it cannot reach is irrelevant here: overlay.h is
    // explicit that this window repaints a few times a second and runs no render loop.
    //
    // The pixel format must be premultiplied BGRA — that is what AC_SRC_ALPHA in the
    // BLENDFUNCTION below means, and D2D writes premultiplied output for this alpha mode, so no
    // conversion pass is needed.
    //
    // ── _SOFTWARE, deliberately, and it is not a fallback ───────────────────────────────────
    //
    // `_DEFAULT` means "hardware if there is any", and Direct2D honours it by creating a D3D11
    // device through DXGI. That loads `d3d11.dll`, `dxgi.dll` and the vendor's user-mode driver
    // into this process and costs ~18 MB of **dedicated video memory**, measured on an NVIDIA
    // adapter with `\GPU Process Memory(*)\Dedicated Usage` — a fixed price for owning a
    // device, unrelated to the size of the pill. A user found it in Task Manager and asked the
    // obvious question: why does a text overlay need VRAM?
    //
    // It does not, and the hardware path is the wrong shape for this window anyway. The
    // destination is a DIB section in system memory, because `UpdateLayeredWindow` takes a DC
    // and nothing else; rendering into video memory only to read it back is work in the wrong
    // direction. overlay.h is explicit that this window repaints a few times a second and runs
    // no render loop, so there is no frame rate to defend — and the software rasteriser also
    // sidesteps the alpha-channel question the old comment here flagged as unverified, and any
    // GPU wake-up or hybrid-graphics switch on a laptop.
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 0.0f, 0.0f,
        D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);

    HRESULT hr = d2d->CreateDCRenderTarget(&props, rt.put());
    if (FAILED(hr)) {
        // Software rasterisation is part of Direct2D itself and has no device to lose, so this
        // is close to unreachable. `_DEFAULT` is the one thing left to try rather than giving
        // up and drawing nothing: a pill that costs VRAM beats no pill at all.
        props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
        hr = d2d->CreateDCRenderTarget(&props, rt.put());
        if (FAILED(hr)) {
            return hr;
        }
        KD_LOG_WARN("D2D software DC render target failed (hr=0x%08lX); using the hardware path, "
                    "which costs this process a D3D device and its video memory",
                    static_cast<unsigned long>(hr));
    }

    struct BrushSpec {
        ComRef<ID2D1SolidColorBrush>* slot;
        uint32_t color;
    };
    const BrushSpec specs[] = {
        {&brush_pending, painted.color_pending},
        {&brush_unstable, painted.color_unstable},
        {&brush_background, painted.color_background},
        {&brush_warning, painted.color_warning},
        {&brush_border, painted.color_border},
        {&brush_dot, dot_color(state, painted, painting_dark)},
    };
    for (const BrushSpec& spec : specs) {
        hr = rt->CreateSolidColorBrush(argb(spec.color), spec.slot->put());
        if (FAILED(hr)) {
            release_device_resources();
            return hr;
        }
    }
    return S_OK;
}

OverlayMode OverlayCore::effective_mode() const noexcept {
    // The authoritative degradation choice belongs to the caller: anchor::AnchorConfig
    // ::below_threshold selects FloatingPill or NoPreview and main.cpp is expected to call
    // set_mode() accordingly (anchor::AnchorEstimator::current_fallback() returns exactly that).
    // This is only the "the caller has not told us yet" default, and it matches AnchorConfig's
    // own default. The overlay never invents a position from a stale anchor — plan.md §1.5's
    // deterministic-degradation rule is the reason this is one branch and not a heuristic.
    if (mode == OverlayMode::Inline && !anchor.has_value()) {
        return OverlayMode::FloatingPill;
    }
    return mode;
}

anchor::Rect OverlayCore::monitor_work_area_and_dpi(uint32_t& dpi_out) const {
    HMONITOR mon = nullptr;
    if (anchor.has_value()) {
        POINT pt{anchor->insertion_point.x, anchor->insertion_point.y};
        mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    } else {
        // No anchor: the target's monitor is the next best thing, and the foreground window is
        // the target by definition — this window can never be it (WS_EX_NOACTIVATE).
        HWND fg = GetForegroundWindow();
        mon = fg ? MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY)
                 : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }

    anchor::Rect work{};
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) {
        // rcWork excludes the taskbar. Physical pixels, because the process is per-monitor
        // DPI aware v2 — the same units anchor.h publishes.
        work.left = mi.rcWork.left;
        work.top = mi.rcWork.top;
        work.right = mi.rcWork.right;
        work.bottom = mi.rcWork.bottom;
    }

    dpi_out = USER_DEFAULT_SCREEN_DPI;
    if (mon) {
        UINT dx = 0;
        UINT dy = 0;
        if (SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy)) && dx != 0) {
            // Windows reports square DPI for every display it supports; dy is read and ignored
            // rather than averaged, because a non-square value would mean something we do not
            // model and silently averaging it would hide that.
            dpi_out = static_cast<uint32_t>(dx);
        }
    }
    return work;
}

bool OverlayCore::has_anything_to_show() const noexcept {
    if (mode == OverlayMode::Hidden) {
        return false;
    }
    if (violation || state == OverlayState::Warning || state == OverlayState::Unsupported ||
        state == OverlayState::Error || state == OverlayState::ModelLoading) {
        return true;
    }
    if (mode == OverlayMode::StatusOnly) {
        return state != OverlayState::Idle;
    }
    return !pending.empty() || !unstable.empty() || state != OverlayState::Idle;
}

// One paint. Returns D2DERR_RECREATE_TARGET unchanged so render() can retry exactly once.
HRESULT OverlayCore::paint_once() {
    HRESULT hr = ensure_factories();
    if (FAILED(hr)) {
        return hr;
    }
    hr = ensure_text_formats();
    if (FAILED(hr)) {
        return hr;
    }
    hr = ensure_render_target();
    if (FAILED(hr)) {
        return hr;
    }

    uint32_t monitor_dpi = USER_DEFAULT_SCREEN_DPI;
    const anchor::Rect work = monitor_work_area_and_dpi(monitor_dpi);
    dpi = monitor_dpi;
    const float scale = static_cast<float>(monitor_dpi) / static_cast<float>(USER_DEFAULT_SCREEN_DPI);

    const OverlayMode em = effective_mode();

    // ── Build the strings ───────────────────────────────────────────────────────────────────

    std::wstring body;
    size_t pending_units = 0;
    if (em == OverlayMode::StatusOnly) {
        body = utf8::to_utf16_or_empty(state_label(state));
        pending_units = body.size();
    } else {
        std::wstring wp = wide_or_log(pending, "pending text");
        std::wstring wu = wide_or_log(unstable, "unstable text");
        pending_units = wp.size();
        body = std::move(wp);
        body.append(wu);
    }

    std::wstring note;
    if (!hint.empty() && !violation) {
        // docs/editing.md §4.4: `Ctrl+S — press Enter to finish first`. It takes the note slot
        // ahead of the state label because it is the only line here the user has to act on, and
        // it yields to a violation because a violation is about the text and this is not.
        note = wide_or_log(hint, "hint");
    } else if (violation) {
        // plan.md §1.4: a visible, persistent mark naming which utterance is suspect — not a
        // toast that disappears before the user has finished reading their own document.
        note = utf8::to_utf16_or_empty("\xE2\x9A\xA0 ");  // U+26A0 WARNING SIGN
        note.append(wide_or_log(violation_reason, "violation reason"));
    } else if (transcriber_lag_ms >= kLagNoteMs &&
               (state == OverlayState::Listening || state == OverlayState::Speaking)) {
        // The words are still coming; they are just behind. Said here rather than through
        // `state`, because the session is not in a different state and the microphone has not
        // done anything wrong — see set_transcriber_lag(). The seconds are in it because "5s"
        // and "20s" call for different reactions from the speaker.
        std::string line = "â³ heard you â waiting on the transcriber (";
        line += std::to_string(transcriber_lag_ms / 1000);
        line += "s)";
        note = utf8::to_utf16_or_empty(line);
    } else if (em != OverlayMode::StatusOnly && state != OverlayState::Idle &&
               state != OverlayState::Listening && state != OverlayState::Speaking) {
        note = utf8::to_utf16_or_empty(state_label(state));
    }

    // ── Measure ─────────────────────────────────────────────────────────────────────────────

    const float pad = std::max(0.0f, style.padding_dip);
    const float gap = std::max(2.0f, pad * 0.5f);
    const float dot_r = std::clamp(style.font_size_dip * 0.22f, 2.0f, 10.0f);
    const float text_x = pad + dot_r * 2.0f + gap;
    const float max_text_w =
        std::max(64.0f, std::clamp(style.max_width_dip, 120.0f, 4096.0f) - text_x - pad);

    ComRef<IDWriteTextLayout> body_layout;
    DWRITE_TEXT_METRICS body_metrics{};
    size_t cut = 0;
    size_t shown_len = 0;

    // The scrolled view, in DIPs relative to the top of the laid-out text. Everything the draw
    // section below needs to put a window onto a buffer taller than the pill.
    float view_top = 0.0f;      // how far down the layout the visible window starts
    float view_height = 0.0f;   // how tall that window is
    // What the scroll indicator needs, hoisted out of the block that computes them: how many
    // lines the buffer has, and how many of them the window is showing. Equal means the whole
    // buffer fits and there is nothing to indicate.
    uint32_t total_lines = 0;
    uint32_t shown_lines = 0;
    bool caret_known = false;
    float caret_x = 0.0f, caret_y = 0.0f, caret_h = 0.0f;
    std::vector<DWRITE_HIT_TEST_METRICS> selection_boxes;

    hit_layout.reset();

    if (!body.empty()) {
        // One front-elision, as a ceiling rather than as a fitting loop — see kMaxDisplayUnits.
        // Scrollback is what shows the rest, so nothing below this line depends on the text
        // happening to be short.
        if (body.size() > kMaxDisplayUnits) {
            KD_LOG_WARN("overlay: %zu UTF-16 units exceeds the %zu-unit display cap; the oldest "
                        "text is elided and cannot be scrolled back to",
                        body.size(), kMaxDisplayUnits);
        }
        const std::wstring shown = elide_front(body, kMaxDisplayUnits, cut);
        shown_len = shown.size();

        // A tall box, not 4096: the layout is the whole buffer now and its height is the thing
        // being scrolled through. A max height that truncated it would make the lines below the
        // cut unreachable — which is exactly the trap §7.3 exists to close.
        hr = dwrite->CreateTextLayout(shown.c_str(), static_cast<UINT32>(shown.size()),
                                      format_body.get(), max_text_w, 1.0e6f, body_layout.put());
        if (FAILED(hr)) {
            return hr;
        }
        hr = body_layout->GetMetrics(&body_metrics);
        if (FAILED(hr)) {
            return hr;
        }

        // Colour the unstable tail differently so the user can see which part is still moving.
        // D2D's default text renderer honours an ID2D1Brush set as a drawing effect, which is
        // why this needs no custom IDWriteTextRenderer.
        const size_t shown_pending = (pending_units > cut) ? (pending_units - cut) : 0;
        // The ellipsis, when one was inserted, is coloured with the committed half: it is a
        // marker that text was dropped, not a piece of the hypothesis still being revised.
        const size_t total = shown_len;
        const UINT32 pending_end =
            static_cast<UINT32>(std::min<size_t>(shown_pending + (cut > 0 ? 1 : 0), total));
        if (pending_end > 0) {
            body_layout->SetDrawingEffect(brush_pending.get(),
                                          DWRITE_TEXT_RANGE{0, pending_end});
        }
        if (static_cast<size_t>(pending_end) < total) {
            body_layout->SetDrawingEffect(
                brush_unstable.get(),
                DWRITE_TEXT_RANGE{pending_end, static_cast<UINT32>(total) - pending_end});
        }

        // ── Where each line starts, so the view can be a whole number of lines ──────────────
        //
        // Not `metrics.height / lineCount`: a line carrying a font fallback (an emoji among
        // Latin text, say) is taller than its neighbours, and an averaged line height would
        // drift a fraction of a line per line until the window showed half of one at each end.
        std::vector<DWRITE_LINE_METRICS> lines(std::max<UINT32>(body_metrics.lineCount, 1));
        UINT32 line_count = 0;
        if (SUCCEEDED(body_layout->GetLineMetrics(lines.data(),
                                                  static_cast<UINT32>(lines.size()), &line_count)) &&
            line_count > 0) {
            std::vector<float> line_top(static_cast<size_t>(line_count) + 1, 0.0f);
            for (UINT32 i = 0; i < line_count; ++i) {
                line_top[i + 1] = line_top[i] + lines[i].height;
            }

            // ── The caret's line, which is what the view is required to keep visible ─────────
            //
            // §3's freeze rule again, arriving as a clamp: the caret is an offset into the
            // settled region, and the display string is the settled region with an ellipsis in
            // front of whatever was elided. A caret behind the elision has nowhere to be drawn,
            // so it is pinned to just after the ellipsis rather than silently drawn at zero.
            UINT32 caret_line = 0;
            if (edit_active) {
                const size_t caret_units = utf16_prefix_units(pending, caret);
                const size_t shown_caret =
                    caret_units > cut ? (caret_units - cut) + (cut > 0 ? 1 : 0) : (cut > 0 ? 1 : 0);
                DWRITE_HIT_TEST_METRICS hm{};
                if (SUCCEEDED(body_layout->HitTestTextPosition(
                        static_cast<UINT32>(std::min<size_t>(shown_caret, shown_len)), FALSE,
                        &caret_x, &caret_y, &hm))) {
                    caret_h = hm.height > 0.0f ? hm.height : lines[0].height;
                    caret_known = true;
                    for (UINT32 i = 0; i < line_count; ++i) {
                        if (caret_y + 0.5f < line_top[i + 1]) {
                            caret_line = i;
                            break;
                        }
                        caret_line = i;
                    }
                }

                // The selection, as the rectangles DirectWrite reports for the range. A
                // multi-line selection comes back as several; all of them are painted (§7.2).
                if (sel_end > sel_begin) {
                    const size_t b_units = utf16_prefix_units(pending, sel_begin);
                    const size_t e_units = utf16_prefix_units(pending, sel_end);
                    const size_t b = b_units > cut ? (b_units - cut) + (cut > 0 ? 1 : 0)
                                                   : (cut > 0 ? 1 : 0);
                    const size_t e = e_units > cut ? (e_units - cut) + (cut > 0 ? 1 : 0)
                                                   : (cut > 0 ? 1 : 0);
                    if (e > b) {
                        UINT32 actual = 0;
                        // Ask for the count first: HitTestTextRange returns E_NOT_SUFFICIENT_
                        // BUFFER with the required size rather than truncating, and a fixed-size
                        // array here would silently drop the tail of a long selection.
                        body_layout->HitTestTextRange(static_cast<UINT32>(b),
                                                      static_cast<UINT32>(e - b), 0.0f, 0.0f,
                                                      nullptr, 0, &actual);
                        if (actual > 0) {
                            selection_boxes.resize(actual);
                            UINT32 got = 0;
                            if (FAILED(body_layout->HitTestTextRange(
                                    static_cast<UINT32>(b), static_cast<UINT32>(e - b), 0.0f, 0.0f,
                                    selection_boxes.data(), actual, &got))) {
                                selection_boxes.clear();
                            } else {
                                selection_boxes.resize(got);
                            }
                        }
                    }
                }
            }

            const uint32_t visible = std::min<uint32_t>(kMaxLines, line_count);
            const uint32_t desired =
                scroll_pinned ? (line_count > visible ? line_count - visible : 0u) : scroll_line;
            scroll_line = clamp_scroll_line(desired, caret_line, caret_known, line_count, visible);

            view_top = line_top[scroll_line];
            view_height = line_top[scroll_line + visible] - view_top;
            total_lines = line_count;
            shown_lines = visible;
        } else {
            view_height = body_metrics.height;
        }

        // Kept for the mouse hit test, which must ask the same layout the user is looking at
        // rather than build a second one that could disagree with it (docs/editing.md §5.2).
        hit_layout = body_layout.clone();
        hit_cut_units = cut;
    }

    ComRef<IDWriteTextLayout> note_layout;
    DWRITE_TEXT_METRICS note_metrics{};
    if (!note.empty()) {
        hr = dwrite->CreateTextLayout(note.c_str(), static_cast<UINT32>(note.size()),
                                      format_note.get(), max_text_w, 4096.0f, note_layout.put());
        if (FAILED(hr)) {
            return hr;
        }
        hr = note_layout->GetMetrics(&note_metrics);
        if (FAILED(hr)) {
            return hr;
        }
    }

    // widthIncludingTrailingWhitespace, not width: a pending prefix ends in a held-back space
    // (plan.md §1.4) and clipping it would make the unstable tail sit against the previous word.
    const float body_w = body_layout ? body_metrics.widthIncludingTrailingWhitespace : 0.0f;
    const float note_w = note_layout ? note_metrics.widthIncludingTrailingWhitespace : 0.0f;
    // The *visible* height, which is the scrolled window onto the layout rather than the
    // layout's own height. A pill sized to a session-long buffer would cover the document it is
    // annotating, which is the whole reason kMaxLines exists.
    const float body_h = body_layout ? view_height : 0.0f;
    const float note_h = note_layout ? note_metrics.height : 0.0f;

    // The pill does not resize with its contents. It sits in one fixed place at the bottom of the
    // monitor, so a box that grew and shrank on every revision would jitter under the eye for no
    // information gain — and with a session-long buffer behind it, the width would otherwise be
    // pinned at the maximum nearly all the time anyway. The inline overlay keeps shrinking to fit,
    // because there it is a label attached to a caret and its size is what keeps it out of the way.
    // The microphone button (docs/editing.md §13 stage 3), drawn only while editing — the one
    // state where the hotkey's meaning has to be remembered, and therefore the one state where a
    // control that says what it does earns its space.
    ComRef<IDWriteTextLayout> mic_layout;
    DWRITE_TEXT_METRICS mic_metrics{};
    const bool want_mic = state == OverlayState::Editing && em != OverlayMode::StatusOnly;
    if (want_mic) {
        const std::wstring label = utf8::to_utf16_or_empty(kMicButtonLabel);
        if (SUCCEEDED(dwrite->CreateTextLayout(label.c_str(), static_cast<UINT32>(label.size()),
                                               format_note.get(), max_text_w, 4096.0f,
                                               mic_layout.put()))) {
            if (FAILED(mic_layout->GetMetrics(&mic_metrics))) mic_layout.reset();
        }
    }
    const float mic_w = mic_layout ? mic_metrics.widthIncludingTrailingWhitespace +
                                         kMicButtonPadDip * 2.0f
                                   : 0.0f;
    const float mic_h = mic_layout ? mic_metrics.height + kMicButtonPadDip : 0.0f;

    // The key legend (docs/editing.md §7.5), on the bottom line — **the pill only**. StatusOnly
    // has nothing to steer with the keys; Inline is a label attached to a caret, sized to get out
    // of the way, and a standing two-line legend there would make it the largest thing on the
    // line it is annotating. The pill is the mode with room, and it is where the compose buffer,
    // the mouse and the microphone button already live.
    ComRef<IDWriteTextLayout> legend_layout;
    DWRITE_TEXT_METRICS legend_metrics{};
    if (em == OverlayMode::FloatingPill) {
        const std::wstring line = utf8::to_utf16_or_empty(
            key_legend(state, legend_composing, legend_hotkey, legend_push_to_talk));
        if (!line.empty() &&
            SUCCEEDED(dwrite->CreateTextLayout(line.c_str(), static_cast<UINT32>(line.size()),
                                               format_note.get(), max_text_w, 4096.0f,
                                               legend_layout.put()))) {
            if (FAILED(legend_layout->GetMetrics(&legend_metrics))) legend_layout.reset();
        }
    }
    const float legend_w =
        legend_layout ? legend_metrics.widthIncludingTrailingWhitespace : 0.0f;
    const float legend_h = legend_layout ? legend_metrics.height : 0.0f;

    const float measured_w = std::max(std::max(std::max(body_w, note_w), mic_w), legend_w);
    const float content_w = (mode == OverlayMode::FloatingPill && measured_w > 0.0f)
                                ? max_text_w
                                : measured_w;
    const float content_h = body_h + (note_layout ? (body_layout ? gap : 0.0f) + note_h : 0.0f) +
                            (mic_layout ? gap + mic_h : 0.0f) +
                            (legend_layout ? gap + legend_h : 0.0f);

    // A minimum height so a bare status dot still reads as a control and not as a speck.
    const float box_w_dip = text_x + std::max(content_w, dot_r * 2.0f) + pad;
    const float box_h_dip = pad + std::max(content_h, dot_r * 2.0f) + pad;

    const int box_w_px = static_cast<int>(std::ceil(box_w_dip * scale));
    const int box_h_px = static_cast<int>(std::ceil(box_h_dip * scale));
    if (box_w_px <= 0 || box_h_px <= 0) {
        return S_OK;
    }

    // ── Place ───────────────────────────────────────────────────────────────────────────────

    const anchor::Anchor placement = anchor.value_or(anchor::Anchor{});
    const anchor::Rect rect = layout_overlay(placement, box_w_px, box_h_px, work, em);
    if (rect.empty()) {
        return S_OK;
    }

    // Where the overlay actually went, logged when it changes.
    //
    // "The pill is below the screen, I can barely see a sliver" is not a diagnosable report, and
    // it was the first thing a human said about the floating pill. Nothing recorded the work
    // area, the chosen monitor or the resulting rectangle, so the difference between "the layout
    // arithmetic is wrong", "the anchor picked a monitor the user is not looking at" and "the
    // work area came back empty so the final clamp was skipped" could not be told apart after
    // the fact. All three are one line of evidence apart.
    if (rect.left != last_logged_rect.left || rect.top != last_logged_rect.top ||
        rect.right != last_logged_rect.right || rect.bottom != last_logged_rect.bottom) {
        last_logged_rect = rect;
        KD_LOG_INFO("overlay placed: mode=%s rect=[%d,%d %dx%d] work=[%d,%d %dx%d]%s "
                    "anchor=%s(%d,%d) dpi=%u",
                    to_string(em), rect.left, rect.top, rect.width(), rect.height(), work.left,
                    work.top, work.width(), work.height(),
                    work.empty() ? " EMPTY WORK AREA - the final clamp was skipped" : "",
                    anchor.has_value() ? anchor::to_string(anchor->source) : "none",
                    placement.insertion_point.x, placement.insertion_point.y, dpi);
    }

    // ── Draw ────────────────────────────────────────────────────────────────────────────────

    if (!surface.ensure(box_w_px, box_h_px)) {
        return HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);
    }

    RECT bind{0, 0, box_w_px, box_h_px};
    hr = rt->BindDC(surface.dc, &bind);
    if (FAILED(hr)) {
        return hr;
    }
    // Set the DPI after BindDC, every frame: this is what makes font_size_dip mean the same
    // physical size on a 150 % monitor and a 100 % one. It is also the only thing that has to
    // change when the overlay crosses monitors, which is why WM_DPICHANGED just marks dirty.
    const float dpif = static_cast<float>(monitor_dpi);
    rt->SetDpi(dpif, dpif);

    // Draw in the DIP-space that exactly covers the pixel surface, so the background fills it
    // to the edge after the ceil() above rather than leaving a transparent hairline.
    const float draw_w = static_cast<float>(box_w_px) / scale;
    const float draw_h = static_cast<float>(box_h_px) / scale;

    // The dot brush is the only one whose colour depends on state; update it in place rather
    // than recreating the render target on every state change.
    if (brush_dot) {
        brush_dot->SetColor(argb(dot_color(state, painted, painting_dark)));
    }

    rt->BeginDraw();
    rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    // Fully transparent, not "the background colour": every pixel we do not paint must let the
    // target's own pixels through, which is the entire point of the layered window.
    rt->Clear(D2D1::ColorF(0, 0.0f));

    const float radius = std::max(0.0f, style.corner_radius_dip);
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, draw_w - 0.5f, draw_h - 0.5f),
                                             radius, radius);
    rt->FillRoundedRectangle(rr, brush_background.get());
    if (violation) {
        rt->DrawRoundedRectangle(rr, brush_warning.get(), 2.0f);
    } else {
        rt->DrawRoundedRectangle(rr, brush_border.get(), 1.0f);
    }

    // The dot, breathing with the microphone. `live` is the only condition under which the level
    // means anything: in every other state the device is closed or the session is over, and a dot
    // that pulsed there would be reporting a microphone nobody is holding open.
    const bool live = state == OverlayState::Listening || state == OverlayState::Speaking;
    const float level = live ? input_level : 0.0f;
    const float dot_scale = live ? (kDotLevelFloor + kDotLevelGain * level) : 1.0f;
    const D2D1_POINT_2F dot_centre = D2D1::Point2F(pad + dot_r, pad + dot_r + 1.0f);
    rt->FillEllipse(D2D1::Ellipse(dot_centre, dot_r * dot_scale, dot_r * dot_scale),
                    brush_dot.get());

    // The meter, down the gutter the dot already reserves. Only when the pill is tall enough for
    // a bar to be readable — see kMeterMinGutterDip — and only while the device is open.
    const float meter_top = dot_centre.y + dot_r + gap;
    const float meter_bottom = draw_h - pad;
    if (live && meter_bottom - meter_top >= kMeterMinGutterDip) {
        const float meter_w = std::max(2.0f, dot_r * 0.8f);
        const float meter_x = dot_centre.x - meter_w * 0.5f;
        const float meter_r = meter_w * 0.5f;
        const D2D1_ROUNDED_RECT track = D2D1::RoundedRect(
            D2D1::RectF(meter_x, meter_top, meter_x + meter_w, meter_bottom), meter_r, meter_r);
        if (brush_border) {
            const D2D1_COLOR_F was = brush_border->GetColor();
            brush_border->SetColor(argb(kMeterTrack[painting_dark ? 1 : 0]));
            rt->FillRoundedRectangle(track, brush_border.get());
            brush_border->SetColor(was);
        }
        // Rising from the bottom, which is where a level meter's zero belongs: the fill grows
        // toward the dot, so the eye reads the two as one indicator rather than as a bar and an
        // unrelated circle.
        const float fill_h = (meter_bottom - meter_top) * level;
        if (fill_h > 0.5f) {
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(meter_x, meter_bottom - fill_h, meter_x + meter_w, meter_bottom),
                    meter_r, meter_r),
                brush_dot.get());
        }
    }

    if (body_layout) {
        // The scrolled window. Everything is drawn at `-view_top` and clipped to the pill's text
        // area, so the lines above and below the window exist in the layout — which is what makes
        // them reachable by scrolling and by the arrow keys — without being painted.
        const D2D1_RECT_F clip = D2D1::RectF(text_x, pad, text_x + std::max(content_w, 1.0f),
                                             pad + body_h);
        rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);

        const float origin_x = text_x;
        const float origin_y = pad - view_top;

        // Under the text, so the glyphs keep their own colours and the committed/unstable split
        // stays readable through the selection (docs/editing.md §7.2).
        if (!selection_boxes.empty() && brush_unstable) {
            const D2D1_COLOR_F was = brush_unstable->GetColor();
            brush_unstable->SetColor(argb(kSelectionFill[painting_dark ? 1 : 0]));
            for (const DWRITE_HIT_TEST_METRICS& b : selection_boxes) {
                rt->FillRectangle(D2D1::RectF(origin_x + b.left, origin_y + b.top,
                                              origin_x + b.left + b.width,
                                              origin_y + b.top + b.height),
                                  brush_unstable.get());
            }
            brush_unstable->SetColor(was);
        }

        rt->DrawTextLayout(D2D1::Point2F(origin_x, origin_y), body_layout.get(),
                           brush_pending.get(), D2D1_DRAW_TEXT_OPTIONS_NONE);

        // A solid bar, never a blinking one (docs/editing.md §7.1). No timer, no power cost, and
        // no ambiguity about whether the caret is there.
        if (caret_known && brush_pending) {
            rt->FillRectangle(D2D1::RectF(origin_x + caret_x, origin_y + caret_y,
                                          origin_x + caret_x + kCaretWidthDip,
                                          origin_y + caret_y + caret_h),
                              brush_pending.get());
        }

        rt->PopAxisAlignedClip();

        hit_origin_x = origin_x;
        hit_origin_y = origin_y;
        hit_dpi = monitor_dpi;

        // ── The scroll indicator (docs/editing.md §7.3) ─────────────────────────────────────
        //
        // Until this there was none, and overlay.cpp said so in as many words: "There is no
        // scrollbar and no way to look further back." Scrollback then arrived and made the second
        // half untrue while leaving the first half in place, which is the worse of the two
        // states — the wheel and the arrow keys move a view whose position nothing reports. The
        // front-elision ellipsis is not that report: it appears at `kMaxDisplayUnits`, about five
        // thousand words, so a session showing the last eight lines of eighteen shows nothing at
        // all to say the other ten exist. Reported as "it starts scrolling, but I don't see a
        // scroll bar appearing".
        //
        // In the right padding strip, which is already reserved and empty, so it costs no width
        // and cannot reflow anything. Track and thumb, proportional, drawn only when there is
        // something off-screen — a full-height thumb on a buffer that fits would be furniture.
        if (total_lines > shown_lines && shown_lines > 0 && brush_border && brush_unstable) {
            const float bar_w = std::max(2.0f, pad * 0.4f);
            const float bar_x = draw_w - pad * 0.5f - bar_w * 0.5f;
            const float bar_top = pad;
            const float bar_bottom = pad + body_h;
            const float bar_r = bar_w * 0.5f;
            if (bar_bottom - bar_top > bar_w * 2.0f) {
                const D2D1_COLOR_F was = brush_border->GetColor();
                brush_border->SetColor(argb(kMeterTrack[painting_dark ? 1 : 0]));
                rt->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(bar_x, bar_top, bar_x + bar_w, bar_bottom),
                                      bar_r, bar_r),
                    brush_border.get());
                brush_border->SetColor(was);

                const float track_h = bar_bottom - bar_top;
                // Never shorter than its own width, or a long session's thumb becomes a dot that
                // reads as a mark on the pill rather than as a position in a buffer.
                const float thumb_h =
                    std::max(bar_w * 2.0f, track_h * static_cast<float>(shown_lines) /
                                               static_cast<float>(total_lines));
                const float span = static_cast<float>(total_lines - shown_lines);
                const float at = std::clamp(static_cast<float>(scroll_line) / span, 0.0f, 1.0f);
                const float thumb_top = bar_top + (track_h - thumb_h) * at;
                rt->FillRoundedRectangle(
                    D2D1::RoundedRect(
                        D2D1::RectF(bar_x, thumb_top, bar_x + bar_w, thumb_top + thumb_h), bar_r,
                        bar_r),
                    brush_unstable.get());
            }
        }
    }
    const float note_y = pad + body_h + (body_layout ? gap : 0.0f);
    if (note_layout) {
        rt->DrawTextLayout(D2D1::Point2F(text_x, note_y), note_layout.get(),
                           violation ? brush_warning.get() : brush_unstable.get(),
                           D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    const float mic_y = note_y + (note_layout ? note_h + gap : 0.0f);
    mic_left = mic_top = mic_right = mic_bottom = 0.0f;
    if (mic_layout && brush_border && brush_pending) {
        mic_left = text_x;
        mic_top = mic_y;
        mic_right = text_x + mic_w;
        mic_bottom = mic_y + mic_h;

        const D2D1_ROUNDED_RECT button = D2D1::RoundedRect(
            D2D1::RectF(mic_left, mic_top, mic_right, mic_bottom), 4.0f, 4.0f);
        // The border brush recoloured in place rather than a seventh brush on OverlayCore: the
        // dot brush already does this, and a brush per decoration is how a render target's
        // resource list grows without anybody deciding it should.
        const D2D1_COLOR_F was = brush_border->GetColor();
        brush_border->SetColor(argb(kMicButtonFill[painting_dark ? 1 : 0]));
        rt->FillRoundedRectangle(button, brush_border.get());
        brush_border->SetColor(was);
        rt->DrawRoundedRectangle(button, brush_border.get(), 1.0f);

        rt->DrawTextLayout(D2D1::Point2F(mic_left + kMicButtonPadDip,
                                         mic_top + kMicButtonPadDip * 0.5f),
                           mic_layout.get(), brush_pending.get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    if (legend_layout && brush_unstable) {
        // Last line, below everything including the button, and in the unstable brush — the
        // dimmest thing the style defines. It is standing information rather than a message: it
        // must be readable when looked for and must not compete with the words the user is
        // dictating, which is the one way a permanent legend can go wrong.
        const float legend_y = mic_y + (mic_layout ? mic_h + gap : 0.0f);
        rt->DrawTextLayout(D2D1::Point2F(text_x, legend_y), legend_layout.get(),
                           brush_unstable.get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    hr = rt->EndDraw();
    if (FAILED(hr)) {
        return hr;  // includes D2DERR_RECREATE_TARGET; render() decides what to do
    }

    // D2D may have reached the DIB through GDI. Flushing the GDI batch before the DIB bits are
    // read by UpdateLayeredWindow is the documented requirement for reading a DIB section that
    // GDI has drawn into; it is a no-op otherwise and costs nothing at this frame rate.
    GdiFlush();

    // ── Present ─────────────────────────────────────────────────────────────────────────────
    //
    // UpdateLayeredWindow moves, resizes and repaints in one call. Doing the move with a
    // separate SetWindowPos would show one frame of the previous content at the new position,
    // which reads as the overlay smearing across the screen as the caret advances.
    //
    // hdcDst is nullptr — documented as "use the screen DC" — so there is no DC to leak here.
    POINT dst{rect.left, rect.top};
    SIZE size{box_w_px, box_h_px};
    POINT src{0, 0};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;  // per-pixel alpha; the DIB is premultiplied

    // Where the pill actually is, for the mouse filter's point-in-rect test (docs/editing.md
    // §5.2) and for hit_test_text's coordinate conversion. Recorded from the values that were
    // just presented, not from GetWindowRect, so it can never describe a frame that has not
    // happened yet.
    painted_rect = anchor::Rect{rect.left, rect.top, rect.left + box_w_px, rect.top + box_h_px};

    if (!UpdateLayeredWindow(hwnd, nullptr, &dst, &size, surface.dc, &src, 0, &blend, ULW_ALPHA)) {
        const DWORD err = GetLastError();
        KD_LOG_WARN("UpdateLayeredWindow failed (%lu)", err);
        return HRESULT_FROM_WIN32(err);
    }
    return S_OK;
}

void OverlayCore::render() {
    if (!hwnd) {
        return;
    }
    if (!want_visible || !has_anything_to_show()) {
        apply_visibility();
        return;
    }

    HRESULT hr = paint_once();
    if (hr == D2DERR_RECREATE_TARGET) {
        // Routine on this machine: driver updates, remote-session transitions and display-mode
        // changes all produce it. overlay.h is explicit that it must be *handled*, not logged.
        KD_LOG_DEBUG("D2D device lost; recreating render target");
        release_device_resources();
        hr = paint_once();
    }
    if (FAILED(hr)) {
        KD_LOG_WARN("overlay paint failed (hr=0x%08lX)", static_cast<unsigned long>(hr));
        return;
    }

    dirty = false;
    apply_visibility();
}

void OverlayCore::apply_visibility() {
    if (!hwnd) {
        return;
    }
    const bool show_now = want_visible && has_anything_to_show();
    const bool is_visible = IsWindowVisible(hwnd) != FALSE;
    if (show_now == is_visible) {
        return;
    }
    if (show_now) {
        // SW_SHOWNOACTIVATE, never SW_SHOW: WS_EX_NOACTIVATE covers mouse activation, but a
        // plain ShowWindow(SW_SHOW) on first display would still bring the window forward and
        // take focus from the target — and injection would then land in our own window.
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    } else {
        ShowWindow(hwnd, SW_HIDE);
    }
}

LRESULT CALLBACK overlay_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    auto* core = reinterpret_cast<OverlayCore*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!core) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    switch (msg) {
        case WM_MOUSEACTIVATE:
            // Belt and braces with WS_EX_NOACTIVATE. Clicks cannot reach us at all while
            // WS_EX_TRANSPARENT is set, but if a future change ever drops that flag this is the
            // line that keeps the target's focus.
            return MA_NOACTIVATE;

        case WM_NCHITTEST:
            // While click-through, this makes the window transparent to WindowFromPoint-style
            // probing as well as to clicks. While a composing session has dropped
            // WS_EX_TRANSPARENT (docs/editing.md §5.1), answering HTTRANSPARENT would undo the
            // whole thing — the click would be routed to the target after all, which is exactly
            // what the flag was dropped to prevent.
            return core->click_through ? HTTRANSPARENT : HTCLIENT;

        case WM_SETCURSOR: {
            // The pill's pointer shape is the only thing that says the pill can be clicked at
            // all: it takes no focus, it has no title bar, and nothing else about it looks like a
            // control (docs/editing.md §5.4). While click-through this message never arrives —
            // WS_EX_TRANSPARENT means the window under us answers it — so reaching here at all
            // means a composing session has made the pill clickable and the shape can promise
            // something true.
            if (core->click_through || LOWORD(lparam) != HTCLIENT) {
                break;  // the frame, or a state where a click is not ours: let the class cursor win
            }
            POINT pt{};
            if (!GetCursorPos(&pt) || !ScreenToClient(hwnd, &pt)) {
                break;  // an arrow from the class cursor beats guessing
            }
            // Client coordinates of a per-monitor-DPI-aware window are physical pixels measured
            // from the client origin, which for this borderless window is the pill's top-left —
            // exactly the units point_in_mic and hit_test_text take.
            const bool over_mic = core->point_in_mic(pt.x, pt.y);
            // A hand for the button, because it acts on a click; an I-beam everywhere else,
            // because everywhere else *is* the compose buffer and a click there places the caret.
            // Both are the shapes Windows has trained everyone to read, which is worth more than
            // a bespoke microphone cursor that would have to be drawn and would say less.
            SetCursor(LoadCursorW(nullptr, over_mic ? IDC_HAND : IDC_IBEAM));
            return TRUE;
        }

        case WM_ERASEBKGND:
            // A layered window's pixels come from UpdateLayeredWindow. Erasing would be a
            // wasted GDI round trip on the message-loop thread.
            return 1;

        case WM_PAINT: {
            // Layered windows painted with UpdateLayeredWindow do not paint through WM_PAINT,
            // but the message still has to be *answered properly*, and ValidateRect is not a
            // proper answer. ValidateRect clears the client update region only; it leaves the
            // window's internal paint state — the non-client half of it — set. Windows then
            // regenerates WM_PAINT the moment the queue drains, and GetMessage hands it back
            // forever: measured at ~190,000 WM_PAINT per second on an idle, *hidden* overlay,
            // which is one core of kernel time spent dispatching a message we already answered.
            // BeginPaint/EndPaint is the only pair that clears both halves. The empty body is
            // deliberate — the pixels come from UpdateLayeredWindow.
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DPICHANGED:
            // The suggested rectangle in lParam is deliberately ignored. It is a proportional
            // rescale of *this* window, but the overlay's position is derived from the anchor in
            // physical pixels and its size from text measured at the new monitor's DPI — so the
            // correct response is to re-measure and re-place from scratch, which render() does.
            core->dirty = true;
            core->render();
            return 0;

        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            // Two unrelated things arrive on the same message. Work areas move when a taskbar
            // does, and a stale work area puts the overlay under it — that is the unconditional
            // re-render below. The other is the colour scheme, which is only this message when
            // it names "ImmersiveColorSet", and is the one case worth re-reading the registry
            // for; refresh_theme() decides whether anything actually moved.
            if (msg == WM_SETTINGCHANGE && theme::is_color_scheme_change(lparam) &&
                core->refresh_theme(true)) {
                KD_LOG_INFO("the system colour scheme changed; the pill is now %s",
                            core->painting_dark ? "dark" : "light");
            }
            core->dirty = true;
            core->render();
            return 0;

        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            core->hwnd = nullptr;
            return DefWindowProcW(hwnd, msg, wparam, lparam);

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

ATOM ensure_window_class(HINSTANCE instance) {
    // UI thread only, and a magic static is thread-safe anyway. Registered once per process:
    // RegisterClassExW fails with ERROR_CLASS_ALREADY_EXISTS on a second Overlay otherwise.
    static ATOM atom = [instance]() -> ATOM {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &overlay_wndproc;
        wc.hInstance = instance;
        // A click-through window never owns the cursor, and for most of a session this one is
        // click-through. But a composing session drops WS_EX_TRANSPARENT (docs/editing.md §5.1)
        // and the window then does own it, so a class cursor of nullptr would leave whatever
        // shape the pointer arrived wearing — including the busy shape it was wearing over
        // something else. An arrow is the floor; WM_SETCURSOR sharpens it per region.
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;  // see WM_ERASEBKGND
        wc.lpszClassName = kWindowClass;
        return RegisterClassExW(&wc);
    }();
    return atom;
}

}  // namespace

// ─── Overlay::Impl ──────────────────────────────────────────────────────────────────────────

struct Overlay::Impl : OverlayCore {};

Overlay::Overlay() : impl_(new Impl()) {}

Overlay::~Overlay() {
    destroy();
    delete impl_;
}

Status Overlay::create(uintptr_t instance, const OverlayStyle& style) {
    if (impl_->hwnd) {
        return Status::success();  // idempotent: re-creating would orphan the window
    }

    impl_->style = style;
    // Before the window exists, so the very first paint is already in the right scheme. A pill
    // that flashes white on a dark desktop for one frame is a pill that flashes white.
    impl_->refresh_theme(true);
    impl_->instance = instance ? reinterpret_cast<HINSTANCE>(instance) : GetModuleHandleW(nullptr);

    if (ensure_window_class(impl_->instance) == 0) {
        const DWORD err = GetLastError();
        return Status{StatusCode::OsError, "RegisterClassExW failed for the overlay class",
                      static_cast<uint32_t>(err)};
    }

    // ── The five extended styles, each load-bearing ─────────────────────────────────────────
    //
    //   WS_EX_LAYERED     — per-pixel alpha. Without it UpdateLayeredWindow fails outright and
    //                       there is no way to blend pending text over the target's pixels.
    //   WS_EX_TOPMOST     — without it the overlay falls behind the target the moment the target
    //                       repaints, and the preview is invisible exactly while it is needed.
    //   WS_EX_TOOLWINDOW  — without it the overlay gets a taskbar button and an Alt-Tab entry,
    //                       and Alt-Tab to it would hand it the focus §1.7 depends on the target
    //                       keeping.
    //   WS_EX_NOACTIVATE  — **the worst failure in the product if missing.** The overlay takes
    //                       focus, the target loses it, and every injected keystroke lands in
    //                       our own window (plan.md §1.3).
    //   WS_EX_TRANSPARENT — clicks pass through to the target. Without it the overlay eats the
    //                       click the user makes to correct the text it is previewing — and that
    //                       click is also the stop-on-input signal (plan.md §1.8).
    //
    // WS_POPUP, and never WS_VISIBLE: the first show must go through SW_SHOWNOACTIVATE.
    const DWORD ex_style = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                           WS_EX_TRANSPARENT;

    HWND hwnd = CreateWindowExW(ex_style, kWindowClass, L"kvit-dictate overlay", WS_POPUP, 0, 0, 1,
                                1, nullptr, nullptr, impl_->instance,
                                static_cast<OverlayCore*>(impl_));
    if (!hwnd) {
        const DWORD err = GetLastError();
        return Status{StatusCode::OsError, "CreateWindowExW failed for the overlay window",
                      static_cast<uint32_t>(err)};
    }
    impl_->hwnd = hwnd;

    // ── Capture exclusion (plan.md §1.3 / §1.5) ─────────────────────────────────────────────
    //
    // DWM excludes the window from every capture surface. **This is a correctness measure, not
    // a security one** — it is not a content-protection guarantee and must never be described as
    // one. What it buys is that anchor.cpp's BitBlt of the region around the insertion point
    // does not contain the pending text this window just drew there.
    //
    // Windows 10 2004+ only. On an older build the call fails (WDA_EXCLUDEFROMCAPTURE is not a
    // recognised affinity), and the degradation is specific and worth stating: the anchor
    // estimator's commit-differencing source (source C) sees our own glyphs in its after-frame
    // and scores them as the target's response to an injection. The anchor then tracks the
    // overlay instead of the caret, with high confidence, which is worse than having no source C
    // at all. The honest mitigation on such a build is to hide the overlay across each capture,
    // or to disable source C — both are the caller's decision, so this is reported rather than
    // silently worked around here.
    if (SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)) {
        impl_->capture_excluded = true;
    } else {
        impl_->capture_excluded = false;
        KD_LOG_WARN(
            "SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) failed (%lu); the overlay will "
            "appear in anchor captures and commit-differencing confidence is not trustworthy",
            GetLastError());
    }

    KD_LOG_INFO("overlay created (capture_excluded=%d, mode=%s)",
                impl_->capture_excluded ? 1 : 0, to_string(impl_->mode));
    return Status::success();
}

void Overlay::destroy() {
    if (!impl_) {
        return;
    }
    impl_->release_device_resources();
    impl_->release_text_formats();
    impl_->surface.reset();
    impl_->dwrite.reset();
    impl_->d2d.reset();
    if (impl_->hwnd) {
        HWND h = impl_->hwnd;
        impl_->hwnd = nullptr;  // WM_NCDESTROY also clears it; clearing first is idempotent
        DestroyWindow(h);
    }
    impl_->want_visible = false;
}

bool Overlay::is_created() const noexcept { return impl_ && impl_->hwnd != nullptr; }

uintptr_t Overlay::hwnd() const noexcept { return reinterpret_cast<uintptr_t>(impl_->hwnd); }

bool Overlay::capture_excluded() const noexcept { return impl_->capture_excluded; }

void Overlay::set_text(std::string_view pending, std::string_view unstable) {
    if (impl_->pending == pending && impl_->unstable == unstable) {
        return;  // render() is cheap when nothing changed; not marking dirty is cheaper
    }
    impl_->pending.assign(pending);
    impl_->unstable.assign(unstable);
    // The caret and the selection are offsets into the string that has just been replaced.
    // Leaving them alone would point them into text that is no longer there — the caller sets
    // them again immediately, but only when there is a compose buffer, and a session with none
    // must not inherit an offset from one that had.
    if (impl_->caret > impl_->pending.size()) impl_->caret = impl_->pending.size();
    if (impl_->sel_begin > impl_->pending.size()) impl_->sel_begin = impl_->pending.size();
    if (impl_->sel_end > impl_->pending.size()) impl_->sel_end = impl_->pending.size();
    impl_->dirty = true;
}

void Overlay::set_state(OverlayState s) {
    if (impl_->state == s) {
        return;
    }
    impl_->state = s;
    impl_->dirty = true;
}

void Overlay::set_mode(OverlayMode m) {
    if (impl_->mode == m) {
        return;
    }
    impl_->mode = m;
    impl_->dirty = true;
}

void Overlay::set_input_level(float level01) {
    const float clamped = std::clamp(level01, 0.0f, 1.0f);
    // Quantised before it is compared, so a level arriving at capture rate repaints the pill only
    // when the bar would actually move. Thirty-two steps is finer than the meter can show at any
    // pill height and coarse enough that microphone noise floor does not flicker it.
    const float stepped = std::round(clamped * 32.0f) / 32.0f;
    if (stepped == impl_->input_level) {
        return;
    }
    impl_->input_level = stepped;
    impl_->dirty = true;
}

void Overlay::set_transcriber_lag(uint64_t lag_ms) {
    // Only the crossing matters, not the number: the note says the same thing at 4 s and at 9 s,
    // and repainting a pill for a value it does not draw would undo set_input_level's care.
    const bool was = impl_->transcriber_lag_ms >= kLagNoteMs;
    const bool now = lag_ms >= kLagNoteMs;
    impl_->transcriber_lag_ms = lag_ms;
    if (was != now) impl_->dirty = true;
}

void Overlay::set_anchor(const std::optional<anchor::Anchor>& a) {
    impl_->anchor = a;
    impl_->dirty = true;
}

void Overlay::flag_violation(std::string_view reason) {
    impl_->violation = true;
    impl_->violation_reason.assign(reason);
    impl_->dirty = true;
    // The text itself is not logged here: util/log.h's rule is that recognised text never enters
    // the diagnostic channel. The reason string comes from commit_engine and is a description,
    // not a transcript, but its length is all this line needs.
    KD_LOG_WARN("overlay flagged a committed-prefix violation (%zu byte reason)", reason.size());
}

void Overlay::set_edit_view(bool active, size_t caret_off, size_t sel_b, size_t sel_e) {
    // Clamped against `pending` here rather than trusted, for the reason docs/editing.md §3 gives
    // the caret clamp in EditBuffer: the unstable tail is not addressable, and an offset that
    // reached past the settled region would draw a caret sitting in text the model is still
    // revising — which is the one place the user must be able to see they cannot edit.
    const size_t n = impl_->pending.size();
    const size_t c = std::min(caret_off, n);
    const size_t b = std::min(sel_b, n);
    const size_t e = std::min(sel_e, n);

    if (impl_->edit_active == active && impl_->caret == c && impl_->sel_begin == b &&
        impl_->sel_end == e) {
        return;
    }
    // A caret the user moved unpins the view, so that the next paint scrolls the caret into view
    // rather than staying at the bottom of a buffer the user has arrowed away from (§7.3). A
    // caret that merely followed appended speech does not — see caret_move_unpins_scroll().
    if (caret_move_unpins_scroll(active, impl_->caret != c)) impl_->scroll_pinned = false;
    impl_->edit_active = active;
    impl_->caret = c;
    impl_->sel_begin = b;
    impl_->sel_end = e;
    impl_->dirty = true;
}

void Overlay::set_hint(std::string_view h) {
    if (impl_->hint == h) {
        return;
    }
    impl_->hint.assign(h);
    impl_->dirty = true;
}

void Overlay::set_key_legend(std::string_view hotkey, bool composing, bool push_to_talk) {
    if (impl_->legend_hotkey == hotkey && impl_->legend_composing == composing &&
        impl_->legend_push_to_talk == push_to_talk) {
        return;
    }
    impl_->legend_hotkey.assign(hotkey);
    impl_->legend_composing = composing;
    impl_->legend_push_to_talk = push_to_talk;
    impl_->dirty = true;
}

void Overlay::clear_hint() {
    if (impl_->hint.empty()) {
        return;
    }
    impl_->hint.clear();
    impl_->dirty = true;
}

void Overlay::scroll_by_lines(int32_t delta) {
    if (delta == 0) {
        return;
    }
    // Clamped against the real line count on the next paint, which is the only place that knows
    // it. Here the offset is only ever moved and floored at zero; letting it run past the end
    // and be clamped later is what keeps this function free of a layout it would have to build.
    const int64_t next = static_cast<int64_t>(impl_->scroll_line) + delta;
    impl_->scroll_line = next <= 0 ? 0u : static_cast<uint32_t>(next);
    impl_->scroll_pinned = false;
    impl_->dirty = true;
}

void Overlay::scroll_to_bottom() {
    if (impl_->scroll_pinned) {
        return;
    }
    impl_->scroll_pinned = true;
    impl_->dirty = true;
}

void Overlay::set_click_through(bool value) {
    if (impl_->click_through == value) {
        return;
    }
    impl_->click_through = value;
    if (!impl_->hwnd) {
        return;
    }

    LONG_PTR ex = GetWindowLongPtrW(impl_->hwnd, GWL_EXSTYLE);
    const LONG_PTR was = ex;
    if (value) {
        ex |= WS_EX_TRANSPARENT;
    } else {
        ex &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
    }
    // WS_EX_NOACTIVATE is deliberately untouched. It is what keeps the target foreground when a
    // click lands here, which is what keeps `validate_target` passing, which is what keeps a
    // session's text going into the document instead of onto the clipboard (docs/editing.md
    // §4.6). Dropping WS_EX_TRANSPARENT without it would be the worst failure overlay.h names.
    if (ex == was) {
        return;
    }
    SetLastError(0);
    if (SetWindowLongPtrW(impl_->hwnd, GWL_EXSTYLE, ex) == 0 && GetLastError() != 0) {
        KD_LOG_WARN("could not %s the overlay's click-through flag (err %lu); the pill will not "
                    "be clickable this session",
                    value ? "restore" : "drop", GetLastError());
        impl_->click_through = !value;
        return;
    }

    // docs/editing.md §15 question 2 asked whether the change takes effect without this. The
    // documentation for SetWindowLongPtr says a style change is not applied to some styles until
    // SetWindowPos is called with SWP_FRAMECHANGED, and it does not enumerate which — so the call
    // is made rather than the question left open. It costs one syscall per session boundary, and
    // NOMOVE/NOSIZE/NOZORDER/NOACTIVATE mean it changes nothing else.
    SetWindowPos(impl_->hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                     SWP_FRAMECHANGED);
    KD_LOG_INFO("overlay is now %s", value ? "click-through" : "clickable");
}

bool Overlay::click_through() const noexcept { return impl_->click_through; }

anchor::Rect Overlay::pill_rect() const noexcept {
    // Empty while hidden. The hook reads an empty rectangle as "there is no pill", so a hidden
    // overlay cannot swallow a click that was meant for the document.
    if (!impl_->hwnd || !impl_->want_visible) {
        return anchor::Rect{};
    }
    return impl_->painted_rect;
}

bool Overlay::hit_test_text(int32_t x, int32_t y, size_t& offset) const {
    offset = 0;
    if (!impl_->hit_layout) {
        return false;
    }
    // Physical pixels in, DIPs to DirectWrite. The pill can be on a monitor at a different scale
    // from the one this process started on, so the divisor is the DPI the last paint used rather
    // than a constant — the same value that paint chose the layout's own metrics with.
    const float scale =
        static_cast<float>(impl_->hit_dpi) / static_cast<float>(USER_DEFAULT_SCREEN_DPI);
    if (scale <= 0.0f) {
        return false;
    }
    const float px = static_cast<float>(x) / scale - impl_->hit_origin_x;
    const float py = static_cast<float>(y) / scale - impl_->hit_origin_y;

    BOOL trailing = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS hm{};
    if (FAILED(impl_->hit_layout->HitTestPoint(px, py, &trailing, &inside, &hm))) {
        return false;
    }
    // `inside` false means the point was past the end of a line or below the last one, and
    // DirectWrite has answered with the nearest position anyway. That is the right answer for a
    // click: dragging past the end of a line selects to its end.
    size_t units = hm.textPosition + (trailing ? hm.length : 0);

    // Undo the front-elision, which shifted every position by the dropped units plus the one
    // ellipsis that replaced them. A click on the ellipsis itself lands at the start of what is
    // still shown, which is as far back as the caret can go.
    if (impl_->hit_cut_units > 0) {
        units = units == 0 ? 0 : units - 1;
        units += impl_->hit_cut_units;
    }

    offset = utf8_offset_for_utf16_units(impl_->pending, units);
    // docs/editing.md §3: the unstable tail is not addressable. A click on the model's
    // still-moving words puts the caret at the end of what is settled rather than inside what is
    // not — the clamp EditBuffer would apply anyway, applied here so the two cannot disagree.
    if (offset > impl_->pending.size()) offset = impl_->pending.size();
    return true;
}

bool Overlay::hit_test_mic_button(int32_t x, int32_t y) const {
    return impl_->point_in_mic(x, y);
}

void Overlay::clear_violation() {
    if (!impl_->violation) {
        return;
    }
    impl_->violation = false;
    impl_->violation_reason.clear();
    impl_->dirty = true;
}

void Overlay::show() {
    impl_->want_visible = true;
    impl_->dirty = true;
    // Paint before the window becomes visible: a layered window shows nothing until its first
    // UpdateLayeredWindow, so this ordering is what prevents an empty frame at the old position.
    impl_->render();
}

void Overlay::hide() {
    impl_->want_visible = false;
    impl_->apply_visibility();
}

void Overlay::render() {
    if (!impl_->hwnd) {
        return;
    }
    const bool should_show = impl_->want_visible && impl_->has_anything_to_show();
    if (!impl_->dirty && (IsWindowVisible(impl_->hwnd) != FALSE) == should_show) {
        return;  // nothing changed and the window's visibility already matches
    }
    impl_->render();
}

void Overlay::set_style(const OverlayStyle& s) {
    impl_->style = s;
    // The OS is not re-asked here: nothing about a settings change moves the *system* scheme,
    // and the cached answer is kept fresh by WM_SETTINGCHANGE. What has moved is which scheme
    // the user wants, so the palette is refolded either way.
    impl_->refresh_theme(false);
    // Font family/size live in the text formats and the colours live in the brushes, so both
    // caches are invalidated. The render target itself survives — it is device state, not style.
    impl_->release_text_formats();
    impl_->release_device_resources();
    impl_->dirty = true;
}

const OverlayStyle& Overlay::style() const noexcept { return impl_->style; }

// ─── layout_overlay — the pure, testable half ───────────────────────────────────────────────
//
// All arguments and the result are **physical pixels**, matching anchor.h.
//
// `text_width_px` / `text_height_px` are the overlay's outer size, padding included. The caller
// has already measured the text and added its own padding; this function does no padding
// arithmetic of its own, because padding is a DIP quantity and this function has no DPI.
//
// The gap between the overlay and the anchor line is derived from `text_height_px` rather than
// being a constant, so it scales with DPI without this function needing to know the DPI.

uint32_t clamp_scroll_line(uint32_t desired, uint32_t caret_line, bool caret_known,
                           uint32_t total_lines, uint32_t visible_lines) noexcept {
    if (visible_lines == 0 || total_lines <= visible_lines) {
        return 0;  // everything fits; there is nothing to scroll and no valid non-zero start
    }
    const uint32_t max_start = total_lines - visible_lines;
    uint32_t start = desired > max_start ? max_start : desired;

    // docs/editing.md §7.3: "any caret movement scrolls the caret into view". Applied *after*
    // the clamp above rather than before it, because it is the rule that overrides the requested
    // position and not the other way round — a caret above the view scrolls back to it even
    // though the user had scrolled forward, which is what makes the arrow keys able to reach the
    // top of a long buffer at all.
    if (caret_known) {
        if (caret_line < start) {
            start = caret_line;
        } else if (caret_line >= start + visible_lines) {
            start = caret_line - visible_lines + 1;
        }
    }

    return start > max_start ? max_start : start;
}

anchor::Rect layout_overlay(const anchor::Anchor& a, int32_t text_width_px, int32_t text_height_px,
                            const anchor::Rect& monitor_work_area, OverlayMode mode) {
    anchor::Rect out{};
    if (mode == OverlayMode::Hidden || text_width_px <= 0 || text_height_px <= 0) {
        return out;  // empty rect: nothing to place
    }

    const anchor::Rect& work = monitor_work_area;
    const int32_t gap = std::max<int32_t>(2, text_height_px / 6);

    // How far the screen-edge modes sit from the edge.
    //
    // This deliberately does NOT derive from `text_height_px` the way `gap` does. The inline gap
    // is a distance from a line of text, so scaling it with the overlay's own height is right.
    // The distance from the bottom of the screen is not: a status-only pill with no preview text
    // is about 19 px tall, so `gap * 2` placed it **6 px** above the bottom of a 3840x2160
    // monitor — inside the strip an auto-hide taskbar reveals itself in, and reported by the
    // first human to see it as "the pill is all the way down, mostly below screen".
    //
    // Deriving it from the work area keeps the DPI-independence the `gap` comment is really
    // after: the work area is in physical pixels, so a 150 % monitor has proportionally more of
    // them and the margin grows to match without this function knowing the DPI. 1/24 of the
    // height is ~90 px at 2160 and ~45 px at 1080. The `gap * 2` floor keeps the old behaviour
    // when the work area is unknown.
    const int32_t edge_margin =
        work.empty() ? gap * 2 : std::max<int32_t>(gap * 2, work.height() / 24);

    int32_t left = 0;
    int32_t top = 0;

    switch (mode) {
        case OverlayMode::Inline: {
            // RTL: anchor.h defines the insertion point as the LEFT edge of the changed region
            // for right-to-left text, so the box grows leftwards from it — mirroring the
            // direction the text itself will grow.
            left = a.rtl ? a.insertion_point.x - text_width_px : a.insertion_point.x;

            const int32_t line_height = std::max<int32_t>(0, a.line_height);
            const int32_t line_bottom = a.insertion_point.y + line_height;

            top = line_bottom + gap;
            if (!work.empty() && top + text_height_px > work.bottom) {
                // Flip above the line rather than off the bottom of the screen.
                const int32_t flipped = a.insertion_point.y - gap - text_height_px;
                if (flipped >= work.top) {
                    top = flipped;
                } else {
                    // No room either side of the line — a very short work area, or a very tall
                    // overlay. Clamping wins over disappearing; the final clamp below does it,
                    // and the overlay may then cover the line it is annotating. That is the
                    // documented worst case, not a silent one.
                    top = work.bottom - text_height_px;
                }
            }
            break;
        }

        case OverlayMode::FloatingPill: {
            // plan.md §1.5's deterministic degradation: position independent of the caret, so it
            // cannot flap as the estimate changes. Bottom-centre of the target's monitor.
            //
            // "Near the target window" is implemented as "on the target's monitor": this
            // function is given the monitor work area and no target rectangle, and inventing one
            // from the anchor would reintroduce exactly the caret dependence the pill exists to
            // remove.
            if (work.empty()) {
                left = a.insertion_point.x - text_width_px / 2;
                top = a.insertion_point.y;
            } else {
                left = work.left + (work.width() - text_width_px) / 2;
                top = work.bottom - text_height_px - edge_margin;
            }
            break;
        }

        case OverlayMode::StatusOnly: {
            // A corner indicator: out of the way of the document, and in the same place every
            // time so the eye learns where to find the microphone state (plan.md §1.6).
            if (work.empty()) {
                left = a.insertion_point.x;
                top = a.insertion_point.y;
            } else {
                left = work.right - text_width_px - edge_margin;
                top = work.bottom - text_height_px - edge_margin;
            }
            break;
        }

        case OverlayMode::Hidden:
        default:
            return out;
    }

    // Final clamp. Order matters: clamping to the right edge first and the left edge second
    // means a box wider than the monitor is left-aligned rather than pushed off-screen left.
    if (!work.empty()) {
        if (left + text_width_px > work.right) {
            left = work.right - text_width_px;
        }
        if (left < work.left) {
            left = work.left;
        }
        if (top + text_height_px > work.bottom) {
            top = work.bottom - text_height_px;
        }
        if (top < work.top) {
            top = work.top;
        }
    }

    out.left = left;
    out.top = top;
    out.right = left + text_width_px;
    out.bottom = top + text_height_px;
    return out;
}

}  // namespace kd
