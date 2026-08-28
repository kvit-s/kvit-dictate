// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Wire protocol codec: framing, message structs, encode/decode. Pure — no Windows, no I/O.
//
// Normative source: docs/protocol.md (proto 1). Section references below point into it. Where
// this file and that document disagree, the document wins and this file is the bug.
//
// ─── Two decisions taken here that a reader will want justified ──────────────────────────────
//
// 1. **`decode_control` does NOT enforce §8.2's range or grapheme-boundary rules on
//    `stable_bytes`.** It is tempting — the value is right there in the message — but §8.3 is
//    explicit that an out-of-range or off-boundary `stable_bytes` gets the *stability_violation*
//    handling: `fatal:false`, abort the session, keep the connection. `bad_message` is
//    `fatal:true` and tears the connection down. Rejecting it at decode time would therefore
//    kill a connection the spec says must survive, so the check lives in check_stability()
//    instead — which is also why StabilityCheck has OutOfRange and NotGraphemeBoundary members
//    rather than those being decode failures. The session layer calls it and raises the
//    non-fatal error. decode_control still rejects a *negative* stable_bytes, because that is a
//    JSON-schema fault (the field is an unsigned integer) and not a stability judgement.
//
// 2. **`final.stable_bytes`, when present, must equal `len(utf8(text))`, and that IS a
//    `bad_message` here.** §6.2 states it as a field constraint on a redundant field, not as a
//    §8.2 stability rule, and a `final` whose redundant copy disagrees with its own text is a
//    plainly malformed message rather than an inadmissible revision. Flagged in the return
//    report as the one place the layering was a judgement call.
//
// ─── Things that are easy to get wrong and are pinned by fixtures ───────────────────────────
//
//   * `length = 1 + sizeof(payload)` — the length field COVERS the type byte (§3.1). Total
//     on-wire size is `4 + length`, at most 65541.
//   * `length` is validated against [1, 65537] from the 4-byte prefix ALONE, before any payload
//     is read, buffered or allocated (§3.3). Fixture 007 (`0xFFFFFFFF`) fails a reader that
//     sign-extends or that allocates first.
//   * Frames decoded before an error are still delivered (fixture 006). That is why faults
//     surface through next()'s return value and never through push().
//   * There is no resynchronisation, ever (§3.3): any byte pattern is a legal payload, so a
//     scan for a plausible header would find false ones. Once errored, the reader stays errored.

#include "protocol.h"

#include <algorithm>
#include <string>
#include <type_traits>

#include "util/utf8.h"

namespace kd::proto {
namespace {

// ─── Small formatting helpers ───────────────────────────────────────────────────────────────
//
// std::format is available under MSVC's /std:c++20, but the house rule prefers plain string
// building where there is any doubt, and these are three lines each.

std::string hex_byte(uint8_t b) {
    static const char* kDigits = "0123456789ABCDEF";
    std::string s = "0x";
    s.push_back(kDigits[(b >> 4) & 0x0F]);
    s.push_back(kDigits[b & 0x0F]);
    return s;
}

std::string u64(uint64_t v) { return std::to_string(v); }

// Populate `err` and return false, so every validation site is a one-liner.
bool fail(ProtocolError& err, ErrorCode code, std::string detail, uint64_t at_offset = 0) {
    err.code = code;
    err.fatal = default_fatal(code);
    err.msg = std::move(detail);
    err.session_id.reset();
    err.at_offset = at_offset;
    err.supported.clear();
    return false;
}

}  // namespace

// ─── §3.2 Frame types ───────────────────────────────────────────────────────────────────────

bool is_known_frame_type(uint8_t b) noexcept {
    return b == static_cast<uint8_t>(FrameType::Control) ||
           b == static_cast<uint8_t>(FrameType::Audio);
}

// ─── §11.3 Error codes ──────────────────────────────────────────────────────────────────────

namespace {

struct ErrorCodeRow {
    ErrorCode code;
    const char* text;
    bool wire;   // may legally appear on the wire
    bool fatal;  // the `fatal` value §11.3 assigns when *originating* this code
};

// One table, so a spelling and its two flags cannot drift apart. Order matches the enum for
// readability only; lookup is linear over the table and is not on any hot path.
constexpr ErrorCodeRow kErrorCodes[] = {
    {ErrorCode::proto_unsupported, "proto_unsupported", true, true},
    {ErrorCode::bad_frame, "bad_frame", true, true},
    {ErrorCode::frame_too_large, "frame_too_large", true, true},
    {ErrorCode::truncated_frame, "truncated_frame", true, true},
    {ErrorCode::unknown_frame_type, "unknown_frame_type", true, true},
    {ErrorCode::bad_utf8, "bad_utf8", true, true},
    {ErrorCode::bad_json, "bad_json", true, true},
    {ErrorCode::bad_message, "bad_message", true, true},
    {ErrorCode::unknown_message, "unknown_message", true, true},
    {ErrorCode::state_violation, "state_violation", true, true},
    {ErrorCode::overload_audio, "overload_audio", true, true},
    {ErrorCode::overload_control, "overload_control", true, true},
    {ErrorCode::model_load_failed, "model_load_failed", true, true},
    {ErrorCode::internal_error, "internal_error", true, true},
    {ErrorCode::server_busy, "server_busy", true, true},
    {ErrorCode::model_unavailable, "model_unavailable", true, true},
    // The three non-fatal codes. §11.1 REQUIRES session_id on each of them.
    {ErrorCode::stability_violation, "stability_violation", true, false},
    {ErrorCode::decode_failed, "decode_failed", true, false},
    {ErrorCode::audio_underrun, "audio_underrun", true, false},
    // Locally-detected only: there is no peer to tell, because the peer is what failed. Same
    // spellings so a log reader learns one vocabulary (§11.3, final paragraph).
    {ErrorCode::spawn_timeout, "spawn_timeout", false, true},
    {ErrorCode::handshake_timeout, "handshake_timeout", false, true},
    {ErrorCode::model_load_timeout, "model_load_timeout", false, true},
    {ErrorCode::peer_mismatch, "peer_mismatch", false, true},
    {ErrorCode::pipe_squatted, "pipe_squatted", false, true},
    {ErrorCode::connect_timeout, "connect_timeout", false, true},
    {ErrorCode::connect_refused, "connect_refused", false, true},
    {ErrorCode::tls_failed, "tls_failed", false, true},
    // Fatal in the sense that no session can start, and *not* a fault. The tray says so in
    // words and the settings page opens; nothing is logged at error level for it.
    {ErrorCode::no_server_configured, "no_server_configured", false, true},
    // §13's single point of forward tolerance. `wire` is true because a value carrying this
    // enumerator *did* arrive on the wire — it is just not a spelling this build knows, which is
    // why ErrorMsg::code_raw keeps the original. `fatal` defaults to true only for the case of
    // originating one locally, which nothing should ever do.
    {ErrorCode::unrecognised, "unrecognised", true, true},
};

const ErrorCodeRow& error_row(ErrorCode c) noexcept {
    for (const ErrorCodeRow& r : kErrorCodes) {
        if (r.code == c) return r;
    }
    // Unreachable for any enumerator; a static fallback rather than an index into the table so
    // that reordering the table can never silently change what an out-of-range value maps to.
    static constexpr ErrorCodeRow kFallback{ErrorCode::internal_error, "internal_error", true,
                                            true};
    return kFallback;
}

}  // namespace

const char* to_string(ErrorCode c) noexcept { return error_row(c).text; }

ErrorCode error_code_from_string(std::string_view s) noexcept {
    for (const ErrorCodeRow& r : kErrorCodes) {
        if (r.code == ErrorCode::unrecognised) continue;  // never matched by its own spelling
        if (s == r.text) return r.code;
    }
    return ErrorCode::unrecognised;
}

bool is_wire_error_code(ErrorCode c) noexcept { return error_row(c).wire; }

bool default_fatal(ErrorCode c) noexcept { return error_row(c).fatal; }

std::string ProtocolError::to_string() const {
    // Qualified: an unqualified to_string(code) inside a member of this name would resolve to
    // the member itself and fail to compile.
    std::string s = kd::proto::to_string(code);
    s += fatal ? " (fatal)" : " (session)";
    if (session_id.has_value()) {
        s += " session=";
        s += *session_id;
    }
    s += " at_offset=";
    s += u64(at_offset);
    if (!supported.empty()) {
        s += " supported=[";
        for (size_t i = 0; i < supported.size(); ++i) {
            if (i != 0) s += ",";
            s += std::to_string(supported[i]);
        }
        s += "]";
    }
    if (!msg.empty()) {
        s += ": ";
        s += msg;
    }
    return s;
}

// ─── §3 Framing: the incremental decoder ────────────────────────────────────────────────────

struct FrameReader::Impl {
    // One growable buffer with a read head, rather than an erase-on-consume deque. The head is
    // only collapsed in push() and reset(), which is precisely the contract Frame::payload
    // documents: a payload view stays valid until the next push()/next()/reset().
    std::vector<uint8_t> buf;
    size_t head = 0;
    uint64_t consumed = 0;  // bytes belonging to fully-delivered frames — the at_offset base
    bool eof = false;
    bool errored = false;
    ProtocolError err;

    [[nodiscard]] size_t avail() const noexcept { return buf.size() - head; }

    void collapse() {
        if (head == 0) return;
        buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(head));
        head = 0;
    }
};

FrameReader::FrameReader() : impl_(new Impl()) {}

FrameReader::~FrameReader() { delete impl_; }

void FrameReader::push(const void* data, size_t size) {
    // Collapsing here — not in next() — is what makes the payload-view lifetime rule true as
    // written. push() is documented to invalidate outstanding views; next() is too, but only on
    // its *next* call, which is why the loop shape in the header works.
    impl_->collapse();
    if (size == 0 || data == nullptr) return;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    impl_->buf.insert(impl_->buf.end(), p, p + size);
}

void FrameReader::notify_eof() noexcept { impl_->eof = true; }

FrameStatus FrameReader::next(Frame& out) noexcept {
    Impl& s = *impl_;
    if (s.errored) return FrameStatus::Error;

    const size_t have = s.avail();

    if (have < kFrameHeaderBytes) {
        if (!s.eof) return FrameStatus::NeedMore;
        if (have == 0) return FrameStatus::EndOfStream;  // orderly shutdown (fixture 010)
        s.errored = true;
        fail(s.err, ErrorCode::truncated_frame,
             "end of stream with " + u64(have) + " buffered bytes, fewer than the 4-byte length "
             "prefix",
             s.consumed);
        return FrameStatus::Error;
    }

    // Unsigned, little-endian, assembled byte by byte. Not a memcpy into a uint32_t and not a
    // cast through a pointer: the buffer has no alignment guarantee, and a reader that
    // sign-extends fails fixture 007.
    const uint8_t* h = s.buf.data() + s.head;
    const uint32_t length = static_cast<uint32_t>(h[0]) | (static_cast<uint32_t>(h[1]) << 8) |
                            (static_cast<uint32_t>(h[2]) << 16) |
                            (static_cast<uint32_t>(h[3]) << 24);

    // §3.3: the range check happens here, on the prefix alone, before a single payload byte is
    // read or a single byte allocated for one.
    if (length < kMinLengthField) {
        s.errored = true;
        fail(s.err, ErrorCode::bad_frame,
             "length field " + u64(length) + " is below the minimum legal value " +
                 u64(kMinLengthField),
             s.consumed);
        return FrameStatus::Error;
    }
    if (length > kMaxLengthField) {
        s.errored = true;
        fail(s.err, ErrorCode::frame_too_large,
             "length field " + u64(length) + " exceeds MAX_PAYLOAD+1 (" + u64(kMaxLengthField) +
                 ")",
             s.consumed);
        return FrameStatus::Error;
    }

    const size_t total = kFrameHeaderBytes + static_cast<size_t>(length);
    if (have < total) {
        if (!s.eof) return FrameStatus::NeedMore;
        s.errored = true;
        fail(s.err, ErrorCode::truncated_frame,
             "end of stream with " + u64(have) + " of " + u64(total) + " frame bytes buffered",
             s.consumed);
        return FrameStatus::Error;
    }

    const uint8_t type = h[kFrameHeaderBytes];
    if (!is_known_frame_type(type)) {
        // Fatal, not skippable (§3.2). The handshake exists so both peers agree on the message
        // set; an undefined type means one of them is broken, and skipping it would leave the
        // stream looking healthy while the bug went on producing garbage.
        s.errored = true;
        fail(s.err, ErrorCode::unknown_frame_type,
             "type byte " + hex_byte(type) + " undefined in proto 1",
             s.consumed + kFrameHeaderBytes);
        return FrameStatus::Error;
    }

    out.type = static_cast<FrameType>(type);
    out.payload = std::string_view(reinterpret_cast<const char*>(h + kFrameHeaderBytes + 1),
                                   static_cast<size_t>(length) - 1);
    s.head += total;
    s.consumed += total;
    return FrameStatus::Frame;
}

const ProtocolError& FrameReader::error() const noexcept { return impl_->err; }

uint64_t FrameReader::stream_offset() const noexcept { return impl_->consumed; }

size_t FrameReader::buffered() const noexcept { return impl_->avail(); }

void FrameReader::reset() noexcept {
    Impl& s = *impl_;
    s.buf.clear();
    s.head = 0;
    s.consumed = 0;
    s.eof = false;
    s.errored = false;
    s.err = ProtocolError{};
}

// ─── Frame encoding ─────────────────────────────────────────────────────────────────────────

bool encode_frame(FrameType type, std::string_view payload, std::vector<uint8_t>& out) {
    if (payload.size() > kMaxPayload) return false;  // local bug, not a wire condition
    const uint32_t length = static_cast<uint32_t>(payload.size()) + 1;  // §3.1: covers the type
    out.reserve(out.size() + kFrameHeaderBytes + 1 + payload.size());
    out.push_back(static_cast<uint8_t>(length & 0xFF));
    out.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((length >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((length >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>(type));
    out.insert(out.end(), payload.begin(), payload.end());
    return true;
}

std::vector<uint8_t> encode_frame(FrameType type, std::string_view payload) {
    std::vector<uint8_t> out;
    if (!encode_frame(type, payload, out)) out.clear();
    return out;
}

bool encode_audio_frame(const int16_t* samples, size_t count, std::vector<uint8_t>& out) {
    if (count > kMaxPayload / 2) return false;
    if (count != 0 && samples == nullptr) return false;

    const uint32_t length = static_cast<uint32_t>(count * 2) + 1;
    out.reserve(out.size() + kFrameHeaderBytes + 1 + count * 2);
    out.push_back(static_cast<uint8_t>(length & 0xFF));
    out.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((length >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((length >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>(FrameType::Audio));
    for (size_t i = 0; i < count; ++i) {
        // Written explicitly rather than memcpy'd. On x64 the two are identical, but a memcpy
        // silently becomes wrong on a big-endian target, and the wire format is little-endian by
        // specification rather than by host coincidence (§6.3).
        const uint16_t u = static_cast<uint16_t>(samples[i]);
        out.push_back(static_cast<uint8_t>(u & 0xFF));
        out.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
    }
    return true;
}

// ─── §6.3 Audio payload decoding ────────────────────────────────────────────────────────────

bool decode_audio_payload(std::string_view payload, std::vector<int16_t>& out, ProtocolError& err) {
    out.clear();
    if ((payload.size() % 2) != 0) {
        // The sender split a sample, which means its buffer arithmetic is wrong (§6.3).
        return fail(err, ErrorCode::bad_message,
                    "audio payload length " + u64(payload.size()) +
                        " is odd; s16le samples are 2 bytes");
    }
    const size_t n = payload.size() / 2;
    out.resize(n);
    for (size_t i = 0; i < n; ++i) {
        // **Copy, byte by byte.** `payload` is a view into a read buffer with no 2-byte
        // alignment guarantee, so reinterpret_cast<const int16_t*>(payload.data()) is undefined
        // behaviour — and it is the kind of UB that gets mis-optimised long before it faults.
        const uint16_t lo = static_cast<uint8_t>(payload[2 * i]);
        const uint16_t hi = static_cast<uint8_t>(payload[2 * i + 1]);
        out[i] = static_cast<int16_t>(static_cast<uint16_t>(lo | static_cast<uint16_t>(hi << 8)));
    }
    return true;
}

// ─── §6 Message catalogue: names and taxonomies ─────────────────────────────────────────────

namespace {

struct MessageTypeRow {
    MessageType type;
    const char* text;
    bool from_client;
    bool from_server;
};

constexpr MessageTypeRow kMessageTypes[] = {
    {MessageType::Hello, "hello", true, false},
    {MessageType::Start, "start", true, false},
    {MessageType::Stop, "stop", true, false},
    {MessageType::Cancel, "cancel", true, false},
    {MessageType::Discontinuity, "discontinuity", true, false},
    {MessageType::HelloAck, "hello_ack", false, true},
    {MessageType::Loading, "loading", false, true},
    {MessageType::Ready, "ready", false, true},
    {MessageType::Partial, "partial", false, true},
    {MessageType::Final, "final", false, true},
    {MessageType::Stopped, "stopped", false, true},
    {MessageType::Error, "error", true, true},  // §6.2: bidirectional
};

const MessageTypeRow& message_row(MessageType t) noexcept {
    for (const MessageTypeRow& r : kMessageTypes) {
        if (r.type == t) return r;
    }
    return kMessageTypes[0];
}

}  // namespace

const char* to_string(MessageType t) noexcept { return message_row(t).text; }

bool message_type_from_string(std::string_view s, MessageType& out) noexcept {
    for (const MessageTypeRow& r : kMessageTypes) {
        if (s == r.text) {
            out = r.type;
            return true;
        }
    }
    return false;
}

bool is_client_message(MessageType t) noexcept { return message_row(t).from_client; }
bool is_server_message(MessageType t) noexcept { return message_row(t).from_server; }

const char* to_string(StreamingMode m) noexcept {
    switch (m) {
        case StreamingMode::NativeStreaming: return "native-streaming";
        case StreamingMode::StatefulChunked: return "stateful-chunked";
        case StreamingMode::PseudoStreaming: return "pseudo-streaming";
    }
    return "native-streaming";
}

bool streaming_mode_from_string(std::string_view s, StreamingMode& out) noexcept {
    if (s == "native-streaming") {
        out = StreamingMode::NativeStreaming;
    } else if (s == "stateful-chunked") {
        out = StreamingMode::StatefulChunked;
    } else if (s == "pseudo-streaming") {
        out = StreamingMode::PseudoStreaming;
    } else {
        return false;
    }
    return true;
}

const char* to_string(StopReason r) noexcept {
    switch (r) {
        case StopReason::Stop: return "stop";
        case StopReason::Cancel: return "cancel";
        case StopReason::Error: return "error";
        case StopReason::UnknownSession: return "unknown_session";
        case StopReason::Other: return "other";
    }
    return "other";
}

StopReason stop_reason_from_string(std::string_view s) noexcept {
    if (s == "stop") return StopReason::Stop;
    if (s == "cancel") return StopReason::Cancel;
    if (s == "error") return StopReason::Error;
    if (s == "unknown_session") return StopReason::UnknownSession;
    return StopReason::Other;  // §6.2 calls the field diagnostics; an unknown value is not fatal
}

// ─── Message: type discrimination and the accessors §7.1 is written against ─────────────────

namespace {

template <typename T>
struct MessageTypeOf;

template <>
struct MessageTypeOf<HelloMsg> {
    static constexpr MessageType value = MessageType::Hello;
};
template <>
struct MessageTypeOf<StartMsg> {
    static constexpr MessageType value = MessageType::Start;
};
template <>
struct MessageTypeOf<StopMsg> {
    static constexpr MessageType value = MessageType::Stop;
};
template <>
struct MessageTypeOf<CancelMsg> {
    static constexpr MessageType value = MessageType::Cancel;
};
template <>
struct MessageTypeOf<DiscontinuityMsg> {
    static constexpr MessageType value = MessageType::Discontinuity;
};
template <>
struct MessageTypeOf<HelloAckMsg> {
    static constexpr MessageType value = MessageType::HelloAck;
};
template <>
struct MessageTypeOf<LoadingMsg> {
    static constexpr MessageType value = MessageType::Loading;
};
template <>
struct MessageTypeOf<ReadyMsg> {
    static constexpr MessageType value = MessageType::Ready;
};
template <>
struct MessageTypeOf<PartialMsg> {
    static constexpr MessageType value = MessageType::Partial;
};
template <>
struct MessageTypeOf<FinalMsg> {
    static constexpr MessageType value = MessageType::Final;
};
template <>
struct MessageTypeOf<StoppedMsg> {
    static constexpr MessageType value = MessageType::Stopped;
};
template <>
struct MessageTypeOf<ErrorMsg> {
    static constexpr MessageType value = MessageType::Error;
};

}  // namespace

MessageType Message::type() const noexcept {
    // Visited rather than derived from body_.index(): the variant's alternative order happens to
    // match the enum today, and a silent reordering of either would be a very quiet bug.
    return std::visit(
        [](const auto& m) noexcept {
            return MessageTypeOf<std::decay_t<decltype(m)>>::value;
        },
        body_);
}

const std::string* Message::session_id() const noexcept {
    return std::visit(
        [](const auto& m) noexcept -> const std::string* {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, StartMsg> || std::is_same_v<T, StopMsg> ||
                          std::is_same_v<T, CancelMsg> || std::is_same_v<T, DiscontinuityMsg> ||
                          std::is_same_v<T, PartialMsg> || std::is_same_v<T, FinalMsg> ||
                          std::is_same_v<T, StoppedMsg>) {
                return &m.session_id;
            } else if constexpr (std::is_same_v<T, ErrorMsg>) {
                // Optional: §11.1 requires it only when !fatal, and a fatal error may precede
                // any session.
                return m.session_id.has_value() ? &*m.session_id : nullptr;
            } else {
                return nullptr;  // hello, hello_ack, loading, ready
            }
        },
        body_);
}

bool Message::carries_text() const noexcept {
    const MessageType t = type();
    return t == MessageType::Partial || t == MessageType::Final;
}

bool Message::consumes_seq() const noexcept {
    // §7.4: loading, ready, hello_ack, stopped and error do NOT consume seq numbers.
    const MessageType t = type();
    return t == MessageType::Partial || t == MessageType::Final;
}

// ─── §4 Control-frame decoding ──────────────────────────────────────────────────────────────

namespace {

// Offset of the first byte that begins a malformed UTF-8 sequence.
//
// utf8::validate() reports "the first byte that could not begin or continue a valid sequence",
// which for a truncated sequence is the *continuation* position, not the lead. `bad_utf8` is far
// more useful pointing at the lead byte (fixture 008 expects exactly that), so the payload is
// re-walked here rather than depending on the other module's exact convention.
size_t malformed_sequence_offset(std::string_view s) noexcept {
    size_t i = 0;
    while (i < s.size()) {
        char32_t cp = 0;
        size_t len = 0;
        if (!utf8::decode(s, i, cp, len) || len == 0) return i;
        i += len;
    }
    return s.size();
}

// ── Field accessors ─────────────────────────────────────────────────────────────────────────
//
// An explicit JSON `null` is rejected everywhere, for required and optional fields alike:
// util/json.h and docs/protocol.md §4.2 agree that the protocol never defines a null, and
// treating `{"stage":null}` as "absent" is how two implementations quietly diverge.

bool reject_null(const json::Value& v, const char* key, ProtocolError& err) {
    if (v.is_null()) {
        return fail(err, ErrorCode::bad_message,
                    std::string("field '") + key + "' is null; the protocol defines no null");
    }
    return true;
}

bool get_req_string(const json::Value& o, const char* key, std::string& out, ProtocolError& err) {
    const json::Value* v = o.find(key);
    if (v == nullptr) {
        return fail(err, ErrorCode::bad_message, std::string("missing required field '") + key + "'");
    }
    if (!reject_null(*v, key, err)) return false;
    std::string_view sv;
    if (!v->get_string(sv)) {
        return fail(err, ErrorCode::bad_message, std::string("field '") + key + "' is not a string");
    }
    out.assign(sv);
    return true;
}

bool get_opt_string(const json::Value& o, const char* key, std::optional<std::string>& out,
                    ProtocolError& err) {
    const json::Value* v = o.find(key);
    if (v == nullptr) return true;
    if (!reject_null(*v, key, err)) return false;
    std::string_view sv;
    if (!v->get_string(sv)) {
        return fail(err, ErrorCode::bad_message, std::string("field '") + key + "' is not a string");
    }
    out = std::string(sv);
    return true;
}

bool get_req_int(const json::Value& o, const char* key, int64_t& out, ProtocolError& err) {
    const json::Value* v = o.find(key);
    if (v == nullptr) {
        return fail(err, ErrorCode::bad_message, std::string("missing required field '") + key + "'");
    }
    if (!reject_null(*v, key, err)) return false;
    // get_int() succeeds only for a literal written without a fraction or exponent, so `12.0`
    // and `1e2` are rejected here. §4.2 permits exactly that: "A receiver MAY reject a
    // non-integer-valued number where an integer is specified".
    if (!v->get_int(out)) {
        return fail(err, ErrorCode::bad_message,
                    std::string("field '") + key + "' is not an integer");
    }
    return true;
}

bool get_opt_int(const json::Value& o, const char* key, std::optional<int64_t>& out,
                 ProtocolError& err) {
    const json::Value* v = o.find(key);
    if (v == nullptr) return true;
    if (!reject_null(*v, key, err)) return false;
    int64_t n = 0;
    if (!v->get_int(n)) {
        return fail(err, ErrorCode::bad_message,
                    std::string("field '") + key + "' is not an integer");
    }
    out = n;
    return true;
}

bool get_req_bool(const json::Value& o, const char* key, bool& out, ProtocolError& err) {
    const json::Value* v = o.find(key);
    if (v == nullptr) {
        return fail(err, ErrorCode::bad_message, std::string("missing required field '") + key + "'");
    }
    if (!reject_null(*v, key, err)) return false;
    if (!v->get_bool(out)) {
        return fail(err, ErrorCode::bad_message, std::string("field '") + key + "' is not a boolean");
    }
    return true;
}

bool get_opt_bool(const json::Value& o, const char* key, bool& out, ProtocolError& err) {
    const json::Value* v = o.find(key);
    if (v == nullptr) return true;
    if (!reject_null(*v, key, err)) return false;
    if (!v->get_bool(out)) {
        return fail(err, ErrorCode::bad_message, std::string("field '") + key + "' is not a boolean");
    }
    return true;
}

bool require_range(int64_t v, int64_t lo, int64_t hi, const char* key, ProtocolError& err) {
    if (v < lo || v > hi) {
        return fail(err, ErrorCode::bad_message,
                    std::string("field '") + key + "' value " + std::to_string(v) +
                        " is outside [" + std::to_string(lo) + ", " + std::to_string(hi) + "]");
    }
    return true;
}

bool require_session_id(const std::string& s, ProtocolError& err) {
    if (!is_valid_session_id(s)) {
        return fail(err, ErrorCode::bad_message,
                    "session_id is not 1-64 bytes of [A-Za-z0-9_-] (docs/protocol.md 7.1)");
    }
    return true;
}

// ── Per-message decoders ────────────────────────────────────────────────────────────────────

bool decode_hello(const json::Value& o, Message& out, ProtocolError& err) {
    HelloMsg m;
    int64_t proto = 0;
    if (!get_req_int(o, "proto", proto, err)) return false;
    if (!require_range(proto, 1, 65535, "proto", err)) return false;
    m.proto = static_cast<int32_t>(proto);
    if (!get_opt_string(o, "client", m.client, err)) return false;
    if (!get_opt_string(o, "model", m.model, err)) return false;
    out = Message::of(std::move(m));
    return true;
}

bool decode_start(const json::Value& o, Message& out, ProtocolError& err) {
    StartMsg m;
    if (!get_req_string(o, "session_id", m.session_id, err)) return false;
    if (!require_session_id(m.session_id, err)) return false;

    int64_t rate = 0;
    if (!get_req_int(o, "sample_rate", rate, err)) return false;
    if (rate != static_cast<int64_t>(kSampleRate)) {
        // Present despite being fixed so a mismatch is caught at the start of a session rather
        // than inferred later from bad transcripts (§6.1).
        return fail(err, ErrorCode::bad_message,
                    "sample_rate " + std::to_string(rate) + " must be 16000 in proto 1");
    }
    m.sample_rate = static_cast<int32_t>(rate);

    std::optional<int64_t> channels;
    if (!get_opt_int(o, "channels", channels, err)) return false;
    if (channels.has_value() && *channels != 1) {
        return fail(err, ErrorCode::bad_message,
                    "channels " + std::to_string(*channels) + " must be 1 in proto 1");
    }
    m.channels = 1;

    std::optional<std::string> format;
    if (!get_opt_string(o, "format", format, err)) return false;
    if (format.has_value() && *format != "s16le") {
        return fail(err, ErrorCode::bad_message,
                    "format '" + *format + "' must be 's16le' in proto 1");
    }
    m.format = "s16le";

    if (const json::Value* ctx = o.find("context"); ctx != nullptr) {
        if (!reject_null(*ctx, "context", err)) return false;
        if (!ctx->is_array()) {
            return fail(err, ErrorCode::bad_message, "field 'context' is not an array");
        }
        if (ctx->size() > kMaxContextItems) {
            return fail(err, ErrorCode::bad_message,
                        "context has " + u64(ctx->size()) + " items; at most " +
                            u64(kMaxContextItems) + " are permitted");
        }
        m.context.reserve(ctx->size());
        for (size_t i = 0; i < ctx->size(); ++i) {
            const json::Value* item = ctx->at(i);
            std::string_view sv;
            if (item == nullptr || !item->get_string(sv)) {
                return fail(err, ErrorCode::bad_message,
                            "context[" + u64(i) + "] is not a string");
            }
            if (sv.size() > kMaxContextItemBytes) {
                return fail(err, ErrorCode::bad_message,
                            "context[" + u64(i) + "] is " + u64(sv.size()) +
                                " bytes; at most " + u64(kMaxContextItemBytes) + " are permitted");
            }
            m.context.emplace_back(sv);
        }
        // Distinguishes "sent an empty array" from "did not send the field": plan.md §4 compares
        // context-enabled and context-free runs separately, so the two cannot be merged.
        m.context_present = true;
    }

    out = Message::of(std::move(m));
    return true;
}

bool decode_stop(const json::Value& o, Message& out, ProtocolError& err) {
    StopMsg m;
    if (!get_req_string(o, "session_id", m.session_id, err)) return false;
    if (!require_session_id(m.session_id, err)) return false;
    out = Message::of(std::move(m));
    return true;
}

bool decode_cancel(const json::Value& o, Message& out, ProtocolError& err) {
    CancelMsg m;
    if (!get_req_string(o, "session_id", m.session_id, err)) return false;
    if (!require_session_id(m.session_id, err)) return false;
    out = Message::of(std::move(m));
    return true;
}

bool decode_discontinuity(const json::Value& o, Message& out, ProtocolError& err) {
    DiscontinuityMsg m;
    if (!get_req_string(o, "session_id", m.session_id, err)) return false;
    if (!require_session_id(m.session_id, err)) return false;
    int64_t gap = 0;
    if (!get_req_int(o, "gap_ms", gap, err)) return false;
    if (gap < 0) {
        return fail(err, ErrorCode::bad_message,
                    "gap_ms " + std::to_string(gap) + " is negative");
    }
    m.gap_ms = gap;
    if (!get_opt_string(o, "reason", m.reason, err)) return false;
    out = Message::of(std::move(m));
    return true;
}

bool decode_hello_ack(const json::Value& o, Message& out, ProtocolError& err) {
    HelloAckMsg m;
    int64_t proto = 0;
    if (!get_req_int(o, "proto", proto, err)) return false;
    if (!require_range(proto, 1, 65535, "proto", err)) return false;
    m.proto = static_cast<int32_t>(proto);
    if (!get_opt_string(o, "server", m.server, err)) return false;

    // §5.1: the catalogue is advisory. A malformed *element* is skipped rather than failing the
    // handshake — the spec says so explicitly, and it is the right trade: losing a working
    // connection over a mis-typed display name would be a worse outcome than an incomplete
    // model list. A `models` that is not an array at all is still bad_message, because that is
    // a server that has misunderstood the field rather than one with a bad row.
    if (const json::Value* arr = o.find("models"); arr != nullptr && !arr->is_null()) {
        if (!arr->is_array()) {
            return fail(err, ErrorCode::bad_message, "field 'models' is not an array");
        }
        m.models.reserve(arr->size());
        for (size_t i = 0; i < arr->size(); ++i) {
            const json::Value* item = arr->at(i);
            if (item == nullptr || !item->is_object()) continue;
            ModelInfo info;
            const json::Value* id = item->find("id");
            std::string_view id_sv;
            if (id == nullptr || !id->get_string(id_sv) || id_sv.empty()) continue;
            info.id.assign(id_sv);
            std::string_view sv;
            if (const json::Value* v = item->find("display_name");
                v != nullptr && v->get_string(sv)) {
                info.display_name = std::string(sv);
            }
            if (const json::Value* v = item->find("mode"); v != nullptr && v->get_string(sv)) {
                StreamingMode sm{};
                if (streaming_mode_from_string(sv, sm)) info.mode = sm;
            }
            if (const json::Value* v = item->find("chunk_ms"); v != nullptr) {
                int64_t n = 0;
                if (v->get_int(n) && n >= 0 && n <= 3600000) info.chunk_ms = static_cast<int32_t>(n);
            }
            if (const json::Value* v = item->find("context_supported"); v != nullptr) {
                bool b = false;
                if (v->get_bool(b)) info.context_supported = b;
            }
            m.models.push_back(std::move(info));
        }
    }

    // NOTE: "proto must equal the offered version" is deliberately NOT checked here. This
    // function does not know what was offered; §5 makes that the Client's check, and its failure
    // is proto_unsupported (a session-layer decision), not bad_message.
    out = Message::of(std::move(m));
    return true;
}

bool decode_loading(const json::Value& o, Message& out, ProtocolError& err) {
    LoadingMsg m;
    if (!get_req_string(o, "model", m.model, err)) return false;
    if (!get_opt_string(o, "stage", m.stage, err)) return false;
    std::optional<int64_t> progress;
    if (!get_opt_int(o, "progress", progress, err)) return false;
    if (progress.has_value()) {
        if (!require_range(*progress, 0, 100, "progress", err)) return false;
        m.progress = static_cast<int32_t>(*progress);
    }
    out = Message::of(std::move(m));
    return true;
}

bool decode_ready(const json::Value& o, Message& out, ProtocolError& err) {
    ReadyMsg m;
    if (!get_req_string(o, "model", m.model, err)) return false;
    int64_t warm = 0;
    if (!get_req_int(o, "warm_ms", warm, err)) return false;
    if (warm < 0) {
        return fail(err, ErrorCode::bad_message, "warm_ms " + std::to_string(warm) + " is negative");
    }
    m.warm_ms = warm;

    std::string mode;
    if (!get_req_string(o, "mode", mode, err)) return false;
    if (!streaming_mode_from_string(mode, m.mode)) {
        // Exactly the Phase 0A taxonomy (§6.2); anything else makes the latency numbers
        // uninterpretable, which is why the spec rejects rather than passes it through.
        return fail(err, ErrorCode::bad_message,
                    "mode '" + mode +
                        "' is not one of native-streaming / stateful-chunked / pseudo-streaming");
    }

    std::optional<int64_t> chunk;
    if (!get_opt_int(o, "chunk_ms", chunk, err)) return false;
    if (chunk.has_value()) {
        if (!require_range(*chunk, 0, 2147483647, "chunk_ms", err)) return false;
        m.chunk_ms = static_cast<int32_t>(*chunk);
    }
    if (!get_opt_bool(o, "context_supported", m.context_supported, err)) return false;
    out = Message::of(std::move(m));
    return true;
}

bool decode_partial(const json::Value& o, Message& out, ProtocolError& err) {
    PartialMsg m;
    if (!get_req_string(o, "session_id", m.session_id, err)) return false;
    if (!require_session_id(m.session_id, err)) return false;

    int64_t utt = 0;
    if (!get_req_int(o, "utterance_id", utt, err)) return false;
    if (utt < 1) {
        return fail(err, ErrorCode::bad_message,
                    "utterance_id " + std::to_string(utt) + " must be >= 1");
    }
    m.utterance_id = static_cast<uint64_t>(utt);

    int64_t seq = 0;
    if (!get_req_int(o, "seq", seq, err)) return false;
    if (seq < 1) {
        return fail(err, ErrorCode::bad_message, "seq " + std::to_string(seq) + " must be >= 1");
    }
    m.seq = static_cast<uint64_t>(seq);

    if (!get_req_string(o, "text", m.text, err)) return false;

    int64_t stable = 0;
    if (!get_req_int(o, "stable_bytes", stable, err)) return false;
    if (stable < 0) {
        return fail(err, ErrorCode::bad_message,
                    "stable_bytes " + std::to_string(stable) + " is negative");
    }
    // Range and grapheme-boundary validation live in check_stability(), not here: §8.3 gives
    // those a NON-fatal stability_violation, and bad_message is fatal. See the header comment.
    m.stable_bytes = static_cast<uint64_t>(stable);

    std::optional<int64_t> pos;
    if (!get_opt_int(o, "audio_pos_ms", pos, err)) return false;
    if (pos.has_value()) {
        if (*pos < 0) {
            return fail(err, ErrorCode::bad_message,
                        "audio_pos_ms " + std::to_string(*pos) + " is negative");
        }
        m.audio_pos_ms = *pos;
    }

    out = Message::of(std::move(m));
    return true;
}

bool decode_final(const json::Value& o, Message& out, ProtocolError& err) {
    FinalMsg m;
    if (!get_req_string(o, "session_id", m.session_id, err)) return false;
    if (!require_session_id(m.session_id, err)) return false;

    int64_t utt = 0;
    if (!get_req_int(o, "utterance_id", utt, err)) return false;
    if (utt < 1) {
        return fail(err, ErrorCode::bad_message,
                    "utterance_id " + std::to_string(utt) + " must be >= 1");
    }
    m.utterance_id = static_cast<uint64_t>(utt);

    int64_t seq = 0;
    if (!get_req_int(o, "seq", seq, err)) return false;
    if (seq < 1) {
        return fail(err, ErrorCode::bad_message, "seq " + std::to_string(seq) + " must be >= 1");
    }
    m.seq = static_cast<uint64_t>(seq);

    if (!get_req_string(o, "text", m.text, err)) return false;

    int64_t pos = 0;
    if (!get_req_int(o, "audio_pos_ms", pos, err)) return false;
    if (pos < 0) {
        return fail(err, ErrorCode::bad_message,
                    "audio_pos_ms " + std::to_string(pos) + " is negative");
    }
    m.audio_pos_ms = pos;

    std::optional<int64_t> stable;
    if (!get_opt_int(o, "stable_bytes", stable, err)) return false;
    if (stable.has_value()) {
        // §6.2 states this as a field constraint on a redundant field: "If present MUST equal
        // len(utf8(text))". A `final` whose own redundant copy contradicts its own text is
        // malformed, not an inadmissible revision, so this is bad_message rather than
        // stability_violation.
        if (*stable < 0 || static_cast<uint64_t>(*stable) != static_cast<uint64_t>(m.text.size())) {
            return fail(err, ErrorCode::bad_message,
                        "final.stable_bytes " + std::to_string(*stable) +
                            " must equal len(utf8(text)) = " + u64(m.text.size()));
        }
        m.stable_bytes = static_cast<uint64_t>(*stable);
    }

    out = Message::of(std::move(m));
    return true;
}

bool decode_stopped(const json::Value& o, Message& out, ProtocolError& err) {
    StoppedMsg m;
    if (!get_req_string(o, "session_id", m.session_id, err)) return false;
    if (!require_session_id(m.session_id, err)) return false;
    if (!get_opt_string(o, "reason", m.reason, err)) return false;
    out = Message::of(std::move(m));
    return true;
}

bool decode_error(const json::Value& o, Message& out, ProtocolError& err) {
    ErrorMsg m;
    if (!get_req_string(o, "code", m.code_raw, err)) return false;
    m.code = error_code_from_string(m.code_raw);  // never fails; §13's forward tolerance
    if (!get_req_bool(o, "fatal", m.fatal, err)) return false;
    if (!get_req_string(o, "msg", m.msg, err)) return false;

    if (!get_opt_string(o, "session_id", m.session_id, err)) return false;
    if (!m.fatal && !m.session_id.has_value()) {
        // §11.1: fatal:false means "the named session is aborted" — with no name there is
        // nothing to abort, and a receiver that guessed would abort the wrong one.
        return fail(err, ErrorCode::bad_message,
                    "error with fatal:false must carry session_id (docs/protocol.md 11.1)");
    }
    if (m.session_id.has_value() && !require_session_id(*m.session_id, err)) return false;

    if (const json::Value* sup = o.find("supported"); sup != nullptr) {
        if (!reject_null(*sup, "supported", err)) return false;
        if (!sup->is_array()) {
            return fail(err, ErrorCode::bad_message, "field 'supported' is not an array");
        }
        m.supported.reserve(sup->size());
        for (size_t i = 0; i < sup->size(); ++i) {
            const json::Value* item = sup->at(i);
            int64_t n = 0;
            if (item == nullptr || !item->get_int(n)) {
                return fail(err, ErrorCode::bad_message,
                            "supported[" + u64(i) + "] is not an integer");
            }
            if (!require_range(n, 1, 65535, "supported[]", err)) return false;
            m.supported.push_back(static_cast<int32_t>(n));
        }
    }

    // §2.7: a hint that accompanies `server_busy`. Accepted on any error rather than only that
    // one, because §13's forward tolerance means a future code may carry it too, and rejecting
    // a field for appearing on the wrong message would be the kind of strictness that buys
    // nothing and breaks a working connection.
    std::optional<int64_t> retry;
    if (!get_opt_int(o, "retry_after_ms", retry, err)) return false;
    if (retry.has_value()) {
        // Clamped rather than rejected: a server that says "wait a week" is misconfigured, not
        // malicious, and the client's own backoff is the real bound anyway.
        const int64_t v = *retry < 0 ? 0 : (*retry > 3600000 ? 3600000 : *retry);
        m.retry_after_ms = static_cast<int32_t>(v);
    }

    // §5.1: what the server does have, when it has refused what was asked for. Elements that
    // are not strings are skipped, on the same reasoning as `hello_ack.models[]` — the list is
    // there to be shown to a person, and a bad row is not worth failing a diagnostic over.
    if (const json::Value* av = o.find("available"); av != nullptr && !av->is_null()) {
        if (!av->is_array()) {
            return fail(err, ErrorCode::bad_message, "field 'available' is not an array");
        }
        m.available.reserve(av->size());
        for (size_t i = 0; i < av->size(); ++i) {
            const json::Value* item = av->at(i);
            std::string_view sv;
            if (item != nullptr && item->get_string(sv) && !sv.empty()) {
                m.available.emplace_back(sv);
            }
        }
    }

    out = Message::of(std::move(m));
    return true;
}

}  // namespace

bool decode_control(std::string_view payload, Message& out, ProtocolError& err) {
    // Layer order matters (§1): fail at the lowest layer that can see the fault, and never let a
    // higher layer paper over a lower one.

    // ── L2a: UTF-8 ──────────────────────────────────────────────────────────────────────────
    if (!utf8::validate(payload)) {
        // No U+FFFD substitution, ever (§4.1). This text is bound for a keystroke injector
        // aimed at the user's document; a replacement character is a silent corruption they
        // discover weeks later by reading their own notes.
        const size_t off = malformed_sequence_offset(payload);
        return fail(err, ErrorCode::bad_utf8,
                    "control payload is not well-formed UTF-8 at byte " + u64(off),
                    static_cast<uint64_t>(off));
    }

    // ── L2b: JSON ───────────────────────────────────────────────────────────────────────────
    json::Options opts;
    opts.max_depth = 32;
    opts.max_bytes = kMaxPayload;
    opts.reject_duplicate_keys = true;  // §4.2: never last-wins

    json::Value root;
    json::Error jerr;
    if (!json::parse_object(payload, root, jerr, opts)) {
        const ErrorCode code =
            (jerr.code == json::ErrorCode::InvalidUtf8) ? ErrorCode::bad_utf8 : ErrorCode::bad_json;
        return fail(err, code,
                    std::string("control payload is not a conforming JSON object: ") +
                        json::to_string(jerr.code) +
                        (jerr.detail.empty() ? std::string() : (" (" + jerr.detail + ")")),
                    static_cast<uint64_t>(jerr.offset));
    }

    // ── L3: schema ──────────────────────────────────────────────────────────────────────────
    const json::Value* tv = root.find("t");
    std::string_view t;
    if (tv == nullptr || !tv->get_string(t)) {
        // A message with no `t` selects no schema at all. That is a malformed message, not an
        // unknown one — `unknown_message` is reserved for a `t` we could read and did not know.
        return fail(err, ErrorCode::bad_message,
                    "control message has no string 't' field to select a schema");
    }

    MessageType type{};
    if (!message_type_from_string(t, type)) {
        // §4.2's asymmetry: an unknown field is ignored, an unknown `t` is fatal. An extra field
        // cannot change the meaning of a message we already understand; an unknown message type
        // means the peer believes something is happening that we know nothing about.
        return fail(err, ErrorCode::unknown_message,
                    "message type '" + std::string(t) + "' is not defined in proto 1");
    }

    // Unknown *fields* are ignored here by construction: every decoder below reads the fields it
    // knows and never enumerates the object, so an extra key simply goes unread.
    switch (type) {
        case MessageType::Hello: return decode_hello(root, out, err);
        case MessageType::Start: return decode_start(root, out, err);
        case MessageType::Stop: return decode_stop(root, out, err);
        case MessageType::Cancel: return decode_cancel(root, out, err);
        case MessageType::Discontinuity: return decode_discontinuity(root, out, err);
        case MessageType::HelloAck: return decode_hello_ack(root, out, err);
        case MessageType::Loading: return decode_loading(root, out, err);
        case MessageType::Ready: return decode_ready(root, out, err);
        case MessageType::Partial: return decode_partial(root, out, err);
        case MessageType::Final: return decode_final(root, out, err);
        case MessageType::Stopped: return decode_stopped(root, out, err);
        case MessageType::Error: return decode_error(root, out, err);
    }
    return fail(err, ErrorCode::internal_error, "unhandled message type in decode_control");
}

// ─── §4 Control-frame encoding ──────────────────────────────────────────────────────────────

namespace {

// Absent optionals are OMITTED, never emitted as null (protocol.h): the protocol defines no
// null, and a conforming receiver — including this file's decoder — rejects one.
void set_opt(json::Value& o, const char* key, const std::optional<std::string>& v) {
    if (v.has_value()) o.set(key, json::Value::string(*v));
}

void set_opt_int(json::Value& o, const char* key, const std::optional<int32_t>& v) {
    if (v.has_value()) o.set(key, json::Value::number(static_cast<int64_t>(*v)));
}

json::Value string_array(const std::vector<std::string>& items) {
    json::Value a = json::Value::array();
    for (const std::string& s : items) a.push_back(json::Value::string(s));
    return a;
}

}  // namespace

std::string encode_control(const Message& m) {
    json::Value o = json::Value::object();
    o.set("t", json::Value::string(to_string(m.type())));

    std::visit(
        [&o](const auto& b) {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, HelloMsg>) {
                o.set("proto", json::Value::number(static_cast<int64_t>(b.proto)));
                set_opt(o, "client", b.client);
                set_opt(o, "model", b.model);
            } else if constexpr (std::is_same_v<T, StartMsg>) {
                o.set("session_id", json::Value::string(b.session_id));
                o.set("sample_rate", json::Value::number(static_cast<int64_t>(b.sample_rate)));
                o.set("channels", json::Value::number(static_cast<int64_t>(b.channels)));
                o.set("format", json::Value::string(b.format));
                // Emitted only when the field was really meant. An empty array is a different
                // statement from an absent field (plan.md §4 compares the two run kinds).
                if (b.context_present) o.set("context", string_array(b.context));
            } else if constexpr (std::is_same_v<T, StopMsg> || std::is_same_v<T, CancelMsg>) {
                o.set("session_id", json::Value::string(b.session_id));
            } else if constexpr (std::is_same_v<T, DiscontinuityMsg>) {
                o.set("session_id", json::Value::string(b.session_id));
                o.set("gap_ms", json::Value::number(b.gap_ms));
                set_opt(o, "reason", b.reason);
            } else if constexpr (std::is_same_v<T, HelloAckMsg>) {
                o.set("proto", json::Value::number(static_cast<int64_t>(b.proto)));
                set_opt(o, "server", b.server);
                if (!b.models.empty()) {
                    json::Value a = json::Value::array();
                    for (const ModelInfo& mi : b.models) {
                        json::Value e = json::Value::object();
                        e.set("id", json::Value::string(mi.id));
                        set_opt(e, "display_name", mi.display_name);
                        if (mi.mode.has_value()) {
                            e.set("mode", json::Value::string(to_string(*mi.mode)));
                        }
                        set_opt_int(e, "chunk_ms", mi.chunk_ms);
                        e.set("context_supported", json::Value::boolean(mi.context_supported));
                        a.push_back(std::move(e));
                    }
                    o.set("models", std::move(a));
                }
            } else if constexpr (std::is_same_v<T, LoadingMsg>) {
                o.set("model", json::Value::string(b.model));
                set_opt(o, "stage", b.stage);
                set_opt_int(o, "progress", b.progress);
            } else if constexpr (std::is_same_v<T, ReadyMsg>) {
                o.set("model", json::Value::string(b.model));
                o.set("warm_ms", json::Value::number(b.warm_ms));
                o.set("mode", json::Value::string(to_string(b.mode)));
                set_opt_int(o, "chunk_ms", b.chunk_ms);
                o.set("context_supported", json::Value::boolean(b.context_supported));
            } else if constexpr (std::is_same_v<T, PartialMsg>) {
                o.set("session_id", json::Value::string(b.session_id));
                o.set("utterance_id", json::Value::number(static_cast<int64_t>(b.utterance_id)));
                o.set("seq", json::Value::number(static_cast<int64_t>(b.seq)));
                o.set("text", json::Value::string(b.text));
                o.set("stable_bytes", json::Value::number(static_cast<int64_t>(b.stable_bytes)));
                if (b.audio_pos_ms.has_value()) {
                    o.set("audio_pos_ms", json::Value::number(*b.audio_pos_ms));
                }
            } else if constexpr (std::is_same_v<T, FinalMsg>) {
                o.set("session_id", json::Value::string(b.session_id));
                o.set("utterance_id", json::Value::number(static_cast<int64_t>(b.utterance_id)));
                o.set("seq", json::Value::number(static_cast<int64_t>(b.seq)));
                o.set("text", json::Value::string(b.text));
                o.set("audio_pos_ms", json::Value::number(b.audio_pos_ms));
                if (b.stable_bytes.has_value()) {
                    o.set("stable_bytes",
                          json::Value::number(static_cast<int64_t>(*b.stable_bytes)));
                }
            } else if constexpr (std::is_same_v<T, StoppedMsg>) {
                o.set("session_id", json::Value::string(b.session_id));
                set_opt(o, "reason", b.reason);
            } else if constexpr (std::is_same_v<T, ErrorMsg>) {
                // code_raw wins when it is set: a code this build does not recognise must be
                // relayed and logged with the spelling that actually arrived (§13), not
                // flattened to "unrecognised".
                o.set("code", json::Value::string(b.code_raw.empty()
                                                      ? std::string(to_string(b.code))
                                                      : b.code_raw));
                o.set("fatal", json::Value::boolean(b.fatal));
                o.set("msg", json::Value::string(b.msg));
                set_opt(o, "session_id", b.session_id);
                if (!b.supported.empty()) {
                    json::Value a = json::Value::array();
                    for (int32_t v : b.supported) {
                        a.push_back(json::Value::number(static_cast<int64_t>(v)));
                    }
                    o.set("supported", std::move(a));
                }
                set_opt_int(o, "retry_after_ms", b.retry_after_ms);
                if (!b.available.empty()) {
                    json::Value a = json::Value::array();
                    for (const std::string& v : b.available) a.push_back(json::Value::string(v));
                    o.set("available", std::move(a));
                }
            }
        },
        m.body());

    return json::serialize(o);
}

std::vector<uint8_t> encode_control_frame(const Message& m) {
    return encode_frame(FrameType::Control, encode_control(m));
}

// ─── §7, §8 Pure validators ─────────────────────────────────────────────────────────────────

bool is_valid_session_id(std::string_view s) noexcept {
    if (s.empty() || s.size() > kMaxSessionIdBytes) return false;
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool is_valid_stable_offset(std::string_view text, uint64_t stable_bytes) noexcept {
    if (stable_bytes > static_cast<uint64_t>(text.size())) return false;
    // The UTF-8 sequence check alone is NOT sufficient — fixture 013 is built entirely from
    // offsets that are valid code-point boundaries and invalid cluster boundaries.
    // is_grapheme_boundary() subsumes it.
    return utf8::is_grapheme_boundary(text, static_cast<size_t>(stable_bytes));
}

const char* to_string(StabilityCheck c) noexcept {
    switch (c) {
        case StabilityCheck::Ok: return "ok";
        case StabilityCheck::OutOfRange: return "out_of_range";
        case StabilityCheck::NotGraphemeBoundary: return "not_grapheme_boundary";
        case StabilityCheck::Decreased: return "decreased";
        case StabilityCheck::PrefixMutated: return "prefix_mutated";
    }
    return "ok";
}

StabilityCheck check_stability(std::string_view prev_text, uint64_t prev_stable,
                               std::string_view text, uint64_t stable_bytes,
                               size_t* diverge_offset) noexcept {
    if (diverge_offset != nullptr) *diverge_offset = 0;

    // §8.2 requirement 1.
    if (stable_bytes > static_cast<uint64_t>(text.size())) return StabilityCheck::OutOfRange;

    // §8.2 requirement 2.
    if (!utf8::is_grapheme_boundary(text, static_cast<size_t>(stable_bytes))) {
        return StabilityCheck::NotGraphemeBoundary;
    }

    // §8.2 requirement 3, first half.
    if (stable_bytes < prev_stable) return StabilityCheck::Decreased;

    // §8.2 requirement 3, second half — the one with teeth. A non-decreasing stable_bytes whose
    // underlying bytes changed has still retracted text the Client may already have typed into
    // the user's document, and no amount of "it did not go backwards" makes that recoverable.
    const size_t prev_n = static_cast<size_t>(
        std::min<uint64_t>(prev_stable, static_cast<uint64_t>(prev_text.size())));
    if (prev_stable > static_cast<uint64_t>(prev_text.size())) {
        // The previously published mark did not even fit its own text. Whatever happened, the
        // published prefix cannot be reproduced, so it counts as mutated rather than as ok.
        if (diverge_offset != nullptr) *diverge_offset = prev_text.size();
        return StabilityCheck::PrefixMutated;
    }
    for (size_t i = 0; i < prev_n; ++i) {
        if (text[i] != prev_text[i]) {
            if (diverge_offset != nullptr) *diverge_offset = i;
            return StabilityCheck::PrefixMutated;
        }
    }
    return StabilityCheck::Ok;
}

// ─── §10.1 Connection states ────────────────────────────────────────────────────────────────

namespace {

struct ConnStateRow {
    ConnState state;
    const char* text;
};

// Uppercase, exactly as docs/protocol.md §10.1 and the fixture `initial.state` field spell them.
// The Python runner compares these as strings; changing one changes a cross-language contract.
constexpr ConnStateRow kConnStates[] = {
    {ConnState::NoServer, "NO_SERVER"},  {ConnState::Connecting, "CONNECTING"},
    {ConnState::Spawning, "SPAWNING"},   {ConnState::Validating, "VALIDATING"},
    {ConnState::Handshake, "HANDSHAKE"}, {ConnState::Loading, "LOADING"},
    {ConnState::Idle, "IDLE"},           {ConnState::Running, "RUNNING"},
    {ConnState::Finalizing, "FINALIZING"}, {ConnState::Cancelling, "CANCELLING"},
    {ConnState::Dead, "DEAD"},
};

}  // namespace

const char* to_string(ConnState s) noexcept {
    for (const ConnStateRow& r : kConnStates) {
        if (r.state == s) return r.text;
    }
    return "DEAD";
}

bool conn_state_from_string(std::string_view s, ConnState& out) noexcept {
    for (const ConnStateRow& r : kConnStates) {
        if (s == r.text) {
            out = r.state;
            return true;
        }
    }
    return false;
}

// ─── §7.4 Sequence rules ────────────────────────────────────────────────────────────────────

SeqVerdict classify_seq(uint64_t last_seq, uint64_t seq) noexcept {
    if (seq == last_seq) return SeqVerdict::DropDuplicate;
    if (seq < last_seq) return SeqVerdict::DropOutOfOrder;
    if (seq == last_seq + 1) return SeqVerdict::Accept;
    // A gap is NOT fatal. On a reliable transport it cannot arise in transit, so it means the
    // sender skipped numbers or dropped its own output — but the message in hand is still the
    // newest hypothesis, and discarding it would be worse than accepting it. The consequence is
    // the anchor-confidence reset the caller must perform, not a rejection.
    return SeqVerdict::AcceptAfterGap;
}

}  // namespace kd::proto
