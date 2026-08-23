#pragma once
//
// The notification-area icon: a microphone, drawn at run time rather than shipped as a resource.
//
// ─── Why it is drawn and not a .ico ─────────────────────────────────────────────────────────
//
// The icon has to answer three questions at once, and each one multiplies the number of image
// files a resource-based icon would need:
//
//   which state?      Nine of them (overlay.h's OverlayState), and the one that matters is
//                     whether the microphone is open — plan.md §1.6 is explicit that "am I being
//                     listened to" must never require inference, and the tray is where a user
//                     checks it when no pill is on screen.
//   which scheme?     The notification area follows SystemUsesLightTheme, so the glyph is dark
//                     on a light taskbar and light on a dark one. Ship one bitmap and it is
//                     invisible on half of desktops.
//   which size?       GetSystemMetricsForDpi(SM_CXSMICON) — 16 px at 100 %, 24 at 150 %, 32 at
//                     200 %. A bitmap scaled by the shell to a size it was not authored at is
//                     the blurry tray icon everyone recognises.
//
// That is nine states × two schemes × four sizes as artwork, against forty lines of geometry
// here. It also keeps the repository free of binary assets and the build free of a resource
// script, which CMakeLists.txt says in as many words is a change nobody has needed to make yet.
//
// **This is the tray icon only.** The executable's own icon — the one Explorer, Alt-Tab and the
// taskbar button show — is a resource and this does not provide it; the process has no visible
// top-level window of its own, so nothing here is affected by that.
//
// ─── The glyph ──────────────────────────────────────────────────────────────────────────────
//
// A capsule on a cradle: the microphone every recording application draws, because it is
// recognisable at 16 px where a more literal one is a smudge. The one piece of meaning carried
// by the shape rather than the colour is the fill:
//
//     the capsule is FILLED when the microphone is open, and hollow when it is closed.
//
// Colour is the second channel, from overlay.h's `state_dot_color`, so the tray and the pill's
// state dot are the same colour for the same state — see that function on why they must be.
// Colour alone would not do: a user who cannot distinguish the green from the grey still has to
// be able to tell an open microphone from a closed one, and that is what the fill is for.

#include <cstdint>

#include "overlay.h"

namespace kd {

// Draw the tray icon for `state`.
//
// Returns an HICON as an opaque value, or 0 if it could not be drawn — the caller falls back to
// a stock icon rather than showing no icon at all. **The caller owns it and must DestroyIcon
// it**, and must not do so until the shell has been told about the icon that replaces it.
//
// `size_px` is the icon's edge in physical pixels; pass 0 for the system's small-icon size at
// the system DPI, which is what the notification area asks for.
[[nodiscard]] uintptr_t make_tray_icon(OverlayState state, bool dark_taskbar, int size_px = 0);

}  // namespace kd
