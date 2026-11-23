// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Supervising kvit-asrd.exe — see translator.h for why this spawn is a different thing from
// the one release-plan.md §1.2 deleted.
//
// ─── Windows facts this file depends on, each measured rather than assumed ───────────────────
//
//   * `AssignProcessToJobObject` succeeds even though this process is already inside a job
//     (nested jobs, Windows 8+), and closing the job handle does kill the child. Measured in
//     `ipc_client.cpp` (see its header) and carried here rather than re-discovered.
//   * `CREATE_SUSPENDED` is load-bearing: the child must be inside the job before it executes
//     a single instruction, or a fast child could start a grandchild that escapes
//     `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`.
//   * An explicit `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` rather than bare `bInheritHandles=TRUE`.
//     Without it the child inherits *every* inheritable handle in this process, including ones
//     another thread happens to be creating at this instant — a real race, and the leaked handle
//     would keep a pipe or a file alive for the child's whole lifetime.
//   * Our copy of the stderr pipe's write end must be closed after `CreateProcessW`, or the
//     reader never sees end of stream and the log thread never exits.

#define KD_LOG_MODULE "translator"

#include "translator.h"

#include <winsock2.h>
#include <ws2tcpip.h>
// Must follow winsock2.h.
#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "settings.h"
#include "util/log.h"
#include "util/utf8.h"

#pragma comment(lib, "ws2_32.lib")

namespace kd {
namespace {

// How many times to retry when the child dies immediately. The only expected cause is losing
// the TOCTOU race on the port, which a fresh port fixes; more than a handful of consecutive
// losses means something else is wrong and a retry loop would hide it.
constexpr int kPortRetries = 5;

// How long to wait after launch before concluding the child is up. Long enough for a bind
// failure to surface as an exit, short enough not to be felt in a settings dialog.
constexpr DWORD kStartupSettleMs = 250;

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h) {}
    ~Handle() { close(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) {
            close();
            h_ = o.h_;
            o.h_ = nullptr;
        }
        return *this;
    }
    void reset(HANDLE h = nullptr) {
        close();
        h_ = h;
    }
    void close() {
        if (h_ != nullptr && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = nullptr;
    }
    [[nodiscard]] HANDLE get() const noexcept { return h_; }
    [[nodiscard]] bool valid() const noexcept { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE h_ = nullptr;
};

// The same quoting rule `ipc_client.cpp` uses, and for the same reason: CommandLineToArgvW's
// backslash-before-quote rule is not the intuitive one, and a hand-rolled "wrap it in quotes"
// gets `C:\path\` wrong.
void append_quoted_arg(std::wstring& out, const std::wstring& arg) {
    if (!out.empty()) out.push_back(L' ');
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        out += arg;
        return;
    }
    out.push_back(L'"');
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++i;
            ++backslashes;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        out.push_back(arg[i]);
    }
    out.push_back(L'"');
}

Status os_failure(const char* what, DWORD err) {
    return Status{StatusCode::OsError, std::string(what) + " failed",
                  static_cast<uint32_t>(err)};
}

}  // namespace

// ─── Port selection ─────────────────────────────────────────────────────────────────────────

Result<uint16_t> pick_loopback_port() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return Status{StatusCode::Unavailable, "WSAStartup failed"};
    }
    struct Cleanup {
        ~Cleanup() { WSACleanup(); }
    } cleanup;

    const SOCKET fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) {
        return os_failure("socket", static_cast<DWORD>(WSAGetLastError()));
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;  // let the stack choose
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        const DWORD err = static_cast<DWORD>(WSAGetLastError());
        ::closesocket(fd);
        return os_failure("bind (choosing a loopback port)", err);
    }
    sockaddr_in bound{};
    int len = sizeof bound;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
        const DWORD err = static_cast<DWORD>(WSAGetLastError());
        ::closesocket(fd);
        return os_failure("getsockname", err);
    }
    ::closesocket(fd);

    // Deliberately NOT listen()ed on: the child must be the one to listen, and a socket left in
    // TIME_WAIT here would make the port unusable for it. The gap between this close and the
    // child's bind is the TOCTOU window translator.h names, and start() retries through it.
    return ntohs(bound.sin_port);
}

// ─── The command line ───────────────────────────────────────────────────────────────────────

std::wstring build_translator_command_line(const std::filesystem::path& image,
                                           const TranslatorSpec& spec, uint16_t port,
                                           uint32_t parent_pid) {
    std::wstring cmd;
    append_quoted_arg(cmd, image.wstring());

    // Loopback, always. `--listen` on a non-loopback address is an explicit act by someone
    // running the translator by hand; nothing the client builds can produce one.
    append_quoted_arg(cmd, L"--listen");
    append_quoted_arg(cmd, L"127.0.0.1:" + std::to_wstring(port));

    if (spec.on_device) {
        // No service, no model name, no key: the model is on this machine. The helper's path is
        // the translator's own fixed default (backend_windows.h), so nothing here names it.
        append_quoted_arg(cmd, L"--backend");
        append_quoted_arg(cmd, L"windows-speech");
        if (spec.install_speech_model) append_quoted_arg(cmd, L"--speech-install-model");
    } else {
        append_quoted_arg(cmd, L"--service");
        append_quoted_arg(cmd, utf8::to_utf16_or_empty(spec.service_url));

        if (!spec.model.empty()) {
            append_quoted_arg(cmd, L"--model");
            append_quoted_arg(cmd, utf8::to_utf16_or_empty(spec.model));
        }
    }

    // Explicit on/off rather than a bare switch, because the translator's default is ON and a
    // bare `--finalize-only` would read as if it changed something.
    append_quoted_arg(cmd, L"--finalize-only");
    append_quoted_arg(cmd, spec.live_preview && !spec.on_device ? L"off" : L"on");

    append_quoted_arg(cmd, L"--endpoint-silence-ms");
    append_quoted_arg(cmd, std::to_wstring(spec.endpoint_silence_ms));

    if (!spec.revision_dir.empty()) {
        append_quoted_arg(cmd, L"--revision-dir");
        append_quoted_arg(cmd, spec.revision_dir.wstring());
    }

    // The parent always passes its own pid. The job object is the real guarantee; this watch is
    // the backup for the case where the child somehow ends up outside the job.
    append_quoted_arg(cmd, L"--parent-pid");
    append_quoted_arg(cmd, std::to_wstring(parent_pid));

    // A switch, not a value: the key itself goes down the child's stdin (see `launch`).
    if (!spec.api_key.empty() && !spec.on_device) append_quoted_arg(cmd, L"--api-key-stdin");

    // There is deliberately no --silero-model and no environment block: openai-translator.md
    // §1.4 and §4.6. `--backend` is a constant this program chooses, never text the user typed.
    // Nothing the user types reaches this command line except `--service` and `--model`, both of
    // them values rather than arguments.
    return cmd;
}

// ─── Impl ───────────────────────────────────────────────────────────────────────────────────

struct Translator::Impl {
    mutable std::mutex mu;
    Handle job;
    Handle process;
    Handle child_err;  // read end of the child's stderr
    std::thread log_thread;
    std::thread watch_thread;
    std::atomic<uint32_t> pid{0};
    std::atomic<uint16_t> port{0};
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};
    std::string url;
    std::function<void(uint32_t)> on_exit;

    // Pump the child's stderr into this process's own log.
    //
    // openai-translator.md §4.8: there is no separate `asrd.log`, because the first support
    // question would then be answered from two files that have to be interleaved by hand. The
    // child already formats its own lines; they are forwarded verbatim with an `asrd:` prefix so
    // a reader can tell which process spoke.
    void log_main() {
        std::string pending;
        for (;;) {
            char buf[4096];
            DWORD read = 0;
            if (!ReadFile(child_err.get(), buf, sizeof buf, &read, nullptr) || read == 0) break;
            pending.append(buf, read);
            for (;;) {
                const size_t eol = pending.find('\n');
                if (eol == std::string::npos) break;
                std::string line = pending.substr(0, eol);
                pending.erase(0, eol + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) KD_LOG_INFO("asrd: %s", line.c_str());
            }
            // A child that writes an unbounded line must not make us buffer it unbounded.
            if (pending.size() > 8192) {
                KD_LOG_WARN("asrd: %s [line truncated]", pending.substr(0, 480).c_str());
                pending.clear();
            }
        }
        if (!pending.empty()) KD_LOG_INFO("asrd: %s", pending.c_str());
    }

    void watch_main() {
        WaitForSingleObject(process.get(), INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(process.get(), &code);
        running.store(false);
        if (stopping.load()) return;  // we asked for this
        KD_LOG_ERROR("kvit-asrd (pid %lu) exited unexpectedly with code %lu",
                     static_cast<unsigned long>(pid.load()), static_cast<unsigned long>(code));
        std::function<void(uint32_t)> cb;
        {
            std::lock_guard<std::mutex> lock(mu);
            cb = on_exit;
        }
        if (cb) cb(static_cast<uint32_t>(code));
    }
};

Translator::Translator() : impl_(std::make_unique<Impl>()) {}

Translator::~Translator() { stop(); }

Result<std::filesystem::path> Translator::image_path() {
    // `GetModuleFileName(nullptr)`'s directory plus `kvit-asrd.exe`, and nothing else. This one
    // line is the security property translator.h describes: there is no setting, environment
    // variable or manifest that can make this resolve anywhere else.
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return os_failure("GetModuleFileNameW", GetLastError());
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    std::filesystem::path image = std::filesystem::path(buf).parent_path() / L"kvit-asrd.exe";
    std::error_code ec;
    if (!std::filesystem::exists(image, ec)) {
        // A broken installation. The path is in the message because the user cannot act on
        // "the translator is missing" and can act on "this file is not where it should be".
        return Status{StatusCode::NotFound,
                      "kvit-asrd.exe is not next to kvit-dictate.exe. It should be at " +
                          image.string() + ". Reinstall, or run your own server instead."};
    }
    return image;
}

void Translator::set_on_exit(std::function<void(uint32_t)> cb) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->on_exit = std::move(cb);
}

bool Translator::running() const noexcept { return impl_->running.load(); }
uint16_t Translator::port() const noexcept { return impl_->port.load(); }
uint32_t Translator::pid() const noexcept { return impl_->pid.load(); }

std::string Translator::url() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->url;
}

Result<std::string> Translator::start(const TranslatorSpec& spec_in) {
    if (impl_->running.load()) stop();

    // On this PC there is no address, no model and no key, and none may reach the command line
    // by accident: `build_translator_command_line` leaves them off, and so does this copy.
    TranslatorSpec on_device_spec;
    if (spec_in.on_device) {
        on_device_spec.on_device = true;
        on_device_spec.install_speech_model = spec_in.install_speech_model;
        on_device_spec.live_preview = false;
        on_device_spec.endpoint_silence_ms = spec_in.endpoint_silence_ms;
        on_device_spec.revision_dir = spec_in.revision_dir;
    }
    const TranslatorSpec& spec = spec_in.on_device ? on_device_spec : spec_in;

    // The URL is validated before it can reach a command line. This is the check that makes
    // "user-supplied content" different from "a user-supplied argv" — see translator.h.
    if (const std::string why = validate_speech_service_url(spec.service_url); !why.empty()) {
        return Status{StatusCode::InvalidArgument, "speech service address: " + why};
    }
    if (spec.service_url.empty() && !spec.on_device) {
        return Status{StatusCode::InvalidArgument,
                      "no speech service is configured (docs/protocol.md §10.6)"};
    }
    // The child reads one line of stdin, so a key with a line break in it would arrive cut short
    // and fail as "refused" with nothing saying why.
    if (const std::string why = validate_api_key(spec.api_key); !why.empty()) {
        return Status{StatusCode::InvalidArgument, why};
    }

    const Result<std::filesystem::path> image = image_path();
    if (!image) return image.status();

    Status last{StatusCode::Internal, "the translator was never launched"};
    for (int attempt = 0; attempt < kPortRetries; ++attempt) {
        const Result<uint16_t> port = pick_loopback_port();
        if (!port) return port.status();

        Status st = launch(*image, spec, *port);
        if (st.ok()) {
            std::lock_guard<std::mutex> lock(impl_->mu);
            impl_->url = "ws://127.0.0.1:" + std::to_string(*port) + "/";
            // Logged rather than merely known: openai-translator.md §4.6 makes this the line a
            // support question is answered from, and the Settings page shows it too.
            // "Will listen", not "listening": the process is up, but on a slow start it binds
            // its port seconds later (kTranslatorListenWaitMs), and a log that claimed otherwise
            // made a refused connect look like a translator that had refused it on purpose.
            KD_LOG_INFO("kvit-asrd pid %lu started; it will listen on %s for %s",
                        static_cast<unsigned long>(impl_->pid.load()), impl_->url.c_str(),
                        spec.on_device ? "the Windows speech model on this PC"
                                       : spec.service_url.c_str());
            return impl_->url;
        }
        last = st;
        // The only expected cause is losing the race on the port. Retrying with a fresh one is
        // cheap; retrying forever would hide a real failure, so the count is small and every
        // attempt is logged.
        KD_LOG_WARN("kvit-asrd did not come up on port %u (%s); retrying with a fresh port",
                    *port, st.to_string().c_str());
    }
    return last;
}

Status Translator::launch(const std::filesystem::path& image, const TranslatorSpec& spec,
                          uint16_t port) {
    Impl& im = *impl_;

    // The job first, so the child can be assigned before it runs a single instruction.
    im.job.reset(CreateJobObjectW(nullptr, nullptr));
    if (!im.job.valid()) return os_failure("CreateJobObjectW", GetLastError());

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    // A crashed or killed client must never leave a translator behind holding a loopback port.
    // KILL_ON_JOB_CLOSE gives that even when this process is terminated without running any
    // cleanup at all, which no amount of careful shutdown code can.
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(im.job.get(), JobObjectExtendedLimitInformation, &limits,
                                 sizeof limits)) {
        const DWORD err = GetLastError();
        im.job.close();
        return os_failure("SetInformationJobObject", err);
    }

    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof inherit;
    inherit.bInheritHandle = TRUE;

    Handle err_read;
    Handle err_write;
    {
        HANDLE r = nullptr;
        HANDLE w = nullptr;
        if (!CreatePipe(&r, &w, &inherit, 0)) {
            const DWORD err = GetLastError();
            im.job.close();
            return os_failure("CreatePipe (child stderr)", err);
        }
        err_read.reset(r);
        err_write.reset(w);
        // Our end must not be inheritable, or the child holds a copy and we never see EOF.
        SetHandleInformation(err_read.get(), HANDLE_FLAG_INHERIT, 0);
    }
    // stdin is NUL, unless there is an API key to hand over — then it is a pipe that carries the
    // key and nothing else. The key never touches the command line or an environment block,
    // both of which other processes can read.
    Handle child_in;
    Handle key_write;
    if (spec.api_key.empty()) {
        child_in.reset(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  &inherit, OPEN_EXISTING, 0, nullptr));
    } else {
        HANDLE r = nullptr;
        HANDLE w = nullptr;
        if (!CreatePipe(&r, &w, &inherit, 0)) {
            const DWORD err = GetLastError();
            im.job.close();
            return os_failure("CreatePipe (child stdin)", err);
        }
        child_in.reset(r);
        key_write.reset(w);
        SetHandleInformation(key_write.get(), HANDLE_FLAG_INHERIT, 0);
    }

    const std::wstring cmdline =
        build_translator_command_line(image, spec, port, GetCurrentProcessId());

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = child_in.get();
    si.StartupInfo.hStdOutput = err_write.get();
    si.StartupInfo.hStdError = err_write.get();

    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<uint8_t> attr_buf(attr_size);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    HANDLE inherited[2] = {err_write.get(), child_in.get()};
    const DWORD inherited_count = child_in.valid() ? 2u : 1u;
    if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size) ||
        !UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited, inherited_count * sizeof(HANDLE), nullptr,
                                   nullptr)) {
        const DWORD err = GetLastError();
        im.job.close();
        return os_failure("InitializeProcThreadAttributeList", err);
    }

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutable_cmdline(cmdline.begin(), cmdline.end());
    mutable_cmdline.push_back(L'\0');  // CreateProcessW may write into lpCommandLine

    // lpApplicationName is the absolute image path, so the command line's first token is never
    // consulted for *what* to run. The working directory is the installation directory.
    const std::wstring image_w = image.wstring();
    const std::wstring workdir = image.parent_path().wstring();
    const BOOL created = CreateProcessW(
        image_w.c_str(), mutable_cmdline.data(), nullptr, nullptr, TRUE,
        // CREATE_SUSPENDED so the process is in the job before it executes anything.
        // CREATE_NO_WINDOW so the console subsystem child is invisible when supervised.
        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
        workdir.c_str(), &si.StartupInfo, &pi);
    const DWORD create_err = GetLastError();
    DeleteProcThreadAttributeList(si.lpAttributeList);

    if (!created) {
        im.job.close();
        return os_failure("CreateProcessW (kvit-asrd.exe)", create_err);
    }

    Handle proc(pi.hProcess);
    Handle thread(pi.hThread);
    if (key_write.valid()) {
        // Written while the child is still suspended. A key is far smaller than a pipe's buffer,
        // so this cannot block on a reader that has not started; closing the write end is what
        // gives the child its end of input.
        std::string line = spec.api_key + "\n";
        DWORD written = 0;
        const BOOL ok = WriteFile(key_write.get(), line.data(), static_cast<DWORD>(line.size()),
                                  &written, nullptr);
        const DWORD err = GetLastError();
        SecureZeroMemory(line.data(), line.size());
        key_write.close();
        if (!ok || written != line.size()) {
            TerminateProcess(proc.get(), 1);
            im.job.close();
            return os_failure("WriteFile (API key to the translator)", err);
        }
    }
    if (!AssignProcessToJobObject(im.job.get(), proc.get())) {
        const DWORD err = GetLastError();
        TerminateProcess(proc.get(), 1);
        im.job.close();
        return os_failure("AssignProcessToJobObject", err);
    }
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        const DWORD err = GetLastError();
        TerminateProcess(proc.get(), 1);
        im.job.close();
        return os_failure("ResumeThread", err);
    }

    im.process = std::move(proc);
    im.child_err = std::move(err_read);
    im.pid.store(pi.dwProcessId);
    im.port.store(port);
    im.stopping.store(false);
    im.running.store(true);
    // Drop our copy of the write end, or the log reader never sees end of stream.
    err_write.close();
    child_in.close();

    im.log_thread = std::thread([&im] { im.log_main(); });

    // A bind failure surfaces as an immediate exit, and it is the one failure worth waiting for:
    // returning a URL that nothing is listening on would turn a retryable port collision into a
    // connect timeout the user reads as "the server is down".
    if (WaitForSingleObject(im.process.get(), kStartupSettleMs) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(im.process.get(), &code);
        im.running.store(false);
        if (im.log_thread.joinable()) im.log_thread.join();
        im.process.close();
        im.child_err.close();
        im.job.close();
        return Status{StatusCode::Unavailable,
                      "kvit-asrd exited immediately with code " + std::to_string(code) +
                          " (its own diagnostics are in this log, prefixed \"asrd:\")"};
    }

    im.watch_thread = std::thread([&im] { im.watch_main(); });
    return Status::success();
}

void Translator::stop() {
    Impl& im = *impl_;
    if (!im.process.valid()) return;
    im.stopping.store(true);

    // Closing the job is what kills the child — the same guarantee that holds when this process
    // dies without running any of this.
    im.job.close();
    WaitForSingleObject(im.process.get(), 3000);
    if (WaitForSingleObject(im.process.get(), 0) != WAIT_OBJECT_0) {
        KD_LOG_WARN("kvit-asrd pid %lu did not exit when its job closed; terminating",
                    static_cast<unsigned long>(im.pid.load()));
        TerminateProcess(im.process.get(), 1);
        WaitForSingleObject(im.process.get(), 2000);
    }

    if (im.watch_thread.joinable()) im.watch_thread.join();
    // Closing the read end unblocks the log reader if the child somehow left the write end open.
    if (im.log_thread.joinable()) im.log_thread.join();

    im.child_err.close();
    im.process.close();
    im.running.store(false);
    im.pid.store(0);
    im.port.store(0);
    {
        std::lock_guard<std::mutex> lock(im.mu);
        im.url.clear();
    }
    KD_LOG_INFO("kvit-asrd stopped");
}

}  // namespace kd
