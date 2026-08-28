// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// kvit-dictate.exe — tray, message loop, lifecycle.
//
// This file is the serialisation point. SessionMachine, CommitEngine, Injector, Overlay and the
// anchor bookkeeping are all single-threaded by design; three other threads exist (the hotkey
// hook, the WASAPI capture thread, the IPC I/O thread) and NONE of them touches those objects.
// They post to the message-only window and return immediately. That is not a style preference:
// a WH_KEYBOARD_LL callback that blocks is silently unhooked by Windows and the hotkey dies with
// no error at all (plan.md §1.8), and a WASAPI callback that blocks drops audio.
//
// The division of labour with session.cpp is worth stating, because it is easy to misread this
// file as "the logic lives here". It does not. SessionMachine decides; main.cpp *applies*. Every
// decision arrives as Outcome{verdict, to_send, effects} and this file's job is to execute the
// effects in order and write the messages. Anything that looks like a policy decision in here
// (when to inject, when to end a session, which messages are stale) is a bug — it belongs in the
// state machine where it is unit-tested without a window.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <objbase.h>
#include <shellapi.h>

#include <atomic>
#include <deque>
#include <iterator>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "anchor.h"
#include "audio_capture.h"
#include "commit_engine.h"
#include "hotkey.h"
#include "normalizer.h"
#include "injector.h"
#include "ipc_client.h"
#include "overlay.h"
#include "protocol.h"
#include "session.h"
#include "settings.h"
#include "theme.h"
#include "tray_icon.h"
#include "util/clock.h"
#include "util/json.h"
#include "util/log.h"
#include "util/utf8.h"

namespace {

// ─── Window messages ────────────────────────────────────────────────────────────────────────
//
// One per producer thread, all carrying "there is work in a queue" rather than the work itself.
// Passing a pointer through WPARAM would mean allocating on the producer thread and freeing on
// the consumer, and a dropped message would leak; a queue plus a wake-up cannot.

constexpr UINT kMsgIpcQueue    = WM_APP + 1;
constexpr UINT kMsgIpcState    = WM_APP + 2;
constexpr UINT kMsgAudioQueue  = WM_APP + 3;
constexpr UINT kMsgHotkeyQueue = WM_APP + 4;
constexpr UINT kMsgTray        = WM_APP + 5;
constexpr UINT kMsgShowSettings = WM_APP + 6;
constexpr UINT kMsgNormalized  = WM_APP + 7;

constexpr UINT_PTR kTickTimer = 1;
constexpr UINT kTickIntervalMs = 50;  // drives the two silence timers and the commit cadence

// How long the target is held after `stop` while waiting for `final` and `stopped`. Generous
// because a real model's flush is not instant, finite because a child that never answers must not
// strand the session forever.
constexpr uint64_t kFinalizeGraceMs = 5000;

// How long a hotkey press waits for a model that is still loading before it is given up on. The
// protocol allows the sidecar 180 s to reach ready (docs/protocol.md §1.1 READY_TIMEOUT_MS), which
// is far too long to hold a microphone-shaped promise on screen; a model that has not loaded in
// this long is a problem to report, not to wait out. The slowest real load measured is 4155 ms
// (windows-speech, cold).
constexpr uint64_t kActivationWaitMs = 30000;

// How long docs/editing.md §4.4's one-line hint stays in the pill. "A couple of seconds", says
// §4.4, on the tick App already runs — no new timer, and deliberately not flag_violation(),
// which is a persistent mark on text that may be wrong (plan.md §1.4).
constexpr uint64_t kHintMs = 2500;

// Is this connection state one that still reaches IDLE on its own? The four states of a child
// coming up are; the three that mean a session is already in flight are handled before this is
// asked; DEAD is not, and waiting in it would be waiting for nothing.
bool connection_can_become_idle(kd::proto::ConnState s) noexcept {
    switch (s) {
        case kd::proto::ConnState::Spawning:
        case kd::proto::ConnState::Validating:
        case kd::proto::ConnState::Handshake:
        case kd::proto::ConnState::Loading:
            return true;
        case kd::proto::ConnState::Idle:
        case kd::proto::ConnState::Running:
        case kd::proto::ConnState::Finalizing:
        case kd::proto::ConnState::Cancelling:
        case kd::proto::ConnState::Dead:
            return false;
    }
    return false;  // unreachable; enumerated so a new value is a compiler warning
}

constexpr UINT kTrayId = 1;

// How many ticks between asking the registry whether the taskbar changed scheme. At kTickIntervalMs
// this is about two seconds — slow enough to be free, fast enough that nobody catches the icon in
// the old scheme. See App::poll_taskbar_theme for why this is a poll and not a message.
constexpr uint32_t kTrayThemePollTicks = 40;
constexpr UINT kMenuToggle = 100;
constexpr UINT kMenuSettings = 101;
constexpr UINT kMenuOpenLogs = 102;
constexpr UINT kMenuQuit = 103;

const wchar_t kWindowClass[] = L"kvit-dictate-message-window";
const wchar_t kSingleInstanceMutex[] = L"Local\\kvit-dictate-single-instance";

// A queued hotkey/mouse notification from the hook thread.
struct HookNote {
    enum class Kind : uint8_t {
        Activate, Finalize, Cancel, StopInput, LeftClick, Wheel, HookLost,
        Commit,          // Enter — docs/editing.md §4.3
        Edit,            // a keystroke the compose buffer claimed (§4.3)
        EditingBegan,    // the first edit key: close the microphone (§3.2)
        ResumeDictation, // the hotkey while editing: reopen it (§3.3)
        ForeignKey,      // §4.4's Ignore class: name the key in the pill and do nothing
        PillPointer,     // the mouse inside the pill (§5.2)
    };
    Kind kind = Kind::Activate;
    kd::StopInputReason reason = kd::StopInputReason::Keystroke;
    int32_t x = 0;
    int32_t y = 0;
    uintptr_t window = 0;
    uint64_t time_ms = 0;
    kd::EditPost edit{};
    kd::PillPost pill{};
};

std::wstring widen(std::string_view utf8) {
    std::wstring w;
    kd::utf8::to_utf16(utf8, w, nullptr);
    return w;
}

// Where `-m <module>` can be imported from.
//
// The sidecar package is deliberately **not** installed into any of the interpreters
// (server/README.md §2: the `mock` environment has nothing installed at all, which is the whole
// point of it), so the launcher has to put the directory *containing* the package on the child's
// PYTHONPATH. models.json cannot carry it, because that path is a property of where this build
// happens to live, not of the model.
//
// Two layouts, in order: the package beside the executable (a deployed tree), then `server/`
// under any ancestor (the source tree, where the exe sits in build-*/bin/<config>/).
std::filesystem::path find_sidecar_root(std::string_view module) {
    if (module.empty()) return {};
    const std::filesystem::path pkg = std::filesystem::path(module) / "__init__.py";
    std::error_code ec;
    std::filesystem::path dir = kd::executable_directory();
    for (int depth = 0; depth < 8 && !dir.empty(); ++depth) {
        if (std::filesystem::exists(dir / pkg, ec)) return dir;
        if (std::filesystem::exists(dir / "server" / pkg, ec)) return dir / "server";
        std::filesystem::path parent = dir.parent_path();
        if (parent == dir) break;  // the root; parent_path() of "C:\" is itself
        dir = std::move(parent);
    }
    return {};
}

bool env_names_equal(std::string_view a, std::string_view b) {
    // Windows environment variable names are case-insensitive. ASCII is enough here: the only
    // name this is asked about is PYTHONPATH.
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const char ca = (a[i] >= 'a' && a[i] <= 'z') ? static_cast<char>(a[i] - 32) : a[i];
        const char cb = (b[i] >= 'a' && b[i] <= 'z') ? static_cast<char>(b[i] - 32) : b[i];
        if (ca != cb) return false;
    }
    return true;
}

// ─── The application ────────────────────────────────────────────────────────────────────────

class App final : public kd::IHotkeyListener,
                  public kd::audio::IAudioSink,
                  public kd::anchor::IAnchorListener {
public:
    explicit App(HINSTANCE instance) : instance_(instance) {}

    bool startup();
    void shutdown();
    int run();

    // ── IHotkeyListener (hook thread — post only, never touch state) ────────────────────────
    void on_activate() override { post_hook(HookNote{HookNote::Kind::Activate}); }
    void on_finalize() override { post_hook(HookNote{HookNote::Kind::Finalize}); }
    void on_cancel() override { post_hook(HookNote{HookNote::Kind::Cancel}); }
    void on_commit() override { post_hook(HookNote{HookNote::Kind::Commit}); }
    void on_edit(const kd::EditPost& p) override {
        HookNote n;
        n.kind = HookNote::Kind::Edit;
        n.edit = p;
        post_hook(n);
    }
    void on_editing_began() override { post_hook(HookNote{HookNote::Kind::EditingBegan}); }
    void on_resume_dictation() override { post_hook(HookNote{HookNote::Kind::ResumeDictation}); }
    void on_foreign_key(uint32_t vk, uint32_t mods) override {
        HookNote n;
        n.kind = HookNote::Kind::ForeignKey;
        n.edit.vk = vk;
        n.edit.mods = mods;
        post_hook(n);
    }
    void on_pill_pointer(const kd::PillPost& p) override {
        HookNote n;
        n.kind = HookNote::Kind::PillPointer;
        n.pill = p;
        post_hook(n);
    }
    void on_stop_input(kd::StopInputReason reason) override {
        HookNote n;
        n.kind = HookNote::Kind::StopInput;
        n.reason = reason;
        post_hook(n);
    }
    void on_left_click(int32_t x, int32_t y, uintptr_t window, uint64_t time_ms) override {
        HookNote n;
        n.kind = HookNote::Kind::LeftClick;
        n.x = x;
        n.y = y;
        n.window = window;
        n.time_ms = time_ms;
        post_hook(n);
    }
    void on_wheel() override { post_hook(HookNote{HookNote::Kind::Wheel}); }
    void on_hook_lost() override { post_hook(HookNote{HookNote::Kind::HookLost}); }

    // ── IAudioSink (WASAPI thread — post only) ──────────────────────────────────────────────
    void on_audio(const kd::audio::AudioChunk& chunk) override;
    void on_discontinuity(uint32_t gap_ms, const char* reason) override;
    void on_device_change(const kd::audio::DeviceInfo& dev, bool device_lost) override;

    // ── IAnchorListener (anchor worker thread — post only) ──────────────────────────────────
    void on_anchor(const kd::anchor::Anchor& a) override;

private:
    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    void post_hook(const HookNote& n);
    void drain_hook_queue();
    void drain_ipc_queue();
    void drain_audio_queue();

    // `src` is the message being processed, if any. Effect::ApplyText names an action, not a
    // payload — the Outcome deliberately does not copy the message back to us — so the caller
    // that owns the message passes it through.
    void apply(const kd::SessionMachine::Outcome& o, const kd::proto::Message* src = nullptr);
    void apply_text(const kd::proto::Message& m);
    void perform_commit(const kd::CommitEngine::Result& r);
    bool submit_before_frame();
    void apply_overlay_mode();

    void begin_dictation();
    void end_dictation(kd::LocalEvent why);
    void finish_dictation(const char* why);
    // Holds the display awake for as long as a session is running. See the definition for why a
    // dictation that types nothing looks exactly like an idle machine to Windows.
    void keep_display_awake(bool on);

    // ── The compose buffer (docs/editing.md) ────────────────────────────────────────────────
    //
    // True when the pill owns the keyboard for this session: the setting is on, which
    // Settings::normalise() only permits under Cadence::EndOfSession (§6.1). Read in one place
    // so that nothing downstream re-derives it and disagrees — §16's `min_confidence` shape.
    [[nodiscard]] bool composing() const noexcept;

    void apply_edit(const kd::EditPost& p);
    void begin_editing();      // §3.2: the microphone closes, at the device
    void resume_dictation();   // §3.3: it reopens, the caret moves to the end
    void show_foreign_key_hint(uint32_t vk, uint32_t mods);
    void apply_pill_pointer(const kd::PillPost& p);
    // Republish the pill's rectangle to the hook after every paint. docs/editing.md §16 calls
    // this the `last_voice_ms` shape: if the UI thread never publishes it, the slot reads zero,
    // every click is outside the pill, and the mouse silently reverts to ending the session —
    // with a green suite, because the tests write the rectangle themselves.
    void publish_pill_rect();
    // Pushes the buffer, the caret and the selection at the overlay together, because a caret
    // offset that arrives without the text it indexes is an offset into the previous frame.
    void push_edit_view();

    void request_normalize();
    void drain_normalize_queue();
    void cancel_normalize();
    // Empties the compose buffer into the document, or onto the clipboard when the target is
    // gone. Safe to call with nothing buffered: it returns having done nothing.
    void flush_deferred_commits(const char* why);
    // What the overlay should be showing right now, given the cadence. `tail` is the engine's
    // unstable text for the utterance in progress.
    void render_preview(std::string_view tail);

    bool spawn_sidecar();
    void on_conn_state(kd::proto::ConnState s);
    void on_sidecar_lost();

    void show_settings();
    void apply_settings(kd::Settings next);

    void set_tray_state(kd::OverlayState s);
    void update_tray();
    void poll_taskbar_theme();
    void show_tray_menu();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HANDLE instance_mutex_ = nullptr;
    bool tray_added_ = false;
    bool quitting_ = false;

    kd::Settings settings_;
    kd::ModelManifest manifest_;
    kd::SteadyClock clock_;

    std::unique_ptr<kd::SessionMachine> session_;
    std::unique_ptr<kd::CommitEngine> commit_;
    std::unique_ptr<kd::Injector> injector_;
    std::unique_ptr<kd::IpcClient> ipc_;
    std::unique_ptr<kd::audio::AudioCapture> capture_;
    std::unique_ptr<kd::HotkeyManager> hotkey_;
    std::unique_ptr<kd::Overlay> overlay_;

    std::unique_ptr<kd::anchor::IScreenCapture> screen_capture_;
    std::unique_ptr<kd::anchor::ICaretProvider> caret_provider_;
    std::unique_ptr<kd::anchor::AnchorEstimator> anchor_;

    std::optional<kd::TargetIdentity> target_;

    // Cadence::EndOfSession: committed ops accumulate here instead of going straight into the
    // document, and `flush_deferred_commits` empties it exactly once per session. It holds the
    // TEXT rather than the ops because that is all the injector needs, and because the leading
    // space between utterances is decided by `CommitOp::leading_space` in the one place that
    // already decides it (plan.md §1.6) rather than re-derived later from a list of ops.
    //
    // docs/editing.md §3: the settled region — text the commit engine has finalised and handed
    // over under Cadence::EndOfSession. Complete, not going to change on its own, and the only
    // region the caret can occupy. The unstable tail is deliberately *not* in here; because it
    // is absent, the buffer's own caret clamp is §3's freeze rule in its entirety.
    //
    // It replaces a plain std::string, and with no editing it behaves as one: append at the end,
    // take it all at the end of the session.
    kd::EditBuffer edit_;

    // The rewriter (normalizer.h). Only ever touches `edit_`, only under Cadence::EndOfSession,
    // and never after the user has started editing.
    std::unique_ptr<kd::Normalizer> normalizer_;
    // The raw accumulation, kept alongside `edit_` because a rewriter is not idempotent: feeding
    // it its own output makes it keep editing, and the drift is toward shorter. Every request
    // sends this, never the pill's current contents.
    std::string normalize_raw_;
    uint64_t normalize_pending_id_ = 0;
    // Cleared for the session the moment the user edits. Their text is authoritative and a
    // rewrite arriving afterwards would silently discard the correction they just made.
    bool normalize_active_ = false;
    // `stopped` arrived while a rewrite was still in flight. The target is held (it already is,
    // for `final`) until the answer lands or the finalize grace expires, because the last
    // utterance of a session is otherwise the one utterance that never gets normalized.
    bool finish_awaiting_normalize_ = false;
    std::mutex normalize_mutex_;
    std::vector<kd::Normalizer::Result> normalize_queue_;

    // Which of the two states this session is in (§1). Dictating: the microphone is open and
    // speech is appended. Editing: the microphone is **closed** and the buffer is a text field.
    bool editing_ = false;
    uint64_t editing_began_ms_ = 0;   // for the resume `discontinuity` (§3.3)

    // §4.4's hint line, expired on the tick App already runs. Not flag_violation(): a violation
    // is a persistent mark on text that may be wrong (plan.md §1.4), and this is neither
    // persistent nor about the text.
    uint64_t hint_expires_ms_ = 0;

    kd::OverlayState tray_state_ = kd::OverlayState::Idle;
    // The icon currently in the notification area, and the taskbar scheme it was drawn for.
    // Owned here: tray_icon.h's icons are ours to destroy, and the old one is only released
    // *after* the shell has been handed its replacement.
    HICON tray_icon_ = nullptr;
    bool tray_icon_dark_ = false;
    uint32_t tray_theme_tick_ = 0;
    // What apply_overlay_mode() last pushed to the overlay, so the transition is logged
    // once rather than on every tick that carries an anchor.
    kd::OverlayMode overlay_mode_applied_ = kd::OverlayMode::Hidden;
    std::string loaded_model_;
    bool audio_running_ = false;
    bool display_awake_ = false;    // an ES_DISPLAY_REQUIRED request is outstanding
    unsigned respawn_attempts_ = 0;
    bool sidecar_respawn_ = false;  // this spawn is §10.3's recovery, not the first launch
    bool settings_open_ = false;
    bool finalizing_ = false;       // `stop` is out; the target is held until `stopped`
    uint64_t finalize_deadline_ms_ = 0;

    // A hotkey press that arrived before the model was ready, held until it is (§10.2 allows
    // `start` only from IDLE). Nothing before `windows-speech` made this reachable: every other
    // backend reports ready in 0–77 ms, so the window did not exist in practice. That model takes
    // ~4.2 s, which is long enough that pressing the hotkey inside it is the normal case rather
    // than a race.
    bool activation_pending_ = false;
    uint64_t activation_deadline_ms_ = 0;

    std::mutex ipc_mutex_;
    std::deque<kd::proto::Message> ipc_queue_;
    std::mutex audio_mutex_;
    std::vector<int16_t> audio_queue_;
    std::mutex hook_mutex_;
    std::deque<HookNote> hook_queue_;
    std::atomic<bool> anchor_dirty_{false};
    std::mutex anchor_mutex_;
    std::optional<kd::anchor::Anchor> latest_anchor_;
};

App* g_app = nullptr;

// ─── Producer-side posts ────────────────────────────────────────────────────────────────────

void App::post_hook(const HookNote& n) {
    {
        std::lock_guard<std::mutex> lock(hook_mutex_);
        hook_queue_.push_back(n);
    }
    // PostMessage, never SendMessage: SendMessage would block the hook callback until the UI
    // thread answered, which is the exact stall that gets a low-level hook unhooked.
    if (hwnd_ != nullptr) PostMessageW(hwnd_, kMsgHotkeyQueue, 0, 0);
}

void App::on_audio(const kd::audio::AudioChunk& chunk) {
    {
        std::lock_guard<std::mutex> lock(audio_mutex_);
        audio_queue_.insert(audio_queue_.end(), chunk.samples, chunk.samples + chunk.count);
    }
    if (hwnd_ != nullptr) PostMessageW(hwnd_, kMsgAudioQueue, 0, 0);
}

void App::on_discontinuity(uint32_t gap_ms, const char* reason) {
    // plan.md §1.6 and docs §6.1: a WASAPI glitch is reported, never papered over. Padding or
    // splicing hands the decoder a lie it cannot detect.
    KD_LOG_WARN("audio discontinuity: %u ms (%s)", gap_ms, reason != nullptr ? reason : "?");
    if (session_ != nullptr && hwnd_ != nullptr) {
        PostMessageW(hwnd_, kMsgHotkeyQueue, 0, 0);  // wake the loop; the tick applies the event
    }
}

void App::on_device_change(const kd::audio::DeviceInfo& dev, bool device_lost) {
    (void)dev;
    if (device_lost) {
        KD_LOG_WARN("capture device lost; ending the session rather than recording silence");
        if (hwnd_ != nullptr) PostMessageW(hwnd_, kMsgHotkeyQueue, 0, 0);
    }
}

void App::on_anchor(const kd::anchor::Anchor& a) {
    {
        std::lock_guard<std::mutex> lock(anchor_mutex_);
        latest_anchor_ = a;
    }
    anchor_dirty_.store(true, std::memory_order_release);
}

// ─── Startup ────────────────────────────────────────────────────────────────────────────────

bool App::startup() {
    // One instance. A second launch raises the first's settings dialog rather than exiting
    // silently, because a tray app that appears to do nothing when double-clicked reads as
    // broken.
    instance_mutex_ = CreateMutexW(nullptr, TRUE, kSingleInstanceMutex);
    if (instance_mutex_ != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        // FindWindowExW with HWND_MESSAGE as the parent, not FindWindowW: a message-only window is
        // not a child of the desktop and the top-level search never sees it. FindWindowW returns
        // null here every time, which would make a second launch exit in silence — precisely the
        // "double-click does nothing, so it must be broken" outcome this branch exists to avoid.
        if (HWND existing = FindWindowExW(HWND_MESSAGE, nullptr, kWindowClass, nullptr)) {
            PostMessageW(existing, kMsgShowSettings, 0, 0);
        }
        return false;
    }

    const std::filesystem::path settings_path = kd::default_settings_path();
    if (auto loaded = kd::load_settings(settings_path); loaded) {
        settings_ = loaded.value();
    } else {
        // A missing file is the first run, not a failure. A malformed one IS a failure, and we
        // say so rather than silently reverting to defaults the user did not choose.
        KD_LOG_WARN("settings: %s — using defaults", loaded.status().message().c_str());
    }
    settings_.normalise();
    if (const kd::Status st = settings_.validate(); !st.ok()) {
        MessageBoxW(nullptr, widen(st.message()).c_str(), L"kvit-dictate — settings",
                    MB_OK | MB_ICONERROR);
        return false;
    }

    kd::log::Config log_cfg;
    log_cfg.level = settings_.log_level;
    if (settings_.diagnostics_file_enabled) {
        // Opt-in only (plan.md §1.9): a diagnostics file records window titles and process names,
        // which is close enough to the transcript line that it is off by default. The setting is
        // parsed and offered in the dialog, so it has to actually reach the logger — an opt-in
        // that silently does nothing is worse than no switch at all.
        log_cfg.file = kd::default_log_directory() / L"kvit-dictate.log";
    }
    kd::log::init(log_cfg);
    kd::log::set_transcript_policy(settings_.transcript);

    const std::filesystem::path manifest_path = kd::executable_directory() / "models.json";
    if (auto m = kd::load_model_manifest(manifest_path); m) {
        manifest_ = m.value();
    } else {
        KD_LOG_ERROR("models.json: %s", m.status().message().c_str());
    }
    // A first run has no settings.json, so nothing has chosen a model yet; models.json's
    // `default_model` *is* that choice. Without this, the first launch resolves the empty id,
    // reports "no model is selected" and never starts a sidecar at all.
    if (settings_.model_id.empty()) settings_.model_id = manifest_.default_model;

    // Before the first window of any kind: theme.h is explicit that a control created before
    // the process is opted in stays light, and this is also what makes the tray's popup menu
    // follow the scheme — a menu is drawn by the system from a cache built at first use, so
    // there is no later moment at which it can be fixed.
    kd::theme::init_app_dark_mode();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &App::wnd_proc;
    wc.hInstance = instance_;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    // HWND_MESSAGE: a message-only window. It is never visible, never activated, and cannot
    // steal focus from the target we are about to type into.
    hwnd_ = CreateWindowExW(0, kWindowClass, L"kvit-dictate", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                            instance_, this);
    if (hwnd_ == nullptr) return false;

    session_ = std::make_unique<kd::SessionMachine>(clock_, settings_.session);
    commit_ = std::make_unique<kd::CommitEngine>(clock_, settings_.stability);
    injector_ = std::make_unique<kd::Injector>(settings_.inject);
    ipc_ = std::make_unique<kd::IpcClient>();
    capture_ = std::make_unique<kd::audio::AudioCapture>();
    hotkey_ = std::make_unique<kd::HotkeyManager>();

    overlay_ = std::make_unique<kd::Overlay>();
    if (const kd::Status st = overlay_->create(reinterpret_cast<uintptr_t>(instance_),
                                               settings_.overlay_style);
        !st.ok()) {
        // A missing overlay is a degraded mode, not a dead app: injection still works and the
        // user still gets their text. Say so once and continue.
        KD_LOG_ERROR("overlay unavailable (%s) — continuing without a preview",
                     st.message().c_str());
        overlay_.reset();
    } else {
        // No anchor yet, so this resolves to the configured fallback rather than Inline. That is
        // the contract in anchor.h: current_fallback() is consulted when current() is absent.
        apply_overlay_mode();
    }

    screen_capture_ = kd::anchor::make_bitblt_capture(true);
    caret_provider_ = kd::anchor::make_gui_thread_caret_provider();
    anchor_ = std::make_unique<kd::anchor::AnchorEstimator>(settings_.anchor, screen_capture_.get(),
                                                           caret_provider_.get(), this);
    if (const kd::Status st = anchor_->start(); !st.ok()) {
        KD_LOG_WARN("anchor estimator did not start (%s); the overlay will use its fallback",
                    st.message().c_str());
    }

    // plan.md §1.8: "Our own SendInput traffic must never stop the session producing it." The
    // injector stamps every synthetic event with a per-process random dwExtraInfo; the filter
    // ignores events carrying it. Wired here rather than inside either module, because the hook
    // must not depend on the thing it is filtering out. Without this, the first injected
    // character stops the session that produced it.
    hotkey_->set_injection_mark(kd::injection_mark().extra_info);

    // docs/editing.md §12/§16. Before start(), so the very first chord is classified against the
    // user's configuration rather than against EditingConfig's defaults — and unconditionally,
    // because Settings::normalise() has already forced `enabled` off for any cadence but
    // end-of-session and this is the one place that answer is consumed.
    hotkey_->set_editing(settings_.editing);

    // Said out loud at startup, because the way this feature disappears is silent.
    // docs/editing.md §16: `editing.enabled` defaults to true and Cadence::Immediate is still the
    // default in commit_engine.h, so a user who has changed nothing has a compose buffer that
    // parses, validates, round-trips through settings.json — and is off. That is
    // `min_confidence` and `is_usable()` verbatim, and one line in the log is the difference
    // between "the pill will not let me edit" and an hour of looking for the bug.
    KD_LOG_INFO("compose buffer: %s (cadence=%s, mouse=%d, on_foreign_key=%s)",
                settings_.editing.enabled ? "ENABLED" : "off - it needs cadence=end-of-session",
                kd::to_string(settings_.stability.cadence), settings_.editing.mouse ? 1 : 0,
                kd::to_string(settings_.editing.on_foreign_key));

    if (const kd::Status st = hotkey_->start(settings_.hotkey,
                                             reinterpret_cast<uintptr_t>(hwnd_), this);
        !st.ok()) {
        MessageBoxW(hwnd_,
                    L"The global hotkey could not be installed. Another program may already own "
                    L"it, or a security product may be blocking low-level keyboard hooks.",
                    L"kvit-dictate", MB_OK | MB_ICONWARNING);
    }

    // The rewriter. Started even when disabled — start() is cheap, the worker parks on a
    // condition variable, and request() refuses while `enabled` is false — so that turning it on
    // in the settings dialog needs no lifecycle handling beyond set_config().
    normalizer_ = std::make_unique<kd::Normalizer>();
    {
        kd::Normalizer::Callbacks ncb;
        // Worker thread. Enqueue and wake; decide nothing here — EditBuffer is single-threaded
        // by design (edit_buffer.h) and the UI thread is the only one allowed to touch it.
        ncb.on_result = [this](kd::Normalizer::Result r) {
            {
                std::lock_guard<std::mutex> lock(normalize_mutex_);
                normalize_queue_.push_back(std::move(r));
            }
            PostMessageW(hwnd_, kMsgNormalized, 0, 0);
        };
        if (const kd::Status st = normalizer_->start(settings_.normalizer, std::move(ncb));
            !st.ok()) {
            // Not fatal, and deliberately not a message box: a normalizer that will not start is
            // a machine that dictates without it, which is the shipped default anyway.
            KD_LOG_ERROR("normalizer disabled: %s", st.to_string().c_str());
        }
    }

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kMsgTray;
    // Drawn, not loaded: see tray_icon.h. A failure to draw it is not a reason to have no tray
    // icon at all, so the stock application icon remains the fallback — which is what this
    // showed before there was a microphone to show.
    tray_icon_dark_ = kd::theme::taskbar_prefers_dark();
    tray_icon_ = reinterpret_cast<HICON>(
        kd::make_tray_icon(kd::OverlayState::Idle, tray_icon_dark_));
    nid.hIcon = tray_icon_ != nullptr ? tray_icon_ : LoadIcon(nullptr, IDI_APPLICATION);
    lstrcpynW(nid.szTip, L"kvit-dictate — idle", ARRAYSIZE(nid.szTip));
    tray_added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;

    SetTimer(hwnd_, kTickTimer, kTickIntervalMs, nullptr);

    spawn_sidecar();
    set_tray_state(kd::OverlayState::ModelLoading);
    return true;
}

bool App::spawn_sidecar() {
    // Everything that must be true before CreateProcess is worth attempting — the id resolves,
    // the entry is enabled, the interpreter is on disk — checked here so the failure names the
    // model and the missing path instead of surfacing as ERROR_FILE_NOT_FOUND.
    if (const kd::Status st = kd::check_model_launchable(manifest_, settings_.model_id); !st.ok()) {
        KD_LOG_ERROR("%s", st.message().c_str());
        set_tray_state(kd::OverlayState::Error);
        return false;
    }
    const kd::ModelEntry* entry = manifest_.find(settings_.model_id);
    if (entry == nullptr) return false;  // check_model_launchable already proved otherwise

    kd::ChildSpec spec;
    // Already a filesystem::path (settings.h keeps it typed so a relative or malformed
    // interpreter path is rejected at load time, not at CreateProcess time).
    spec.interpreter = entry->interpreter.wstring();
    // `-m <module>`. IpcClient appends only the protocol-mandated arguments (--pipe, --proto,
    // --parent-pid), so telling the interpreter *what to run* is this file's job — without it the
    // child is launched as `python.exe --backend mock …` and dies on an unknown option.
    if (!entry->module.empty()) {
        spec.args.push_back(L"-m");
        spec.args.push_back(widen(entry->module));
    }
    for (const std::string& a : entry->args) spec.args.push_back(widen(a));
    spec.model_id = entry->id;

    bool entry_sets_pythonpath = false;
    for (const auto& kv : entry->env) {
        spec.environment.emplace_back(widen(kv.first), widen(kv.second));
        if (env_names_equal(kv.first, "PYTHONPATH")) entry_sets_pythonpath = true;
    }

    // The manifest wins if it names a PYTHONPATH: an entry that spells one out knows something
    // this search does not.
    if (!entry_sets_pythonpath) {
        const std::filesystem::path root = find_sidecar_root(entry->module);
        if (root.empty()) {
            // Not fatal here — the child will say so far more precisely, with a traceback naming
            // the module — but it is the likeliest cause of the failure that follows, and saying
            // so now saves reading the child's stderr to find out.
            KD_LOG_ERROR("cannot find a directory containing the '%s' package near %s; the "
                         "sidecar will almost certainly fail to import",
                         entry->module.c_str(),
                         kd::executable_directory().string().c_str());
        } else {
            // Replaces any inherited PYTHONPATH rather than prepending to it. A stray global
            // PYTHONPATH is far more likely to poison a locked per-model environment than to
            // contribute anything to it (plan.md §5.1).
            spec.environment.emplace_back(L"PYTHONPATH", root.wstring());
            spec.working_directory = root.wstring();
            KD_LOG_INFO("sidecar PYTHONPATH=%s", root.string().c_str());
        }
    }

    kd::IpcClient::Callbacks cb;
    // Every one of these runs on the I/O thread. They enqueue and wake; they never decide.
    cb.on_message = [this](const kd::proto::Message& m) {
        {
            std::lock_guard<std::mutex> lock(ipc_mutex_);
            ipc_queue_.push_back(m);
        }
        PostMessageW(hwnd_, kMsgIpcQueue, 0, 0);
    };
    cb.on_error = [this](const kd::proto::ProtocolError& e) {
        KD_LOG_ERROR("protocol error: %s", e.to_string().c_str());
        PostMessageW(hwnd_, kMsgIpcState, static_cast<WPARAM>(e.fatal ? 1 : 0), 0);
    };
    cb.on_disconnected = [this]() { PostMessageW(hwnd_, kMsgIpcState, 2, 0); };
    cb.on_state = [this](kd::proto::ConnState s) {
        PostMessageW(hwnd_, kMsgIpcState, 3, static_cast<LPARAM>(s));
    };

    if (const kd::Status st = ipc_->start(spec, cb); !st.ok()) {
        KD_LOG_ERROR("sidecar did not start: %s", st.message().c_str());
        set_tray_state(kd::OverlayState::Error);
        return false;
    }
    return true;
}

// ─── The message loop ───────────────────────────────────────────────────────────────────────

int App::run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // docs/editing.md §10.1's fail-open heartbeat, and it is the single most important line
        // in this function. While a session is composing the hook swallows most of the keyboard
        // on a thread that stays healthy whatever happens here; if this loop stops turning, the
        // hook sees the stamp go stale and stops swallowing, and the user gets their machine
        // back. Two calls — GetTickCount and a relaxed store — per message.
        //
        // **Any future loop that dispatches messages must call this too.** The settings dialog
        // runs a nested loop (settings.h: "modal with respect to the owner"), and a session
        // cannot be composing while it is up — the dialog is opened from the tray, which needs a
        // click, and a click ends the session — but a nested loop that is added later and does
        // not stamp is a keyboard that dies two seconds into it.
        if (hotkey_ != nullptr) hotkey_->heartbeat();
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK App::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) {
        self->hwnd_ = hwnd;
        return self->handle(msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::handle(UINT msg, WPARAM wp, LPARAM lp) {
    // The hook callback is forbidden from calling a listener — arbitrary UI work on the hook
    // thread is how a low-level hook gets slow enough for Windows to remove it — so it posts here
    // instead, and this call is where IHotkeyListener is finally invoked, on this thread
    // (hotkey.h, "The hook thread → UI thread post"). Without it the hook installs, sees every
    // keystroke, recognises the chord, posts, and nothing whatsoever happens: the message falls
    // through to DefWindowProc. That was the state of this file until the first person pressed
    // the hotkey.
    if (kd::dispatch_hotkey_message(msg, static_cast<uintptr_t>(wp), static_cast<intptr_t>(lp))) {
        return 0;
    }

    switch (msg) {
        case kMsgHotkeyQueue:
            drain_hook_queue();
            return 0;

        case kMsgNormalized:
            drain_normalize_queue();
            return 0;

        case kMsgIpcQueue:
            drain_ipc_queue();
            return 0;

        case kMsgAudioQueue:
            drain_audio_queue();
            return 0;

        case kMsgIpcState:
            if (wp == 1 || wp == 2) {
                on_sidecar_lost();
            } else if (wp == 3) {
                on_conn_state(static_cast<kd::proto::ConnState>(lp));
            }
            return 0;

        case WM_TIMER:
            if (wp == kTickTimer) {
                if (finalizing_ && clock_.now_ms() >= finalize_deadline_ms_) {
                    // Covers both waits: for `stopped`, and for a rewrite that `stopped` is
                    // being held on. A normalizer that has stopped answering must never be the
                    // reason a session fails to end, so the raw text is committed and the reason
                    // is logged.
                    if (finish_awaiting_normalize_) {
                        KD_LOG_WARN("normalize did not answer within the finalize grace; "
                                    "committing the raw text");
                        cancel_normalize();
                    } else {
                        KD_LOG_WARN("no `stopped` within %llu ms of `stop`; releasing the target",
                                    static_cast<unsigned long long>(kFinalizeGraceMs));
                    }
                    finish_dictation("finalize grace expired");
                }
                // The held press, honoured the moment the machine itself says it can accept a
                // `start`. begin_dictation() is re-entered from the top rather than resumed
                // part-way, so the target is captured *now* and every refusal — password field,
                // elevated target, secure desktop, app policy — is decided against the window
                // that has focus at this moment. Seconds passed while the model loaded, and
                // plan.md §1.7 is emphatic that a target is not allowed to drift under a session.
                if (activation_pending_ && session_ != nullptr &&
                    session_->state() == kd::proto::ConnState::Idle) {
                    activation_pending_ = false;
                    activation_deadline_ms_ = 0;
                    KD_LOG_INFO("model ready; starting the activation that was waiting for it");
                    begin_dictation();
                }
                if (activation_pending_ && clock_.now_ms() >= activation_deadline_ms_) {
                    // Held long enough. Said out loud with the state it is stuck in, because "I
                    // pressed the hotkey and nothing happened" has to be answerable from the log.
                    activation_pending_ = false;
                    activation_deadline_ms_ = 0;
                    KD_LOG_WARN("the model did not become ready within %llu ms of the hotkey "
                                "press (connection is %s); the activation was dropped",
                                static_cast<unsigned long long>(kActivationWaitMs),
                                session_ != nullptr ? kd::proto::to_string(session_->state())
                                                    : "(no session machine)");
                    set_tray_state(kd::OverlayState::Error);
                    if (overlay_ != nullptr) overlay_->set_state(kd::OverlayState::Error);
                }
                // docs/editing.md §4.4's hint, expired on the tick that already exists.
                if (hint_expires_ms_ != 0 && clock_.now_ms() >= hint_expires_ms_) {
                    hint_expires_ms_ = 0;
                    if (overlay_ != nullptr) overlay_->clear_hint();
                }
                // Two seconds' worth of ticks. See poll_taskbar_theme() for why the taskbar's
                // scheme is polled at all rather than arriving as a message.
                if (++tray_theme_tick_ >= kTrayThemePollTicks) {
                    tray_theme_tick_ = 0;
                    poll_taskbar_theme();
                }
                if (session_ != nullptr) apply(session_->on_tick());
                if (commit_ != nullptr) perform_commit(commit_->on_tick());
                if (overlay_ != nullptr) {
                    if (anchor_dirty_.exchange(false, std::memory_order_acq_rel)) {
                        {
                            std::lock_guard<std::mutex> lock(anchor_mutex_);
                            overlay_->set_anchor(latest_anchor_);
                        }
                        // This is where plan.md §1.5's degradation gate belongs, and
                        // apply_overlay_mode() explains at length why it is not applied yet.
                        // The call stays so the seam is real rather than hypothetical.
                        apply_overlay_mode();
                    }
                    // Unconditionally, and cheaply: render() returns immediately unless something
                    // is dirty or the visibility no longer matches. Rendering only when an *anchor*
                    // arrived meant set_text() marked the overlay dirty and nothing ever painted
                    // it — the preview was invisible for a whole session unless a new anchor
                    // happened to land in the same 50 ms tick.
                    overlay_->render();
                    publish_pill_rect();
                }
            }
            return 0;

        case kMsgTray:
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
                show_tray_menu();
            } else if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
                PostMessageW(hwnd_, kMsgShowSettings, 0, 0);
            }
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kMenuToggle:
                    if (session_ != nullptr) {
                        if (target_.has_value()) {
                            end_dictation(kd::LocalEvent::FinalisePressed);
                        } else {
                            begin_dictation();
                        }
                    }
                    return 0;
                case kMenuSettings:
                    PostMessageW(hwnd_, kMsgShowSettings, 0, 0);
                    return 0;
                case kMenuOpenLogs:
                    ShellExecuteW(nullptr, L"open",
                                  kd::default_settings_path().parent_path().wstring().c_str(),
                                  nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                case kMenuQuit:
                    // Every exit path logs why. Without this the process can vanish and leave
                    // nothing behind but a hotkey that no longer answers, which is precisely how
                    // it presented the first time a human ran it against a real model: three
                    // symptoms - no settings dialog, no pill, no response to the chord - all of
                    // which were one dead process, and the log could not say so.
                    KD_LOG_INFO("exiting: Quit chosen from the tray menu");
                    quitting_ = true;
                    DestroyWindow(hwnd_);
                    return 0;
                default:
                    return 0;
            }

        case kMsgShowSettings:
            show_settings();
            return 0;

        case WM_QUERYENDSESSION:
            KD_LOG_INFO("exiting: Windows is logging off or shutting down (WM_QUERYENDSESSION)");
            // Log-off or shutdown. Release the microphone and kill the child now: a sidecar left
            // holding VRAM through a shutdown is exactly what the job object exists to prevent.
            shutdown();
            return TRUE;

        case WM_ENDSESSION:
            KD_LOG_INFO("exiting: WM_ENDSESSION");
            shutdown();
            return 0;

        case WM_DESTROY:
            // Reached from the tray Quit above, and from anything else that destroys the window.
            // `quitting_` tells the two apart, and the difference matters: an unexplained destroy
            // is a bug report, a chosen one is not.
            KD_LOG_INFO("exiting: window destroyed (%s)",
                        quitting_ ? "requested via the tray" : "NOT via the tray - unexplained");
            shutdown();
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hwnd_, msg, wp, lp);
    }
}

// ─── Queue draining (UI thread) ─────────────────────────────────────────────────────────────

void App::drain_hook_queue() {
    for (;;) {
        HookNote n;
        {
            std::lock_guard<std::mutex> lock(hook_mutex_);
            if (hook_queue_.empty()) return;
            n = hook_queue_.front();
            hook_queue_.pop_front();
        }
        switch (n.kind) {
            case HookNote::Kind::Activate:
                begin_dictation();
                break;
            case HookNote::Kind::Finalize:
                end_dictation(kd::LocalEvent::FinalisePressed);
                break;
            case HookNote::Kind::Cancel:
                end_dictation(kd::LocalEvent::EscPressed);
                break;
            case HookNote::Kind::Commit:
                // docs/editing.md §4.3. The same finalise the hotkey's second press produces —
                // `stop`, a last decode, then finish_dictation() flushing the buffer — reached
                // by the gesture that now means it. What is different is upstream: the filter
                // swallowed the Enter, so the target never receives it and the session's text can
                // no longer land after a line break the user meant to come *after* it (§2).
                KD_LOG_INFO("commit: Enter");
                end_dictation(kd::LocalEvent::FinalisePressed);
                break;
            case HookNote::Kind::StopInput:
                // The reason is known here and was being dropped one line later: end_dictation
                // takes a LocalEvent, so the log read "dictation ending: stop_on_input" and left
                // the user's actual gesture unrecorded. "A click stopped it" and "a keystroke
                // stopped it" call for completely different answers - the first is plan.md §1.8
                // working as designed when someone clicks into the window they are dictating
                // into, the second is worth investigating.
                KD_LOG_INFO("stop-on-input: %s", kd::to_string(n.reason));
                end_dictation(kd::LocalEvent::StopOnInput);
                break;
            case HookNote::Kind::Edit:
                apply_edit(n.edit);
                break;
            case HookNote::Kind::EditingBegan:
                begin_editing();
                break;
            case HookNote::Kind::ResumeDictation:
                resume_dictation();
                break;
            case HookNote::Kind::ForeignKey:
                show_foreign_key_hint(n.edit.vk, n.edit.mods);
                break;
            case HookNote::Kind::PillPointer:
                apply_pill_pointer(n.pill);
                break;
            case HookNote::Kind::LeftClick:
                // plan.md §1.5 source B. Cached even when no session is running, because the
                // click that matters is the one BEFORE the hotkey.
                if (anchor_ != nullptr) anchor_->set_last_click({n.x, n.y}, n.window, n.time_ms);
                break;
            case HookNote::Kind::Wheel:
                // Scrolling moves the caret with no click, so the cached point is now a lie.
                if (anchor_ != nullptr) anchor_->invalidate_click();
                break;
            case HookNote::Kind::HookLost:
                KD_LOG_ERROR("keyboard hook was removed by the system; reinstalling");
                if (hotkey_ != nullptr) {
                    hotkey_->stop();
                    hotkey_->set_editing(settings_.editing);
                    hotkey_->start(settings_.hotkey, reinterpret_cast<uintptr_t>(hwnd_), this);
                }
                break;
        }
    }
}

void App::drain_ipc_queue() {
    for (;;) {
        kd::proto::Message m;
        {
            std::lock_guard<std::mutex> lock(ipc_mutex_);
            if (ipc_queue_.empty()) return;
            m = ipc_queue_.front();
            ipc_queue_.pop_front();
        }

        if (const auto* ready = m.get_if<kd::proto::ReadyMsg>()) {
            loaded_model_ = ready->model;
            respawn_attempts_ = 0;
            set_tray_state(kd::OverlayState::Idle);
        } else if (m.get_if<kd::proto::LoadingMsg>() != nullptr) {
            set_tray_state(kd::OverlayState::ModelLoading);
        }

        const kd::SessionMachine::Outcome outcome = session_->on_message(m);
        apply(outcome, &m);

        // §6.2: `stopped` is terminal for the session, and only now is the target free. The
        // verdict is what distinguishes it from a `stopped` for a session we have already moved
        // on from, which §7.1 says to record and apply to nothing.
        // Tell the machine somebody is talking.
        //
        // Both silence timers hang off `last_voice_ms`, and `LocalEvent::SpeechDetected` is its
        // only writer — which nothing in src/ was raising, so it kept the value it was seeded with
        // at `start`. The endpoint timer is additionally guarded by `utterance_open`, set in that
        // same unreachable handler, so it never fired at all; the session timer has no such guard
        // and so counted wall-clock from activation instead of silence. The shipped 120 s became a
        // hard two-minute cap on any dictation, however much the user was saying — which
        // end-of-session cadence makes far easier to hit, since it exists to encourage long
        // sessions.
        //
        // The signal is the sidecar's, not a second detector: it owns the VAD, and a `partial` or
        // `final` carrying text is it reporting speech. Deriving it here costs nothing, cannot
        // disagree with the endpointer that produced the words, and needs no audio analysis on
        // this side. Empty text does not count — that is the backend saying it heard nothing.
        if (outcome.verdict == kd::Verdict::Accept ||
            outcome.verdict == kd::Verdict::AcceptAfterGap) {
            const auto* p = m.get_if<kd::proto::PartialMsg>();
            const auto* f = m.get_if<kd::proto::FinalMsg>();
            if ((p != nullptr && !p->text.empty()) || (f != nullptr && !f->text.empty())) {
                apply(session_->on_local(kd::LocalEvent::SpeechDetected));
            }
        }

        if (m.get_if<kd::proto::StoppedMsg>() != nullptr &&
            outcome.verdict == kd::Verdict::Accept) {
            // The last utterance's `final` arrives moments before `stopped`, so a rewrite fired
            // from it is almost always still in flight right here. Committing now would make the
            // last utterance of every session the one utterance that never gets normalized. The
            // target is already held for exactly this kind of wait, and finalize_deadline_ms_
            // still bounds it — NormalizerConfig::validate caps the timeout well inside the
            // grace, so this can defer the commit but never prevent it.
            if (normalize_pending_id_ != 0) {
                finish_awaiting_normalize_ = true;
                KD_LOG_DEBUG("`stopped` held: normalize #%llu is still in flight",
                             static_cast<unsigned long long>(normalize_pending_id_));
            } else {
                finish_dictation("stopped");
            }
        }
    }
}

void App::drain_audio_queue() {
    std::vector<int16_t> batch;
    {
        std::lock_guard<std::mutex> lock(audio_mutex_);
        batch.swap(audio_queue_);
    }
    if (batch.empty() || ipc_ == nullptr) return;

    // A full queue is fatal by design (docs §9): silently dropping or delaying audio produces
    // mysteriously bad transcription, which is far harder to diagnose than a loud failure.
    if (const kd::Status st = ipc_->send_audio(batch.data(), batch.size()); !st.ok()) {
        KD_LOG_ERROR("audio send failed: %s", st.message().c_str());
        apply(session_->on_local(kd::LocalEvent::AudioQueueFull));
    }
}

// ─── Applying the state machine's decisions ─────────────────────────────────────────────────

void App::apply(const kd::SessionMachine::Outcome& o, const kd::proto::Message* src) {
    // The session machine's own diagnosis, which was being thrown away.
    //
    // `Effect::SurfaceWarning` reaches the overlay with a fixed string, and it is raised from six
    // different places in session.cpp — a §8.2 stability violation with an offset, a refusal, a
    // non-fatal error frame from the child, and others. Without this line all six look identical
    // in the log, and the first real failure was misread because of it: "overlay flagged a
    // committed-prefix violation" was taken for the commit engine's contradiction check when in
    // fact that check never ran, and the abort came from the protocol-level check upstream of it.
    // `o.error` already carries the code, the message and the offset; it just had no reader.
    if (o.error.has_value()) {
        KD_LOG_ERROR("session error: code=%s fatal=%d at_offset=%llu %s",
                     kd::proto::to_string(o.error->code), o.error->fatal ? 1 : 0,
                     static_cast<unsigned long long>(o.error->at_offset), o.error->msg.c_str());
    }

    for (const kd::proto::Message& m : o.to_send) {
        if (ipc_ != nullptr) ipc_->send_control(m);
    }

    for (const kd::Effect e : o.effects) {
        switch (e) {
            case kd::Effect::LogGap:
                KD_LOG_WARN("sequence gap: expected %llu, received %llu",
                            static_cast<unsigned long long>(o.gap_expected_seq),
                            static_cast<unsigned long long>(o.gap_received_seq));
                break;

            case kd::Effect::ResetAnchorConfidence:
                // A gap means we may have missed an injection, so the commit-differencing
                // history no longer describes the document. Guessing from it would be worse
                // than admitting we do not know (plan.md §1.5).
                if (anchor_ != nullptr) anchor_->reset_confidence();
                break;

            case kd::Effect::ResetCommitEngine:
                if (commit_ != nullptr) commit_->reset();
                break;

            case kd::Effect::ApplyText:
                if (src != nullptr) apply_text(*src);
                break;

            case kd::Effect::FlushPendingAudio:
                if (ipc_ != nullptr) ipc_->flush_audio(2000);
                break;

            case kd::Effect::DropQueuedAudio:
                if (ipc_ != nullptr) ipc_->discard_queued_audio();
                break;

            case kd::Effect::SurfaceWarning:
                // One fixed string used to serve all seven raisers of this effect, and it named
                // the rarest of them: a press refused during a model load put "committed text was
                // contradicted" on screen with nothing committed and nothing contradicted. Three
                // incidents in the whole log carry that message and only two of them are the
                // thing it describes — which is the ambiguity main.cpp's `o.error` reader was
                // added to end, one effect too late.
                //
                // `o.error` is the machine's own diagnosis where it built one; `refusal()` is what
                // it set where it did not, and its own comment in session.h says it exists so the
                // user is not left guessing. Neither is a transcript, so both may be logged
                // (util/log.h).
                set_tray_state(kd::OverlayState::Warning);
                if (overlay_ != nullptr) {
                    if (o.error.has_value()) {
                        overlay_->flag_violation(o.error->msg);
                    } else if (session_ != nullptr &&
                               session_->refusal() != kd::InjectStatus::Ok) {
                        overlay_->flag_violation(kd::to_string(session_->refusal()));
                    } else {
                        overlay_->flag_violation("the session was refused");
                    }
                }
                break;

            case kd::Effect::SurfaceFatal:
                set_tray_state(kd::OverlayState::Error);
                break;

            case kd::Effect::KillChild:
                if (ipc_ != nullptr) ipc_->stop();
                break;

            case kd::Effect::RespawnChild:
                // A new child gets a new random pipe name and a new session_id space, so a late
                // message from the dead one can never be mistaken for a live one (§10.3).
                if (respawn_attempts_ < 5) {
                    ++respawn_attempts_;
                    sidecar_respawn_ = true;
                    spawn_sidecar();
                } else {
                    KD_LOG_ERROR("sidecar failed %u times; not restarting again", respawn_attempts_);
                    set_tray_state(kd::OverlayState::Error);
                }
                break;

            case kd::Effect::StartEndpointTimer:
            case kd::Effect::StartSessionTimer:
            case kd::Effect::StopTimers:
                // Both timers live inside SessionMachine and are driven by on_tick(); there is
                // nothing for this thread to arm. They remain distinct effects because plan.md
                // §1.6 is emphatic that they are unrelated, and collapsing them here would be
                // the first step to re-merging them.
                break;
        }
    }

    if (o.verdict == kd::Verdict::Refused) {
        set_tray_state(kd::OverlayState::Unsupported);
        if (overlay_ != nullptr) {
            overlay_->set_state(kd::OverlayState::Unsupported);
            overlay_->show();
        }
    }
}

void App::apply_text(const kd::proto::Message& m) {
    if (commit_ == nullptr) return;
    if (const auto* p = m.get_if<kd::proto::PartialMsg>()) {
        perform_commit(commit_->on_partial(*p));
    } else if (const auto* f = m.get_if<kd::proto::FinalMsg>()) {
        perform_commit(commit_->on_final(*f));
    }
}

// The rectangle source C captures, and whether there is one worth capturing.
//
// With an anchor already in hand it is anchor.h's own box around the insertion point — DPI-scaled,
// clamped to the monitor, and sitting right-of-centre because text grows rightward. With no anchor
// yet (the first commit of a session into a target that exposes no caret) there is nothing to
// centre on, so it falls back to the target window: bigger and slower, but a compact change inside
// it still scores as compact, and one large capture per session beats never bootstrapping at all.
bool App::submit_before_frame() {
    if (!target_.has_value()) return false;

    if (const std::optional<kd::anchor::Anchor> a = anchor_->current(); a.has_value()) {
        const kd::anchor::Rect region = kd::anchor::capture_region_for(
            a->insertion_point, settings_.anchor,
            kd::anchor::monitor_geometry_for(a->insertion_point));
        if (region.empty()) return false;
        anchor_->submit_before_frame(region, target_->top_level);
        return true;
    }

    RECT wr{};
    if (GetWindowRect(reinterpret_cast<HWND>(target_->top_level), &wr) == FALSE) return false;
    const kd::anchor::Rect region{wr.left, wr.top, wr.right, wr.bottom};
    if (region.empty()) return false;
    anchor_->submit_before_frame(region, target_->top_level);
    return true;
}

// The ONLY place the overlay's mode is chosen — startup, a settings change and every anchor all
// come through here, so the three cannot drift apart.
//
// ─── plan.md §1.5's degradation gate is DELIBERATELY NOT APPLIED HERE (2026-08-18) ───────────
//
// `kd::overlay_mode_for()` is that gate. It is written, it is documented in overlay.h and it is
// covered by tests/test_overlay.cpp — and this function does not call it, on purpose, with a
// condition for turning it on written below. That is a dangerous shape in this codebase, which
// is why it is spelled out at this length rather than left to a commit message.
//
// The gate was wired up and then unwired the same day, because wiring it made
// `AnchorConfig::min_confidence` load-bearing and **its default of 0.50 is above the range
// `score_diff` actually produces**. Enforcing it would degrade every commit on every target,
// including ones the estimator gets right to a couple of pixels.
//
// The scorer itself is fine. docs/supported-targets.md §5.9.2.1 has the measurement: across the
// twelve measurable A09 cells, every estimate accurate to within one line height scored
// 0.3742–0.5000 and every inaccurate one scored 0.1858–0.3415, so any threshold in
// (0.3415, 0.3742] classifies all twelve correctly. 0.50 classifies none of them correctly. The
// discrimination is there; the default is simply in the wrong place.
//
// **To turn it on:** lower `AnchorConfig::min_confidence` (src/anchor.h) to about 0.33, then
// replace the line below with `kd::overlay_mode_for(settings_.overlay_mode, latest_anchor_,
// settings_.anchor)` under anchor_mutex_. What is deliberately NOT being done here on one
// application's evidence is picking that number, and the caution has data behind it: adding two
// cells (`S05`/`S06`, the invisible commits) took the margin from 0.080 to 0.033 in one sweep,
// and `S06` at 0.3415 is the nearest false positive. That is open question 7 in
// docs/supported-targets.md §9.
//
// Two related weaknesses are recorded there rather than fixed here, because neither blocks the
// gate: `score_diff`'s proximity term measures horizontal movement in units of line height (so a
// 25-character commit reads as an 11-line-height jump, compressing every score toward the bottom
// of the range), and an estimate with NO incumbent to check against scores kProxNoPrevious 0.85 —
// higher than a corroborated accurate one — which is backwards.
//
// Until then this ships the pre-gate behaviour: the overlay draws at whatever anchor it is handed,
// which is a known defect (docs/supported-targets.md §9, open question 7) and not an oversight.
void App::apply_overlay_mode() {
    if (overlay_ == nullptr) return;
    const kd::OverlayMode want = settings_.overlay_mode;  // see the block above before changing
    if (want != overlay_mode_applied_) {
        // At INFO, not DEBUG: this is a change the user can see, and "why is it a pill today" is
        // a question the log has to be able to answer once the gate is live.
        KD_LOG_INFO("overlay mode -> %s (configured %s; confidence gate not applied — see "
                    "App::apply_overlay_mode)",
                    kd::to_string(want), kd::to_string(settings_.overlay_mode));
        overlay_mode_applied_ = want;
    }
    overlay_->set_mode(want);
}

void App::perform_commit(const kd::CommitEngine::Result& r) {
    if (r.violation.has_value()) {
        // plan.md §1.4: preserve the document. No repair, no backspaces, ever. The incident is
        // logged with both texts and surfaced; it is never silently absorbed.
        KD_LOG_ERROR("committed-prefix violation: %s", r.violation->to_log_line().c_str());
        set_tray_state(kd::OverlayState::Warning);
        if (overlay_ != nullptr) overlay_->flag_violation(r.violation->to_log_line());

        // The violation is still reported and the document is still not repaired — but the rest
        // of the sentence is no longer thrown away with it. `salvage` is the words the rejected
        // hypothesis and the committed text still agree about the position of; appending them
        // leaves the contradicted text exactly where it is and costs the user nothing further.
        //
        // Why this is the client's job rather than the backend's: the client owns the document
        // and cannot assume a backend is well behaved. Asking backends not to contradict
        // themselves would work only for the backends that comply, and the whole point of
        // honour_server_stable_bytes and this check is that compliance is not assumed.
        if (r.salvage.has_value() && !r.salvage->text.empty() && target_.has_value() &&
            kd::validate_target(*target_)) {
            std::string patch;
            if (r.salvage->leading_space) patch.push_back(' ');
            patch += r.salvage->text;
            KD_LOG_WARN("salvaging %zu bytes after the contradiction; the text already in the "
                        "document is left as it is",
                        patch.size());
            const kd::InjectResult ir = injector_->inject(*target_, patch);
            if (ir.status != kd::InjectStatus::Ok) {
                KD_LOG_WARN("salvage injection failed: %s", kd::to_string(ir.status));
            }
        }
        apply(session_->on_local(kd::LocalEvent::StabilityViolation));
        return;
    }

    render_preview(r.pending_text);

    if (!r.op.has_value() || r.op->text.empty()) return;
    if (!target_.has_value()) return;

    // Revalidate immediately before injecting, never once at session start. Focus can move
    // between the hypothesis arriving and this line executing, and injecting into whatever the
    // user has since focused is the failure this check exists to prevent (plan.md §1.7).
    if (!kd::validate_target(*target_)) {
        KD_LOG_WARN("target changed before injection; ending the session with nothing injected");
        end_dictation(kd::LocalEvent::TargetChanged);
        return;
    }

    // plan.md §1.6: "resuming starts a new utterance appended after a space". The engine decides
    // when — it holds back each utterance's trailing whitespace precisely so this space is added
    // exactly once, by the side that knows an utterance boundary happened — and this is the only
    // place that can put it on the wire. Dropping the flag ran every utterance into the one before
    // it: "…the default modelopen src/protocol.h…", "…code point indexthe café bill…".
    std::string text;
    if (r.op->leading_space) text.push_back(' ');
    text += r.op->text;

    // Cadence::EndOfSession (plan.md §1.4): accumulate, do not type. The engine has already
    // decided this text is eligible and the invariant is untouched — deferring can only commit
    // *less*, never more, and never rewrites anything.
    //
    // Nothing anchor-shaped happens here on purpose. Source C detects a commit by pixel
    // differencing around the caret, and source A re-reads the caret "after each commit"; with
    // nothing injected the document has not changed and the caret has not moved, so submitting a
    // before/after frame pair would ask the estimator to find a change that does not exist.
    if (settings_.stability.cadence == kd::Cadence::EndOfSession) {
        // §1's invariant: dictation only ever appends. EditBuffer::append() puts the text at the
        // end and takes the caret with it — which is safe here, and only here, because §3.2
        // closes the microphone at the first edit key, so the only way speech reaches this line
        // is after the user's own resume gesture.
        edit_.append(text);
        // The raw baseline grows in step, and it is what every request sends. normalizer.h: a
        // rewriter is not idempotent, so re-sending its own output makes it keep editing.
        normalize_raw_ += text;
        KD_LOG_DEBUG("deferred %zu bytes (%zu buffered) — cadence is end-of-session",
                     text.size(), edit_.size());
        // At an utterance boundary, which is the one moment in a session when both machines are
        // idle: the endpointer has just fired on silence, so the recogniser is not decoding, and
        // the user has stopped talking. The rewrite lands in the pill during that pause, so by
        // the time they press Enter they have already read what they are committing — which is
        // the whole of docs/editing.md §2, and the reason this needs no extra keystroke.
        request_normalize();
        // Again, because the buffer just grew. Without this the preview empties at every
        // utterance boundary: the call above renders the engine's `pending_text`, which is scoped
        // to the utterance in progress and is empty on a `final` — so everything said so far
        // vanished from the pill the moment the speaker paused, while the buffer behind it was
        // perfectly intact and typed in full at the end. Reported exactly that way.
        render_preview({});
        return;
    }

    // Source C's before/after pair, captured from a rectangle around where we expect the text to
    // land. It used to pass an empty Rect, which fails every capture — "capture region is empty"
    // on every injection — so commit differencing never ran once and the anchor could never do
    // better than the caret or the last click.
    const bool framed = anchor_ != nullptr && submit_before_frame();
    const kd::InjectResult res = injector_->inject(*target_, text);
    if (framed) anchor_->submit_after_frame(text);

    if (res.status != kd::InjectStatus::Ok) {
        // Injection success is decided here, by the mechanism that performed it — never inferred
        // from pixels (plan.md §1.5).
        KD_LOG_ERROR("injection failed: %s", kd::to_string(res.status));
        set_tray_state(res.status == kd::InjectStatus::ElevatedUnsupported
                           ? kd::OverlayState::Unsupported
                           : kd::OverlayState::Error);
        return;
    }

    // Source A again, which anchor.h asks for "at session start **and after each commit**". The
    // caret has just moved — we moved it — and a caret anchor goes stale after
    // AnchorConfig::stale_after_ms (3 s). Refreshing only at session start therefore meant the
    // inline overlay was correct for three seconds and then degraded to the floating pill for the
    // rest of the session, which is exactly what the first person to watch it reported seeing.
    if (anchor_ != nullptr) anchor_->refresh_caret(target_->focus);
}

// ─── The compose buffer (docs/editing.md) ───────────────────────────────────────────────────

bool App::composing() const noexcept {
    // One reader of one answer. `Settings::normalise()` has already forced `editing.enabled`
    // false for any cadence but end-of-session (§6.1), so this does not re-derive that condition
    // — re-deriving it is exactly how the two halves of a rule come to disagree.
    return settings_.editing.enabled && target_.has_value();
}

// The virtual key a keystroke produced, as text, asked of the keyboard layout the *target* is
// using. docs/editing.md §4.2 puts this on the UI thread deliberately: the hook may not call
// into the layout, and the layout is a property of the focused window's thread rather than of
// this one — a user typing into a Greek-layout window must get Greek letters in the pill.
static std::string char_for_key(const kd::EditPost& p) {
    BYTE state[256] = {};
    // Rebuilt from the modifiers the filter recorded at the keystroke rather than read with
    // GetKeyboardState here. This runs some milliseconds later, on a different thread, and by
    // then the user may have let go of Shift — which would silently lower-case the character
    // they typed. The high bit is "down"; the low bit of VK_CAPITAL is the toggle.
    if (p.mods & kd::kModShift) state[VK_SHIFT] = 0x80;
    if (p.mods & kd::kModCtrl) state[VK_CONTROL] = 0x80;
    if (p.mods & kd::kModAlt) state[VK_MENU] = 0x80;
    if (p.caps_lock) state[VK_CAPITAL] = 0x01;

    HKL layout = nullptr;
    if (HWND fg = GetForegroundWindow(); fg != nullptr) {
        layout = GetKeyboardLayout(GetWindowThreadProcessId(fg, nullptr));
    }
    if (layout == nullptr) layout = GetKeyboardLayout(0);

    // The 1<<2 flag means "do not change the kernel keyboard state", which matters more here
    // than anywhere else: without it a dead key typed into the pill would leave the layout's
    // pending-diacritic state set, and the *target* would then receive the composed character
    // the next time the user typed into it.
    wchar_t buf[8] = {};
    const int n = ToUnicodeEx(p.vk, p.scan, state, buf, static_cast<int>(std::size(buf)), 1u << 2,
                              layout);
    if (n <= 0) {
        // n < 0 is a dead key, which has produced no character yet. Dropping it is the honest
        // answer for now: composing a diacritic needs the layout state this call is deliberately
        // not allowed to keep, and a half-composed accent in the buffer would be worse than a
        // key that did nothing. Recorded here rather than in a document nobody reads.
        return {};
    }

    std::string out;
    size_t bad = 0;
    if (!kd::utf8::from_utf16(std::wstring_view(buf, static_cast<size_t>(n)), out, &bad)) {
        KD_LOG_WARN("the keyboard layout produced text this build cannot encode (at %zu)", bad);
        return {};
    }
    // Control characters are what a Ctrl chord produces on most layouts, and the pill has no use
    // for one: Enter commits, Tab is a foreign key, and the rest would be invisible bytes in the
    // user's document.
    if (out.size() == 1 && static_cast<unsigned char>(out[0]) < 0x20) return {};
    return out;
}

void App::apply_edit(const kd::EditPost& p) {
    if (!composing()) return;

    kd::EditCommand c;
    c.op = p.op;
    c.extend = p.shift;

    if (p.op == kd::EditOp::InsertText) {
        if (p.vk == kd::vk::kV && (p.mods & kd::kModCtrl) != 0) {
            // Ctrl+V. EditBuffer is pure and has no clipboard, so the read happens here and the
            // result is handed to it as an ordinary insertion — one insertion path rather than
            // two (edit_buffer.h).
            std::string pasted;
            if (!kd::get_clipboard_text(pasted)) return;
            c.text = std::move(pasted);
        } else {
            c.text = char_for_key(p);
        }
        if (c.text.empty()) return;
    }

    // Ctrl+C and Ctrl+X: the selection has to be read *before* the command is applied, because
    // Cut removes it. Nothing is put on the clipboard when there is no selection — silently
    // clearing what the user had copied would be worse than the key doing nothing.
    if (p.op == kd::EditOp::Copy || p.op == kd::EditOp::Cut) {
        const std::string_view sel = edit_.selected_text();
        if (sel.empty()) return;
        if (!kd::set_clipboard_text(sel)) {
            KD_LOG_WARN("could not put %zu selected bytes on the clipboard", sel.size());
            if (p.op == kd::EditOp::Copy) return;
            // A Cut whose copy failed still removes the text, and that would lose it outright.
            // Refusing the whole command is the recoverable failure.
            return;
        }
    }

    const kd::EditResult r = edit_.apply(c);
    if (!r.changed()) return;

    render_preview({});
}

void App::begin_editing() {
    if (!composing() || editing_) return;

    // The user's text is authoritative from here. A rewrite still in flight would land on top of
    // the correction they just made and discard it silently, which is the one failure mode a
    // compose buffer exists to prevent — so the rewriter is done for this session, not merely
    // paused. It resumes at the next session, which starts from a raw buffer again.
    if (normalize_active_) {
        KD_LOG_DEBUG("normalizer stood down for this session: the user is editing");
        cancel_normalize();
    }

    editing_ = true;
    editing_began_ms_ = clock_.now_ms();

    // docs/editing.md §3.2: **the first edit key stops the capture** — not gates it, not discards
    // its frames; stops it, at the device. audio_capture.h states the rule this obeys: "the mic
    // is open only between start() and stop() — plan.md §1.9 makes that a privacy promise, and
    // the tray state is driven from it, so nothing else may hold the device open." A pill that
    // said *paused* over a lit microphone light would break the one promise the product makes
    // about the microphone.
    if (audio_running_) {
        capture_->stop();
        audio_running_ = false;
    }

    // §3.3, and §16's sixth open question. An utterance that was open when the edit key arrived
    // is flushed rather than abandoned: the user pressed an arrow key to go and fix something
    // they just said, and the thing they just said had better be editable by the time they get
    // there. The session machine raises FlushPendingAudio; the `final` it produces lands in the
    // settled region through the ordinary commit path.
    if (session_ != nullptr) apply(session_->on_local(kd::LocalEvent::EditingBegan));

    KD_LOG_INFO("editing: the microphone is closed and the pill has the keyboard");
    set_tray_state(kd::OverlayState::Editing);
    if (overlay_ != nullptr) overlay_->set_state(kd::OverlayState::Editing);
    push_edit_view();
}

void App::resume_dictation() {
    if (!composing() || !editing_) return;

    // §1's invariant, and note *why* the caret moves: the user has made a gesture that means "I
    // am going to talk now". It is a consequence of an explicit action rather than a microphone
    // surprising them mid-correction, which is the whole difference between this design and the
    // one §14 records as retracted.
    edit_.set_caret(edit_.size(), false);

    kd::audio::CaptureConfig cfg;
    cfg.device_id = settings_.audio_device_id;
    if (const kd::Status st = capture_->start(cfg, this); !st.ok()) {
        // The buffer is intact and the session is still running, so this is recoverable: stay in
        // editing, say so, and let the user press Enter to keep what they have.
        KD_LOG_ERROR("could not reopen the microphone: %s", st.message().c_str());
        set_tray_state(kd::OverlayState::Error);
        if (overlay_ != nullptr) {
            overlay_->set_state(kd::OverlayState::Editing);
            overlay_->set_hint("The microphone could not be reopened — press Enter to keep this");
        }
        hint_expires_ms_ = clock_.now_ms() + kHintMs;
        return;
    }
    audio_running_ = true;
    editing_ = false;

    // §3.3: a stateful backend must not be told that nothing happened. The gap is real — the
    // device was closed for it — and plan.md §1.6 forbids papering over a gap by padding or
    // splicing, which is exactly what saying nothing would amount to.
    const uint64_t now = clock_.now_ms();
    const uint32_t gap = editing_began_ms_ == 0 || now <= editing_began_ms_
                             ? 0u
                             : static_cast<uint32_t>(now - editing_began_ms_);
    if (session_ != nullptr) {
        apply(session_->on_capture_glitch(gap, "editing"));
        apply(session_->on_local(kd::LocalEvent::DictationResumed));
    }
    editing_began_ms_ = 0;

    KD_LOG_INFO("dictation resumed after %u ms of editing", gap);
    set_tray_state(kd::OverlayState::Listening);
    if (overlay_ != nullptr) {
        overlay_->set_state(kd::OverlayState::Listening);
        overlay_->scroll_to_bottom();
        overlay_->clear_hint();
    }
    hint_expires_ms_ = 0;
    push_edit_view();
}

void App::publish_pill_rect() {
    if (hotkey_ == nullptr) return;
    const kd::anchor::Rect r =
        overlay_ != nullptr ? overlay_->pill_rect() : kd::anchor::Rect{};
    hotkey_->set_pill_rect(r.left, r.top, r.right, r.bottom);
}

void App::apply_pill_pointer(const kd::PillPost& p) {
    if (!composing() || !settings_.editing.mouse || overlay_ == nullptr) return;

    if (p.kind == kd::FilterOutcome::PillPointerKind::Scroll) {
        // docs/editing.md §5.2: scrolling is **not** an edit gesture and does not close the
        // microphone. Three lines per notch is the Windows convention and, at eight visible
        // lines, moves the view by a readable amount without jumping past what the user is
        // looking for. A positive delta is away from the user, which scrolls back.
        const int32_t notches = p.wheel / WHEEL_DELTA;
        overlay_->scroll_by_lines(notches != 0 ? -notches * 3 : (p.wheel > 0 ? -3 : 3));
        return;
    }

    if (p.kind == kd::FilterOutcome::PillPointerKind::Up) return;  // the drag simply ends

    if (p.kind == kd::FilterOutcome::PillPointerKind::Down &&
        overlay_->hit_test_mic_button(p.x, p.y)) {
        // §13 stage 3: the control that says what it does. It is the same gesture the hotkey
        // makes, routed to the same place, so there is one resume path rather than two.
        resume_dictation();
        return;
    }

    size_t offset = 0;
    if (!overlay_->hit_test_text(p.x, p.y, offset)) return;

    // §5.2: placing a caret is an edit gesture, so it closes the microphone exactly as the first
    // edit key does. The mouse must not open a second route into editing mode that skips the
    // suspend — that would leave a lit microphone light over a pill claiming to be paused, which
    // is the failure §3.2 exists to prevent.
    if (!editing_) begin_editing();

    kd::EditCommand c;
    c.op = p.kind == kd::FilterOutcome::PillPointerKind::Down ? kd::EditOp::PlaceCaret
                                                              : kd::EditOp::ExtendCaret;
    c.offset = offset;
    if (edit_.apply(c).changed()) render_preview({});
}

void App::show_foreign_key_hint(uint32_t vk, uint32_t mods) {
    if (!composing() || overlay_ == nullptr) return;

    // Named through HotkeySpec so that "Ctrl+S" is spelled here exactly as it is spelled in the
    // settings file and the hotkey dialog. One table, three readers.
    kd::HotkeySpec spec;
    spec.vk = vk;
    spec.mods = mods;
    overlay_->set_hint(spec.to_display_string() + " \xE2\x80\x94 press Enter to finish first");
    hint_expires_ms_ = clock_.now_ms() + kHintMs;
}

// ─── Session lifecycle ──────────────────────────────────────────────────────────────────────

void App::begin_dictation() {
    if (session_ == nullptr) return;

    // Defensive. A buffer that survived into a new session belongs to the *previous* session's
    // target and must never be typed into this one's window. `finish_dictation` is the one path a
    // session ends through and it flushes, so this should never fire — but if it ever does, the
    // text is rescued (document if that target still has focus, clipboard otherwise) rather than
    // silently carried into a stranger's window. Runs before target_ is replaced, deliberately.
    flush_deferred_commits("a new session started while text was still buffered");

    if (finalizing_) {
        // §7.2: one session per connection. The previous one has been asked to stop and its
        // `final` may still be in flight, so starting another now would be a fatal
        // state_violation on the wire. Ignored and said out loud, rather than refused silently.
        KD_LOG_WARN("hotkey pressed while the previous session was still finalising; ignored");
        return;
    }

    // A second press while one is already waiting means "never mind" — the same toggle the hotkey
    // has everywhere else. The filter never learned a session started, so this press arrives as
    // another Activate rather than as a Finalize, and without this it would silently re-arm.
    if (activation_pending_) {
        activation_pending_ = false;
        activation_deadline_ms_ = 0;
        KD_LOG_INFO("queued activation cancelled by a second press");
        set_tray_state(kd::OverlayState::Idle);
        if (overlay_ != nullptr) {
            overlay_->set_state(kd::OverlayState::Idle);
            overlay_->hide();
        }
        return;
    }

    // §10.2: `start` may only be sent from IDLE. The press is held rather than refused, and the
    // microphone is NOT opened — the old code opened it first and asked afterwards, so a press
    // during a model load left the mic running, the overlay saying "Listening" and the log saying
    // "dictation started" for a session that was never started at all.
    if (session_->state() != kd::proto::ConnState::Idle) {
        if (!connection_can_become_idle(session_->state())) {
            KD_LOG_WARN("hotkey pressed while the connection is %s; nothing to wait for",
                        kd::proto::to_string(session_->state()));
            set_tray_state(kd::OverlayState::Error);
            return;
        }
        activation_pending_ = true;
        activation_deadline_ms_ = clock_.now_ms() + kActivationWaitMs;
        KD_LOG_INFO("hotkey pressed while the model is still loading (%s); holding the "
                    "activation for up to %llu ms",
                    kd::proto::to_string(session_->state()),
                    static_cast<unsigned long long>(kActivationWaitMs));
        set_tray_state(kd::OverlayState::ModelLoading);
        if (overlay_ != nullptr) {
            overlay_->set_state(kd::OverlayState::ModelLoading);
            overlay_->show();
        }
        return;
    }

    auto captured = kd::capture_foreground_target();
    if (!captured) {
        KD_LOG_WARN("no usable foreground target: %s", captured.status().message().c_str());
        set_tray_state(kd::OverlayState::Unsupported);
        return;
    }
    target_ = captured.value();

    // The per-application policy, which decides *how* we type into this target and whether we
    // type into it at all. docs/supported-targets.md is where these come from: they are
    // measurements, not guesses, and this is the mechanism the matrix's tiers are declared
    // through. find_app_policy() existed, was tested, and had never been called.
    kd::InjectConfig inject_cfg = settings_.inject;
    if (const kd::AppPolicy* p =
            kd::find_app_policy(settings_.app_policies, target_->process_name)) {
        if (!p->supported) {
            KD_LOG_WARN("policy for %s says unsupported: %s", target_->process_name.c_str(),
                        p->note.empty() ? "(no reason recorded)" : p->note.c_str());
            set_tray_state(kd::OverlayState::Unsupported);
            if (overlay_ != nullptr) {
                overlay_->set_state(kd::OverlayState::Unsupported);
                overlay_->show();
            }
            target_.reset();
            return;
        }
        KD_LOG_INFO("policy for %s: method=%s%s", target_->process_name.c_str(),
                    kd::to_string(p->method), p->use_ctrl_shift_v ? " ctrl+shift+v" : "");
        inject_cfg.method = p->method;
        // injector.cpp picks the paste chord from TargetIdentity::is_console and says in its own
        // comment that a caller applying an AppPolicy override sets it here. A terminal-style
        // paste chord in a non-terminal is exactly what this flag exists to force.
        if (p->use_ctrl_shift_v) target_->is_console = true;
    }
    injector_->set_config(inject_cfg);

    // Decided BEFORE a word is spoken. plan.md §1.7: SendInput into a higher-integrity window is
    // silently ignored and the return value does not identify UIPI as the cause, so discovering
    // this after the fact is impossible — the only honest moment to refuse is now.
    if (const kd::InjectStatus s = injector_->check_supported(*target_); s != kd::InjectStatus::Ok) {
        KD_LOG_WARN("refusing to dictate into this target: %s", kd::to_string(s));
        set_tray_state(kd::OverlayState::Unsupported);
        if (overlay_ != nullptr) {
            overlay_->set_state(kd::OverlayState::Unsupported);
            overlay_->show();
        }
        target_.reset();
        return;
    }

    if (anchor_ != nullptr) anchor_->refresh_caret(target_->focus);

    kd::audio::CaptureConfig cfg;
    cfg.device_id = settings_.audio_device_id;
    if (const kd::Status st = capture_->start(cfg, this); !st.ok()) {
        KD_LOG_ERROR("microphone unavailable: %s", st.message().c_str());
        set_tray_state(kd::OverlayState::Error);
        target_.reset();
        return;
    }
    audio_running_ = true;

    // The filter has to know a session is running before the first key-up of the chord that
    // started it arrives — plan.md §1.8's "ignore key-up events for keys already down at
    // activation", and the switch that turns typing into stop-on-input for the duration.
    hotkey_->note_session_started();

    // The verdict was being computed and dropped — `apply()` returns void, so a `Refused` press
    // fell straight through to "Listening" and "dictation started" with no `start` on the wire.
    // The session machine's refusals (password field, elevated target, secure desktop) all take
    // that path, and so did the model-load press until the check above. §8's shape exactly: one
    // side decides, the other never asks.
    const kd::SessionMachine::Outcome o = session_->on_local(kd::LocalEvent::FinalisePressed);
    apply(o);
    if (o.verdict == kd::Verdict::Refused) {
        KD_LOG_WARN("the session machine refused the activation (%s); closing the microphone",
                    kd::to_string(session_->refusal()));
        capture_->stop();
        audio_running_ = false;
        hotkey_->note_session_ended();
        target_.reset();
        // The tray and the overlay were already set by SurfaceWarning in apply(); leaving them is
        // the point — the refusal is what the user needs to see.
        return;
    }

    // Windows resets its idle timers on *input*, and dictation produces none — see
    // keep_display_awake(). Held from here, released on the one path every session ends through.
    keep_display_awake(true);

    // A session starts dictating, never editing (§1), with an empty buffer and nothing hinted.
    editing_ = false;
    editing_began_ms_ = 0;
    hint_expires_ms_ = 0;
    // Cleared here for the reason note_session_ended() is: the abort paths skip end_dictation
    // entirely, and a baseline left behind would make the next session's first rewrite request
    // carry the previous session's words.
    cancel_normalize();
    if (overlay_ != nullptr) {
        overlay_->clear_hint();
        // The violation mark is persistent by design (plan.md §1.4: the user has to be able to
        // see which sentence in their document may be wrong), but it is persistent *within the
        // session it belongs to*. clear_violation() existed, was tested by nobody and called by
        // nobody, so the mark outlived its session and every later one: a session ended by
        // `target_changed` left "⚠ target_changed" painted on the pill of the next dictation,
        // and of the one after that, until the process was restarted. Reported as "it warns me
        // the target changed when I start a new dictation somewhere else".
        //
        // Cleared here rather than in finish_dictation() precisely so the mark survives the end
        // of its own session — it is still on screen while the user reads what was typed, and it
        // goes when they start dictating again.
        overlay_->clear_violation();
        overlay_->scroll_to_bottom();
        // docs/editing.md §5.1: WS_EX_TRANSPARENT is dropped **only for the duration of a
        // composing session**, so that a click meant for the pill lands on the pill instead of
        // moving the target's caret. WS_EX_NOACTIVATE stays set, which is what keeps the target
        // foreground and `validate_target` passing.
        overlay_->set_click_through(!(composing() && settings_.editing.mouse));
    }
    push_edit_view();

    set_tray_state(kd::OverlayState::Listening);
    if (overlay_ != nullptr) {
        overlay_->set_state(kd::OverlayState::Listening);
        overlay_->show();
    }
    KD_LOG_INFO("dictation started: process=%s pid=%lu password_field=%d console=%d compose=%d",
                target_->process_name.c_str(),
                static_cast<unsigned long>(target_->process_id),
                target_->is_password_field ? 1 : 0, target_->is_console ? 1 : 0,
                composing() ? 1 : 0);
}

// The hotkey (or Esc, or stop-on-input) has ended the *user's* part of the session. The session
// itself is not over: `stop` has only just gone out, and the `final` it asks for — the tail of the
// utterance, the part nothing has committed yet — is still to come.
//
// So the target is deliberately NOT released here. It was released here once, and the effect was
// that every session lost its last words: `final` arrived a few milliseconds later, perform_commit
// found no target and returned, and the text went nowhere. The microphone closes now (nothing more
// will be transcribed), the target closes at `stopped`.
// ─── Keeping the screen alive while the user is only talking ────────────────────────────────
//
// Windows resets its idle timers on *input*. Dictation produces none: the whole point is that the
// user is talking instead of typing, so to the power manager a long utterance is indistinguishable
// from a machine nobody is sitting at. The display blanks mid-sentence, and waking it moves the
// foreground — which plan.md §1.7 reads, correctly, as the target changing, so the next commit
// ends the session and the text goes to the clipboard. The session was lost to the *screen
// blanking*, not to anything the user did, and the log said `target_changed` with no hint that
// the power manager was involved.
//
// ES_DISPLAY_REQUIRED is the narrow request that fixes it, and ES_SYSTEM_REQUIRED goes with it so
// the machine does not suspend under an open microphone. ES_CONTINUOUS makes this a state rather
// than a one-shot poke, so it is set once per session instead of on every tick — and the state is
// **per-thread**, which is why both calls are made from the message-loop thread (begin_dictation
// and finish_dictation are only ever reached from the window procedure). ES_AWAYMODE_REQUIRED is
// deliberately not requested: nothing here needs to keep running with the screen off, and asking
// for it would suppress the user's sleep button.
void App::keep_display_awake(bool on) {
    if (display_awake_ == on) return;
    const EXECUTION_STATE want =
        on ? (ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED) : ES_CONTINUOUS;
    // Returns the previous state, and 0 for failure. Not fatal either way — the worst case is the
    // behaviour this function was written to fix — but it is not allowed to fail silently, or the
    // next lost session would be diagnosed from scratch.
    if (SetThreadExecutionState(want) == 0) {
        KD_LOG_WARN("SetThreadExecutionState failed; the screen may blank mid-dictation");
        return;
    }
    display_awake_ = on;
    KD_LOG_DEBUG("display sleep %s", on ? "inhibited for this session" : "released");
}

void App::end_dictation(kd::LocalEvent why) {
    // Esc while an activation is still waiting for the model cancels the wait. It has to be
    // handled before the guard below, which returns early precisely because none of the things it
    // checks for exist yet: no target was captured and no microphone was opened. Without this the
    // flag would survive Esc and the session would start on its own once the model loaded.
    if (activation_pending_) {
        activation_pending_ = false;
        activation_deadline_ms_ = 0;
        KD_LOG_INFO("queued activation cancelled: %s", kd::to_string(why));
        set_tray_state(kd::OverlayState::Idle);
        if (overlay_ != nullptr) {
            overlay_->set_state(kd::OverlayState::Idle);
            overlay_->hide();
        }
        return;
    }

    if (session_ == nullptr || !target_.has_value() || finalizing_) return;

    KD_LOG_INFO("dictation ending: %s", kd::to_string(why));

    // Esc is the one gesture that means "throw this away" (plan.md §1.8 cancels rather than
    // finalises, so the undecoded tail is dropped too). Under Cadence::EndOfSession that has to
    // discard the accumulated buffer as well, or Esc would type in everything the user just asked
    // to abandon — the exact opposite of what they pressed.
    if (why == kd::LocalEvent::EscPressed && !edit_.empty()) {
        KD_LOG_INFO("discarding %zu deferred bytes: Esc cancels the session", edit_.size());
        edit_.clear();
        cancel_normalize();
    }

    apply(session_->on_local(why));

    if (audio_running_) {
        capture_->stop();
        audio_running_ = false;
    }
    hotkey_->note_session_ended();

    finalizing_ = true;
    // A sidecar that never answers must not strand the target forever: a real model's flush can
    // take a moment, so the grace is generous, but it is finite and it is logged when it expires.
    finalize_deadline_ms_ = clock_.now_ms() + kFinalizeGraceMs;
    set_tray_state(kd::OverlayState::Finalizing);
    if (overlay_ != nullptr) overlay_->set_state(kd::OverlayState::Finalizing);
}

void App::render_preview(std::string_view tail) {
    if (overlay_ == nullptr) return;

    // The overlay's two colours mean "settled" (opaque) and "still moving" (60% alpha), and under
    // a deferred cadence that maps onto something real for the first time: everything already
    // accumulated is settled and merely waiting for a keystroke, while the tail of the current
    // utterance is still being revised. With Cadence::Immediate the settled half is in the
    // document rather than in the overlay, so there is nothing to put in it.
    if (settings_.stability.cadence == kd::Cadence::EndOfSession) {
        if (edit_.empty() || tail.empty()) {
            overlay_->set_text(edit_.text(), tail);
            push_edit_view();
            return;
        }
        // The overlay concatenates its two halves with no separator, and committed text never
        // ends in whitespace (plan.md §1.4 holds it back so `CommitOp::leading_space` can add it
        // exactly once). Without this the preview reads "…the configand restart". Display only:
        // the space that actually reaches the document still comes from `leading_space`, so this
        // cannot double it.
        std::string settled;
        settled.reserve(edit_.size() + 1);
        settled += edit_.text();
        settled.push_back(' ');
        overlay_->set_text(settled, tail);
        push_edit_view();
        return;
    }
    overlay_->set_text(tail, {});
}

// docs/editing.md §7. The caret and the selection are offsets into the string set_text() has just
// been given, so they are pushed immediately after it and never on their own: an offset that
// arrives without the text it indexes is an offset into the previous frame.
// ─── The rewriter (normalizer.h) ────────────────────────────────────────────────────────────

void App::request_normalize() {
    if (normalizer_ == nullptr || normalize_raw_.empty()) return;
    // Belt and braces with Settings::normalise(), which already forces `enabled` off under any
    // other cadence. Stated here too because this is the line that would corrupt a document if
    // the forcing ever regressed: under Cadence::Immediate the text is already typed.
    if (settings_.stability.cadence != kd::Cadence::EndOfSession) return;
    if (editing_) return;

    const uint64_t id = normalizer_->request(normalize_raw_);
    if (id == 0) return;  // disabled, empty, or past max_bytes — each logged by the module
    normalize_pending_id_ = id;
    normalize_active_ = true;
}

void App::cancel_normalize() {
    if (normalizer_ != nullptr) normalizer_->cancel_all();
    normalize_pending_id_ = 0;
    normalize_active_ = false;
    finish_awaiting_normalize_ = false;
    normalize_raw_.clear();
}

void App::drain_normalize_queue() {
    std::vector<kd::Normalizer::Result> batch;
    {
        std::lock_guard<std::mutex> lock(normalize_mutex_);
        batch.swap(normalize_queue_);
    }

    for (kd::Normalizer::Result& r : batch) {
        // Everything below is a reason to leave the raw text alone. The user keeps their words in
        // every one of them; that is the whole failure policy (normalizer.h).
        if (r.id != normalize_pending_id_) {
            KD_LOG_DEBUG("ignoring normalize #%llu: superseded",
                         static_cast<unsigned long long>(r.id));
            continue;
        }
        normalize_pending_id_ = 0;

        if (!r.status.ok()) {
            KD_LOG_WARN("normalize #%llu failed after %llu ms (%s); the text stands as spoken",
                        static_cast<unsigned long long>(r.id),
                        static_cast<unsigned long long>(r.elapsed_ms),
                        r.status.to_string().c_str());
        } else if (editing_ || !composing()) {
            // The user started editing, or the session ended, between the request and the
            // answer. Their text wins.
            KD_LOG_DEBUG("dropping normalize #%llu: the buffer is no longer ours to rewrite",
                         static_cast<unsigned long long>(r.id));
        } else if (r.raw != normalize_raw_) {
            // More speech landed while this was in flight. A newer request is already out for the
            // longer buffer, and applying this one would drop the words that arrived since.
            KD_LOG_DEBUG("dropping normalize #%llu: the buffer grew while it was in flight",
                         static_cast<unsigned long long>(r.id));
        } else {
            const kd::GuardResult g =
                kd::guard_verdict(r.raw, r.normalized, settings_.normalizer.protect_terms);
            if (!g.accepted()) {
                // Named, not counted. bench/metrics.py:675 predicted `sm 120` for `sm_120`
                // before this model existed, and a log line that says which term died is the
                // difference between tuning the term table and guessing at it.
                KD_LOG_WARN("normalize #%llu rejected (%s%s%s); the text stands as spoken",
                            static_cast<unsigned long long>(r.id), kd::to_string(g.verdict),
                            g.lost_term.empty() ? "" : ": ", g.lost_term.c_str());
            } else {
                KD_LOG_INFO("normalize #%llu applied in %llu ms: %zu -> %zu bytes",
                            static_cast<unsigned long long>(r.id),
                            static_cast<unsigned long long>(r.elapsed_ms), r.raw.size(),
                            r.normalized.size());
                // The pill's text is replaced wholesale; `normalize_raw_` is deliberately left
                // as it was, so the next utterance extends the raw baseline and the next request
                // still sends text the model has never seen.
                edit_.clear();
                edit_.append(r.normalized);
                render_preview({});
            }
        }

        // Whatever the verdict, a session that was waiting on this answer may now end.
        if (finish_awaiting_normalize_) {
            finish_awaiting_normalize_ = false;
            finish_dictation("stopped");
        }
    }
}

void App::push_edit_view() {
    if (overlay_ == nullptr) return;
    overlay_->set_edit_view(composing() && editing_, edit_.caret(), edit_.selection_begin(),
                            edit_.selection_end());
    // §7.5's legend, from the same funnel and for the same reason: both of its inputs follow the
    // session (the binding can be re-bound while one is not running, and `composing()` needs a
    // target), and the overlay ignores a push that changes nothing. Which keys the line names is
    // the overlay's decision from the state it is painting — this only supplies the two facts it
    // cannot see.
    overlay_->set_key_legend(settings_.hotkey.to_display_string(), composing());
}

void App::flush_deferred_commits(const char* why) {
    if (edit_.empty()) return;
    // Taken first, so no path can flush the same text twice. EditBuffer::take() moves the text
    // out and leaves the buffer empty in one step, which is the same rule the previous
    // `text.swap(deferred_)` enforced and the reason it is a single call rather than a read
    // followed by a clear.
    const std::string text = edit_.take();

    // The target has to be revalidated here for the same reason perform_commit revalidates:
    // SendInput types into whatever is focused *now*, not into the window we were aiming at.
    // `validate_target` requires that window to still be the foreground one, so once focus has
    // moved there is no way to put the text where it was meant to go.
    if (target_.has_value() && kd::validate_target(*target_)) {
        const kd::InjectResult res = injector_->inject(*target_, text);
        if (res.status == kd::InjectStatus::Ok) {
            KD_LOG_INFO("flushed %zu deferred bytes on %s", text.size(), why);
            if (anchor_ != nullptr) anchor_->refresh_caret(target_->focus);
            return;
        }
        KD_LOG_ERROR("deferred flush failed to inject: %s", kd::to_string(res.status));
    } else {
        KD_LOG_WARN("target is gone at flush time (%s); the document cannot receive the text",
                    why);
    }

    // Whatever went wrong, a whole session of dictation is in hand and there is nowhere to type
    // it. Dropping it silently is the one unacceptable outcome — the longer the buffer, the more
    // it costs — so it goes to the clipboard and the tray says so. Deliberately NOT restored
    // afterwards, unlike the clipboard *injection* path: the point is for the user to be able to
    // paste it, which means it has to stay.
    if (kd::set_clipboard_text(text)) {
        KD_LOG_WARN("%zu bytes placed on the clipboard instead; press Ctrl+V to recover them",
                    text.size());
        set_tray_state(kd::OverlayState::Warning);
    } else {
        KD_LOG_ERROR("could not reach the clipboard either; %zu bytes are lost", text.size());
        set_tray_state(kd::OverlayState::Error);
    }
}

// The session is over: `stopped` arrived, the child went away, or the grace expired.
void App::finish_dictation(const char* why) {
    if (!finalizing_ && !target_.has_value()) return;

    KD_LOG_INFO("dictation finished: %s", why);

    // Before target_ is cleared below, and before the overlay is torn down: this is the one path
    // every session ends through, so it is the one place a deferred buffer is guaranteed to be
    // dealt with rather than dropped.
    flush_deferred_commits(why);
    finalizing_ = false;
    finalize_deadline_ms_ = 0;

    // Tell the input filter, here, on the ONE path every session ends through.
    //
    // It was told only in end_dictation(), which the abort paths never reach — a §8.2 stability
    // violation goes perform_commit -> StabilityViolation -> the session machine -> here, and
    // end_dictation is skipped entirely. The filter was then left believing a session was still
    // running, permanently, with two visible consequences: every subsequent keystroke posted a
    // stop-on-input for a session that did not exist, and **the hotkey stopped working** —
    // `chord_matches` reads `session_active_` and sends `toggle_finalize` instead of `activate`,
    // so Win+Y silently finalised nothing rather than starting a session. The only way out was to
    // restart the process. Reported as "Win+Y no longer works" after the first abort.
    //
    // Safe to call twice: it clears a flag and some key state, and end_dictation still calls it
    // at the moment the microphone closes, which is earlier and is what the user's gesture means.
    if (hotkey_ != nullptr) hotkey_->note_session_ended();
    // Drop any per-application override, so the next session starts from the user's own settings
    // rather than from whatever the last target happened to need.
    if (injector_ != nullptr) injector_->set_config(settings_.inject);
    // Released here for the reason note_session_ended() is here: the abort paths skip
    // end_dictation entirely, and a display request left outstanding would keep the user's
    // screen awake for the rest of the process's life.
    keep_display_awake(false);
    if (audio_running_) {
        capture_->stop();
        audio_running_ = false;
    }
    target_.reset();
    // Cleared on the ONE path every session ends through, for the reason the note_session_ended()
    // call above is here rather than in end_dictation(): the abort paths skip end_dictation
    // entirely, and an `editing_` left true would make the next session's first speech look like
    // a resume and its hotkey press mean the wrong thing. progress.md §8 is a catalogue of state
    // cleared on one of the two paths a session can end through.
    editing_ = false;
    editing_began_ms_ = 0;
    hint_expires_ms_ = 0;
    if (overlay_ != nullptr) {
        overlay_->clear_hint();
        overlay_->set_edit_view(false, 0, 0, 0);
        overlay_->scroll_to_bottom();
        overlay_->set_text({}, {});
        // docs/editing.md §5.3, and it is here rather than in end_dictation() for the reason the
        // note_session_ended() call above is: the abort paths skip end_dictation entirely. A pill
        // that stays non-transparent after a session eats clicks meant for the document, forever,
        // and the user's only recovery is to restart the process — the same shape as the
        // `note_session_ended()` bug in progress.md §8, and the one this file has already been
        // bitten by once.
        overlay_->set_click_through(true);
        overlay_->hide();
    }
    // The pill is gone, so the hook must stop believing there is one to click inside. Without
    // this the last session's rectangle would keep absorbing clicks from the mouse filter's
    // point of view — the session would not end on a click in that region.
    publish_pill_rect();
    set_tray_state(kd::OverlayState::Idle);
}

void App::on_conn_state(kd::proto::ConnState s) {
    // IpcClient walks the transport (docs §2.4: connect, validate the peer); SessionMachine is the
    // authority on the protocol (§5, §10.3) and has to see the same walk, or `hello_ack` arrives
    // while it still believes it is SPAWNING and it kills the connection as a state violation.
    // The machine's Outcome carries `hello` — this is the only thing that puts that frame on the
    // wire, and IpcClient deliberately no longer writes one of its own.
    switch (s) {
        case kd::proto::ConnState::Validating:
            if (session_ != nullptr && !sidecar_respawn_) {
                apply(session_->on_transport(kd::TransportEvent::Connected));
            }
            break;

        case kd::proto::ConnState::Handshake:
            if (session_ != nullptr) {
                // §10.3 step 5: after a respawn the machine is in DEAD, not VALIDATING, and
                // ChildRespawned is the event that takes it back to HANDSHAKE — collapsing
                // SPAWNING/VALIDATING, clearing the dead session, and keeping the session_id
                // counter, which is the whole point of fixture 020.
                apply(session_->on_transport(sidecar_respawn_
                                                 ? kd::TransportEvent::ChildRespawned
                                                 : kd::TransportEvent::PeerValidated));
                sidecar_respawn_ = false;
            }
            break;

        case kd::proto::ConnState::Idle:
            if (tray_state_ == kd::OverlayState::ModelLoading) set_tray_state(kd::OverlayState::Idle);
            // A held activation is deliberately NOT started from here. This is IpcClient's view
            // of the transport, and SessionMachine reaches IDLE only when it drains the `ready`
            // *frame*, which arrives on a different queue (kMsgIpcQueue) than this notification
            // (kMsgIpcState). Starting here would race that: on the losing order the machine is
            // still LOADING, begin_dictation() re-queues the press, and nothing ever posts this
            // state again — a hotkey that silently does nothing, permanently. The tick polls the
            // machine's own state instead, which cannot disagree with itself. Costs one 50 ms tick.
            break;

        default:
            break;
    }
}

void App::on_sidecar_lost() {
    if (quitting_) return;
    KD_LOG_WARN("sidecar connection ended");
    if (activation_pending_) {
        // A respawn reaches IDLE again and would honour the held press seconds after a crash the
        // user was never told about. A lost sidecar is something to report, not to quietly wait
        // out; pressing the hotkey again once the tray recovers is one keystroke.
        activation_pending_ = false;
        activation_deadline_ms_ = 0;
        KD_LOG_WARN("queued activation dropped: the sidecar was lost before the model was ready");
    }
    if (target_.has_value() && !finalizing_ && hotkey_ != nullptr) hotkey_->note_session_ended();
    // Committed text stays in the document — it was correct when it was written and the
    // invariant forbids taking it back. Only the uncommitted tail is lost.
    finish_dictation("sidecar connection ended");
    apply(session_->on_transport(kd::TransportEvent::BrokenPipe));
}

// ─── Settings ───────────────────────────────────────────────────────────────────────────────

void App::show_settings() {
    // The dialog runs a nested message loop, so this message can arrive again while it is up —
    // from the tray, or from a second launch of the executable, which posts it here rather than
    // exiting silently. One dialog is the answer to all of them.
    if (settings_open_) {
        if (HWND owned = GetLastActivePopup(hwnd_); owned != nullptr && owned != hwnd_) {
            SetForegroundWindow(owned);
        }
        return;
    }

    settings_open_ = true;
    kd::Settings edited = settings_;
    const bool accepted = kd::show_settings_dialog(reinterpret_cast<uintptr_t>(instance_),
                                                   reinterpret_cast<uintptr_t>(hwnd_), edited,
                                                   manifest_);
    settings_open_ = false;
    if (accepted) apply_settings(std::move(edited));
}

void App::apply_settings(kd::Settings next) {
    // The dialog documents that it returns a normalised, validated copy. Checking anyway is two
    // lines and the alternative is writing a settings file this build would refuse to load.
    next.normalise();
    if (const kd::Status st = next.validate(); !st.ok()) {
        MessageBoxW(hwnd_, widen(st.message()).c_str(), L"kvit-dictate — settings",
                    MB_OK | MB_ICONERROR);
        return;
    }

    const std::string previous_model = settings_.model_id;
    const bool diagnostics_file_changed =
        next.diagnostics_file_enabled != settings_.diagnostics_file_enabled;
    settings_ = std::move(next);

    if (const kd::Status st = kd::save_settings(settings_, kd::default_settings_path()); !st.ok()) {
        // Applied in memory but not persisted: say so rather than letting the next launch quietly
        // revert to the old file.
        KD_LOG_ERROR("settings could not be saved: %s", st.message().c_str());
        MessageBoxW(hwnd_,
                    L"These settings are in effect now but could not be written to disk, so they "
                    L"will be lost when kvit-dictate restarts.",
                    L"kvit-dictate — settings", MB_OK | MB_ICONWARNING);
    }

    kd::log::set_level(settings_.log_level);
    kd::log::set_transcript_policy(settings_.transcript);
    if (diagnostics_file_changed) {
        // log::init is documented as "once, early". The level and the transcript policy are
        // runtime switches; opening or closing the diagnostics file is not, and pretending it took
        // effect would be worse than saying it did not.
        MessageBoxW(hwnd_,
                    L"The diagnostics-file setting takes effect the next time kvit-dictate starts.",
                    L"kvit-dictate — settings", MB_OK | MB_ICONINFORMATION);
    }

    // Everything below is applied unconditionally rather than only when it changed: each call is
    // idempotent and cheap next to the dialog the user just closed, and a "did it change" test per
    // field is one more place for a field to be forgotten.
    if (injector_ != nullptr) injector_->set_config(settings_.inject);
    if (normalizer_ != nullptr) normalizer_->set_config(settings_.normalizer);
    if (commit_ != nullptr) commit_->set_params(settings_.stability);
    if (session_ != nullptr) session_->set_config(settings_.session);
    if (overlay_ != nullptr) {
        overlay_->set_style(settings_.overlay_style);
        // Re-derived, not copied: the user may have changed overlay_mode, min_confidence or
        // below_threshold in the dialog that just closed, and all three feed this.
        apply_overlay_mode();
    }

    if (hotkey_ != nullptr) {
        hotkey_->stop();
        hotkey_->set_editing(settings_.editing);
        if (const kd::Status st = hotkey_->start(settings_.hotkey,
                                                 reinterpret_cast<uintptr_t>(hwnd_), this);
            !st.ok()) {
            MessageBoxW(hwnd_,
                        L"The new hotkey could not be installed. Another program may already own "
                        L"it. The previous one is no longer active either.",
                        L"kvit-dictate", MB_OK | MB_ICONWARNING);
        }
    }

    // The anchor estimator takes its configuration at construction, so it is rebuilt. It holds no
    // session state — the caret cache and the click cache are both refreshed at the next
    // activation — so this is cheaper than it looks.
    if (anchor_ != nullptr) {
        anchor_->stop();
        anchor_ = std::make_unique<kd::anchor::AnchorEstimator>(
            settings_.anchor, screen_capture_.get(), caret_provider_.get(), this);
        if (const kd::Status st = anchor_->start(); !st.ok()) {
            KD_LOG_WARN("anchor estimator did not restart (%s)", st.message().c_str());
        }
    }

    if (settings_.model_id != previous_model) {
        // plan.md §1.1: a model change is a new child process, never a swap in place. Stopping the
        // connection is enough to cause one — the transport reports the loss, the state machine
        // asks for a respawn, and spawn_sidecar() reads the model id we have just written.
        KD_LOG_INFO("model changed: %s -> %s", previous_model.c_str(), settings_.model_id.c_str());
        respawn_attempts_ = 0;  // a deliberate change is not one of the five failures
        loaded_model_.clear();
        set_tray_state(kd::OverlayState::ModelLoading);
        if (ipc_ != nullptr) ipc_->stop();
    }
}

// ─── Tray ───────────────────────────────────────────────────────────────────────────────────

void App::set_tray_state(kd::OverlayState s) {
    if (tray_state_ == s) return;
    tray_state_ = s;
    update_tray();
}

void App::update_tray() {
    if (!tray_added_) return;

    // plan.md §1.6: the microphone stays open across long pauses, so "am I being listened to"
    // must never require inference. Every state gets its own words.
    const wchar_t* text = L"kvit-dictate";
    switch (tray_state_) {
        case kd::OverlayState::Idle:         text = L"kvit-dictate — idle (mic closed)"; break;
        case kd::OverlayState::Listening:    text = L"kvit-dictate — LISTENING (mic open)"; break;
        case kd::OverlayState::Speaking:     text = L"kvit-dictate — listening, speech detected"; break;
        case kd::OverlayState::Finalizing:   text = L"kvit-dictate — finalising"; break;
        case kd::OverlayState::ModelLoading: text = L"kvit-dictate — loading model"; break;
        case kd::OverlayState::Warning:      text = L"kvit-dictate — WARNING: a correction could not be applied"; break;
        // docs/editing.md §7.4 and §16's fifth bullet: a state the pill renders and the tray
        // does not is `Effect::SurfaceWarning`'s shape, where one fixed string served seven
        // raisers and nobody noticed for months. The microphone is closed in this state and the
        // tray is where the user checks that.
        case kd::OverlayState::Editing:      text = L"kvit-dictate — editing (mic CLOSED)"; break;
        case kd::OverlayState::Unsupported:  text = L"kvit-dictate — this window cannot be dictated into"; break;
        case kd::OverlayState::Error:        text = L"kvit-dictate — ERROR (see log)"; break;
    }

    std::wstring tip = text;
    if (!loaded_model_.empty()) tip += L"\n" + widen(loaded_model_);

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_TIP | NIF_ICON;
    HICON drawn = reinterpret_cast<HICON>(kd::make_tray_icon(tray_state_, tray_icon_dark_));
    nid.hIcon = drawn != nullptr
                    ? drawn
                    : LoadIcon(nullptr, tray_state_ == kd::OverlayState::Error ||
                                                tray_state_ == kd::OverlayState::Warning
                                            ? IDI_WARNING
                                            : IDI_APPLICATION);
    lstrcpynW(nid.szTip, tip.c_str(), ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &nid);

    // Only now. The shell has been given the new icon, so the one it was showing until this line
    // is no longer in use — destroying it any earlier is a handle the notification area is still
    // drawing from, which presents as an icon that vanishes at a state change.
    if (drawn != nullptr) {
        if (tray_icon_ != nullptr) DestroyIcon(tray_icon_);
        tray_icon_ = drawn;
    }
}

// Has the *taskbar* changed scheme under us?
//
// Polled rather than pushed, and the reason is structural: WM_SETTINGCHANGE is broadcast to
// top-level windows, and this program's window is message-only (see startup()), so it never
// arrives here. The overlay is top-level and does get it — that is how the pill follows the
// scheme — but the tray icon belongs to this file, and routing a message through the overlay to
// reach it would make the pill responsible for something it knows nothing about.
//
// So: one registry read every kTrayThemePollTicks ticks. At 50 ms a tick that is once every two
// seconds, against a poll that already runs the anchor, the session machine and the commit
// engine on every one of them.
void App::poll_taskbar_theme() {
    const bool dark = kd::theme::taskbar_prefers_dark();
    if (dark == tray_icon_dark_) return;
    tray_icon_dark_ = dark;
    KD_LOG_INFO("the taskbar is now %s; redrawing the tray icon", dark ? "dark" : "light");
    update_tray();
}

void App::show_tray_menu() {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;
    AppendMenuW(menu, MF_STRING, kMenuToggle,
                target_.has_value() ? L"Stop dictation" : L"Start dictation");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (!loaded_model_.empty()) {
        AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, (L"Model: " + widen(loaded_model_)).c_str());
    } else {
        AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, L"Model: not loaded");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuSettings, L"Settings…");
    AppendMenuW(menu, MF_STRING, kMenuOpenLogs, L"Open settings folder");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuit, L"Quit");

    POINT pt{};
    GetCursorPos(&pt);
    // Required by TrackPopupMenu on a tray menu, or the menu will not dismiss when the user
    // clicks elsewhere — a documented quirk, not superstition.
    SetForegroundWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// ─── Shutdown ───────────────────────────────────────────────────────────────────────────────

void App::shutdown() {
    static bool done = false;
    if (done) return;
    done = true;

    KillTimer(hwnd_, kTickTimer);
    if (hotkey_ != nullptr) hotkey_->stop();
    if (audio_running_ && capture_ != nullptr) capture_->stop();
    if (anchor_ != nullptr) anchor_->stop();
    if (ipc_ != nullptr) ipc_->stop();  // the job object kills the child even if it ignores us
    if (overlay_ != nullptr) overlay_->destroy();

    if (tray_added_) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = kTrayId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        tray_added_ = false;
    }
    if (tray_icon_ != nullptr) {
        // After NIM_DELETE, for the same reason update_tray() destroys the old icon last: while
        // the notification area still has an entry, it still has this handle.
        DestroyIcon(tray_icon_);
        tray_icon_ = nullptr;
    }

    kd::log::shutdown();
    if (instance_mutex_ != nullptr) {
        ReleaseMutex(instance_mutex_);
        CloseHandle(instance_mutex_);
        instance_mutex_ = nullptr;
    }
}

}  // namespace

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance,
                      _In_ LPWSTR lpCmdLine, _In_ int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    // Before any window exists. Captured rectangles are physical pixels and the target may be on
    // a different monitor at a different scale, so a process that is not per-monitor-v2 aware
    // gets silently virtualised coordinates and places the overlay in the wrong place — which
    // looks like an anchor bug and is not one. This comes from user32, not shcore's v1-only enum.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) return 1;

    int rc = 0;
    {
        App app(hInstance);
        g_app = &app;
        if (app.startup()) {
            rc = app.run();
        } else {
            app.shutdown();
        }
        g_app = nullptr;
    }

    CoUninitialize();
    return rc;
}
