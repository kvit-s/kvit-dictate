// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// Wire protocol codec: framing, message structs, encode/decode. **Pure — no Windows.**
//
// Implements: docs/protocol.md in full (proto 1). That document is normative; where this header
// and that document disagree, the document wins and this header is the bug. Section references
// below point into it.
//
// ─── Scope, and the deliberate absence of Windows ───────────────────────────────────────────
//
// This header includes no <windows.h>, declares no HANDLE, and performs no I/O. Everything here
// is a pure function over bytes: bytes in -> frames -> messages, and messages -> bytes. That is
// what makes the framing and message layers unit-testable against the cross-language fixtures
// in tests/fixtures/protocol/ without a pipe, a child process, or a message loop.
//
// The transport — pipe creation, DACL, child spawn, overlapped I/O, peer validation — lives in
// ipc_client.h, which owns docs/protocol.md §2 and calls into this file for §3 and §4.
//
// ─── Role naming, restated because it is the most confusing thing in the spec ───────────────
//
// `kvit-dictate.exe` is the **pipe server** (it creates the instance) but the **protocol
// client** (it drives sessions). `asr-server` is the **pipe client** but the **protocol
// server**. Throughout this header, "Client" and "Server" mean the *protocol* roles. See
// docs/protocol.md §1.
//
// ─── Threading ──────────────────────────────────────────────────────────────────────────────
//
// Every function here is reentrant and free of global state. FrameReader owns a buffer and is
// therefore *not* thread-safe; it is single-owner, and in practice that owner is the IPC read
// thread. Encoding functions are pure and callable from any thread — including the WASAPI
// capture thread, which encodes audio frames inline.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "util/json.h"

namespace kd::proto {

// ─── §1.1 Constants ─────────────────────────────────────────────────────────────────────────
//
// These are normative. Nothing may hard-code them a second time.

inline constexpr int32_t kProtoVersion = 1;

inline constexpr uint32_t kMaxPayload = 65536;      // 64 KiB
inline constexpr uint32_t kMinLengthField = 1;      // length covers the type byte -> never 0
inline constexpr uint32_t kMaxLengthField = 65537;  // kMaxPayload + 1
inline constexpr uint32_t kMaxFrameOnWire = 65541;  // 4 + kMaxLengthField
inline constexpr uint32_t kFrameHeaderBytes = 4;    // the u32 length prefix only

inline constexpr uint32_t kSampleRate = 16000;
inline constexpr uint32_t kChannels = 1;

inline constexpr uint32_t kAudioQueueMaxMs = 3000;
inline constexpr uint32_t kControlQueueMax = 256;
inline constexpr uint32_t kWriteDeadlineMs = 5000;
inline constexpr uint32_t kConnectTimeoutMs = 15000;
inline constexpr uint32_t kHelloTimeoutMs = 5000;
inline constexpr uint32_t kLoadingHeartbeatMs = 5000;
inline constexpr uint32_t kReadyTimeoutMs = 180000;

// Recommended audio payload window, §6.3. Not enforced — a conforming peer may send anything
// from 0 to kMaxPayload — but a sender outside this band is doing something worth logging.
inline constexpr uint32_t kAudioFrameMsMinRecommended = 20;
inline constexpr uint32_t kAudioFrameMsMaxRecommended = 100;

// §6.1 `start.context` limits.
inline constexpr size_t kMaxContextItems = 256;
inline constexpr size_t kMaxContextItemBytes = 128;

// §7.1 `session_id` limits.
inline constexpr size_t kMaxSessionIdBytes = 64;

// ─── §3.2 Frame types ───────────────────────────────────────────────────────────────────────

enum class FrameType : uint8_t {
    Control = 0x01,  // UTF-8 JSON object
    Audio = 0x02,    // raw PCM, 16 kHz mono s16le
};

// True for the two types defined in proto 1. An undefined byte is a **fatal**
// `unknown_frame_type`, never a skippable extension (§3.2) — the handshake exists precisely so
// both peers agree on the message set, so an undefined type means one peer is broken.
bool is_known_frame_type(uint8_t b) noexcept;

// ─── §11.3 Error codes ──────────────────────────────────────────────────────────────────────
//
// The enumerator names match the wire strings exactly, in snake_case, so a reader comparing
// this file with §11.3 does not have to translate. `to_string()` is the wire spelling.

enum class ErrorCode : uint8_t {
    // Fatal, wire-visible.
    proto_unsupported,
    bad_frame,
    frame_too_large,
    truncated_frame,
    unknown_frame_type,
    bad_utf8,
    bad_json,
    bad_message,
    unknown_message,
    state_violation,
    overload_audio,
    overload_control,
    model_load_failed,
    internal_error,
    // Added with the network binding (docs/protocol.md §2.7, §5.1). Both are Server -> Client
    // and both are fatal: the connection cannot proceed, but the endpoint is working and said
    // so, which is a materially different thing for the user than a socket that did not open.
    server_busy,
    model_unavailable,
    // Non-fatal, wire-visible. `session_id` is REQUIRED on these (§11.1).
    stability_violation,
    decode_failed,
    audio_underrun,
    // Locally-detected only — never sent, because there is no peer to tell (§11.3, final
    // paragraph). They use the same spellings so a log reader learns one vocabulary.
    spawn_timeout,
    handshake_timeout,
    model_load_timeout,
    peer_mismatch,
    pipe_squatted,
    // The network binding's local failures (docs/protocol.md §11.3, final paragraph). Kept
    // apart rather than collapsed into one "cannot connect", because each one has a different
    // next action for the user and the tray text is the only place they will ever see it.
    connect_timeout,     // the host did not answer: wrong address, machine off, firewall
    connect_refused,     // it answered and said no: server not running, wrong port
    tls_failed,          // wss:// and a certificate this machine does not trust
    no_server_configured,  // the settings are empty. Not an error — see ConnState::NoServer.
    // Forward tolerance (§13): a code this build does not know. `ErrorMsg::code_raw` keeps the
    // original string; the receiver acts on `fatal`. This is the only place in the protocol
    // where an unknown value is tolerated, and the reason is that the alternative — a fatal
    // error about a fatal error — loses the diagnostic being delivered.
    unrecognised,
};

const char* to_string(ErrorCode c) noexcept;

// Never fails: an unrecognised string maps to ErrorCode::unrecognised, per §13.
ErrorCode error_code_from_string(std::string_view s) noexcept;

// True for codes that may legally appear on the wire (i.e. not the local-only set).
bool is_wire_error_code(ErrorCode c) noexcept;

// The `fatal` value the spec assigns to this code. Advisory only — a receiver MUST act on the
// `fatal` field it actually received, not on this table, because §13 requires unknown codes to
// be handled by their flag. Used when *originating* an error.
bool default_fatal(ErrorCode c) noexcept;

// A detected fault, at any layer. Carries enough to satisfy §8.3 step 3's logging requirement
// and §12.3's `at_offset` field.
struct ProtocolError {
    ErrorCode code = ErrorCode::internal_error;
    bool fatal = true;
    std::string msg;                         // prose, for logs and the tray. Never parsed.
    std::optional<std::string> session_id;   // REQUIRED when !fatal (§11.1)
    uint64_t at_offset = 0;                  // byte offset in the stream, for framing faults
    std::vector<int32_t> supported;          // only with proto_unsupported

    [[nodiscard]] std::string to_string() const;
};

// ─── §3 Framing ─────────────────────────────────────────────────────────────────────────────
//
//  0        1        2        3        4        5                    4+length
//  +--------+--------+--------+--------+--------+--------------------+
//  |      length  (u32, little-endian) |  type  |  payload           |
//  +--------+--------+--------+--------+--------+--------------------+
//                                      \___________________________/
//                                        length counts these bytes
//
// **length = 1 + sizeof(payload).** The length field covers the type byte. §3.1 records why:
// the reader needs exactly `length` more bytes after the prefix with no special case, and
// `length == 0` becomes structurally impossible, turning "forgot the type byte" into a loud
// bad_frame instead of a silent one-byte desync that surfaces as garbage text much later.
//
// Little-endian, no htonl. Readers MUST treat it as unsigned; a reader that sign-extends fails
// fixture 007 (`0xFFFFFFFF`).

struct Frame {
    FrameType type = FrameType::Control;
    // Points into the FrameReader's internal buffer. **Valid only until the next call to
    // push(), next(), or reset() on that reader.** Copy it if you need to keep it — the session
    // machine does, because a message outlives the read that delivered it.
    std::string_view payload;
};

enum class FrameStatus : uint8_t {
    Frame,        // `out` holds a frame
    NeedMore,     // buffered bytes do not yet complete a frame; push() more
    EndOfStream,  // EOF was signalled and the buffer is empty — orderly shutdown (fixture 010)
    Error,        // error() describes the fault; the connection is over
};

// A byte-stream frame decoder that tolerates arbitrary fragmentation and coalescing.
//
// **Message mode does not excuse a message-oriented parser** (§2.2). The pipe is created with
// PIPE_READMODE_MESSAGE, but three real situations break the one-read-one-message assumption:
// a reader whose buffer is smaller than the message gets ERROR_MORE_DATA and must reassemble;
// a peer that sets PIPE_READMODE_BYTE on its own handle silently changes delivery; and the
// Python side reaches the pipe through ctypes/kernel32 where the read mode is easy to get
// wrong. Fixtures 001 and 002 fail any decoder that assumes otherwise.
//
// Usage:
//     FrameReader r;
//     r.push(bytes, n);                    // as delivered, however fragmented
//     Frame f;
//     for (;;) {
//         switch (r.next(f)) {
//             case FrameStatus::Frame:     handle(f); continue;   // drain fully before reading
//             case FrameStatus::NeedMore:  break;                 // read more from the pipe
//             case FrameStatus::EndOfStream: return orderly();
//             case FrameStatus::Error:     return fail(r.error());
//         }
//         break;
//     }
//
// Note the loop shape: after every push() the caller must drain with next() until NeedMore.
// Fixture 002 (three frames in one read) fails any caller that handles one frame per read.
//
// **Frames decoded before an error are still delivered.** A reader that discards already-decoded
// frames when it later hits a bad header fails fixture 006. That is why errors surface through
// next()'s return value rather than through push().
class FrameReader {
public:
    FrameReader();
    ~FrameReader();
    FrameReader(const FrameReader&) = delete;
    FrameReader& operator=(const FrameReader&) = delete;

    // Append bytes exactly as they were delivered. Never blocks, never fails: the size limit is
    // enforced on the length prefix (§3.3), not on the buffer, so push() cannot be the place a
    // frame is rejected.
    void push(const void* data, size_t size);
    void push(std::string_view bytes) { push(bytes.data(), bytes.size()); }

    // Signal end of stream. After this, next() returns EndOfStream if the buffer is empty and
    // Error(truncated_frame) if a partial frame remains (fixture 011).
    void notify_eof() noexcept;

    FrameStatus next(Frame& out) noexcept;

    // Valid after next() returned Error. Sticky: once errored, next() keeps returning Error.
    [[nodiscard]] const ProtocolError& error() const noexcept;

    // Total bytes consumed from the stream so far — the base for ProtocolError::at_offset.
    [[nodiscard]] uint64_t stream_offset() const noexcept;

    // Bytes buffered but not yet forming a complete frame.
    [[nodiscard]] size_t buffered() const noexcept;

    // Return to the initial state. Used between fixture cases; never on a live connection,
    // because §3.3 forbids resynchronisation — there is no framing marker to resynchronise to,
    // so any byte pattern is a legal payload and a scan would find false headers.
    void reset() noexcept;

private:
    struct Impl;
    Impl* impl_;
};

// ─── Frame encoding ─────────────────────────────────────────────────────────────────────────
//
// A writer MUST issue exactly one write per frame and MUST NOT split a frame across writes or
// pack two frames into one write (§2.2). Every function here therefore produces one complete,
// contiguous frame; the transport writes the whole buffer in a single overlapped write.

// Appends `4 + 1 + payload.size()` bytes to `out`. Returns false — writing nothing — if the
// payload exceeds kMaxPayload, which is a local bug rather than a wire condition.
bool encode_frame(FrameType type, std::string_view payload, std::vector<uint8_t>& out);

// Convenience forms that allocate. Empty on failure.
std::vector<uint8_t> encode_frame(FrameType type, std::string_view payload);

// Audio: `samples` are host-order int16 and are written little-endian. On x64 that is a memcpy,
// but the conversion is written explicitly so the code does not become wrong by accident if it
// is ever read on another architecture.
bool encode_audio_frame(const int16_t* samples, size_t count, std::vector<uint8_t>& out);

// ─── §6.3 Audio payload decoding ────────────────────────────────────────────────────────────

// Decode an audio payload into int16 samples.
//
// **The alignment gotcha, stated because it is easy to get wrong:** the payload is a
// `std::string_view` into a read buffer and is *not* guaranteed to be 2-byte aligned, so
// `reinterpret_cast<const int16_t*>(payload.data())` is undefined behaviour and will be
// mis-optimised long before it faults. This function copies. Do not replace it with a cast.
//
// An odd payload length is `bad_message` (§6.3) — it means the sender split a sample, which
// means its buffer arithmetic is wrong. A zero-length payload is legal and yields no samples
// (fixture 003); it is a no-op, not an error.
bool decode_audio_payload(std::string_view payload, std::vector<int16_t>& out, ProtocolError& err);

// ─── §6 Message catalogue ───────────────────────────────────────────────────────────────────

enum class MessageType : uint8_t {
    // Client -> Server
    Hello,
    Start,
    Stop,
    Cancel,
    Discontinuity,
    // Server -> Client
    HelloAck,
    Loading,
    Ready,
    Partial,
    Final,
    Stopped,
    // Bidirectional
    Error,
};

const char* to_string(MessageType t) noexcept;              // the wire `t` value
bool message_type_from_string(std::string_view s, MessageType& out) noexcept;

// True if this message type is legally sent by the Client / by the Server. `Error` is both.
bool is_client_message(MessageType t) noexcept;
bool is_server_message(MessageType t) noexcept;

// §6.2 `ready.mode` — exactly the taxonomy Phase 0A records (plan.md §3), so latency numbers
// stay interpretable. Any other value is `bad_message`.
enum class StreamingMode : uint8_t { NativeStreaming, StatefulChunked, PseudoStreaming };

const char* to_string(StreamingMode m) noexcept;  // "native-streaming", …
bool streaming_mode_from_string(std::string_view s, StreamingMode& out) noexcept;

// §6.2 `stopped.reason`. Free-form on the wire; this enum covers the four documented values and
// keeps the raw string for anything else.
enum class StopReason : uint8_t { Stop, Cancel, Error, UnknownSession, Other };

const char* to_string(StopReason r) noexcept;
StopReason stop_reason_from_string(std::string_view s) noexcept;

// ── Client -> Server ────────────────────────────────────────────────────────────────────────

struct HelloMsg {
    int32_t proto = kProtoVersion;         // R, range 1..65535
    std::optional<std::string> client;     // O, diagnostics only; never parsed for behaviour
    // O, §5.1. Absent means "whatever the server serves by default". A server that does not
    // have the named model answers `model_unavailable`; substituting another is forbidden,
    // because the result is transcripts that are merely worse and nobody notices.
    std::optional<std::string> model;
};

struct StartMsg {
    std::string session_id;                // R, §7.1
    int32_t sample_rate = kSampleRate;     // R, MUST be 16000 in proto 1
    int32_t channels = 1;                  // O, default 1, MUST be 1
    std::string format = "s16le";          // O, default "s16le", MUST be "s16le"
    std::vector<std::string> context;      // O, ≤256 items, ≤128 bytes each
    // Distinguishes "sent an empty array" from "did not send the field". A Server without
    // context support must report context_supported:false in `ready` rather than silently
    // ignoring the field, because plan.md §4 compares context-enabled and context-free runs
    // separately and a silent drop would corrupt that comparison.
    bool context_present = false;
};

struct StopMsg {
    std::string session_id;  // R
};

struct CancelMsg {
    std::string session_id;  // R
};

struct DiscontinuityMsg {
    std::string session_id;               // R
    int64_t gap_ms = 0;                   // R, ≥0. 0 means "a gap occurred, duration unknown".
    std::optional<std::string> reason;    // O, e.g. "AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY"
};

// ── Server -> Client ────────────────────────────────────────────────────────────────────────

// One entry of `hello_ack.models[]` (§5.1). Everything but `id` is optional, because a server
// that knows only the ids of its models is still more useful than one that advertises nothing.
struct ModelInfo {
    std::string id;                              // R
    std::optional<std::string> display_name;     // O
    std::optional<StreamingMode> mode;           // O, the ready.mode taxonomy
    std::optional<int32_t> chunk_ms;             // O
    bool context_supported = false;              // O, default false
};

struct HelloAckMsg {
    int32_t proto = kProtoVersion;      // R, MUST equal the offered proto
    std::optional<std::string> server;  // O
    // O, §5.1. Empty means "not advertised", never "there is only one" — a receiver that
    // inferred a one-model server from an absent catalogue would hide every other model the
    // server has. The settings page falls back to a free-text field instead.
    std::vector<ModelInfo> models;
};

struct LoadingMsg {
    std::string model;                   // R, as spelled in models.json
    std::optional<std::string> stage;    // O
    std::optional<int32_t> progress;     // O, 0..100. Absent means unknown — a Client MUST NOT
                                         // invent a progress bar from its absence.
};

struct ReadyMsg {
    std::string model;                              // R
    int64_t warm_ms = 0;                            // R, ≥0
    StreamingMode mode = StreamingMode::NativeStreaming;  // R
    std::optional<int32_t> chunk_ms;                // O
    bool context_supported = false;                 // O, default false
};

struct PartialMsg {
    std::string session_id;                 // R
    uint64_t utterance_id = 0;              // R, ≥1
    uint64_t seq = 0;                       // R, ≥1, strictly increasing per session
    std::string text;                       // R, CUMULATIVE for the utterance, not a delta
    uint64_t stable_bytes = 0;              // R, §8.2
    std::optional<int64_t> audio_pos_ms;    // O — see docs/protocol.md §14 item 4
};

struct FinalMsg {
    std::string session_id;                  // R
    uint64_t utterance_id = 0;               // R
    uint64_t seq = 0;                        // R, same sequence space as Partial
    std::string text;                        // R, complete text of the utterance
    int64_t audio_pos_ms = 0;                // R, ≥0
    std::optional<uint64_t> stable_bytes;    // O, if present MUST equal len(utf8(text))
};

struct StoppedMsg {
    std::string session_id;               // R
    std::optional<std::string> reason;    // O
};

struct ErrorMsg {
    ErrorCode code = ErrorCode::internal_error;  // R
    std::string code_raw;                        // the wire spelling, kept for unknown codes
    bool fatal = true;                           // R
    std::string msg;                             // R, prose, never parsed
    std::optional<std::string> session_id;       // REQUIRED when fatal == false (§11.1)
    std::vector<int32_t> supported;              // O, only with proto_unsupported
    std::optional<int32_t> retry_after_ms;       // O, only with server_busy (§2.7)
    std::vector<std::string> available;          // O, only with model_unavailable (§5.1)
};

// One decoded control message.
//
// A std::variant rather than a flat struct with every field, because a flat struct makes it
// possible to read `partial.audio_pos_ms` out of a `stopped` and get a plausible zero. The
// helper accessors below keep the ceremony down at call sites.
class Message {
public:
    using Body = std::variant<HelloMsg, StartMsg, StopMsg, CancelMsg, DiscontinuityMsg,
                              HelloAckMsg, LoadingMsg, ReadyMsg, PartialMsg, FinalMsg,
                              StoppedMsg, ErrorMsg>;

    Message() : body_(HelloMsg{}) {}
    explicit Message(Body b) : body_(std::move(b)) {}

    template <typename T>
    static Message of(T m) {
        return Message(Body{std::move(m)});
    }

    [[nodiscard]] MessageType type() const noexcept;

    [[nodiscard]] const Body& body() const noexcept { return body_; }
    [[nodiscard]] Body& body() noexcept { return body_; }

    template <typename T>
    [[nodiscard]] const T* get_if() const noexcept {
        return std::get_if<T>(&body_);
    }
    template <typename T>
    [[nodiscard]] T* get_if() noexcept {
        return std::get_if<T>(&body_);
    }

    // The `session_id` this message carries, or nullptr for the ones that carry none
    // (hello, hello_ack, loading, ready, and a fatal error that precedes any session).
    // This is the accessor the staleness rule in §7.1 is written against, so that the rule is
    // implemented once rather than per message type.
    [[nodiscard]] const std::string* session_id() const noexcept;

    // True for `partial` and `final` — the two message types §7.1 requires be **dropped** when
    // stale, as opposed to `stopped`/`error`, which are accepted for bookkeeping only.
    [[nodiscard]] bool carries_text() const noexcept;

    // True for the message types that consume a `seq` number (§7.4): exactly partial and final.
    [[nodiscard]] bool consumes_seq() const noexcept;

private:
    Body body_;
};

// ─── §4 Control-frame encoding and decoding ─────────────────────────────────────────────────

// Decode a control payload into a Message.
//
// Fails in the layer order §1 requires, with the code that layer owns:
//   1. not well-formed UTF-8            -> bad_utf8     (never substitute U+FFFD, §4.1)
//   2. not a conforming JSON object     -> bad_json
//   3. `t` unknown                      -> unknown_message
//   4. schema fault (missing/wrong type/
//      out of range/duplicate)          -> bad_message
//
// **Unknown fields in a known message are ignored** (with a log line); an **unknown `t` is
// fatal**. §4.2 states the asymmetry: an extra field cannot change the meaning of a message a
// receiver already understands, while an unknown message type means the peer believes something
// is happening that this peer knows nothing about.
bool decode_control(std::string_view payload, Message& out, ProtocolError& err);

// Serialize a Message to compact UTF-8 JSON. Optional fields that are absent are omitted, not
// emitted as null: the protocol never defines an explicit null and a receiver should reject one.
std::string encode_control(const Message& m);

// Message -> complete frame, ready for one write.
std::vector<uint8_t> encode_control_frame(const Message& m);

// ─── §7, §8 Pure validators ─────────────────────────────────────────────────────────────────

// §7.1: 1–64 bytes of ASCII [A-Za-z0-9_-]. Restricted so it is safe in log lines and file names.
bool is_valid_session_id(std::string_view s) noexcept;

// §8.2 requirements 1 and 2, together: `0 ≤ stable_bytes ≤ len(utf8(text))` **and**
// `stable_bytes` falls on an extended-grapheme-cluster boundary.
//
// This is the function the `text`-kind fixtures (012, 013) name. Fixture 013 exists entirely to
// prove that the UTF-8 boundary check alone is not sufficient: every offset it rejects is a
// valid code-point boundary.
bool is_valid_stable_offset(std::string_view text, uint64_t stable_bytes) noexcept;

// §8.2 requirement 3 — the monotonicity pair, both halves.
enum class StabilityCheck : uint8_t {
    Ok,
    OutOfRange,           // stable_bytes > len(utf8(text))
    NotGraphemeBoundary,  // lands inside a cluster
    Decreased,            // stable_bytes went backwards within the utterance (fixture 017)
    PrefixMutated,        // stable_bytes did not decrease, but the bytes below it changed
                          // (fixture 018) — the half that is easy to forget and the one with
                          // teeth, because those bytes may already be in the user's document
};

const char* to_string(StabilityCheck c) noexcept;

// Check a new hypothesis against the utterance's high-water mark. `prev_text` is the text of the
// message that established `prev_stable`; pass an empty view and 0 at the start of an utterance.
// On PrefixMutated, `diverge_offset` (if non-null) receives the first differing byte offset —
// §8.3 step 3 requires it in the log line.
StabilityCheck check_stability(std::string_view prev_text, uint64_t prev_stable,
                               std::string_view text, uint64_t stable_bytes,
                               size_t* diverge_offset = nullptr) noexcept;

// ─── §10.1 Connection states ────────────────────────────────────────────────────────────────
//
// Defined here rather than in session.h because they are protocol states named by the spec, and
// the fixture files (§12.5 `initial.state`) spell them exactly this way.

enum class ConnState : uint8_t {
    // docs/protocol.md §10.6. Not an error state: it is what every fresh install looks like and
    // what every user sees when the machine hosting their server is off. The obligations it
    // carries are real ones — no microphone, words in the tray, no pill, never silent.
    NoServer,    // Client only, WebSocket binding: no URL is configured
    Connecting,  // Client only, WebSocket binding: resolving, connecting, upgrading (§2.6.2)
    Spawning,    // Client only, pipe binding: pipe created, child launched, awaiting connect
    Validating,  // Client only, pipe binding: connected, checking the peer PID
    Handshake,   // hello sent/awaited; nothing else may cross
    Loading,     // hello_ack exchanged; model loading
    Idle,        // ready exchanged; no session open
    Running,     // session open; audio flowing
    Finalizing,  // stop sent/received; awaiting stopped
    Cancelling,  // cancel sent/received; awaiting stopped; all text discarded
    Dead,        // terminal. Recovery is a new child and a new pipe.
};

// Uppercase spellings ("RUNNING"), matching docs/protocol.md §10.1 and the fixtures.
const char* to_string(ConnState s) noexcept;
bool conn_state_from_string(std::string_view s, ConnState& out) noexcept;

// ─── §7.4 Sequence rules ────────────────────────────────────────────────────────────────────

enum class SeqVerdict : uint8_t {
    Accept,           // seq == last + 1
    AcceptAfterGap,   // seq > last + 1: accept, log, and RESET ANCHOR CONFIDENCE
    DropDuplicate,    // seq == last
    DropOutOfOrder,   // seq < last
};

// Pure implementation of the §7.4 table. A gap is not fatal: on a reliable transport it cannot
// arise in transit, so it means the sender skipped or dropped its own output — but the message
// in hand is still the newest hypothesis, and discarding it would be worse. The consequence is
// the anchor-confidence reset, because commit differencing (plan.md §1.5) infers the caret from
// a change produced by text we believe we injected, and a missed hypothesis makes that belief
// unsound.
SeqVerdict classify_seq(uint64_t last_seq, uint64_t seq) noexcept;

}  // namespace kd::proto
