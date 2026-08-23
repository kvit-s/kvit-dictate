// Suite `tray_icon` — the notification-area icon that is drawn rather than shipped.
//
// This is a drawing routine, and the parts of drawing that matter to a *user* — is it a
// recognisable microphone, is it balanced at 16 px — are not testable and are checked by looking
// at it. What is testable is the part that carries meaning, and it is the part that would fail
// silently:
//
//   the microphone is filled when the device is open and hollow when it is closed. That is
//   plan.md §1.6's "am I being listened to must never require inference", drawn — and if a
//   future edit inverts it, or flattens both states to the same shape, nothing else in the
//   program notices. The pixels are counted here for exactly that reason.
//
// The ink is checked the same way: an icon drawn in the taskbar's own colour on the taskbar's
// own background is an invisible icon, and "the tray icon disappeared after I switched to dark
// mode" is a bug report nobody can reproduce on the machine that shipped it.
//
// These call Win32 and GDI and produce a real HICON. They need a window station — they run under
// the same conditions the rest of the suite does, on the machine the product targets.

#include "kd_test.h"

#include "tray_icon.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <vector>

namespace {

using kd::OverlayState;

struct IconPixels {
    int size = 0;
    std::vector<uint32_t> bgra;  // premultiplied, top-down

    [[nodiscard]] bool ok() const { return size > 0 && bgra.size() == static_cast<size_t>(size) * size; }

    // How many pixels the glyph actually covers, at all. Alpha rather than colour, so it counts
    // ink regardless of which state's colour was used.
    [[nodiscard]] size_t inked(uint8_t min_alpha = 40) const {
        size_t n = 0;
        for (uint32_t px : bgra) {
            if (((px >> 24) & 0xFFu) >= min_alpha) ++n;
        }
        return n;
    }

    [[nodiscard]] uint32_t at(int x, int y) const {
        return bgra[static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)];
    }
};

// Read an HICON's colour bitmap back out. GetIconInfo hands back copies of both bitmaps, which
// this owns and must delete — the leak is silent and this suite draws a lot of icons.
IconPixels read_icon(uintptr_t handle) {
    IconPixels out;
    HICON icon = reinterpret_cast<HICON>(handle);
    if (icon == nullptr) return out;

    ICONINFO info{};
    if (!GetIconInfo(icon, &info)) return out;

    BITMAP bm{};
    if (GetObjectW(info.hbmColor, sizeof(bm), &bm) != 0 && bm.bmWidth > 0) {
        out.size = bm.bmWidth;
        out.bgra.resize(static_cast<size_t>(bm.bmWidth) * static_cast<size_t>(bm.bmHeight));
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = bm.bmWidth;
        bi.bmiHeader.biHeight = -bm.bmHeight;  // top-down, matching how it was drawn
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        if (HDC dc = CreateCompatibleDC(nullptr)) {
            if (GetDIBits(dc, info.hbmColor, 0, static_cast<UINT>(bm.bmHeight), out.bgra.data(),
                          &bi, DIB_RGB_COLORS) == 0) {
                out.size = 0;
                out.bgra.clear();
            }
            DeleteDC(dc);
        }
    }
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    return out;
}

// Draw one, read it back, destroy it.
IconPixels draw(OverlayState state, bool dark, int size = 32) {
    const uintptr_t handle = kd::make_tray_icon(state, dark, size);
    IconPixels px = read_icon(handle);
    if (handle != 0) DestroyIcon(reinterpret_cast<HICON>(handle));
    return px;
}

}  // namespace

KD_TEST(tray_icon, an_icon_is_produced_at_every_size_the_shell_asks_for) {
    // 16 at 100 %, 20 at 125 %, 24 at 150 %, 32 at 200 %. The shell picks; this must not have an
    // opinion about which of them is the real one.
    for (int size : {16, 20, 24, 32, 48}) {
        const IconPixels px = draw(OverlayState::Idle, true, size);
        KD_CHECK(px.ok());
        KD_CHECK_EQ(px.size, size);
        // Some ink, and not a solid block: a glyph, not a filled square or an empty bitmap.
        const size_t inked = px.inked();
        KD_CHECK(inked > static_cast<size_t>(size));
        KD_CHECK(inked < static_cast<size_t>(size) * static_cast<size_t>(size) / 2);
    }
}

KD_TEST(tray_icon, the_microphone_is_filled_only_while_the_device_is_open) {
    // The one thing this icon must never get wrong. Listening and Speaking hold the device open;
    // every other state has closed it — Finalizing has already sent `stop`, and Editing is
    // docs/editing.md §7.4's "the microphone is CLOSED".
    // Asked of the capsule's *interior*, not of the icon's total ink. Total ink does not answer
    // it: at the stroke width this glyph is drawn with, the outline of the capsule covers nearly
    // as many pixels as filling it does, so a count over the whole icon separates the two states
    // by a few per cent — which is a test that passes today and would keep passing if the fill
    // were removed altogether. One pixel in the middle of the capsule separates them completely.
    const int size = 32;
    const int cx = size / 2;
    const int cy = static_cast<int>(size * 0.33f);  // inside the capsule, clear of its outline
    auto centre_alpha = [&](OverlayState s) {
        const IconPixels px = draw(s, true, size);
        return px.ok() ? static_cast<uint8_t>((px.at(cx, cy) >> 24) & 0xFFu) : uint8_t{0};
    };

    KD_CHECK(centre_alpha(OverlayState::Listening) > 200);
    KD_CHECK(centre_alpha(OverlayState::Speaking) > 200);

    for (OverlayState closed : {OverlayState::Idle, OverlayState::Finalizing,
                                OverlayState::ModelLoading, OverlayState::Editing,
                                OverlayState::Unsupported, OverlayState::Error,
                                OverlayState::Warning}) {
        // Hollow: the taskbar shows through the middle of the microphone.
        KD_CHECK(centre_alpha(closed) < 40);
        // And still a microphone — a state that drew nothing at all would also pass the line
        // above, and would be an application that vanished from the tray.
        KD_CHECK(draw(closed, true, size).inked() > static_cast<size_t>(size));
    }
}

KD_TEST(tray_icon, the_idle_glyph_inverts_with_the_taskbar) {
    // An idle icon is drawn in the taskbar's own ink — near-black on a light taskbar, near-white
    // on a dark one — and getting this backwards is invisible to whoever made the change and
    // total for the user who has the other setting.
    const IconPixels light = draw(OverlayState::Idle, false);
    const IconPixels dark = draw(OverlayState::Idle, true);
    KD_REQUIRE(light.ok());
    KD_REQUIRE(dark.ok());

    // Brightest ink in each, over the pixels the glyph actually covers. Premultiplied, so a
    // partly transparent edge pixel is darker than the glyph itself — the maximum is the glyph.
    auto brightest = [](const IconPixels& px) {
        uint32_t best = 0;
        for (uint32_t p : px.bgra) {
            if (((p >> 24) & 0xFFu) < 200) continue;  // edges, not the body
            const uint32_t g = (p >> 8) & 0xFFu;
            if (g > best) best = g;
        }
        return best;
    };
    KD_CHECK(brightest(dark) > 200);
    KD_CHECK(brightest(light) < 80);
}

KD_TEST(tray_icon, the_tray_and_the_pill_agree_about_what_a_state_looks_like) {
    // Both read overlay.h's state_dot_color, and the point of this test is that they keep doing
    // so: docs/editing.md §16 records what it costs when the pill renders a state the tray maps
    // onto a different one, and a colour is that same disagreement in a form nobody greps for.
    //
    // Error is the case worth pinning: it is red in both, and red is the only thing in the tray
    // that means "your dictation stopped and you were not told".
    const IconPixels px = draw(OverlayState::Error, true);
    KD_REQUIRE(px.ok());
    const uint32_t expected = kd::state_dot_color(OverlayState::Error, true);

    bool found = false;
    for (uint32_t p : px.bgra) {
        if (((p >> 24) & 0xFFu) < 250) continue;  // fully opaque body pixels only
        if ((p & 0x00FFFFFFu) == (expected & 0x00FFFFFFu)) {
            found = true;
            break;
        }
    }
    KD_CHECK(found);
}

KD_TEST(tray_icon, an_absurd_size_is_clamped_rather_than_refused) {
    // The shell asks; this answers. A zero or a negative from a DPI query that failed must not
    // produce a null icon, because the caller's fallback for null is the stock application icon
    // — which is the icon this whole file exists to replace.
    KD_CHECK(draw(OverlayState::Idle, true, 0).ok());
    KD_CHECK(draw(OverlayState::Idle, true, -8).ok());
    KD_CHECK(draw(OverlayState::Idle, true, 4096).ok());
}

