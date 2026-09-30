// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// One connection, end to end — see connection.h for why this is not inside main.cpp.

#define KD_LOG_MODULE "asrd"

#include "asrd/connection.h"

#include <windows.h>

#include <string>
#include <vector>

#include "asrd/session_server.h"
#include "protocol.h"
#include "util/log.h"

namespace kd::asrd {
namespace {

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

}  // namespace

bool length_agrees(const std::string& message) {
    if (message.size() < proto::kFrameHeaderBytes + 1) return false;
    const auto* p = reinterpret_cast<const uint8_t*>(message.data());
    const uint32_t length = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                            (static_cast<uint32_t>(p[2]) << 16) |
                            (static_cast<uint32_t>(p[3]) << 24);
    return static_cast<uint64_t>(length) + proto::kFrameHeaderBytes == message.size();
}

int serve_connection(WsConnection& conn, const TranslatorConfig& cfg) {
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
        if (hello->model.has_value() && !hello->model->empty() && *hello->model != cfg.model) {
            proto::ErrorMsg e;
            e.code = proto::ErrorCode::model_unavailable;
            e.code_raw = proto::to_string(e.code);
            e.fatal = true;
            e.msg = "this translator serves \"" + cfg.model + "\", not \"" + *hello->model + "\"";
            e.available.push_back(cfg.model);
            sink.send(proto::Message::of(std::move(e)));
            return kExitProtocolError;
        }
        hello_model = hello->model.value_or(cfg.model);

        proto::HelloAckMsg ack;
        ack.proto = proto::kProtoVersion;
        ack.server = "kvit-asrd";
        proto::ModelInfo info;
        info.id = cfg.model;
        // `mode` is stated here as well as in `ready`: unlike the Python server, this process
        // knows its backend before it loads anything, because there is exactly one and it is
        // compiled in.
        info.mode = proto::StreamingMode::PseudoStreaming;
        info.chunk_ms = static_cast<int32_t>(cfg.finalize_only ? 0 : 1000);
        // The endpoint takes vocabulary hints as `prompt`; `session_server` joins `start.context`
        // and sends them. Measured 2026-09-20: on the same audio, "Qt UI" becomes "kvit-ui" and
        // "Piper D" becomes "Piperd" once the terms are on the wire.
        info.context_supported = true;
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
    //
    // The endpointer is configured *first*, and its bound on an utterance is a precondition
    // for the backend rather than a detail of it.
    //
    // `OpenAiBackend` buffers the whole utterance and never drops a sample — that invariant is
    // what makes a duplicated word impossible there (see `PseudoStreamingDriver`). The price is
    // that nothing inside the backend bounds the buffer, so something outside it must, and the
    // endpointer already does: `max_utterance_ms` closes an utterance after 30 s.
    //
    // Setting it to 0 disables that, which this backend cannot survive: it would buffer, and
    // POST, without limit. The previous design "handled" an unbounded utterance by sliding a
    // window and stitching the seam, and the stitch is precisely the thing that put the same
    // speech in the user's document twice. Refusing at startup, with a message, is the honest
    // alternative to a heuristic that fails quietly and irreversibly.
    VadConfig vad_cfg;
    vad_cfg.endpoint_silence_ms = static_cast<double>(cfg.endpoint_silence_ms);
    if (const std::string why = vad_cfg.validate(); !why.empty()) {
        KD_LOG_ERROR("VAD configuration is not coherent: %s", why.c_str());
        send_error(proto::ErrorCode::internal_error, true, why, nullptr);
        return kExitInternal;
    }
    if (vad_cfg.max_utterance_ms == 0.0) {
        const std::string why =
            "max_utterance_ms is 0, so an utterance is never closed by the endpointer. This "
            "backend buffers the whole utterance and never drops audio, so it needs that bound; "
            "there is no windowing fallback, by design.";
        KD_LOG_ERROR("%s", why.c_str());
        send_error(proto::ErrorCode::internal_error, true, why, nullptr);
        return kExitInternal;
    }

    OpenAiBackendConfig backend_cfg;
    backend_cfg.base_url = cfg.service;
    backend_cfg.model = cfg.model;
    backend_cfg.timeout_ms = cfg.http_timeout_ms;
    backend_cfg.language = cfg.language;
    backend_cfg.finalize_only = cfg.finalize_only;
    backend_cfg.vad_gate = cfg.vad_gate;
    backend_cfg.api_key = cfg.api_key;
    OpenAiBackend backend(backend_cfg);
    // Only when there is somewhere to write it: the capture keeps a copy of every posted window,
    // which is the whole utterance on every hop, and nobody should pay that to produce nothing.
    backend.set_capture_window(!cfg.revision_dir.empty());

    const ULONGLONG load_started = GetTickCount64();
    if (const Status st = backend.load(); !st.ok()) {
        send_error(proto::ErrorCode::connect_refused, true, st.message(), nullptr);
        KD_LOG_ERROR("load failed: %s", st.to_string().c_str());
        return kExitProtocolError;
    }

    proto::ReadyMsg ready;
    ready.model = cfg.model;
    ready.warm_ms = static_cast<int64_t>(GetTickCount64() - load_started);
    // The protocol's taxonomy has no other value for a batch model behind a window, and
    // base.py's rule applies here too: a wrapper that relabelled itself would put uncomparable
    // latency numbers in the same column as the native-streaming rows.
    ready.mode = proto::StreamingMode::PseudoStreaming;
    ready.chunk_ms = static_cast<int32_t>(cfg.finalize_only ? 0 : 1000);
    ready.context_supported = true;  // see the `hello_ack` above
    if (!sink.send(proto::Message::of(std::move(ready)))) return kExitTransport;

    // ── §6 the conversation ─────────────────────────────────────────────────────────────────

    SessionServerConfig session_cfg;
    session_cfg.stability = cfg.stability;
    session_cfg.vad = vad_cfg;
    // Two switches, not one, and they are set from different facts.
    //
    // Whether to *show* anything mid-utterance is the user's choice, and `--finalize-only off`
    // is how it arrives. Whether a `partial` may carry a *mark* is the backend's property, and
    // this process has exactly one backend: an OpenAI-compatible batch service, which re-decodes
    // the whole window every hop and re-punctuates what it already said. Per
    // `SessionServerConfig::publish_stable_partials`'s own comment — "false for any backend that
    // re-decodes the whole utterance from scratch" — that is a permanent no, not a mode.
    //
    // These used to be the same line, `= !cfg.finalize_only`, which made turning the preview on
    // also promise a mark this backend cannot keep. It kept it for 490 bytes and then re-cased a
    // word at byte 421; §8.3 obliges the sender to refuse, so the session was failed and the
    // utterance in flight was lost. Preview now publishes `stable_bytes = 0` and promises
    // nothing, so there is no mark to contradict and everything still commits at the `final`.
    session_cfg.emit_partials = !cfg.finalize_only;
    session_cfg.publish_stable_partials = false;
    session_cfg.revision_dir = cfg.revision_dir;
    session_cfg.hostile_stability = cfg.hostile_stability;

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

}  // namespace kd::asrd
