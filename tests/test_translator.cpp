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

#include <windows.h>

#include "ipc_client.h"
#include "kd_test.h"
#include "settings.h"
#include "util/utf8.h"

#include <string>

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
    // absent flag.
    KD_CHECK(!contains(cmd, L"--api-key"));
    KD_CHECK(!contains(cmd, L"--backend"));
    KD_CHECK(!contains(cmd, L"--silero-model"));
    KD_CHECK(!contains(cmd, L"--hostile-stability"));
    // And no place for one: there is no environment-block mapping and no models.json.
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
