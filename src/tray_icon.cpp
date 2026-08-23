// The notification-area icon, drawn with Direct2D into a DIB. See tray_icon.h for why it is
// drawn rather than shipped, and for what the shape and the fill mean.
//
// Direct2D and not GDI because a microphone at 16 px is all curves, and GDI does not
// anti-alias: the capsule and the cradle come out as staircases at exactly the size the icon is
// actually seen at. The technique — a DC render target bound to a 32-bit top-down DIB section —
// is the one overlay.cpp already uses for the pill, so it costs no new dependency; d2d1 is
// already linked for the overlay.

#define KD_LOG_MODULE "tray"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d2d1.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "tray_icon.h"

#include "util/log.h"

namespace kd {
namespace {

// Minimal COM handle. overlay.cpp has one of these too; it is four lines and not worth a shared
// header that both would then have to include for this alone.
template <typename T>
struct ComRef {
    T* p = nullptr;
    ~ComRef() { reset(); }
    ComRef() = default;
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;
    T** put() { reset(); return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
    void reset() {
        if (p) { p->Release(); p = nullptr; }
    }
};

D2D1_COLOR_F argb(uint32_t c) noexcept {
    return D2D1::ColorF(c & 0x00FFFFFFu, static_cast<float>((c >> 24) & 0xFFu) / 255.0f);
}

// Is the microphone open in this state?
//
// The single most important thing the icon says, and the reason it is a *shape* difference
// rather than only a colour one. plan.md §1.6: the microphone stays open across long pauses, so
// "am I being listened to" must never require inference — and docs/editing.md §3.2 makes the
// closed device's appearance a privacy promise rather than a nicety.
//
// Finalizing is closed: the `stop` has gone and the device is released while the last words are
// transcribed. Editing is closed, emphatically — that is the whole of docs/editing.md §7.4.
bool microphone_is_open(OverlayState s) noexcept {
    return s == OverlayState::Listening || s == OverlayState::Speaking;
}

// The ink for `state` on a taskbar of this scheme.
//
// Everything except Idle comes from overlay.h's shared table, so the tray and the pill's dot
// agree. Idle does not, and the divergence is deliberate: the pill has no idle appearance to
// disagree with — it is hidden in that state — and a tray icon that sits there in table-grey all
// day reads as a disabled application rather than a ready one. So idle is the taskbar's own ink,
// which is what every other icon beside it is drawn in.
uint32_t ink_for(OverlayState state, bool dark_taskbar) noexcept {
    if (state == OverlayState::Idle) {
        return dark_taskbar ? 0xFFF2F2F2u : 0xFF1A1A1Au;
    }
    return state_dot_color(state, dark_taskbar);
}

// A 32-bit top-down DIB and the memory DC it is selected into, released together.
struct IconSurface {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    void* bits = nullptr;

    ~IconSurface() {
        if (dc && previous) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }

    bool create(int size) {
        dc = CreateCompatibleDC(nullptr);
        if (dc == nullptr) return false;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = size;
        bi.bmiHeader.biHeight = -size;  // negative: top-down, which is what D2D writes
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap == nullptr) return false;
        previous = SelectObject(dc, bitmap);
        return true;
    }
};

// Draw the microphone into `rt`, in a coordinate space that is `size` units on a side.
//
// Every number below is a fraction of that edge, so the shape is identical at 16 px and at 32.
// They were chosen against the 16 px rendering, which is the one that is actually hard: a stem
// half a pixel wide disappears, and a capsule any wider than this touches the cradle.
void draw_microphone(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, float size, bool filled) {
    // A whole number of pixels, and never zero. Direct2D will happily stroke 1.36 px, and at 16
    // px — the size this icon is actually seen at — that is a line spread over two pixels at
    // half strength each, which is the difference between a microphone and a grey smudge.
    const float stroke = std::max(1.0f, std::round(size * 0.085f));
    // Where the *centre* of such a stroke has to sit for its edges to land on pixel boundaries:
    // on a half-pixel for an odd width, on a whole one for an even width. This is the whole of
    // what a font renderer calls hinting, and at these sizes it is worth more than the geometry.
    const float grid = (static_cast<int>(stroke) % 2 == 1) ? 0.5f : 0.0f;
    const auto snap = [grid](float v) { return std::floor(v) + grid; };

    // The capsule — the microphone itself. Filled when the device is open (tray_icon.h).
    const D2D1_ROUNDED_RECT capsule = D2D1::RoundedRect(
        D2D1::RectF(snap(size * 0.34f), snap(size * 0.12f), snap(size * 0.66f),
                    snap(size * 0.60f)),
        size * 0.16f, size * 0.16f);
    if (filled) {
        rt->FillRoundedRectangle(capsule, brush);
    } else {
        // Stroked on the snapped rectangle itself, *not* inset by half a stroke to keep the
        // outer edges flush with the filled version. Insetting is the tidier geometry and it
        // costs the whole point of the snapping above: it moves the stroke's centre off the
        // half-pixel it was just put on, and the capsule's two straight sides come back at half
        // intensity — grey where the rest of the glyph is white. The half-pixel of size the
        // outline gains over the fill is not visible; a grey microphone is.
        rt->DrawRoundedRectangle(capsule, brush, stroke);
    }

    // The cradle: the lower half of a circle the capsule hangs inside. Drawn as an arc rather
    // than a clipped ellipse so the ends stop where they should.
    ComRef<ID2D1Factory> factory;
    rt->GetFactory(factory.put());
    ComRef<ID2D1PathGeometry> cradle;
    if (factory && SUCCEEDED(factory->CreatePathGeometry(cradle.put()))) {
        ComRef<ID2D1GeometrySink> sink;
        if (SUCCEEDED(cradle->Open(sink.put()))) {
            const float cradle_y = snap(size * 0.52f);
            const float cradle_r = std::round(size * 0.26f);
            sink->BeginFigure(D2D1::Point2F(snap(size * 0.5f) - cradle_r, cradle_y),
                              D2D1_FIGURE_BEGIN_HOLLOW);
            // Counter-clockwise, because Y grows downward: clockwise from the left-hand end
            // would arc over the *top* of the capsule instead of under it.
            sink->AddArc(D2D1::ArcSegment(
                D2D1::Point2F(snap(size * 0.5f) + cradle_r, cradle_y),
                D2D1::SizeF(cradle_r, cradle_r), 0.0f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                D2D1_ARC_SIZE_SMALL));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            if (SUCCEEDED(sink->Close())) {
                rt->DrawGeometry(cradle.p, brush, stroke);
            }
        }
    }

    // The stand: the stem down from the cradle, and the foot it rests on.
    const float stem_x = snap(size * 0.5f);
    const float foot_y = snap(size * 0.90f);
    rt->DrawLine(D2D1::Point2F(stem_x, snap(size * 0.76f)), D2D1::Point2F(stem_x, foot_y), brush,
                 stroke);
    rt->DrawLine(D2D1::Point2F(snap(size * 0.34f), foot_y), D2D1::Point2F(snap(size * 0.66f), foot_y),
                 brush, stroke);
}

}  // namespace

uintptr_t make_tray_icon(OverlayState state, bool dark_taskbar, int size_px) {
    if (size_px <= 0) {
        // What the notification area asks for at this DPI. GetSystemMetricsForDpi rather than
        // GetSystemMetrics: the latter answers for 96 DPI whatever the taskbar is actually
        // running at, and a 16 px icon on a 150 % taskbar is the blurry one.
        const UINT dpi = GetDpiForSystem();
        size_px = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    }
    size_px = std::clamp(size_px, 8, 256);

    IconSurface surface;
    if (!surface.create(size_px)) {
        KD_LOG_WARN("tray icon: could not create a %dpx DIB (err=%lu)", size_px, GetLastError());
        return 0;
    }

    // A factory per call. This runs when the state changes — a handful of times in a session,
    // never in a loop — so the alternative, a cached factory living for the life of the process
    // with the thread-affinity rules that come with it, would buy nothing measurable.
    ComRef<ID2D1Factory> factory;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.put()))) {
        return 0;
    }

    // Premultiplied BGRA, which is both what D2D writes for this alpha mode and what
    // CreateIconIndirect expects of a 32-bit colour bitmap. Get this wrong and the icon appears
    // with a black box behind it on one taskbar scheme and looks fine on the other.
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 0.0f, 0.0f,
        D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
    ComRef<ID2D1DCRenderTarget> rt;
    if (FAILED(factory->CreateDCRenderTarget(&props, rt.put()))) {
        props.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
        if (FAILED(factory->CreateDCRenderTarget(&props, rt.put()))) return 0;
    }

    const RECT bind{0, 0, size_px, size_px};
    if (FAILED(rt->BindDC(surface.dc, &bind))) return 0;

    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0.0f));  // fully transparent: the taskbar shows through
    ComRef<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(rt->CreateSolidColorBrush(argb(ink_for(state, dark_taskbar)), brush.put()))) {
        draw_microphone(rt.p, brush.p, static_cast<float>(size_px), microphone_is_open(state));
    }
    if (FAILED(rt->EndDraw())) return 0;
    GdiFlush();  // the DIB bits are written through the DC; nothing below may run before they are

    // The AND mask. Ignored for a 32-bit colour bitmap, which carries its own alpha — but
    // CreateIconIndirect still requires one, and it must be *zeroed*: CreateBitmap with a null
    // pointer does not promise zeroed bits, and a mask of ones is an invisible icon.
    const size_t mask_stride = static_cast<size_t>((size_px + 15) / 16) * 2;
    std::vector<uint8_t> mask_bits(mask_stride * static_cast<size_t>(size_px), 0);
    HBITMAP mask = CreateBitmap(size_px, size_px, 1, 1, mask_bits.data());
    if (mask == nullptr) return 0;

    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmMask = mask;
    info.hbmColor = surface.bitmap;
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(mask);
    if (icon == nullptr) {
        KD_LOG_WARN("tray icon: CreateIconIndirect failed (err=%lu)", GetLastError());
        return 0;
    }
    return reinterpret_cast<uintptr_t>(icon);
}

}  // namespace kd
