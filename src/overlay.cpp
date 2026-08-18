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
//     is WS_EX_TRANSPARENT lost.
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
#include <utility>

#include "overlay.h"
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

constexpr uint32_t kDotIdle = 0xFF9E9E9E;
constexpr uint32_t kDotListening = 0xFF2E7D32;
constexpr uint32_t kDotSpeaking = 0xFF43A047;
constexpr uint32_t kDotFinalizing = 0xFF1565C0;
constexpr uint32_t kDotLoading = 0xFFB8860B;
constexpr uint32_t kDotUnsupported = 0xFF757575;
constexpr uint32_t kDotError = 0xFFC62828;

// Upper bound on the number of wrapped lines the preview may occupy. An overlay that grows with
// the utterance covers the document it is annotating; OverlayStyle::max_width_dip caps the
// width, this caps the height, and text beyond both is elided from the *front* because the
// newest words are the ones the user is checking.
constexpr uint32_t kMaxLines = 3;

// Starting budget, in UTF-16 code units, for the front-elision loop below.
constexpr size_t kInitialDisplayBudget = 512;

D2D1_COLOR_F argb(uint32_t c) noexcept {
    return D2D1::ColorF(c & 0x00FFFFFFu, static_cast<float>((c >> 24) & 0xFFu) / 255.0f);
}

uint32_t dot_color(OverlayState s, const OverlayStyle& style) noexcept {
    switch (s) {
        case OverlayState::Idle: return kDotIdle;
        case OverlayState::Listening: return kDotListening;
        case OverlayState::Speaking: return kDotSpeaking;
        case OverlayState::Finalizing: return kDotFinalizing;
        case OverlayState::ModelLoading: return kDotLoading;
        case OverlayState::Warning: return style.color_warning;
        case OverlayState::Unsupported: return kDotUnsupported;
        case OverlayState::Error: return kDotError;
    }
    return kDotIdle;
}

// Deliberately *not* to_string(OverlayState). to_string is the programmatic spelling used in
// logs; this is prose shown to a user, and the two must be free to diverge — "Unsupported"
// tells a developer which enumerator it is and tells a user nothing about why their dictation
// is not appearing.
const char* state_label(OverlayState s) noexcept {
    switch (s) {
        case OverlayState::Idle: return "";
        case OverlayState::Listening: return "Listening";
        case OverlayState::Speaking: return "Listening \xE2\x80\xA2";  // U+2022, speech in progress
        case OverlayState::Finalizing: return "Finishing\xE2\x80\xA6";
        case OverlayState::ModelLoading: return "Loading model\xE2\x80\xA6";
        case OverlayState::Warning: return "Correction could not be applied";
        case OverlayState::Unsupported: return "Dictation is not supported in this target";
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

const char* to_string(OverlayState s) noexcept {
    switch (s) {
        case OverlayState::Idle: return "Idle";
        case OverlayState::Listening: return "Listening";
        case OverlayState::Speaking: return "Speaking";
        case OverlayState::Finalizing: return "Finalizing";
        case OverlayState::ModelLoading: return "ModelLoading";
        case OverlayState::Warning: return "Warning";
        case OverlayState::Unsupported: return "Unsupported";
        case OverlayState::Error: return "Error";
    }
    return "Idle";
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

    OverlayStyle style{};
    OverlayMode mode = OverlayMode::Inline;
    OverlayState state = OverlayState::Idle;

    std::string pending;
    std::string unstable;
    std::optional<anchor::Anchor> anchor;

    bool violation = false;
    std::string violation_reason;

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

    // ── Device resources ────────────────────────────────────────────────────────────────────

    void release_device_resources() noexcept {
        brush_dot.reset();
        brush_border.reset();
        brush_warning.reset();
        brush_background.reset();
        brush_unstable.reset();
        brush_pending.reset();
        rt.reset();
    }

    void release_text_formats() noexcept {
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
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 0.0f, 0.0f,
        D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);

    HRESULT hr = d2d->CreateDCRenderTarget(&props, rt.put());
    if (FAILED(hr)) {
        // **Uncertain Windows behaviour, handled rather than assumed.** A DC render target is
        // documented to fall back to software, but the hardware path's handling of the alpha
        // channel when the bound DC is a DIB section is not something I can verify from the
        // documentation alone. If item 3 of the manual procedure ever shows an opaque black
        // box instead of a translucent pill, force _SOFTWARE unconditionally here — that is the
        // known workaround, and it costs nothing at this frame rate.
        props.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
        hr = d2d->CreateDCRenderTarget(&props, rt.put());
        if (FAILED(hr)) {
            return hr;
        }
        KD_LOG_WARN("D2D DC render target fell back to software (hr=0x%08lX)",
                    static_cast<unsigned long>(hr));
    }

    struct BrushSpec {
        ComRef<ID2D1SolidColorBrush>* slot;
        uint32_t color;
    };
    const BrushSpec specs[] = {
        {&brush_pending, style.color_pending},   {&brush_unstable, style.color_unstable},
        {&brush_background, style.color_background}, {&brush_warning, style.color_warning},
        {&brush_border, style.color_border},     {&brush_dot, dot_color(state, style)},
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
    if (violation) {
        // plan.md §1.4: a visible, persistent mark naming which utterance is suspect — not a
        // toast that disappears before the user has finished reading their own document.
        note = utf8::to_utf16_or_empty("\xE2\x9A\xA0 ");  // U+26A0 WARNING SIGN
        note.append(wide_or_log(violation_reason, "violation reason"));
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
    if (!body.empty()) {
        // Front-elision loop: shrink the budget until the preview fits kMaxLines. Bounded to a
        // handful of iterations over a few hundred characters — this runs on the UI thread and
        // "never block" (overlay.h) rules out anything that could iterate over a whole session.
        size_t budget = kInitialDisplayBudget;
        for (int attempt = 0; attempt < 6; ++attempt) {
            std::wstring shown = elide_front(body, budget, cut);
            shown_len = shown.size();
            hr = dwrite->CreateTextLayout(shown.c_str(), static_cast<UINT32>(shown.size()),
                                          format_body.get(), max_text_w, 4096.0f,
                                          body_layout.put());
            if (FAILED(hr)) {
                return hr;
            }
            hr = body_layout->GetMetrics(&body_metrics);
            if (FAILED(hr)) {
                return hr;
            }
            if (body_metrics.lineCount <= kMaxLines || budget <= 24) {
                break;
            }
            budget = budget * 2 / 3;
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
    const float body_h = body_layout ? body_metrics.height : 0.0f;
    const float note_h = note_layout ? note_metrics.height : 0.0f;

    const float content_w = std::max(body_w, note_w);
    const float content_h = body_h + (note_layout ? (body_layout ? gap : 0.0f) + note_h : 0.0f);

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
        brush_dot->SetColor(argb(dot_color(state, style)));
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

    const D2D1_POINT_2F dot_centre = D2D1::Point2F(pad + dot_r, pad + dot_r + 1.0f);
    rt->FillEllipse(D2D1::Ellipse(dot_centre, dot_r, dot_r), brush_dot.get());

    if (body_layout) {
        rt->DrawTextLayout(D2D1::Point2F(text_x, pad), body_layout.get(), brush_pending.get(),
                           D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
    if (note_layout) {
        rt->DrawTextLayout(D2D1::Point2F(text_x, pad + body_h + (body_layout ? gap : 0.0f)),
                           note_layout.get(),
                           violation ? brush_warning.get() : brush_unstable.get(),
                           D2D1_DRAW_TEXT_OPTIONS_NONE);
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
            // Same reasoning: WS_EX_TRANSPARENT already routes clicks past us, and this makes
            // the window transparent to WindowFromPoint-style probing as well.
            return HTTRANSPARENT;

        case WM_ERASEBKGND:
            // A layered window's pixels come from UpdateLayeredWindow. Erasing would be a
            // wasted GDI round trip on the message-loop thread.
            return 1;

        case WM_PAINT:
            // Layered windows painted with UpdateLayeredWindow do not paint through WM_PAINT.
            // Validate so the message is not re-posted forever if one ever arrives.
            ValidateRect(hwnd, nullptr);
            return 0;

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
            // Work areas move when a taskbar does; a stale work area puts the overlay under it.
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
        wc.hCursor = nullptr;  // a click-through window never owns the cursor
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
        return Status::ok();  // idempotent: re-creating would orphan the window
    }

    impl_->style = style;
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
    return Status::ok();
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

anchor::Rect layout_overlay(const anchor::Anchor& a, int32_t text_width_px, int32_t text_height_px,
                            const anchor::Rect& monitor_work_area, OverlayMode mode) {
    anchor::Rect out{};
    if (mode == OverlayMode::Hidden || text_width_px <= 0 || text_height_px <= 0) {
        return out;  // empty rect: nothing to place
    }

    const anchor::Rect& work = monitor_work_area;
    const int32_t gap = std::max<int32_t>(2, text_height_px / 6);

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
                top = work.bottom - text_height_px - gap * 2;
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
                left = work.right - text_width_px - gap * 2;
                top = work.bottom - text_height_px - gap * 2;
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
