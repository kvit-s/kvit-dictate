#pragma once
//
// The system colour scheme, and the Win32 plumbing that makes a window follow it.
//
// Windows has no supported, complete "dark mode" for a plain Win32 window. The title bar is a DWM
// attribute, the common controls are opted in one at a time by *theme class name*, the dialog
// face and every label on it are the application's own job through WM_CTLCOLOR*, and the switch
// that makes those theme class names mean anything at all lives behind unnamed uxtheme ordinals.
// This file is where that mess is contained, so that overlay.cpp and settings.cpp each ask one
// question — "is the system dark right now?" — and make one call — "paint this window that way".
//
// ─── What here is documented, and what is not ───────────────────────────────────────────────
//
//   Documented: the AppsUseLightTheme registry value; WM_SETTINGCHANGE carrying
//               "ImmersiveColorSet"; DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE);
//               SetWindowTheme with the DarkMode_* class names.
//
//   NOT:        uxtheme.dll ordinals 133/135/136 — AllowDarkModeForWindow, SetPreferredAppMode,
//               FlushMenuThemes. They are what every dark-mode Win32 application uses because
//               Microsoft ships no alternative, and they are resolved **by ordinal at runtime
//               and allowed to be absent**. When they are absent the degradation is specific
//               rather than fatal: the colours this file hands out still reach every surface the
//               application paints itself, so the dialog face, its labels and the pill are dark;
//               only the pieces Windows draws for us — a push button's face, a scroll bar — stay
//               light. That is worth having. A light dialog on a dark desktop is not.
//
// Nothing here is a security boundary and nothing here fails the caller: every function is safe
// to call on a build that supports none of it, and the worst outcome is that the window looks
// exactly as it did before this file existed.
//
// **UI thread only**, like both callers.

#include <cstdint>

namespace kd::theme {

// Has the user asked for dark application windows?
//
// Read from HKCU\...\Themes\Personalize\AppsUseLightTheme, which is the value the shell's own
// "Choose your default app mode" writes and the only one that tracks *apps* rather than the
// taskbar. A missing value means light — that is what a fresh profile and a Windows build older
// than 1809 both look like, and both should be light.
//
// Read on demand rather than cached: it is one registry open per colour-scheme change, and a
// cache would need an invalidation path that is exactly this call.
[[nodiscard]] bool system_prefers_dark();

// Is the *taskbar* dark? A different question with a different answer, and the tray icon is
// drawn from this one.
//
// Windows keeps two switches next to each other: AppsUseLightTheme, which is what a window
// should follow, and SystemUsesLightTheme, which is what the taskbar, the Start menu and the
// notification area follow. They are set independently, and the combination that catches a
// program reading only the first is the common one — light apps on a dark taskbar — where a
// tray icon drawn dark to match the settings dialog becomes a black smudge on black.
[[nodiscard]] bool taskbar_prefers_dark();

// Is this WM_SETTINGCHANGE the one that means "the colour scheme moved"?
//
// `lparam` is the message's LPARAM, as an intptr_t so this header stays Windows-free. It may be
// null (many senders pass no section name), which is not a colour-scheme change.
[[nodiscard]] bool is_color_scheme_change(intptr_t lparam) noexcept;

// Opt the *process* into dark mode. Idempotent, and must be called before the windows that will
// use it are created — SetPreferredAppMode decides whether the DarkMode_* theme classes below
// resolve to anything at all, and a control themed before the process was opted in stays light.
//
// Also flushes the menu themes, which is what makes the tray's popup menu dark; menus are drawn
// by the system from a cache that is only rebuilt on that call.
void init_app_dark_mode();

// Paint one window and its children in `dark` (or back out of it).
//
// Does three things, none of which subsumes the others: the title bar (DWM), the window's own
// theme class (uxtheme, so a scroll bar in it is dark), and every child control's theme class,
// chosen by window class — edits and combo boxes want "DarkMode_CFD", buttons and everything
// else "DarkMode_Explorer". Children created *after* this call are not covered; call it again,
// or call apply_control_dark_mode on the new control.
void apply_window_dark_mode(uintptr_t hwnd, bool dark);

// One control, by the same rules. Exposed because the settings dialog creates its controls in
// one pass and themes them in another, and because a control created later must not be missed.
void apply_control_dark_mode(uintptr_t hwnd, bool dark);

// ─── The dialog's palette ───────────────────────────────────────────────────────────────────
//
// COLORREFs — 0x00BBGGRR, the byte order GDI wants, **not** the 0xAARRGGBB that OverlayStyle and
// the settings file use. The two orders are one byte-swap apart and mixing them up produces a
// window that is merely wrongly coloured rather than broken, so the distinction is stated here
// rather than left to be discovered.
//
// Only the dark half is ever consulted: in light mode the settings dialog takes the same path it
// always took, using the system's own COLOR_BTNFACE brush and the system's own control colours.
// That is deliberate. Light mode is the mode this application shipped with, and a hand-written
// "light" palette here could only differ from the system's — never improve on it.
struct DialogPalette {
    uint32_t face;          // the dialog's background, and what its labels sit on
    uint32_t text;          // label and static text
    uint32_t field;         // the inside of an edit, combo or list box
    uint32_t field_text;
    uint32_t tab_face;      // an unselected tab
    uint32_t tab_selected;  // the selected one, which must read as raised, not as a hole

    // Push buttons. They are drawn by hand for the same reason the tab strip is: there is no
    // dark theme class that takes for a BS_PUSHBUTTON, so an unthemed button draws its 3D frame
    // in COLOR_3DHIGHLIGHT (white) and its label in COLOR_BTNTEXT (black) whatever the parent
    // says through WM_CTLCOLORBTN — a dark face in a white box with unreadable text.
    uint32_t button;
    uint32_t button_hot;      // under the pointer
    uint32_t button_pressed;
    uint32_t button_border;
    uint32_t button_default;  // the fill of the default button, which must be findable by eye
    uint32_t disabled_text;
};

[[nodiscard]] const DialogPalette& dialog_palette(bool dark) noexcept;

}  // namespace kd::theme
