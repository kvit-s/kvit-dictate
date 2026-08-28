// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// tests/test_asrd_ws.cpp — the RFC 6455 server, without a socket.
//
// `WsReader` and the handshake helpers are pure by design (ws_server.h's threading note), which
// is what lets this file run on a machine with no network and in a build where nothing listens.
// Everything below is a byte-level assertion about the layer that the rest of the translator
// is invisible above.
//
// The one test here that is not optional is `accept_key_matches_the_rfc_test_vector`. A wrong
// GUID or a wrong base64 produces a handshake **both endpoints believe succeeded** and that
// WinHTTP rejects with "invalid server response" — a failure that looks like a network problem
// and is not one. ws.py carries the same warning for the same reason.

#include "kd_test.h"

#include "asrd/ws_server.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace kd::asrd;

// A client frame: masked, as RFC 6455 §5.1 requires of a client.
std::string client_frame(WsOpcode op, std::string_view payload, bool fin = true) {
    static const uint8_t mask[4] = {0x37, 0xFA, 0x21, 0x3D};  // the RFC's own example mask
    std::string f = encode_ws_frame(op, payload, mask);
    if (!fin) f[0] = static_cast<char>(static_cast<uint8_t>(f[0]) & 0x7F);
    return f;
}

// Feed `bytes` to a reader `chunk` bytes at a time, collecting the messages that come out.
// Chunking is a parameter because a WebSocket reader that is only correct at one chunk size is
// correct by accident — the same property `proto::FrameReader` is held to by fixture 001.
struct Drained {
    std::vector<std::string> messages;
    std::vector<WsOpcode> opcodes;
    std::string outbound;
    WsStatus last = WsStatus::NeedMore;
    std::string error;
};

Drained drain(std::string_view bytes, size_t chunk, bool eof = false) {
    Drained out;
    WsReader reader;
    size_t at = 0;
    for (;;) {
        for (;;) {
            WsOpcode op = WsOpcode::Binary;
            std::string payload;
            const WsStatus st = reader.next(op, payload);
            std::string owed;
            while (reader.take_outbound(owed)) out.outbound += owed;
            out.last = st;
            if (st == WsStatus::Message) {
                out.messages.push_back(std::move(payload));
                out.opcodes.push_back(op);
                continue;
            }
            if (st == WsStatus::Error) out.error = reader.error();
            break;
        }
        if (out.last == WsStatus::Error || out.last == WsStatus::Closed) return out;
        if (at >= bytes.size()) {
            if (!eof) return out;
            reader.notify_eof();
            eof = false;
            continue;
        }
        const size_t n = chunk == 0 ? bytes.size() - at : (std::min)(chunk, bytes.size() - at);
        reader.push(bytes.data() + at, n);
        at += n;
    }
}

}  // namespace

// ─── The handshake ──────────────────────────────────────────────────────────────────────────

KD_TEST(asrd_ws, accept_key_matches_the_rfc_test_vector) {
    // RFC 6455 §1.3, verbatim. This is the vector, not a value copied from our own output — a
    // self-consistent implementation of a wrong GUID passes every test but the real one.
    std::string accept;
    KD_REQUIRE(compute_accept("dGhlIHNhbXBsZSBub25jZQ==", accept));
    KD_CHECK_EQ(accept, std::string("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}

KD_TEST(asrd_ws, base64_covers_every_padding_case) {
    KD_CHECK_EQ(base64_encode("", 0), std::string(""));
    KD_CHECK_EQ(base64_encode("f", 1), std::string("Zg=="));
    KD_CHECK_EQ(base64_encode("fo", 2), std::string("Zm8="));
    KD_CHECK_EQ(base64_encode("foo", 3), std::string("Zm9v"));
    KD_CHECK_EQ(base64_encode("foob", 4), std::string("Zm9vYg=="));
    KD_CHECK_EQ(base64_encode("fooba", 5), std::string("Zm9vYmE="));
    KD_CHECK_EQ(base64_encode("foobar", 6), std::string("Zm9vYmFy"));
}

KD_TEST(asrd_ws, parses_a_request_head_and_lowercases_names) {
    HttpHead head;
    KD_REQUIRE(parse_http_head(
        "GET /asr?x=1 HTTP/1.1\r\n"
        "Host: 127.0.0.1:10402\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        head));
    KD_CHECK_EQ(head.method, std::string("GET"));
    KD_CHECK_EQ(head.path, std::string("/asr?x=1"));
    KD_CHECK_EQ(std::string(head.get("upgrade")), std::string("websocket"));
    KD_CHECK_EQ(std::string(head.get("Sec-WebSocket-Key")),
                std::string("dGhlIHNhbXBsZSBub25jZQ=="));
    KD_CHECK(is_upgrade_request(head));
    KD_CHECK(!offers_subprotocol(head));
}

KD_TEST(asrd_ws, a_plain_get_is_not_an_upgrade) {
    HttpHead head;
    KD_REQUIRE(parse_http_head("GET / HTTP/1.1\r\nHost: x\r\n\r\n", head));
    KD_CHECK(!is_upgrade_request(head));

    // Somebody pasted the URL into a browser. The answer says what this is and names the port,
    // rather than hanging up — which is the difference between "the app is broken" and "oh, it
    // is not an HTTP API".
    const std::string reply = build_bad_request_response(10402);
    KD_CHECK(reply.find("400 Bad Request") != std::string::npos);
    KD_CHECK(reply.find("ws://127.0.0.1:10402/") != std::string::npos);
    KD_CHECK(reply.find("Content-Length: ") != std::string::npos);
}

KD_TEST(asrd_ws, subprotocol_is_echoed_only_when_offered) {
    // §2.6.2: echoing is a SHOULD, and echoing something the client did NOT offer is fatal on
    // its side. So the two directions are tested separately.
    HttpHead offered;
    KD_REQUIRE(parse_http_head(
        "GET / HTTP/1.1\r\nUpgrade: websocket\r\nSec-WebSocket-Key: k\r\n"
        "Sec-WebSocket-Protocol: something-else, kvit-dictate.v1\r\n\r\n",
        offered));
    KD_CHECK(offers_subprotocol(offered));

    HttpHead absent;
    KD_REQUIRE(parse_http_head(
        "GET / HTTP/1.1\r\nUpgrade: websocket\r\nSec-WebSocket-Key: k\r\n"
        "Sec-WebSocket-Protocol: chat\r\n\r\n",
        absent));
    KD_CHECK(!offers_subprotocol(absent));

    const std::string with = build_upgrade_response("abc", true);
    KD_CHECK(with.find("101 Switching Protocols") != std::string::npos);
    KD_CHECK(with.find("Sec-WebSocket-Accept: abc\r\n") != std::string::npos);
    KD_CHECK(with.find("Sec-WebSocket-Protocol: kvit-dictate.v1\r\n") != std::string::npos);

    const std::string without = build_upgrade_response("abc", false);
    KD_CHECK(without.find("Sec-WebSocket-Protocol") == std::string::npos);
}

KD_TEST(asrd_ws, upgrade_header_is_matched_token_wise) {
    // Real clients send "Connection: keep-alive, Upgrade" and, less often, an Upgrade value with
    // a version token. A substring match on "websocket" would also accept "not-websocket".
    HttpHead head;
    KD_REQUIRE(parse_http_head(
        "GET / HTTP/1.1\r\nUpgrade: WebSocket\r\nConnection: keep-alive, Upgrade\r\n"
        "Sec-WebSocket-Key: k\r\n\r\n",
        head));
    KD_CHECK(is_upgrade_request(head));
}

// ─── Framing ────────────────────────────────────────────────────────────────────────────────

KD_TEST(asrd_ws, server_frames_are_never_masked) {
    // RFC 6455 §5.1. A server that masks is rejected by every conforming client.
    const std::string f = encode_ws_frame(WsOpcode::Binary, "hi", nullptr);
    KD_REQUIRE(f.size() >= 2);
    KD_CHECK_EQ(static_cast<int>(static_cast<uint8_t>(f[0])), 0x82);
    KD_CHECK_EQ(static_cast<int>(static_cast<uint8_t>(f[1]) & 0x80), 0);
    KD_CHECK_EQ(f.substr(2), std::string("hi"));
}

KD_TEST(asrd_ws, length_encoding_switches_at_126_and_65536) {
    KD_CHECK_EQ(encode_ws_frame(WsOpcode::Binary, std::string(125, 'x'), nullptr).size(),
                size_t{2 + 125});
    KD_CHECK_EQ(encode_ws_frame(WsOpcode::Binary, std::string(126, 'x'), nullptr).size(),
                size_t{4 + 126});
    KD_CHECK_EQ(encode_ws_frame(WsOpcode::Binary, std::string(65535, 'x'), nullptr).size(),
                size_t{4 + 65535});
    KD_CHECK_EQ(encode_ws_frame(WsOpcode::Binary, std::string(65536, 'x'), nullptr).size(),
                size_t{10 + 65536});
}

KD_TEST(asrd_ws, unmasks_a_client_frame_at_every_chunking) {
    const std::string payload = "the quick brown fox jumps over the lazy dog";
    const std::string wire = client_frame(WsOpcode::Binary, payload);

    for (const size_t chunk : {size_t{0}, size_t{1}, size_t{2}, size_t{7}, size_t{1024}}) {
        const Drained d = drain(wire, chunk);
        KD_CHECK_EQ(d.messages.size(), size_t{1});
        if (!d.messages.empty()) KD_CHECK_EQ(d.messages[0], payload);
    }
}

KD_TEST(asrd_ws, rejects_an_unmasked_client_frame) {
    // Refused rather than tolerated: a client that does not mask has not read the RFC, and the
    // next thing it gets wrong will be less visible.
    const std::string wire = encode_ws_frame(WsOpcode::Binary, "hi", nullptr);
    const Drained d = drain(wire, 0);
    KD_CHECK(d.last == WsStatus::Error);
    KD_CHECK(d.error.find("unmasked") != std::string::npos);
}

KD_TEST(asrd_ws, reassembles_continuation_frames) {
    // §2.6.3 makes fragmentation the transport's business: a receiver puts the message back
    // together before the protocol decoder ever sees it.
    std::string wire;
    wire += client_frame(WsOpcode::Binary, "abc", /*fin=*/false);
    wire += client_frame(WsOpcode::Continuation, "def", /*fin=*/false);
    wire += client_frame(WsOpcode::Continuation, "ghi", /*fin=*/true);

    for (const size_t chunk : {size_t{0}, size_t{1}, size_t{5}}) {
        const Drained d = drain(wire, chunk);
        KD_CHECK_EQ(d.messages.size(), size_t{1});
        if (!d.messages.empty()) KD_CHECK_EQ(d.messages[0], std::string("abcdefghi"));
    }
}

KD_TEST(asrd_ws, a_ping_between_fragments_is_answered_and_stays_invisible) {
    // This is the case that kills a handshake which "looked fine": control frames may be
    // interleaved *between* the fragments of a data message, and a reader that reassembles
    // naively either corrupts the message or stops answering pings and gets hung up on.
    std::string wire;
    wire += client_frame(WsOpcode::Binary, "abc", /*fin=*/false);
    wire += client_frame(WsOpcode::Ping, "hello");
    wire += client_frame(WsOpcode::Continuation, "def", /*fin=*/true);

    const Drained d = drain(wire, 0);
    KD_CHECK_EQ(d.messages.size(), size_t{1});
    if (!d.messages.empty()) KD_CHECK_EQ(d.messages[0], std::string("abcdef"));

    // The pong is owed unmasked, with the ping's payload echoed (RFC 6455 §5.5.3).
    const std::string want = encode_ws_frame(WsOpcode::Pong, "hello", nullptr);
    KD_CHECK(d.outbound.find(want) != std::string::npos);
}

KD_TEST(asrd_ws, a_pong_is_swallowed) {
    std::string wire;
    wire += client_frame(WsOpcode::Pong, "");
    wire += client_frame(WsOpcode::Binary, "x");
    const Drained d = drain(wire, 0);
    KD_CHECK_EQ(d.messages.size(), size_t{1});
    KD_CHECK(d.outbound.empty());
}

KD_TEST(asrd_ws, close_is_answered_and_ends_the_stream) {
    std::string payload;
    payload.push_back(static_cast<char>(0x03));  // 1000, big-endian
    payload.push_back(static_cast<char>(0xE8));
    const std::string wire = client_frame(WsOpcode::Close, payload);

    const Drained d = drain(wire, 0);
    KD_CHECK(d.last == WsStatus::Closed);
    KD_CHECK(!d.outbound.empty());
    // §2.6.4: answer a Close with a Close. A server that just drops the socket leaves a
    // conforming client waiting for its own close timeout.
    KD_CHECK_EQ(static_cast<int>(static_cast<uint8_t>(d.outbound[0])), 0x88);
}

KD_TEST(asrd_ws, a_text_message_reaches_the_caller_who_owns_the_binary_only_rule) {
    // "Binary only" is a docs/protocol.md rule (§2.6.3), not an RFC 6455 one, so this layer
    // hands the message up and the protocol layer answers `unknown_frame_type`. Keeping the two
    // separate is what makes the error code right rather than merely fatal.
    const Drained d = drain(client_frame(WsOpcode::Text, "{}"), 0);
    KD_CHECK_EQ(d.messages.size(), size_t{1});
    if (!d.opcodes.empty()) KD_CHECK(d.opcodes[0] == WsOpcode::Text);
}

KD_TEST(asrd_ws, refuses_a_fragmented_or_oversized_control_frame) {
    // RFC 6455 §5.5: control frames are never fragmented and carry at most 125 bytes.
    const Drained fragmented = drain(client_frame(WsOpcode::Ping, "x", /*fin=*/false), 0);
    KD_CHECK(fragmented.last == WsStatus::Error);

    const Drained oversized = drain(client_frame(WsOpcode::Ping, std::string(200, 'x')), 0);
    KD_CHECK(oversized.last == WsStatus::Error);
}

KD_TEST(asrd_ws, refuses_a_reserved_bit) {
    // No extension was negotiated, so a reserved bit means the peer is speaking something this
    // build does not implement (RFC 6455 §5.2).
    std::string wire = client_frame(WsOpcode::Binary, "x");
    wire[0] = static_cast<char>(static_cast<uint8_t>(wire[0]) | 0x40);
    const Drained d = drain(wire, 0);
    KD_CHECK(d.last == WsStatus::Error);
    KD_CHECK(d.error.find("reserved") != std::string::npos);
}

KD_TEST(asrd_ws, refuses_a_continuation_with_nothing_to_continue) {
    const Drained d = drain(client_frame(WsOpcode::Continuation, "abc"), 0);
    KD_CHECK(d.last == WsStatus::Error);
}

KD_TEST(asrd_ws, refuses_a_new_data_frame_inside_a_fragmented_message) {
    std::string wire;
    wire += client_frame(WsOpcode::Binary, "abc", /*fin=*/false);
    wire += client_frame(WsOpcode::Binary, "def", /*fin=*/true);
    const Drained d = drain(wire, 0);
    KD_CHECK(d.last == WsStatus::Error);
}

KD_TEST(asrd_ws, refuses_a_message_past_max_frame_on_wire) {
    // The bound is applied before the protocol layer sees anything: §3.3 bounds the protocol
    // frame, this bounds what a hostile peer can make us allocate while we are still working
    // out that the frame is illegal. Declared without ever building the payload, because the
    // point is that the length field alone is enough to refuse.
    std::string wire;
    wire.push_back(static_cast<char>(0x82));
    wire.push_back(static_cast<char>(0x80 | 127));
    const uint64_t huge = 1ull << 40;
    for (int shift = 56; shift >= 0; shift -= 8) {
        wire.push_back(static_cast<char>((huge >> shift) & 0xFF));
    }
    wire += "\x00\x00\x00\x00";  // mask
    const Drained d = drain(wire, 0);
    KD_CHECK(d.last == WsStatus::Error);
    KD_CHECK(d.error.find("MAX_FRAME_ON_WIRE") != std::string::npos);
}

KD_TEST(asrd_ws, eof_at_a_message_boundary_is_a_clean_close) {
    const Drained d = drain(client_frame(WsOpcode::Binary, "x"), 0, /*eof=*/true);
    KD_CHECK_EQ(d.messages.size(), size_t{1});
    KD_CHECK(d.last == WsStatus::Closed);
    KD_CHECK(d.error.empty());
}

KD_TEST(asrd_ws, eof_mid_frame_is_an_error_not_a_clean_close) {
    // The difference matters: a truncated frame means the peer died mid-write, and reporting it
    // as an orderly shutdown would hide a crash on the other end. Same distinction fixtures 010
    // and 011 draw one layer up.
    std::string wire = client_frame(WsOpcode::Binary, "abcdef");
    wire.resize(wire.size() - 3);
    const Drained d = drain(wire, 0, /*eof=*/true);
    KD_CHECK(d.last == WsStatus::Error);
}

KD_TEST(asrd_ws, several_messages_in_one_delivery_all_come_out) {
    // Fixture 002's lesson, one layer up: a caller that handles one message per read stalls.
    std::string wire;
    wire += client_frame(WsOpcode::Binary, "one");
    wire += client_frame(WsOpcode::Binary, "two");
    wire += client_frame(WsOpcode::Binary, "three");
    const Drained d = drain(wire, 0);
    KD_REQUIRE_EQ(d.messages.size(), size_t{3});
    KD_CHECK_EQ(d.messages[0], std::string("one"));
    KD_CHECK_EQ(d.messages[1], std::string("two"));
    KD_CHECK_EQ(d.messages[2], std::string("three"));
}
