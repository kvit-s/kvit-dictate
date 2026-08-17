#pragma once
//
// Global hotkey and stop-on-input: WH_KEYBOARD_LL + WH_MOUSE_LL, plus the pure input filter
// that decides what to swallow, what to pass, and what counts as "the user did something else".
//
// Implements: plan.md §1.8 in full, and the source-B half of plan.md §1.5 (the last left-click
// before the hotkey is cached here, because the mouse hook is already installed).
//
// ─── Why hooks and not RegisterHotKey ───────────────────────────────────────────────────────
//
// The default is **Win+Y with toggle semantics**, chosen to match the Win+H gesture the product
// replaces. `RegisterHotKey` refuses shell-owned combos, so it cannot bind Win+Y at all. A
// WH_KEYBOARD_LL hook on a dedicated thread sees keys before the shell's hotkey processing, and
// returning 1 without calling CallNextHookEx swallows the combo. (Ctrl+Alt+Del and Win+L are
// immune to this and always will be.)
//
// Win+Y is nominally the Mixed Reality toggle, effectively dead on a desktop without a headset.
//
// ─── The rule that governs this entire file ─────────────────────────────────────────────────
//
//   > **The callback must never block.** A slow hook is silently unhooked by Windows and the
//   > hotkey dies with no error.
//
// No logging to a file, no allocation, no lock that another thread can hold, no window
// creation, no cross-process call, and no call into SessionMachine — inside the hook callback.
// What the callback does: run InputFilter (pure, allocation-free, bounded), decide swallow or
// pass, and PostMessage the decision to the owner. Everything else happens elsewhere.
//
// ─── Start-menu suppression, and why it is quarantined ──────────────────────────────────────
//
// Swallowing the Y leaves Windows seeing a bare Win press, which pops Start on release. The fix
// is to inject a harmless VK_CONTROL tap before the Win key-up. **This is a hack against
// undocumented shell behaviour and a plausible casualty of a Windows update**, so it lives
// behind one function (`suppress_start_menu()`), has its own test, and every non-Win hotkey
// alternative stays one click away in settings (F13–F24 need no hack at all).
//
// ─── Activation must not immediately self-stop ──────────────────────────────────────────────
//
// The chord's own key-up events, and the injected suppression tap, arrive *after* the session
// starts and would otherwise trigger stop-on-input. The rule, stated explicitly because it is
// the kind of thing that becomes an ad-hoc filter otherwise:
//
//   * ignore all key-up events for keys that were already down at activation;
//   * ignore anything flagged LLKHF_INJECTED / LLMHF_INJECTED;
//   * ignore anything carrying our own injection mark (injector.h's InjectionMark) — our own
//     SendInput traffic must never stop the session producing it.
//
// InputFilter below is that rule as a **pure, unit-tested state machine** (plan.md §1.8 and §7),
// not a collection of conditions inside the hook callback.
//
// ─── Threading ──────────────────────────────────────────────────────────────────────────────
//
//   * `HotkeyManager::start()` spawns a **dedicated hook thread** with its own message loop.
//     WH_KEYBOARD_LL and WH_MOUSE_LL require the installing thread to pump messages, and using
//     the UI thread would couple hook latency to overlay painting.
//   * Hook callbacks run on that thread. They call InputFilter and PostMessage; nothing else.
//   * `IHotkeyListener` methods are invoked on the **owner's UI thread**, after the post.
//   * `InputFilter` is pure, has no thread affinity, and is the only part covered by tests.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util/result.h"

namespace kd {

// ─── Virtual-key codes, spelled out ─────────────────────────────────────────────────────────
//
// No header under src/ includes <windows.h>, and that rule is what keeps InputFilter testable
// without a UI. But the filter reasons about specific keys, and so do its tests. So the handful
// of VK_* values this module needs are written out here, and hotkey.cpp static_asserts every one
// of them against WinUser.h — if Microsoft ever renumbered a code, the build breaks rather than
// the hotkey silently binding to the wrong key.
namespace vk {

inline constexpr uint32_t kBack = 0x08;
inline constexpr uint32_t kTab = 0x09;
inline constexpr uint32_t kReturn = 0x0D;
inline constexpr uint32_t kShift = 0x10;    // the "either side" codes; a low-level hook normally
inline constexpr uint32_t kControl = 0x11;  // reports the sided codes below instead, but injected
inline constexpr uint32_t kMenu = 0x12;     // input (including our own Ctrl tap) uses these.
inline constexpr uint32_t kCapsLock = 0x14;
inline constexpr uint32_t kEscape = 0x1B;
inline constexpr uint32_t kSpace = 0x20;
inline constexpr uint32_t kLeft = 0x25;
inline constexpr uint32_t kUp = 0x26;
inline constexpr uint32_t kRight = 0x27;
inline constexpr uint32_t kDown = 0x28;
inline constexpr uint32_t k0 = 0x30;
inline constexpr uint32_t k9 = 0x39;
inline constexpr uint32_t kA = 0x41;
inline constexpr uint32_t kJ = 0x4A;
inline constexpr uint32_t kO = 0x4F;
inline constexpr uint32_t kY = 0x59;
inline constexpr uint32_t kZ = 0x5A;
inline constexpr uint32_t kLWin = 0x5B;
inline constexpr uint32_t kRWin = 0x5C;
inline constexpr uint32_t kF1 = 0x70;
inline constexpr uint32_t kF13 = 0x7C;
inline constexpr uint32_t kF24 = 0x87;
inline constexpr uint32_t kLShift = 0xA0;
inline constexpr uint32_t kRShift = 0xA1;
inline constexpr uint32_t kLControl = 0xA2;
inline constexpr uint32_t kRControl = 0xA3;
inline constexpr uint32_t kLMenu = 0xA4;
inline constexpr uint32_t kRMenu = 0xA5;

inline constexpr uint32_t kCount = 256;  // the width of InputFilter's key bitsets

}  // namespace vk

// ─── Hotkey specification ───────────────────────────────────────────────────────────────────

// Modifier bitmask. Not the RegisterHotKey MOD_* values — those are a different API with
// different semantics, and reusing their numbering would invite someone to pass this to
// RegisterHotKey, which cannot bind the default combo.
enum ModMask : uint32_t {
    kModNone = 0,
    kModCtrl = 1u << 0,
    kModAlt = 1u << 1,
    kModShift = 1u << 2,
    kModWin = 1u << 3,
};

struct HotkeySpec {
    uint32_t vk = 0x59;             // VK_Y
    uint32_t mods = kModWin;        // Win+Y, the default (plan.md §1.8)
    bool require_left_win = false;  // false = either Win key

    // The settings menu offers, per plan.md §1.8: Win+Y (default) · Win+J · Win+O ·
    // F13–F24 (free, no Win-key hack) · Ctrl+Win+Space · right Ctrl / right Alt / CapsLock.
    [[nodiscard]] bool needs_start_menu_suppression() const noexcept { return (mods & kModWin) != 0; }

    [[nodiscard]] std::string to_display_string() const;  // "Win+Y"
    static bool parse(std::string_view s, HotkeySpec& out);
};

// The presets settings offers, as data rather than as a switch statement in the dialog.
std::vector<std::pair<std::string, HotkeySpec>> hotkey_presets();

// ─── Reasons a session ends from input ──────────────────────────────────────────────────────

enum class StopInputReason : uint8_t {
    Keystroke,
    MouseClick,
    HotkeyToggle,  // the second press: finalise (commit the stable prefix, discard the tail
                   // after a final decode)
    Escape,        // cancel: discard everything uncommitted
};

const char* to_string(StopInputReason r) noexcept;

// ─── The pure filter (plan.md §1.8, unit-tested) ────────────────────────────────────────────

// One low-level input event, reduced to the fields the decision depends on. Deliberately not a
// KBDLLHOOKSTRUCT: keeping the filter free of Windows types is what lets the state machine be
// tested exhaustively, including sequences that are awkward to produce with a real keyboard.
struct InputEvent {
    enum class Kind : uint8_t { KeyDown, KeyUp, MouseDown, MouseUp, MouseWheel, MouseMove };

    Kind kind = Kind::KeyDown;
    uint32_t vk = 0;              // key events only
    bool injected = false;        // LLKHF_INJECTED / LLMHF_INJECTED
    uintptr_t extra_info = 0;     // dwExtraInfo; compared against our InjectionMark
    bool left_button = false;     // mouse events: left button specifically (plan.md §1.5 —
                                  // right-click opens a context menu, middle-click pastes on X11)
    int32_t x = 0, y = 0;         // mouse position, physical pixels
    uint64_t time_ms = 0;
};

// What the hook must do with the event, and what the session should be told.
struct FilterOutcome {
    bool swallow = false;         // return 1 without CallNextHookEx

    bool activate = false;        // start a session
    bool toggle_finalize = false; // second hotkey press
    bool cancel = false;          // Esc

    bool stop_on_input = false;
    StopInputReason stop_reason = StopInputReason::Keystroke;

    bool click_captured = false;  // a left click worth caching as anchor source B
    bool click_invalidated = false;  // wheel: scrolling moves the caret with no click
    bool suppress_start_menu = false;  // inject the VK_CONTROL tap before the Win key-up
};

class InputFilter {
public:
    InputFilter();

    void set_hotkey(const HotkeySpec& spec);
    [[nodiscard]] const HotkeySpec& hotkey() const noexcept;

    // Tell the filter which extra_info value marks our own synthetic input.
    void set_injection_mark(uintptr_t extra_info) noexcept;

    // The session started; remember which keys were down so their key-ups are ignored. This is
    // the concrete form of "ignore key-up events for keys already down at activation".
    void note_session_started(uint64_t now_ms);
    void note_session_ended();

    // Pure decision. No allocation, no syscall, bounded work — safe to call from the hook.
    FilterOutcome on_event(const InputEvent& e);

    // Test/inspection accessors.
    [[nodiscard]] bool session_active() const noexcept;
    [[nodiscard]] bool is_key_down(uint32_t vk) const noexcept;
    [[nodiscard]] size_t keys_held_at_activation() const noexcept;

private:
    // Split by event class so that each rule is readable in isolation. All three are pure and
    // allocation-free; none of them may grow a call into anything that can block.
    FilterOutcome on_key_event(const InputEvent& e, bool down);
    FilterOutcome on_mouse_event(const InputEvent& e);

    // Modifier bitmask implied by the keys currently down. `ignore_vk` is excluded, which is what
    // lets a modifier key itself be the binding (right Ctrl, right Alt): otherwise pressing the
    // bound key would put kModCtrl into the current mask and never match a spec of kModNone.
    [[nodiscard]] uint32_t current_mods(uint32_t ignore_vk) const noexcept;
    [[nodiscard]] bool chord_matches(const InputEvent& e) const noexcept;

    // Snapshot the keys currently down. Called the instant the chord is recognised — NOT when
    // the owner later confirms via note_session_started(). See the comment in hotkey.cpp: the
    // chord's key-ups reach the hook long before the posted activation reaches the UI thread.
    void begin_session_state(uint64_t now_ms) noexcept;

    // Fixed-size state: a bitset over the 256 virtual-key codes, twice (currently down, and
    // down-at-activation). No heap, so the hook path cannot allocate.
    static constexpr size_t kVkWords = vk::kCount / 64;
    uint64_t down_[kVkWords]{};
    uint64_t down_at_activation_[kVkWords]{};
    HotkeySpec spec_{};
    uintptr_t injection_mark_ = 0;
    bool session_active_ = false;
    uint64_t session_started_ms_ = 0;
    bool hotkey_chord_consumed_ = false;  // one activation per physical chord press
};

// ─── The Windows-facing manager ─────────────────────────────────────────────────────────────

class IHotkeyListener {
public:
    virtual ~IHotkeyListener() = default;

    // All of these arrive on the owner's UI thread, never on the hook thread.
    virtual void on_activate() = 0;
    virtual void on_finalize() = 0;
    virtual void on_cancel() = 0;
    virtual void on_stop_input(StopInputReason reason) = 0;

    // Anchor source B (plan.md §1.5). `window` is the HWND under the click, as an opaque value.
    virtual void on_left_click(int32_t x, int32_t y, uintptr_t window, uint64_t time_ms) = 0;

    // Wheel invalidates the cached click: scrolling moves the caret with no click.
    virtual void on_wheel() = 0;

    // The hook was removed by Windows or failed to install. Surfaced rather than swallowed,
    // because the symptom otherwise is "the hotkey stopped working" with no explanation.
    virtual void on_hook_lost() = 0;
};

// ─── The hook thread → UI thread post ───────────────────────────────────────────────────────
//
// The hook callback is forbidden from calling a listener (that would run arbitrary UI-thread work
// on the hook thread, which is exactly how a hook gets slow enough for Windows to remove it). It
// PostMessages instead, and the owner's window procedure hands the message back through
// dispatch_hotkey_message(), which is where IHotkeyListener is finally called — on the UI thread.
//
// The value is WM_APP + 0x11. WM_APP is 0x8000; it is written out rather than included because
// this header must stay free of <windows.h>. hotkey.cpp static_asserts it against the real macro.
inline constexpr uint32_t kMsgHotkey = 0x8000u + 0x11u;

// wParam of kMsgHotkey. lParam carries the StopInputReason for StopInput and is 0 otherwise —
// the click coordinates do not fit in a message parameter and are read from the manager's
// single-slot cache instead (see hotkey.cpp).
enum class HotkeyPost : uint32_t {
    Activate,
    Finalize,
    Cancel,
    StopInput,
    LeftClick,
    Wheel,
    HookLost,
};

// Call from the owner window's WndProc. Returns true if the message belonged to this module,
// in which case the listener has already been invoked and the WndProc should return 0.
bool dispatch_hotkey_message(uint32_t msg, uintptr_t wparam, intptr_t lparam);

class HotkeyManager {
public:
    HotkeyManager();
    ~HotkeyManager();
    HotkeyManager(const HotkeyManager&) = delete;
    HotkeyManager& operator=(const HotkeyManager&) = delete;

    // Installs both hooks on a dedicated thread. `owner_window` receives the posted decisions;
    // pass the hidden message-only window that main.cpp owns.
    Status start(const HotkeySpec& spec, uintptr_t owner_window, IHotkeyListener* listener);
    void stop();

    // Change the binding without tearing the hooks down.
    void set_hotkey(const HotkeySpec& spec);

    // The dwExtraInfo value that marks this process's own synthetic input, so the filter can
    // ignore it (plan.md §1.8: "Our own SendInput traffic must never stop the session producing
    // it"). The value belongs to injector.h's InjectionMark; the owner wires the two together
    // with `mgr.set_injection_mark(injection_mark().extra_info)` rather than this module reaching
    // into the injector — the hook must not depend on the thing it is filtering out.
    void set_injection_mark(uintptr_t extra_info);

    // Tell the filter about the session lifecycle. Called from the UI thread.
    void note_session_started();
    void note_session_ended();

    [[nodiscard]] bool is_running() const noexcept;

    // Hooks are a documented antivirus trigger (plan.md §6 risk table: "this looks like a
    // keylogger to heuristics"), and Windows can remove a hook that was too slow. This counter
    // is surfaced in diagnostics so a silent removal is visible rather than mysterious.
    struct Stats {
        uint64_t key_events = 0;
        uint64_t mouse_events = 0;
        uint64_t swallowed = 0;
        uint64_t hook_reinstalls = 0;
        uint64_t max_callback_us = 0;  // if this climbs, the hook is at risk of being removed
    };
    [[nodiscard]] Stats stats() const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

// The Start-menu suppression hack, quarantined in one function with one test.
//
// Injects a VK_CONTROL down/up pair so that the shell does not see a bare Win press-and-release
// when we swallowed the second key of the chord. It is undocumented behaviour and may break
// with any Windows update; when it does, the symptom is the Start menu appearing on every
// activation, and the answer is to switch to an F13–F24 binding rather than to escalate the
// hack. Returns false if the injection call itself failed.
//
// `injection_mark` is stamped on the synthetic events as dwExtraInfo so the filter recognises
// them as ours. Passing 0 still works — the events also carry LLKHF_INJECTED, which the filter
// rejects on its own — but the mark is the stronger of the two signals and should be supplied.
bool suppress_start_menu(uintptr_t injection_mark = 0);

}  // namespace kd
