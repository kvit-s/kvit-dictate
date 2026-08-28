# Copyright 2026 Kvit
# Licensed under the Apache License, Version 2.0. See LICENSE-APACHE, or
# https://www.apache.org/licenses/LICENSE-2.0
r"""A minimal conforming kvit-dictate server, in the standard library and nothing else.

docs/protocol.md §15 walks through this file. It exists because a specification for outsiders
needs one thing an internal one does not: proof that somebody who has only the document can
build a working peer. Every line here is written against `docs/protocol.md` and nothing else —
no import from `kvit_asr`, no shared helper, no private knowledge.

**It does not recognise speech.** It accepts a session, counts the audio, and answers with a
`final` naming how much it heard::

    heard 2.4 s of audio (38400 samples)

That is deliberate. What it demonstrates is the part a server author gets wrong: the WebSocket
upgrade, the frame layout, the handshake, the ordering rules, and the committed-prefix
invariant. Swapping the placeholder in `transcribe()` for a real recogniser is the easy half.

Run it::

    python docs/examples/minimal_server.py                 # ws://127.0.0.1:10402
    python docs/examples/minimal_server.py --port 9000
    python docs/examples/minimal_server.py --host 0.0.0.0  # see the warning it prints

Then point the client's Server settings page at the URL it prints and press **Test**.

Threading: one thread per connection, which is what makes the concurrency rule of §2.7
observable. `--max-connections 1` makes it refuse a second one with `server_busy` so a client's
handling of that path can be exercised on purpose rather than by unplugging something.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import ipaddress
import json
import socket
import socketserver
import struct
import sys
import threading

# ─── docs/protocol.md §1.1 ────────────────────────────────────────────────────────────────────

PROTO_VERSION = 1
MAX_PAYLOAD = 65536
MIN_LENGTH_FIELD = 1
MAX_LENGTH_FIELD = MAX_PAYLOAD + 1
SAMPLE_RATE = 16000

TYPE_CONTROL = 0x01
TYPE_AUDIO = 0x02

SUBPROTOCOL = "kvit-dictate.v1"
# RFC 6455 §1.3. Verified against the RFC's own test vector in
# server/tests/test_minimal_server.py rather than trusted from memory: a wrong GUID here
# produces a handshake that looks perfect on both sides and that a real WebSocket client
# rejects with "invalid server response", which is a genuinely miserable thing to debug.
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

SERVER_ID = "minimal-example/1"
MODEL_ID = "counter"


class Fatal(Exception):
    """A protocol fault. `code` is a docs/protocol.md §11.3 code; the connection ends."""

    def __init__(self, code: str, msg: str, **extra: object) -> None:
        super().__init__(f"{code}: {msg}")
        self.code = code
        self.msg = msg
        self.extra = extra


# ══════════════════════════════════════════════════════════════════════════════════════════════
#  L0 — the WebSocket binding (docs/protocol.md §2.6)
# ══════════════════════════════════════════════════════════════════════════════════════════════


def read_exactly(sock: socket.socket, n: int) -> bytes:
    """`n` bytes or a clean end of stream. Never a short read that a caller might treat as one."""
    out = bytearray()
    while len(out) < n:
        chunk = sock.recv(n - len(out))
        if not chunk:
            raise ConnectionError("end of stream")
        out += chunk
    return bytes(out)


def websocket_handshake(sock: socket.socket) -> str:
    """Complete the RFC 6455 opening handshake. Returns the request path.

    §2.6.2: the client offers `kvit-dictate.v1`; echoing it is a SHOULD. This server echoes it
    when offered and stays silent when not, which is what a client is required to tolerate.
    """
    raw = bytearray()
    while b"\r\n\r\n" not in raw:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("end of stream during upgrade")
        raw += chunk
        if len(raw) > 65536:  # a request this large is not a WebSocket upgrade
            raise ConnectionError("upgrade request too large")

    head, _, _rest = bytes(raw).partition(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    request_line = lines[0].split(" ")
    path = request_line[1] if len(request_line) > 1 else "/"

    headers: dict[str, str] = {}
    for line in lines[1:]:
        name, _, value = line.partition(":")
        headers[name.strip().lower()] = value.strip()

    key = headers.get("sec-websocket-key")
    if headers.get("upgrade", "").lower() != "websocket" or not key:
        sock.sendall(b"HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n")
        raise ConnectionError("not a WebSocket upgrade")

    accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode("ascii")).digest()).decode()
    offered = [t.strip() for t in headers.get("sec-websocket-protocol", "").split(",") if t.strip()]

    response = [
        "HTTP/1.1 101 Switching Protocols",
        "Upgrade: websocket",
        "Connection: Upgrade",
        f"Sec-WebSocket-Accept: {accept}",
    ]
    if SUBPROTOCOL in offered:
        response.append(f"Sec-WebSocket-Protocol: {SUBPROTOCOL}")
    sock.sendall(("\r\n".join(response) + "\r\n\r\n").encode("ascii"))
    return path


def ws_recv_message(sock: socket.socket) -> tuple[int, bytes] | None:
    """One reassembled WebSocket message as `(opcode, payload)`. `None` on a Close frame.

    Handles continuation frames, because §2.6.3 permits them: fragmentation is the transport's
    business and a receiver reassembles before decoding. Control frames (ping/pong/close) may be
    interleaved between fragments, so they are handled inside the loop rather than after it.
    """
    first_opcode: int | None = None
    body = bytearray()

    while True:
        b0, b1 = read_exactly(sock, 2)
        fin = bool(b0 & 0x80)
        opcode = b0 & 0x0F
        masked = bool(b1 & 0x80)
        length = b1 & 0x7F

        if length == 126:
            (length,) = struct.unpack("!H", read_exactly(sock, 2))
        elif length == 127:
            (length,) = struct.unpack("!Q", read_exactly(sock, 8))

        # RFC 6455 §5.1: every frame from a client MUST be masked.
        if not masked:
            raise ConnectionError("unmasked frame from client")
        # A ceiling so a hostile or broken peer cannot ask for an arbitrary allocation. §3.3's
        # limit is on the protocol frame; this is the transport's own sanity bound.
        if length > MAX_PAYLOAD + 64 or len(body) + length > MAX_PAYLOAD + 64:
            raise ConnectionError("message exceeds the maximum frame on the wire")

        mask = read_exactly(sock, 4)
        payload = bytearray(read_exactly(sock, length))
        for i in range(length):
            payload[i] ^= mask[i & 3]

        if opcode == 0x8:  # close
            return None
        if opcode == 0x9:  # ping -> pong, same payload
            ws_send(sock, 0xA, bytes(payload))
            continue
        if opcode == 0xA:  # pong; nothing to do
            continue

        if opcode == 0x0:  # continuation
            if first_opcode is None:
                raise ConnectionError("continuation frame with nothing to continue")
        else:
            if first_opcode is not None:
                raise ConnectionError("new data frame inside a fragmented message")
            first_opcode = opcode

        body += payload
        if fin:
            return first_opcode, bytes(body)


def ws_send(sock: socket.socket, opcode: int, payload: bytes) -> None:
    """One unfragmented, unmasked frame. Server-to-client frames are never masked."""
    header = bytearray([0x80 | opcode])
    n = len(payload)
    if n < 126:
        header.append(n)
    elif n < 65536:
        header.append(126)
        header += struct.pack("!H", n)
    else:
        header.append(127)
        header += struct.pack("!Q", n)
    sock.sendall(bytes(header) + payload)


# ══════════════════════════════════════════════════════════════════════════════════════════════
#  L1/L2 — framing and encoding (docs/protocol.md §3, §4)
# ══════════════════════════════════════════════════════════════════════════════════════════════


def decode_frame(message: bytes) -> tuple[int, bytes]:
    """`(type, payload)` from one WebSocket message. §2.6.3: exactly one frame per message."""
    if len(message) < 5:
        raise Fatal("bad_frame", f"message of {len(message)} bytes cannot hold a frame header")
    (length,) = struct.unpack("<I", message[:4])
    if length < MIN_LENGTH_FIELD:
        raise Fatal("bad_frame", f"length {length} below MIN_LENGTH_FIELD")
    if length > MAX_LENGTH_FIELD:
        raise Fatal("frame_too_large", f"length {length} above MAX_LENGTH_FIELD")
    # §3.1: length covers the type byte, so the whole frame is 4 + length bytes.
    if len(message) != 4 + length:
        raise Fatal(
            "bad_frame",
            f"length field says {4 + length} bytes, message is {len(message)}",
        )
    return message[4], message[5:]


def encode_frame(ftype: int, payload: bytes) -> bytes:
    """§3: u32 little-endian length (covering the type byte), type byte, payload."""
    return struct.pack("<I", 1 + len(payload)) + bytes([ftype]) + payload


def decode_control(payload: bytes) -> dict:
    """§4: UTF-8, then a JSON object. Neither failure may be papered over."""
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError as exc:
        # §4.1 and §1: no U+FFFD substitution to rescue a malformed control payload.
        raise Fatal("bad_utf8", f"control payload is not valid UTF-8: {exc}") from None
    try:
        msg = json.loads(text)
    except ValueError as exc:
        raise Fatal("bad_json", f"control payload is not JSON: {exc}") from None
    if not isinstance(msg, dict) or not isinstance(msg.get("t"), str):
        raise Fatal("bad_message", "control payload is not an object with a string `t`")
    return msg


def send_control(sock: socket.socket, msg: dict) -> None:
    payload = json.dumps(msg, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    ws_send(sock, 0x2, encode_frame(TYPE_CONTROL, payload))


# ══════════════════════════════════════════════════════════════════════════════════════════════
#  L3 — the session (docs/protocol.md §5–§10)
# ══════════════════════════════════════════════════════════════════════════════════════════════


def transcribe(sample_count: int) -> str:
    """Where a real recogniser goes. This one reports what it was given.

    A real implementation replaces this with a decoder and must honour §8: `partial.text` is
    **cumulative** for the utterance, and `stable_bytes` is a monotonically non-decreasing count
    of UTF-8 bytes of `text` that will never change again. Emitting a `stable_bytes` that later
    shrinks, or revising bytes below it, is a `stability_violation` and the client will abort the
    session — which is the correct outcome, because the alternative is rewriting words already in
    somebody's document.
    """
    seconds = sample_count / SAMPLE_RATE
    return f"heard {seconds:.1f} s of audio ({sample_count} samples)"


class Connection:
    """One client. Independent of every other connection (§2.7)."""

    def __init__(self, sock: socket.socket, peer: str, catalogue: bool) -> None:
        self.sock = sock
        self.peer = peer
        self.catalogue = catalogue
        self.session_id: str | None = None
        self.samples = 0
        self.utterance = 0
        self.seq = 0

    # ── outbound helpers ──────────────────────────────────────────────────────────────────

    def send(self, **msg: object) -> None:
        send_control(self.sock, msg)

    def send_error(self, code: str, fatal: bool, msg: str, **extra: object) -> None:
        body: dict = {"t": "error", "code": code, "fatal": fatal, "msg": msg}
        if self.session_id and not fatal:
            body["session_id"] = self.session_id
        body.update(extra)
        send_control(self.sock, body)

    def next_seq(self) -> int:
        # §7.4: monotonically increasing within the session, one space for partial and final.
        self.seq += 1
        return self.seq

    # ── the loop ──────────────────────────────────────────────────────────────────────────

    def run(self) -> None:
        try:
            self.handshake()
            while True:
                message = ws_recv_message(self.sock)
                if message is None:
                    return  # orderly Close
                opcode, data = message
                if opcode != 0x2:
                    # §2.6.3: text frames are fatal even when their bytes would decode.
                    raise Fatal("unknown_frame_type", "control frames must be binary messages")
                ftype, payload = decode_frame(data)
                if ftype == TYPE_CONTROL:
                    self.on_control(decode_control(payload))
                elif ftype == TYPE_AUDIO:
                    self.on_audio(payload)
                else:
                    raise Fatal("unknown_frame_type", f"frame type 0x{ftype:02x}")
        except Fatal as exc:
            self.send_error(exc.code, True, exc.msg, **exc.extra)
            self.close(1000)
        except (ConnectionError, OSError):
            pass  # §10.3 from the other side: the peer is gone, there is nobody to tell
        finally:
            try:
                self.sock.close()
            except OSError:
                pass

    def close(self, status: int) -> None:
        try:
            ws_send(self.sock, 0x8, struct.pack("!H", status))
        except OSError:
            pass

    # ── §5: handshake ─────────────────────────────────────────────────────────────────────

    def handshake(self) -> None:
        message = ws_recv_message(self.sock)
        if message is None:
            raise ConnectionError("closed before hello")
        opcode, data = message
        if opcode != 0x2:
            raise Fatal("unknown_frame_type", "control frames must be binary messages")
        ftype, payload = decode_frame(data)
        if ftype != TYPE_CONTROL:
            raise Fatal("state_violation", "audio before hello")

        msg = decode_control(payload)
        if msg["t"] != "hello":
            raise Fatal("state_violation", f"expected hello, got {msg['t']!r}")
        if msg.get("proto") != PROTO_VERSION:
            # §5: never downgrade silently, never proceed on a best-effort basis.
            raise Fatal(
                "proto_unsupported",
                f"this server speaks proto {PROTO_VERSION}",
                supported=[PROTO_VERSION],
            )

        # §5.1: an unknown requested model is refused, never silently substituted.
        requested = msg.get("model")
        if requested is not None and requested != MODEL_ID:
            raise Fatal(
                "model_unavailable",
                f"no model named {requested!r}",
                available=[MODEL_ID],
            )

        ack: dict = {"t": "hello_ack", "proto": PROTO_VERSION, "server": SERVER_ID}
        if self.catalogue:
            ack["models"] = [
                {
                    "id": MODEL_ID,
                    "display_name": "Audio counter (not a recogniser)",
                    "mode": "native-streaming",
                    "context_supported": False,
                }
            ]
        send_control(self.sock, ack)

        # This server has nothing to load, so `ready` follows immediately and no `loading`
        # heartbeat is needed. A real one emits `loading` at least every LOADING_HEARTBEAT_MS
        # while a model is coming up, because the client's READY_TIMEOUT_MS restarts on each.
        self.send(
            t="ready",
            model=MODEL_ID,
            warm_ms=0,
            mode="native-streaming",
            context_supported=False,
        )

    # ── §6, §10: routing ──────────────────────────────────────────────────────────────────

    def on_control(self, msg: dict) -> None:
        t = msg["t"]
        if t == "start":
            self.on_start(msg)
        elif t in ("stop", "cancel"):
            self.on_stop(msg, cancelled=(t == "cancel"))
        elif t == "discontinuity":
            # §6.1: the client is telling us its audio has a gap. A real backend flushes and
            # resets its decoder state here. Acknowledging is not required.
            pass
        elif t == "hello":
            raise Fatal("state_violation", "a second hello on one connection (§5.1)")
        elif t == "error":
            # §11: a non-fatal error from the client (e.g. stability_violation) ends the
            # session but not the connection. A fatal one means it is already tearing down.
            if msg.get("fatal"):
                raise ConnectionError("peer reported a fatal error")
            self.session_id = None
        else:
            raise Fatal("unknown_message", f"unknown message type {t!r}")

    def on_start(self, msg: dict) -> None:
        if self.session_id is not None:
            raise Fatal("state_violation", "start while a session is open (§7.2)")
        sid = msg.get("session_id")
        if not isinstance(sid, str) or not sid:
            raise Fatal("bad_message", "start without a session_id")
        if msg.get("sample_rate") != SAMPLE_RATE:
            raise Fatal("bad_message", f"sample_rate must be {SAMPLE_RATE}")
        self.session_id = sid
        self.samples = 0
        self.utterance = 1
        self.seq = 0

    def on_audio(self, payload: bytes) -> None:
        if self.session_id is None:
            raise Fatal("state_violation", "audio outside a session")
        if len(payload) % 2:
            # §6.3: 16-bit samples, so an odd payload is malformed and not roundable.
            raise Fatal("bad_message", "audio payload has an odd byte count")
        self.samples += len(payload) // 2

        # A real streaming backend emits `partial` here as hypotheses firm up. This one has
        # nothing to say until the session ends, and saying nothing is conforming: `partial` is
        # optional, and §6.2 lets a server produce one `final` at the endpoint instead.

    def on_stop(self, msg: dict, *, cancelled: bool) -> None:
        sid = msg.get("session_id")
        if self.session_id is None or sid != self.session_id:
            # §10.5, §6.1: stop/cancel are idempotent and may name a session we never had.
            # That is not an error — the client's teardown runs on crash paths too.
            send_control(self.sock, {"t": "stopped", "session_id": sid, "reason": "unknown_session"})
            return

        if not cancelled and self.samples > 0:
            text = transcribe(self.samples)
            self.send(
                t="final",
                session_id=self.session_id,
                utterance_id=self.utterance,
                seq=self.next_seq(),
                text=text,
                stable_bytes=len(text.encode("utf-8")),  # §8.2: a final is wholly stable
            )
        # §10.4: after reading `cancel`, no further partial/final for that session — but
        # `stopped` is always emitted, cancelled or not.
        self.send(
            t="stopped",
            session_id=self.session_id,
            reason="cancelled" if cancelled else "eos",
        )
        self.session_id = None


# ══════════════════════════════════════════════════════════════════════════════════════════════
#  Listener
# ══════════════════════════════════════════════════════════════════════════════════════════════


class _Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


class _Handler(socketserver.BaseRequestHandler):
    max_connections = 0  # 0 = unlimited; set from argv
    advertise_models = True
    _live = 0
    _lock = threading.Lock()

    def handle(self) -> None:
        sock: socket.socket = self.request
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        peer = f"{self.client_address[0]}:{self.client_address[1]}"
        try:
            websocket_handshake(sock)
        except (ConnectionError, OSError) as exc:
            print(f"[{peer}] upgrade failed: {exc}", file=sys.stderr)
            return

        with _Handler._lock:
            admitted = _Handler.max_connections == 0 or _Handler._live < _Handler.max_connections
            if admitted:
                _Handler._live += 1
        if not admitted:
            # §2.7: refuse in words, after reading hello, instead of hello_ack. Silence and a
            # connection that never becomes ready both present to the user as "it hangs".
            print(f"[{peer}] refused: at the connection limit", file=sys.stderr)
            try:
                message = ws_recv_message(sock)  # read the hello we are about to refuse
                if message is not None:
                    send_control(
                        sock,
                        {
                            "t": "error",
                            "code": "server_busy",
                            "fatal": True,
                            "msg": "this server serves one connection at a time",
                            "retry_after_ms": 5000,
                        },
                    )
                ws_send(sock, 0x8, struct.pack("!H", 1000))
            except (ConnectionError, OSError):
                pass
            sock.close()
            return

        print(f"[{peer}] connected", file=sys.stderr)
        try:
            Connection(sock, peer, _Handler.advertise_models).run()
        finally:
            with _Handler._lock:
                _Handler._live -= 1
            print(f"[{peer}] closed", file=sys.stderr)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="A minimal conforming kvit-dictate server (docs/protocol.md §15). "
        "It counts audio; it does not recognise speech.",
    )
    ap.add_argument("--host", default="127.0.0.1", help="Bind address (default: loopback).")
    ap.add_argument("--port", type=int, default=10402)
    ap.add_argument(
        "--max-connections",
        type=int,
        default=0,
        help="0 = unlimited. Set to 1 to exercise a client's `server_busy` handling (§2.7).",
    )
    ap.add_argument(
        "--no-catalogue",
        action="store_true",
        help="Omit hello_ack.models[], so a client's free-text model fallback is exercised.",
    )
    args = ap.parse_args(argv)

    _Handler.max_connections = max(0, args.max_connections)
    _Handler.advertise_models = not args.no_catalogue

    # §2.8: this binding has no authentication and no confidentiality. Binding to anything
    # routable is a decision the operator has to make on purpose, so say so when they do.
    try:
        routable = not ipaddress.ip_address(args.host).is_loopback
    except ValueError:
        routable = True
    if routable:
        print(
            f"WARNING: listening on {args.host}, which is not loopback. There is no "
            "authentication\n         and ws:// is unencrypted (docs/protocol.md §2.8): anyone "
            "who can reach this\n         port can send audio, and anyone on the path can read "
            "it.",
            file=sys.stderr,
        )

    with _Server((args.host, args.port), _Handler) as srv:
        host = args.host if ":" not in args.host else f"[{args.host}]"
        print(f"listening on ws://{host}:{args.port}/", file=sys.stderr)
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            print("stopping", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
