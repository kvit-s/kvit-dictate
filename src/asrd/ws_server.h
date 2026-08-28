// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// RFC 6455 **server**, by hand, on raw Winsock. docs/protocol.md §2.6, from the other side.
//
// ─── Why this file exists at all ────────────────────────────────────────────────────────────
//
// **WinHTTP is a client API.** It gave `src/ipc_client.cpp` its WebSocket for free
// (WinHttpWebSocketCompleteUpgrade); there is no server-side counterpart short of `http.sys`,
// which wants an HTTP namespace reservation and — on any binding that is not loopback —
// elevation. Neither is acceptable for a program whose installer is deliberately per-user and
// prompt-free (release-plan.md §3). So: `ws2_32`, and the RFC by hand.
//
// This is the third implementation of the same handshake in this repository. `ws.py` and
// `docs/examples/minimal_server.py` are the first two, both validated against a real WinHTTP
// client, and this file is a transcription of their design rather than a fresh reading of the
// RFC. Where it and they disagree, they are right and this file is the bug.
//
// ─── Layering, restated because it is the thing most easily got wrong ───────────────────────
//
// L0 (this file)   TCP, the opening handshake, masking, ping/pong, close, continuation.
// L1 (protocol.h)  the ordinary 4-byte-length protocol frame, *inside* one binary message.
//
// `proto::FrameReader` is **not** a WebSocket parser. After a binary message is reassembled
// here, exactly one protocol frame is expected inside it (§2.6.3), and the redundant length
// prefix is checked against the message size — two frames packed into one message would
// otherwise decode as a legal stream and surface much later as garbled text.
//
// ─── Threading ──────────────────────────────────────────────────────────────────────────────
//
// `WsReader` is pure and single-owner: bytes in, messages out, no socket, no allocation policy
// beyond a buffer. That is what makes the handshake and the frame layer testable without a
// listening port, which is how `tests/test_ws_server.cpp` runs on a machine with no network.
//
// `WsConnection` is a socket. `send_binary()` takes a lock because the decode thread writes
// `partial`/`final` while the connection thread is parked in `recv()`; two frames interleaved on
// one socket is not a slow stream, it is a corrupt one. `shutdown_now()` is callable from any
// thread and is the only way to unblock a parked reader.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "util/result.h"

namespace kd::asrd {

// RFC 6455 §1.3. Written here rather than recalled, because getting it wrong produces a
// handshake both endpoints believe succeeded and that WinHTTP rejects with "invalid server
// response" — see the note in ws.py, which paid for that lesson first.
inline constexpr const char* kWsGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// docs/protocol.md §2.6.2. Echoed when a client offers it; never invented.
inline constexpr const char* kSubprotocol = "kvit-dictate.v1";

inline constexpr uint16_t kCloseNormal = 1000;
inline constexpr uint16_t kCloseProtocolError = 1002;
inline constexpr uint16_t kCloseTooBig = 1009;

// Default listening port for a hand-started translator. The supervised case gets a port the
// client chose (openai-translator.md §4.6); this is only the standalone default, so that
// `kd_probe --server ws://127.0.0.1:10402/` keeps working.
inline constexpr uint16_t kDefaultPort = 10402;

// ─── Opening handshake ──────────────────────────────────────────────────────────────────────

// base64(SHA-1(key + kWsGuid)). SHA-1 comes from bcrypt.dll, which CMakeLists.txt already links
// for BCryptGenRandom; `crypt32` is not a dependency of this project and is not added for one
// call. Returns false only if bcrypt itself fails.
bool compute_accept(std::string_view key, std::string& out);

// Standard base64, no line breaks. Twenty lines by hand rather than a library.
std::string base64_encode(const void* data, size_t size);

struct HttpHead {
    std::string method;
    std::string path;
    // Names are lowercased; values are trimmed. Linear search: a request has a dozen headers.
    std::vector<std::pair<std::string, std::string>> headers;

    [[nodiscard]] std::string_view get(std::string_view name) const noexcept;
    [[nodiscard]] bool has(std::string_view name) const noexcept { return !get(name).empty(); }
};

// Parse a complete request head (everything before the blank line). Returns false if there is
// not even a request line.
bool parse_http_head(std::string_view raw, HttpHead& out);

// The 101 response for a conforming upgrade request, or the 400 body for anything else.
//
// `echo_subprotocol` is honoured only when the client actually offered `kvit-dictate.v1`;
// §2.6.2 makes echoing a SHOULD, and echoing something that was not offered is fatal on the
// client's side.
std::string build_upgrade_response(std::string_view accept_key, bool echo_subprotocol);

// A plain HTTP request against this port is somebody pasting the URL into a browser more often
// than it is anything else. Say what this is, with the port, rather than hanging up.
std::string build_bad_request_response(uint16_t port);

// True when the head is a conforming WebSocket upgrade: `Upgrade: websocket` (token-wise) and a
// non-empty `Sec-WebSocket-Key`.
bool is_upgrade_request(const HttpHead& head);

// True when `Sec-WebSocket-Protocol` lists `kvit-dictate.v1` among its comma-separated tokens.
bool offers_subprotocol(const HttpHead& head);

// ─── Frames ─────────────────────────────────────────────────────────────────────────────────

enum class WsOpcode : uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

enum class WsStatus : uint8_t {
    Message,   // `out` holds one reassembled data message
    NeedMore,  // push() more bytes
    Closed,    // the peer sent Close, or the stream ended cleanly at a message boundary
    Error,     // error() says why; the connection is over
};

// One unfragmented frame. `mask` is false for everything this file sends: RFC 6455 §5.1 says a
// server MUST NOT mask. The parameter exists so the tests can build client frames.
std::string encode_ws_frame(WsOpcode opcode, std::string_view payload, const uint8_t* mask4);

// A byte-stream WebSocket message decoder.
//
// Tolerates arbitrary fragmentation and coalescing for the same reason `proto::FrameReader`
// does: TCP delivers bytes, not frames. Control frames interleaved between the fragments of a
// data message are handled here and never reach the caller — omitting that is how a handshake
// that looks fine dies on the first idle ping.
class WsReader {
public:
    WsReader();
    ~WsReader();
    WsReader(const WsReader&) = delete;
    WsReader& operator=(const WsReader&) = delete;

    void push(const void* data, size_t size);
    void push(std::string_view bytes) { push(bytes.data(), bytes.size()); }
    void notify_eof() noexcept;

    // One reassembled data message. `opcode` is Binary or Text — a Text message is not rejected
    // here, because "binary only" is a docs/protocol.md rule and not an RFC 6455 one, and the
    // layer that owns that rule is the one that must answer `unknown_frame_type` for it.
    WsStatus next(WsOpcode& opcode, std::string& payload) noexcept;

    // Bytes this reader owes the peer: a pong for every ping, and the echo of a Close. The
    // caller must drain this after every next() and write it to the socket. Returns false when
    // there is nothing outstanding.
    bool take_outbound(std::string& out);

    // Valid after next() returned Error. Sticky.
    [[nodiscard]] const std::string& error() const noexcept;

    // The status code the peer sent with its Close, or 1005 ("no status") when it sent none.
    [[nodiscard]] uint16_t close_status() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ─── Sockets ────────────────────────────────────────────────────────────────────────────────

// One upgraded connection. Owns the socket and closes it in the destructor.
class WsConnection {
public:
    ~WsConnection();
    WsConnection(const WsConnection&) = delete;
    WsConnection& operator=(const WsConnection&) = delete;

    // One reassembled data message, blocking. False on close, EOF, or a transport fault;
    // `error()` distinguishes them for the log.
    bool recv_message(WsOpcode& opcode, std::string& payload);

    // One binary WebSocket message. Serialised against every other sender. False if the socket
    // is gone, which is not a protocol error — there is nobody left to tell.
    bool send_binary(std::string_view payload);

    // Best-effort orderly close (§2.6.4), then the socket. Idempotent, callable from any thread.
    void close(uint16_t status = kCloseNormal);

    // Unblock a parked reader from another thread. Does not send a Close.
    void shutdown_now();

    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] const std::string& peer() const noexcept { return peer_; }

private:
    friend class WsListener;
    WsConnection(uintptr_t sock, std::string peer);

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string error_;
    std::string peer_;
};

// The listening socket.
class WsListener {
public:
    WsListener();
    ~WsListener();
    WsListener(const WsListener&) = delete;
    WsListener& operator=(const WsListener&) = delete;

    // `host` is an address literal ("127.0.0.1", "0.0.0.0", "::1"). Port 0 binds an ephemeral
    // port; port() reports what was chosen. Loopback is the default everywhere else in this
    // program and binding anything else is an explicit act by whoever passed --listen.
    Status listen(const std::string& host, uint16_t port);

    [[nodiscard]] uint16_t port() const noexcept { return port_; }

    // Accept one connection and complete the opening handshake.
    //
    // A request that is not an upgrade gets the 400 from build_bad_request_response() and this
    // returns nullptr with `retry` set — the listener is fine and the caller should accept
    // again. `retry` false means the listener itself is finished (stop() was called, or accept
    // failed unrecoverably).
    std::unique_ptr<WsConnection> accept(bool& retry);

    // Unblock accept() from another thread and refuse further connections.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    uint16_t port_ = 0;
};

}  // namespace kd::asrd
