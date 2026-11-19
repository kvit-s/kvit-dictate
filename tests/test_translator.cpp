// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Suite `translator` — the client's half of the spawn contract (openai-translator.md §4.6, §5).
//
// The security argument for reinstating process spawning rests on three concrete claims, and
// each one is asserted here rather than only described in `translator.h`:
//
//   1. **The image path is fixed.** `GetModuleFileName(nullptr)`'s directory plus
//      `kvit-asrd.exe`, and nothing else. No setting, environment variable or manifest can
//      point it anywhere.
//   2. **The argv is constructed by this program.** The user contributes two *values* —
//      `--service` and `--model` — and nothing that could become an argument.
//   3. **A `--service` that would need quoting is rejected before it reaches argv**, which is
//      what makes "user-supplied content" different from "a user-supplied argv vector".
//
// `Translator::start()` itself launches a process and is exercised end to end by
// `tests/test_asrd_session.cpp`'s harness and by the release gate; what is here is everything
// that can be checked without one.

#include "translator.h"

#include <winsock2.h>
#include <ws2tcpip.h>
// Must follow winsock2.h.
#include <windows.h>

#include "ipc_client.h"
#include "kd_test.h"
#include "settings.h"
#include "util/utf8.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace {

std::wstring command_line_for(const std::string& service, const std::string& model = {},
                              bool live_preview = false, uint16_t port = 10500) {
    kd::TranslatorSpec spec;
    spec.service_url = service;
    spec.model = model;
    spec.live_preview = live_preview;
    return kd::build_translator_command_line(L"C:\\Program Files\\kvit-dictate\\kvit-asrd.exe",
                                             spec, port, 4242);
}

bool contains(const std::wstring& haystack, const wchar_t* needle) {
    return haystack.find(needle) != std::wstring::npos;
}

// Just enough of a speech service to see what a real, spawned kvit-asrd sends it: every request
// is answered with one fixed status and body, and the head of each is kept. The full stub, with
// routes and scripts, is test_asrd_session.cpp's and lives in-process; this one exists because
// the only way to see the stdin handoff is from outside a child that was really launched.
class RecordingService {
public:
    explicit RecordingService(int status, std::string body)
        : status_(status), body_(std::move(body)) {
        WSADATA d{};
        WSAStartup(MAKEWORD(2, 2), &d);
    }
    ~RecordingService() {
        if (listen_ != INVALID_SOCKET) ::closesocket(listen_);
        if (thread_.joinable()) thread_.join();
        WSACleanup();
    }

    bool start() {
        listen_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_ == INVALID_SOCKET) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) return false;
        if (::listen(listen_, 8) != 0) return false;
        int len = sizeof addr;
        ::getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] {
            for (;;) {
                const SOCKET fd = ::accept(listen_, nullptr, nullptr);
                if (fd == INVALID_SOCKET) return;
                serve(fd);
                ::closesocket(fd);
            }
        });
        return true;
    }

    [[nodiscard]] std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }
    [[nodiscard]] std::string heads() const {
        std::lock_guard<std::mutex> lock(m_);
        return heads_;
    }

private:
    void serve(SOCKET fd) {
        // The head, then Content-Length more bytes: answering before WinHTTP has finished
        // sending resets the connection on Windows.
        std::string raw;
        size_t head_end = std::string::npos;
        size_t length = 0;
        for (;;) {
            if (head_end == std::string::npos && (head_end = raw.find("\r\n\r\n")) != std::string::npos) {
                const size_t at = raw.find("Content-Length:");
                if (at != std::string::npos && at < head_end) {
                    length = std::strtoul(raw.c_str() + at + 15, nullptr, 10);
                }
            }
            if (head_end != std::string::npos && raw.size() >= head_end + 4 + length) break;
            char buf[8192];
            const int n = ::recv(fd, buf, static_cast<int>(sizeof buf), 0);
            if (n <= 0) return;  // the TCP probe at load connects and closes without a request
            raw.append(buf, static_cast<size_t>(n));
        }
        {
            std::lock_guard<std::mutex> lock(m_);
            heads_ += raw.substr(0, head_end + 4);
        }
        const std::string out = "HTTP/1.1 " + std::to_string(status_) +
                                " X\r\nContent-Type: application/json\r\nContent-Length: " +
                                std::to_string(body_.size()) + "\r\nConnection: close\r\n\r\n" +
                                body_;
        ::send(fd, out.data(), static_cast<int>(out.size()), 0);
        ::shutdown(fd, SD_SEND);
    }

    int status_;
    std::string body_;
    SOCKET listen_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    std::thread thread_;
    mutable std::mutex m_;
    std::string heads_;
};

}  // namespace

// ─── The command line ───────────────────────────────────────────────────────────────────────

KD_TEST(translator, the_command_line_is_what_the_translator_expects) {
    const std::wstring cmd = command_line_for("http://strixhalo.lan:10301");

    // The image path is argv[0], quoted because it has a space in it. `CreateProcessW` is
    // additionally given the path as lpApplicationName, so this token is never consulted for
    // *what* to run — but it must still be right, because the child sees it.
    KD_CHECK(cmd.rfind(L"\"C:\\Program Files\\kvit-dictate\\kvit-asrd.exe\"", 0) == 0);

    // Loopback, always. Nothing the client builds can produce a non-loopback --listen.
    KD_CHECK(contains(cmd, L"--listen 127.0.0.1:10500"));
    KD_CHECK(contains(cmd, L"--service http://strixhalo.lan:10301"));
    // The parent always passes its own pid; the job object is the real guarantee and this is the
    // backup for a child that somehow ends up outside it.
    KD_CHECK(contains(cmd, L"--parent-pid 4242"));
}

KD_TEST(translator, finalize_only_is_explicit_in_both_directions) {
    // The translator's default is ON, so a bare `--finalize-only` would read as if it changed
    // something. Both the on and the off case are written out.
    KD_CHECK(contains(command_line_for("http://h:1"), L"--finalize-only on"));
    KD_CHECK(contains(command_line_for("http://h:1", "", /*live_preview=*/true),
                      L"--finalize-only off"));
}

KD_TEST(translator, an_empty_model_is_omitted_rather_than_passed_empty) {
    // Absent `--model` and `--model ""` mean different things: the first takes the translator's
    // default, the second would ask for a model called "". Same rule `hello.model` follows.
    KD_CHECK(!contains(command_line_for("http://h:1", ""), L"--model"));
    KD_CHECK(contains(command_line_for("http://h:1", "Qwen/Qwen3-ASR-1.7B"),
                      L"--model Qwen/Qwen3-ASR-1.7B"));
}

KD_TEST(translator, nothing_the_user_types_becomes_an_argument) {
    // Claim 2. The whole argument vector is enumerated here, so that a future flag added from a
    // settings field has to change this test — which is the point of writing it out.
    const std::wstring cmd = command_line_for("http://h:1", "some-model");
    for (const wchar_t* flag : {L"--listen", L"--service", L"--model", L"--finalize-only",
                                L"--endpoint-silence-ms", L"--parent-pid"}) {
        KD_CHECK(contains(cmd, flag));
    }
    // Deliberately absent: openai-translator.md §1.4 and §4.7. Each is a separate, later,
    // argued addition, and a silent omission inside the HTTP client would be worse than an
    // absent flag. (The API key is one that was added, and it is not an argument either: see
    // the_api_key_never_reaches_the_command_line.)
    KD_CHECK(!contains(cmd, L"--api-key"));
    KD_CHECK(!contains(cmd, L"--backend"));
    KD_CHECK(!contains(cmd, L"--silero-model"));
    KD_CHECK(!contains(cmd, L"--hostile-stability"));
    // And no place for one: there is no environment-block mapping and no models.json.
}

KD_TEST(translator, the_api_key_never_reaches_the_command_line) {
    // Any process running as this user can read another's command line — Task Manager shows it
    // in a column. So the key adds a bare switch and nothing else; `launch` writes it into the
    // child's stdin.
    kd::TranslatorSpec spec;
    spec.service_url = "https://openrouter.ai/api";
    spec.api_key = "sk-or-v1-secret-0123456789";
    const std::wstring cmd = kd::build_translator_command_line(
        L"C:\\Program Files\\kvit-dictate\\kvit-asrd.exe", spec, 10500, 4242);
    KD_CHECK(contains(cmd, L"--api-key-stdin"));
    KD_CHECK(!contains(cmd, L"secret"));
    KD_CHECK(!contains(cmd, L"sk-or-v1"));

    // And no key, no switch: a child told to read stdin would read NUL and refuse to start.
    KD_CHECK(!contains(command_line_for("http://h:1"), L"--api-key-stdin"));
}

KD_TEST(translator, a_key_that_would_be_cut_at_a_line_break_is_refused_before_launch) {
    // The child reads one line, so a key with a newline in it would arrive truncated and fail as
    // "refused" with nothing to say why. start() says why instead, before spawning anything.
    kd::Translator translator;
    kd::TranslatorSpec spec;
    spec.service_url = "http://127.0.0.1:9";
    spec.api_key = "sk-1\nsk-2";
    const kd::Result<std::string> started = translator.start(spec);
    KD_CHECK(!started);
    KD_CHECK(!translator.running());
}

KD_TEST(translator, the_api_key_reaches_the_child_through_its_stdin) {
    // The handoff itself, which no in-process test can see: a real kvit-asrd, launched the way
    // the app launches it, must put the key it was piped on the wire as a bearer token. The
    // check at load is the request this looks at; it happens before `ready`, so a probe that
    // becomes ready means the service saw — and accepted — the key.
    if (!kd::Translator::image_path()) {
        KD_INFO("skipped: kvit-asrd.exe is not beside kd_tests.exe");
        KD_CHECK(true);
        return;
    }
    RecordingService service(400, R"({"error":"Missing required \"file\" field"})");
    KD_REQUIRE(service.start());

    {
        kd::Translator translator;
        kd::TranslatorSpec spec;
        spec.service_url = service.url();
        spec.api_key = "sk-or-v1-piped-0123456789";
        const kd::Result<std::string> started = translator.start(spec);
        KD_REQUIRE(started.is_ok());

        kd::ServerEndpoint ep;
        ep.url = kd::utf8::to_utf16_or_empty(*started);
        const kd::Result<kd::ServerProbe> probe = kd::probe_server(ep, 15000);
        if (!probe) KD_FAIL("the translator did not become ready: " + probe.status().message());
    }
    KD_CHECK(service.heads().find("\r\nAuthorization: Bearer sk-or-v1-piped-0123456789\r\n") !=
             std::string::npos);
}

KD_TEST(translator, a_refused_key_is_what_the_test_button_reports) {
    if (!kd::Translator::image_path()) {
        KD_INFO("skipped: kvit-asrd.exe is not beside kd_tests.exe");
        KD_CHECK(true);
        return;
    }
    RecordingService service(401, R"({"error":{"message":"User not found.","code":401}})");
    KD_REQUIRE(service.start());

    kd::Translator translator;
    kd::TranslatorSpec spec;
    spec.service_url = service.url();
    spec.api_key = "sk-or-v1-wrong";
    const kd::Result<std::string> started = translator.start(spec);
    KD_REQUIRE(started.is_ok());

    kd::ServerEndpoint ep;
    ep.url = kd::utf8::to_utf16_or_empty(*started);
    const kd::Result<kd::ServerProbe> probe = kd::probe_server(ep, 15000);
    KD_REQUIRE(!probe);
    KD_CHECK(probe.status().message().find("refused the API key") != std::string::npos);
}

KD_TEST(translator, a_model_name_with_a_space_is_quoted_not_split) {
    // A model id is free text from a settings box. It is a *value*, and the quoting is what
    // keeps it one — `CommandLineToArgvW`'s backslash-before-quote rule is not the intuitive
    // one, so this is asserted rather than assumed.
    const std::wstring cmd = command_line_for("http://h:1", "my model v2");
    KD_CHECK(contains(cmd, L"--model \"my model v2\""));

    const std::wstring quoted = command_line_for("http://h:1", "say \"hello\"");
    KD_CHECK(contains(quoted, L"\"say \\\"hello\\\"\""));
}

KD_TEST(translator, the_service_url_is_rejected_before_it_reaches_argv) {
    // Claim 3, and the line between "user-supplied content" and "a user-supplied argv vector".
    // `Translator::start()` runs this check before building anything; the guard itself lives in
    // settings.cpp so the dialog and the spawn cannot disagree about what is acceptable.
    KD_CHECK(!kd::validate_speech_service_url("http://host --service http://evil").empty());
    KD_CHECK(!kd::validate_speech_service_url("http://host\" --model x").empty());
    KD_CHECK(!kd::validate_speech_service_url("ws://127.0.0.1:10402/").empty());
    KD_CHECK(kd::validate_speech_service_url("http://strixhalo.lan:10301").empty());
}

// ─── The image path ─────────────────────────────────────────────────────────────────────────

KD_TEST(translator, the_image_is_looked_for_beside_this_executable_and_nowhere_else) {
    // Claim 1. In the test binary's own directory `kvit-asrd.exe` is present (CMake puts every
    // executable in one bin/), so this resolves; what matters is *where* it resolved.
    const kd::Result<std::filesystem::path> image = kd::Translator::image_path();
    if (!image) {
        // A build layout that does not put the two side by side. The failure must still name the
        // absolute path, because "the translator is missing" is not something a user can act on
        // and "this file is not where it should be" is.
        KD_CHECK(image.status().message().find("kvit-asrd.exe") != std::string::npos);
        KD_CHECK(image.status().code() == kd::StatusCode::NotFound);
        KD_INFO("kvit-asrd.exe is not beside kd_tests.exe: " + image.status().message());
        return;
    }
    KD_CHECK_EQ(image->filename().string(), std::string("kvit-asrd.exe"));

    wchar_t self[MAX_PATH] = {};
    KD_REQUIRE(GetModuleFileNameW(nullptr, self, MAX_PATH) != 0);
    KD_CHECK(image->parent_path() == std::filesystem::path(self).parent_path());
    KD_CHECK(image->is_absolute());
}

// ─── The port ───────────────────────────────────────────────────────────────────────────────

KD_TEST(translator, the_client_chooses_a_free_loopback_port) {
    // A fixed 10402 is deliberately not used for the supervised case: two clients, or a
    // leftover Python server, would collide — and a leftover Python server on that port is
    // exactly what this replaces.
    const kd::Result<uint16_t> a = kd::pick_loopback_port();
    KD_REQUIRE(a.is_ok());
    KD_CHECK_GT(*a, uint16_t{0});

    // Successive calls should not hand back the same port while the first is still notionally in
    // play. This is not a guarantee the OS makes, so a repeat is reported rather than failed —
    // start() retries on a fresh port anyway, which is what makes the TOCTOU window survivable.
    const kd::Result<uint16_t> b = kd::pick_loopback_port();
    KD_REQUIRE(b.is_ok());
    if (*a == *b) KD_INFO("the stack reused a port immediately; start() retries through that");
    KD_CHECK(true);
}

// ─── The whole spawn, when a speech service is named ────────────────────────────────────────

KD_TEST(translator, launches_and_supervises_a_real_child_when_a_service_is_named) {
    // openai-translator.md §7's T3 gate is "install from the installer, type an address, press
    // Test, dictate", and no automated test can be the dictating part. What this is, is the rest
    // of it: the client's spawn contract exercised end to end against a real speech service —
    // the port choice, the job object, the argv, the stderr pump, and the handshake that Test
    // itself performs.
    //
    // Opt-in, because it needs a host:
    //
    //     set KD_ASRD_SERVICE=http://strixhalo.lan:10301
    //     kd_tests --suite translator
    //
    // Skipped otherwise. A test that silently required somebody's LAN would fail on every other
    // machine and teach people to ignore a red suite.
    char service[512] = {};
    const DWORD n = GetEnvironmentVariableA("KD_ASRD_SERVICE", service, sizeof service);
    if (n == 0 || n >= sizeof service) {
        KD_INFO("skipped: set KD_ASRD_SERVICE to a speech service URL to run this");
        KD_CHECK(true);
        return;
    }
    if (!kd::Translator::image_path()) {
        KD_INFO("skipped: kvit-asrd.exe is not beside kd_tests.exe");
        KD_CHECK(true);
        return;
    }

    uint32_t child_pid = 0;
    std::string url;
    {
        kd::Translator translator;
        kd::TranslatorSpec spec;
        spec.service_url = service;
        // A hosted service wants both; see the same variables in test_asrd_session.cpp.
        char model[256] = {};
        DWORD got = GetEnvironmentVariableA("KD_ASRD_MODEL", model, sizeof model);
        if (got != 0 && got < sizeof model) spec.model = model;
        char key[1100] = {};
        got = GetEnvironmentVariableA("KD_ASRD_API_KEY", key, sizeof key);
        if (got != 0 && got < sizeof key) spec.api_key = key;

        const kd::Result<std::string> started = translator.start(spec);
        if (!started) {
            KD_FAIL("could not start the translator: " + started.status().message());
            return;
        }
        url = *started;
        child_pid = translator.pid();
        KD_CHECK(translator.running());
        KD_CHECK_GT(translator.port(), uint16_t{0});
        KD_CHECK_EQ(translator.url(), url);
        // Loopback, chosen by this process. Never the fixed 10402, which a leftover Python
        // server would already be holding.
        KD_CHECK(url.rfind("ws://127.0.0.1:", 0) == 0);
        KD_INFO("kvit-asrd pid " + std::to_string(child_pid) + " on " + url);

        // The same handshake the Test button runs, against the child this process launched.
        // `ready` is only emitted after asrd's own TCP connect to the speech host succeeded, so
        // this passing means the whole path answered.
        kd::ServerEndpoint ep;
        ep.url = kd::utf8::to_utf16_or_empty(url);
        const kd::Result<kd::ServerProbe> probe = kd::probe_server(ep, 30000);
        if (!probe) {
            KD_FAIL("the translator did not become ready: " + probe.status().message());
        } else {
            KD_CHECK(probe->mode == kd::proto::StreamingMode::PseudoStreaming);
            KD_CHECK(!probe->model.empty());
            KD_INFO("ready in " + std::to_string(probe->ready_ms) + " ms as " + probe->model);
        }
    }

    // Out of scope: the destructor closed the job, and closing a job with
    // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE kills everything in it. A translator that outlived its
    // client would hold a loopback port and, worse, a microphone-adjacent process nobody can see.
    KD_REQUIRE_GT(child_pid, uint32_t{0});
    const HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 child_pid);
    if (h == nullptr) {
        KD_CHECK(true);  // already reaped; the pid is gone, which is the same conclusion
        return;
    }
    const DWORD waited = WaitForSingleObject(h, 3000);
    DWORD code = STILL_ACTIVE;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    if (waited != WAIT_OBJECT_0) {
        KD_FAIL("kvit-asrd pid " + std::to_string(child_pid) +
                " is still running after its supervisor was destroyed");
    } else {
        KD_CHECK(true);
    }
}

// ─── This PC (ServerMode::ThisPc) ───────────────────────────────────────────────────────────

KD_TEST(translator, on_this_pc_the_command_line_names_the_backend_and_nothing_typed) {
    // Whatever the other modes left in the spec, none of it may reach the child: no address, no
    // model, no key, and no live preview the model cannot keep up with.
    kd::TranslatorSpec spec;
    spec.on_device = true;
    spec.service_url = "http://stale.lan:10301";
    spec.model = "Qwen/Qwen3-ASR-1.7B";
    spec.api_key = "sk-or-v1-0123456789abcdef";
    spec.live_preview = true;
    const std::wstring cmd = kd::build_translator_command_line(
        L"C:\\Program Files\\kvit-dictate\\kvit-asrd.exe", spec, 10500, 4242);
    KD_CHECK(contains(cmd, L"--backend windows-speech"));
    KD_CHECK(contains(cmd, L"--finalize-only on"));
    KD_CHECK(contains(cmd, L"--listen 127.0.0.1:10500"));
    KD_CHECK(contains(cmd, L"--parent-pid 4242"));
    KD_CHECK(!contains(cmd, L"--service"));
    KD_CHECK(!contains(cmd, L"--model"));
    KD_CHECK(!contains(cmd, L"--api-key"));
    KD_CHECK(!contains(cmd, L"stale.lan"));
    // The helper's path is the translator's fixed default; the client never names it.
    KD_CHECK(!contains(cmd, L"--speech-helper"));
    // A dictation never downloads the model; only Test asks for that.
    KD_CHECK(!contains(cmd, L"--speech-install-model"));
    spec.install_speech_model = true;
    KD_CHECK(contains(kd::build_translator_command_line(L"C:\\k\\kvit-asrd.exe", spec, 1, 2),
                      L"--speech-install-model"));
}
