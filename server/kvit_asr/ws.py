# Copyright 2026 Kvit
# Licensed under the Apache License, Version 2.0. See LICENSE-APACHE, or
# https://www.apache.org/licenses/LICENSE-2.0
r"""The WebSocket binding (docs/protocol.md §2.6), in the standard library.

This is what replaced `transport.py` for shipped use. That file is 800 lines of ctypes against
kernel32 and it is still here, because the bench still spawns its subject locally and a named
pipe is the right channel for that. But the app no longer launches a child process: the endpoint
is a URL somebody configures (release-plan.md §1), and reaching it takes a socket.

The trade is stark and worth stating: 800 lines of hand-bound Win32 became about 200 lines of
`socket` and `struct`, **and the result is cross-platform for free**. Nothing in this module is
Windows-specific. A reference server can now run on the Linux box the GPU is actually in.

─── What is deliberately not here ───────────────────────────────────────────────────────────

* **No third-party WebSocket library.** `server/README.md` §1 makes the stdlib-only core
  load-bearing: the whole client can be exercised on a bare interpreter with nothing installed,
  and an install step between "download" and "does it work" is the thing that loses people.
* **No TLS.** docs §2.8: this binding has no confidentiality in v1 and says so rather than
  implying otherwise. An operator who needs `wss://` puts a reverse proxy in front, which is one
  configuration file and does not put certificate handling in the audio path.
* **No masking on send.** RFC 6455 §5.1: a server MUST NOT mask. A client MUST, and this module
  rejects an unmasked client frame rather than tolerating it — a client that does not mask is
  one that has not read the RFC, and the next thing it gets wrong will be less visible.

─── Known limitation, recorded rather than discovered ───────────────────────────────────────

`serve_forever` gives each accepted connection its own `Server`, and each `Server` loads its own
backend. For the `mock` backend that costs nothing. For a real model it means one model load per
connection, which is why `--max-connections` defaults to 1 and a second client is refused with
`server_busy` (docs §2.7) rather than quietly given a second copy of a 3 GB model. Sharing one
loaded backend across connections is a real feature and it belongs to the reference server that
release-plan.md §1.5 defers, not here.
"""

from __future__ import annotations

import base64
import hashlib
import socket
import socketserver
import struct
import threading
from typing import Any, Callable

from . import protocol as proto

#: RFC 6455 §1.3. Checked against the RFC's own test vector in `server/tests/test_ws.py` rather
#: than trusted from memory: a wrong value here produces a handshake both endpoints believe
#: succeeded and that every real client rejects as an invalid server response.
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

#: docs §2.6.2. Echoed when a client offers it; never required of one.
SUBPROTOCOL = "kvit-dictate.v1"

OPCODE_CONTINUATION = 0x0
OPCODE_TEXT = 0x1
OPCODE_BINARY = 0x2
OPCODE_CLOSE = 0x8
OPCODE_PING = 0x9
OPCODE_PONG = 0xA

CLOSE_NORMAL = 1000

#: A ceiling on one reassembled message, applied before the framing layer sees it. §3.3 bounds
#: the protocol frame; this bounds what a hostile or broken peer can make us allocate while we
#: work out that the frame is illegal.
MAX_MESSAGE_BYTES = int(proto.MAX_FRAME_ON_WIRE)

_RECV_CHUNK = 65536


class WebSocketError(OSError):
    """The socket is unusable. Not a protocol error: there is nobody to tell."""


class UpgradeRejected(WebSocketError):
    """The HTTP request was not a WebSocket upgrade. A 400 has already been written."""


# ─── The opening handshake (docs §2.6.2) ──────────────────────────────────────────────────────


def compute_accept(key: str) -> str:
    """`Sec-WebSocket-Accept` for a client's `Sec-WebSocket-Key`."""
    return base64.b64encode(hashlib.sha1((key + WS_GUID).encode("ascii")).digest()).decode("ascii")


def read_http_head(sock: socket.socket, limit: int = 65536) -> bytes:
    """Everything up to and including the blank line that ends the request headers."""
    raw = bytearray()
    while b"\r\n\r\n" not in raw:
        chunk = sock.recv(4096)
        if not chunk:
            raise WebSocketError("end of stream during the upgrade request")
        raw += chunk
        if len(raw) > limit:
            raise UpgradeRejected("upgrade request headers too large")
    return bytes(raw)


def parse_http_head(raw: bytes) -> tuple[str, dict[str, str]]:
    """`(path, headers)`. Header names are lowercased; values are stripped."""
    head, _, _rest = raw.partition(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    parts = lines[0].split(" ")
    path = parts[1] if len(parts) > 1 else "/"
    headers: dict[str, str] = {}
    for line in lines[1:]:
        name, _, value = line.partition(":")
        if name:
            headers[name.strip().lower()] = value.strip()
    return path, headers


def accept_upgrade(sock: socket.socket) -> str:
    """Complete the opening handshake, or write a 400 and raise. Returns the request path."""
    path, headers = parse_http_head(read_http_head(sock))

    key = headers.get("sec-websocket-key")
    upgrade = headers.get("upgrade", "").lower()
    if upgrade != "websocket" or not key:
        # A plain HTTP request against the WebSocket port, which is a person checking the URL in
        # a browser more often than it is anything else. Say what this is rather than hanging up.
        body = (
            b"kvit-dictate ASR endpoint. This is a WebSocket endpoint "
            b"(docs/protocol.md section 2.6), not an HTTP API.\n"
        )
        sock.sendall(
            b"HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain; charset=utf-8\r\n"
            b"Content-Length: " + str(len(body)).encode("ascii") + b"\r\n"
            b"Connection: close\r\n\r\n" + body
        )
        raise UpgradeRejected("not a WebSocket upgrade")

    offered = [t.strip() for t in headers.get("sec-websocket-protocol", "").split(",") if t.strip()]
    response = [
        "HTTP/1.1 101 Switching Protocols",
        "Upgrade: websocket",
        "Connection: Upgrade",
        f"Sec-WebSocket-Accept: {compute_accept(key)}",
    ]
    # §2.6.2: echoing is a SHOULD, and echoing something the client did *not* offer is fatal on
    # its side. So it is echoed only when offered, and never invented.
    if SUBPROTOCOL in offered:
        response.append(f"Sec-WebSocket-Protocol: {SUBPROTOCOL}")
    sock.sendall(("\r\n".join(response) + "\r\n\r\n").encode("ascii"))
    return path


# ─── Frames (docs §2.6.3) ─────────────────────────────────────────────────────────────────────


def encode_frame(opcode: int, payload: bytes) -> bytes:
    """One unfragmented, unmasked frame. RFC 6455 §5.1: a server never masks."""
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
    return bytes(header) + payload


class WebSocketConnection:
    """One upgraded socket. Reads reassembled messages, writes whole ones.

    Send and receive are used from different threads — `Server` writes from its decode thread
    while the reader is parked — so `_send_lock` serialises writes. A WebSocket frame that
    interleaves with another is not a slow frame, it is a corrupt stream.
    """

    def __init__(self, sock: socket.socket) -> None:
        self._sock = sock
        self._send_lock = threading.Lock()
        self._closed = False
        self._buf = bytearray()

    # -- reading ------------------------------------------------------------------------------

    def _read_exactly(self, n: int) -> bytes:
        while len(self._buf) < n:
            chunk = self._sock.recv(max(_RECV_CHUNK, n - len(self._buf)))
            if not chunk:
                raise WebSocketError("end of stream")
            self._buf += chunk
        out = bytes(self._buf[:n])
        del self._buf[:n]
        return out

    def recv_message(self) -> tuple[int, bytes] | None:
        """One reassembled data message as `(opcode, payload)`, or None on Close.

        Continuation frames are reassembled here because §2.6.3 makes fragmentation the
        transport's business: a receiver puts the message back together before decoding. Control
        frames may be interleaved between fragments, so ping/pong/close are handled inside the
        loop rather than after it.
        """
        first_opcode: int | None = None
        body = bytearray()

        while True:
            b0, b1 = self._read_exactly(2)
            fin = bool(b0 & 0x80)
            opcode = b0 & 0x0F
            masked = bool(b1 & 0x80)
            length = b1 & 0x7F
            if length == 126:
                (length,) = struct.unpack("!H", self._read_exactly(2))
            elif length == 127:
                (length,) = struct.unpack("!Q", self._read_exactly(8))

            if not masked:
                # RFC 6455 §5.1. Refused rather than tolerated: a client that does not mask has
                # not read the RFC, and the next thing it gets wrong will be less visible.
                raise WebSocketError("a client frame arrived unmasked (RFC 6455 §5.1)")
            if length > MAX_MESSAGE_BYTES or len(body) + length > MAX_MESSAGE_BYTES:
                raise WebSocketError("message exceeds MAX_FRAME_ON_WIRE")

            mask = self._read_exactly(4)
            payload = bytearray(self._read_exactly(length))
            for i in range(length):
                payload[i] ^= mask[i & 3]

            if opcode == OPCODE_CLOSE:
                return None
            if opcode == OPCODE_PING:
                self.send(OPCODE_PONG, bytes(payload))
                continue
            if opcode == OPCODE_PONG:
                continue

            if opcode == OPCODE_CONTINUATION:
                if first_opcode is None:
                    raise WebSocketError("continuation frame with nothing to continue")
            else:
                if first_opcode is not None:
                    raise WebSocketError("a new data frame inside a fragmented message")
                first_opcode = opcode

            body += payload
            if fin:
                return first_opcode, bytes(body)

    # -- writing ------------------------------------------------------------------------------

    def send(self, opcode: int, payload: bytes) -> None:
        with self._send_lock:
            if self._closed:
                raise WebSocketError("the WebSocket is closed")
            try:
                self._sock.sendall(encode_frame(opcode, payload))
            except OSError as exc:
                raise WebSocketError(str(exc)) from None

    def send_binary(self, payload: bytes) -> None:
        self.send(OPCODE_BINARY, payload)

    def close(self, status: int = CLOSE_NORMAL) -> None:
        """Best-effort orderly close (§2.6.4), then the socket. Idempotent."""
        with self._send_lock:
            if self._closed:
                return
            self._closed = True
            try:
                self._sock.sendall(encode_frame(OPCODE_CLOSE, struct.pack("!H", status)))
            except OSError:
                pass
        try:
            self._sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self._sock.close()
        except OSError:
            pass


# ─── The Channel `Server` codes against ───────────────────────────────────────────────────────


class WebSocketChannel:
    """`server.Channel` over a WebSocket. One protocol frame per binary message (§2.6.3).

    `Server` wants a byte stream, and this is message-oriented, so `read()` hands back one
    message's bytes at a time and lets the FrameReader on the other side reassemble. The length
    prefix is validated first: §2.6.3 keeps it precisely so that one decoder serves both
    bindings, and a decoder that ignores a field it is given will be wrong the first time the
    field matters — two frames packed into one message would otherwise decode as a legal stream
    and surface later as garbled text.
    """

    def __init__(self, conn: WebSocketConnection) -> None:
        self._conn = conn
        self._pending = bytearray()
        self._eof = False
        self._cancelled = False

    def read(self, max_bytes: int = _RECV_CHUNK) -> bytes:
        while not self._pending:
            if self._eof or self._cancelled:
                return b""
            try:
                message = self._conn.recv_message()
            except WebSocketError:
                self._eof = True
                return b""
            if message is None:
                self._eof = True
                return b""
            opcode, data = message
            if opcode == OPCODE_TEXT:
                # §2.6.3, and fatal even though these bytes would have decoded: a text message
                # means the peer thinks this is JSON-over-WebSocket, and half its audio frames
                # will arrive mangled by a UTF-8 round trip.
                raise proto.ProtocolError(
                    proto.ErrorCode.unknown_frame_type,
                    "a text message arrived; this protocol is binary only "
                    "(docs/protocol.md §2.6.3)",
                    fatal=True,
                )
            if not _length_agrees(data):
                raise proto.ProtocolError(
                    proto.ErrorCode.bad_frame,
                    "one WebSocket message must hold exactly one frame: the length field and "
                    "the message size disagree (docs/protocol.md §2.6.3)",
                    fatal=True,
                )
            self._pending += data
        take = min(max_bytes, len(self._pending))
        out = bytes(self._pending[:take])
        del self._pending[:take]
        return out

    def write(self, data: bytes) -> None:
        try:
            self._conn.send_binary(data)
        except WebSocketError as exc:
            raise ConnectionError(str(exc)) from None

    def close(self) -> None:
        self._conn.close()

    def cancel(self) -> None:
        """Unblock a reader. Closing the socket is the only way to do that here."""
        self._cancelled = True
        self._conn.close()


def _length_agrees(message: bytes) -> bool:
    if len(message) < 5:
        return False
    (length,) = struct.unpack("<I", message[:4])
    return length + 4 == len(message)


# ─── Listener ─────────────────────────────────────────────────────────────────────────────────


class _ThreadingServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def serve_forever(
    host: str,
    port: int,
    handler: Callable[[WebSocketChannel, str], None],
    *,
    max_connections: int = 1,
    log: Any = None,
    on_refused: Callable[[WebSocketConnection], None] | None = None,
    ready: threading.Event | None = None,
) -> None:
    """Accept connections and hand each one to `handler(channel, peer)` on its own thread.

    `max_connections` of 0 means unlimited. Above the limit the connection is **refused in
    words** (§2.7): `on_refused` is called with the upgraded connection so it can read the
    client's `hello` and answer `server_busy`. Silence, and a connection that never becomes
    ready, both present to the user as "it hangs".
    """
    live = 0
    lock = threading.Lock()

    class _Handler(socketserver.BaseRequestHandler):
        def handle(self) -> None:
            nonlocal live
            sock: socket.socket = self.request
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            peer = f"{self.client_address[0]}:{self.client_address[1]}"
            try:
                accept_upgrade(sock)
            except (WebSocketError, OSError) as exc:
                if log is not None:
                    log.info("upgrade_rejected", peer=peer, reason=str(exc))
                try:
                    sock.close()
                except OSError:
                    pass
                return

            conn = WebSocketConnection(sock)
            with lock:
                admitted = max_connections == 0 or live < max_connections
                if admitted:
                    live += 1
            if not admitted:
                if log is not None:
                    log.warning("connection_refused", peer=peer, limit=max_connections)
                if on_refused is not None:
                    try:
                        on_refused(conn)
                    except (WebSocketError, OSError):
                        pass
                conn.close()
                return

            try:
                handler(WebSocketChannel(conn), peer)
            finally:
                with lock:
                    live -= 1
                conn.close()

    with _ThreadingServer((host, port), _Handler) as srv:
        if log is not None:
            bound_host, bound_port = srv.server_address[:2]
            log.info("listening", url=f"ws://{bound_host}:{bound_port}/")
        if ready is not None:
            ready.set()
        srv.serve_forever()
