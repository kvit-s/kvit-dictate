# Copyright 2026 Kvit
# Licensed under the Apache License, Version 2.0. See LICENSE-APACHE, or
# https://www.apache.org/licenses/LICENSE-2.0
"""Wire protocol codec: framing, message catalogue, encode/decode, offsets. **Pure — no I/O.**

Implements docs/protocol.md (proto 1) in full. That document is normative; where this module and
that document disagree, the document wins and this module is the bug. Section references below
point into it. The C++ peer's view of the same contract is src/protocol.h — field names and enum
spellings here were taken from both and cross-checked.

Nothing in this file performs I/O, touches ctypes, or imports anything outside the stdlib
(`json`, `unicodedata`, `enum`, `dataclasses`). That is what makes the framing and message layers
testable against tests/fixtures/protocol/ without a pipe, a child process, or an event loop.

─── Roles, restated because it is the most confusing thing in the spec ───────────────────────

`kvit-dictate.exe` is the **pipe server** (it creates the instance) but the **protocol client**
(it drives sessions). This Python process is the **pipe client** but the **protocol server**.
Throughout this module, "Client" and "Server" mean the *protocol* roles (docs/protocol.md §1).

─── The one bug this file is most likely to have ─────────────────────────────────────────────

Every offset that crosses the wire is a **UTF-8 byte offset**, and Python strings are sequences
of code points. `len(text)` is therefore never the right answer for `stable_bytes`, and
`text[:stable_bytes]` is never the right slice. Every conversion in this file goes through
`utf8_len()` / `byte_offset_of_index()` / `index_of_byte_offset()`, which exist so that the
mismatch has exactly one place to be wrong instead of forty.
"""

from __future__ import annotations

import json
import unicodedata
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Callable, Iterable, Iterator, Sequence

__all__ = [
    # constants
    "PROTO_VERSION", "MAX_PAYLOAD", "MIN_LENGTH_FIELD", "MAX_LENGTH_FIELD", "MAX_FRAME_ON_WIRE",
    "FRAME_HEADER_BYTES", "SAMPLE_RATE", "CHANNELS", "AUDIO_QUEUE_MAX_MS", "CONTROL_QUEUE_MAX",
    "WRITE_DEADLINE_MS", "CONNECT_TIMEOUT_MS", "HELLO_TIMEOUT_MS", "LOADING_HEARTBEAT_MS",
    "READY_TIMEOUT_MS", "MAX_CONTEXT_ITEMS", "MAX_CONTEXT_ITEM_BYTES", "MAX_SESSION_ID_BYTES",
    # framing
    "FrameType", "Frame", "FrameReader", "encode_frame", "encode_audio_frame",
    "encode_control_frame", "decode_audio_payload", "pcm_bytes_to_ms",
    # errors
    "ErrorCode", "ProtocolError", "WIRE_ERROR_CODES", "NON_FATAL_ERROR_CODES", "ModelInfo",
    # messages
    "Message", "Hello", "Start", "Stop", "Cancel", "Discontinuity", "HelloAck", "Loading",
    "Ready", "Partial", "Final", "Stopped", "ErrorMessage", "StreamingMode",
    "decode_control", "encode_control", "strict_json_loads",
    # text / offsets
    "utf8_len", "byte_offset_of_index", "index_of_byte_offset", "is_code_point_boundary",
    "grapheme_boundaries", "is_grapheme_boundary", "assert_grapheme_boundary",
    "floor_grapheme", "count_graphemes", "is_valid_stable_offset", "UNICODE_VERSION",
    "UNICODE_VERSION_NOTE",
    # validators
    "is_valid_session_id", "SeqVerdict", "classify_seq", "StabilityCheck", "check_stability",
    # session model
    "ConnState", "Verdict", "ReceiverState", "InboundOutcome", "classify_inbound",
    "apply_outbound",
]

# ─── §1.1 Constants ───────────────────────────────────────────────────────────────────────────
#
# Normative. Nothing may hard-code them a second time.

PROTO_VERSION = 1

MAX_PAYLOAD = 65536          # 64 KiB
MIN_LENGTH_FIELD = 1         # length covers the type byte, so 0 is structurally impossible
MAX_LENGTH_FIELD = 65537     # MAX_PAYLOAD + 1 type byte
MAX_FRAME_ON_WIRE = 65541    # 4 + MAX_LENGTH_FIELD
FRAME_HEADER_BYTES = 4       # the u32 length prefix only

SAMPLE_RATE = 16000
CHANNELS = 1
BYTES_PER_SAMPLE = 2

AUDIO_QUEUE_MAX_MS = 3000
CONTROL_QUEUE_MAX = 256
WRITE_DEADLINE_MS = 5000
CONNECT_TIMEOUT_MS = 15000
HELLO_TIMEOUT_MS = 5000
LOADING_HEARTBEAT_MS = 5000
READY_TIMEOUT_MS = 180000

# Recommended audio payload window (§6.3). Not enforced — a conforming peer may send anything
# from 0 to MAX_PAYLOAD — but a sender outside this band is doing something worth logging.
AUDIO_FRAME_MS_MIN_RECOMMENDED = 20
AUDIO_FRAME_MS_MAX_RECOMMENDED = 100

MAX_CONTEXT_ITEMS = 256          # §6.1 start.context
MAX_CONTEXT_ITEM_BYTES = 128     # §6.1 start.context, bytes not characters
MAX_SESSION_ID_BYTES = 64        # §7.1


def pcm_bytes_to_ms(n: int) -> float:
    """Milliseconds of 16 kHz mono s16le audio in `n` bytes. Used for the §9 audio queue bound."""
    return (n * 1000.0) / (SAMPLE_RATE * CHANNELS * BYTES_PER_SAMPLE)


# ─── §3.2 Frame types ─────────────────────────────────────────────────────────────────────────


class FrameType(int, Enum):
    CONTROL = 0x01  # UTF-8 JSON object (§4)
    AUDIO = 0x02    # raw PCM, 16 kHz mono s16le (§6.3)


_KNOWN_FRAME_TYPES = frozenset(int(t) for t in FrameType)


# ─── §11.3 Error codes ────────────────────────────────────────────────────────────────────────
#
# The member names match the wire strings exactly, so a reader comparing this file with §11.3
# does not have to translate. `.value` is the wire spelling.


class ErrorCode(str, Enum):
    # Fatal, wire-visible.
    proto_unsupported = "proto_unsupported"
    bad_frame = "bad_frame"
    frame_too_large = "frame_too_large"
    truncated_frame = "truncated_frame"
    unknown_frame_type = "unknown_frame_type"
    bad_utf8 = "bad_utf8"
    bad_json = "bad_json"
    bad_message = "bad_message"
    unknown_message = "unknown_message"
    state_violation = "state_violation"
    overload_audio = "overload_audio"
    overload_control = "overload_control"
    model_load_failed = "model_load_failed"
    internal_error = "internal_error"
    # Added with the network binding (§2.7, §5.1). Both are Server -> Client and both are fatal:
    # the connection cannot proceed, but the endpoint is working and said so, which is a
    # materially different thing for the user than a socket that never opened.
    server_busy = "server_busy"
    model_unavailable = "model_unavailable"
    # Non-fatal, wire-visible. `session_id` is REQUIRED on these (§11.1).
    stability_violation = "stability_violation"
    decode_failed = "decode_failed"
    audio_underrun = "audio_underrun"
    # Locally-detected only — never sent, because there is no peer to tell (§11.3, last
    # paragraph). Same spellings so a log reader learns one vocabulary.
    spawn_timeout = "spawn_timeout"
    handshake_timeout = "handshake_timeout"
    model_load_timeout = "model_load_timeout"
    peer_mismatch = "peer_mismatch"
    pipe_squatted = "pipe_squatted"
    # The network binding's local failures (§11.3, last paragraph). Named on this side too so a
    # log reader still learns one vocabulary, even though only the Client can raise them.
    connect_timeout = "connect_timeout"
    connect_refused = "connect_refused"
    tls_failed = "tls_failed"
    no_server_configured = "no_server_configured"


#: Codes that may legally appear on the wire (i.e. not the local-only set).
WIRE_ERROR_CODES = frozenset(
    {
        ErrorCode.proto_unsupported, ErrorCode.bad_frame, ErrorCode.frame_too_large,
        ErrorCode.truncated_frame, ErrorCode.unknown_frame_type, ErrorCode.bad_utf8,
        ErrorCode.bad_json, ErrorCode.bad_message, ErrorCode.unknown_message,
        ErrorCode.state_violation, ErrorCode.overload_audio, ErrorCode.overload_control,
        ErrorCode.model_load_failed, ErrorCode.internal_error, ErrorCode.server_busy,
        ErrorCode.model_unavailable, ErrorCode.stability_violation, ErrorCode.decode_failed,
        ErrorCode.audio_underrun,
    }
)

#: The three codes §11.3 marks `fatal: false`. Everything else defaults to fatal.
NON_FATAL_ERROR_CODES = frozenset(
    {ErrorCode.stability_violation, ErrorCode.decode_failed, ErrorCode.audio_underrun}
)


def default_fatal(code: ErrorCode | str) -> bool:
    """The `fatal` value §11.3 assigns to `code`, for use when *originating* an error.

    Advisory only. A receiver MUST act on the `fatal` field it actually received, not on this
    table, because §13 requires an unrecognised code to be handled by its flag.
    """
    try:
        return ErrorCode(code) not in NON_FATAL_ERROR_CODES
    except ValueError:
        return True


class ProtocolError(Exception):
    """A detected fault at any layer.

    Carries enough to satisfy §8.3 step 3's logging requirement and §12.3's `at_offset` field.

    `at_offset` is a byte offset. For framing faults it is an offset from the start of the whole
    stream (that is what the fixtures assert). For payload faults raised by :func:`decode_control`
    it is an offset **within the payload**, because a pure codec has no idea where the payload sat
    in the stream — the caller adds `Frame.payload_offset` to get the fixture's number.
    """

    def __init__(
        self,
        code: ErrorCode | str,
        msg: str,
        *,
        fatal: bool | None = None,
        session_id: str | None = None,
        at_offset: int | None = None,
        supported: Sequence[int] | None = None,
        detail: str | None = None,
    ) -> None:
        super().__init__(f"{code}: {msg}")
        self.code_raw = code.value if isinstance(code, ErrorCode) else str(code)
        try:
            self.code: ErrorCode | None = ErrorCode(self.code_raw)
        except ValueError:
            self.code = None  # §13 forward tolerance: unknown codes are still well-formed errors
        self.msg = msg
        self.fatal = default_fatal(self.code_raw) if fatal is None else bool(fatal)
        self.session_id = session_id
        self.at_offset = at_offset
        self.supported = list(supported) if supported is not None else []
        self.detail = detail if detail is not None else msg

    def to_message(self) -> "ErrorMessage":
        """The wire `error` message for this fault (§6.2). Only meaningful for wire codes."""
        return ErrorMessage(
            code=self.code_raw,
            fatal=self.fatal,
            msg=self.msg,
            session_id=self.session_id,
            supported=list(self.supported),
        )

    def __repr__(self) -> str:  # pragma: no cover - diagnostics only
        return (
            f"ProtocolError(code={self.code_raw!r}, fatal={self.fatal}, "
            f"at_offset={self.at_offset}, msg={self.msg!r})"
        )


# ─── §3 Framing ───────────────────────────────────────────────────────────────────────────────
#
#  0        1        2        3        4        5                    4+length
#  +--------+--------+--------+--------+--------+--------------------+
#  |      length  (u32, little-endian) |  type  |  payload           |
#  +--------+--------+--------+--------+--------+--------------------+
#                                      \___________________________/
#                                        length counts these bytes
#
# **length = 1 + len(payload).** The length field covers the type byte. §3.1 records why: the
# reader needs exactly `length` more bytes after the prefix with no special case, and `length == 0`
# becomes structurally impossible, turning "forgot the type byte" into a loud bad_frame instead of
# a silent one-byte desync that surfaces as garbage text much later.
#
# Little-endian, no htonl. `int.from_bytes(..., "little")` without `signed=True` is already
# unsigned, which is what fixture 007 (0xFFFFFFFF) checks for.


@dataclass(frozen=True, slots=True)
class Frame:
    """One decoded frame.

    `payload` is a fresh `bytes` object, not a view into the reader's buffer: Python has no
    lifetime story that would make a view safe once the reader compacts, and the copy is one
    memcpy of at most 64 KiB.
    """

    type: int
    payload: bytes
    #: Stream offset of this frame's 4-byte length prefix.
    offset: int = 0
    #: Stream offset of the first payload byte (== offset + 5). The base for turning a
    #: payload-relative `ProtocolError.at_offset` into the absolute offset the fixtures assert.
    payload_offset: int = 0


class FrameReader:
    """Incremental, byte-stream frame decoder. Single-owner; not thread-safe.

    **Message mode does not excuse a message-oriented parser** (§2.2). The pipe is created with
    `PIPE_READMODE_MESSAGE`, but three real situations break the one-read-one-message assumption:
    a reader whose buffer is smaller than the message gets `ERROR_MORE_DATA` and must reassemble;
    a peer that sets `PIPE_READMODE_BYTE` on its own handle silently changes delivery; and this
    Python side reaches the pipe through ctypes/kernel32 where the read mode is easy to get wrong
    (docs/protocol.md §14 item 3 records that it is *unverified* whether it works at all). Fixtures
    001 and 002 fail any decoder that assumes otherwise.

    Usage — drain fully after every push, or fixture 002 (three frames in one read) fails::

        r = FrameReader()
        r.push(chunk)
        for frame in r:            # yields every whole frame the buffer now contains
            handle(frame)
        if r.at_end_of_stream:
            orderly_shutdown()

    Errors surface from :meth:`next_frame` as a raised :class:`ProtocolError`, *after* every frame
    that was decoded before the fault has been delivered. A reader that discarded already-decoded
    frames on a later bad header would fail fixture 006. The error is sticky: once raised, it is
    raised again on every subsequent call, because §3.3 forbids resynchronisation — there is no
    framing marker to resynchronise *to*, so any byte pattern is a legal payload and a scan would
    find false headers.
    """

    __slots__ = ("_buf", "_pos", "_stream_offset", "_eof", "_error")

    # Compact the buffer once this many consumed bytes have accumulated in front of it. Purely a
    # memory/copy trade-off; correctness does not depend on the value.
    _COMPACT_THRESHOLD = MAX_FRAME_ON_WIRE

    def __init__(self) -> None:
        self._buf = bytearray()
        self._pos = 0            # consumed bytes at the front of _buf
        self._stream_offset = 0  # total bytes consumed from the stream
        self._eof = False
        self._error: ProtocolError | None = None

    # -- feeding -------------------------------------------------------------------------------

    def push(self, data: bytes | bytearray | memoryview) -> None:
        """Append bytes exactly as they were delivered.

        Never blocks and never fails: the size limit is enforced on the length prefix (§3.3), not
        on the buffer, so `push` cannot be the place a frame is rejected. That matters because a
        peer may legitimately deliver a 64 KiB frame in 4096-byte pieces.
        """
        if data:
            self._buf += data

    def notify_eof(self) -> None:
        """Signal end of stream (0-byte read, or ERROR_BROKEN_PIPE).

        After this, :meth:`next_frame` returns ``None`` with :attr:`at_end_of_stream` true if the
        buffer is empty (fixture 010, orderly shutdown), and raises `truncated_frame` if a partial
        frame remains (fixture 011).
        """
        self._eof = True

    def reset(self) -> None:
        """Return to the initial state.

        Used between fixture cases; **never** on a live connection, because §3.3 forbids
        resynchronisation.
        """
        self._buf.clear()
        self._pos = 0
        self._stream_offset = 0
        self._eof = False
        self._error = None

    # -- state ---------------------------------------------------------------------------------

    @property
    def buffered(self) -> int:
        """Bytes buffered but not yet forming a complete frame."""
        return len(self._buf) - self._pos

    @property
    def stream_offset(self) -> int:
        """Total bytes consumed from the stream so far — the base for `at_offset`."""
        return self._stream_offset

    @property
    def at_end_of_stream(self) -> bool:
        """True only for the orderly case: EOF signalled *and* nothing left buffered."""
        return self._eof and self.buffered == 0 and self._error is None

    @property
    def error(self) -> ProtocolError | None:
        return self._error

    # -- decoding ------------------------------------------------------------------------------

    def next_frame(self) -> Frame | None:
        """Return the next whole frame, or ``None`` if more bytes are needed.

        ``None`` means *either* "push more" *or* "orderly end of stream" — distinguish with
        :attr:`at_end_of_stream`. Raises :class:`ProtocolError` on any framing fault.
        """
        if self._error is not None:
            raise self._error

        avail = self.buffered
        if avail < FRAME_HEADER_BYTES:
            if self._eof and avail > 0:
                raise self._fail(
                    ErrorCode.truncated_frame,
                    f"end of stream with {avail} bytes buffered, fewer than the "
                    f"{FRAME_HEADER_BYTES}-byte length prefix",
                    at_offset=self._stream_offset,
                )
            return None

        # §3.3: validate `length` from the 4-byte prefix ALONE, before reading, buffering, or
        # allocating any payload. Everything below this point must not depend on payload bytes
        # existing, which is why the range check precedes the completeness check.
        base = self._pos
        length = int.from_bytes(self._buf[base : base + FRAME_HEADER_BYTES], "little")

        if length < MIN_LENGTH_FIELD:
            raise self._fail(
                ErrorCode.bad_frame,
                f"length field {length} is below the minimum legal value {MIN_LENGTH_FIELD}",
                at_offset=self._stream_offset,
            )
        if length > MAX_LENGTH_FIELD:
            raise self._fail(
                ErrorCode.frame_too_large,
                f"length field {length} exceeds MAX_PAYLOAD+1 ({MAX_LENGTH_FIELD})",
                at_offset=self._stream_offset,
            )

        total = FRAME_HEADER_BYTES + length
        if avail < total:
            if self._eof:
                raise self._fail(
                    ErrorCode.truncated_frame,
                    f"end of stream with {avail} of {total} frame bytes buffered",
                    at_offset=self._stream_offset,
                )
            return None

        type_byte = self._buf[base + FRAME_HEADER_BYTES]
        if type_byte not in _KNOWN_FRAME_TYPES:
            # §3.2: an undefined type is fatal, not a skippable extension. The handshake exists
            # precisely so both peers agree on the message set, so an undefined type means one
            # peer is broken and skipping it would hide the bug while the stream looks healthy.
            raise self._fail(
                ErrorCode.unknown_frame_type,
                f"type byte 0x{type_byte:02X} undefined in proto {PROTO_VERSION}",
                at_offset=self._stream_offset + FRAME_HEADER_BYTES,
            )

        payload = bytes(self._buf[base + FRAME_HEADER_BYTES + 1 : base + total])
        frame = Frame(
            type=type_byte,
            payload=payload,
            offset=self._stream_offset,
            payload_offset=self._stream_offset + FRAME_HEADER_BYTES + 1,
        )

        self._pos += total
        self._stream_offset += total
        self._maybe_compact()
        return frame

    def __iter__(self) -> Iterator[Frame]:
        """Drain every whole frame currently buffered. Re-iterate after each push."""
        while True:
            frame = self.next_frame()
            if frame is None:
                return
            yield frame

    # -- internals -----------------------------------------------------------------------------

    def _fail(self, code: ErrorCode, msg: str, *, at_offset: int) -> ProtocolError:
        err = ProtocolError(code, msg, fatal=True, at_offset=at_offset)
        self._error = err
        return err

    def _maybe_compact(self) -> None:
        if self._pos and (self._pos >= self._COMPACT_THRESHOLD or self._pos == len(self._buf)):
            del self._buf[: self._pos]
            self._pos = 0


# ─── Frame encoding ───────────────────────────────────────────────────────────────────────────
#
# A writer MUST issue exactly one write per frame and MUST NOT split a frame across writes or pack
# two frames into one write (§2.2). Every function here therefore produces one complete, contiguous
# frame, and the transport writes the whole buffer in a single WriteFile.


def encode_frame(frame_type: int | FrameType, payload: bytes) -> bytes:
    """Encode one frame. Raises `ProtocolError(internal_error)` on an oversize payload.

    An oversize payload is a *local* bug, not a wire condition — this endpoint built something it
    is not allowed to send — so it fails loudly here rather than travelling and being rejected by
    the peer as `frame_too_large`.
    """
    n = len(payload)
    if n > MAX_PAYLOAD:
        raise ProtocolError(
            ErrorCode.internal_error,
            f"refusing to encode a {n}-byte payload; MAX_PAYLOAD is {MAX_PAYLOAD}",
        )
    length = n + 1
    return length.to_bytes(4, "little") + bytes((int(frame_type),)) + payload


def encode_audio_frame(pcm: bytes) -> bytes:
    """Encode a `0x02` audio frame. `pcm` must already be 16 kHz mono s16le.

    An odd length is `bad_message` on the wire (§6.3) because it means the sender split a sample;
    we refuse to produce one at all.
    """
    if len(pcm) % BYTES_PER_SAMPLE:
        raise ProtocolError(
            ErrorCode.internal_error,
            f"audio payload length {len(pcm)} is odd; a sample was split",
        )
    return encode_frame(FrameType.AUDIO, pcm)


def decode_audio_payload(payload: bytes) -> memoryview:
    """Validate an audio payload and return it as a memoryview of s16le samples.

    A zero-length payload is legal and yields no samples (fixture 003) — it is a no-op, not an
    error. An odd length is `bad_message` (§6.3).

    Returns a `memoryview` rather than a list: the caller (a backend) will hand it to numpy or to
    `array`, and materialising 32768 Python ints per 2 s frame would be absurd. Note the payload
    is not guaranteed to be 2-byte aligned; `memoryview.cast('h')` requires alignment, so callers
    that need `int16` should use `array('h', payload)` (which copies) or `numpy.frombuffer` on a
    `bytes` object (which is aligned).
    """
    if len(payload) % BYTES_PER_SAMPLE:
        raise ProtocolError(
            ErrorCode.bad_message,
            f"audio payload length {len(payload)} is odd; the sender split a sample",
        )
    return memoryview(payload)


# ─── §4 Encoding rules for control frames ─────────────────────────────────────────────────────


def _reject_constant(name: str) -> Any:
    # json.loads accepts NaN / Infinity / -Infinity by default. §4.2 is plain RFC 8259: they are
    # bad_json. This hook is the only way to refuse them without re-implementing the scanner.
    raise ProtocolError(ErrorCode.bad_json, f"{name} is not permitted (RFC 8259 JSON only)")


def _parse_float(s: str) -> float:
    # Guards the other route to a non-finite number: "1e400" is syntactically legal JSON and
    # Python turns it into inf without going through parse_constant.
    v = float(s)
    if v != v or v in (float("inf"), float("-inf")):
        raise ProtocolError(ErrorCode.bad_json, f"number {s!r} is not finite")
    return v


def _no_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    # §4.2: no duplicate keys in any object -> bad_json. Python's default object hook silently
    # keeps the last one, which is exactly the kind of quiet divergence between two independent
    # parsers this protocol is trying to avoid.
    seen: dict[str, Any] = {}
    for k, v in pairs:
        if k in seen:
            raise ProtocolError(ErrorCode.bad_json, f"duplicate key {k!r}")
        seen[k] = v
    return seen


def _reject_lone_surrogates(node: Any, path: str = "") -> None:
    # §4.2: "\uXXXX escapes are permitted; surrogate pairs MUST be well formed. A lone surrogate
    # is bad_json." Python's json decoder happily produces a lone surrogate character, and that
    # string cannot then be encoded back to UTF-8 — it would blow up much later, somewhere that
    # has no idea it is holding wire data. Check here, once, where the error is still nameable.
    if isinstance(node, str):
        for ch in node:
            if 0xD800 <= ord(ch) <= 0xDFFF:
                raise ProtocolError(
                    ErrorCode.bad_json,
                    f"lone surrogate U+{ord(ch):04X} in string at {path or '<root>'}",
                )
    elif isinstance(node, dict):
        for k, v in node.items():
            _reject_lone_surrogates(k, f"{path}.{k}")
            _reject_lone_surrogates(v, f"{path}.{k}")
    elif isinstance(node, list):
        for i, v in enumerate(node):
            _reject_lone_surrogates(v, f"{path}[{i}]")


def strict_json_loads(payload: bytes) -> dict[str, Any]:
    """Decode a control payload to a dict under the §4 restrictions.

    Layer order is §1's, and each layer reports the code it owns:
      1. not well-formed UTF-8        -> `bad_utf8`   (never substitute U+FFFD, §4.1)
      2. not conforming JSON          -> `bad_json`
      3. not a top-level object       -> `bad_json`

    `at_offset` on a `bad_utf8` failure is relative to the payload; the caller adds
    `Frame.payload_offset`.
    """
    if len(payload) > MAX_PAYLOAD:
        raise ProtocolError(
            ErrorCode.frame_too_large,
            f"control payload {len(payload)} bytes exceeds MAX_PAYLOAD {MAX_PAYLOAD}",
        )

    # Python's UTF-8 decoder is already strict in exactly the way §4.1 requires: no overlongs, no
    # surrogate encodings, nothing above U+10FFFF, no truncated sequences. What it is *not* is
    # forgiving, and that is the point — `errors='replace'` here would be the U+FFFD substitution
    # the spec forbids, silently corrupting text on its way to the user's document.
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ProtocolError(
            ErrorCode.bad_utf8,
            f"control payload is not well-formed UTF-8: {exc.reason}",
            at_offset=exc.start,
            detail=f"invalid UTF-8 at payload byte {exc.start}: {exc.reason}",
        ) from None

    try:
        obj = json.loads(
            text,
            parse_constant=_reject_constant,
            parse_float=_parse_float,
            object_pairs_hook=_no_duplicate_keys,
        )
    except ProtocolError:
        raise
    except json.JSONDecodeError as exc:
        raise ProtocolError(ErrorCode.bad_json, f"malformed JSON: {exc.msg} at char {exc.pos}",
                            at_offset=exc.pos) from None
    except RecursionError:
        raise ProtocolError(ErrorCode.bad_json, "JSON nesting too deep") from None

    if not isinstance(obj, dict):
        raise ProtocolError(
            ErrorCode.bad_json,
            f"top level must be an object, got {type(obj).__name__}",
        )
    _reject_lone_surrogates(obj)
    return obj


# ─── §6 Message catalogue ─────────────────────────────────────────────────────────────────────


class StreamingMode(str, Enum):
    """§6.2 `ready.mode` — exactly the taxonomy Phase 0A records, so latency numbers stay
    interpretable. Any other value is `bad_message`."""

    NATIVE_STREAMING = "native-streaming"
    STATEFUL_CHUNKED = "stateful-chunked"
    PSEUDO_STREAMING = "pseudo-streaming"


class Message:
    """Base for every control message. `T` is the wire `t` value."""

    T: str = ""

    def to_json(self) -> dict[str, Any]:  # pragma: no cover - overridden everywhere
        raise NotImplementedError

    def encode(self) -> bytes:
        """Compact UTF-8 JSON. Absent optional fields are omitted, never emitted as null."""
        return encode_control(self)

    def encode_frame(self) -> bytes:
        return encode_control_frame(self)

    #: The `session_id` this message carries, or None for the ones that carry none (hello,
    #: hello_ack, loading, ready). This is the accessor the §7.1 staleness rule is written
    #: against, so the rule is implemented once rather than once per message type. Message types
    #: that *do* carry a session id declare it as a dataclass field, which shadows this.
    session_id: str | None = None

    @property
    def carries_text(self) -> bool:
        """True for `partial` and `final` — the two types §7.1 requires be **dropped** when
        stale, as opposed to `stopped`/`error`, which are accepted for bookkeeping only."""
        return False

    @property
    def consumes_seq(self) -> bool:
        """True for the types that consume a `seq` number (§7.4): exactly partial and final."""
        return False


# -- Client -> Server -------------------------------------------------------------------------


@dataclass(slots=True)
class Hello(Message):
    T = "hello"
    proto: int = PROTO_VERSION
    client: str | None = None
    #: §5.1. None means "whatever the server serves by default". A server that does not have the
    #: named model answers `model_unavailable`; substituting another is forbidden, because the
    #: result is transcripts that are merely worse and nobody notices.
    model: str | None = None

    def to_json(self) -> dict[str, Any]:
        out: dict[str, Any] = {"t": self.T, "proto": self.proto}
        if self.client is not None:
            out["client"] = self.client
        if self.model is not None:
            out["model"] = self.model
        return out


@dataclass(slots=True)
class Start(Message):
    T = "start"
    session_id: str = ""
    sample_rate: int = SAMPLE_RATE
    channels: int = 1
    format: str = "s16le"
    context: list[str] = field(default_factory=list)
    #: Distinguishes "sent an empty array" from "did not send the field". A Server without context
    #: support must report `context_supported: false` in `ready` rather than silently ignoring the
    #: field, because plan.md §4 compares context-enabled and context-free runs separately and a
    #: silent drop would corrupt that comparison.
    context_present: bool = False


    def to_json(self) -> dict[str, Any]:
        out: dict[str, Any] = {
            "t": self.T,
            "session_id": self.session_id,
            "sample_rate": self.sample_rate,
        }
        if self.channels != 1:
            out["channels"] = self.channels
        if self.format != "s16le":
            out["format"] = self.format
        if self.context_present:
            out["context"] = list(self.context)
        return out


@dataclass(slots=True)
class Stop(Message):
    T = "stop"
    session_id: str = ""


    def to_json(self) -> dict[str, Any]:
        return {"t": self.T, "session_id": self.session_id}


@dataclass(slots=True)
class Cancel(Message):
    T = "cancel"
    session_id: str = ""


    def to_json(self) -> dict[str, Any]:
        return {"t": self.T, "session_id": self.session_id}


@dataclass(slots=True)
class Discontinuity(Message):
    T = "discontinuity"
    session_id: str = ""
    gap_ms: int = 0
    reason: str | None = None


    def to_json(self) -> dict[str, Any]:
        out: dict[str, Any] = {"t": self.T, "session_id": self.session_id, "gap_ms": self.gap_ms}
        if self.reason is not None:
            out["reason"] = self.reason
        return out


# -- Server -> Client -------------------------------------------------------------------------


@dataclass(slots=True)
class ModelInfo:
    """One entry of `hello_ack.models[]` (§5.1).

    Everything but `id` is optional: a server that knows only the ids of its models is still
    more useful than one that advertises nothing, because the settings page can then offer a
    list instead of a text box.
    """

    id: str = ""
    display_name: str | None = None
    mode: str | None = None
    chunk_ms: int | None = None
    context_supported: bool = False

    def to_json(self) -> dict[str, Any]:
        out: dict[str, Any] = {"id": self.id, "context_supported": self.context_supported}
        if self.display_name is not None:
            out["display_name"] = self.display_name
        if self.mode is not None:
            out["mode"] = self.mode
        if self.chunk_ms is not None:
            out["chunk_ms"] = self.chunk_ms
        return out


@dataclass(slots=True)
class HelloAck(Message):
    T = "hello_ack"
    proto: int = PROTO_VERSION
    server: str | None = None
    #: §5.1. Empty means "not advertised", never "there is only one".
    models: list[ModelInfo] = field(default_factory=list)

    def to_json(self) -> dict[str, Any]:
        out: dict[str, Any] = {"t": self.T, "proto": self.proto}
        if self.server is not None:
            out["server"] = self.server
        if self.models:
            out["models"] = [m.to_json() for m in self.models]
        return out


@dataclass(slots=True)
class Loading(Message):
    T = "loading"
    model: str = ""
    stage: str | None = None
    progress: int | None = None

    def to_json(self) -> dict[str, Any]:
        out: dict[str, Any] = {"t": self.T, "model": self.model}
        if self.stage is not None:
            out["stage"] = self.stage
        if self.progress is not None:
            out["progress"] = self.progress
        return out


@dataclass(slots=True)
class Ready(Message):
    T = "ready"
    model: str = ""
    warm_ms: int = 0
    mode: str = StreamingMode.NATIVE_STREAMING.value
    chunk_ms: int | None = None
    context_supported: bool = False

    def to_json(self) -> dict[str, Any]:
        out: dict[str, Any] = {
            "t": self.T,
            "model": self.model,
            "warm_ms": self.warm_ms,
            "mode": self.mode,
        }
        if self.chunk_ms is not None:
            out["chunk_ms"] = self.chunk_ms
        if self.context_supported:
            out["context_supported"] = True
        return out


@dataclass(slots=True)
class Partial(Message):
    T = "partial"
    session_id: str = ""
    utterance_id: int = 0
    seq: int = 0
    #: **Cumulative** hypothesis for the current utterance, not a delta (§8.1).
    text: str = ""
    #: Prefix length of `text` in UTF-8 bytes of the *decoded* string (§8.2). Not characters, not
    #: code points, not UTF-16 code units. `len(text)` is never the right value here.
    stable_bytes: int = 0
    audio_pos_ms: int | None = None


    @property
    def carries_text(self) -> bool:
        return True

    @property
    def consumes_seq(self) -> bool:
        return True

    def to_json(self) -> dict[str, Any]:
        out: dict[str, Any] = {
            "t": self.T,
            "session_id": self.session_id,
            "utterance_id": self.utterance_id,
            "seq": self.seq,
            "text": self.text,
            "stable_bytes": self.stable_bytes,
        }
        if self.audio_pos_ms is not None:
            out["audio_pos_ms"] = self.audio_pos_ms
        return out


@dataclass(slots=True)
class Final(Message):
    T = "final"
    session_id: str = ""
    utterance_id: int = 0
    seq: int = 0
    text: str = ""
    audio_pos_ms: int = 0
    #: Optional and redundant; if present MUST equal `utf8_len(text)` (§6.2).
    stable_bytes: int | None = None


    @property
    def carries_text(self) -> bool:
        return True

    @property
    def consumes_seq(self) -> bool:
        return True

    def to_json(self) -> dict[str, Any]:
        out: dict[str, Any] = {
            "t": self.T,
            "session_id": self.session_id,
            "utterance_id": self.utterance_id,
            "seq": self.seq,
            "text": self.text,
            "audio_pos_ms": self.audio_pos_ms,
        }
        if self.stable_bytes is not None:
            out["stable_bytes"] = self.stable_bytes
        return out


@dataclass(slots=True)
class Stopped(Message):
    T = "stopped"
    session_id: str = ""
    reason: str | None = None


    def to_json(self) -> dict[str, Any]:
        out: dict[str, Any] = {"t": self.T, "session_id": self.session_id}
        if self.reason is not None:
            out["reason"] = self.reason
        return out


@dataclass(slots=True)
class ErrorMessage(Message):
    """§6.2 `error`. Bidirectional; the Client sends it too (e.g. `stability_violation`).

    `code` keeps the **raw wire string**, not an `ErrorCode`, because §13 requires an
    unrecognised code to be treated as a well-formed error and acted on by its `fatal` flag. Use
    :attr:`code_enum` when you want the parsed form and are prepared for None.
    """

    T = "error"
    code: str = ErrorCode.internal_error.value
    fatal: bool = True
    msg: str = ""
    session_id: str | None = None
    supported: list[int] = field(default_factory=list)
    #: §2.7, with `server_busy`. A hint; the Client's own backoff is the real bound.
    retry_after_ms: int | None = None
    #: §5.1, with `model_unavailable`. What the server does have.
    available: list[str] = field(default_factory=list)


    @property
    def code_enum(self) -> ErrorCode | None:
        try:
            return ErrorCode(self.code)
        except ValueError:
            return None

    def to_json(self) -> dict[str, Any]:
        out: dict[str, Any] = {
            "t": self.T,
            "code": self.code,
            "fatal": self.fatal,
            "msg": self.msg,
        }
        if self.session_id is not None:
            out["session_id"] = self.session_id
        if self.supported:
            out["supported"] = list(self.supported)
        if self.retry_after_ms is not None:
            out["retry_after_ms"] = self.retry_after_ms
        if self.available:
            out["available"] = list(self.available)
        return out


CLIENT_MESSAGE_TYPES = frozenset({"hello", "start", "stop", "cancel", "discontinuity", "error"})
SERVER_MESSAGE_TYPES = frozenset(
    {"hello_ack", "loading", "ready", "partial", "final", "stopped", "error"}
)


# ─── Control-frame decoding ───────────────────────────────────────────────────────────────────
#
# Fails in the layer order §1 requires, with the code that layer owns:
#   1. not well-formed UTF-8                      -> bad_utf8        (never substitute U+FFFD)
#   2. not a conforming JSON object               -> bad_json
#   3. `t` unknown                                -> unknown_message
#   4. schema fault (missing / wrong type / out of range / duplicate) -> bad_message
#
# **Unknown fields in a known message are ignored**; an **unknown `t` is fatal** (§4.2). The
# asymmetry is intentional: an extra field cannot change the meaning of a message a receiver
# already understands, while an unknown message type means the peer believes something is
# happening that this peer knows nothing about.


def _bad(msg: str) -> ProtocolError:
    return ProtocolError(ErrorCode.bad_message, msg)


def _req(obj: dict[str, Any], key: str, t: str) -> Any:
    if key not in obj:
        raise _bad(f"{t}: missing required field {key!r}")
    return obj[key]


def _as_int(v: Any, t: str, key: str, *, lo: int | None = None, hi: int | None = None) -> int:
    # bool is a subclass of int in Python; `True` must not satisfy an integer field.
    if isinstance(v, bool) or not isinstance(v, int):
        raise _bad(f"{t}.{key}: expected an integer, got {type(v).__name__}")
    if lo is not None and v < lo:
        raise _bad(f"{t}.{key}: {v} is below the minimum {lo}")
    if hi is not None and v > hi:
        raise _bad(f"{t}.{key}: {v} is above the maximum {hi}")
    return v


def _as_str(v: Any, t: str, key: str) -> str:
    if not isinstance(v, str):
        raise _bad(f"{t}.{key}: expected a string, got {type(v).__name__}")
    return v


def _as_bool(v: Any, t: str, key: str) -> bool:
    if not isinstance(v, bool):
        raise _bad(f"{t}.{key}: expected a boolean, got {type(v).__name__}")
    return v


def _as_session_id(v: Any, t: str, key: str = "session_id") -> str:
    s = _as_str(v, t, key)
    if not is_valid_session_id(s):
        raise _bad(
            f"{t}.{key}: {s!r} is not a valid session id "
            f"(1-{MAX_SESSION_ID_BYTES} bytes of ASCII [A-Za-z0-9_-])"
        )
    return s


def _opt(obj: dict[str, Any], key: str) -> Any | None:
    # An explicit JSON null is not the same as an absent field: the protocol never defines a null
    # anywhere, so one on the wire is a schema fault rather than "unset".
    if key not in obj:
        return None
    v = obj[key]
    if v is None:
        raise _bad(f"field {key!r} is null; the protocol defines no null values")
    return v


def _decode_hello(o: dict[str, Any]) -> Hello:
    t = "hello"
    m = Hello(proto=_as_int(_req(o, "proto", t), t, "proto", lo=1, hi=65535))
    c = _opt(o, "client")
    if c is not None:
        m.client = _as_str(c, t, "client")
    mo = _opt(o, "model")
    if mo is not None:
        m.model = _as_str(mo, t, "model")
    return m


def _decode_start(o: dict[str, Any]) -> Start:
    t = "start"
    m = Start(session_id=_as_session_id(_req(o, "session_id", t), t))
    m.sample_rate = _as_int(_req(o, "sample_rate", t), t, "sample_rate")
    if m.sample_rate != SAMPLE_RATE:
        raise _bad(f"{t}.sample_rate: {m.sample_rate} != {SAMPLE_RATE}; proto 1 fixes the rate")
    ch = _opt(o, "channels")
    if ch is not None:
        m.channels = _as_int(ch, t, "channels")
        if m.channels != CHANNELS:
            raise _bad(f"{t}.channels: {m.channels} != {CHANNELS} in proto 1")
    fmt = _opt(o, "format")
    if fmt is not None:
        m.format = _as_str(fmt, t, "format")
        if m.format != "s16le":
            raise _bad(f"{t}.format: {m.format!r} != 's16le' in proto 1")
    ctx = _opt(o, "context")
    if ctx is not None:
        if not isinstance(ctx, list):
            raise _bad(f"{t}.context: expected an array of string")
        if len(ctx) > MAX_CONTEXT_ITEMS:
            raise _bad(f"{t}.context: {len(ctx)} items exceeds the limit {MAX_CONTEXT_ITEMS}")
        items: list[str] = []
        for i, item in enumerate(ctx):
            s = _as_str(item, t, f"context[{i}]")
            # The limit is 128 *bytes*, not characters — one more place the Python/UTF-8
            # mismatch would silently admit an over-long item.
            if utf8_len(s) > MAX_CONTEXT_ITEM_BYTES:
                raise _bad(
                    f"{t}.context[{i}]: {utf8_len(s)} bytes exceeds the limit "
                    f"{MAX_CONTEXT_ITEM_BYTES}"
                )
            items.append(s)
        m.context = items
        m.context_present = True
    return m


def _decode_stop(o: dict[str, Any]) -> Stop:
    return Stop(session_id=_as_session_id(_req(o, "session_id", "stop"), "stop"))


def _decode_cancel(o: dict[str, Any]) -> Cancel:
    return Cancel(session_id=_as_session_id(_req(o, "session_id", "cancel"), "cancel"))


def _decode_discontinuity(o: dict[str, Any]) -> Discontinuity:
    t = "discontinuity"
    m = Discontinuity(
        session_id=_as_session_id(_req(o, "session_id", t), t),
        gap_ms=_as_int(_req(o, "gap_ms", t), t, "gap_ms", lo=0),
    )
    r = _opt(o, "reason")
    if r is not None:
        m.reason = _as_str(r, t, "reason")
    return m


def _decode_hello_ack(o: dict[str, Any]) -> HelloAck:
    t = "hello_ack"
    m = HelloAck(proto=_as_int(_req(o, "proto", t), t, "proto", lo=1, hi=65535))
    s = _opt(o, "server")
    if s is not None:
        m.server = _as_str(s, t, "server")
    models = _opt(o, "models")
    if models is not None:
        if not isinstance(models, list):
            raise _bad(f"{t}.models: expected an array of object")
        # §5.1: the catalogue is advisory, so a malformed *element* is skipped rather than
        # failing the handshake. Losing a working connection over a mis-typed display name
        # would be a worse outcome than an incomplete model list. A `models` that is not an
        # array at all is still bad_message — that is a server which misunderstood the field.
        for entry in models:
            if not isinstance(entry, dict):
                continue
            ident = entry.get("id")
            if not isinstance(ident, str) or not ident:
                continue
            mode = entry.get("mode")
            chunk = entry.get("chunk_ms")
            name = entry.get("display_name")
            m.models.append(
                ModelInfo(
                    id=ident,
                    display_name=name if isinstance(name, str) else None,
                    mode=mode if isinstance(mode, str) else None,
                    chunk_ms=chunk if isinstance(chunk, int) and not isinstance(chunk, bool)
                    else None,
                    context_supported=bool(entry.get("context_supported", False)),
                )
            )
    return m


def _decode_loading(o: dict[str, Any]) -> Loading:
    t = "loading"
    m = Loading(model=_as_str(_req(o, "model", t), t, "model"))
    st = _opt(o, "stage")
    if st is not None:
        m.stage = _as_str(st, t, "stage")
    pr = _opt(o, "progress")
    if pr is not None:
        m.progress = _as_int(pr, t, "progress", lo=0, hi=100)
    return m


def _decode_ready(o: dict[str, Any]) -> Ready:
    t = "ready"
    mode = _as_str(_req(o, "mode", t), t, "mode")
    if mode not in {m.value for m in StreamingMode}:
        raise _bad(f"{t}.mode: {mode!r} is not one of {sorted(m.value for m in StreamingMode)}")
    m = Ready(
        model=_as_str(_req(o, "model", t), t, "model"),
        warm_ms=_as_int(_req(o, "warm_ms", t), t, "warm_ms", lo=0),
        mode=mode,
    )
    ck = _opt(o, "chunk_ms")
    if ck is not None:
        m.chunk_ms = _as_int(ck, t, "chunk_ms", lo=0)
    cs = _opt(o, "context_supported")
    if cs is not None:
        m.context_supported = _as_bool(cs, t, "context_supported")
    return m


def _decode_partial(o: dict[str, Any]) -> Partial:
    t = "partial"
    text = _as_str(_req(o, "text", t), t, "text")
    m = Partial(
        session_id=_as_session_id(_req(o, "session_id", t), t),
        utterance_id=_as_int(_req(o, "utterance_id", t), t, "utterance_id", lo=1),
        seq=_as_int(_req(o, "seq", t), t, "seq", lo=1),
        text=text,
        stable_bytes=_as_int(_req(o, "stable_bytes", t), t, "stable_bytes", lo=0),
    )
    # Range only. The grapheme-boundary and monotonicity halves of §8.2 are session state, not
    # message schema, and live in check_stability() so a violation is reported as
    # `stability_violation` (session-fatal) rather than `bad_message` (connection-fatal).
    if m.stable_bytes > utf8_len(text):
        raise _bad(
            f"{t}.stable_bytes: {m.stable_bytes} exceeds len(utf8(text)) {utf8_len(text)}"
        )
    ap = _opt(o, "audio_pos_ms")
    if ap is not None:
        m.audio_pos_ms = _as_int(ap, t, "audio_pos_ms", lo=0)
    return m


def _decode_final(o: dict[str, Any]) -> Final:
    t = "final"
    text = _as_str(_req(o, "text", t), t, "text")
    m = Final(
        session_id=_as_session_id(_req(o, "session_id", t), t),
        utterance_id=_as_int(_req(o, "utterance_id", t), t, "utterance_id", lo=1),
        seq=_as_int(_req(o, "seq", t), t, "seq", lo=1),
        text=text,
        audio_pos_ms=_as_int(_req(o, "audio_pos_ms", t), t, "audio_pos_ms", lo=0),
    )
    sb = _opt(o, "stable_bytes")
    if sb is not None:
        m.stable_bytes = _as_int(sb, t, "stable_bytes", lo=0)
        if m.stable_bytes != utf8_len(text):
            raise _bad(
                f"{t}.stable_bytes: {m.stable_bytes} must equal len(utf8(text)) "
                f"{utf8_len(text)} when present"
            )
    return m


def _decode_stopped(o: dict[str, Any]) -> Stopped:
    t = "stopped"
    m = Stopped(session_id=_as_session_id(_req(o, "session_id", t), t))
    r = _opt(o, "reason")
    if r is not None:
        m.reason = _as_str(r, t, "reason")
    return m


def _decode_error(o: dict[str, Any]) -> ErrorMessage:
    t = "error"
    m = ErrorMessage(
        code=_as_str(_req(o, "code", t), t, "code"),
        fatal=_as_bool(_req(o, "fatal", t), t, "fatal"),
        msg=_as_str(_req(o, "msg", t), t, "msg"),
    )
    sid = _opt(o, "session_id")
    if sid is not None:
        m.session_id = _as_session_id(sid, t)
    elif not m.fatal:
        # §11.1: fatal:false means "the named session is aborted"; without a name there is
        # nothing to abort, so the message is unusable rather than merely odd.
        raise _bad(f"{t}: session_id is required when fatal is false")
    sup = _opt(o, "supported")
    if sup is not None:
        if not isinstance(sup, list):
            raise _bad(f"{t}.supported: expected an array of integer")
        m.supported = [_as_int(v, t, "supported[]", lo=1, hi=65535) for v in sup]
    retry = _opt(o, "retry_after_ms")
    if retry is not None:
        # Clamped rather than rejected: a server that says "wait a week" is misconfigured, not
        # malicious, and the receiver's own backoff is the real bound anyway.
        m.retry_after_ms = max(0, min(3_600_000, _as_int(retry, t, "retry_after_ms")))
    avail = _opt(o, "available")
    if avail is not None:
        if not isinstance(avail, list):
            raise _bad(f"{t}.available: expected an array of string")
        m.available = [v for v in avail if isinstance(v, str) and v]
    return m


_DECODERS: dict[str, Callable[[dict[str, Any]], Message]] = {
    "hello": _decode_hello,
    "start": _decode_start,
    "stop": _decode_stop,
    "cancel": _decode_cancel,
    "discontinuity": _decode_discontinuity,
    "hello_ack": _decode_hello_ack,
    "loading": _decode_loading,
    "ready": _decode_ready,
    "partial": _decode_partial,
    "final": _decode_final,
    "stopped": _decode_stopped,
    "error": _decode_error,
}


def decode_control(payload: bytes) -> Message:
    """Decode a `0x01` control payload into a :class:`Message`. Raises :class:`ProtocolError`."""
    obj = strict_json_loads(payload)
    if "t" not in obj:
        raise ProtocolError(ErrorCode.bad_message, "control message has no 't' field")
    t = obj["t"]
    if not isinstance(t, str):
        raise ProtocolError(
            ErrorCode.bad_message, f"'t' must be a string, got {type(t).__name__}"
        )
    decoder = _DECODERS.get(t)
    if decoder is None:
        raise ProtocolError(
            ErrorCode.unknown_message, f"unknown message type {t!r} in proto {PROTO_VERSION}"
        )
    return decoder(obj)


def encode_control(m: Message) -> bytes:
    """Serialize a message to compact UTF-8 JSON.

    `ensure_ascii=False` on purpose (§4.2): raw UTF-8 on the wire makes `stable_bytes` offsets
    directly checkable against the wire bytes, whereas `\\uXXXX` escapes put the byte count of the
    payload and the byte count the offset indexes into permanent disagreement.

    `allow_nan=False` so a stray float infinity becomes a loud local error instead of the
    non-RFC-8259 token `Infinity` on the wire.
    """
    return json.dumps(
        m.to_json(), ensure_ascii=False, allow_nan=False, separators=(",", ":")
    ).encode("utf-8")


def encode_control_frame(m: Message) -> bytes:
    return encode_frame(FrameType.CONTROL, encode_control(m))


# ─── §7.1 session_id ──────────────────────────────────────────────────────────────────────────

_SESSION_ID_CHARS = frozenset(
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-"
)


def is_valid_session_id(s: str) -> bool:
    """§7.1: 1-64 **bytes** of ASCII ``[A-Za-z0-9_-]``.

    Restricted so it is safe in log lines and file names. Because the character set is ASCII, byte
    length and character length coincide here — but the spec says bytes, so the check is written
    against bytes and any non-ASCII character fails the character-set test first.
    """
    if not isinstance(s, str):
        return False
    if not 1 <= len(s) <= MAX_SESSION_ID_BYTES:
        return False
    return all(c in _SESSION_ID_CHARS for c in s)


# ─── §8.2 / §8.4 UTF-8 byte offsets and grapheme clusters ─────────────────────────────────────
#
# This is the part of the Python side most likely to disagree with the C++ side, and the fixtures
# under tests/fixtures/protocol/ (cases 012 and 013) exist to catch exactly that.
#
# **Unicode version drift — the open question docs/protocol.md §14 item 2 records.** Both endpoints
# must pin the *same* UAX #29 property tables, and the tables must live in-repo. Python's
# `unicodedata` tracks the CPython build's Unicode version, which is not under our control and will
# drift from a hand-maintained C++ table. Until that question is answered:
#
#   * the code points the spec **names** (§8.4) are covered by explicit in-repo ranges below, so
#     the normative floor cannot move when CPython's tables move;
#   * everything beyond the floor falls back to `unicodedata.category()`, which *can* drift;
#   * the drift is one-directional in a safe way for this process. Python is the protocol Server,
#     i.e. the *sender* of `stable_bytes`. A stricter boundary predicate makes this sender emit
#     offsets that are a subset of what the Client will accept, and §8.4 explicitly permits a
#     receiver's check to be strengthened without a proto bump for the same reason.
#
# `UNICODE_VERSION_NOTE` is deliberately not a version number: asserting one we have not pinned
# would be exactly the kind of unverified claim plan.md's tone forbids.

#: The Unicode version of the *fallback* classifier in this build. Informational, and explicitly
#: NOT the pinned protocol version — see the note above.
UNICODE_VERSION = unicodedata.unidata_version

#: Mirrors kd::utf8::kUnicodeVersion in src/util/utf8.h, which currently reads
#: "unspecified-pending-protocol-md-14.2". Both sides must be changed together, from one generated
#: source, when §14 item 2 is resolved.
UNICODE_VERSION_NOTE = "unspecified-pending-protocol-md-14.2"

_ZWJ = "‍"

# The normative floor from docs/protocol.md §8.4, as explicit ranges so it is version-independent.
# Anything here is treated as Grapheme_Extend=Yes regardless of what `unicodedata` says.
_FORCED_EXTEND_RANGES: tuple[tuple[int, int], ...] = (
    (0x0300, 0x036F),    # combining diacritical marks (the fixture's e + U+0301)
    (0x200D, 0x200D),    # ZWJ — also handled by name in the break rules
    (0x20E3, 0x20E3),    # combining enclosing keycap (fixture 013's "#️⃣")
    (0xFE00, 0xFE0F),    # variation selectors 1-16
    (0x1F3FB, 0x1F3FF),  # emoji skin-tone modifiers (category Sk, but Grapheme_Extend=Yes)
    (0xE0100, 0xE01EF),  # variation selectors 17-256
)

# Extended_Pictographic, approximated by ranges rather than a generated table — the same
# approximation src/util/utf8.h declares. It is applied slightly more broadly than UAX #29
# specifies, which is conservative: it can merge two adjacent emoji into one cluster (delaying a
# commit) but never split one (which would break text).
_EXT_PICTOGRAPHIC_RANGES: tuple[tuple[int, int], ...] = (
    (0x00A9, 0x00A9), (0x00AE, 0x00AE), (0x203C, 0x203C), (0x2049, 0x2049),
    (0x2122, 0x2122), (0x2139, 0x2139), (0x2194, 0x21AA), (0x231A, 0x231B),
    (0x2328, 0x2328), (0x23CF, 0x23CF), (0x23E9, 0x23FA), (0x24C2, 0x24C2),
    (0x25AA, 0x25AB), (0x25B6, 0x25B6), (0x25C0, 0x25C0), (0x25FB, 0x25FE),
    (0x2600, 0x27BF), (0x2934, 0x2935), (0x2B00, 0x2BFF), (0x3030, 0x3030),
    (0x303D, 0x303D), (0x3297, 0x3297), (0x3299, 0x3299),
    (0x1F000, 0x1FAFF), (0x1FC00, 0x1FFFD),
)

_REGIONAL_INDICATOR = (0x1F1E6, 0x1F1FF)


def _in_ranges(cp: int, ranges: tuple[tuple[int, int], ...]) -> bool:
    for lo, hi in ranges:
        if lo <= cp <= hi:
            return True
        if cp < lo:
            break  # ranges are sorted; nothing further can match
    return False


def _is_extend(ch: str) -> bool:
    """Grapheme_Extend=Yes, approximated.

    Floor: the explicit ranges above. Beyond the floor: categories Mn (nonspacing mark) and Me
    (enclosing mark), which is a close but not exact stand-in for Grapheme_Extend and is the
    version-dependent half of the approximation.
    """
    cp = ord(ch)
    if _in_ranges(cp, _FORCED_EXTEND_RANGES):
        return True
    return unicodedata.category(ch) in ("Mn", "Me")


def _is_spacing_mark(ch: str) -> bool:
    """Grapheme_Cluster_Break=SpacingMark, approximated by category Mc.

    UAX #29 excludes a handful of Mc code points from SpacingMark. Including them makes this
    predicate slightly over-eager at *joining*, which is the conservative direction: it can only
    make an offset invalid that UAX #29 would allow, never the reverse.
    """
    return unicodedata.category(ch) == "Mc"


def _is_ext_pictographic(ch: str) -> bool:
    return _in_ranges(ord(ch), _EXT_PICTOGRAPHIC_RANGES)


def _is_regional_indicator(ch: str) -> bool:
    return _REGIONAL_INDICATOR[0] <= ord(ch) <= _REGIONAL_INDICATOR[1]


def _utf8_char_len(ch: str) -> int:
    cp = ord(ch)
    if cp < 0x80:
        return 1
    if cp < 0x800:
        return 2
    if cp < 0x10000:
        return 3
    return 4


def utf8_len(text: str) -> int:
    """Length of `text` in UTF-8 bytes.

    Use this, never `len(text)`, anywhere a value crosses the wire. Written arithmetically rather
    than as `len(text.encode('utf-8'))` because it is called per-message on the hot path and the
    encode allocates.
    """
    n = 0
    for ch in text:
        n += _utf8_char_len(ch)
    return n


def byte_offset_of_index(text: str, index: int) -> int:
    """UTF-8 byte offset of the code point at Python string `index`.

    `index` is a code-point index (what `text[i]` uses); the result is a byte offset (what the wire
    uses). `index == len(text)` maps to `utf8_len(text)`.
    """
    if not 0 <= index <= len(text):
        raise ValueError(f"string index {index} out of range for a {len(text)}-code-point string")
    return utf8_len(text[:index])


def index_of_byte_offset(text: str, offset: int) -> int:
    """Python string index for a UTF-8 byte `offset`. Raises if the offset splits a sequence.

    The inverse of :func:`byte_offset_of_index`. Raising rather than rounding is deliberate: a
    silent round would turn a protocol violation into a plausible-looking slice, which is precisely
    how a `stable_bytes` bug becomes corrupted text in someone's document instead of an error.
    """
    if offset < 0:
        raise ValueError(f"negative byte offset {offset}")
    n = 0
    for i, ch in enumerate(text):
        if n == offset:
            return i
        n += _utf8_char_len(ch)
        if n > offset:
            raise ValueError(
                f"byte offset {offset} falls inside the UTF-8 sequence for {ch!r} "
                f"at string index {i}"
            )
    if n == offset:
        return len(text)
    raise ValueError(f"byte offset {offset} is past the end of the text ({n} bytes)")


def is_code_point_boundary(text: str, offset: int) -> bool:
    """True if `offset` is 0, `utf8_len(text)`, or the start of a UTF-8 sequence.

    This is the **weak** check. §8.2 requirement 2 is explicit that it is necessary but not
    sufficient, and fixture 013 is built entirely from offsets that pass this and fail
    :func:`is_grapheme_boundary`.
    """
    try:
        index_of_byte_offset(text, offset)
    except ValueError:
        return False
    return True


def grapheme_boundaries(text: str) -> tuple[int, ...]:
    """Every extended-grapheme-cluster boundary in `text`, as UTF-8 byte offsets.

    Always includes 0 and `utf8_len(text)`. The rule set is exactly the list src/util/utf8.h
    declares and docs/protocol.md §8.4 makes the normative minimum — no more, no less, because a
    divergence in *either* direction between the two implementations is an interop bug:

      * GB3   — never split a CR LF pair;
      * GB9   — never split before Grapheme_Extend or ZWJ (this covers variation selectors and
                U+20E3, which the spec calls out separately);
      * GB9a  — never split before a SpacingMark;
      * GB11  — never split a ZWJ emoji sequence;
      * GB12/13 — never split a regional-indicator pair.

    Deliberately **not** implemented, to match the C++ side's declared approximation:

      * GB4/GB5 (break around Control/CR/LF other than the CR LF pair itself). Implementing them
        would create *more* boundaries than the C++ side, i.e. this sender could emit an offset the
        Client rejects. Omitting them can only create fewer.
      * GB9b (Prepend), GB6-GB8 (Hangul L/V/T composition), GB9c (Indic conjuncts). All fail in
        the conservative direction: a decomposed Hangul syllable is reported as several clusters,
        so we may commit less than a full syllable boundary would allow. An over-eager split would
        break text; an over-cautious one only delays a commit.
    """
    if not text:
        return (0,)

    out = [0]
    off = 0
    prev: str | None = None
    ri_run = 0           # consecutive regional indicators ending at `prev`
    pict_extend = False  # the run ending at `prev` matches  ExtPict Extend*
    zwj_after_pict = False  # the run ending at `prev` matches  ExtPict Extend* ZWJ

    for ch in text:
        if prev is not None:
            if _breaks_between(prev, ch, ri_run, zwj_after_pict):
                out.append(off)

        # Advance the running classification to include `ch`. Order matters: ZWJ is also
        # Grapheme_Extend, so it must be recognised by name before the generic Extend branch.
        if ch == _ZWJ:
            zwj_after_pict = pict_extend
            pict_extend = False
        elif _is_ext_pictographic(ch):
            pict_extend = True
            zwj_after_pict = False
        elif _is_extend(ch):
            zwj_after_pict = False   # pict_extend is carried: ExtPict Extend* stays a pict run
        else:
            pict_extend = False
            zwj_after_pict = False

        ri_run = ri_run + 1 if _is_regional_indicator(ch) else 0

        off += _utf8_char_len(ch)
        prev = ch

    out.append(off)
    return tuple(out)


def _breaks_between(prev: str, cur: str, ri_run: int, zwj_after_pict: bool) -> bool:
    if prev == "\r" and cur == "\n":
        return False                                            # GB3
    if cur == _ZWJ or _is_extend(cur):
        return False                                            # GB9
    if _is_spacing_mark(cur):
        return False                                            # GB9a
    if zwj_after_pict and _is_ext_pictographic(cur):
        return False                                            # GB11
    if _is_regional_indicator(prev) and _is_regional_indicator(cur) and ri_run % 2 == 1:
        return False                                            # GB12/GB13
    return True


def is_grapheme_boundary(text: str, offset: int) -> bool:
    """True if `offset` is a boundary between extended grapheme clusters, including 0 and the end.

    False for any offset inside a UTF-8 sequence, inside a cluster, negative, or past the end.
    """
    if offset < 0 or offset > utf8_len(text):
        return False
    return offset in grapheme_boundaries(text)


def assert_grapheme_boundary(text: str, offset: int) -> int:
    """Return `offset`, or raise `ProtocolError(stability_violation)` if it is not a boundary.

    Sender-side guard. Emitting an off-boundary `stable_bytes` would make the Client abort the
    session under §8.3, so failing here — where the offending text is still in hand — is strictly
    better than discovering it as a `stability_violation` coming back over the pipe.
    """
    if not is_grapheme_boundary(text, offset):
        raise ProtocolError(
            ErrorCode.stability_violation,
            f"byte offset {offset} is not a grapheme-cluster boundary of {text!r}",
            fatal=False,
        )
    return offset


def floor_grapheme(text: str, offset: int) -> int:
    """Snap `offset` down to the nearest cluster boundary at or before it.

    Never moves forward: a commit engine that rounded *up* would commit a byte the model has not
    stabilised. Clamps to `utf8_len(text)` for an offset past the end.
    """
    total = utf8_len(text)
    if offset >= total:
        return total
    if offset <= 0:
        return 0
    best = 0
    for b in grapheme_boundaries(text):
        if b > offset:
            break
        best = b
    return best


def count_graphemes(text: str) -> int:
    """Number of grapheme clusters, under the approximation above."""
    return len(grapheme_boundaries(text)) - 1


def is_valid_stable_offset(text: str, stable_bytes: int) -> bool:
    """§8.2 requirements 1 and 2 together: in range **and** on a grapheme-cluster boundary.

    This is the predicate the `text`-kind fixtures (012, 013) name.
    """
    if not isinstance(stable_bytes, int) or isinstance(stable_bytes, bool):
        return False
    return is_grapheme_boundary(text, stable_bytes)


# ─── §7.4 Sequence rules ──────────────────────────────────────────────────────────────────────


class SeqVerdict(str, Enum):
    """Spellings match docs/protocol.md §12.5, because the fixture runner compares them as
    strings against the C++ side's `to_string(Verdict)`."""

    ACCEPT = "accept"
    ACCEPT_AFTER_GAP = "accept_after_gap"
    DROP_DUPLICATE_SEQ = "drop_duplicate_seq"
    DROP_OUT_OF_ORDER = "drop_out_of_order"


def classify_seq(last_seq: int, seq: int) -> SeqVerdict:
    """Pure implementation of the §7.4 table.

    A gap is **not** fatal. On a reliable transport a gap cannot arise in transit, so it means the
    sender skipped numbers or dropped its own output — but the message in hand is still the newest
    hypothesis, and discarding it would be worse than accepting it. The consequence is the
    anchor-confidence reset, because commit differencing (plan.md §1.5) infers the caret from a
    change produced by text we believe we injected, and a missed hypothesis makes that unsound.
    """
    if seq == last_seq:
        return SeqVerdict.DROP_DUPLICATE_SEQ
    if seq < last_seq:
        return SeqVerdict.DROP_OUT_OF_ORDER
    if seq == last_seq + 1:
        return SeqVerdict.ACCEPT
    return SeqVerdict.ACCEPT_AFTER_GAP


# ─── §8.2 requirement 3 — the monotonicity pair, both halves ──────────────────────────────────


class StabilityCheck(str, Enum):
    OK = "ok"
    OUT_OF_RANGE = "out_of_range"                    # stable_bytes > len(utf8(text))
    NOT_GRAPHEME_BOUNDARY = "not_grapheme_boundary"  # lands inside a cluster
    DECREASED = "decreased"                          # went backwards (fixture 017)
    PREFIX_MUTATED = "prefix_mutated"                # bytes below the mark changed (fixture 018)


def check_stability(
    prev_text: str,
    prev_stable: int,
    text: str,
    stable_bytes: int,
) -> tuple[StabilityCheck, int | None]:
    """Check a new hypothesis against the utterance's high-water mark.

    `prev_text` is the text of the message that established `prev_stable`; pass ``("", 0)`` at the
    start of an utterance. Returns the verdict and, for ``PREFIX_MUTATED``, the first differing
    byte offset — §8.3 step 3 requires it in the log line.

    The second half of requirement 3 is the one that is easy to forget and the one with teeth: a
    non-decreasing `stable_bytes` whose underlying bytes changed has still retracted text the
    Client may already have typed into the user's document. The bytes below the mark must be
    **byte-identical**, which is why the comparison happens on `.encode('utf-8')` rather than on
    the Python strings — the mark is a byte offset and slicing the string by it would compare the
    wrong region entirely.
    """
    total = utf8_len(text)
    if stable_bytes < 0 or stable_bytes > total:
        return StabilityCheck.OUT_OF_RANGE, None
    if not is_grapheme_boundary(text, stable_bytes):
        return StabilityCheck.NOT_GRAPHEME_BOUNDARY, None
    if stable_bytes < prev_stable:
        return StabilityCheck.DECREASED, None
    if prev_stable > 0:
        new_b = text.encode("utf-8")[:prev_stable]
        old_b = prev_text.encode("utf-8")[:prev_stable]
        if new_b != old_b:
            diverge = next(
                (i for i in range(min(len(new_b), len(old_b))) if new_b[i] != old_b[i]),
                min(len(new_b), len(old_b)),
            )
            return StabilityCheck.PREFIX_MUTATED, diverge
    return StabilityCheck.OK, None


# ─── §10.1 Connection states ──────────────────────────────────────────────────────────────────


class ConnState(str, Enum):
    """Uppercase spellings, matching docs/protocol.md §10.1 and the fixtures' `initial.state`."""

    SPAWNING = "SPAWNING"
    VALIDATING = "VALIDATING"
    HANDSHAKE = "HANDSHAKE"
    LOADING = "LOADING"
    IDLE = "IDLE"
    RUNNING = "RUNNING"
    FINALIZING = "FINALIZING"
    CANCELLING = "CANCELLING"
    DEAD = "DEAD"


# ─── §12.5 Verdicts and the pure receiver model ───────────────────────────────────────────────
#
# This is the identity/ordering/stability half of a receiver, written as a pure function so it can
# be driven by the shared fixtures without a pipe. It is not a full session machine: it decides
# what to do with an inbound message, it does not own a decoder, a queue, or an injector.
#
# The verdict spellings are the contract with tests/fixtures/protocol/ and with
# kd::Verdict::to_string() in src/session.h. Do not rename one without the other two.


class Verdict(str, Enum):
    ACCEPT = "accept"
    ACCEPT_AFTER_GAP = "accept_after_gap"
    ACCEPT_BOOKKEEPING_ONLY = "accept_bookkeeping_only"
    DROP_STALE_SESSION = "drop_stale_session"
    DROP_CANCELLED_SESSION = "drop_cancelled_session"
    DROP_DUPLICATE_SEQ = "drop_duplicate_seq"
    DROP_OUT_OF_ORDER = "drop_out_of_order"
    PROTOCOL_ERROR = "protocol_error"
    SENT = "sent"
    ABORT_SESSION = "abort_session"
    RECONNECTED = "reconnected"


@dataclass(slots=True)
class ReceiverState:
    """Everything the §7/§8 rules need. Mirrors the fixtures' `initial` block."""

    current_session_id: str | None = None
    state: ConnState = ConnState.IDLE
    last_seq: int = 0
    utterance_id: int = 0
    last_stable_bytes: int = 0
    #: The text that established `last_stable_bytes`. Needed for the prefix-stability half of
    #: §8.2 requirement 3; the fixtures do not carry it, so it starts empty and is filled by the
    #: first accepted hypothesis.
    last_stable_text: str = ""
    #: Sessions for which this endpoint has already written `cancel` (§10.4). A cancelled session
    #: stays in this set until its `stopped` arrives.
    cancelled_sessions: set[str] = field(default_factory=set)

    @classmethod
    def from_fixture(cls, initial: dict[str, Any]) -> "ReceiverState":
        return cls(
            current_session_id=initial.get("current_session_id"),
            state=ConnState(initial.get("state", "IDLE")),
            last_seq=int(initial.get("last_seq", 0)),
            utterance_id=int(initial.get("utterance_id", 0)),
            last_stable_bytes=int(initial.get("last_stable_bytes", 0)),
        )


@dataclass(slots=True)
class InboundOutcome:
    verdict: Verdict
    error: ProtocolError | None = None
    #: For ACCEPT_AFTER_GAP: {"expected_seq", "received_seq", "missing"} exactly as §12.5 spells
    #: them, so the fixture's `expect.gap` block can be compared field by field.
    gap: dict[str, int] | None = None
    #: Prose effect labels this model can actually assert. §12.5: a runner MUST NOT fail a case
    #: for an effect it does not model, so this list is deliberately a subset.
    effects: list[str] = field(default_factory=list)


def classify_inbound(state: ReceiverState, msg: Message) -> InboundOutcome:
    """Decide what to do with one inbound message, and apply the state change it implies.

    Rule order is forced by the spec and by the fixtures:

    1. §7.1 staleness first — a message for a session that is not current can never be applied,
       whatever else is wrong with it. `partial`/`final` are **dropped**; `stopped`/`error` are
       accepted for bookkeeping only, because that is how a receiver observes the orderly end of a
       session it has already moved on from.
    2. §10.4 cancellation — after this endpoint wrote `cancel`, every subsequent `partial`/`final`
       for that session is discarded regardless of content, until `stopped`.
    3. §7.4 sequencing — duplicate and out-of-order are dropped **with no state change**, which is
       what stops a stale hypothesis from moving the committed prefix backwards.
    4. §8.2 stability — checked only on a message that survived 1-3, so a duplicate cannot trigger
       a spurious violation.
    """
    sid = msg.session_id

    # 1. Staleness (§7.1).
    if sid is not None and sid != state.current_session_id:
        if msg.carries_text:
            return InboundOutcome(Verdict.DROP_STALE_SESSION)
        # `stopped` / `error` for a non-current session: recorded, never applied to a document.
        if isinstance(msg, Stopped):
            state.cancelled_sessions.discard(sid)
        return InboundOutcome(
            Verdict.ACCEPT_BOOKKEEPING_ONLY,
            effects=[f"record end of {sid}; no text applied, no injection"],
        )

    # 2. Cancellation (§10.4).
    if sid is not None and sid in state.cancelled_sessions and msg.carries_text:
        return InboundOutcome(Verdict.DROP_CANCELLED_SESSION)

    # Messages with no session identity and no text: handshake / lifecycle only.
    if isinstance(msg, HelloAck):
        state.state = ConnState.LOADING
        return InboundOutcome(Verdict.ACCEPT, effects=["state -> LOADING"])
    if isinstance(msg, Loading):
        return InboundOutcome(Verdict.ACCEPT)
    if isinstance(msg, Ready):
        state.state = ConnState.IDLE
        return InboundOutcome(Verdict.ACCEPT, effects=["state -> IDLE"])
    if isinstance(msg, Stopped):
        was_cancelled = sid is not None and sid in state.cancelled_sessions
        state.cancelled_sessions.discard(sid or "")
        state.state = ConnState.IDLE
        effects = ["state -> IDLE"]
        if was_cancelled:
            effects.append(f"no injection performed for {sid} after cancel")
        return InboundOutcome(Verdict.ACCEPT, effects=effects)
    if isinstance(msg, ErrorMessage):
        if msg.fatal:
            state.state = ConnState.DEAD
            return InboundOutcome(Verdict.ACCEPT, effects=["state -> DEAD"])
        return InboundOutcome(Verdict.ACCEPT, effects=[f"abort session {sid}"])

    if not isinstance(msg, (Partial, Final)):
        # A Client->Server message arriving inbound at a Client, or vice versa. Not modelled here.
        return InboundOutcome(Verdict.ACCEPT)

    # 3. Sequencing (§7.4).
    seq_verdict = classify_seq(state.last_seq, msg.seq)
    if seq_verdict is SeqVerdict.DROP_DUPLICATE_SEQ:
        return InboundOutcome(Verdict.DROP_DUPLICATE_SEQ)
    if seq_verdict is SeqVerdict.DROP_OUT_OF_ORDER:
        return InboundOutcome(Verdict.DROP_OUT_OF_ORDER)

    gap = None
    effects: list[str] = []
    if seq_verdict is SeqVerdict.ACCEPT_AFTER_GAP:
        gap = {
            "expected_seq": state.last_seq + 1,
            "received_seq": msg.seq,
            "missing": msg.seq - state.last_seq - 1,
        }
        effects = ["log_gap", "reset_anchor_confidence"]

    # 4. Stability (§8.2). The high-water mark resets at every utterance boundary, and is **not**
    #    reset by a seq gap — a gap means hypotheses were missed, not retracted, and weakening the
    #    invariant on a gap would let a buggy Server escape it by skipping numbers.
    prev_stable = state.last_stable_bytes
    prev_text = state.last_stable_text
    if msg.utterance_id != state.utterance_id:
        prev_stable, prev_text = 0, ""

    stable = msg.stable_bytes if isinstance(msg, Partial) else utf8_len(msg.text)
    check, diverge = check_stability(prev_text, prev_stable, msg.text, stable)
    if check is not StabilityCheck.OK:
        detail = {
            StabilityCheck.OUT_OF_RANGE: f"stable_bytes {stable} exceeds len(utf8(text))",
            StabilityCheck.NOT_GRAPHEME_BOUNDARY: (
                f"stable_bytes {stable} is not a grapheme-cluster boundary"
            ),
            StabilityCheck.DECREASED: (
                f"stable_bytes {stable} < {prev_stable} within utterance {msg.utterance_id}"
            ),
            StabilityCheck.PREFIX_MUTATED: f"stable prefix mutated at byte {diverge}",
        }[check]
        err = ProtocolError(
            ErrorCode.stability_violation,
            detail,
            fatal=False,
            session_id=msg.session_id,
            at_offset=diverge,
            detail=detail,
        )
        # §8.3: preserve the document, abort the session, do not clamp. The state is intentionally
        # left untouched apart from the abort, so nothing about the bad message is absorbed.
        state.state = ConnState.FINALIZING
        return InboundOutcome(
            Verdict.PROTOCOL_ERROR,
            error=err,
            effects=["preserve document", "abort session", "count irrevocable-error event"],
        )

    state.last_seq = msg.seq
    state.utterance_id = msg.utterance_id
    state.last_stable_bytes = stable
    state.last_stable_text = msg.text
    if isinstance(msg, Final):
        # §8.2: the high-water mark resets at the utterance boundary the final closes.
        state.last_stable_bytes = 0
        state.last_stable_text = ""

    verdict = Verdict.ACCEPT_AFTER_GAP if gap else Verdict.ACCEPT
    return InboundOutcome(verdict, gap=gap, effects=effects)


def apply_outbound(state: ReceiverState, msg: Message) -> InboundOutcome:
    """Apply the state change an **outbound** message implies (the fixtures' ``from: "client"``).

    The fixture verdict for these entries is ``sent``: the Client must emit exactly this message at
    exactly this point. This process is the protocol *Server*, so it cannot assert that the Client
    produced the message — §12.5 covers that case ("MUST NOT fail a case for an effect they do not
    model"). What it *can* do, and what the later verdicts in fixtures 019 and 020 depend on, is
    apply the state transition the message causes.
    """
    if isinstance(msg, Cancel):
        state.cancelled_sessions.add(msg.session_id)
        state.state = ConnState.CANCELLING
        return InboundOutcome(
            Verdict.SENT,
            effects=["state -> CANCELLING", f"drop queued outbound audio for {msg.session_id}"],
        )
    if isinstance(msg, Stop):
        state.state = ConnState.FINALIZING
        return InboundOutcome(Verdict.SENT, effects=["state -> FINALIZING"])
    if isinstance(msg, Start):
        # A new session resets the whole per-session identity space (§7.3, §7.4). Fixture 020
        # depends on this: seq 1 in session s8 must be `accept`, not `drop_out_of_order` against
        # the dead session's last_seq of 12.
        state.current_session_id = msg.session_id
        state.state = ConnState.RUNNING
        state.last_seq = 0
        state.utterance_id = 0
        state.last_stable_bytes = 0
        state.last_stable_text = ""
        return InboundOutcome(
            Verdict.SENT,
            effects=[f"current_session_id -> {msg.session_id}", "state -> RUNNING"],
        )
    if isinstance(msg, Hello):
        state.state = ConnState.HANDSHAKE
        return InboundOutcome(Verdict.SENT, effects=["state -> HANDSHAKE"])
    if isinstance(msg, ErrorMessage) and not msg.fatal:
        state.state = ConnState.FINALIZING
        return InboundOutcome(Verdict.SENT, effects=[f"abort session {msg.session_id}"])
    return InboundOutcome(Verdict.SENT)
