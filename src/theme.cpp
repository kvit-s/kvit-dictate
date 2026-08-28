// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// The system colour scheme, and the Win32 plumbing that makes a window follow it.
// See theme.h for what here is documented Windows behaviour and what is not.

#define KD_LOG_MODULE "theme"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <cwchar>

#include "theme.h"

#include "util/log.h"

namespace kd::theme {
namespace {

// DWMWA_USE_IMMERSIVE_DARK_MODE. Two numbers, because Microsoft changed it: 19 on Windows 10
// 1809-1903, 20 from 2004 onward, and older SDK headers define neither. Both are attempted and
// both are allowed to fail — DwmSetWindowAttribute returns an error for an attribute the running
// DWM does not know, which is exactly the "this build has no dark title bar" case.
constexpr DWORD kImmersiveDarkModeOld = 19;
constexpr DWORD kImmersiveDarkMode = 20;

// uxtheme.dll's unnamed ordinals. See theme.h: undocumented, resolved once, allowed to be null.
enum class PreferredAppMode { Default = 0, AllowDark = 1, ForceDark = 2, ForceLight = 3 };

using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND, BOOL);
using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
using FlushMenuThemesFn = void(WINAPI*)();

struct UxThemeEntries {
    AllowDarkModeForWindowFn allow_dark_mode_for_window = nullptr;
    SetPreferredAppModeFn set_preferred_app_mode = nullptr;
    FlushMenuThemesFn flush_menu_themes = nullptr;
};

const UxThemeEntries& ux() {
    // Resolved once, on the UI thread that first asks, and never freed — uxtheme.dll is loaded
    // for the life of the process anyway (SetWindowTheme lives in it and is linked normally), so
    // there is nothing here to leak.
    static const UxThemeEntries entries = [] {
        UxThemeEntries e{};
        HMODULE m = GetModuleHandleW(L"uxtheme.dll");
        if (m == nullptr) {
            m = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        }
        if (m == nullptr) {
            KD_LOG_WARN("uxtheme.dll is not present; dark mode will be colours only");
            return e;
        }
        // MAKEINTRESOURCEA on an ordinal, which is how these are exported: they have no names.
        // A build that predates them returns null for each, which is a supported outcome and not
        // an error — see theme.h for exactly what is lost.
        e.allow_dark_mode_for_window = reinterpret_cast<AllowDarkModeForWindowFn>(
            reinterpret_cast<void*>(GetProcAddress(m, MAKEINTRESOURCEA(133))));
        e.set_preferred_app_mode = reinterpret_cast<SetPreferredAppModeFn>(
            reinterpret_cast<void*>(GetProcAddress(m, MAKEINTRESOURCEA(135))));
        e.flush_menu_themes = reinterpret_cast<FlushMenuThemesFn>(
            reinterpret_cast<void*>(GetProcAddress(m, MAKEINTRESOURCEA(136))));
        return e;
    }();
    return entries;
}

// Which dark theme class does this control want?
//
// "DarkMode_CFD" — the common file dialog's — is the only one that darkens a text field's
// *border* as well as its interior, so edits and combo boxes take it. "DarkMode_Explorer" is
// right for everything else: buttons, check boxes, list boxes, scroll bars.
const wchar_t* dark_theme_class_for(HWND hwnd) {
    wchar_t cls[64]{};
    if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) == 0) return L"DarkMode_Explorer";
    if (_wcsicmp(cls, L"Edit") == 0 || _wcsicmp(cls, L"ComboBox") == 0) {
        return L"DarkMode_CFD";
    }
    return L"DarkMode_Explorer";
}

BOOL CALLBACK theme_child(HWND child, LPARAM param) {
    apply_control_dark_mode(reinterpret_cast<uintptr_t>(child), param != 0);
    return TRUE;
}

}  // namespace

namespace {

// The two Personalize switches, read the same way. `value_name` is AppsUseLightTheme or
// SystemUsesLightTheme; both are "1 means light", so a missing key, a missing value and a value
// of the wrong type all answer light — which is what a fresh profile and a build older than 1809
// both look like, and both should be light.
bool personalize_says_dark(const wchar_t* value_name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD value = 1;  // the values are named *Light*Theme, so 1 — light — is the safe default
    DWORD size = sizeof(value);
    DWORD type = 0;
    const LSTATUS st = RegQueryValueExW(key, value_name, nullptr, &type,
                                        reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS || type != REG_DWORD) return false;
    return value == 0;
}

}  // namespace

bool system_prefers_dark() { return personalize_says_dark(L"AppsUseLightTheme"); }

bool taskbar_prefers_dark() { return personalize_says_dark(L"SystemUsesLightTheme"); }

bool is_color_scheme_change(intptr_t lparam) noexcept {
    const wchar_t* section = reinterpret_cast<const wchar_t*>(lparam);
    // Null is common: WM_SETTINGCHANGE is broadcast for actions that name no section, and a
    // taskbar moving is one of them. Not a colour-scheme change.
    return section != nullptr && wcscmp(section, L"ImmersiveColorSet") == 0;
}

void init_app_dark_mode() {
    static bool done = false;
    if (done) return;
    done = true;

    const UxThemeEntries& e = ux();
    if (e.set_preferred_app_mode == nullptr) {
        KD_LOG_INFO("uxtheme's app-mode switch is absent here; dark mode will be colours only");
        return;
    }
    // AllowDark, not ForceDark: the process follows the system, and the system's answer is read
    // per window through system_prefers_dark(). ForceDark would paint dark controls on a light
    // desktop the moment the user switched back.
    e.set_preferred_app_mode(PreferredAppMode::AllowDark);
    if (e.flush_menu_themes != nullptr) {
        // Menus are drawn by the system from a cache built at first use. Without this the tray's
        // popup menu stays light for the life of the process, and it is the one surface here that
        // cannot be repainted by hand.
        e.flush_menu_themes();
    }
}

void apply_control_dark_mode(uintptr_t hwnd, bool dark) {
    HWND h = reinterpret_cast<HWND>(hwnd);
    if (h == nullptr) return;
    if (ux().allow_dark_mode_for_window != nullptr) {
        ux().allow_dark_mode_for_window(h, dark ? TRUE : FALSE);
    }
    // "Explorer" rather than the documented nullptr on the way back to light: nullptr means "no
    // subclass override", which on some builds leaves the dark class in place. Naming the light
    // class is what actually undoes this when the user switches back mid-session.
    SetWindowTheme(h, dark ? dark_theme_class_for(h) : L"Explorer", nullptr);
}

void apply_window_dark_mode(uintptr_t hwnd, bool dark) {
    HWND h = reinterpret_cast<HWND>(hwnd);
    if (h == nullptr) return;

    apply_control_dark_mode(hwnd, dark);

    const BOOL on = dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(h, kImmersiveDarkMode, &on, sizeof(on)))) {
        // The older numbering. Both failing means this build has no dark title bar; the client
        // area below it is still ours to paint, so nothing is reported up.
        DwmSetWindowAttribute(h, kImmersiveDarkModeOld, &on, sizeof(on));
    }

    // The title bar does not repaint just because the attribute changed — it is drawn by DWM
    // from the frame, and the frame is only reconsidered when the window says it moved. Without
    // this, switching scheme with the window open leaves a dark caption over a light dialog
    // until the next time it is activated.
    SetWindowPos(h, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    EnumChildWindows(h, theme_child, dark ? 1 : 0);
}

const DialogPalette& dialog_palette(bool dark) noexcept {
    // COLORREF: 0x00BBGGRR. See theme.h — this is the byte order GDI wants, and the reverse of
    // the settings file's #AARRGGBB.
    static constexpr DialogPalette kLight{
        0x00F0F0F0,  // face — COLOR_BTNFACE's usual value; present only so the struct is total
        0x00000000, 0x00FFFFFF, 0x00000000, 0x00F0F0F0, 0x00FFFFFF,
        0x00F0F0F0, 0x00F0F0F0, 0x00F0F0F0, 0x00A0A0A0, 0x00F0F0F0, 0x006D6D6D,
    };
    static constexpr DialogPalette kDark{
        0x00202020,  // face   — the shell's own dark window background (#202020)
        0x00E8E8E8,  // text   — not pure white: white on #202020 is what makes dark UI vibrate
        0x002B2B2B,  // field  — a shade lighter than the face, so an edit reads as a well
        0x00E8E8E8,  // field text
        0x00272727,  // tab face
        0x00323232,  // tab selected — lighter, because a selected tab reads as nearer
        // The button ramp. Three steps in the same direction — a button lightens as the pointer
        // arrives and darkens as it goes down, which is the convention every dark shell uses and
        // the only feedback an owner-drawn button has left once its 3D frame is gone.
        0x002D2D2D,  // button
        0x00383838,  // hot
        0x00262626,  // pressed
        0x00454545,  // border  — a step above the face, not a highlight; a white line here is
                     //           exactly the defect this palette exists to remove
        0x00383838,  // default — the fill Enter would press, a step up from its neighbours
        0x007A7A7A,  // disabled text
    };
    return dark ? kDark : kLight;
}

}  // namespace kd::theme
