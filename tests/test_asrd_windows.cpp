// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// `--backend windows-speech` (src/asrd/backend_windows.cpp), against tests/fake_speech: the
// helper's framing, its startup failures, cancellation, and reuse across connections.
//
// A fake, not the real `kvit-speech.exe`, because the real one needs the optional Windows AI
// component, a .NET runtime and a 160 MB Windows App SDK folder, and a test that only runs on the
// author's machine is a test that stops running. The end-to-end cases through the real client
// are in test_asrd_session.cpp, beside the harness they share.

#include "kd_test.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "asrd/backend_windows.h"

namespace {

using namespace kd::asrd;

std::filesystem::path fake_exe() { return std::filesystem::path(KD_FAKE_SPEECH_EXE); }

SpeechHelper::Spec fake(std::vector<std::wstring> args = {}, uint32_t startup_ms = 10000) {
    SpeechHelper::Spec spec;
    spec.exe = fake_exe();
    spec.args = std::move(args);
    spec.startup_timeout_ms = startup_ms;
    return spec;
}

std::vector<int16_t> tone_ms(uint32_t ms, int16_t amplitude = 4000) {
    std::vector<int16_t> pcm(static_cast<size_t>(ms) * 16);
    for (size_t i = 0; i < pcm.size(); ++i) {
        pcm[i] = static_cast<int16_t>((i / 8) % 2 == 0 ? amplitude : -amplitude);
    }
    return pcm;
}

const std::atomic<bool> kNeverCancel{false};

}  // namespace

// ─── The gate's arithmetic ──────────────────────────────────────────────────────────────────

KD_TEST(asrd_windows, pcm_peak_is_a_magnitude_and_survives_the_most_negative_sample) {
    const int16_t quiet[] = {0, 3, -7, 5};
    KD_CHECK_EQ(pcm_peak(quiet, 4), 7u);
    const int16_t floor[] = {100, -32768, 200};
    KD_CHECK_EQ(pcm_peak(floor, 3), 32768u);
    KD_CHECK_EQ(pcm_peak(nullptr, 0), 0u);
}

KD_TEST(asrd_windows, the_helper_is_installed_beside_the_translator) {
    const std::filesystem::path p = default_speech_helper_path();
    KD_CHECK_EQ(p.filename().wstring(), std::wstring(L"kvit-speech.exe"));
    KD_CHECK_EQ(p.parent_path().filename().wstring(), std::wstring(L"kvit-speech"));
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    KD_CHECK(p.parent_path().parent_path() == std::filesystem::path(self).parent_path());
}

// ─── Startup ────────────────────────────────────────────────────────────────────────────────

KD_TEST(asrd_windows, a_ready_helper_answers_and_trims_what_it_heard) {
    SpeechHelper helper(fake());
    KD_REQUIRE(helper.ensure_ready().ok());
    KD_CHECK(helper.ready_info().find("fake-speech") != std::string::npos);

    const std::vector<int16_t> pcm = tone_ms(1500);
    std::string text;
    KD_REQUIRE(helper.transcribe(pcm.data(), pcm.size(), 5000, kNeverCancel, text).ok());
    // The fake pads with spaces; SpeechHelper hands over the payload verbatim and the backend
    // trims, so here the padding is still there.
    KD_CHECK_EQ(text, std::string(" heard 1500 ms "));

    // A second request on the same process: the link survives a round trip.
    KD_REQUIRE(helper.transcribe(pcm.data(), 320, 5000, kNeverCancel, text).ok());
    KD_CHECK_EQ(text, std::string(" heard 20 ms "));
    KD_CHECK_EQ(helper.launches(), uint64_t{1});
}

KD_TEST(asrd_windows, a_missing_helper_says_which_file_is_missing) {
    SpeechHelper::Spec spec = fake();
    spec.exe = fake_exe().parent_path() / L"no-such-dir" / L"kvit-speech.exe";
    SpeechHelper helper(spec);
    const kd::Status st = helper.ensure_ready();
    KD_REQUIRE(!st.ok());
    KD_CHECK_EQ(st.code(), kd::StatusCode::NotFound);
    KD_CHECK(st.message().find("no-such-dir") != std::string::npos);
    KD_CHECK(st.message().find("is missing") != std::string::npos);
}

KD_TEST(asrd_windows, a_model_that_is_not_installed_is_reported_in_the_helpers_words) {
    SpeechHelper helper(fake({L"--fail-start", L"the Windows AI speech model is not installed"}));
    const kd::Status st = helper.ensure_ready();
    KD_REQUIRE(!st.ok());
    KD_CHECK(st.message().find("the Windows AI speech model is not installed") != std::string::npos);
}

KD_TEST(asrd_windows, a_helper_that_exits_before_ready_names_its_last_words) {
    SpeechHelper helper(fake({L"--exit-before-ready"}));
    const kd::Status st = helper.ensure_ready();
    KD_REQUIRE(!st.ok());
    KD_CHECK(st.message().find("exited before it was ready") != std::string::npos);
    KD_CHECK(st.message().find("no speech component package found") != std::string::npos);
    // The loader chatter is not a reason and must not be quoted as one.
    KD_CHECK(st.message().find("INT8 zp") == std::string::npos);
}

KD_TEST(asrd_windows, a_helper_that_never_becomes_ready_times_out_and_is_killed) {
    SpeechHelper helper(fake({L"--hang-start"}, 600));
    const auto t0 = std::chrono::steady_clock::now();
    const kd::Status st = helper.ensure_ready();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    KD_REQUIRE(!st.ok());
    KD_CHECK_EQ(st.code(), kd::StatusCode::Timeout);
    KD_CHECK_GE(ms, 500);
    KD_CHECK_LT(ms, 8000);
}

KD_TEST(asrd_windows, text_on_stdout_is_not_mistaken_for_a_frame) {
    SpeechHelper helper(fake({L"--junk-stdout"}));
    const kd::Status st = helper.ensure_ready();
    KD_REQUIRE(!st.ok());
    KD_CHECK(st.message().find("not a frame stream") != std::string::npos);
}

// ─── Decoding ───────────────────────────────────────────────────────────────────────────────

KD_TEST(asrd_windows, a_decode_error_fails_one_window_and_keeps_the_link) {
    SpeechHelper helper(fake({L"--error-on-transcribe", L"COMException 0x80004005"}));
    KD_REQUIRE(helper.ensure_ready().ok());
    const std::vector<int16_t> pcm = tone_ms(500);
    std::string text;
    const kd::Status st = helper.transcribe(pcm.data(), pcm.size(), 5000, kNeverCancel, text);
    KD_REQUIRE(!st.ok());
    KD_CHECK(st.message().find("COMException 0x80004005") != std::string::npos);
    KD_CHECK(st.message().find("500 ms") != std::string::npos);
    // Still the same process, still answering.
    KD_CHECK(!helper.transcribe(pcm.data(), pcm.size(), 5000, kNeverCancel, text).ok());
    KD_CHECK_EQ(helper.launches(), uint64_t{1});
}

KD_TEST(asrd_windows, a_helper_that_dies_mid_decode_is_replaced_on_the_next_request) {
    SpeechHelper helper(fake({L"--die-on-transcribe"}));
    KD_REQUIRE(helper.ensure_ready().ok());
    const std::vector<int16_t> pcm = tone_ms(500);
    std::string text;
    const kd::Status st = helper.transcribe(pcm.data(), pcm.size(), 5000, kNeverCancel, text);
    KD_REQUIRE(!st.ok());
    KD_CHECK(st.message().find("exited mid-decode") != std::string::npos);
    // The next call starts a fresh one rather than writing into a dead pipe.
    KD_CHECK(!helper.transcribe(pcm.data(), pcm.size(), 5000, kNeverCancel, text).ok());
    KD_CHECK_EQ(helper.launches(), uint64_t{2});
}

KD_TEST(asrd_windows, a_decode_past_its_timeout_kills_the_helper) {
    SpeechHelper helper(fake({L"--delay-ms", L"3000"}));
    KD_REQUIRE(helper.ensure_ready().ok());
    const std::vector<int16_t> pcm = tone_ms(500);
    std::string text;
    const kd::Status st = helper.transcribe(pcm.data(), pcm.size(), 300, kNeverCancel, text);
    KD_REQUIRE(!st.ok());
    KD_CHECK_EQ(st.code(), kd::StatusCode::Timeout);
}

KD_TEST(asrd_windows, a_cancelled_decode_does_not_shift_the_next_answer) {
    // The helper cannot be interrupted, so the cancelled request's answer still arrives. If it
    // were read as the answer to the next question, every transcript after a cancel would be
    // the previous one's — the bug this case exists to rule out.
    SpeechHelper helper(fake({L"--delay-ms", L"400"}));
    KD_REQUIRE(helper.ensure_ready().ok());

    std::atomic<bool> cancel{false};
    const std::vector<int16_t> first = tone_ms(1000);
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cancel.store(true);
    });
    std::string text;
    const auto t0 = std::chrono::steady_clock::now();
    const kd::Status st = helper.transcribe(first.data(), first.size(), 5000, cancel, text);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    canceller.join();
    KD_REQUIRE(!st.ok());
    KD_CHECK_LT(ms, 350);  // returned on the cancel, not on the answer

    const std::vector<int16_t> second = tone_ms(250);
    KD_REQUIRE(helper.transcribe(second.data(), second.size(), 5000, kNeverCancel, text).ok());
    KD_CHECK_EQ(text, std::string(" heard 250 ms "));
    KD_CHECK_EQ(helper.launches(), uint64_t{1});
}

KD_TEST(asrd_windows, the_backend_trims_and_gates_silence_without_asking_the_model) {
    const std::filesystem::path counts =
        std::filesystem::temp_directory_path() /
        (L"kd_fake_speech_" + std::to_wstring(GetCurrentProcessId()) + L".txt");
    std::filesystem::remove(counts);

    WindowsSpeechConfig cfg;
    cfg.helper = fake_exe();
    auto helper = std::make_shared<SpeechHelper>(fake({L"--count-file", counts.wstring()}));
    WindowsSpeechBackend backend(cfg, helper);
    KD_REQUIRE(backend.load().ok());

    // Speech-level audio: decoded, and the padding the model returns is gone.
    const std::vector<int16_t> pcm = tone_ms(800);
    backend.feed(pcm.data(), pcm.size());
    Transcription out;
    KD_REQUIRE(backend.transcribe(true, out).ok());
    KD_CHECK(out.have_text);
    KD_CHECK_EQ(out.text, std::string("heard 800 ms"));
    KD_CHECK(!out.have_cut);  // no timestamps, so never a cut

    // Below the peak gate: an empty answer, and the helper is never asked. Whisper over this
    // window would have written a sentence.
    backend.reset();
    const std::vector<int16_t> hush = tone_ms(2000, 150);
    backend.feed(hush.data(), hush.size());
    KD_REQUIRE(backend.transcribe(true, out).ok());
    KD_CHECK(out.have_text);
    KD_CHECK(out.text.empty());
    KD_CHECK_EQ(backend.silence_skipped(), uint64_t{1});
    KD_CHECK_EQ(backend.requests(), uint64_t{1});

    helper->stop();
    std::ifstream in(counts);
    size_t lines = 0;
    for (std::string line; std::getline(in, line);) ++lines;
    KD_CHECK_EQ(lines, size_t{1});
    in.close();
    std::filesystem::remove(counts);
}

KD_TEST(asrd_windows, vocabulary_is_accepted_and_not_sent) {
    WindowsSpeechConfig cfg;
    WindowsSpeechBackend backend(cfg, std::make_shared<SpeechHelper>(fake()));
    backend.set_context({"llama.cpp", "sm_120"});
    KD_CHECK(backend.context_prompt().empty());
}

KD_TEST(asrd_windows, one_helper_per_process_however_many_connections) {
    release_speech_helpers();
    const SpeechHelper::Spec spec = fake();
    auto a = shared_speech_helper(spec);
    auto b = shared_speech_helper(spec);
    KD_CHECK(a.get() == b.get());
    KD_REQUIRE(a->ensure_ready().ok());
    KD_REQUIRE(b->ensure_ready().ok());
    KD_CHECK_EQ(a->launches(), uint64_t{1});
    // Different arguments are a different helper: a test's misbehaving fake must not be shared.
    auto c = shared_speech_helper(fake({L"--answer", L"x"}));
    KD_CHECK(c.get() != a.get());
    release_speech_helpers();
}
