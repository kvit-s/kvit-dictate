// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
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

#include "edit_buffer.h"
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
inline constexpr uint32_t kPageUp = 0x21;
inline constexpr uint32_t kPageDown = 0x22;
inline constexpr uint32_t kEnd = 0x23;
inline constexpr uint32_t kHome = 0x24;
inline constexpr uint32_t kLeft = 0x25;
inline constexpr uint32_t kUp = 0x26;
inline constexpr uint32_t kRight = 0x27;
inline constexpr uint32_t kDown = 0x28;
inline constexpr uint32_t kPrintScreen = 0x2C;  // VK_SNAPSHOT
inline constexpr uint32_t kInsert = 0x2D;
inline constexpr uint32_t kDelete = 0x2E;
inline constexpr uint32_t k0 = 0x30;
inline constexpr uint32_t k9 = 0x39;
inline constexpr uint32_t kA = 0x41;
inline constexpr uint32_t kC = 0x43;
inline constexpr uint32_t kJ = 0x4A;
inline constexpr uint32_t kO = 0x4F;
inline constexpr uint32_t kV = 0x56;
inline constexpr uint32_t kX = 0x58;
inline constexpr uint32_t kY = 0x59;
inline constexpr uint32_t kZ = 0x5A;
inline constexpr uint32_t kLWin = 0x5B;
inline constexpr uint32_t kRWin = 0x5C;
inline constexpr uint32_t kNumpad0 = 0x60;
inline constexpr uint32_t kDivide = 0x6F;   // the last of the numeric-keypad block
inline constexpr uint32_t kF1 = 0x70;
inline constexpr uint32_t kF13 = 0x7C;
inline constexpr uint32_t kF24 = 0x87;
inline constexpr uint32_t kLShift = 0xA0;
inline constexpr uint32_t kRShift = 0xA1;
inline constexpr uint32_t kLControl = 0xA2;
inline constexpr uint32_t kRControl = 0xA3;
inline constexpr uint32_t kLMenu = 0xA4;
inline constexpr uint32_t kRMenu = 0xA5;
inline constexpr uint32_t kNumLock = 0x90;
inline constexpr uint32_t kScrollLock = 0x91;
// The keys that change the machine rather than the document (docs/editing.md §4.3). None of them
// can put a character anywhere, so the pill never swallows one — turning the volume down
// mid-dictation should simply work.
inline constexpr uint32_t kVolumeMute = 0xAD;
inline constexpr uint32_t kVolumeDown = 0xAE;
inline constexpr uint32_t kVolumeUp = 0xAF;
inline constexpr uint32_t kMediaNextTrack = 0xB0;
inline constexpr uint32_t kMediaPrevTrack = 0xB1;
inline constexpr uint32_t kMediaStop = 0xB2;
inline constexpr uint32_t kMediaPlayPause = 0xB3;
// The OEM punctuation blocks, which carry a printable character on every layout but whose
// meaning is layout-dependent — which is exactly why the *character* is resolved on the UI
// thread and only the class is decided here (docs/editing.md §4.2).
inline constexpr uint32_t kOem1 = 0xBA;
inline constexpr uint32_t kOem3 = 0xC0;
inline constexpr uint32_t kOem4 = 0xDB;
inline constexpr uint32_t kOem8 = 0xDF;
inline constexpr uint32_t kOem102 = 0xE2;

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

    // ─── Push-to-talk ───────────────────────────────────────────────────────────────────────
    //
    // Toggle (false, the default) is plan.md §1.8: press to start, press again to finalise.
    // Push-to-talk (true) is the walkie-talkie gesture: **hold** the binding to dictate, and
    // the release finalises — the same finalise the second press produces, so the text lands in
    // the target application exactly as it does today. Nothing about transcription changes; it
    // still runs live while the key is held. Only the gesture that ends the session moves, from
    // a second press to the release of the first.
    //
    // It lives on the spec rather than beside it because the filter needs to know both facts at
    // the same instant, and `set_hotkey()` is already the one channel that carries the binding
    // to the hook thread. A second setter would be a second thing to forget to call.
    //
    // Which bindings suit it: any of them, but the ones worth holding for a sentence are the
    // ones that are not also a modifier you are using for something else — F13–F24, right Ctrl,
    // right Alt or CapsLock. Win+Y works and is the default for continuity, not because holding
    // the Win key for ten seconds is comfortable.
    bool push_to_talk = false;

    // The settings menu offers, per plan.md §1.8: Win+Y (default) · Win+J · Win+O ·
    // F13–F24 (free, no Win-key hack) · Ctrl+Win+Space · right Ctrl / right Alt / CapsLock.
    [[nodiscard]] bool needs_start_menu_suppression() const noexcept { return (mods & kModWin) != 0; }

    // The display string names the KEYS only ("Win+Y"). Push-to-talk is a separate settings key,
    // deliberately: `parse()` has to accept everything `to_display_string()` produces, and the
    // moment the spelling carries a mode the two stop being inverses of each other.
    [[nodiscard]] std::string to_display_string() const;  // "Win+Y"
    static bool parse(std::string_view s, HotkeySpec& out);
};

// The presets settings offers, as data rather than as a switch statement in the dialog.
std::vector<std::pair<std::string, HotkeySpec>> hotkey_presets();

// ─── The compose buffer's configuration (docs/editing.md §12) ───────────────────────────────

// What happens to a key the pill claims but has no meaning for — Ctrl+S, F5, Win+E.
//
// Deliberately an enum and not a boolean, because the two values name two different things that
// happen and `capture_all_keys: true` would have left the reader to guess which
// (docs/editing.md §12).
enum class ForeignKeyAction : uint8_t {
    Ignore,      // swallowed, nothing happens, the pill shows a one-line hint (§4.4, the default)
    EndSession,  // today's behaviour: the key passes through and stop-on-input ends the session
};

const char* to_string(ForeignKeyAction a) noexcept;
bool parse_foreign_key_action(std::string_view s, ForeignKeyAction& out) noexcept;

struct EditingConfig {
    // Forced off unless `stability.cadence` is `end-of-session` — docs/editing.md §6.1: under
    // Cadence::Immediate the text is already in the document as it is spoken and there is
    // nothing in the pill to edit. Settings::normalise() is where that is applied, so nothing
    // downstream has to re-derive it.
    bool enabled = true;
    bool mouse = true;
    ForeignKeyAction on_foreign_key = ForeignKeyAction::Ignore;

    // Only Enter is offered, but it is a value rather than a constant so that the settings block
    // has somewhere to land and so the tests can prove the classification is driven by
    // configuration rather than by a VK buried in a switch.
    uint32_t commit_vk = vk::kReturn;
};

// ─── Reasons a session ends from input ──────────────────────────────────────────────────────

enum class StopInputReason : uint8_t {
    Keystroke,
    MouseClick,
    HotkeyToggle,  // the second press: finalise (commit the stable prefix, discard the tail
                   // after a final decode)
    Escape,        // cancel: discard everything uncommitted
    Commit,        // Enter: docs/editing.md §4.3. Swallowed, never replayed to the target.
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
    uint32_t scan_code = 0;       // key events only; carried so the UI thread can ask the
                                  // keyboard layout what character this key produces
                                  // (docs/editing.md §4.2 — the hook may not ask)
    bool caps_lock = false;       // read at the keystroke, not when the character is resolved
    bool injected = false;        // LLKHF_INJECTED / LLMHF_INJECTED
    uintptr_t extra_info = 0;     // dwExtraInfo; compared against our InjectionMark
    bool left_button = false;     // mouse events: left button specifically (plan.md §1.5 —
                                  // right-click opens a context menu, middle-click pastes on X11)
    int32_t wheel_delta = 0;      // WM_MOUSEWHEEL's signed delta; positive is away from the user
    int32_t x = 0, y = 0;         // mouse position, physical pixels
    uint64_t time_ms = 0;
};

// What the hook must do with the event, and what the session should be told.
struct FilterOutcome {
    bool swallow = false;         // return 1 without CallNextHookEx

    bool activate = false;        // start a session
    bool toggle_finalize = false; // second hotkey press
    bool cancel = false;          // Esc

    // docs/editing.md §4.3's Commit class: Enter, unmodified, while a session is running.
    //
    // This is **not** stop-on-input wearing a different name, and the difference is the whole
    // point of the feature's first stage. Stop-on-input observes a key and lets it through, so
    // the target received the Enter *before* the session's text was injected and the dictated
    // words landed on the line below where they belonged. Commit swallows the key: it means
    // "finish and type what you have", nothing else, and the target never sees it. Press Enter
    // again once the session is over and it reaches the application normally.
    bool commit = false;

    // ── The compose buffer (docs/editing.md §4.3) ────────────────────────────────────────────
    //
    // The hook **classifies and posts; it never edits** (§4.2). Everything below is a decision
    // about what the key means, taken in the callback because it is O(1) and allocation-free.
    // Applying it — moving a caret, inserting a character, repainting — happens on the UI thread.

    // The first edit key of a session. The microphone closes on this, at the device: §3.2 makes
    // speech and editing disjoint states, because audio_capture.h's "the mic is open only between
    // start() and stop()" is plan.md §1.9's privacy promise, and a pill that says *paused* over a
    // lit microphone light breaks the one promise the product makes about the microphone.
    bool editing_began = false;

    // The hotkey, pressed while editing (§4.5). It reopens the microphone; the caret moves to the
    // end of the buffer and the selection is dropped, because the user has just made a gesture
    // that means "I am going to talk now".
    bool resume_dictation = false;

    // What the key means to the buffer. `EditOp::None` when the key was not an edit key.
    EditOp edit_op = EditOp::None;
    bool edit_shift = false;   // extend the selection

    // InsertText, whose payload this filter cannot produce: turning a virtual key into a
    // character means asking the keyboard layout, and the hook may not. So the class is decided
    // here and the character is resolved on the UI thread from the fields below.
    bool edit_needs_char = false;

    // §4.4's Ignore class — Ctrl+S, F5, Alt+Tab, Win+E. Swallowed, and **nothing happens**; the
    // pill shows one line naming the key. It does not commit, and that is the retraction §4.4
    // records: a rule that was right for a passive preview fires an irreversible action from a
    // key that means something else, the moment the pill is a surface the user is operating.
    bool foreign_key = false;

    // Carried for the two cases above: `edit_needs_char` needs all four to ask the layout, and
    // `foreign_key` needs the virtual key and the modifiers to name the key in the hint.
    uint32_t key_vk = 0;
    uint32_t key_scan = 0;
    uint32_t key_mods = 0;
    bool caps_lock = false;

    bool stop_on_input = false;
    StopInputReason stop_reason = StopInputReason::Keystroke;

    // ── The mouse over the pill (docs/editing.md §5.2) ───────────────────────────────────────
    //
    // `InputFilter::on_mouse_event` used to end the session on **every** button-down. With a
    // clickable pill it must not do that for a click inside the pill's rectangle — and the hook
    // cannot ask the overlay where it is, because that would be a cross-module call from the
    // callback. So the UI thread publishes the rectangle into a slot and the filter does a
    // point-in-rect test, exactly as `write_click`/`read_click` publish the click point in the
    // other direction.
    //
    // Coordinates are **relative to the pill's top-left, in physical pixels**, because that is
    // what the UI thread needs to hit-test against a DirectWrite layout, and because relative
    // coordinates fit in the message parameter that carries them.
    bool pill_pointer = false;
    enum class PillPointerKind : uint8_t { Down, Move, Up, Scroll } pill_kind = PillPointerKind::Down;
    int32_t pill_x = 0;
    int32_t pill_y = 0;
    int32_t pill_wheel = 0;

    // The post-submission pill is up (overlay.h) and the user has just done something else —
    // pressed a key, or clicked anywhere but the pill. It is an offer, not a dialog, so anything
    // that is plainly not an answer to it dismisses it.
    //
    // **Never swallows.** The keystroke or click belongs to whatever the user was doing; this
    // only notices it went past. That is the same rule the mouse hook already follows for
    // stop-on-input, and it is why this is a separate flag rather than a branch inside one.
    bool dismiss_review = false;

    bool click_captured = false;  // a left click worth caching as anchor source B
    bool click_invalidated = false;  // wheel: scrolling moves the caret with no click
    bool suppress_start_menu = false;  // inject the VK_CONTROL tap before the Win key-up
};

// ─── The Edit post's payload ────────────────────────────────────────────────────────────────
//
// Everything the UI thread needs in order to apply one keystroke, packed into a single LPARAM.
//
// A single-slot cache in the style of the click seqlock would be wrong here, and the difference
// matters: clicks arrive one at a time and only the newest is of interest, whereas a fast typist
// can put several keystrokes into the queue before the UI thread drains any of them, and a slot
// would silently drop all but the last. Packing keeps each keystroke with its own message, on a
// queue that already exists and already preserves order.
//
//   bits  0.. 7  EditOp
//   bit      8   shift (extend the selection)
//   bit      9   caps lock is on
//   bit     10   the payload is a character the UI thread must resolve
//   bits 16..23  virtual key
//   bits 32..47  scan code
//   bits 48..51  modifier mask
struct EditPost {
    EditOp op = EditOp::None;
    bool shift = false;
    bool caps_lock = false;
    bool needs_char = false;
    uint32_t vk = 0;
    uint32_t scan = 0;
    uint32_t mods = 0;
};

[[nodiscard]] uint64_t pack_edit_post(const EditPost& p) noexcept;
[[nodiscard]] EditPost unpack_edit_post(uint64_t packed) noexcept;

// The mouse over the pill, packed into one LPARAM (docs/editing.md §5.2).
//
// Coordinates are relative to the pill's top-left in physical pixels, so they fit in sixteen
// signed bits with room to spare — the pill is capped at OverlayStyle::max_width_dip and eight
// lines. Absolute screen coordinates would not: a second monitor to the left of the primary has
// negative x, and a 4K wall exceeds what is left after the kind and the wheel delta.
struct PillPost {
    FilterOutcome::PillPointerKind kind = FilterOutcome::PillPointerKind::Down;
    int32_t x = 0;
    int32_t y = 0;
    int32_t wheel = 0;
};

[[nodiscard]] uint64_t pack_pill_post(const PillPost& p) noexcept;
[[nodiscard]] PillPost unpack_pill_post(uint64_t packed) noexcept;

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

    // ── Fail open: the single most important property of the feature (docs/editing.md §10.1) ──
    //
    // The hook runs on its own thread and swallows keys **independently of the UI thread**. If
    // the UI thread hangs while a session is composing, keys are swallowed by a thread that is
    // still healthy and delivered to a thread that will never process them: the user's keyboard
    // is dead, in every application, with no way out short of Ctrl+Alt+Del. That is strictly
    // worse than any failure this product can produce today, and the existing hook watchdog does
    // not help — the hook is fine, the consumer is not.
    //
    // So the UI thread stamps a monotonic tick every message-loop iteration, and before
    // swallowing anything on the pill's behalf the filter compares it against the event's own
    // timestamp. More than `kUiStaleMs` behind and the filter stops swallowing and stops posting,
    // permanently, for that session. Keys go to the application. The user has their machine back.
    //
    // The comparison is a load and a subtraction, which costs nothing against the callback budget
    // `Stats::max_callback_us` exists to watch.
    static constexpr uint32_t kUiStaleMs = 2000;
    void note_ui_heartbeat(uint64_t tick_ms) noexcept;
    [[nodiscard]] bool failed_open() const noexcept;

    // How far behind the UI thread was at the instant the latch tripped, in milliseconds. Zero
    // until it does. Kept because the latch turns the whole feature off silently and mid-session,
    // and a number is the difference between "the settings dialog was open" and a guess.
    [[nodiscard]] uint32_t failed_open_behind_ms() const noexcept;

    // True while the pill owns the keyboard: a session is running, editing is enabled, and the UI
    // thread is answering. docs/editing.md §4.1 states the price of the feature in one line —
    // "While a session is composing, the keyboard belongs to the pill" — and this is that line as
    // a predicate.
    [[nodiscard]] bool composing() const noexcept;

    // True once the first edit key has closed the microphone, until the user resumes or the
    // session ends. Set by the filter itself at the instant it classifies an Edit key, rather
    // than by a call back from the UI thread: the UI thread's answer would arrive after the next
    // keystroke, and the hotkey means different things in the two states (§4.5).
    [[nodiscard]] bool editing_active() const noexcept;

    // Where the pill is, in physical screen pixels, published by the UI thread after every
    // paint. An empty rectangle means "there is no pill" and every click is outside it, which is
    // exactly today's behaviour — docs/editing.md §16 names this as the `last_voice_ms` shape: a
    // value the tests supply and the product may not, where the symptom of a missing publisher
    // is a mouse that silently reverts to ending the session, with a green suite.
    void set_pill_rect(int32_t left, int32_t top, int32_t right, int32_t bottom) noexcept;

    // Whether the post-submission pill is up. While it is, a keystroke or a click outside the
    // pill sets FilterOutcome::dismiss_review. Off by default and off again the moment the
    // pill closes, so the ordinary idle path costs one predictable branch.
    void set_review_open(bool open) noexcept;
    [[nodiscard]] bool point_in_pill(int32_t x, int32_t y) const noexcept;

    // Pure decision. No allocation, no syscall, bounded work — safe to call from the hook.
    FilterOutcome on_event(const InputEvent& e);

    // docs/editing.md §12. With `enabled` false the filter behaves exactly as it did before the
    // compose buffer existed: Enter is stop-on-input and reaches the target, and the hotkey
    // keeps plan.md §1.8's toggle.
    void set_editing(const EditingConfig& cfg) noexcept;
    [[nodiscard]] const EditingConfig& editing() const noexcept;

    // Test/inspection accessors.
    [[nodiscard]] bool session_active() const noexcept;
    [[nodiscard]] bool is_key_down(uint32_t vk) const noexcept;
    [[nodiscard]] size_t keys_held_at_activation() const noexcept;

private:
    // Split by event class so that each rule is readable in isolation. All three are pure and
    // allocation-free; none of them may grow a call into anything that can block.
    FilterOutcome on_key_event(const InputEvent& e, bool down);
    FilterOutcome on_mouse_event(const InputEvent& e);

    // docs/editing.md §4.3's class table, as a pure function of (vk, modifiers). Returns
    // EditOp::None for a key that is not an edit key. `needs_char` comes back true for a
    // printable character, whose payload only the UI thread can produce.
    [[nodiscard]] EditOp classify_edit(uint32_t vk, uint32_t mods, bool& needs_char) const noexcept;

    // The two never-swallowed groups of §4.3: bare modifiers, and the keys that change the
    // machine rather than the document.
    [[nodiscard]] bool is_passthrough(uint32_t vk) const noexcept;

    // Modifier bitmask implied by the keys currently down. `ignore_vk` is excluded, which is what
    // lets a modifier key itself be the binding (right Ctrl, right Alt): otherwise pressing the
    // bound key would put kModCtrl into the current mask and never match a spec of kModNone.
    [[nodiscard]] uint32_t current_mods(uint32_t ignore_vk) const noexcept;
    [[nodiscard]] bool chord_matches(const InputEvent& e) const noexcept;

    // True when `vk` is one of the modifier keys this binding uses — either side, plus the
    // neutral code SendInput produces. It is what stops the first half of the finalising chord
    // from being read as the user typing; see the comment at its use.
    [[nodiscard]] bool is_binding_modifier(uint32_t vk) const noexcept;

    // Snapshot the keys currently down. Called the instant the chord is recognised — NOT when
    // the owner later confirms via note_session_started(). See the comment in hotkey.cpp: the
    // chord's key-ups reach the hook long before the posted activation reaches the UI thread.
    void begin_session_state(uint64_t now_ms) noexcept;

    // Fixed-size state: a bitset over the 256 virtual-key codes, twice (currently down, and
    // down-at-activation). No heap, so the hook path cannot allocate.
    static constexpr size_t kVkWords = vk::kCount / 64;
    uint64_t down_[kVkWords]{};
    uint64_t down_at_activation_[kVkWords]{};
    // Key-downs this filter swallowed on the pill's behalf. Their key-ups must be swallowed too:
    // a target that never saw the press but does see the release can get its own key-state
    // tracking stuck — the same failure the binding key's release is already swallowed to avoid.
    uint64_t swallowed_down_[kVkWords]{};
    HotkeySpec spec_{};
    EditingConfig editing_{};
    // Constructed **disabled**, deliberately, even though EditingConfig's own default is
    // enabled. Until the owner has told this filter what the user configured, it must not be
    // eating keys on behalf of a pill that may or may not exist — the fail-safe direction is a
    // filter that behaves exactly as it did before the compose buffer was written. The two
    // defaults are not in conflict: EditingConfig's is what a settings file with no `editing`
    // block means, and this is what a filter nobody has configured means. See the constructor.
    uint64_t ui_heartbeat_ms_ = 0;
    bool ui_fail_open_ = false;   // latched for the rest of the session once it trips
    uint32_t ui_behind_ms_ = 0;   // how far behind it was when it tripped, for the log
    bool editing_active_ = false;
    int32_t pill_left_ = 0, pill_top_ = 0, pill_right_ = 0, pill_bottom_ = 0;
    bool review_open_ = false;
    bool pill_dragging_ = false;  // a button-down landed in the pill and has not been released
    uintptr_t injection_mark_ = 0;
    bool session_active_ = false;
    uint64_t session_started_ms_ = 0;
    bool hotkey_chord_consumed_ = false;  // one activation per physical chord press

    // Push-to-talk: the binding is being held for *this* session, so its release finalises.
    //
    // Distinct from `hotkey_chord_consumed_`, which answers "did we swallow this key's press"
    // and must survive the end of the session so the matching key-up is swallowed too. This one
    // answers "does the release mean finish", and is cleared the moment it fires so the rest of
    // the chord coming up cannot finalise a second time.
    bool ptt_held_ = false;
};

// ─── The Windows-facing manager ─────────────────────────────────────────────────────────────

class IHotkeyListener {
public:
    virtual ~IHotkeyListener() = default;

    // All of these arrive on the owner's UI thread, never on the hook thread.
    virtual void on_activate() = 0;
    virtual void on_finalize() = 0;
    virtual void on_cancel() = 0;

    // Enter, swallowed: type the composed buffer into the document and end the session
    // (docs/editing.md §4.3). Distinct from on_finalize() so the log — and anyone reading this
    // interface — can tell the two gestures apart.
    virtual void on_commit() = 0;

    // The compose buffer. `p.op` is what the key meant; when `p.needs_char` is set the UI thread
    // must ask the keyboard layout for the character before it can insert anything
    // (docs/editing.md §4.2 — the hook is not allowed to make that call).
    virtual void on_edit(const EditPost& p) = 0;

    // The first edit key: close the microphone (§3.2).
    virtual void on_editing_began() = 0;

    // The hotkey while editing: reopen it, drop the selection, caret to the end (§3.3).
    virtual void on_resume_dictation() = 0;

    // §4.4's Ignore class. The pill names the key and says what to do about it; nothing else
    // happens, and in particular nothing is committed.
    virtual void on_foreign_key(uint32_t vk, uint32_t mods) = 0;

    // The mouse inside the pill (§5.2). Placing a caret is an edit gesture and closes the
    // microphone exactly as the first edit key does; scrolling is **not** — hotkey.cpp already
    // reasons that scrolling to re-read what you dictated is a normal thing to do mid-session,
    // and that stays true here.
    virtual void on_pill_pointer(const PillPost& p) = 0;

    virtual void on_stop_input(StopInputReason reason) = 0;

    // Anchor source B (plan.md §1.5). `window` is the HWND under the click, as an opaque value.
    virtual void on_left_click(int32_t x, int32_t y, uintptr_t window, uint64_t time_ms) = 0;

    // Wheel invalidates the cached click: scrolling moves the caret with no click.
    virtual void on_wheel() = 0;

    // Any real input arrived while the post-submission pill was up. See FilterOutcome above.
    virtual void on_dismiss_review() = 0;

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
    Commit,     // Enter — docs/editing.md §4.3. Appended, not inserted: the values are compared
                // as integers across a PostMessage boundary and renumbering them would make an
                // in-flight message mean something else.
    Edit,            // lParam: pack_edit_post()
    EditingBegan,
    ResumeDictation,
    ForeignKey,      // lParam: vk | (mods << 16)
    PillPointer,     // lParam: pack_pill_post()
    FailedOpen,      // the fail-open latch tripped; lParam: how many ms behind the UI thread was
    DismissReview,   // any real input while the post-submission pill is up (overlay.h)
};


// Call from the owner window's WndProc. Returns true if the message belonged to this module,
// in which case the listener has already been invoked and the WndProc should return 0.
bool dispatch_hotkey_message(uint32_t msg, uintptr_t wparam, intptr_t lparam);

// docs/editing.md §10.1's fail-open heartbeat, as a free function so that **any** message pump on
// the UI thread can stamp it without holding a HotkeyManager.
//
// That is not a convenience. `HotkeyManager::heartbeat()` is called once per iteration of the
// owner's loop, and the owner's loop is not the only loop: the settings dialog pumps its own
// (settings.cpp, "modal by hand"), MessageBoxW pumps its own, TrackPopupMenu pumps its own. Every
// one of those is a *healthy* UI thread that is simply not turning the outer loop, and each of
// them used to look identical to a hung one — two seconds in, the hook stopped swallowing, and a
// session started while the settings window was open sent Enter to the target and injected the
// dictated text after it. The fix that survives the next nested loop somebody adds is a timer
// with a TIMERPROC (main.cpp), because DispatchMessageW delivers those from inside every one of
// these pumps; this is the entry point it calls.
//
// A no-op when the hooks are not installed, which is also when nothing can fail open.
void note_ui_alive() noexcept;

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

    // Same route, same reason: the filter is hook-thread-only state, so a settings change
    // reaches it as a posted message rather than as a write from the UI thread.
    void set_editing(const EditingConfig& cfg);

    // The dwExtraInfo value that marks this process's own synthetic input, so the filter can
    // ignore it (plan.md §1.8: "Our own SendInput traffic must never stop the session producing
    // it"). The value belongs to injector.h's InjectionMark; the owner wires the two together
    // with `mgr.set_injection_mark(injection_mark().extra_info)` rather than this module reaching
    // into the injector — the hook must not depend on the thing it is filtering out.
    void set_injection_mark(uintptr_t extra_info);

    // Tell the filter about the session lifecycle. Called from the UI thread.
    void note_session_started();
    void note_session_ended();

    // docs/editing.md §10.1's fail-open heartbeat. **Call this every message-loop iteration on
    // the UI thread.** It is one relaxed atomic store and one GetTickCount, and it is what stands
    // between a hung UI thread and a keyboard that is dead in every application on the machine.
    //
    // If it is ever removed, or a loop is added that does not call it, the symptom is not a crash
    // — it is the compose buffer silently ceasing to accept keys about two seconds into every
    // session, which is the safe direction and the reason the check is written this way round.
    void heartbeat() noexcept;

    // Publish the pill's rectangle in physical screen pixels, after every paint that moved or
    // resized it (docs/editing.md §5.2). An empty rectangle unpublishes it, which is what a
    // hidden pill means and what every session must end with.
    void set_pill_rect(int32_t left, int32_t top, int32_t right, int32_t bottom) noexcept;

    // Whether the post-submission pill is up. While it is, a keystroke or a click outside the
    // pill sets FilterOutcome::dismiss_review. Off by default and off again the moment the
    // pill closes, so the ordinary idle path costs one predictable branch.
    void set_review_open(bool open) noexcept;

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
