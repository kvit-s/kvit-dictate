// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// `--backend nemotron` and the catalog download (src/asrd/backend_nemotron.h, model_download.h).
//
// Two kinds of case. The first half needs nothing: the registry answer, the pins, the folder
// check and the refusals all run anywhere. The second half needs the real runtime and model,
// which are 760 MB and not in the repository, and runs only when they are given:
//
//   KD_NEMOTRON_RUNTIME   folder with onnxruntime-genai.dll, onnxruntime.dll and the VC++ DLLs
//   KD_NEMOTRON_MODEL     the model folder; default %LOCALAPPDATA%\kvit-dictate\models\...
//   KD_NEMOTRON_WAV       optional: a 16 kHz mono s16 recording of someone speaking
//
// Without them those cases say so on stderr and pass. A case that silently passed *because* the
// model was absent would be worse than none, so each one prints which it did.

#include "kd_test.h"

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "asrd/backend_nemotron.h"
#include "asrd/connection.h"
#include "model_download.h"
#include "asrd/ws_server.h"
#include "ipc_client.h"
#include "protocol.h"
#include "util/utf8.h"

namespace {

using namespace kd;
using namespace kd::asrd;
using kd::Result;
using kd::Status;
namespace fs = std::filesystem;

fs::path env_path(const wchar_t* name) {
    wchar_t buf[4096];
    const DWORD n = GetEnvironmentVariableW(name, buf, 4096);
    if (n == 0 || n >= 4096) return {};
    return fs::path(std::wstring(buf, n));
}

fs::path fresh_dir(const char* name) {
    const fs::path dir = fs::temp_directory_path() / (std::string("kd_nemotron_") + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void write_file(const fs::path& p, const std::string& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << bytes;
}

// The real runtime and model, or false with a line on stderr saying the case did not run.
bool have_real_model(NemotronConfig& cfg, const char* test) {
    cfg.runtime_dir = env_path(L"KD_NEMOTRON_RUNTIME");
    const fs::path model = env_path(L"KD_NEMOTRON_MODEL");
    cfg.model_dir = model.empty() ? default_nemotron_model_dir() : model;
    std::error_code ec;
    if (cfg.runtime_dir.empty() || !fs::exists(cfg.runtime_dir / L"onnxruntime-genai.dll", ec) ||
        !verify_model_dir(nemotron_en_catalog_model(), cfg.model_dir, false).ok()) {
        std::fprintf(stderr, "  [asrd_nemotron] %s: NOT RUN, no KD_NEMOTRON_RUNTIME/model\n", test);
        return false;
    }
    return true;
}

// 16 kHz mono s16 only, which is what the corpus records.
std::vector<int16_t> read_wav(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<int16_t> pcm;
    size_t at = 12;
    while (at + 8 <= bytes.size()) {
        const std::string id = bytes.substr(at, 4);
        uint32_t size = 0;
        std::memcpy(&size, bytes.data() + at + 4, 4);
        if (id == "data") {
            const size_t n = (std::min<size_t>)(size, bytes.size() - at - 8) / 2;
            pcm.resize(n);
            std::memcpy(pcm.data(), bytes.data() + at + 8, n * 2);
            break;
        }
        at += 8 + size + (size & 1);
    }
    return pcm;
}

// ─── The registry's answer ──────────────────────────────────────────────────────────────────

KD_TEST(asrd_nemotron, the_registry_answer_gives_the_container_the_sas_and_the_folder) {
    const char* json = R"({"modelEntity":{"name":"x","kvTags":{"directoryPath":"v3"}},)"
                       R"("blobSasUri":"https://acct.blob.core.windows.net/azureml-abc?sv=1&sig=Z%2B"})";
    Result<RegistryLocation> loc = parse_registry_answer(json);
    KD_REQUIRE(loc.is_ok());
    KD_CHECK_EQ(loc->container_url, std::string("https://acct.blob.core.windows.net/azureml-abc"));
    KD_CHECK_EQ(loc->sas_query, std::string("sv=1&sig=Z%2B"));
    KD_CHECK_EQ(loc->directory, std::string("v3"));
}

KD_TEST(asrd_nemotron, a_registry_answer_without_a_usable_sas_is_refused) {
    KD_CHECK(!parse_registry_answer("not json").is_ok());
    KD_CHECK(!parse_registry_answer(R"({"modelEntity":{}})").is_ok());
    // Plain http, or no query at all, is not a read-only SAS on a blob container.
    KD_CHECK(!parse_registry_answer(R"({"blobSasUri":"http://a.blob.core.windows.net/c?sig=1"})")
                  .is_ok());
    KD_CHECK(!parse_registry_answer(R"({"blobSasUri":"https://a.blob.core.windows.net/c"})")
                  .is_ok());
}

// ─── The pins ───────────────────────────────────────────────────────────────────────────────

KD_TEST(asrd_nemotron, the_pinned_model_is_fourteen_files_and_728_mb) {
    const CatalogModel& m = nemotron_en_catalog_model();
    KD_CHECK_EQ(m.file_count, static_cast<size_t>(14));
    KD_CHECK_EQ(m.total_bytes(), static_cast<uint64_t>(728007158));
    KD_CHECK_EQ(std::string(m.directory), std::string("v3"));
    for (size_t i = 0; i < m.file_count; ++i) {
        KD_CHECK_EQ(std::strlen(m.files[i].sha256), static_cast<size_t>(64));
        for (size_t j = i + 1; j < m.file_count; ++j) {
            KD_CHECK(std::string(m.files[i].name) != m.files[j].name);
        }
    }
}

KD_TEST(asrd_nemotron, sha256_of_a_file_is_lowercase_hex) {
    const fs::path dir = fresh_dir("sha");
    write_file(dir / "abc", "abc");
    Result<std::string> h = sha256_file(dir / "abc");
    KD_REQUIRE(h.is_ok());
    KD_CHECK_EQ(*h, std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

KD_TEST(asrd_nemotron, a_model_folder_is_checked_by_size_and_then_by_hash) {
    static const CatalogFile files[] = {
        {"a.txt", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"b.txt", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    };
    const CatalogModel m{"azureml://test", "v1", files, 2};
    const fs::path dir = fresh_dir("verify");

    Status st = verify_model_dir(m, dir, false);
    KD_CHECK(!st.ok());
    KD_CHECK(st.message().find("a.txt is missing") != std::string::npos);

    write_file(dir / "a.txt", "abd");  // the right size, the wrong bytes
    write_file(dir / "b.txt", "");
    KD_CHECK(verify_model_dir(m, dir, false).ok());
    st = verify_model_dir(m, dir, true);
    KD_CHECK(!st.ok());
    KD_CHECK(st.message().find("SHA-256") != std::string::npos);

    write_file(dir / "a.txt", "abc");
    KD_CHECK(verify_model_dir(m, dir, true).ok());

    write_file(dir / "a.txt", "abcd");
    st = verify_model_dir(m, dir, false);
    KD_CHECK(!st.ok());
    KD_CHECK(st.message().find("expected 3") != std::string::npos);
}

// ─── Refusals in the user's words ───────────────────────────────────────────────────────────

KD_TEST(asrd_nemotron, a_missing_runtime_names_the_dll_and_what_to_do) {
    NemotronConfig cfg;
    cfg.runtime_dir = fresh_dir("no_runtime");
    cfg.model_dir = fresh_dir("no_model");
    NemotronBackend backend(cfg);
    const Status st = backend.load();
    KD_CHECK(!st.ok());
    KD_CHECK(st.message().find("onnxruntime-genai.dll is missing") != std::string::npos);
    KD_CHECK(st.message().find("Settings > Server") != std::string::npos);
}

KD_TEST(asrd_nemotron, a_missing_model_points_at_the_download_button_and_downloads_nothing) {
    NemotronConfig cfg;
    if (!have_real_model(cfg, "a_missing_model_points_at_the_download_button")) return;
    cfg.model_dir = fresh_dir("empty_model");
    NemotronBackend backend(cfg);
    const Status st = backend.load();
    KD_CHECK(!st.ok());
    KD_CHECK(st.message().find("Download Nemotron model") != std::string::npos);
    KD_CHECK(st.message().find("728 MB") != std::string::npos);
    // install_model was off: nothing may have been fetched or even started.
    std::error_code ec;
    KD_CHECK(!fs::exists(cfg.model_dir.wstring() + L".partial", ec));
}

// ─── The real model ─────────────────────────────────────────────────────────────────────────

KD_TEST(asrd_nemotron, silence_decodes_to_nothing) {
    NemotronConfig cfg;
    if (!have_real_model(cfg, "silence_decodes_to_nothing")) return;
    NemotronBackend backend(cfg);
    KD_REQUIRE(backend.load().ok());
    backend.reset();
    const std::vector<int16_t> silence(16000 * 3, 0);
    for (size_t at = 0; at < silence.size(); at += 320) backend.feed(silence.data() + at, 320);
    Transcription out;
    KD_REQUIRE(backend.transcribe(true, out).ok());
    KD_CHECK(out.have_text);
    KD_CHECK_EQ(out.text, std::string());
    KD_CHECK(backend.chunks_decoded() >= 5);  // 3 s is five whole 560 ms chunks
}

KD_TEST(asrd_nemotron, speech_streams_while_it_is_fed_and_the_final_follows_quickly) {
    NemotronConfig cfg;
    if (!have_real_model(cfg, "speech_streams_while_it_is_fed")) return;
    const fs::path wav = env_path(L"KD_NEMOTRON_WAV");
    std::vector<int16_t> pcm;
    if (!wav.empty()) pcm = read_wav(wav);
    if (pcm.size() < 16000 * 2) {
        std::fprintf(stderr, "  [asrd_nemotron] speech_streams_while_it_is_fed: NOT RUN, no "
                             "KD_NEMOTRON_WAV\n");
        return;
    }
    cfg.finalize_only = false;
    NemotronBackend backend(cfg);
    KD_REQUIRE(backend.load().ok());
    backend.reset();

    // At microphone pace, so the measurement is of the backend and not of a backlog.
    size_t hops = 0;
    size_t partial_bytes = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t at = 0; at < pcm.size(); at += 320) {
        const size_t n = (std::min<size_t>)(320, pcm.size() - at);
        if (backend.feed(pcm.data() + at, n)) {
            ++hops;
            Transcription partial;
            KD_REQUIRE(backend.transcribe(false, partial).ok());
            // Its text only grows: each partial is longer than or equal to the last.
            KD_CHECK(partial.text.size() + 1 >= partial_bytes);
            partial_bytes = partial.text.size();
        }
        std::this_thread::sleep_until(t0 + std::chrono::microseconds((at + n) * 1000000 / 16000));
    }
    const auto stopped = std::chrono::steady_clock::now();
    Transcription out;
    KD_REQUIRE(backend.transcribe(true, out).ok());
    const auto tail_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - stopped)
                             .count();
    std::fprintf(stderr,
                 "  [asrd_nemotron] %.1f s of speech: %zu hops, final %zu bytes, %lld ms after "
                 "the last audio\n",
                 static_cast<double>(pcm.size()) / 16000.0, hops, out.text.size(),
                 static_cast<long long>(tail_ms));
    KD_CHECK(hops > 0);
    KD_CHECK(out.text.size() >= partial_bytes);
    KD_CHECK(!out.text.empty());
    // One partial chunk and one decode; a second is a stalled worker.
    KD_CHECK(tail_ms < 1000);
}

// ─── Through the protocol, the way the client meets it ──────────────────────────────────────

struct Seen {
    std::mutex m;
    std::condition_variable cv;
    bool ready = false;
    bool stopped = false;
    bool dead = false;
    size_t errors = 0;
    size_t partials = 0;
    std::vector<std::string> finals;
    kd::proto::ReadyMsg ready_msg;
    std::string error_text;
};

KD_TEST(asrd_nemotron, a_session_over_the_wire_is_native_streaming_and_ends_in_a_final) {
    NemotronConfig ncfg;
    if (!have_real_model(ncfg, "a_session_over_the_wire")) return;
    const fs::path wav = env_path(L"KD_NEMOTRON_WAV");
    std::vector<int16_t> pcm;
    if (!wav.empty()) pcm = read_wav(wav);
    if (pcm.size() < 16000 * 2) {
        std::fprintf(stderr, "  [asrd_nemotron] a_session_over_the_wire: NOT RUN, no "
                             "KD_NEMOTRON_WAV\n");
        return;
    }
    // Silence after the speech, so the endpointer closes the utterance on its own.
    pcm.insert(pcm.end(), 16000 * 2, 0);

    TranslatorConfig cfg;
    cfg.backend = BackendKind::Nemotron;
    cfg.model = kNemotronModel;
    cfg.nemotron = ncfg;
    cfg.finalize_only = false;
    cfg.stability.hold_ms_t = 0;

    WsListener listener;
    KD_REQUIRE(listener.listen("127.0.0.1", 0).ok());
    std::thread server([&] {
        bool retry = false;
        std::unique_ptr<WsConnection> conn = listener.accept(retry);
        if (conn != nullptr) {
            serve_connection(*conn, cfg);
            conn->close();
        }
    });

    Seen seen;
    kd::IpcClient client;
    kd::IpcClient::Callbacks cb;
    cb.on_message = [&seen](const kd::proto::Message& msg) {
        std::lock_guard<std::mutex> lock(seen.m);
        if (const auto* r = msg.get_if<kd::proto::ReadyMsg>()) {
            seen.ready = true;
            seen.ready_msg = *r;
        }
        if (msg.get_if<kd::proto::PartialMsg>() != nullptr) ++seen.partials;
        if (const auto* f = msg.get_if<kd::proto::FinalMsg>()) seen.finals.push_back(f->text);
        if (msg.get_if<kd::proto::StoppedMsg>() != nullptr) seen.stopped = true;
        if (const auto* e = msg.get_if<kd::proto::ErrorMsg>()) {
            ++seen.errors;
            seen.error_text = e->msg;
        }
        seen.cv.notify_all();
    };
    cb.on_error = [&seen](const kd::proto::ProtocolError&) {
        std::lock_guard<std::mutex> lock(seen.m);
        seen.dead = true;
        seen.cv.notify_all();
    };
    cb.on_disconnected = [&seen]() {
        std::lock_guard<std::mutex> lock(seen.m);
        seen.dead = true;
        seen.cv.notify_all();
    };
    kd::ServerEndpoint endpoint;
    endpoint.url = kd::utf8::to_utf16_or_empty("ws://127.0.0.1:" + std::to_string(listener.port()) + "/");
    KD_REQUIRE(client.connect(endpoint, cb).ok());
    kd::proto::HelloMsg hello;
    hello.proto = kd::proto::kProtoVersion;
    hello.client = "kd_tests";
    KD_REQUIRE(client.send_control(kd::proto::Message::of(hello)).ok());
    const auto wait = [&seen](auto pred, int ms) {
        std::unique_lock<std::mutex> lock(seen.m);
        return seen.cv.wait_for(lock, std::chrono::milliseconds(ms), pred);
    };
    KD_REQUIRE(wait([&] { return seen.ready || seen.dead || seen.errors > 0; }, 30000));
    {
        std::lock_guard<std::mutex> lock(seen.m);
        if (!seen.ready) std::fprintf(stderr, "  error: %s\n", seen.error_text.c_str());
        KD_REQUIRE(seen.ready);
        KD_CHECK(seen.ready_msg.mode == kd::proto::StreamingMode::NativeStreaming);
        KD_CHECK_EQ(seen.ready_msg.chunk_ms.value_or(-1), 560);
        KD_CHECK(!seen.ready_msg.context_supported);
        KD_CHECK_EQ(seen.ready_msg.model, std::string(kNemotronModel));
    }

    kd::proto::StartMsg start;
    start.session_id = "s1";
    KD_REQUIRE(client.send_control(kd::proto::Message::of(start)).ok());
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t at = 0; at < pcm.size(); at += 320) {
        const size_t n = (std::min<size_t>)(320, pcm.size() - at);
        KD_REQUIRE(client.send_audio(pcm.data() + at, n).ok());
        if ((at / 320) % 25 == 24) {
            KD_REQUIRE(client.flush_audio(5000).ok());
            std::this_thread::sleep_until(t0 +
                                          std::chrono::microseconds((at + n) * 1000000 / 16000));
        }
    }
    client.flush_audio(5000);
    wait([&] { return !seen.finals.empty() || seen.errors > 0; }, 10000);
    kd::proto::StopMsg stop;
    stop.session_id = "s1";
    KD_REQUIRE(client.send_control(kd::proto::Message::of(stop)).ok());
    wait([&] { return seen.stopped; }, 10000);
    client.stop();
    listener.stop();
    server.join();

    std::lock_guard<std::mutex> lock(seen.m);
    std::fprintf(stderr, "  [asrd_nemotron] over the wire: %zu partials, %zu finals, %zu errors\n",
                 seen.partials, seen.finals.size(), seen.errors);
    KD_CHECK_EQ(seen.errors, static_cast<size_t>(0));
    KD_CHECK(seen.partials > 0);
    KD_CHECK(!seen.finals.empty());
    KD_CHECK(seen.stopped);
    size_t bytes = 0;
    for (const std::string& f : seen.finals) bytes += f.size();
    KD_CHECK(bytes > 0);
}

}  // namespace
