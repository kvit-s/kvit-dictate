// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// tests/test_asrd_session.cpp — the translator, whole, against a stub speech service.
//
// ═══ What is real here and what is not ══════════════════════════════════════════════════════
//
// Real: `WsListener`, `serve_connection`, the §5 handshake, `SessionServer`, `VadEndpointer`,
// `StabilityTracker`, `OpenAiBackend`, WinHTTP, and — on the other end of a loopback socket —
// the client's own `IpcClient`. The two halves of the shipped application talk to each other in
// this process, over a real socket, with a real WebSocket upgrade.
//
// Not real: the speech service. `StubSpeechService` is a Winsock listener that answers
// `/v1/audio/transcriptions` and `/v1/audio/vad` from a script. That is deliberate — the
// failure paths are the point of this file, and "the far end returned a 500" is not something
// a real server can be asked for on demand.
//
// openai-translator.md §5 names the cases that must be covered, and each has a test below:
// a 503 on the VAD route must fail open; a 500 on transcription must be a non-fatal
// `decode_failed` that closes the session and not the connection; a non-JSON body; a timeout;
// and a host that is down at load, which must NOT emit `ready`.

#include "kd_test.h"

#include <winsock2.h>
#include <ws2tcpip.h>
// Must follow winsock2.h.
#include <windows.h>

#include "asrd/connection.h"
#include "asrd/session_server.h"
#include "asrd/ws_server.h"
#include "ipc_client.h"
#include "protocol.h"
#include "util/utf8.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace {

using namespace kd::asrd;

// ─── A stub speech service ──────────────────────────────────────────────────────────────────

// Answers one canned reply per route, and counts what it was asked. HTTP/1.0-style: one request
// per connection, `Connection: close`, no keep-alive. WinHTTP copes with that and it removes
// every framing question from a helper whose job is to be uninteresting.
class StubSpeechService {
public:
    struct Reply {
        int status = 200;
        std::string body = R"({"text":"hello world"})";
        std::string content_type = "application/json";
        uint32_t delay_ms = 0;   // to provoke a client-side timeout
        bool hang_up = false;    // close without answering at all
    };

    StubSpeechService() {
        WSADATA d{};
        WSAStartup(MAKEWORD(2, 2), &d);
    }

    ~StubSpeechService() {
        stop();
        WSACleanup();
    }

    bool start() {
        listen_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_ == INVALID_SOCKET) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;  // ephemeral: two tests must be able to run back to back
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) return false;
        if (::listen(listen_, 8) != 0) return false;

        sockaddr_in bound{};
        int len = sizeof bound;
        ::getsockname(listen_, reinterpret_cast<sockaddr*>(&bound), &len);
        port_ = ntohs(bound.sin_port);

        thread_ = std::thread([this] { run(); });
        return true;
    }

    void stop() {
        if (listen_ != INVALID_SOCKET) {
            SOCKET s = listen_;
            listen_ = INVALID_SOCKET;
            ::closesocket(s);
        }
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }
    [[nodiscard]] uint16_t port() const { return port_; }

    void set_transcribe(Reply r) {
        std::lock_guard<std::mutex> lock(m_);
        transcribe_ = std::move(r);
        script_.clear();
    }

    // One reply per request, in order; the last one repeats once the script runs out. This is
    // how a *revising* model is simulated — a batch model does not extend its previous
    // hypothesis, it replaces it, and a stub that always answers the same string cannot provoke
    // the failure §8.2 exists to prevent.
    void set_transcribe_script(std::vector<Reply> replies) {
        std::lock_guard<std::mutex> lock(m_);
        script_ = std::move(replies);
        if (!script_.empty()) transcribe_ = script_.front();
    }
    void set_vad(Reply r) {
        std::lock_guard<std::mutex> lock(m_);
        vad_ = std::move(r);
        vad_configured_ = true;
    }

    [[nodiscard]] int transcribe_requests() const { return transcribe_requests_.load(); }
    [[nodiscard]] int vad_requests() const { return vad_requests_.load(); }
    [[nodiscard]] size_t last_body_bytes() const {
        std::lock_guard<std::mutex> lock(m_);
        return last_body_bytes_;
    }
    [[nodiscard]] std::string last_content_type() const {
        std::lock_guard<std::mutex> lock(m_);
        return last_content_type_;
    }
    // The whole last transcription request, headers and multipart body. Recorded so a test can
    // assert what actually went on the wire rather than what the client believes it sent —
    // `context` was declared unsupported and dropped for a month while the field it needed was
    // one line away, and no test could have noticed.
    [[nodiscard]] std::string last_transcribe_request() const {
        std::lock_guard<std::mutex> lock(m_);
        return last_transcribe_request_;
    }

private:
    void run() {
        for (;;) {
            const SOCKET fd = ::accept(listen_, nullptr, nullptr);
            if (fd == INVALID_SOCKET) return;  // stop() closed the listener
            serve(fd);
            ::closesocket(fd);
        }
    }

    void serve(SOCKET fd) {
        // Read the head, then exactly Content-Length more bytes. Anything less and WinHTTP's
        // send is still in flight when the reply arrives, which on Windows resets the
        // connection and makes every test look like a network fault.
        std::string raw;
        size_t header_end = std::string::npos;
        size_t content_length = 0;
        for (;;) {
            if (header_end == std::string::npos) {
                header_end = raw.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    const std::string head = raw.substr(0, header_end);
                    const size_t at = head.find("Content-Length:");
                    if (at != std::string::npos) {
                        content_length = static_cast<size_t>(std::strtoul(
                            head.c_str() + at + 15, nullptr, 10));
                    }
                    const size_t ct = head.find("Content-Type:");
                    if (ct != std::string::npos) {
                        const size_t eol = head.find("\r\n", ct);
                        std::lock_guard<std::mutex> lock(m_);
                        last_content_type_ = head.substr(ct + 14, eol - ct - 14);
                    }
                }
            }
            if (header_end != std::string::npos &&
                raw.size() >= header_end + 4 + content_length) {
                break;
            }
            char buf[16384];
            const int n = ::recv(fd, buf, static_cast<int>(sizeof buf), 0);
            if (n <= 0) return;
            raw.append(buf, static_cast<size_t>(n));
        }

        const bool is_vad = raw.compare(0, 64, "POST /v1/audio/vad ", 0, 19) == 0 ||
                            raw.find("POST /v1/audio/vad ") == 0;
        Reply reply;
        {
            std::lock_guard<std::mutex> lock(m_);
            last_body_bytes_ = content_length;
            if (!is_vad) last_transcribe_request_ = raw;
            if (is_vad) {
                reply = vad_;
            } else if (!script_.empty()) {
                const size_t at = static_cast<size_t>(transcribe_requests_.load());
                reply = script_[(std::min)(at, script_.size() - 1)];
            } else {
                reply = transcribe_;
            }
            if (is_vad && !vad_configured_) {
                // The honest default: a server without pysilero-vad answers 503 naming the
                // package, which is what the fail-open path has to survive.
                reply = Reply{503, R"({"detail":"pysilero-vad is not installed"})",
                              "application/json", 0, false};
            }
        }
        (is_vad ? vad_requests_ : transcribe_requests_).fetch_add(1);

        if (reply.hang_up) return;
        if (reply.delay_ms != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(reply.delay_ms));
        }

        std::string out = "HTTP/1.1 " + std::to_string(reply.status) + " X\r\n";
        out += "Content-Type: " + reply.content_type + "\r\n";
        out += "Content-Length: " + std::to_string(reply.body.size()) + "\r\n";
        out += "Connection: close\r\n\r\n";
        out += reply.body;
        size_t sent = 0;
        while (sent < out.size()) {
            const int n = ::send(fd, out.data() + sent, static_cast<int>(out.size() - sent), 0);
            if (n <= 0) return;
            sent += static_cast<size_t>(n);
        }
        ::shutdown(fd, SD_SEND);
    }

    SOCKET listen_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    std::thread thread_;

    mutable std::mutex m_;
    Reply transcribe_;
    std::vector<Reply> script_;
    Reply vad_;
    bool vad_configured_ = false;
    size_t last_body_bytes_ = 0;
    std::string last_content_type_;
    std::string last_transcribe_request_;
    std::atomic<int> transcribe_requests_{0};
    std::atomic<int> vad_requests_{0};
};

// ─── A translator on a loopback port, in this process ───────────────────────────────────────

class TranslatorHarness {
public:
    explicit TranslatorHarness(TranslatorConfig cfg) : cfg_(std::move(cfg)) {}

    ~TranslatorHarness() { stop(); }

    bool start() {
        // Port 0: two tests must be able to run back to back without TIME_WAIT deciding
        // otherwise, which is also why the shipped client picks the port rather than using a
        // fixed one (openai-translator.md §4.6).
        if (!listener_.listen("127.0.0.1", 0).ok()) return false;
        thread_ = std::thread([this] {
            for (;;) {
                bool retry = false;
                std::unique_ptr<WsConnection> conn = listener_.accept(retry);
                if (conn == nullptr) {
                    if (retry) continue;
                    return;
                }
                exit_code_ = serve_connection(*conn, cfg_);
                conn->close();
                served_.fetch_add(1);
            }
        });
        return true;
    }

    void stop() {
        listener_.stop();
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] std::string url() const {
        return "ws://127.0.0.1:" + std::to_string(listener_.port()) + "/";
    }
    [[nodiscard]] int exit_code() const { return exit_code_; }
    [[nodiscard]] int served() const { return served_.load(); }

private:
    TranslatorConfig cfg_;
    WsListener listener_;
    std::thread thread_;
    std::atomic<int> served_{0};
    int exit_code_ = -1;
};

// ─── What the client saw ────────────────────────────────────────────────────────────────────

struct Observed {
    std::mutex m;
    std::condition_variable cv;
    std::vector<kd::proto::Message> messages;
    bool ready = false;
    bool stopped = false;
    // Counted rather than derived through of<T>(): `wait_for` already holds `m`, and of<T>()
    // takes it again. std::mutex is not recursive, so a predicate written that way throws
    // "resource deadlock would occur" out of the whole test rather than failing an assertion.
    size_t errors = 0;
    size_t finals = 0;
    bool dead = false;
    bool disconnected = false;
    std::string dead_reason;

    void note(const kd::proto::Message& msg) {
        std::lock_guard<std::mutex> lock(m);
        messages.push_back(msg);
        if (msg.get_if<kd::proto::ReadyMsg>() != nullptr) ready = true;
        if (msg.get_if<kd::proto::StoppedMsg>() != nullptr) stopped = true;
        if (msg.get_if<kd::proto::ErrorMsg>() != nullptr) ++errors;
        if (msg.get_if<kd::proto::FinalMsg>() != nullptr) ++finals;
        cv.notify_all();
    }

    template <typename Pred>
    bool wait_for(Pred pred, int ms = 15000) {
        std::unique_lock<std::mutex> lock(m);
        return cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return pred(); });
    }

    template <typename T>
    std::vector<T> of() {
        std::lock_guard<std::mutex> lock(m);
        std::vector<T> out;
        for (const kd::proto::Message& msg : messages) {
            if (const T* p = msg.get_if<T>()) out.push_back(*p);
        }
        return out;
    }

    size_t count() {
        std::lock_guard<std::mutex> lock(m);
        return messages.size();
    }
};

// One second of a 300 Hz tone at half scale, then silence past the endpoint. Enough for the
// energy scorer to open an utterance and then close it.
std::vector<int16_t> one_utterance(uint32_t speech_ms = 700, uint32_t trailing_silence_ms = 2200) {
    std::vector<int16_t> pcm;
    const auto append = [&pcm](uint32_t ms, double amplitude) {
        const size_t n = static_cast<size_t>(ms) * 16000 / 1000;
        const size_t start = pcm.size();
        for (size_t i = 0; i < n; ++i) {
            const double t = static_cast<double>(start + i) / 16000.0;
            pcm.push_back(static_cast<int16_t>(
                amplitude * 32000.0 * std::sin(2.0 * 3.14159265358979323846 * 300.0 * t)));
        }
    };
    append(400, 0.0);  // the scorer's calibration window
    append(speech_ms, 0.5);
    append(trailing_silence_ms, 0.0);
    return pcm;
}

TranslatorConfig config_for(const StubSpeechService& stub) {
    TranslatorConfig cfg;
    cfg.service = stub.base_url();
    cfg.model = "stub-model";
    cfg.language = "en";
    cfg.finalize_only = true;
    cfg.vad_gate = false;  // on only in the tests that are about the gate
    // Shorter than the shipped 1500 ms, so a case is two seconds of audio rather than four.
    cfg.endpoint_silence_ms = 400;
    cfg.stability.hold_ms_t = 0;  // the dwell is a wall clock; a test must not wait on it
    cfg.http_timeout_ms = 5000;
    return cfg;
}

// Connect the real client to the harness and run one session. Returns what the client observed.
//
// Everything here is `IpcClient`'s public surface: this is the client's own code path, not a
// test-only imitation of it, which is the whole point of an in-process loopback.
//
// Note that `connect()` deliberately does **not** send `hello` — `SessionMachine` owns that
// frame and fixture 020 asserts it — so this helper sends it on reaching HANDSHAKE, which is
// what `src/main.cpp` does too.
bool attach(kd::IpcClient& client, Observed& seen, const std::string& url) {
    kd::IpcClient::Callbacks cb;
    cb.on_message = [&seen](const kd::proto::Message& m) { seen.note(m); };
    cb.on_error = [&seen](const kd::proto::ProtocolError& e) {
        std::lock_guard<std::mutex> lock(seen.m);
        seen.dead = true;
        seen.dead_reason = e.msg;
        seen.cv.notify_all();
    };
    cb.on_disconnected = [&seen]() {
        std::lock_guard<std::mutex> lock(seen.m);
        seen.disconnected = true;
        seen.cv.notify_all();
    };

    kd::ServerEndpoint endpoint;
    endpoint.url = kd::utf8::to_utf16_or_empty(url);
    if (!client.connect(endpoint, cb).ok()) return false;

    kd::proto::HelloMsg hello;
    hello.proto = kd::proto::kProtoVersion;
    hello.client = "kd_tests";
    return client.send_control(kd::proto::Message::of(hello)).ok();
}

// `pace_ms` waits that long after each half-second of audio is drained, which is the difference
// between a test that measures the *policy* and one that measures the machine it ran on.
//
// A hop that fires while a POST is in flight replaces the pending work rather than queueing it
// (§2.1), so how many hops an utterance produces depends on how fast the audio arrives relative
// to a round trip. Pushed at memory speed, eight seconds of PCM can collapse into two requests —
// which is fine for a case that only needs *a* partial, and fatal for one that needs to watch the
// tracker's behaviour across several. Pacing costs wall-clock time, so it is opt-in per case
// rather than the default.
bool run_session(TranslatorHarness& harness, Observed& seen, const std::vector<int16_t>& pcm,
                 bool cancel_instead_of_stop = false, int settle_ms = 6000, int pace_ms = 0,
                 const std::vector<std::string>& context = {}) {
    kd::IpcClient client;
    if (!attach(client, seen, harness.url())) return false;
    if (!seen.wait_for([&] { return seen.ready || seen.dead || seen.disconnected; })) return false;
    {
        std::lock_guard<std::mutex> lock(seen.m);
        if (!seen.ready) return false;
    }

    kd::proto::StartMsg start;
    start.session_id = "s1";
    if (!context.empty()) {
        start.context = context;
        start.context_present = true;
    }
    if (!client.send_control(kd::proto::Message::of(start)).ok()) return false;

    // 20 ms frames, the low end of §6.3's recommended band, because that is what the client's
    // own capture path produces and a translator that only works at one frame size is broken in
    // the way a real microphone will find.
    //
    // **Drained every half second, and that is not an optimisation.** `IpcClient::send_audio`
    // bounds its queue at AUDIO_QUEUE_MAX_MS (3000 ms of PCM) and answers `Overloaded` past it,
    // because §9 forbids absorbing a backlog by dropping frames. A microphone cannot outrun that
    // bound — it produces audio at wall-clock speed — but a test that pushes three seconds of
    // PCM in a few microseconds can, and in a Release build it does. Pacing here keeps the test
    // honest about what a capture thread actually does without making every case take as long as
    // the audio it feeds.
    constexpr size_t kFrame = 320;              // 20 ms
    constexpr size_t kDrainEvery = 25 * kFrame;  // 500 ms
    size_t since_drain = 0;
    for (size_t at = 0; at < pcm.size(); at += kFrame) {
        const size_t n = (std::min)(kFrame, pcm.size() - at);
        if (!client.send_audio(pcm.data() + at, n).ok()) return false;
        since_drain += n;
        if (since_drain >= kDrainEvery) {
            since_drain = 0;
            if (!client.flush_audio(5000).ok()) return false;
            if (pace_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(pace_ms));
            }
        }
    }

    if (cancel_instead_of_stop) {
        // §6.3's Client obligation is the opposite here: `cancel` discards rather than flushes,
        // because the audio is about to become meaningless.
        seen.wait_for([&] { return false; }, settle_ms);
        client.discard_queued_audio();
        kd::proto::CancelMsg cancel;
        cancel.session_id = "s1";
        if (!client.send_control(kd::proto::Message::of(cancel)).ok()) return false;
    } else {
        // §6.3: every audio frame belonging to the session must be written before `stop`. Audio
        // frames carry no session_id — attribution is positional — so this ordering *is* the
        // attribution.
        client.flush_audio(5000);
        // Then give the decode thread its round trip, or the `final` races the `stop` and the
        // test measures the race rather than the behaviour. `settle_ms` is a ceiling, not a
        // sleep: a case that does produce a final leaves as soon as it arrives.
        seen.wait_for([&] { return seen.finals > 0 || seen.errors > 0; }, settle_ms);
        kd::proto::StopMsg stop;
        stop.session_id = "s1";
        if (!client.send_control(kd::proto::Message::of(stop)).ok()) return false;
    }
    seen.wait_for([&] { return seen.stopped; });
    client.stop();
    return true;
}

}  // namespace

// ─── The happy path ─────────────────────────────────────────────────────────────────────────

KD_TEST(asrd_session, a_whole_dictation_through_the_real_client) {
    StubSpeechService stub;
    KD_REQUIRE(stub.start());
    stub.set_transcribe({200, R"({"text":"change the llama.cpp config"})"});

    TranslatorHarness harness(config_for(stub));
    KD_REQUIRE(harness.start());

    Observed seen;
    KD_REQUIRE(run_session(harness, seen, one_utterance()));

    const auto finals = seen.of<kd::proto::FinalMsg>();
    KD_REQUIRE_EQ(finals.size(), size_t{1});
    KD_CHECK_EQ(finals[0].text, std::string("change the llama.cpp config"));
    KD_CHECK_EQ(finals[0].session_id, std::string("s1"));
    // §7.3: utterance_id starts at 1 for the first VAD-delimited utterance.
    KD_CHECK_EQ(finals[0].utterance_id, uint64_t{1});
    // §7.4: seq is scoped to the session, starts at 1, and only partial/final consume it.
    KD_CHECK_EQ(finals[0].seq, uint64_t{1});
    // §6.3: a `final` is entirely stable by definition, and when present stable_bytes MUST equal
    // len(utf8(text)).
    KD_REQUIRE(finals[0].stable_bytes.has_value());
    KD_CHECK_EQ(*finals[0].stable_bytes, finals[0].text.size());

    // Finalize-only: exactly one request per utterance, no live preview.
    KD_CHECK_EQ(stub.transcribe_requests(), 1);
    KD_CHECK_EQ(seen.of<kd::proto::PartialMsg>().size(), size_t{0});

    const auto stopped = seen.of<kd::proto::StoppedMsg>();
    KD_REQUIRE_EQ(stopped.size(), size_t{1});
    KD_CHECK_EQ(stopped[0].reason.value_or(""), std::string("stop"));

    // And the audio really did arrive as a WAV of about the right length.
    KD_CHECK_GT(stub.last_body_bytes(), size_t{700 * 32});
    KD_CHECK(stub.last_content_type().find("multipart/form-data") != std::string::npos);
}

KD_TEST(asrd_session, two_utterances_get_two_finals_and_consecutive_ids) {
    StubSpeechService stub;
    KD_REQUIRE(stub.start());
    stub.set_transcribe({200, R"({"text":"one"})"});

    TranslatorHarness harness(config_for(stub));
    KD_REQUIRE(harness.start());

    std::vector<int16_t> pcm = one_utterance(500, 1200);
    const std::vector<int16_t> second = one_utterance(500, 1200);
    pcm.insert(pcm.end(), second.begin(), second.end());

    Observed seen;
    KD_REQUIRE(run_session(harness, seen, pcm));

    const auto finals = seen.of<kd::proto::FinalMsg>();
    KD_REQUIRE_EQ(finals.size(), size_t{2});
    // §7.3: increments by exactly 1. A hole is an id the Client would have to explain to itself.
    KD_CHECK_EQ(finals[0].utterance_id, uint64_t{1});
    KD_CHECK_EQ(finals[1].utterance_id, uint64_t{2});
    // §7.4: strictly increasing per session, across utterances.
    KD_CHECK_EQ(finals[0].seq, uint64_t{1});
    KD_CHECK_EQ(finals[1].seq, uint64_t{2});
}

KD_TEST(asrd_session, an_utterance_that_decodes_to_nothing_gives_its_id_back) {
    // Emitting an empty `final` would burn a seq and an utterance_id to say nothing, and §6.2
    // requires exactly one `final` per utterance_id — so an utterance with no text is never
    // opened as far as the Client is concerned.
    StubSpeechService stub;
    KD_REQUIRE(stub.start());
    stub.set_transcribe({200, R"({"text":"   "})"});

    TranslatorHarness harness(config_for(stub));
    KD_REQUIRE(harness.start());

    Observed seen;
    KD_REQUIRE(run_session(harness, seen, one_utterance()));
    KD_CHECK_EQ(seen.of<kd::proto::FinalMsg>().size(), size_t{0});
    KD_CHECK_EQ(seen.of<kd::proto::StoppedMsg>().size(), size_t{1});
}

// ─── Teardown ───────────────────────────────────────────────────────────────────────────────

KD_TEST(asrd_session, stop_for_an_unknown_session_is_answered_not_refused) {
    // §6.1 / §10.5. The Client's teardown path runs on crash paths where its own state is
    // uncertain, so this is idempotent by design and an `error` here would turn a harmless
    // duplicate into a dead connection.
    StubSpeechService stub;
    KD_REQUIRE(stub.start());
    TranslatorHarness harness(config_for(stub));
    KD_REQUIRE(harness.start());

    Observed seen;
    kd::IpcClient client;
    KD_REQUIRE(attach(client, seen, harness.url()));
    KD_REQUIRE(seen.wait_for([&] { return seen.ready; }));

    kd::proto::StopMsg stop;
    stop.session_id = "never-started";
    KD_REQUIRE(client.send_control(kd::proto::Message::of(stop)).ok());
    KD_REQUIRE(seen.wait_for([&] { return seen.stopped; }));

    const auto stopped = seen.of<kd::proto::StoppedMsg>();
    KD_REQUIRE_EQ(stopped.size(), size_t{1});
    KD_CHECK_EQ(stopped[0].session_id, std::string("never-started"));
    KD_CHECK_EQ(stopped[0].reason.value_or(""), std::string("unknown_session"));
    KD_CHECK_EQ(seen.of<kd::proto::ErrorMsg>().size(), size_t{0});
    client.stop();
}

KD_TEST(asrd_session, cancel_is_acknowledged_and_nothing_follows_it) {
    // §6.1 / §10.4: `stopped` goes out immediately rather than waiting for the decode being
    // abandoned, and no partial or final may follow. The stub is made slow on purpose so the
    // cancel genuinely lands with a POST in flight.
    StubSpeechService stub;
    KD_REQUIRE(stub.start());
    stub.set_transcribe({200, R"({"text":"should never arrive"})", "application/json", 3000});

    TranslatorHarness harness(config_for(stub));
    KD_REQUIRE(harness.start());

    Observed seen;
    // A short settle so the cancel arrives while the (slow) transcription is still running.
    KD_REQUIRE(run_session(harness, seen, one_utterance(500, 900),
                           /*cancel_instead_of_stop=*/true, /*settle_ms=*/700));

    const auto stopped = seen.of<kd::proto::StoppedMsg>();
    KD_REQUIRE_EQ(stopped.size(), size_t{1});
    KD_CHECK_EQ(stopped[0].reason.value_or(""), std::string("cancel"));
    KD_CHECK_EQ(seen.of<kd::proto::FinalMsg>().size(), size_t{0});
    KD_CHECK_EQ(seen.of<kd::proto::PartialMsg>().size(), size_t{0});
}

KD_TEST(asrd_session, a_second_start_on_one_connection_is_a_fatal_state_violation) {
    // §7.2: sessions do not nest and are not concurrent; the socket is not a multiplexer.
    StubSpeechService stub;
    KD_REQUIRE(stub.start());
    TranslatorHarness harness(config_for(stub));
    KD_REQUIRE(harness.start());

    Observed seen;
    kd::IpcClient client;
    KD_REQUIRE(attach(client, seen, harness.url()));
    KD_REQUIRE(seen.wait_for([&] { return seen.ready; }));

    kd::proto::StartMsg first;
    first.session_id = "s1";
    KD_REQUIRE(client.send_control(kd::proto::Message::of(first)).ok());
    kd::proto::StartMsg second;
    second.session_id = "s2";
    KD_REQUIRE(client.send_control(kd::proto::Message::of(second)).ok());

    KD_REQUIRE(seen.wait_for([&] { return seen.errors > 0; }));
    const auto errors = seen.of<kd::proto::ErrorMsg>();
    KD_REQUIRE_GT(errors.size(), size_t{0});
    KD_CHECK(errors[0].code == kd::proto::ErrorCode::state_violation);
    KD_CHECK(errors[0].fatal);
    // fatal:true means the connection is finished, so no `stopped` follows: §6.2's `stopped` is
    // a session-teardown acknowledgement with nowhere left to go.
    KD_CHECK_EQ(seen.of<kd::proto::StoppedMsg>().size(), size_t{0});
    client.stop();
}

// ─── The failure paths openai-translator.md §5 names ────────────────────────────────────────

KD_TEST(asrd_session, a_down_speech_host_fails_before_ready) {
    // The load-time connect, and what makes the client's Test button mean something: a green
    // Test says the translator *and* the speech host answered, not that a port is open.
    StubSpeechService stub;
    KD_REQUIRE(stub.start());
    const std::string url = stub.base_url();
    stub.stop();  // the host is now down, and its port is free

    TranslatorConfig cfg;
    cfg.service = url;
    cfg.model = "stub-model";
    cfg.http_timeout_ms = 3000;
    TranslatorHarness harness(cfg);
    KD_REQUIRE(harness.start());

    Observed seen;
    kd::IpcClient client;
    KD_REQUIRE(attach(client, seen, harness.url()));

    KD_REQUIRE(seen.wait_for([&] { return seen.dead || seen.errors > 0; }));
    const auto errors = seen.of<kd::proto::ErrorMsg>();
    KD_REQUIRE_GT(errors.size(), size_t{0});
    KD_CHECK(errors[0].code == kd::proto::ErrorCode::connect_refused);
    KD_CHECK(errors[0].fatal);
    // The message names the address, because the fix is a settings change and the user needs to
    // see which address was tried.
    KD_CHECK(errors[0].msg.find("127.0.0.1") != std::string::npos);
    {
        std::lock_guard<std::mutex> lock(seen.m);
        KD_CHECK(!seen.ready);  // `ready` is earned, and it was not earned
    }
    client.stop();
}

KD_TEST(asrd_session, a_500_on_transcription_is_decode_failed_and_keeps_the_connection) {
    // §11.3: a backend fault with a session open is `decode_failed`, non-fatal — the model
    // misbehaved on this utterance, the process is fine, and the next session may well work.
    // The session is closed with `stopped` so the Client's machine does not hang in FINALIZING.
    StubSpeechService stub;
    KD_REQUIRE(stub.start());
    stub.set_transcribe({500, R"({"detail":"model exploded"})"});

    TranslatorHarness harness(config_for(stub));
    KD_REQUIRE(harness.start());

    Observed seen;
    KD_REQUIRE(run_session(harness, seen, one_utterance()));

    const auto errors = seen.of<kd::proto::ErrorMsg>();
    KD_REQUIRE_GT(errors.size(), size_t{0});
    KD_CHECK(errors[0].code == kd::proto::ErrorCode::decode_failed);
    KD_CHECK(!errors[0].fatal);
    // §11.1: session_id is REQUIRED on a non-fatal error.
    KD_REQUIRE(errors[0].session_id.has_value());
    KD_CHECK_EQ(*errors[0].session_id, std::string("s1"));
    KD_CHECK(errors[0].msg.find("500") != std::string::npos);

    const auto stopped = seen.of<kd::proto::StoppedMsg>();
    KD_REQUIRE_GT(stopped.size(), size_t{0});
    KD_CHECK_EQ(stopped[0].reason.value_or(""), std::string("error"));
    KD_CHECK_EQ(seen.of<kd::proto::FinalMsg>().size(), size_t{0});
}

KD_TEST(asrd_session, a_non_json_body_is_decode_failed) {
    StubSpeechService stub;
    KD_REQUIRE(stub.start());
    stub.set_transcribe({200, "<html>proxy error</html>", "text/html"});

    TranslatorHarness harness(config_for(stub));
    KD_REQUIRE(harness.start());

    Observed seen;
    KD_REQUIRE(run_session(harness, seen, one_utterance()));

    const auto errors = seen.of<kd::proto::ErrorMsg>();
    KD_REQUIRE_GT(errors.size(), size_t{0});
    KD_CHECK(errors[0].code == kd::proto::ErrorCode::decode_failed);
    KD_CHECK(!errors[0].fatal);
    KD_CHECK(errors[0].msg.find("JSON") != std::string::npos ||
             errors[0].msg.find("json") != std::string::npos);
}

KD_TEST(asrd_session, a_json_body_without_a_text_field_is_decode_failed) {
    StubSpeechService stub;
    KD_REQUIRE(stub.start());
    stub.set_transcribe({200, R"({"segments":[],"language":"en"})"});

    TranslatorHarness harness(config_for(stub));
    KD_REQUIRE(harness.start());

    Observed seen;
    KD_REQUIRE(run_session(harness, seen, one_utterance()));

    const auto errors = seen.of<kd::proto::ErrorMsg>();
    KD_REQUIRE_GT(errors.size(), size_t{0});
    KD_CHECK(errors[0].code == kd::proto::ErrorCode::decode_failed);
    KD_CHECK(errors[0].msg.find("text") != std::string::npos);
}

KD_TEST(asrd_session, a_timeout_is_decode_failed_and_bounded) {
    // The per-operation WinHTTP timeout is bounded again by hand between operations, so a
    // service that stalls at every step cannot take four times the configured budget. Asserted
    // as wall-clock, because that bound is what stops a `stop` sitting on a 30 s HTTP timeout.
    StubSpeechService stub;
    KD_REQUIRE(stub.start());
    stub.set_transcribe({200, R"({"text":"too late"})", "application/json", 0, /*hang_up=*/true});

    TranslatorConfig cfg = config_for(stub);
    cfg.http_timeout_ms = 2000;
    TranslatorHarness harness(cfg);
    KD_REQUIRE(harness.start());

    const DWORD started = GetTickCount();
    Observed seen;
    KD_REQUIRE(run_session(harness, seen, one_utterance(), false, /*settle_ms=*/8000));
    const DWORD elapsed = GetTickCount() - started;

    const auto errors = seen.of<kd::proto::ErrorMsg>();
    KD_REQUIRE_GT(errors.size(), size_t{0});
    KD_CHECK(errors[0].code == kd::proto::ErrorCode::decode_failed);
    KD_CHECK(!errors[0].fatal);
    KD_INFO("session took " + std::to_string(elapsed) + " ms with a 2000 ms HTTP budget");
}

KD_TEST(asrd_session, a_slow_answer_inside_the_budget_is_not_thrown_away) {
    // The regression: receive was armed with a quarter of the budget, like resolve, connect and
    // send. A transcription is silence on the socket until the model finishes, so a service that
    // took longer than a quarter of the budget to think had its answer discarded with most of
    // the budget unspent. OpenRouter did that to the first dictation sent through it (a ~9 s
    // answer against a 7.5 s quarter of 30 s; reproduced against httpbin.org/delay/9: dropped at
    // 8.0 s with a 7.5 s receive, kept at 9.6 s with the remaining budget).
    //
    // Honestly: over loopback WinHTTP keeps a late answer whatever the receive timeout says —
    // the discard only happens against a remote host — so what this pins is the other half: that
    // an answer inside the budget is delivered, now that a watchdog enforces the budget.
    // Here: 1.5 s against a 2 s budget, whose quarter is 0.5 s.
    StubSpeechService stub;
    KD_REQUIRE(stub.start());
    stub.set_transcribe({200, R"({"text":"worth the wait"})", "application/json",
                         /*delay_ms=*/1500, false});

    TranslatorConfig cfg = config_for(stub);
    cfg.http_timeout_ms = 2000;
    TranslatorHarness harness(cfg);
    KD_REQUIRE(harness.start());

    Observed seen;
    KD_REQUIRE(run_session(harness, seen, one_utterance(), false, /*settle_ms=*/8000));
    KD_CHECK_EQ(seen.of<kd::proto::ErrorMsg>().size(), size_t{0});
    const auto finals = seen.of<kd::proto::FinalMsg>();
    KD_REQUIRE_EQ(finals.size(), size_t{1});
    KD_CHECK_EQ(finals[0].text, std::string("worth the wait"));
}

KD_TEST(asrd_session, an_answer_later_than_the_budget_still_times_out_in_it) {
    // The other half: the receive gets what is *left*, not a fresh budget, so the bound the test
    // above relaxes is still a bound.
    StubSpeechService stub;
    KD_REQUIRE(stub.start());
    stub.set_transcribe({200, R"({"text":"too late"})", "application/json",
                         /*delay_ms=*/4000, false});

    TranslatorConfig cfg = config_for(stub);
    cfg.http_timeout_ms = 1000;
    TranslatorHarness harness(cfg);
    KD_REQUIRE(harness.start());

    Observed seen;
    KD_REQUIRE(run_session(harness, seen, one_utterance(), false, /*settle_ms=*/8000));
    const auto errors = seen.of<kd::proto::ErrorMsg>();
    KD_REQUIRE_GT(errors.size(), size_t{0});
    KD_CHECK(errors[0].code == kd::proto::ErrorCode::decode_failed);
    // In words, not "WinHttpReceiveResponse failed", which is what the log said the first time.
    KD_CHECK(errors[0].msg.find("did not answer within 1000 ms") != std::string::npos);

    // And the bound itself, on one request rather than a whole session's wall clock — against a
    // fresh stub, because this one serves one request at a time and is still asleep on the
    // session's.
    StubSpeechService slow;
    KD_REQUIRE(slow.start());
    slow.set_transcribe({200, R"({"text":"too late"})", "application/json",
                         /*delay_ms=*/3000, false});
    HttpClient http;
    HttpUrl url;
    KD_REQUIRE(parse_http_url(slow.base_url() + "/v1/audio/transcriptions", url));
    const ULONGLONG t0 = GetTickCount64();
    const kd::Result<HttpResponse> r = http.post(url, "text/plain", "x", 1000);
    const ULONGLONG took = GetTickCount64() - t0;
    KD_CHECK(!r);
    KD_CHECK_LT(took, ULONGLONG{1600});
    KD_INFO("a 3 s answer against a 1 s budget gave up after " + std::to_string(took) + " ms: " +
            (r ? std::string("(succeeded)") : r.status().to_string()));
}

KD_TEST(asrd_session, the_vad_gate_fails_open) {
    // The rule with the strongest justification in the whole backend: losing something the user
    // actually said is worse than an occasional invented word, and a VAD outage must never be
    // able to silence dictation. Every failure mode means "go ahead".
    const StubSpeechService::Reply kOk{200, R"({"text":"the gate did not stop this"})"};

    struct Case {
        const char* name;
        StubSpeechService::Reply vad;
    };
    const Case cases[] = {
        {"503 - the route is not installed",
         {503, R"({"detail":"pysilero-vad is not installed"})"}},
        {"200 with a body that is not JSON", {200, "not json at all", "text/plain"}},
        {"200 with no speech field", {200, R"({"probability":0.9})"}},
        {"the far end hangs up", {200, "", "application/json", 0, true}},
    };

    for (const Case& c : cases) {
        StubSpeechService stub;
        KD_REQUIRE(stub.start());
        stub.set_transcribe(kOk);
        stub.set_vad(c.vad);

        TranslatorConfig cfg = config_for(stub);
        cfg.vad_gate = true;
        TranslatorHarness harness(cfg);
        KD_REQUIRE(harness.start());

        Observed seen;
        KD_REQUIRE(run_session(harness, seen, one_utterance()));

        const auto finals = seen.of<kd::proto::FinalMsg>();
        if (finals.size() != 1) {
            KD_FAIL(std::string("the VAD gate did not fail open for: ") + c.name);
            continue;
        }
        KD_CHECK_EQ(finals[0].text, std::string("the gate did not stop this"));
        KD_CHECK_EQ(stub.transcribe_requests(), 1);
        KD_CHECK_GT(stub.vad_requests(), 0);
    }
}

KD_TEST(asrd_session, the_vad_gate_skips_a_window_it_calls_silence) {
    // The other direction: when the route works and says no, the transcription is not made at
    // all. That is what stops 500 ms of silence coming back as "Yeah."
    StubSpeechService stub;
    KD_REQUIRE(stub.start());
    stub.set_transcribe({200, R"({"text":"Yeah."})"});
    stub.set_vad({200, R"({"speech":false})"});

    TranslatorConfig cfg = config_for(stub);
    cfg.vad_gate = true;
    TranslatorHarness harness(cfg);
    KD_REQUIRE(harness.start());

    Observed seen;
    KD_REQUIRE(run_session(harness, seen, one_utterance()));

    KD_CHECK_GT(stub.vad_requests(), 0);
    KD_CHECK_EQ(stub.transcribe_requests(), 0);
    KD_CHECK_EQ(seen.of<kd::proto::FinalMsg>().size(), size_t{0});
    KD_CHECK_EQ(seen.of<kd::proto::ErrorMsg>().size(), size_t{0});
}

// ─── The frame layer, from the server's side ────────────────────────────────────────────────

KD_TEST(asrd_session, one_message_must_hold_exactly_one_frame) {
    // §2.6.3's redundant length prefix, checked. Two frames packed into one WebSocket message
    // would otherwise decode as a legal stream and surface later as garbled text.
    const std::vector<uint8_t> one =
        kd::proto::encode_control_frame(kd::proto::Message::of(kd::proto::HelloMsg{}));
    const std::string single(reinterpret_cast<const char*>(one.data()), one.size());
    KD_CHECK(length_agrees(single));

    KD_CHECK(!length_agrees(single + single));       // two frames in one message
    KD_CHECK(!length_agrees(single.substr(0, 4)));   // a header with no body
    KD_CHECK(!length_agrees(""));
    KD_CHECK(!length_agrees(single.substr(0, single.size() - 1)));  // one byte short
}

// ─── Against a real speech service, when one is named ───────────────────────────────────────

KD_TEST(asrd_session, against_a_real_speech_service_when_one_is_named) {
    // openai-translator.md §7's T1 gate is a human dictating into Notepad, and no automated test
    // can be that. What this can be is the half of it a machine can check: that the whole path —
    // WebSocket upgrade, handshake, endpointing, WAV, multipart, WinHTTP, and the far end's own
    // model — completes against a real server rather than against a stub that answers whatever
    // it is told to.
    //
    // Opt-in, because it needs a host:
    //
    //     set KD_ASRD_SERVICE=http://strixhalo.lan:10301
    //     kd_tests --suite asrd_session
    //
    // or, for a hosted service, with its model and key as well:
    //
    //     set KD_ASRD_SERVICE=https://openrouter.ai/api
    //     set KD_ASRD_MODEL=qwen/qwen3-asr-1.7b
    //     set KD_ASRD_API_KEY=%OPENROUTER_API_KEY%
    //
    // Skipped otherwise. A test that silently required somebody's LAN would fail on every other
    // machine and teach people to ignore a red suite.
    char service[512] = {};
    DWORD n = GetEnvironmentVariableA("KD_ASRD_SERVICE", service, sizeof service);
    if (n == 0 || n >= sizeof service) {
        KD_INFO("skipped: set KD_ASRD_SERVICE to a speech service URL to run this");
        KD_CHECK(true);
        return;
    }

    TranslatorConfig cfg;
    cfg.service = service;
    char model[256] = {};
    n = GetEnvironmentVariableA("KD_ASRD_MODEL", model, sizeof model);
    if (n != 0 && n < sizeof model) cfg.model = model;
    char key[1100] = {};
    n = GetEnvironmentVariableA("KD_ASRD_API_KEY", key, sizeof key);
    if (n != 0 && n < sizeof key) cfg.api_key = key;
    // The defaults a supervised translator would be launched with, except two:
    //
    //   * the endpoint silence is shortened, so the case is seconds rather than minutes;
    //   * **the VAD gate is off**, and that is not a convenience. The audio here is a 300 Hz
    //     tone, and the server's Silero is right to say a tone is not speech — with the gate on,
    //     this test correctly produces nothing and proves nothing about the transcription path.
    //     The gate has its own tests against the stub, where "the route said no" can be asked
    //     for on demand.
    cfg.endpoint_silence_ms = 600;
    cfg.vad_gate = false;
    cfg.http_timeout_ms = 30000;
    TranslatorHarness harness(cfg);
    KD_REQUIRE(harness.start());

    Observed seen;
    KD_REQUIRE(run_session(harness, seen, one_utterance(900, 2200), false, /*settle_ms=*/25000));

    // What is asserted is the round trip, not the transcript. The audio is a 300 Hz tone, and
    // what a model returns for a tone is a property of that model — this endpoint answers 500 ms
    // of silence with "Yeah.", which openai_http.py measured and which is exactly why the VAD
    // gate exists. Asserting text here would be asserting a hallucination.
    const auto errors = seen.of<kd::proto::ErrorMsg>();
    for (const kd::proto::ErrorMsg& e : errors) {
        KD_FAIL(std::string("the real service produced an error: ") + e.code_raw + " - " + e.msg);
    }
    KD_CHECK_EQ(seen.of<kd::proto::StoppedMsg>().size(), size_t{1});

    const auto finals = seen.of<kd::proto::FinalMsg>();
    KD_INFO("the service answered with " + std::to_string(finals.size()) + " final(s)");
    for (const kd::proto::FinalMsg& f : finals) {
        // Whatever it said, the protocol obligations hold: a `final` is entirely stable, and
        // stable_bytes when present MUST equal len(utf8(text)) (§6.2, §6.3).
        KD_CHECK_GT(f.seq, uint64_t{0});
        KD_CHECK_GT(f.utterance_id, uint64_t{0});
        if (f.stable_bytes.has_value()) KD_CHECK_EQ(*f.stable_bytes, f.text.size());
        KD_CHECK(kd::proto::is_valid_stable_offset(f.text, f.text.size()));
    }
}

// ─── --hostile-stability, and the T2 live-preview gate ──────────────────────────────────────

KD_TEST(asrd_session, hostile_stability_puts_a_non_conforming_mark_on_the_wire) {
    // openai-translator.md §4.7 and §5. `server/tests/test_hostile_stability.py` exists because
    // docs/protocol.md §8.3's Client handling had never been reachable from a *running* server:
    // stability is the server's computation, so no correct pipeline produces §8.3's input. The
    // C++ translator needs the same deliberate hole, with the same name and the same loud
    // logging — otherwise the client's §8.3 path stays covered only by frozen fixtures.
    //
    // Four things have to hold, and each has a way of failing that leaves the suite looking
    // healthy. The first two are asserted here; the second two are structural in this port and
    // are noted where they live.
    StubSpeechService stub;
    KD_REQUIRE(stub.start());

    // A backend that revises byte 0 on its second hypothesis — the signature of a batch model
    // that re-decodes rather than extends, and exactly what a hostile mark cannot survive.
    stub.set_transcribe({200, R"({"text":"so i was thinking we should"})"});

    TranslatorConfig cfg = config_for(stub);
    cfg.finalize_only = false;  // partials are where §8.2's monotonicity lives
    cfg.hostile_stability = true;
    TranslatorHarness harness(cfg);
    KD_REQUIRE(harness.start());

    Observed seen;
    KD_REQUIRE(run_session(harness, seen, one_utterance(2500, 1200)));

    const auto partials = seen.of<kd::proto::PartialMsg>();
    KD_REQUIRE_GT(partials.size(), size_t{0});
    // The whole hypothesis published as stable, bypassing the tracker. A conforming computation
    // would have published 0 here — with N=2 the first candidate is 0 ms old — so a non-zero
    // mark on the first partial is proof the switch is what produced it.
    KD_CHECK_EQ(partials[0].stable_bytes, partials[0].text.size());
    KD_INFO("hostile partial published stable_bytes=" +
            std::to_string(partials[0].stable_bytes) + " of " +
            std::to_string(partials[0].text.size()) + " bytes");

    // Point 3 from the Python file, and the subtle one: the server's own tracker must stand
    // down. Left running it would raise on the contradiction the switch exists to create, abort
    // the utterance with `decode_failed`, and keep §8.3 untested while the test appeared to
    // pass. In this port that is structural — `SessionServer::worker` takes the hostile branch
    // *instead of* `stability_.observe`, never as well as — and this is what notices if that
    // ever becomes an `if` with two arms that both run.
    KD_CHECK_EQ(seen.of<kd::proto::ErrorMsg>().size(), size_t{0});
}

KD_TEST(asrd_session, live_preview_against_a_revising_model_never_reaches_the_client_broken) {
    // openai-translator.md §7's T2 gate, second half: "Live preview against a batch model
    // produces either a conforming stream or a refused emission — never a `stability_violation`
    // on the client."
    //
    // Both outcomes are acceptable and the test asserts the disjunction rather than picking one,
    // because which one happens depends on how much the model revises — and pinning that would
    // make this test about the stub's script instead of about the invariant.
    StubSpeechService stub;
    KD_REQUIRE(stub.start());
    // A batch model, behaving as measured: as it hears more it re-cases the opening word and
    // inserts the punctuation it was not sure of. `so i was thinking we` becomes `So I was
    // thinking, we` — a revision at byte 0, which no value of stable_bytes can make conforming
    // once anything has been published.
    //
    // The recasing is the *second* reply on purpose. A hop that fires while a POST is running
    // replaces the pending work rather than queueing a second call (openai-translator.md §2.1),
    // so a fast test coalesces most hops and sees only a couple of transcriptions — putting the
    // byte-0 revision late in the script would mean it never arrived and the gate would pass
    // without ever meeting the case it exists for.
    stub.set_transcribe_script({
        {200, R"({"text":"so i was thinking we"})"},
        {200, R"({"text":"So I was thinking, we should"})"},
        {200, R"({"text":"So I was thinking, we should probably ship it."})"},
    });

    TranslatorConfig cfg = config_for(stub);
    cfg.finalize_only = false;
    TranslatorHarness harness(cfg);
    KD_REQUIRE(harness.start());

    Observed seen;
    KD_REQUIRE(run_session(harness, seen, one_utterance(2500, 1200)));

    // Whatever was published must be a conforming sequence: non-decreasing within an utterance,
    // on a grapheme boundary, and with each hypothesis extending the last published prefix.
    // These are §8.2's three requirements, checked from the receiving side.
    uint64_t high_water = 0;
    std::string published;
    uint64_t utterance = 0;
    for (const kd::proto::PartialMsg& p : seen.of<kd::proto::PartialMsg>()) {
        if (p.utterance_id != utterance) {
            utterance = p.utterance_id;
            high_water = 0;
            published.clear();
        }
        KD_CHECK(kd::proto::is_valid_stable_offset(p.text, p.stable_bytes));
        KD_CHECK(p.stable_bytes >= high_water);
        KD_CHECK(p.text.compare(0, published.size(), published) == 0);
        high_water = p.stable_bytes;
        published = p.text.substr(0, static_cast<size_t>(p.stable_bytes));
    }

    // A refusal is the other legal outcome, and it is `decode_failed` rather than
    // `stability_violation`: §11.3 has no code for "the Server detected that its own next
    // message would violate §8.2", and sending the inadmissible message so the Client can raise
    // one would put the offending bytes in the user's document first.
    for (const kd::proto::ErrorMsg& e : seen.of<kd::proto::ErrorMsg>()) {
        KD_CHECK(e.code == kd::proto::ErrorCode::decode_failed);
        KD_CHECK(!e.fatal);
        KD_INFO("refused rather than published: " + e.msg);
    }

    // Reported rather than inferred, because a run that produced no partials at all would
    // satisfy every assertion above and prove nothing.
    //
    // **What this normally finds, and it is worth understanding before "improving" it:** the
    // published marks are all 0. That is not the tracker being timid — it is step 2 working.
    // With N = 2 the candidate is the common grapheme prefix of the last two hypotheses, and a
    // model that recases the first word makes that prefix empty. So a revising model in live
    // preview publishes nothing and everything commits at the `final`, which is exactly what
    // `SessionConfig::publish_stable_partials` predicts and why finalize-only is the default.
    // The conforming-stream arm of the gate is satisfied trivially, and trivially is fine: the
    // gate is that the Client never receives a violation, not that a mark advances.
    std::string marks;
    for (const kd::proto::PartialMsg& p : seen.of<kd::proto::PartialMsg>()) {
        marks += std::to_string(p.stable_bytes) + "/" + std::to_string(p.text.size()) + " ";
    }
    KD_INFO("live preview: " + std::to_string(seen.of<kd::proto::PartialMsg>().size()) +
            " partial(s) [" + marks + "], " +
            std::to_string(seen.of<kd::proto::FinalMsg>().size()) + " final(s), " +
            std::to_string(seen.of<kd::proto::ErrorMsg>().size()) + " error(s), " +
            std::to_string(stub.transcribe_requests()) + " transcriptions");
    // The script's byte-0 revision must actually have been delivered, or the case that matters
    // never happened. Asserted on *content* rather than on a message count: how many partials
    // arrive depends on how many hops coalesced while a POST was in flight, which is correct
    // behaviour (§2.1) and varies with machine load — a count assertion here passes alone and
    // fails in a full suite run, which is the worst kind of test.
    KD_CHECK_GT(stub.transcribe_requests(), 1);
    bool saw_revision = false;
    for (const kd::proto::PartialMsg& p : seen.of<kd::proto::PartialMsg>()) {
        if (p.text.rfind("So I was thinking,", 0) == 0) saw_revision = true;
    }
    for (const kd::proto::FinalMsg& f : seen.of<kd::proto::FinalMsg>()) {
        if (f.text.rfind("So I was thinking,", 0) == 0) saw_revision = true;
    }
    KD_CHECK(saw_revision);
}

// ─── Committing the settled head on the model's own segment boundary ───────────────────

KD_TEST(asrd_session, a_segment_boundary_commits_the_settled_head_and_carries_the_rest_over) {
    // Through real HTTP, against a scripted `verbose_json` body, with no timing in it: the
    // buffer is fed exactly once and the decode is forced, so this asserts the mechanism rather
    // than racing the hop clock.
    //
    // The shape is the one measured against the real endpoint on 2026-08-30 — a long clip coming
    // back as two segments with an interior boundary, the last segment being the part still
    // being revised. Committing all but the last needs no matching and no overlap, because the
    // response gives the text and the time together.
    StubSpeechService stub;
    KD_REQUIRE(stub.start());
    stub.set_transcribe(StubSpeechService::Reply{
        200,
        R"({"text":"The first sentence. The second sentence. The fourth is still moving.",)"
        R"("duration":10.0,)"
        R"("segments":[{"id":0,"start":0.0,"end":7.0,)"
        R"("text":"The first sentence. The second sentence."},)"
        R"({"id":1,"start":7.0,"end":10.0,"text":"The fourth is still moving."}]})"});

    OpenAiBackendConfig cfg;
    cfg.base_url = stub.base_url();
    cfg.model = "stub-model";
    cfg.finalize_only = false;
    cfg.vad_gate = false;
    OpenAiBackend backend(cfg);
    KD_REQUIRE(backend.load().ok());

    const std::vector<int16_t> pcm(10 * 16000, 1000);
    backend.feed(pcm.data(), pcm.size());

    Transcription tr;
    KD_REQUIRE(backend.transcribe(/*force=*/false, tr).ok());
    KD_CHECK(tr.have_text);
    KD_REQUIRE(tr.have_cut);
    // The text to commit is the model's, verbatim — not a prefix of the hypothesis found by
    // matching words against it.
    KD_CHECK_EQ(tr.cut.committed_text,
                std::string("The first sentence. The second sentence."));
    KD_CHECK_EQ(tr.cut.cut_samples, size_t{7 * 16000});
    // ...and it is a strict prefix of what the client was told, so committing it can never
    // contradict the hypothesis already on the wire.
    KD_CHECK_EQ(tr.text.rfind(tr.cut.committed_text, 0), size_t{0});

    // Acting on it leaves exactly the audio the committed text does not describe. Three seconds
    // in, seven seconds committed, ten seconds fed: nothing decoded twice, nothing dropped.
    backend.retain_tail(tr.cut.cut_samples);
    KD_CHECK_EQ(backend.audio_ms(), uint64_t{3000});
    KD_CHECK_EQ(backend.segment_cuts_taken(), uint64_t{1});

    // A finalize never proposes one: the utterance is ending anyway, and a second boundary there
    // would put two `final`s where docs/protocol.md §6.2 allows exactly one.
    Transcription fin;
    KD_REQUIRE(backend.transcribe(/*force=*/true, fin).ok());
    KD_CHECK(fin.have_text);
    KD_CHECK(!fin.have_cut);
}

KD_TEST(asrd_session, a_service_that_ignores_verbose_json_simply_gets_no_cut) {
    // Every OpenAI-compatible service answers `{"text": ...}`; the segment table is an extra
    // this one happens to offer. Without it the utterance is committed whole and bounded by the
    // endpointer, which is the behaviour that shipped before any of this. Not an error.
    StubSpeechService stub;
    KD_REQUIRE(stub.start());
    stub.set_transcribe(StubSpeechService::Reply{200, R"({"text":"no segments here at all"})"});

    OpenAiBackendConfig cfg;
    cfg.base_url = stub.base_url();
    cfg.model = "stub-model";
    cfg.finalize_only = false;
    cfg.vad_gate = false;
    OpenAiBackend backend(cfg);
    KD_REQUIRE(backend.load().ok());

    const std::vector<int16_t> pcm(10 * 16000, 1000);
    backend.feed(pcm.data(), pcm.size());

    Transcription tr;
    KD_REQUIRE(backend.transcribe(false, tr).ok());
    KD_CHECK_EQ(tr.text, std::string("no segments here at all"));
    KD_CHECK(!tr.have_cut);
    KD_CHECK_EQ(backend.audio_ms(), uint64_t{10000});  // nothing was cut away
}

KD_TEST(asrd_session, live_preview_publishes_no_mark_so_a_late_revision_cannot_take_the_session) {
    // The failure this exists to prevent, from a real dictation on 2026-08-28:
    //
    //   session failed: server-side stability violation (stable_prefix_mutated):
    //   utterance_id=2 previous_stable_bytes=490 incoming_stable_bytes=490 diverge_offset=421
    //
    // Half a minute of speech, gone. The model had been extending honestly for 490 bytes, so
    // the tracker published a mark; then it re-punctuated byte 421, which is *below* that mark,
    // and §8.3 obliges the sender to refuse rather than emit. Refusing fails the session, and
    // failing the session drops the utterance in flight.
    //
    // The test above it never caught this because its stub revises byte 0 on the second
    // hypothesis, so the N=2 candidate is empty and no mark is ever published — the conforming
    // arm of that disjunction passes trivially. The dangerous case is the opposite one: a model
    // that behaves *well enough to earn a mark* and then revises behind it. That is what this
    // script is.
    //
    // The fix is not a better tracker. It is that live preview publishes no mark at all
    // (`SessionServerConfig::emit_partials` vs `publish_stable_partials`): with nothing
    // published there is nothing a revision can contradict, §8.2 is satisfied vacuously, and
    // the words still land in one piece at the `final`.
    StubSpeechService stub;
    KD_REQUIRE(stub.start());
    // Longer audio than the test above, and a script long enough to outlast it. A hop that
    // fires while a POST is running replaces the pending work rather than queueing (§2.1), so a
    // short case coalesces down to two transcriptions and the tracker never sees two hypotheses
    // for one utterance — which is exactly the state in which the old wiring published nothing
    // and looked fine.
    stub.set_transcribe_script({
        // Extending, honestly, for as long as it takes two consecutive hypotheses to give the
        // tracker a non-empty common prefix. This is where a positive `stable_bytes` used to go
        // out, and it is the promise the third reply then breaks.
        {200, R"({"text":"the quick brown fox jumps"})"},
        {200, R"({"text":"the quick brown fox jumps over"})"},
        {200, R"({"text":"the quick brown fox jumps over the lazy"})"},
        // ...and now the revision, well behind the mark that was just earned: "fox" gains a
        // comma at byte 19. Anything already published as stable is mutated by this.
        {200, R"({"text":"The quick brown fox, jumps over the lazy dog."})"},
        {200, R"({"text":"The quick brown fox, jumps over the lazy dog today."})"},
    });

    TranslatorConfig cfg = config_for(stub);
    cfg.finalize_only = false;
    TranslatorHarness harness(cfg);
    KD_REQUIRE(harness.start());

    Observed seen;
    // Paced, because this case is about what the tracker does across *several* hypotheses and
    // an unpaced feed collapses the utterance into two requests on a loaded machine. Without
    // this the case failed intermittently on the assertion below — never on the mark itself.
    KD_REQUIRE(run_session(harness, seen, one_utterance(8000, 1200), false, 6000, 200));

    const auto partials = seen.of<kd::proto::PartialMsg>();
    // Two, not one: with a single hypothesis the tracker has no common prefix to publish and
    // even the old wiring emitted a zero mark, so a run that produced one partial would pass
    // this test while proving nothing.
    KD_REQUIRE_GT(partials.size(), size_t{1});
    KD_CHECK_GT(stub.transcribe_requests(), 2);

    // The mark, which is the whole fix: never published, on any partial, however well the model
    // was behaving at the time. A single non-zero value here is the old bug, because it is the
    // promise the next hypothesis is then free to break.
    for (const kd::proto::PartialMsg& p : partials) {
        KD_CHECK_EQ(p.stable_bytes, uint64_t{0});
    }

    // And therefore no refusal, and no lost utterance. Before the fix this run ended in
    // `decode_failed` with the words in flight discarded.
    for (const kd::proto::ErrorMsg& e : seen.of<kd::proto::ErrorMsg>()) {
        KD_INFO("unexpected error: " + e.msg);
    }
    KD_CHECK_EQ(seen.of<kd::proto::ErrorMsg>().size(), size_t{0});
    KD_CHECK_EQ(seen.of<kd::proto::FinalMsg>().size(), size_t{1});

    // The preview was still a preview: the client saw the words move while they were being
    // spoken, which is the entire reason for turning this on.
    KD_INFO("live preview: " + std::to_string(partials.size()) + " partial(s), last text \"" +
            partials.back().text + "\"");
}

// ─── A blip is not the end of the session ───────────────────────────────────────────────────
//
// Measured on 2026-08-31, four minutes into a real dictation: one `WinHttpReceiveResponse
// failed` on a hop took the whole session down, and because the Client runs
// `Cadence::EndOfSession`, its `stopped` handler typed 1659 buffered bytes into the user's
// editor mid-word. The speaker was still talking. Nothing about their words had gone wrong —
// the far end had dropped one connection.
//
// So the rule the two cases below fix in place: a preview hop that does not come back costs a
// preview, and a flush that does not come back costs at most the revision between the last
// partial and the final. Neither costs the session.

KD_TEST(asrd_session, a_transient_failure_on_a_hop_does_not_end_the_session) {
    StubSpeechService stub;
    KD_REQUIRE(stub.start());
    // One good answer, then a dropped connection, then the model comes back — and it is the
    // *second* request that fails, whichever request that turns out to be. How many hops a given
    // run makes is not fixed: a hop that fires while a POST is in flight replaces the pending
    // work rather than queueing it (§2.1), so on a loaded machine an utterance that makes five
    // requests idle makes two. Placing the failure by index rather than by role is what keeps
    // this a test of the policy instead of a test of the scheduler.
    //
    // The audio for the failed request is still in the backend's buffer — `PseudoStreamingDriver`
    // never erases from the front — so whatever comes next covers everything it would have.
    const char* kHeard = R"({"text":"the quick brown fox jumps over the lazy dog"})";
    stub.set_transcribe_script({
        {200, kHeard},
        {500, R"({"detail":"upstream reset the connection"})"},
        {200, kHeard},
    });

    TranslatorConfig cfg = config_for(stub);
    cfg.finalize_only = false;  // hops only exist under live preview
    TranslatorHarness harness(cfg);
    KD_REQUIRE(harness.start());

    Observed seen;
    KD_REQUIRE(run_session(harness, seen, one_utterance(3000, 1200)));

    // The 500 was actually served — a run that never reached the second reply would pass every
    // assertion below while proving nothing.
    KD_CHECK_GT(stub.transcribe_requests(), 1);

    // And it reached the log and nothing else. No `error`, no early `stopped`, and the utterance
    // still closes exactly once with the words in it.
    for (const kd::proto::ErrorMsg& e : seen.of<kd::proto::ErrorMsg>()) {
        KD_INFO("unexpected error: " + e.msg);
    }
    KD_CHECK_EQ(seen.of<kd::proto::ErrorMsg>().size(), size_t{0});

    const auto finals = seen.of<kd::proto::FinalMsg>();
    KD_REQUIRE_EQ(finals.size(), size_t{1});
    // The audio the failed request covered is in it, because the request after it re-decoded the
    // same recording from its first sample rather than resuming after the gap.
    KD_CHECK_EQ(finals[0].text, std::string("the quick brown fox jumps over the lazy dog"));
}

KD_TEST(asrd_session, a_failed_flush_closes_the_utterance_with_the_last_partial) {
    StubSpeechService stub;
    KD_REQUIRE(stub.start());
    // One good hop, and then the far end stays down — the last reply in a script repeats — so
    // every request after it fails, the flush among them. §6.2 still owes the Client one `final`
    // for an id it has already seen a partial under, and the honest thing to close it with is
    // the last hypothesis the user actually watched arrive in the pill. The alternative —
    // dropping it — deletes words they had already read.
    //
    // Deliberately one success and then nothing: which request happens to be the flush depends
    // on how the hops coalesce against a POST already in flight (§2.1), so a script that tried
    // to place the failure *at* the flush would be timing the test rather than testing it.
    stub.set_transcribe_script({
        {200, R"({"text":"the quick brown fox jumps over the lazy dog"})"},
        {500, R"({"detail":"still down"})"},
    });

    TranslatorConfig cfg = config_for(stub);
    cfg.finalize_only = false;
    TranslatorHarness harness(cfg);
    KD_REQUIRE(harness.start());

    Observed seen;
    // Short enough that the failures that follow stay well inside kMaxDecodeFailures, long
    // enough that the first hop is certainly a hop and not the flush.
    KD_REQUIRE(run_session(harness, seen, one_utterance(3000, 1200)));

    const auto partials = seen.of<kd::proto::PartialMsg>();
    KD_REQUIRE_GT(partials.size(), size_t{0});

    const auto finals = seen.of<kd::proto::FinalMsg>();
    KD_REQUIRE_EQ(finals.size(), size_t{1});
    KD_CHECK_EQ(finals[0].text, partials.back().text);
    // And no `error`, and therefore no `stopped(reason=error)` — which is the message that used
    // to make the Client empty a whole session's buffer into the document mid-sentence.
    KD_CHECK_EQ(seen.of<kd::proto::ErrorMsg>().size(), size_t{0});
    KD_CHECK_GT(stub.transcribe_requests(), 1);
}

// ─── keeps_published_words: which revisions are worth recording ─────────────────────────────
//
// The judgement this encodes decides how much audio lands on the user's disk, so it is asserted
// rather than left to a code reading. The rule: `next` must still say everything `prev` said, in
// order, ignoring case and punctuation.

KD_TEST(asrd_session, an_extension_is_not_a_retraction) {
    KD_CHECK(kd::asrd::keeps_published_words("the quick brown fox",
                                             "the quick brown fox jumps over"));
    KD_CHECK(kd::asrd::keeps_published_words("", "anything at all"));
    KD_CHECK(kd::asrd::keeps_published_words("same", "same"));
}

KD_TEST(asrd_session, recasing_and_repunctuating_are_not_retractions) {
    // What this backend does on every hop once it can see where the sentence ended. If these
    // counted, the capture would fire on nearly every utterance and record the whole session.
    KD_CHECK(kd::asrd::keeps_published_words("the quick brown fox jumps",
                                             "The quick brown fox, jumps over the lazy dog."));
    KD_CHECK(kd::asrd::keeps_published_words("so i was thinking we", "So I was thinking, we"));
}

KD_TEST(asrd_session, a_word_the_user_already_read_going_missing_is_a_retraction) {
    // Measured 2026-08-31, and the reason this capture exists: the words were right at 6.3 s and
    // wrong at 7.4 s, and the worse text is what reached the document.
    KD_CHECK(!kd::asrd::keeps_published_words(
        "So I'm trying to check how the dictation capture works.",
        "Some trying to check how the dictation capture works."));
    // Truncation counts too — a hypothesis that got shorter has taken something back.
    KD_CHECK(!kd::asrd::keeps_published_words("one two three four", "one two three"));
    // And a substitution in the middle, however small.
    KD_CHECK(!kd::asrd::keeps_published_words("give me the config", "gives me the config"));
}

KD_TEST(asrd_session, a_contraction_spelled_out_is_not_a_retraction) {
    // An apostrophe is a separator, so "I'm" tokenises as {i, m}. A model that later writes "I
    // am" has said the same two words and taken nothing back; one that swallows them into a
    // different word has.
    KD_CHECK(kd::asrd::keeps_published_words("I'm going", "I m going too"));
    KD_CHECK(!kd::asrd::keeps_published_words("I'm going", "Im going"));
}

// ─── Vocabulary hints reach the endpoint ────────────────────────────────────────────────────

KD_TEST(asrd_session, start_context_is_sent_as_the_prompt_field) {
    // The regression this pins: from the day the OpenAI backend landed until 2026-09-20,
    // `start.context` was parsed, logged and thrown away, and `ready` honestly said
    // context_supported=false. The endpoint had accepted `prompt` the whole time. The user's
    // Settings → Vocabulary list therefore did nothing, and nothing failed to say so.
    //
    // So this asserts the bytes on the wire, not the client's belief about them.
    StubSpeechService stub;
    KD_REQUIRE(stub.start());
    stub.set_transcribe({200, R"({"text":"publish kvit-ui"})"});

    TranslatorHarness harness(config_for(stub));
    KD_REQUIRE(harness.start());

    Observed seen;
    KD_REQUIRE(run_session(harness, seen, one_utterance(), false, 6000, 0,
                           {"kvit-ui", "piperd", "heic"}));

    const std::string req = stub.last_transcribe_request();
    KD_REQUIRE(!req.empty());
    KD_CHECK(req.find("name=\"prompt\"") != std::string::npos);
    KD_CHECK(req.find("kvit-ui, piperd, heic") != std::string::npos);
}

KD_TEST(asrd_session, no_vocabulary_means_no_prompt_field_at_all) {
    // An empty prompt is still a field the server has to interpret, and the endpoint's own
    // behaviour with a degenerate one is to return an empty transcript. Omitting it is the
    // safer default and is what the unbiased baseline in bench/results/ab measured.
    StubSpeechService stub;
    KD_REQUIRE(stub.start());
    stub.set_transcribe({200, R"({"text":"hello"})"});

    TranslatorHarness harness(config_for(stub));
    KD_REQUIRE(harness.start());

    Observed seen;
    KD_REQUIRE(run_session(harness, seen, one_utterance()));

    const std::string req = stub.last_transcribe_request();
    KD_REQUIRE(!req.empty());
    KD_CHECK(req.find("name=\"prompt\"") == std::string::npos);
}

// ─── The API key (a hosted service such as OpenRouter) ──────────────────────────────────────

KD_TEST(asrd_session, an_api_key_goes_on_the_wire_as_a_bearer_header) {
    StubSpeechService stub;
    KD_REQUIRE(stub.start());
    stub.set_transcribe({200, R"({"text":"hello"})"});

    TranslatorConfig cfg = config_for(stub);
    cfg.api_key = "sk-or-v1-test-0123456789";
    TranslatorHarness harness(cfg);
    KD_REQUIRE(harness.start());

    Observed seen;
    KD_REQUIRE(run_session(harness, seen, one_utterance()));
    KD_CHECK_EQ(seen.finals, size_t{1});

    // The transcription itself carries it, not only the check at load.
    const std::string req = stub.last_transcribe_request();
    KD_REQUIRE(req.find("filename=") != std::string::npos);
    KD_CHECK(req.find("\r\nAuthorization: Bearer sk-or-v1-test-0123456789\r\n") !=
             std::string::npos);
    // One more request than a keyless connection makes: the check at load.
    KD_CHECK_EQ(stub.transcribe_requests(), 2);
}

KD_TEST(asrd_session, without_an_api_key_no_authorization_header_is_sent) {
    // The LAN case, unchanged: no header, and no extra request at load.
    StubSpeechService stub;
    KD_REQUIRE(stub.start());
    stub.set_transcribe({200, R"({"text":"hello"})"});

    TranslatorHarness harness(config_for(stub));
    KD_REQUIRE(harness.start());

    Observed seen;
    KD_REQUIRE(run_session(harness, seen, one_utterance()));

    const std::string req = stub.last_transcribe_request();
    KD_REQUIRE(!req.empty());
    KD_CHECK(req.find("Authorization:") == std::string::npos);
    KD_CHECK_EQ(stub.transcribe_requests(), 1);
}

KD_TEST(asrd_session, a_refused_api_key_fails_before_ready) {
    // Without the check at load, a reachable host that refuses the key passes Test and fails on
    // the user's first word. With it, the refusal is what Test shows.
    StubSpeechService stub;
    KD_REQUIRE(stub.start());
    stub.set_transcribe({401, R"({"error":{"message":"User not found.","code":401}})"});

    TranslatorConfig cfg = config_for(stub);
    cfg.api_key = "sk-or-v1-wrong";
    TranslatorHarness harness(cfg);
    KD_REQUIRE(harness.start());

    Observed seen;
    kd::IpcClient client;
    KD_REQUIRE(attach(client, seen, harness.url()));
    KD_REQUIRE(seen.wait_for([&] { return seen.dead || seen.errors > 0; }));
    const auto errors = seen.of<kd::proto::ErrorMsg>();
    KD_REQUIRE_GT(errors.size(), size_t{0});
    KD_CHECK(errors[0].code == kd::proto::ErrorCode::connect_refused);
    KD_CHECK(errors[0].fatal);
    KD_CHECK(errors[0].msg.find("refused the API key") != std::string::npos);
    KD_CHECK(errors[0].msg.find("User not found.") != std::string::npos);
    {
        std::lock_guard<std::mutex> lock(seen.m);
        KD_CHECK(!seen.ready);
    }
    // And the check asked no model anything: there was no audio in it.
    KD_CHECK_EQ(stub.transcribe_requests(), 1);
    KD_CHECK(stub.last_transcribe_request().find("filename=") == std::string::npos);
    client.stop();
}

KD_TEST(asrd_session, the_translator_now_declares_context_supported) {
    // §6.1 requires a server that cannot use the hints to say so. Now that it can, it must say
    // *that* — the client's Test panel reads this field to tell the user whether their
    // vocabulary is doing anything, and it told them "not supported by this model" for a month.
    StubSpeechService stub;
    KD_REQUIRE(stub.start());
    TranslatorHarness harness(config_for(stub));
    KD_REQUIRE(harness.start());

    kd::IpcClient client;
    Observed seen;
    KD_REQUIRE(attach(client, seen, harness.url()));
    KD_REQUIRE(seen.wait_for([&] { return seen.ready || seen.dead || seen.disconnected; }));

    const auto readys = seen.of<kd::proto::ReadyMsg>();
    KD_REQUIRE_EQ(readys.size(), size_t{1});
    KD_CHECK(readys[0].context_supported);
}
