// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// `--backend windows-speech` — see backend_windows.h.
//
// ─── Windows facts this file depends on ─────────────────────────────────────────────────────
//
// The launch is translator.cpp's, for the same reasons, and they are not re-derived here:
// `CREATE_SUSPENDED` so the helper is in the job before it runs an instruction, an explicit
// `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` so it inherits three pipe ends and nothing else, and our
// copies of the child's ends closed straight after `CreateProcessW` so end of stream is seen.
//
// One addition: **the stdin pipe is sized to hold a whole utterance.** A request is written
// before its answer is awaited, and `WriteFile` on an anonymous pipe blocks while the pipe is
// full. A helper that has stopped reading would then hang the decode thread inside `WriteFile`,
// where no timeout reaches. 4 MiB is two minutes of 16 kHz mono; the endpointer closes an
// utterance at 30 s, so a request always fits and the timeout is always the thing that fires.

#define KD_LOG_MODULE "asrd"

#include "asrd/backend_windows.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <thread>

#include "util/log.h"

namespace kd::asrd {
namespace {

constexpr DWORD kStdinPipeBytes = 4u * 1024u * 1024u;
constexpr size_t kStderrTailLines = 8;
constexpr uint32_t kQuitGraceMs = 2000;
constexpr std::chrono::milliseconds kCancelPoll{50};

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h) {}
    ~Handle() { close(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
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

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr,
                                      0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) {
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n,
                            nullptr, nullptr);
    }
    return out;
}

// The same quoting rule translator.cpp and ipc_client.cpp use (CommandLineToArgvW's).
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
    return Status{StatusCode::OsError, std::string(what) + " failed", static_cast<uint32_t>(err)};
}

bool write_all(HANDLE h, const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    while (size > 0) {
        DWORD n = 0;
        const DWORD want = static_cast<DWORD>(std::min<size_t>(size, 1u << 20));
        if (!WriteFile(h, p, want, &n, nullptr) || n == 0) return false;
        p += n;
        size -= n;
    }
    return true;
}

bool read_exact(HANDLE h, void* data, size_t size) {
    auto* p = static_cast<uint8_t*>(data);
    while (size > 0) {
        DWORD n = 0;
        const DWORD want = static_cast<DWORD>(std::min<size_t>(size, 1u << 20));
        if (!ReadFile(h, p, want, &n, nullptr) || n == 0) return false;
        p += n;
        size -= n;
    }
    return true;
}

std::string ms_of(size_t samples) {
    return std::to_string(samples / 16u) + " ms";
}

}  // namespace

std::filesystem::path default_speech_helper_path() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return std::filesystem::path(buf).parent_path() / L"kvit-speech" / L"kvit-speech.exe";
}

uint32_t pcm_peak(const int16_t* samples, size_t count) noexcept {
    uint32_t peak = 0;
    for (size_t i = 0; i < count; ++i) {
        const int32_t v = samples[i];
        const uint32_t mag = static_cast<uint32_t>(v < 0 ? -v : v);  // -32768 -> 32768, no UB
        if (mag > peak) peak = mag;
    }
    return peak;
}

// ─── SpeechHelper ───────────────────────────────────────────────────────────────────────────

struct SpeechHelper::Impl {
    Spec spec;

    // Serialises requests: one question in flight, answers in order.
    std::mutex request_mu;

    // Everything below is guarded by `mu`.
    mutable std::mutex mu;
    std::condition_variable cv;
    Handle job;
    Handle process;
    Handle in_write;   // our end of the child's stdin
    Handle out_read;   // our end of the child's stdout: frames
    Handle err_read;   // our end of the child's stderr: diagnostics
    std::thread frame_thread;
    std::thread stderr_thread;
    struct Frame {
        uint8_t tag = 0;
        std::string payload;
    };
    std::deque<Frame> frames;
    bool eof = false;           // the frame stream ended: the helper exited or desynchronised
    std::string eof_reason;
    bool ready = false;
    std::string ready_payload;
    uint32_t abandoned = 0;     // answers owed to requests whose caller gave up waiting
    uint64_t launches = 0;
    std::deque<std::string> stderr_tail;

    void read_frames(HANDLE h) {
        for (;;) {
            uint8_t header[4];
            if (!read_exact(h, header, sizeof header)) {
                finish("the helper closed its output");
                return;
            }
            const uint32_t length = static_cast<uint32_t>(header[0]) |
                                    (static_cast<uint32_t>(header[1]) << 8) |
                                    (static_cast<uint32_t>(header[2]) << 16) |
                                    (static_cast<uint32_t>(header[3]) << 24);
            if (length < 1 || length > kSpeechMaxFrameBytes) {
                // Four arbitrary bytes are not a size to allocate. Something other than a frame
                // reached stdout — the hazard Program.cs hijacks stdout to prevent.
                finish("the helper wrote " + std::to_string(length) +
                       " as a frame length; its output is not a frame stream");
                return;
            }
            std::string body(length, '\0');
            if (!read_exact(h, body.data(), body.size())) {
                finish("the helper's output ended inside a frame");
                return;
            }
            Frame f;
            f.tag = static_cast<uint8_t>(body[0]);
            f.payload.assign(body, 1, std::string::npos);
            {
                std::lock_guard<std::mutex> lock(mu);
                frames.push_back(std::move(f));
            }
            cv.notify_all();
        }
    }

    void finish(std::string why) {
        {
            std::lock_guard<std::mutex> lock(mu);
            if (!eof) {
                eof = true;
                eof_reason = std::move(why);
            }
        }
        cv.notify_all();
    }

    void read_stderr(HANDLE h) {
        std::string pending;
        char chunk[4096];
        for (;;) {
            DWORD n = 0;
            if (!ReadFile(h, chunk, sizeof chunk, &n, nullptr) || n == 0) break;
            pending.append(chunk, n);
            for (size_t eol; (eol = pending.find('\n')) != std::string::npos;) {
                std::string line = pending.substr(0, eol);
                pending.erase(0, eol + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                log_stderr_line(line);
            }
        }
        if (!pending.empty()) log_stderr_line(pending);
    }

    void log_stderr_line(const std::string& line) {
        if (line.empty()) return;
        // The quantized model prints this a dozen times on every load. Harmless (README,
        // "The implementation DLLs print to stdout"), and at Info it would bury the lines that
        // say something.
        const bool chatter = line.rfind("[loader]", 0) == 0;
        if (chatter) {
            KD_LOG_DEBUG("kvit-speech: %s", line.c_str());
            return;
        }
        KD_LOG_INFO("kvit-speech: %s", line.c_str());
        std::lock_guard<std::mutex> lock(mu);
        stderr_tail.push_back(line);
        while (stderr_tail.size() > kStderrTailLines) stderr_tail.pop_front();
    }

    // "; it said: ..." — the helper's own last words, which are the useful part of most failures.
    std::string tail_locked() const {
        if (stderr_tail.empty()) return {};
        std::string s = "; it said: ";
        for (size_t i = 0; i < stderr_tail.size(); ++i) {
            if (i != 0) s += " | ";
            s += stderr_tail[i];
        }
        return s;
    }

    Status launch_locked() {
        frames.clear();
        eof = false;
        eof_reason.clear();
        ready = false;
        ready_payload.clear();
        abandoned = 0;
        stderr_tail.clear();

        std::error_code ec;
        if (!std::filesystem::is_regular_file(spec.exe, ec)) {
            return Status{StatusCode::NotFound,
                          "the Windows speech helper is not installed: " +
                              narrow(spec.exe.wstring()) +
                              " is missing. Reinstall kvit-dictate, or choose a speech service "
                              "in Settings > Server."};
        }

        job.reset(CreateJobObjectW(nullptr, nullptr));
        if (!job.valid()) return os_failure("CreateJobObjectW", GetLastError());
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits,
                                     sizeof limits)) {
            const DWORD err = GetLastError();
            job.close();
            return os_failure("SetInformationJobObject", err);
        }

        SECURITY_ATTRIBUTES inherit{};
        inherit.nLength = sizeof inherit;
        inherit.bInheritHandle = TRUE;

        Handle child_in, child_out, child_err;
        {
            HANDLE r = nullptr, w = nullptr;
            if (!CreatePipe(&r, &w, &inherit, kStdinPipeBytes)) {
                return os_failure("CreatePipe (helper stdin)", GetLastError());
            }
            child_in.reset(r);
            in_write.reset(w);
            SetHandleInformation(in_write.get(), HANDLE_FLAG_INHERIT, 0);
        }
        {
            HANDLE r = nullptr, w = nullptr;
            if (!CreatePipe(&r, &w, &inherit, 1u << 16)) {
                return os_failure("CreatePipe (helper stdout)", GetLastError());
            }
            out_read.reset(r);
            child_out.reset(w);
            SetHandleInformation(out_read.get(), HANDLE_FLAG_INHERIT, 0);
        }
        {
            HANDLE r = nullptr, w = nullptr;
            if (!CreatePipe(&r, &w, &inherit, 0)) {
                return os_failure("CreatePipe (helper stderr)", GetLastError());
            }
            err_read.reset(r);
            child_err.reset(w);
            SetHandleInformation(err_read.get(), HANDLE_FLAG_INHERIT, 0);
        }

        std::wstring cmdline;
        append_quoted_arg(cmdline, spec.exe.wstring());
        for (const std::wstring& a : spec.args) append_quoted_arg(cmdline, a);

        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof si;
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = child_in.get();
        si.StartupInfo.hStdOutput = child_out.get();
        si.StartupInfo.hStdError = child_err.get();
        SIZE_T attr_size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
        std::vector<uint8_t> attr_buf(attr_size);
        si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
        HANDLE inherited[3] = {child_in.get(), child_out.get(), child_err.get()};
        if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size) ||
            !UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       inherited, sizeof inherited, nullptr, nullptr)) {
            return os_failure("InitializeProcThreadAttributeList", GetLastError());
        }

        std::vector<wchar_t> mutable_cmdline(cmdline.begin(), cmdline.end());
        mutable_cmdline.push_back(L'\0');
        const std::wstring image = spec.exe.wstring();
        // The helper's own directory: it carries the Windows App SDK runtime beside itself.
        const std::wstring workdir = spec.exe.parent_path().wstring();
        PROCESS_INFORMATION pi{};
        const BOOL created = CreateProcessW(
            image.c_str(), mutable_cmdline.data(), nullptr, nullptr, TRUE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
            workdir.c_str(), &si.StartupInfo, &pi);
        const DWORD create_err = GetLastError();
        DeleteProcThreadAttributeList(si.lpAttributeList);
        if (!created) {
            in_write.close();
            out_read.close();
            err_read.close();
            return Status{StatusCode::OsError,
                          "cannot start the Windows speech helper " + narrow(image),
                          static_cast<uint32_t>(create_err)};
        }
        process.reset(pi.hProcess);
        Handle thread(pi.hThread);
        if (!AssignProcessToJobObject(job.get(), pi.hProcess)) {
            const DWORD err = GetLastError();
            TerminateProcess(pi.hProcess, 1);
            process.close();
            return os_failure("AssignProcessToJobObject (kvit-speech.exe)", err);
        }
        ResumeThread(thread.get());
        ++launches;

        // Ours closed now, or EOF never arrives on the readers.
        child_in.close();
        child_out.close();
        child_err.close();

        frame_thread = std::thread([this, h = out_read.get()] { read_frames(h); });
        stderr_thread = std::thread([this, h = err_read.get()] { read_stderr(h); });
        KD_LOG_INFO("started the Windows speech helper, pid %lu: %s",
                    static_cast<unsigned long>(pi.dwProcessId), narrow(image).c_str());
        return Status::success();
    }

    // Kill the process and reap the threads. Called with `mu` held; releases it while joining,
    // because the reader threads take it to deliver their last frame.
    void kill_locked(std::unique_lock<std::mutex>& lock, bool polite) {
        if (polite && in_write.valid() && !eof) {
            const uint8_t quit[5] = {1, 0, 0, 0, 'Q'};
            write_all(in_write.get(), quit, sizeof quit);
        }
        in_write.close();  // EOF on its stdin: ServeAsync's exit, if 'Q' did not arrive
        if (process.valid()) {
            if (WaitForSingleObject(process.get(), polite ? kQuitGraceMs : 0) != WAIT_OBJECT_0) {
                TerminateJobObject(job.get(), 1);
                WaitForSingleObject(process.get(), kQuitGraceMs);
            }
        }
        job.close();  // kill-on-close: anything it started goes too
        std::thread ft = std::move(frame_thread);
        std::thread st = std::move(stderr_thread);
        lock.unlock();
        if (ft.joinable()) ft.join();
        if (st.joinable()) st.join();
        lock.lock();
        process.close();
        out_read.close();
        err_read.close();
        ready = false;
        frames.clear();
        abandoned = 0;
    }

    std::string exit_description_locked() const {
        DWORD code = STILL_ACTIVE;
        if (process.valid()) GetExitCodeProcess(process.get(), &code);
        std::string s = eof_reason.empty() ? "the helper stopped" : eof_reason;
        if (code != STILL_ACTIVE) {
            char buf[48];
            std::snprintf(buf, sizeof buf, " (exit code 0x%08lX)", static_cast<unsigned long>(code));
            s += buf;
        }
        return s + tail_locked();
    }

    // Wait for the next frame. Returns false on timeout, cancel or end of stream.
    enum class Wait { Frame, Timeout, Cancelled, Ended };
    Wait next_frame_locked(std::unique_lock<std::mutex>& lock, uint32_t timeout_ms,
                           const std::atomic<bool>* cancel, Frame& out) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            if (!frames.empty()) {
                out = std::move(frames.front());
                frames.pop_front();
                return Wait::Frame;
            }
            if (eof) return Wait::Ended;
            if (cancel != nullptr && cancel->load()) return Wait::Cancelled;
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return Wait::Timeout;
            const auto step = cancel != nullptr
                                  ? std::min<std::chrono::steady_clock::duration>(deadline - now, kCancelPoll)
                                  : deadline - now;
            cv.wait_for(lock, step);
        }
    }

    Status ensure_ready_locked(std::unique_lock<std::mutex>& lock) {
        if (ready && !eof) return Status::success();
        if (process.valid()) {
            KD_LOG_WARN("the Windows speech helper is gone (%s); starting it again",
                        exit_description_locked().c_str());
            kill_locked(lock, false);
        }
        const auto started = std::chrono::steady_clock::now();
        if (Status st = launch_locked(); !st.ok()) return st;

        Frame f;
        switch (next_frame_locked(lock, spec.startup_timeout_ms, nullptr, f)) {
            case Wait::Frame:
                break;
            case Wait::Timeout: {
                const std::string tail = tail_locked();
                kill_locked(lock, false);
                return Status{StatusCode::Timeout,
                              "the Windows speech model did not load within " +
                                  std::to_string(spec.startup_timeout_ms / 1000) + " s" + tail};
            }
            default: {
                // Give the stderr reader a moment: the reason is usually its last line.
                lock.unlock();
                if (process.valid()) WaitForSingleObject(process.get(), 1000);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                lock.lock();
                const std::string why = exit_description_locked();
                kill_locked(lock, false);
                return Status{StatusCode::Unavailable,
                              "the Windows speech helper exited before it was ready: " + why};
            }
        }
        if (f.tag == 'E') {
            kill_locked(lock, false);
            return Status{StatusCode::Unavailable,
                          "the Windows speech model is unavailable: " + f.payload};
        }
        if (f.tag != 'R') {
            kill_locked(lock, false);
            return Status{StatusCode::Malformed,
                          std::string("the Windows speech helper's first answer was '") +
                              static_cast<char>(f.tag) + "', expected 'R' (ready) or 'E'"};
        }
        ready = true;
        ready_payload = std::move(f.payload);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
        KD_LOG_INFO("the Windows speech model is ready after %lld ms: %s",
                    static_cast<long long>(ms), ready_payload.c_str());
        return Status::success();
    }
};

SpeechHelper::SpeechHelper(Spec spec) : impl_(std::make_unique<Impl>()) {
    impl_->spec = std::move(spec);
}

SpeechHelper::~SpeechHelper() { stop(); }

Status SpeechHelper::ensure_ready() {
    std::lock_guard<std::mutex> serial(impl_->request_mu);
    std::unique_lock<std::mutex> lock(impl_->mu);
    return impl_->ensure_ready_locked(lock);
}

Status SpeechHelper::transcribe(const int16_t* samples, size_t count, uint32_t timeout_ms,
                                const std::atomic<bool>& cancel, std::string& out) {
    out.clear();
    Impl& im = *impl_;
    std::lock_guard<std::mutex> serial(im.request_mu);
    std::unique_lock<std::mutex> lock(im.mu);
    if (Status st = im.ensure_ready_locked(lock); !st.ok()) return st;

    // Answers owed to requests that were cancelled. The helper is still decoding the last of
    // them, so the wait is real; it is also the only way to keep the next answer paired with the
    // next question.
    while (im.abandoned > 0) {
        Impl::Frame stale;
        const Impl::Wait w = im.next_frame_locked(lock, timeout_ms, &cancel, stale);
        if (w == Impl::Wait::Cancelled) {
            return Status{StatusCode::Unavailable, "cancelled"};
        }
        if (w != Impl::Wait::Frame) {
            KD_LOG_WARN("the Windows speech helper did not finish a cancelled decode; restarting it");
            im.kill_locked(lock, false);
            return Status{StatusCode::Timeout,
                          "the Windows speech helper did not finish a cancelled decode"};
        }
        --im.abandoned;
    }

    const size_t bytes = count * sizeof(int16_t);
    if (bytes + 1 > kSpeechMaxFrameBytes) {
        return Status{StatusCode::InvalidArgument,
                      ms_of(count) + " of audio is more than the helper takes in one request"};
    }
    const uint32_t length = static_cast<uint32_t>(bytes + 1);
    const uint8_t header[5] = {static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8),
                               static_cast<uint8_t>(length >> 16),
                               static_cast<uint8_t>(length >> 24), 'T'};
    // Written under the lock on purpose: the pipe is sized so this never blocks (see the top of
    // this file), and holding it means `stop()` cannot close the handle mid-write.
    if (!write_all(im.in_write.get(), header, sizeof header) ||
        !write_all(im.in_write.get(), samples, bytes)) {
        const std::string why = im.exit_description_locked();
        im.kill_locked(lock, false);
        return Status{StatusCode::Unavailable,
                      "the Windows speech helper stopped taking requests: " + why};
    }

    Impl::Frame f;
    switch (im.next_frame_locked(lock, timeout_ms, &cancel, f)) {
        case Impl::Wait::Frame:
            break;
        case Impl::Wait::Cancelled:
            ++im.abandoned;
            return Status{StatusCode::Unavailable, "cancelled"};
        case Impl::Wait::Timeout:
            KD_LOG_ERROR("the Windows speech helper took more than %u ms over %s of audio; "
                         "killing it, the next decode starts a fresh one",
                         timeout_ms, ms_of(count).c_str());
            im.kill_locked(lock, false);
            return Status{StatusCode::Timeout, "the Windows speech model did not answer within " +
                                                   std::to_string(timeout_ms / 1000) + " s"};
        case Impl::Wait::Ended: {
            const std::string why = im.exit_description_locked();
            im.kill_locked(lock, false);
            return Status{StatusCode::Unavailable,
                          "the Windows speech helper exited mid-decode: " + why};
        }
    }
    if (f.tag == 'E') {
        // One failed window does not end anything: the link stays usable (Program.cs).
        return Status{StatusCode::Unavailable, "the Windows speech model failed on " +
                                                   ms_of(count) + " of audio: " + f.payload};
    }
    if (f.tag != 'O') {
        // An answer that is neither is a stream this side no longer understands; a fresh helper
        // is cheaper than guessing what the next frame means.
        im.kill_locked(lock, false);
        return Status{StatusCode::Malformed, std::string("the Windows speech helper answered '") +
                                                 static_cast<char>(f.tag) + "', expected 'O' or 'E'"};
    }
    out = std::move(f.payload);
    return Status::success();
}

void SpeechHelper::stop() {
    if (!impl_) return;
    std::unique_lock<std::mutex> lock(impl_->mu);
    if (!impl_->process.valid() && !impl_->frame_thread.joinable()) return;
    impl_->kill_locked(lock, true);
}

std::string SpeechHelper::ready_info() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->ready_payload;
}

uint64_t SpeechHelper::launches() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->launches;
}

namespace {

std::mutex g_helpers_mu;
std::map<std::wstring, std::shared_ptr<SpeechHelper>>& helpers() {
    static auto* m = new std::map<std::wstring, std::shared_ptr<SpeechHelper>>();
    return *m;  // leaked on purpose: no static-destructor order to get wrong at exit
}

std::wstring key_of(const SpeechHelper::Spec& spec) {
    std::wstring k = spec.exe.wstring();
    for (const std::wstring& a : spec.args) {
        k += L'\x1f';
        k += a;
    }
    return k;
}

}  // namespace

std::shared_ptr<SpeechHelper> shared_speech_helper(const SpeechHelper::Spec& spec) {
    std::lock_guard<std::mutex> lock(g_helpers_mu);
    std::shared_ptr<SpeechHelper>& slot = helpers()[key_of(spec)];
    if (!slot) slot = std::make_shared<SpeechHelper>(spec);
    return slot;
}

void release_speech_helpers() {
    std::map<std::wstring, std::shared_ptr<SpeechHelper>> taken;
    {
        std::lock_guard<std::mutex> lock(g_helpers_mu);
        taken.swap(helpers());
    }
    for (auto& [key, helper] : taken) {
        if (helper) helper->stop();
    }
}

// ─── WindowsSpeechBackend ───────────────────────────────────────────────────────────────────

WindowsSpeechBackend::WindowsSpeechBackend(WindowsSpeechConfig cfg,
                                           std::shared_ptr<SpeechHelper> helper)
    : cfg_(std::move(cfg)),
      helper_(std::move(helper)),
      driver_(cfg_.finalize_only ? 0u : cfg_.hop_ms) {}

WindowsSpeechBackend::~WindowsSpeechBackend() = default;

SpeechHelper::Spec WindowsSpeechBackend::helper_spec(const WindowsSpeechConfig& cfg) {
    SpeechHelper::Spec spec;
    spec.exe = cfg.helper.empty() ? default_speech_helper_path() : cfg.helper;
    if (cfg.install_model) spec.args.emplace_back(L"--ensure-model");
    spec.startup_timeout_ms = cfg.startup_timeout_ms;
    return spec;
}

Status WindowsSpeechBackend::load() {
    if (!helper_) return Status{StatusCode::Internal, "no Windows speech helper was given"};
    return helper_->ensure_ready();
}

void WindowsSpeechBackend::reset() {
    std::lock_guard<std::mutex> lock(driver_mutex_);
    driver_.reset();
}

void WindowsSpeechBackend::set_context(const std::vector<std::string>& terms) {
    // `ready.context_supported` is false, so a conforming client sends none. One that sends them
    // anyway loses nothing but the bias, and is told once.
    if (!terms.empty() && !context_ignored_logged_) {
        context_ignored_logged_ = true;
        KD_LOG_INFO("the session sent %zu vocabulary terms; the Windows speech model takes no "
                    "vocabulary, so they are not used",
                    terms.size());
    }
}

bool WindowsSpeechBackend::feed(const int16_t* samples, size_t count) {
    std::lock_guard<std::mutex> lock(driver_mutex_);
    return driver_.feed(samples, count);
}

Status WindowsSpeechBackend::transcribe(bool force, Transcription& out) {
    out = {};
    std::vector<int16_t> window;
    {
        std::lock_guard<std::mutex> lock(driver_mutex_);
        if (!driver_.take_window(force, window)) return Status::success();
    }
    if (capture_window_) last_window_ = window;

    // The silence gate. Not a failure: "this window says nothing" is the right answer, and it is
    // the answer that stops Whisper writing a sentence into the document from an empty room.
    if (cfg_.silence_peak_threshold > 0 &&
        pcm_peak(window.data(), window.size()) < cfg_.silence_peak_threshold) {
        ++silence_skipped_;
        out.have_text = true;
        return Status::success();
    }

    ++requests_;
    const auto t0 = std::chrono::steady_clock::now();
    const Status st =
        helper_->transcribe(window.data(), window.size(), cfg_.decode_timeout_ms, cancelled_,
                            out.text);
    if (!st.ok()) {
        out.text.clear();
        return st;
    }
    // Program.cs trims, but the wire's cumulative text is computed over this string, so it is
    // made certain here rather than assumed of another process.
    const size_t first = out.text.find_first_not_of(" \t\r\n");
    const size_t last = out.text.find_last_not_of(" \t\r\n");
    out.text = first == std::string::npos ? std::string() : out.text.substr(first, last - first + 1);
    out.have_text = true;
    KD_LOG_DEBUG("windows-speech decoded %s in %lld ms",
                 ms_of(window.size()).c_str(),
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - t0)
                                            .count()));
    return Status::success();
}

void WindowsSpeechBackend::retain_tail(size_t cut_samples) {
    // Never reached: this backend proposes no cuts. Kept correct anyway rather than asserting,
    // because the session machine owns when it is called.
    std::lock_guard<std::mutex> lock(driver_mutex_);
    driver_.retain_tail(cut_samples);
}

}  // namespace kd::asrd
