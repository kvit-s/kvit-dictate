// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// kvit-asrd — the translator. `docs/protocol.md` on one side, an OpenAI-compatible HTTP speech
// service on the other. See openai-translator.md for why this exists at all.
//
// The short version: the protocol boundary was the right call and stays. Making the *other side
// of it* the user's problem was not. Before this binary, a user who had installed one executable
// and named their endpoint still had to keep a Python window open, because the endpoint on their
// network speaks `/v1/audio/transcriptions` and not this protocol. This program is the half that
// was missing.
//
// ─── Layout ─────────────────────────────────────────────────────────────────────────────────
//
//   ws_server.cpp        L0: TCP, RFC 6455.                                    NEW
//   protocol.cpp         L1/L2: framing and the message codec.                 REUSED, verbatim
//   session_server.cpp   L3: start/stop/cancel, utterances, emission.          PORTED
//   vad.cpp              endpointing.                                          PORTED
//   stability.cpp        hypotheses -> stable_bytes.                           PORTED
//   backend_openai.cpp   PCM -> text.                                          PORTED
//
// This file is the run loop and the handshake, and nothing else.

#define KD_LOG_MODULE "asrd"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "asrd/backend_openai.h"
#include "asrd/http.h"
#include "asrd/session_server.h"
#include "asrd/vad.h"
#include "asrd/ws_server.h"
#include "protocol.h"
#include "util/log.h"

namespace kd::asrd {
namespace {

// ─── Command line (openai-translator.md §4.7) ───────────────────────────────────────────────

struct Options {
    std::string listen_host = "127.0.0.1";
    uint16_t listen_port = kDefaultPort;
    std::string service;
    std::string model = kDefaultModel;
    std::string language = kDefaultLanguage;
    uint32_t parent_pid = 0;
    uint32_t max_connections = 1;
    bool finalize_only = true;
    bool vad_gate = true;
    uint32_t endpoint_silence_ms = 1500;
    uint32_t stability_n = 2;
    uint32_t stability_m = 3;
    uint32_t stability_hold_ms = 200;
    bool hostile_stability = false;
    kd::log::Level log_level = kd::log::Level::Info;
};

void print_usage() {
    std::fputs(
        "kvit-asrd - the kvit-dictate translator.\n"
        "\n"
        "Speaks docs/protocol.md on a loopback WebSocket and an OpenAI-compatible\n"
        "/v1/audio/transcriptions service over HTTP.\n"
        "\n"
        "  kvit-asrd --service http://host:port [--listen 127.0.0.1:10402] [options]\n"
        "\n"
        "  --listen [host:]port        default 127.0.0.1:10402. Loopback unless a host is given.\n"
        "  --service URL               REQUIRED. http:// or https:// only.\n"
        "  --model NAME                default Qwen/Qwen3-ASR-1.7B\n"
        "  --language LANG             default en. Empty means the server's default.\n"
        "  --parent-pid PID            exit when that process does.\n"
        "  --max-connections N         default 1. 0 = unlimited. Extras get server_busy.\n"
        "  --finalize-only on|off      default on. Off is live preview, and is labelled.\n"
        "  --vad-gate on|off           default on. Fails open.\n"
        "  --endpoint-silence-ms MS    default 1500\n"
        "  --stability-n N             default 2\n"
        "  --stability-m M             default 3\n"
        "  --stability-hold-ms T       default 200\n"
        "  --hostile-stability         deliberately non-conforming. Test use only.\n"
        "  --log-level LEVEL           trace|debug|info|warn|error. Default info.\n"
        "  --version, --help\n",
        stderr);
}

bool parse_bool(const std::string& v, bool& out) {
    if (v == "on" || v == "1" || v == "true" || v == "yes") { out = true; return true; }
    if (v == "off" || v == "0" || v == "false" || v == "no") { out = false; return true; }
    return false;
}

bool parse_u32(const std::string& v, uint32_t& out) {
    if (v.empty()) return false;
    char* end = nullptr;
    const unsigned long n = std::strtoul(v.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || n > 0xFFFFFFFFul) return false;
    out = static_cast<uint32_t>(n);
    return true;
}

// `[host:]port`. A bare number is a port on loopback; anything else must be an address literal,
// because a name would have to be resolved and a translator that binds "whatever that name
// resolves to" is a translator that occasionally binds the wrong interface.
bool parse_listen(const std::string& v, std::string& host, uint16_t& port) {
    const size_t colon = v.rfind(':');
    std::string port_text = v;
    if (colon != std::string::npos) {
        host = v.substr(0, colon);
        port_text = v.substr(colon + 1);
        // An IPv6 literal is written [::1]:10402; strip the brackets for getaddrinfo.
        if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
            host = host.substr(1, host.size() - 2);
        }
    }
    uint32_t n = 0;
    if (!parse_u32(port_text, n) || n > 65535) return false;
    port = static_cast<uint16_t>(n);
    return true;
}

// Returns false when the program should exit; `exit_code` says with what.
bool parse_args(int argc, char** argv, Options& opt, int& exit_code) {
    exit_code = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "kvit-asrd: %s needs a value\n", what);
                exit_code = 2;
                return {};
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") {
            print_usage();
            return false;
        }
        if (a == "--version") {
#ifdef KVIT_VERSION_RAW
#define KD_STR2(x) #x
#define KD_STR(x) KD_STR2(x)
            std::fprintf(stderr, "kvit-asrd %s (proto %d)\n", KD_STR(KVIT_VERSION_RAW),
                         proto::kProtoVersion);
#undef KD_STR
#undef KD_STR2
#else
            std::fprintf(stderr, "kvit-asrd (proto %d)\n", proto::kProtoVersion);
#endif
            return false;
        }
        if (a == "--listen") {
            const std::string v = next("--listen");
            if (exit_code != 0) return false;
            if (!parse_listen(v, opt.listen_host, opt.listen_port)) {
                std::fprintf(stderr, "kvit-asrd: --listen wants [host:]port, got %s\n", v.c_str());
                exit_code = 2;
                return false;
            }
            continue;
        }
        if (a == "--service") {
            opt.service = next("--service");
            if (exit_code != 0) return false;
            continue;
        }
        if (a == "--model") {
            opt.model = next("--model");
            if (exit_code != 0) return false;
            continue;
        }
        if (a == "--language") {
            opt.language = next("--language");
            if (exit_code != 0) return false;
            continue;
        }
        if (a == "--parent-pid") {
            const std::string v = next("--parent-pid");
            if (exit_code != 0) return false;
            if (!parse_u32(v, opt.parent_pid)) {
                std::fprintf(stderr, "kvit-asrd: --parent-pid wants a number\n");
                exit_code = 2;
                return false;
            }
            continue;
        }
        if (a == "--max-connections") {
            const std::string v = next("--max-connections");
            if (exit_code != 0) return false;
            if (!parse_u32(v, opt.max_connections)) {
                std::fprintf(stderr, "kvit-asrd: --max-connections wants a number\n");
                exit_code = 2;
                return false;
            }
            continue;
        }
        if (a == "--finalize-only" || a == "--vad-gate") {
            // Both take an explicit on/off rather than being bare switches, because both default
            // to ON and a bare `--finalize-only` would then read as if it changed something.
            const std::string v = next(a.c_str());
            if (exit_code != 0) return false;
            bool value = true;
            if (!parse_bool(v, value)) {
                std::fprintf(stderr, "kvit-asrd: %s wants on or off, got %s\n", a.c_str(),
                             v.c_str());
                exit_code = 2;
                return false;
            }
            (a == "--finalize-only" ? opt.finalize_only : opt.vad_gate) = value;
            continue;
        }
        if (a == "--endpoint-silence-ms" || a == "--stability-n" || a == "--stability-m" ||
            a == "--stability-hold-ms") {
            const std::string v = next(a.c_str());
            if (exit_code != 0) return false;
            uint32_t value = 0;
            if (!parse_u32(v, value)) {
                std::fprintf(stderr, "kvit-asrd: %s wants a number, got %s\n", a.c_str(),
                             v.c_str());
                exit_code = 2;
                return false;
            }
            if (a == "--endpoint-silence-ms") opt.endpoint_silence_ms = value;
            else if (a == "--stability-n") opt.stability_n = value;
            else if (a == "--stability-m") opt.stability_m = value;
            else opt.stability_hold_ms = value;
            continue;
        }
        if (a == "--hostile-stability") {
            opt.hostile_stability = true;
            continue;
        }
        if (a == "--log-level") {
            const std::string v = next("--log-level");
            if (exit_code != 0) return false;
            if (!kd::log::parse_level(v, opt.log_level)) {
                std::fprintf(stderr, "kvit-asrd: --log-level wants trace|debug|info|warn|error\n");
                exit_code = 2;
                return false;
            }
            continue;
        }
        std::fprintf(stderr, "kvit-asrd: unknown option %s\n", a.c_str());
        print_usage();
        exit_code = 2;
        return false;
    }

    // There is deliberately no --api-key, no --silero-model and no --backend. Each is a separate,
    // later, argued addition; openai-translator.md §1.4 records why a silent omission inside the
    // HTTP client would be worse than an absent flag.
    const std::string why = validate_service_url(opt.service);
    if (!why.empty()) {
        std::fprintf(stderr, "kvit-asrd: %s\n", why.c_str());
        print_usage();
        exit_code = 2;
        return false;
    }
    return true;
}

// ─── The connection ─────────────────────────────────────────────────────────────────────────

// Exit codes, matching `server/kvit_asr`'s so a supervisor learns one set.
constexpr int kExitOk = 0;
constexpr int kExitProtocolError = 3;
constexpr int kExitTransport = 4;
constexpr int kExitInternal = 5;

class WsSink final : public MessageSink {
public:
    explicit WsSink(WsConnection& conn) : conn_(conn) {}

    bool send(const proto::Message& m) override {
        const std::vector<uint8_t> frame = proto::encode_control_frame(m);
        if (frame.empty()) {
            KD_LOG_ERROR("refusing to send a message that would not encode");
            return false;
        }
        KD_LOG_DEBUG("tx %s", proto::to_string(m.type()));
        return conn_.send_binary(
            std::string_view(reinterpret_cast<const char*>(frame.data()), frame.size()));
    }

private:
    WsConnection& conn_;
};

// One WebSocket message must hold exactly one protocol frame (§2.6.3), and the redundant 4-byte
// length prefix is checked against the message size. §2.6.3 keeps that field precisely so one
// decoder serves both bindings, and a decoder that ignores a field it is given will be wrong the
// first time the field matters — two frames packed into one message would otherwise decode as a
// legal stream and surface later as garbled text.
bool length_agrees(const std::string& message) {
    if (message.size() < proto::kFrameHeaderBytes + 1) return false;
    const auto* p = reinterpret_cast<const uint8_t*>(message.data());
    const uint32_t length = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                            (static_cast<uint32_t>(p[2]) << 16) |
                            (static_cast<uint32_t>(p[3]) << 24);
    return static_cast<uint64_t>(length) + proto::kFrameHeaderBytes == message.size();
}

// The whole conversation on one connection. Returns a process exit code, which only matters for
// the single-connection case; with several, the last one wins and nobody reads it.
int serve_connection(WsConnection& conn, const Options& opt) {
    WsSink sink(conn);
    proto::FrameReader reader;

    const auto send_error = [&](proto::ErrorCode code, bool fatal, const std::string& msg,
                                const std::string* sid) {
        proto::ErrorMsg e;
        e.code = code;
        e.code_raw = proto::to_string(code);
        e.fatal = fatal;
        e.msg = msg;
        if (sid != nullptr) e.session_id = *sid;
        if (code == proto::ErrorCode::proto_unsupported) e.supported.push_back(proto::kProtoVersion);
        sink.send(proto::Message::of(std::move(e)));
    };

    // ── §5 handshake ────────────────────────────────────────────────────────────────────────
    //
    // Read exactly one frame, require it to be `hello`, answer or reject. Anything else here —
    // audio, `start`, a second `hello` — is a fatal `state_violation`, because the handshake is
    // what establishes that both peers agree on the message set.
    bool have_hello = false;
    std::string hello_model;
    while (!have_hello) {
        WsOpcode opcode = WsOpcode::Binary;
        std::string message;
        if (!conn.recv_message(opcode, message)) {
            KD_LOG_INFO("connection closed before hello (%s)", conn.error().c_str());
            return kExitTransport;
        }
        if (opcode == WsOpcode::Text) {
            // §2.6.3, and fatal even though these bytes would have decoded: a text message means
            // the peer thinks this is JSON-over-WebSocket, and half its audio frames will arrive
            // mangled by a UTF-8 round trip.
            send_error(proto::ErrorCode::unknown_frame_type, true,
                       "a text message arrived; this protocol is binary only "
                       "(docs/protocol.md §2.6.3)",
                       nullptr);
            return kExitProtocolError;
        }
        if (!length_agrees(message)) {
            send_error(proto::ErrorCode::bad_frame, true,
                       "one WebSocket message must hold exactly one frame: the length field and "
                       "the message size disagree (docs/protocol.md §2.6.3)",
                       nullptr);
            return kExitProtocolError;
        }
        reader.push(message);

        proto::Frame frame;
        const proto::FrameStatus st = reader.next(frame);
        if (st == proto::FrameStatus::NeedMore) continue;
        if (st != proto::FrameStatus::Frame) {
            send_error(reader.error().code, true, reader.error().msg, nullptr);
            return kExitProtocolError;
        }
        if (frame.type != proto::FrameType::Control) {
            send_error(proto::ErrorCode::state_violation, true,
                       "audio frame before hello (docs/protocol.md §5)", nullptr);
            return kExitProtocolError;
        }

        proto::Message m;
        proto::ProtocolError err;
        if (!proto::decode_control(frame.payload, m, err)) {
            send_error(err.code, true, err.msg, nullptr);
            return kExitProtocolError;
        }
        const auto* hello = m.get_if<proto::HelloMsg>();
        if (hello == nullptr) {
            send_error(proto::ErrorCode::state_violation, true,
                       std::string("expected hello, got ") + proto::to_string(m.type()), nullptr);
            return kExitProtocolError;
        }
        if (hello->proto != proto::kProtoVersion) {
            // §5: reply proto_unsupported and close. No silent downgrade, no best-effort
            // continuation — a version we do not implement is a message set we cannot parse.
            send_error(proto::ErrorCode::proto_unsupported, true,
                       "this translator speaks proto " + std::to_string(proto::kProtoVersion) +
                           ", the client offered " + std::to_string(hello->proto),
                       nullptr);
            return kExitProtocolError;
        }
        // §5.1. One backend per process, so the catalogue has exactly one entry and the only
        // question is whether the client asked for something else. Refusing is the point:
        // serving a different model than the one requested produces transcripts that are merely
        // *worse*, which is the hardest kind of wrong for a user to notice.
        if (hello->model.has_value() && !hello->model->empty() && *hello->model != opt.model) {
            proto::ErrorMsg e;
            e.code = proto::ErrorCode::model_unavailable;
            e.code_raw = proto::to_string(e.code);
            e.fatal = true;
            e.msg = "this translator serves \"" + opt.model + "\", not \"" + *hello->model + "\"";
            e.available.push_back(opt.model);
            sink.send(proto::Message::of(std::move(e)));
            return kExitProtocolError;
        }
        hello_model = hello->model.value_or(opt.model);

        proto::HelloAckMsg ack;
        ack.proto = proto::kProtoVersion;
        ack.server = "kvit-asrd";
        proto::ModelInfo info;
        info.id = opt.model;
        // `mode` is stated here as well as in `ready`: unlike the Python server, this process
        // knows its backend before it loads anything, because there is exactly one and it is
        // compiled in.
        info.mode = proto::StreamingMode::PseudoStreaming;
        info.chunk_ms = static_cast<int32_t>(opt.finalize_only ? 0 : 1000);
        info.context_supported = false;
        ack.models.push_back(std::move(info));
        if (!sink.send(proto::Message::of(std::move(ack)))) return kExitTransport;
        KD_LOG_INFO("handshake_ok client=%s model=%s",
                    hello->client.value_or(std::string("?")).c_str(), hello_model.c_str());
        have_hello = true;
    }

    // ── Load ────────────────────────────────────────────────────────────────────────────────
    //
    // **Ready is earned.** `--service` is resolved and connected — a TCP connect, not a
    // transcription — before `ready` goes out, so the user is not talking into a dead box.
    // Failure is a fatal `error` *instead of* `ready`, which is what makes the client's Test
    // button mean "the translator and the speech host both answered".
    OpenAiBackendConfig backend_cfg;
    backend_cfg.base_url = opt.service;
    backend_cfg.model = opt.model;
    backend_cfg.language = opt.language;
    backend_cfg.finalize_only = opt.finalize_only;
    backend_cfg.vad_gate = opt.vad_gate;
    OpenAiBackend backend(backend_cfg);

    const ULONGLONG load_started = GetTickCount64();
    if (const Status st = backend.load(); !st.ok()) {
        send_error(proto::ErrorCode::connect_refused, true, st.message(), nullptr);
        KD_LOG_ERROR("load failed: %s", st.to_string().c_str());
        return kExitProtocolError;
    }

    proto::ReadyMsg ready;
    ready.model = opt.model;
    ready.warm_ms = static_cast<int64_t>(GetTickCount64() - load_started);
    // The protocol's taxonomy has no other value for a batch model behind a window, and
    // base.py's rule applies here too: a wrapper that relabelled itself would put uncomparable
    // latency numbers in the same column as the native-streaming rows.
    ready.mode = proto::StreamingMode::PseudoStreaming;
    ready.chunk_ms = static_cast<int32_t>(opt.finalize_only ? 0 : 1000);
    ready.context_supported = false;
    if (!sink.send(proto::Message::of(std::move(ready)))) return kExitTransport;

    // ── §6 the conversation ─────────────────────────────────────────────────────────────────

    SessionServerConfig session_cfg;
    session_cfg.stability.hypotheses_n = opt.stability_n;
    session_cfg.stability.retract_clusters_m = opt.stability_m;
    session_cfg.stability.hold_ms_t = opt.stability_hold_ms;
    session_cfg.vad.endpoint_silence_ms = static_cast<double>(opt.endpoint_silence_ms);
    // Read off the backend's own declaration rather than a setting. A pseudo-streaming backend
    // replaces its previous hypothesis instead of extending it; see SessionServerConfig.
    session_cfg.publish_stable_partials = !opt.finalize_only;
    session_cfg.hostile_stability = opt.hostile_stability;

    if (const std::string why = session_cfg.vad.validate(); !why.empty()) {
        KD_LOG_ERROR("VAD configuration is not coherent: %s", why.c_str());
        send_error(proto::ErrorCode::internal_error, true, why, nullptr);
        return kExitInternal;
    }

    SessionServer session(backend, session_cfg, sink);
    std::vector<int16_t> pcm;
    int exit_code = kExitOk;

    for (;;) {
        WsOpcode opcode = WsOpcode::Binary;
        std::string message;
        if (!conn.recv_message(opcode, message)) {
            KD_LOG_INFO("end of stream (%s)", conn.error().c_str());
            break;
        }
        if (opcode == WsOpcode::Text) {
            send_error(proto::ErrorCode::unknown_frame_type, true,
                       "a text message arrived; this protocol is binary only", nullptr);
            exit_code = kExitProtocolError;
            break;
        }
        if (!length_agrees(message)) {
            send_error(proto::ErrorCode::bad_frame, true,
                       "one WebSocket message must hold exactly one frame", nullptr);
            exit_code = kExitProtocolError;
            break;
        }
        reader.push(message);

        // Drain fully. Fixture 002 (three frames in one read) fails any caller that handles one
        // frame per read, and the rule survives the change of binding.
        bool fatal = false;
        for (;;) {
            proto::Frame frame;
            const proto::FrameStatus st = reader.next(frame);
            if (st == proto::FrameStatus::NeedMore) break;
            if (st == proto::FrameStatus::EndOfStream) { fatal = true; break; }
            if (st == proto::FrameStatus::Error) {
                send_error(reader.error().code, true, reader.error().msg, nullptr);
                exit_code = kExitProtocolError;
                fatal = true;
                break;
            }

            if (frame.type == proto::FrameType::Audio) {
                proto::ProtocolError err;
                if (!proto::decode_audio_payload(frame.payload, pcm, err)) {
                    send_error(err.code, true, err.msg, nullptr);
                    exit_code = kExitProtocolError;
                    fatal = true;
                    break;
                }
                if (!session.on_audio(pcm.data(), pcm.size())) { fatal = true; break; }
                continue;
            }

            proto::Message m;
            proto::ProtocolError err;
            if (!proto::decode_control(frame.payload, m, err)) {
                const std::string* sid = err.session_id.has_value() ? &*err.session_id : nullptr;
                send_error(err.code, err.fatal, err.msg, sid);
                if (err.fatal) {
                    exit_code = kExitProtocolError;
                    fatal = true;
                    break;
                }
                continue;
            }

            bool alive = true;
            if (m.get_if<proto::HelloMsg>() != nullptr) {
                send_error(proto::ErrorCode::state_violation, true,
                           "second hello on one connection", nullptr);
                exit_code = kExitProtocolError;
                fatal = true;
                break;
            } else if (const auto* start = m.get_if<proto::StartMsg>()) {
                alive = session.on_start(*start);
            } else if (const auto* stop = m.get_if<proto::StopMsg>()) {
                alive = session.on_stop(*stop);
            } else if (const auto* c = m.get_if<proto::CancelMsg>()) {
                alive = session.on_cancel(*c);
            } else if (const auto* d = m.get_if<proto::DiscontinuityMsg>()) {
                alive = session.on_discontinuity(*d);
            } else if (const auto* e = m.get_if<proto::ErrorMsg>()) {
                alive = session.on_peer_error(*e);
            } else {
                // A known message type that only the Server may send. Not `unknown_message` —
                // we know exactly what it is; it is illegal in this direction (§11.3).
                send_error(proto::ErrorCode::state_violation, true,
                           std::string(proto::to_string(m.type())) +
                               " is a Server->Client message",
                           nullptr);
                exit_code = kExitProtocolError;
                fatal = true;
                break;
            }
            if (!alive || session.connection_fatal()) {
                exit_code = kExitProtocolError;
                fatal = true;
                break;
            }
        }
        if (fatal) break;
    }

    session.close();
    return exit_code;
}

// ─── Parent liveness ────────────────────────────────────────────────────────────────────────

std::atomic<bool>* g_shutdown = nullptr;
WsListener* g_listener = nullptr;

BOOL WINAPI console_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        if (g_shutdown != nullptr) g_shutdown->store(true);
        if (g_listener != nullptr) g_listener->stop();
        return TRUE;
    }
    return FALSE;
}

// Watch the parent and exit when it dies.
//
// The job object the client puts this process in is the real guarantee — it is what makes an
// orphan impossible even if this process is wedged — and this watch is the backup for the case
// where the child somehow ends up outside that job. `kd_probe` and standalone use omit
// `--parent-pid` and this thread is never started.
void watch_parent(uint32_t pid, std::atomic<bool>& shutdown, WsListener& listener) {
    const HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (h == nullptr) {
        KD_LOG_WARN("cannot open parent pid %u to watch it (error %lu); relying on the job object",
                    pid, GetLastError());
        return;
    }
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    KD_LOG_INFO("parent pid %u exited; shutting down", pid);
    shutdown.store(true);
    listener.stop();
}

int run(int argc, char** argv) {
    Options opt;
    int exit_code = 0;
    if (!parse_args(argc, argv, opt, exit_code)) return exit_code;

    kd::log::Config log_cfg;
    log_cfg.level = opt.log_level;
    // stderr and nothing else. Supervised, the parent redirects this process's stderr into the
    // client's log; standalone, it is the console. No file: a separate asrd.log would split the
    // first support question across two files.
    log_cfg.to_stderr = true;
    log_cfg.to_debugger = false;
    kd::log::init(log_cfg);

    KD_LOG_INFO("kvit-asrd starting: service=%s model=%s %s vad_gate=%s", opt.service.c_str(),
                opt.model.c_str(), opt.finalize_only ? "finalize-only" : "live-preview",
                opt.vad_gate ? "on" : "off");
    if (opt.hostile_stability) {
        KD_LOG_ERROR(
            "--hostile-stability is on. This build deliberately emits non-conforming "
            "stable_bytes and exists only to exercise docs/protocol.md §8.3. Do not ship it.");
    }

    WsListener listener;
    if (const Status st = listener.listen(opt.listen_host, opt.listen_port); !st.ok()) {
        KD_LOG_ERROR("cannot listen on %s:%u — %s", opt.listen_host.c_str(), opt.listen_port,
                     st.to_string().c_str());
        std::fprintf(stderr, "kvit-asrd: cannot listen on %s:%u — %s\n", opt.listen_host.c_str(),
                     opt.listen_port, st.to_string().c_str());
        kd::log::shutdown();
        return kExitTransport;
    }
    // Logged rather than merely known: the client chooses the port for the supervised case, and
    // this line is what a support question is answered from.
    KD_LOG_INFO("listening on ws://%s:%u/", opt.listen_host.c_str(), listener.port());

    std::atomic<bool> shutdown{false};
    g_shutdown = &shutdown;
    g_listener = &listener;
    SetConsoleCtrlHandler(console_handler, TRUE);

    std::thread parent_watch;
    if (opt.parent_pid != 0) {
        parent_watch = std::thread(
            [&] { watch_parent(opt.parent_pid, shutdown, listener); });
        parent_watch.detach();
    }

    int last = kExitOk;
    std::atomic<uint32_t> live{0};
    std::vector<std::thread> connections;
    while (!shutdown.load()) {
        bool retry = false;
        std::unique_ptr<WsConnection> conn = listener.accept(retry);
        if (conn == nullptr) {
            if (retry) continue;
            break;
        }

        const bool admitted = opt.max_connections == 0 || live.load() < opt.max_connections;
        if (!admitted) {
            // §2.7: refused **in words**, not in silence. A connection that never becomes ready
            // and a socket that hangs up both present to the user as "it hangs".
            KD_LOG_WARN("refusing a connection from %s: --max-connections %u is reached",
                        conn->peer().c_str(), opt.max_connections);
            WsSink sink(*conn);
            proto::ErrorMsg e;
            e.code = proto::ErrorCode::server_busy;
            e.code_raw = proto::to_string(e.code);
            e.fatal = true;
            e.msg = "this translator serves one connection at a time";
            e.retry_after_ms = 1000;
            sink.send(proto::Message::of(std::move(e)));
            conn->close();
            continue;
        }

        if (opt.max_connections == 1) {
            // The common case, and worth keeping on this thread: with one connection there is
            // nothing to overlap, and a thread per connection would only make the exit code and
            // the shutdown path harder to follow.
            ++live;
            last = serve_connection(*conn, opt);
            --live;
            conn->close();
            continue;
        }

        ++live;
        connections.emplace_back([&, c = std::move(conn)]() mutable {
            serve_connection(*c, opt);
            c->close();
            --live;
        });
    }

    listener.stop();
    for (std::thread& t : connections) {
        if (t.joinable()) t.join();
    }
    KD_LOG_INFO("kvit-asrd exiting (%d)", last);
    kd::log::shutdown();
    return last;
}

}  // namespace
}  // namespace kd::asrd

int main(int argc, char** argv) { return kd::asrd::run(argc, argv); }
