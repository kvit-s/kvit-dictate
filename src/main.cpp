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
#include "injector.h"
#include "ipc_client.h"
#include "overlay.h"
#include "protocol.h"
#include "session.h"
#include "settings.h"
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

constexpr UINT_PTR kTickTimer = 1;
constexpr UINT kTickIntervalMs = 50;  // drives the two silence timers and the commit cadence

// How long the target is held after `stop` while waiting for `final` and `stopped`. Generous
// because a real model's flush is not instant, finite because a child that never answers must not
// strand the session forever.
constexpr uint64_t kFinalizeGraceMs = 5000;

constexpr UINT kTrayId = 1;
constexpr UINT kMenuToggle = 100;
constexpr UINT kMenuSettings = 101;
constexpr UINT kMenuOpenLogs = 102;
constexpr UINT kMenuQuit = 103;

const wchar_t kWindowClass[] = L"kvit-dictate-message-window";
const wchar_t kSingleInstanceMutex[] = L"Local\\kvit-dictate-single-instance";

// A queued hotkey/mouse notification from the hook thread.
struct HookNote {
    enum class Kind : uint8_t { Activate, Finalize, Cancel, StopInput, LeftClick, Wheel, HookLost };
    Kind kind = Kind::Activate;
    kd::StopInputReason reason = kd::StopInputReason::Keystroke;
    int32_t x = 0;
    int32_t y = 0;
    uintptr_t window = 0;
    uint64_t time_ms = 0;
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

    bool spawn_sidecar();
    void on_conn_state(kd::proto::ConnState s);
    void on_sidecar_lost();

    void show_settings();
    void apply_settings(kd::Settings next);

    void set_tray_state(kd::OverlayState s);
    void update_tray();
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
    kd::OverlayState tray_state_ = kd::OverlayState::Idle;
    // What apply_overlay_mode() last pushed to the overlay, so the transition is logged
    // once rather than on every tick that carries an anchor.
    kd::OverlayMode overlay_mode_applied_ = kd::OverlayMode::Hidden;
    std::string loaded_model_;
    bool audio_running_ = false;
    unsigned respawn_attempts_ = 0;
    bool sidecar_respawn_ = false;  // this spawn is §10.3's recovery, not the first launch
    bool settings_open_ = false;
    bool finalizing_ = false;       // `stop` is out; the target is held until `stopped`
    uint64_t finalize_deadline_ms_ = 0;

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

    if (const kd::Status st = hotkey_->start(settings_.hotkey,
                                             reinterpret_cast<uintptr_t>(hwnd_), this);
        !st.ok()) {
        MessageBoxW(hwnd_,
                    L"The global hotkey could not be installed. Another program may already own "
                    L"it, or a security product may be blocking low-level keyboard hooks.",
                    L"kvit-dictate", MB_OK | MB_ICONWARNING);
    }

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kMsgTray;
    nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
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
                    KD_LOG_WARN("no `stopped` within %llu ms of `stop`; releasing the target",
                                static_cast<unsigned long long>(kFinalizeGraceMs));
                    finish_dictation("finalize grace expired");
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
                            end_dictation(kd::LocalEvent::HotkeyPressed);
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
                end_dictation(kd::LocalEvent::HotkeyPressed);
                break;
            case HookNote::Kind::Cancel:
                end_dictation(kd::LocalEvent::EscPressed);
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
        if (m.get_if<kd::proto::StoppedMsg>() != nullptr &&
            outcome.verdict == kd::Verdict::Accept) {
            finish_dictation("stopped");
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
                set_tray_state(kd::OverlayState::Warning);
                if (overlay_ != nullptr) overlay_->flag_violation("committed text was contradicted");
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
        apply(session_->on_local(kd::LocalEvent::StabilityViolation));
        return;
    }

    if (overlay_ != nullptr) {
        overlay_->set_text(r.pending_text, {});
    }

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

// ─── Session lifecycle ──────────────────────────────────────────────────────────────────────

void App::begin_dictation() {
    if (session_ == nullptr) return;
    if (finalizing_) {
        // §7.2: one session per connection. The previous one has been asked to stop and its
        // `final` may still be in flight, so starting another now would be a fatal
        // state_violation on the wire. Ignored and said out loud, rather than refused silently.
        KD_LOG_WARN("hotkey pressed while the previous session was still finalising; ignored");
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

    apply(session_->on_local(kd::LocalEvent::HotkeyPressed));
    set_tray_state(kd::OverlayState::Listening);
    if (overlay_ != nullptr) {
        overlay_->set_state(kd::OverlayState::Listening);
        overlay_->show();
    }
    KD_LOG_INFO("dictation started: process=%s pid=%lu password_field=%d console=%d",
                target_->process_name.c_str(),
                static_cast<unsigned long>(target_->process_id),
                target_->is_password_field ? 1 : 0, target_->is_console ? 1 : 0);
}

// The hotkey (or Esc, or stop-on-input) has ended the *user's* part of the session. The session
// itself is not over: `stop` has only just gone out, and the `final` it asks for — the tail of the
// utterance, the part nothing has committed yet — is still to come.
//
// So the target is deliberately NOT released here. It was released here once, and the effect was
// that every session lost its last words: `final` arrived a few milliseconds later, perform_commit
// found no target and returned, and the text went nowhere. The microphone closes now (nothing more
// will be transcribed), the target closes at `stopped`.
void App::end_dictation(kd::LocalEvent why) {
    if (session_ == nullptr || !target_.has_value() || finalizing_) return;

    KD_LOG_INFO("dictation ending: %s", kd::to_string(why));
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

// The session is over: `stopped` arrived, the child went away, or the grace expired.
void App::finish_dictation(const char* why) {
    if (!finalizing_ && !target_.has_value()) return;

    KD_LOG_INFO("dictation finished: %s", why);
    finalizing_ = false;
    finalize_deadline_ms_ = 0;
    // Drop any per-application override, so the next session starts from the user's own settings
    // rather than from whatever the last target happened to need.
    if (injector_ != nullptr) injector_->set_config(settings_.inject);
    if (audio_running_) {
        capture_->stop();
        audio_running_ = false;
    }
    target_.reset();
    if (overlay_ != nullptr) {
        overlay_->set_text({}, {});
        overlay_->hide();
    }
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
            break;

        default:
            break;
    }
}

void App::on_sidecar_lost() {
    if (quitting_) return;
    KD_LOG_WARN("sidecar connection ended");
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
    nid.hIcon = LoadIcon(nullptr, tray_state_ == kd::OverlayState::Error ||
                                          tray_state_ == kd::OverlayState::Warning
                                      ? IDI_WARNING
                                      : IDI_APPLICATION);
    lstrcpynW(nid.szTip, tip.c_str(), ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
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
