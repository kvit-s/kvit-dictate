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

    void begin_dictation();
    void end_dictation(kd::LocalEvent why);

    bool spawn_sidecar();
    void on_sidecar_lost();

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
    std::string loaded_model_;
    bool audio_running_ = false;
    unsigned respawn_attempts_ = 0;

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
        if (HWND existing = FindWindowW(kWindowClass, nullptr)) {
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
        overlay_->set_mode(settings_.overlay_mode);
    }

    screen_capture_ = kd::anchor::make_bitblt_capture(true);
    caret_provider_ = kd::anchor::make_gui_thread_caret_provider();
    anchor_ = std::make_unique<kd::anchor::AnchorEstimator>(settings_.anchor, screen_capture_.get(),
                                                           caret_provider_.get(), this);
    if (const kd::Status st = anchor_->start(); !st.ok()) {
        KD_LOG_WARN("anchor estimator did not start (%s); the overlay will use its fallback",
                    st.message().c_str());
    }

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
                const auto state = static_cast<kd::proto::ConnState>(lp);
                if (state == kd::proto::ConnState::Idle && tray_state_ == kd::OverlayState::ModelLoading) {
                    set_tray_state(kd::OverlayState::Idle);
                }
            }
            return 0;

        case WM_TIMER:
            if (wp == kTickTimer) {
                if (session_ != nullptr) apply(session_->on_tick());
                if (commit_ != nullptr) perform_commit(commit_->on_tick());
                if (overlay_ != nullptr && anchor_dirty_.exchange(false, std::memory_order_acq_rel)) {
                    std::lock_guard<std::mutex> lock(anchor_mutex_);
                    overlay_->set_anchor(latest_anchor_);
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
                    quitting_ = true;
                    DestroyWindow(hwnd_);
                    return 0;
                default:
                    return 0;
            }

        case kMsgShowSettings:
            // The settings dialog lives in settings.cpp; until it is surfaced here the tray still
            // points at the file, which is editable and documented, rather than doing nothing.
            ShellExecuteW(nullptr, L"open", kd::default_settings_path().wstring().c_str(), nullptr,
                      nullptr, SW_SHOWNORMAL);
            return 0;

        case WM_QUERYENDSESSION:
            // Log-off or shutdown. Release the microphone and kill the child now: a sidecar left
            // holding VRAM through a shutdown is exactly what the job object exists to prevent.
            shutdown();
            return TRUE;

        case WM_ENDSESSION:
            shutdown();
            return 0;

        case WM_DESTROY:
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

        apply(session_->on_message(m), &m);
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

    if (anchor_ != nullptr) anchor_->submit_before_frame(kd::anchor::Rect{}, target_->top_level);
    const kd::InjectResult res = injector_->inject(*target_, r.op->text);
    if (anchor_ != nullptr) anchor_->submit_after_frame(r.op->text);

    if (res.status != kd::InjectStatus::Ok) {
        // Injection success is decided here, by the mechanism that performed it — never inferred
        // from pixels (plan.md §1.5).
        KD_LOG_ERROR("injection failed: %s", kd::to_string(res.status));
        set_tray_state(res.status == kd::InjectStatus::ElevatedUnsupported
                           ? kd::OverlayState::Unsupported
                           : kd::OverlayState::Error);
    }
}

// ─── Session lifecycle ──────────────────────────────────────────────────────────────────────

void App::begin_dictation() {
    if (session_ == nullptr) return;

    auto captured = kd::capture_foreground_target();
    if (!captured) {
        KD_LOG_WARN("no usable foreground target: %s", captured.status().message().c_str());
        set_tray_state(kd::OverlayState::Unsupported);
        return;
    }
    target_ = captured.value();

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

    apply(session_->on_local(kd::LocalEvent::HotkeyPressed));
    set_tray_state(kd::OverlayState::Listening);
    if (overlay_ != nullptr) {
        overlay_->set_state(kd::OverlayState::Listening);
        overlay_->show();
    }
}

void App::end_dictation(kd::LocalEvent why) {
    if (session_ == nullptr || !target_.has_value()) return;

    apply(session_->on_local(why));

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

void App::on_sidecar_lost() {
    if (quitting_) return;
    KD_LOG_WARN("sidecar connection ended");
    if (audio_running_) {
        capture_->stop();
        audio_running_ = false;
    }
    // Committed text stays in the document — it was correct when it was written and the
    // invariant forbids taking it back. Only the uncommitted tail is lost.
    target_.reset();
    if (overlay_ != nullptr) overlay_->hide();
    apply(session_->on_transport(kd::TransportEvent::BrokenPipe));
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
