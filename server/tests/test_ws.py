# Copyright 2026 Kvit
# Licensed under the Apache License, Version 2.0. See LICENSE-APACHE, or
# https://www.apache.org/licenses/LICENSE-2.0
r"""The WebSocket binding, from the outside: `kvit_asr.ws` and the listener in `server.py`.

``test_server.py`` covers the same `Server` over an in-memory channel, and that is where the
session semantics are asserted. What is only reachable here is everything the transport adds:
the RFC 6455 upgrade, message reassembly, the length-prefix check §2.6.3 keeps, the refusal path
§2.7 requires, and `hello.model` / `hello_ack.models[]` from §5.1.

The client half is written here from docs/protocol.md, not imported from `kvit_asr`, for the
same reason `test_minimal_server.py` does it: a test that shares a codec with the thing it tests
cannot catch the codec being wrong.
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import socket
import struct
import sys
import threading
import time
import unittest

_SERVER_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _SERVER_DIR not in sys.path:
    sys.path.insert(0, _SERVER_DIR)

from kvit_asr import config as kd_config  # noqa: E402
from kvit_asr import protocol as proto  # noqa: E402
from kvit_asr import ws as kd_ws  # noqa: E402
from kvit_asr.server import EventLogger, _serve_websocket  # noqa: E402

TYPE_CONTROL = 0x01
TYPE_AUDIO = 0x02
CRLF = "\r\n"


class WsClient:
    """A throwaway RFC 6455 client. Masks everything it sends, as §2.6 requires of a client."""

    def __init__(self, host: str, port: int, *, offer_subprotocol: bool = True) -> None:
        self.sock = socket.create_connection((host, port), timeout=20)
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        lines = [
            "GET / HTTP/1.1",
            f"Host: {host}:{port}",
            "Upgrade: websocket",
            "Connection: Upgrade",
            f"Sec-WebSocket-Key: {key}",
            "Sec-WebSocket-Version: 13",
        ]
        if offer_subprotocol:
            lines.append("Sec-WebSocket-Protocol: kvit-dictate.v1")
        self.sock.sendall((CRLF.join(lines) + CRLF + CRLF).encode("ascii"))

        raw = b""
        while (CRLF + CRLF).encode("ascii") not in raw:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("closed during upgrade")
            raw += chunk
        self.response = raw.split((CRLF + CRLF).encode("ascii"))[0].decode("latin-1")
        if not self.response.startswith("HTTP/1.1 101"):
            raise ConnectionError(self.response.splitlines()[0])
        self._buf = bytearray()

    def _recv_exactly(self, n: int) -> bytes:
        while len(self._buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("end of stream")
            self._buf += chunk
        out = bytes(self._buf[:n])
        del self._buf[:n]
        return out

    def send_ws(self, opcode: int, payload: bytes) -> None:
        header = bytearray([0x80 | opcode])
        n = len(payload)
        if n < 126:
            header.append(0x80 | n)
        elif n < 65536:
            header.append(0x80 | 126)
            header += struct.pack("!H", n)
        else:
            header.append(0x80 | 127)
            header += struct.pack("!Q", n)
        mask = os.urandom(4)
        header += mask
        self.sock.sendall(bytes(header) + bytes(b ^ mask[i & 3] for i, b in enumerate(payload)))

    def recv_ws(self) -> tuple[int, bytes]:
        b0, b1 = self._recv_exactly(2)
        assert not (b1 & 0x80), "a server MUST NOT mask (RFC 6455 §5.1)"
        length = b1 & 0x7F
        if length == 126:
            (length,) = struct.unpack("!H", self._recv_exactly(2))
        elif length == 127:
            (length,) = struct.unpack("!Q", self._recv_exactly(8))
        return b0 & 0x0F, self._recv_exactly(length)

    def send_frame(self, ftype: int, payload: bytes) -> None:
        self.send_ws(0x2, struct.pack("<I", 1 + len(payload)) + bytes([ftype]) + payload)

    def send_control(self, **msg: object) -> None:
        self.send_frame(TYPE_CONTROL, json.dumps(msg).encode("utf-8"))

    def recv_control(self) -> dict:
        opcode, data = self.recv_ws()
        if opcode == 0x8:
            raise ConnectionError(f"close: {data!r}")
        assert opcode == 0x2, f"expected a binary message, got opcode {opcode}"
        (length,) = struct.unpack("<I", data[:4])
        assert len(data) == 4 + length, "§2.6.3: one message, one frame"
        assert data[4] == TYPE_CONTROL
        return json.loads(data[5:].decode("utf-8"))

    def recv_until(self, t: str, limit: int = 40) -> dict:
        for _ in range(limit):
            msg = self.recv_control()
            if msg.get("t") == t:
                return msg
            if msg.get("t") == "error" and t != "error":
                raise AssertionError(f"expected {t}, got error {msg}")
        raise AssertionError(f"no {t} within {limit} messages")

    def close(self) -> None:
        try:
            self.send_ws(0x8, struct.pack("!H", 1000))
        except OSError:
            pass
        self.sock.close()


class ListeningServer:
    """`kvit_asr` on an ephemeral port, with the mock backend."""

    def __init__(self, *, max_connections: int = 1, model: str | None = None) -> None:
        # An ephemeral port would be ideal, but the listener takes host and port rather than a
        # bound socket. Asking the OS for a free one and closing it immediately races only with
        # another process binding the same port in the same millisecond.
        probe = socket.socket()
        probe.bind(("127.0.0.1", 0))
        self.port = probe.getsockname()[1]
        probe.close()

        argv = ["--listen", f"127.0.0.1:{self.port}", "--backend", "mock", "--max-connections",
                str(max_connections)]
        if model:
            argv += ["--model", model]
        self.cfg = kd_config.parse_args(argv)
        self.log = EventLogger(level="error")
        self.thread = threading.Thread(
            target=_serve_websocket, args=(self.cfg, self.log), daemon=True
        )
        self.thread.start()
        self._wait_until_listening()

    def _wait_until_listening(self, timeout: float = 10.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.5).close()
                return
            except OSError:
                time.sleep(0.02)
        raise AssertionError("the listener never came up")

    def client(self, **kw) -> WsClient:
        return WsClient("127.0.0.1", self.port, **kw)

    def __enter__(self) -> "ListeningServer":
        return self

    def __exit__(self, *exc) -> None:
        # The listener has no shutdown hook of its own; the thread is a daemon and the process
        # is about to move on. Nothing here holds a port for longer than the test does.
        pass


# ─── The pure halves of kvit_asr.ws ───────────────────────────────────────────────────────────


class Rfc6455(unittest.TestCase):
    def test_accept_matches_the_rfc_test_vector(self):
        # A wrong GUID produces a handshake both endpoints believe succeeded and that WinHTTP
        # rejects as an invalid server response. This is the one-second version of that bug.
        self.assertEqual(
            kd_ws.compute_accept("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
        )

    def test_server_frames_are_never_masked(self):
        frame = kd_ws.encode_frame(kd_ws.OPCODE_BINARY, b"hello")
        self.assertEqual(frame[0], 0x82)  # FIN + binary
        self.assertEqual(frame[1] & 0x80, 0)  # RFC 6455 §5.1
        self.assertEqual(frame[2:], b"hello")

    def test_frame_length_forms(self):
        self.assertEqual(kd_ws.encode_frame(0x2, b"x" * 125)[1], 125)
        self.assertEqual(kd_ws.encode_frame(0x2, b"x" * 126)[1], 126)
        self.assertEqual(kd_ws.encode_frame(0x2, b"x" * 65536)[1], 127)

    def test_http_head_parsing(self):
        raw = (CRLF.join(["GET /v1?a=b HTTP/1.1", "Host: h", "Upgrade: WebSocket", "", ""])).encode()
        path, headers = kd_ws.parse_http_head(raw)
        self.assertEqual(path, "/v1?a=b")
        self.assertEqual(headers["upgrade"], "WebSocket")  # value preserved, name lowercased


class ListenArgument(unittest.TestCase):
    def test_accepted_forms(self):
        self.assertEqual(kd_config.parse_listen("ws://127.0.0.1:10402"), ("127.0.0.1", 10402))
        self.assertEqual(kd_config.parse_listen("127.0.0.1:10402"), ("127.0.0.1", 10402))
        self.assertEqual(kd_config.parse_listen("[::1]:10402"), ("::1", 10402))
        self.assertEqual(kd_config.parse_listen(":10402"), ("0.0.0.0", 10402))

    def test_refusals_say_what_to_do(self):
        for bad in ("wss://h:1", "h", "h:0", "h:70000", "h:x", "::1:10402", "ws://h:1/v1"):
            with self.assertRaises(ValueError, msg=bad) as ctx:
                kd_config.parse_listen(bad)
            self.assertTrue(str(ctx.exception), f"{bad} was refused without a reason")

    def test_exactly_one_binding_must_be_chosen(self):
        with self.assertRaises(SystemExit):
            kd_config.parse_args(["--backend", "mock"])
        with self.assertRaises(SystemExit):
            kd_config.parse_args(["--backend", "mock", "--listen", ":1", "--pipe", r"\\.\pipe\x"])

    def test_port_and_host_point_at_listen(self):
        # They used to refuse the very idea of a port. release-plan.md §1 reversed that, and the
        # refusal now names the replacement instead of leaving a reader to guess.
        with self.assertRaises(SystemExit):
            kd_config.parse_args(["--backend", "mock", "--port", "10402"])


# ─── End to end over a real socket ────────────────────────────────────────────────────────────


class Handshake(unittest.TestCase):
    def test_subprotocol_echoed_only_when_offered(self):
        with ListeningServer() as srv:
            c = srv.client()
            self.addCleanup(c.close)
            self.assertIn("Sec-WebSocket-Protocol: kvit-dictate.v1", c.response)

    def test_no_subprotocol_offered_means_none_echoed(self):
        # §2.6.2: echoing something the client did not offer is fatal on its side.
        with ListeningServer() as srv:
            c = srv.client(offer_subprotocol=False)
            self.addCleanup(c.close)
            self.assertNotIn("Sec-WebSocket-Protocol", c.response)

    def test_hello_ack_advertises_the_model_it_serves(self):
        with ListeningServer() as srv:
            c = srv.client()
            self.addCleanup(c.close)
            c.send_control(t="hello", proto=1, client="test/0")
            ack = c.recv_control()
            self.assertEqual(ack["t"], "hello_ack")
            self.assertEqual([m["id"] for m in ack["models"]], ["mock"])
            ready = c.recv_until("ready")
            self.assertEqual(ready["model"], "mock")

    def test_requested_model_is_honoured(self):
        with ListeningServer() as srv:
            c = srv.client()
            self.addCleanup(c.close)
            c.send_control(t="hello", proto=1, model="mock")
            self.assertEqual(c.recv_control()["t"], "hello_ack")

    def test_unavailable_model_is_refused_not_substituted(self):
        # §5.1. The failure this prevents is the quiet one: a server that loads something else
        # produces transcripts that are merely worse, and nobody notices for weeks.
        with ListeningServer() as srv:
            c = srv.client()
            self.addCleanup(c.close)
            c.send_control(t="hello", proto=1, model="parakeet-tdt")
            err = c.recv_control()
            self.assertEqual(err["code"], "model_unavailable")
            self.assertTrue(err["fatal"])
            self.assertEqual(err["available"], ["mock"])

    def test_wrong_proto_is_refused(self):
        with ListeningServer() as srv:
            c = srv.client()
            self.addCleanup(c.close)
            c.send_control(t="hello", proto=7)
            err = c.recv_control()
            self.assertEqual(err["code"], "proto_unsupported")
            self.assertEqual(err["supported"], [1])

    def test_plain_http_gets_an_explanation_not_a_hangup(self):
        # Somebody will paste the URL into a browser. Answering with a sentence costs four lines
        # and saves the support question.
        with ListeningServer() as srv:
            sock = socket.create_connection(("127.0.0.1", srv.port), timeout=10)
            self.addCleanup(sock.close)
            sock.sendall(f"GET / HTTP/1.1{CRLF}Host: x{CRLF}{CRLF}".encode())
            raw = sock.recv(4096).decode("latin-1")
            self.assertTrue(raw.startswith("HTTP/1.1 400"))
            self.assertIn("WebSocket endpoint", raw)


class Session(unittest.TestCase):
    def _ready(self, srv: ListeningServer) -> WsClient:
        c = srv.client()
        self.addCleanup(c.close)
        c.send_control(t="hello", proto=1)
        self.assertEqual(c.recv_control()["t"], "hello_ack")
        c.recv_until("ready")
        return c

    def test_a_whole_session_over_a_socket(self):
        """R1's gate on this side: a full session, client to server, no child process."""
        with ListeningServer() as srv:
            c = self._ready(srv)
            c.send_control(t="start", session_id="s1", sample_rate=16000)
            # 2 s of audio in 100 ms frames — §6.3's recommended window.
            for _ in range(20):
                c.send_frame(TYPE_AUDIO, b"\x00\x00" * 1600)
            c.send_control(t="stop", session_id="s1")

            stopped = c.recv_until("stopped")
            self.assertEqual(stopped["session_id"], "s1")

    def test_two_sessions_on_one_connection(self):
        with ListeningServer() as srv:
            c = self._ready(srv)
            for sid in ("s1", "s2"):
                c.send_control(t="start", session_id=sid, sample_rate=16000)
                c.send_frame(TYPE_AUDIO, b"\x00\x00" * 1600)
                c.send_control(t="stop", session_id=sid)
                self.assertEqual(c.recv_until("stopped")["session_id"], sid)

    def test_text_message_is_rejected(self):
        # §2.6.3, and fatal even though these exact bytes would have decoded.
        with ListeningServer() as srv:
            c = self._ready(srv)
            body = json.dumps({"t": "start", "session_id": "s1", "sample_rate": 16000}).encode()
            c.send_ws(0x1, struct.pack("<I", 1 + len(body)) + bytes([TYPE_CONTROL]) + body)
            err = c.recv_control()
            self.assertEqual(err["code"], "unknown_frame_type")

    def test_length_prefix_must_agree_with_the_message(self):
        # The prefix is redundant inside a WebSocket message and is checked anyway: two frames
        # packed into one message would otherwise decode as a legal stream.
        with ListeningServer() as srv:
            c = self._ready(srv)
            c.send_ws(0x2, struct.pack("<I", 400) + bytes([TYPE_CONTROL]) + b"{}")
            self.assertEqual(c.recv_control()["code"], "bad_frame")

    def test_fragmented_message_is_reassembled(self):
        with ListeningServer() as srv:
            c = self._ready(srv)
            body = json.dumps({"t": "start", "session_id": "s9", "sample_rate": 16000}).encode()
            frame = struct.pack("<I", 1 + len(body)) + bytes([TYPE_CONTROL]) + body

            def masked(fin: bool, opcode: int, payload: bytes) -> bytes:
                mask = os.urandom(4)
                return (
                    bytes([(0x80 if fin else 0) | opcode, 0x80 | len(payload)])
                    + mask
                    + bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
                )

            c.sock.sendall(masked(False, 0x2, frame[:7]) + masked(True, 0x0, frame[7:]))
            c.send_control(t="stop", session_id="s9")
            self.assertEqual(c.recv_until("stopped")["session_id"], "s9")

    def test_ping_is_answered(self):
        with ListeningServer() as srv:
            c = srv.client()
            self.addCleanup(c.close)
            c.send_ws(0x9, b"still there?")
            opcode, payload = c.recv_ws()
            self.assertEqual((opcode, payload), (0xA, b"still there?"))


class Concurrency(unittest.TestCase):
    def test_second_client_is_refused_in_words(self):
        """§2.7. Silence and a connection that never becomes ready both read as "it hangs"."""
        with ListeningServer(max_connections=1) as srv:
            first = srv.client()
            self.addCleanup(first.close)
            first.send_control(t="hello", proto=1)
            self.assertEqual(first.recv_control()["t"], "hello_ack")
            first.recv_until("ready")

            second = srv.client()
            self.addCleanup(second.close)
            second.send_control(t="hello", proto=1)
            err = second.recv_control()
            self.assertEqual(err["t"], "error")
            self.assertEqual(err["code"], "server_busy")
            self.assertTrue(err["fatal"])
            self.assertEqual(err["retry_after_ms"], 5000)

    def test_two_clients_when_the_limit_allows_it(self):
        with ListeningServer(max_connections=2) as srv:
            clients = []
            for _ in range(2):
                c = srv.client()
                self.addCleanup(c.close)
                c.send_control(t="hello", proto=1)
                self.assertEqual(c.recv_control()["t"], "hello_ack")
                c.recv_until("ready")
                clients.append(c)

            # §2.7 and §7.2: session state is per connection, and `session_id` is only unique
            # within one client's process — two clients starting at "s1" is the ordinary case,
            # not a collision, and a server that keyed on it would cross the streams.
            for c in clients:
                c.send_control(t="start", session_id="s1", sample_rate=16000)
                c.send_frame(TYPE_AUDIO, b"\x00\x00" * 1600)
            for c in clients:
                c.send_control(t="stop", session_id="s1")
            for c in clients:
                self.assertEqual(c.recv_until("stopped")["session_id"], "s1")


class MessageRoundTrip(unittest.TestCase):
    """§5.1's new fields survive the codec both ways."""

    def test_hello_model(self):
        encoded = proto.encode_control(proto.Hello(proto=1, model="parakeet"))
        back = proto.decode_control(encoded)
        self.assertEqual(back.model, "parakeet")
        self.assertIsNone(proto.decode_control(proto.encode_control(proto.Hello(proto=1))).model)

    def test_hello_ack_models(self):
        ack = proto.HelloAck(
            proto=1,
            server="x/1",
            models=[proto.ModelInfo(id="a", mode="native-streaming", context_supported=True)],
        )
        back = proto.decode_control(proto.encode_control(ack))
        self.assertEqual(len(back.models), 1)
        self.assertEqual(back.models[0].id, "a")
        self.assertEqual(back.models[0].mode, "native-streaming")
        self.assertTrue(back.models[0].context_supported)

    def test_a_malformed_catalogue_entry_is_skipped_not_fatal(self):
        # §5.1: the catalogue is advisory. Losing a working connection over a mis-typed display
        # name would be a worse outcome than an incomplete model list.
        raw = json.dumps(
            {"t": "hello_ack", "proto": 1, "models": [{"no_id": 1}, "nonsense", {"id": "b"}]}
        ).encode()
        back = proto.decode_control(raw)
        self.assertEqual([m.id for m in back.models], ["b"])

    def test_models_that_is_not_an_array_is_bad_message(self):
        raw = json.dumps({"t": "hello_ack", "proto": 1, "models": {"id": "b"}}).encode()
        with self.assertRaises(proto.ProtocolError):
            proto.decode_control(raw)

    def test_error_retry_after_and_available(self):
        err = proto.ErrorMessage(
            code="server_busy", fatal=True, msg="busy", retry_after_ms=1500
        )
        back = proto.decode_control(proto.encode_control(err))
        self.assertEqual(back.retry_after_ms, 1500)

        err2 = proto.ErrorMessage(
            code="model_unavailable", fatal=True, msg="no", available=["a", "b"]
        )
        self.assertEqual(proto.decode_control(proto.encode_control(err2)).available, ["a", "b"])

    def test_retry_after_ms_is_clamped_not_rejected(self):
        # A server that says "wait a week" is misconfigured, not malicious, and the receiver's
        # own backoff is the real bound anyway.
        raw = json.dumps(
            {"t": "error", "code": "server_busy", "fatal": True, "msg": "x",
             "retry_after_ms": 99_999_999}
        ).encode()
        self.assertEqual(proto.decode_control(raw).retry_after_ms, 3_600_000)


if __name__ == "__main__":
    unittest.main()
