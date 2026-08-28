# Copyright 2026 Kvit
# Licensed under the Apache License, Version 2.0. See LICENSE-APACHE, or
# https://www.apache.org/licenses/LICENSE-2.0
r"""The worked example in docs/protocol.md §15 is executed, not asserted.

`docs/examples/minimal_server.py` is the file a server implementer is pointed at, and a worked
example that has drifted from the specification is worse than none — it teaches the drift. So
this suite plays the *client* side of the WebSocket binding with nothing but the standard
library, from the RFC 6455 masking outwards, and drives the example through a real session.

Deliberately no import of `kvit_asr` here either. Both sides of this test are written against
docs/protocol.md alone, which is the only way it can catch the document being wrong.
"""

from __future__ import annotations

import base64
import hashlib
import importlib.util
import json
import os
import socket
import struct
import threading
import unittest

_EXAMPLE = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "docs",
    "examples",
    "minimal_server.py",
)


def _load_example():
    spec = importlib.util.spec_from_file_location("kd_minimal_server_example", _EXAMPLE)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


example = _load_example()

TYPE_CONTROL = 0x01
TYPE_AUDIO = 0x02

CRLF = "\r\n"
# RFC 6455 §1.3, spelled out here rather than read from the example, so that the
# test would still fail if the example's constant were changed to match a bug.
RFC6455_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


# ─── The client half of §2.6, written from the document ───────────────────────────────────────


class WsClient:
    """A throwaway RFC 6455 client. Masks everything it sends, as §2.6 requires of a client."""

    def __init__(self, host: str, port: int, *, subprotocol: str = "kvit-dictate.v1") -> None:
        self.sock = socket.create_connection((host, port), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        lines = [
            "GET / HTTP/1.1",
            f"Host: {host}:{port}",
            "Upgrade: websocket",
            "Connection: Upgrade",
            f"Sec-WebSocket-Key: {key}",
            "Sec-WebSocket-Version: 13",
        ]
        if subprotocol:
            lines.append(f"Sec-WebSocket-Protocol: {subprotocol}")
        self.sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("ascii"))

        raw = b""
        while b"\r\n\r\n" not in raw:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("closed during upgrade")
            raw += chunk
        head, _, rest = raw.partition(b"\r\n\r\n")
        assert rest == b"", "this server must not pipeline data behind the 101"
        self.response = head.decode("latin-1")
        if not self.response.startswith("HTTP/1.1 101"):
            raise ConnectionError(self.response.splitlines()[0])

    # -- transport ---------------------------------------------------------------------------

    def _recv_exactly(self, n: int) -> bytes:
        out = bytearray()
        while len(out) < n:
            chunk = self.sock.recv(n - len(out))
            if not chunk:
                raise ConnectionError("end of stream")
            out += chunk
        return bytes(out)

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
        masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self.sock.sendall(bytes(header) + masked)

    def recv_ws(self) -> tuple[int, bytes]:
        b0, b1 = self._recv_exactly(2)
        opcode = b0 & 0x0F
        assert b0 & 0x80, "the example never fragments what it sends"
        assert not (b1 & 0x80), "a server MUST NOT mask (RFC 6455 §5.1)"
        length = b1 & 0x7F
        if length == 126:
            (length,) = struct.unpack("!H", self._recv_exactly(2))
        elif length == 127:
            (length,) = struct.unpack("!Q", self._recv_exactly(8))
        return opcode, self._recv_exactly(length)

    # -- framing (§3) ------------------------------------------------------------------------

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
        assert len(data) == 4 + length, "length field must agree with the message size"
        assert data[4] == TYPE_CONTROL
        return json.loads(data[5:].decode("utf-8"))

    def close(self) -> None:
        try:
            self.send_ws(0x8, struct.pack("!H", 1000))
        except OSError:
            pass
        self.sock.close()


class ExampleServer:
    """The example, on an ephemeral port, in a thread."""

    def __init__(self, **handler_attrs: object) -> None:
        for name, value in {"max_connections": 0, "advertise_models": True}.items():
            setattr(example._Handler, name, handler_attrs.get(name, value))
        self.srv = example._Server(("127.0.0.1", 0), example._Handler)
        self.host, self.port = self.srv.server_address[:2]
        self.thread = threading.Thread(target=self.srv.serve_forever, daemon=True)
        self.thread.start()

    def client(self, **kw) -> WsClient:
        return WsClient(self.host, self.port, **kw)

    def __enter__(self) -> "ExampleServer":
        return self

    def __exit__(self, *exc) -> None:
        self.srv.shutdown()
        self.srv.server_close()
        self.thread.join(timeout=5)


# ─── Tests ────────────────────────────────────────────────────────────────────────────────────


class Rfc6455(unittest.TestCase):
    """The magic GUID, checked against the RFC's own vector rather than trusted.

    A wrong GUID produces a handshake both endpoints think succeeded and that every real
    WebSocket client rejects with "invalid server response" — which is exactly what WinHTTP did
    when this file first shipped one. The test that would have caught it in a second is this.
    """

    def test_accept_matches_the_rfc_test_vector(self):
        # RFC 6455 §1.3: key "dGhlIHNhbXBsZSBub25jZQ==" must produce this accept value.
        digest = hashlib.sha1(("dGhlIHNhbXBsZSBub25jZQ==" + example.WS_GUID).encode("ascii"))
        self.assertEqual(base64.b64encode(digest.digest()).decode(), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")


class Handshake(unittest.TestCase):
    def test_accept_header_is_correct_for_our_own_key(self):
        """The end-to-end version: our key, the server's answer, computed independently here."""
        with ExampleServer() as srv:
            key = base64.b64encode(os.urandom(16)).decode("ascii")
            sock = socket.create_connection((srv.host, srv.port), timeout=10)
            self.addCleanup(sock.close)
            request = CRLF.join(
                [
                    "GET / HTTP/1.1",
                    f"Host: {srv.host}:{srv.port}",
                    "Upgrade: websocket",
                    "Connection: Upgrade",
                    f"Sec-WebSocket-Key: {key}",
                    "Sec-WebSocket-Version: 13",
                    "",
                    "",
                ]
            )
            sock.sendall(request.encode("ascii"))
            raw = b""
            while (CRLF + CRLF).encode("ascii") not in raw:
                raw += sock.recv(4096)
            expect = base64.b64encode(
                hashlib.sha1((key + RFC6455_GUID).encode("ascii")).digest()
            ).decode()
            self.assertIn(f"Sec-WebSocket-Accept: {expect}", raw.decode("latin-1"))

    def test_subprotocol_is_echoed_when_offered(self):
        with ExampleServer() as srv:
            c = srv.client()
            self.addCleanup(c.close)
            self.assertIn("Sec-WebSocket-Protocol: kvit-dictate.v1", c.response)

    def test_hello_ack_then_ready(self):
        with ExampleServer() as srv:
            c = srv.client()
            self.addCleanup(c.close)
            c.send_control(t="hello", proto=1, client="test/0")
            ack = c.recv_control()
            self.assertEqual(ack["t"], "hello_ack")
            self.assertEqual(ack["proto"], 1)
            # §5.1: the catalogue is optional, and this server advertises one.
            self.assertEqual([m["id"] for m in ack["models"]], ["counter"])

            ready = c.recv_control()
            self.assertEqual(ready["t"], "ready")
            self.assertEqual(ready["model"], "counter")
            self.assertEqual(ready["mode"], "native-streaming")
            self.assertIsInstance(ready["warm_ms"], int)

    def test_catalogue_can_be_absent(self):
        with ExampleServer(advertise_models=False) as srv:
            c = srv.client()
            self.addCleanup(c.close)
            c.send_control(t="hello", proto=1)
            self.assertNotIn("models", c.recv_control())

    def test_wrong_proto_is_refused_not_downgraded(self):
        with ExampleServer() as srv:
            c = srv.client()
            self.addCleanup(c.close)
            c.send_control(t="hello", proto=99)
            err = c.recv_control()
            self.assertEqual(err["t"], "error")
            self.assertEqual(err["code"], "proto_unsupported")
            self.assertTrue(err["fatal"])
            self.assertEqual(err["supported"], [1])

    def test_unknown_model_is_refused_not_substituted(self):
        with ExampleServer() as srv:
            c = srv.client()
            self.addCleanup(c.close)
            c.send_control(t="hello", proto=1, model="parakeet")
            err = c.recv_control()
            self.assertEqual(err["code"], "model_unavailable")
            self.assertTrue(err["fatal"])
            self.assertEqual(err["available"], ["counter"])

    def test_anything_before_hello_is_a_state_violation(self):
        with ExampleServer() as srv:
            c = srv.client()
            self.addCleanup(c.close)
            c.send_control(t="start", session_id="s1", sample_rate=16000)
            err = c.recv_control()
            self.assertEqual(err["code"], "state_violation")


class Session(unittest.TestCase):
    def _ready_client(self, srv: ExampleServer) -> WsClient:
        c = srv.client()
        self.addCleanup(c.close)
        c.send_control(t="hello", proto=1)
        self.assertEqual(c.recv_control()["t"], "hello_ack")
        self.assertEqual(c.recv_control()["t"], "ready")
        return c

    def test_full_session(self):
        with ExampleServer() as srv:
            c = self._ready_client(srv)
            c.send_control(t="start", session_id="s1", sample_rate=16000)
            # 1.0 s of silence, in 100 ms audio frames (§6.3's recommended range).
            for _ in range(10):
                c.send_frame(TYPE_AUDIO, b"\x00\x00" * 1600)
            c.send_control(t="stop", session_id="s1")

            final = c.recv_control()
            self.assertEqual(final["t"], "final")
            self.assertEqual(final["session_id"], "s1")
            self.assertEqual(final["utterance_id"], 1)
            self.assertIn("16000 samples", final["text"])
            # §8.2: a final is wholly stable, and stable_bytes counts UTF-8 bytes of `text`.
            self.assertEqual(final["stable_bytes"], len(final["text"].encode("utf-8")))

            stopped = c.recv_control()
            self.assertEqual(stopped["t"], "stopped")
            self.assertEqual(stopped["reason"], "eos")

    def test_cancel_produces_stopped_and_no_final(self):
        with ExampleServer() as srv:
            c = self._ready_client(srv)
            c.send_control(t="start", session_id="s2", sample_rate=16000)
            c.send_frame(TYPE_AUDIO, b"\x00\x00" * 1600)
            c.send_control(t="cancel", session_id="s2")
            # §10.4: after reading cancel, no further partial/final — but stopped always.
            stopped = c.recv_control()
            self.assertEqual(stopped["t"], "stopped")
            self.assertEqual(stopped["reason"], "cancelled")

    def test_stop_for_an_unknown_session_is_not_an_error(self):
        with ExampleServer() as srv:
            c = self._ready_client(srv)
            c.send_control(t="stop", session_id="never-started")
            stopped = c.recv_control()
            self.assertEqual(stopped["t"], "stopped")
            self.assertEqual(stopped["reason"], "unknown_session")

    def test_start_twice_is_a_state_violation(self):
        with ExampleServer() as srv:
            c = self._ready_client(srv)
            c.send_control(t="start", session_id="s1", sample_rate=16000)
            c.send_control(t="start", session_id="s2", sample_rate=16000)
            err = c.recv_control()
            self.assertEqual(err["code"], "state_violation")
            self.assertTrue(err["fatal"])

    def test_wrong_sample_rate_is_bad_message(self):
        with ExampleServer() as srv:
            c = self._ready_client(srv)
            c.send_control(t="start", session_id="s1", sample_rate=48000)
            self.assertEqual(c.recv_control()["code"], "bad_message")

    def test_odd_audio_payload_is_bad_message(self):
        with ExampleServer() as srv:
            c = self._ready_client(srv)
            c.send_control(t="start", session_id="s1", sample_rate=16000)
            c.send_frame(TYPE_AUDIO, b"\x00\x00\x00")
            self.assertEqual(c.recv_control()["code"], "bad_message")

    def test_audio_outside_a_session_is_a_state_violation(self):
        with ExampleServer() as srv:
            c = self._ready_client(srv)
            c.send_frame(TYPE_AUDIO, b"\x00\x00" * 160)
            self.assertEqual(c.recv_control()["code"], "state_violation")


class FramingFaults(unittest.TestCase):
    """L1/L2 faults must be reported at the layer that can see them (§1)."""

    def _ready_client(self, srv: ExampleServer) -> WsClient:
        c = srv.client()
        self.addCleanup(c.close)
        c.send_control(t="hello", proto=1)
        c.recv_control()
        c.recv_control()
        return c

    def test_text_message_is_unknown_frame_type(self):
        with ExampleServer() as srv:
            c = self._ready_client(srv)
            # §2.6.3: a text opcode is fatal even when the bytes would have decoded.
            body = json.dumps({"t": "stop", "session_id": "s1"}).encode("utf-8")
            c.send_ws(0x1, struct.pack("<I", 1 + len(body)) + bytes([TYPE_CONTROL]) + body)
            self.assertEqual(c.recv_control()["code"], "unknown_frame_type")

    def test_length_field_disagreeing_with_the_message_is_bad_frame(self):
        with ExampleServer() as srv:
            c = self._ready_client(srv)
            c.send_ws(0x2, struct.pack("<I", 99) + bytes([TYPE_CONTROL]) + b"{}")
            self.assertEqual(c.recv_control()["code"], "bad_frame")

    def test_zero_length_field_is_bad_frame(self):
        with ExampleServer() as srv:
            c = self._ready_client(srv)
            c.send_ws(0x2, struct.pack("<I", 0) + b"")
            self.assertEqual(c.recv_control()["code"], "bad_frame")

    def test_invalid_utf8_is_bad_utf8_never_replacement_characters(self):
        with ExampleServer() as srv:
            c = self._ready_client(srv)
            c.send_frame(TYPE_CONTROL, b'{"t":"\xff\xfe"}')
            self.assertEqual(c.recv_control()["code"], "bad_utf8")

    def test_valid_utf8_that_is_not_json_is_bad_json(self):
        with ExampleServer() as srv:
            c = self._ready_client(srv)
            c.send_frame(TYPE_CONTROL, b"not json at all")
            self.assertEqual(c.recv_control()["code"], "bad_json")

    def test_json_that_is_not_a_message_is_bad_message(self):
        with ExampleServer() as srv:
            c = self._ready_client(srv)
            c.send_frame(TYPE_CONTROL, b"[1,2,3]")
            self.assertEqual(c.recv_control()["code"], "bad_message")

    def test_unknown_message_type(self):
        with ExampleServer() as srv:
            c = self._ready_client(srv)
            c.send_control(t="transcribe_please")
            self.assertEqual(c.recv_control()["code"], "unknown_message")

    def test_unknown_frame_type_byte(self):
        with ExampleServer() as srv:
            c = self._ready_client(srv)
            c.send_frame(0x7F, b"whatever")
            self.assertEqual(c.recv_control()["code"], "unknown_frame_type")

    def test_fragmented_message_is_reassembled(self):
        """§2.6.3: continuation frames are the transport's business, not the protocol's."""
        with ExampleServer() as srv:
            c = self._ready_client(srv)
            body = json.dumps({"t": "start", "session_id": "s1", "sample_rate": 16000}).encode()
            frame = struct.pack("<I", 1 + len(body)) + bytes([TYPE_CONTROL]) + body
            head, tail = frame[:6], frame[6:]

            def masked(fin: bool, opcode: int, payload: bytes) -> bytes:
                mask = os.urandom(4)
                return (
                    bytes([(0x80 if fin else 0) | opcode, 0x80 | len(payload)])
                    + mask
                    + bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
                )

            c.sock.sendall(masked(False, 0x2, head) + masked(True, 0x0, tail))
            # A reassembled `start` is accepted; the proof is that `stop` then finds a session.
            c.send_control(t="stop", session_id="s1")
            stopped = c.recv_control()
            self.assertEqual(stopped["t"], "stopped")
            self.assertEqual(stopped["reason"], "eos")


class Concurrency(unittest.TestCase):
    def test_two_connections_are_independent_and_may_reuse_session_ids(self):
        """§2.7: session state is per connection; ids collide across clients and must not mix."""
        with ExampleServer() as srv:
            a, b = srv.client(), srv.client()
            self.addCleanup(a.close)
            self.addCleanup(b.close)
            for c in (a, b):
                c.send_control(t="hello", proto=1)
                c.recv_control()
                c.recv_control()
                c.send_control(t="start", session_id="s1", sample_rate=16000)

            a.send_frame(TYPE_AUDIO, b"\x00\x00" * 16000)  # 1.0 s
            b.send_frame(TYPE_AUDIO, b"\x00\x00" * 8000)   # 0.5 s
            a.send_control(t="stop", session_id="s1")
            b.send_control(t="stop", session_id="s1")

            self.assertIn("16000 samples", a.recv_control()["text"])
            self.assertIn("8000 samples", b.recv_control()["text"])

    def test_refusal_is_server_busy_in_words_not_silence(self):
        with ExampleServer(max_connections=1) as srv:
            a = srv.client()
            self.addCleanup(a.close)
            a.send_control(t="hello", proto=1)
            self.assertEqual(a.recv_control()["t"], "hello_ack")

            b = srv.client()
            self.addCleanup(b.close)
            b.send_control(t="hello", proto=1)
            err = b.recv_control()
            self.assertEqual(err["t"], "error")
            self.assertEqual(err["code"], "server_busy")
            self.assertTrue(err["fatal"])
            self.assertEqual(err["retry_after_ms"], 5000)


class Ping(unittest.TestCase):
    def test_ping_is_answered_with_pong_and_carries_no_protocol_meaning(self):
        with ExampleServer() as srv:
            c = srv.client()
            self.addCleanup(c.close)
            c.send_ws(0x9, b"are you there")
            opcode, payload = c.recv_ws()
            self.assertEqual(opcode, 0xA)
            self.assertEqual(payload, b"are you there")


if __name__ == "__main__":
    unittest.main()
