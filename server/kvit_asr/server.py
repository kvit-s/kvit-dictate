# Copyright 2026 Kvit
# Licensed under the Apache License, Version 2.0. See LICENSE-APACHE, or
# https://www.apache.org/licenses/LICENSE-2.0
r"""Top-level sidecar: transport, handshake, model load, and message routing.

This module owns docs/protocol.md §2 (transport, from the *pipe client* side), §5 (handshake)
and the connection-level parts of §10 (lifecycle). It owns none of §3/§4 — framing and control
decoding live in `kvit_asr.protocol`, which both this module and the shared conformance
fixtures use, so that a disagreement between the C++ and Python endpoints shows up as a failing
fixture rather than as garbled text in someone's document. It owns none of §6–§9 either:
sessions, VAD, stability and the audio queue live in `kvit_asr.session`.

──────────────────────────────────────────────────────────────────────────────────────────────
ONE BACKEND PER PROCESS. DO NOT TURN THIS INTO A REGISTRY.

There is exactly one backend in this interpreter, chosen once from `--backend` and never
changed. It is tempting to "improve" this into a registry that can switch models at runtime,
and that improvement is wrong for a structural reason, stated in plan.md §1.1: a Python-side
registry cannot enumerate models that only exist under a *different* interpreter, which is
precisely the situation plan.md §5 creates — the ONNX candidates live in `envs/onnx`, the
Whisper baseline lives in another project's read-only venv, and no single interpreter can
import both. The model→interpreter mapping therefore lives in the C++-side `models.json`
manifest, and selecting a model **restarts this process** under that model's interpreter. The
C++ side never learns what the model *is*, only which executable to launch.

Consequences that follow from that and are load-bearing here:
  * `ready` is sent exactly once per connection (docs/protocol.md §6.2). There is no
    "ready again", because a model change is a new process.
  * A backend whose runtime is not installed must disable *that backend*, not this server. So
    the backend package is imported lazily, inside `_load_backend`, and an ImportError there
    becomes a `model_load_failed` error frame the parent can display.

──────────────────────────────────────────────────────────────────────────────────────────────
NETWORK POLICY

No network access at any point during operation, and no model auto-download — plan.md §1.9.
Transport is a named pipe, never a socket: see `kvit_asr.config._RefuseNetworkAction`. Nothing
in this module imports `socket`, `urllib`, `http`, or any client library, and nothing in it
resolves a hostname. `config.apply_offline_policy()` additionally sets the ML libraries'
offline flags before any backend is imported.

──────────────────────────────────────────────────────────────────────────────────────────────
LOGGING

Structured, one JSON object per line, on stderr, for the parent to capture. Recognised text is
never logged unless `--log-transcripts` is given: `EventLogger.redact` replaces it with a byte
count. plan.md §1.9 — "no transcripts or audio persisted by default" — is not satisfied by a
program that writes them to a stream the parent happens to be redirecting to a file.
"""

from __future__ import annotations

import ctypes
import json
import os
import sys
import threading
import time
import traceback
from ctypes import wintypes
from typing import Any, Callable, Iterator, Protocol

from kvit_asr import config as kd_config
from kvit_asr.config import ServerConfig

# ─── Exit codes ─────────────────────────────────────────────────────────────────────────────
#
# The parent reaps this process and reports why it died, so the codes are part of the contract
# with the launcher even though they are not part of the wire protocol. Documented in
# server/README.md. 2 is reserved by argparse for usage errors.

EXIT_OK = 0
EXIT_USAGE = 2
EXIT_TRANSPORT = 3  # could not connect, or the pipe broke — the parent is gone or never came
EXIT_PROTO_UNSUPPORTED = 4  # docs/protocol.md §5 rejection; we told the peer before exiting
EXIT_MODEL_LOAD_FAILED = 5
EXIT_PROTOCOL_ERROR = 6  # L1/L2/L3 fault (§11.3), fatal
EXIT_INTERNAL = 7
EXIT_PEER_MISMATCH = 8  # --parent-pid did not match the pipe's server process

# docs/protocol.md §6.2 `ready.mode` — exactly the Phase 0A taxonomy (plan.md §3). A backend
# reporting anything else is a bug in that backend and must not reach the wire, because the
# whole point of the field is that latency numbers stay interpretable.
VALID_MODES = ("native-streaming", "stateful-chunked", "pseudo-streaming")

# How long we wait for the pipe to exist. The parent creates the instance *before* spawning us
# (docs/protocol.md §2.4 step 1), so this should never be needed; it exists so that a transient
# ERROR_PIPE_BUSY is retried rather than turned into a startup failure.
CONNECT_RETRY_MS = 5000

# Size of a single ReadFile. One byte over MAX_FRAME_ON_WIRE so that a conforming maximum frame
# arrives in one read even in message mode; oversize frames are rejected on the length prefix
# long before the payload matters (docs/protocol.md §3.3).
READ_CHUNK = 65541


# ─── Protocol binding ───────────────────────────────────────────────────────────────────────
#
# `kvit_asr.protocol` is written by a different author against the same frozen document. This
# block is the single place that touches it, so that a naming disagreement is one edit rather
# than a scatter of them, and so that a missing symbol is a clear message instead of an
# AttributeError from somewhere in the read loop.
#
# Two names are accepted in either of two plausible spellings (the frame-type constants and the
# reader's "give me the next frame" method). That tolerance is deliberate and bounded: it exists
# because the two modules are being implemented concurrently, and it should be collapsed to one
# spelling once protocol.py is fixed. Everything else is required as spelled.

from kvit_asr import protocol as proto  # noqa: E402  (imported after the docstring on purpose)


def _bind_frame_type(name: str, enum_member: str) -> int:
    value = getattr(proto, name, None)
    if value is None:
        enum = getattr(proto, "FrameType", None)
        value = getattr(enum, enum_member, None) if enum is not None else None
    if value is None:
        raise ImportError(
            f"kvit_asr.protocol exposes neither {name} nor FrameType.{enum_member}; "
            "docs/protocol.md §3.2 defines the two frame types and the sidecar cannot "
            "frame anything without them"
        )
    return int(value)


def _bind_required(*names: str) -> None:
    missing = [n for n in names if not hasattr(proto, n)]
    if missing:
        raise ImportError(
            "kvit_asr.protocol is missing "
            + ", ".join(missing)
            + ". kvit_asr.server implements docs/protocol.md §2/§5/§10 against that module and "
            "deliberately does not re-implement §1.1 constants or §3/§4 codecs — duplicating a "
            "normative constant is how two endpoints drift apart."
        )


_bind_required(
    "PROTO_VERSION",
    "MAX_PAYLOAD",
    "WRITE_DEADLINE_MS",
    "LOADING_HEARTBEAT_MS",
    "FrameReader",
    "encode_frame",
    "decode_control",
    "ProtocolError",
)

FRAME_CONTROL = _bind_frame_type("FRAME_CONTROL", "CONTROL")
FRAME_AUDIO = _bind_frame_type("FRAME_AUDIO", "AUDIO")
ProtocolError = proto.ProtocolError


def _next_frame(reader: Any) -> tuple[int, bytes] | None:
    """One frame from the reader, or None if it needs more bytes.

    Accepts `next_frame()` or `next()`, returning either a `(type, payload)` pair or an object
    with `.type` / `.payload` (the C++ side's `struct Frame`). See the note above.
    """
    getter = getattr(reader, "next_frame", None) or getattr(reader, "next", None)
    if getter is None:
        raise ImportError("kvit_asr.protocol.FrameReader has neither next_frame() nor next()")
    frame = getter()
    if frame is None:
        return None
    if isinstance(frame, tuple):
        ftype, payload = frame
    else:
        ftype, payload = frame.type, frame.payload
    return int(ftype), bytes(payload)


def decode_control_dict(payload: bytes) -> dict[str, Any]:
    """Validate one control payload and hand it back as a ``dict``.

    `kvit_asr.protocol.decode_control` returns a typed `Message` dataclass; this module and
    `kvit_asr.session` both speak plain dicts (`send()` takes one, `handle_control()` takes one,
    and the session emits them). Rather than teach four call sites two representations, the
    conversion happens once, here.

    **Validation is unchanged** and still belongs to `kvit_asr.protocol`: every §4 encoding rule
    and §6 schema rule is applied by `decode_control`, and a non-conforming payload still raises
    `ProtocolError` before anything below runs.

    The dict comes from the payload's own JSON rather than from `Message.to_json()`, because
    `to_json` omits fields that equal their defaults. The wire is authoritative — a receiver that
    reconstructs a message from the sender's defaults is answering a question nobody asked.

    A `dict` result is passed straight through, so this becomes the identity function on the day
    `decode_control` returns one.
    """
    msg = proto.decode_control(payload)
    if isinstance(msg, dict):
        return msg
    loads = getattr(proto, "strict_json_loads", None)
    obj = loads(payload) if loads is not None else json.loads(payload.decode("utf-8"))
    if not isinstance(obj, dict):  # pragma: no cover - decode_control rejects this first
        raise ProtocolError_new("bad_message", True, "control payload is not a JSON object")
    return obj


def encode_control_payload(msg: dict[str, Any]) -> bytes:
    """A control payload: compact UTF-8 JSON, raw (not \\u-escaped).

    docs/protocol.md §4.2: senders SHOULD emit raw UTF-8 rather than escapes, "because it makes
    `stable_bytes` offsets directly checkable against the wire bytes", and SHOULD emit compact
    JSON. `ensure_ascii=False` is what does that in Python, and it is the single most
    consequential JSON option in this file: with the default `True`, "café" would go out as
    `caf\\u00e9` and a reader checking an offset against the wire bytes would get a different
    answer than one checking against the decoded string.

    Encoding lives here rather than in protocol.py because it is three lines of stdlib and
    verifiable by inspection, while *decoding* — where the §4 restrictions bite — is shared.
    """
    return json.dumps(msg, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


# ─── Structured logging ─────────────────────────────────────────────────────────────────────

_LEVELS = {"debug": 10, "info": 20, "warning": 30, "error": 40}


class EventLogger:
    """One JSON object per line on stderr. Never the recognised text, unless asked."""

    def __init__(self, stream=None, level: str = "info", log_transcripts: bool = False) -> None:
        self._stream = stream if stream is not None else sys.stderr
        self._threshold = _LEVELS.get(level, 20)
        self._log_transcripts = log_transcripts
        self._lock = threading.Lock()
        self._t0 = time.monotonic()

    @property
    def log_transcripts(self) -> bool:
        return self._log_transcripts

    def redact(self, text: str | None) -> str | None:
        """Recognised text, or a description of it.

        plan.md §1.9: diagnostic logging of recognised text is opt-in. The byte count is kept
        because it is the field that matters when debugging `stable_bytes` arithmetic, and it
        discloses nothing about what was said.
        """
        if text is None:
            return None
        if self._log_transcripts:
            return text
        return f"<{len(text.encode('utf-8'))}B redacted>"

    def event(self, level: str, ev: str, **fields: Any) -> None:
        if _LEVELS.get(level, 20) < self._threshold:
            return
        record: dict[str, Any] = {
            "ts": round(time.time(), 3),
            "up_ms": int((time.monotonic() - self._t0) * 1000),
            "lvl": level,
            "ev": ev,
        }
        for key, value in fields.items():
            if value is not None:
                record[key] = value
        try:
            line = json.dumps(record, ensure_ascii=False, default=repr)
        except Exception:  # pragma: no cover - defensive; a log must never kill the server
            line = json.dumps({"lvl": "error", "ev": "log_encode_failed", "orig_ev": ev})
        with self._lock:
            try:
                self._stream.write(line + "\n")
                self._stream.flush()
            except Exception:  # pragma: no cover - stderr closed by the parent
                pass

    def debug(self, ev: str, **f: Any) -> None:
        self.event("debug", ev, **f)

    def info(self, ev: str, **f: Any) -> None:
        self.event("info", ev, **f)

    def warning(self, ev: str, **f: Any) -> None:
        self.event("warning", ev, **f)

    def error(self, ev: str, **f: Any) -> None:
        self.event("error", ev, **f)

    def exception(self, ev: str, exc: BaseException, **f: Any) -> None:
        """An unhandled fault, with its traceback, as one line.

        The traceback goes to the log as a field rather than to a bare stderr write, because a
        raw multi-line traceback interleaved with JSON lines is exactly what the parent's log
        capture cannot parse — and because a traceback on stderr with no error frame on the pipe
        is the failure mode this server is written to avoid.
        """
        self.event(
            "error",
            ev,
            exc_type=type(exc).__name__,
            exc_msg=str(exc),
            traceback="".join(traceback.format_exception(exc)).rstrip(),
            **f,
        )


# ─── Transport ──────────────────────────────────────────────────────────────────────────────


class TransportError(Exception):
    """The pipe is gone or unusable. Not a protocol error: there is nobody to tell."""


class WriteDeadlineExceeded(TransportError):
    """A write did not complete within WRITE_DEADLINE_MS (docs/protocol.md §9)."""


class Channel(Protocol):
    """A duplex byte channel. The only thing `Server` needs from a transport.

    Two implementations exist: `NamedPipeChannel` (production) and the in-memory pair in
    server/tests/test_server.py. The abstraction is not speculative generality — it is what lets
    the end-to-end test run without creating a pipe, a child process, or a firewall prompt.
    """

    def read(self, max_bytes: int) -> bytes:
        """Up to `max_bytes`. Returns b"" at end of stream. Raises TransportError on fault."""

    def write(self, data: bytes) -> None:
        """Write all of `data` as one operation. Raises TransportError / WriteDeadlineExceeded."""

    def close(self) -> None: ...

    def cancel(self) -> None:
        """Unblock a reader blocked in read(). Idempotent."""


# Win32 constants. Named rather than inlined so the values can be checked against the SDK.
_INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
_OPEN_EXISTING = 3
_FILE_FLAG_OVERLAPPED = 0x40000000
_PIPE_READMODE_MESSAGE = 0x00000002

# The exact access rights docs/protocol.md §2.3 step 2 says the parent's DACL grants, and no
# more. This is not a style preference and asking for GENERIC_READ | GENERIC_WRITE would fail:
#   * FILE_GENERIC_WRITE implies FILE_CREATE_PIPE_INSTANCE, which §2.3 deliberately withholds so
#     a client cannot create rival instances of the pipe name;
#   * FILE_GENERIC_READ implies FILE_READ_EA and READ_CONTROL, neither of which is in the ACE.
# CreateFileW asks for what it wants up front, so a superset request is denied outright — the
# failure would be ERROR_ACCESS_DENIED at connect time, and it would look like a broken DACL
# rather than an over-broad request. FILE_WRITE_ATTRIBUTES is required because we call
# SetNamedPipeHandleState (§2.3, final paragraph).
_FILE_READ_DATA = 0x0001
_FILE_WRITE_DATA = 0x0002
_FILE_READ_ATTRIBUTES = 0x0080
_FILE_WRITE_ATTRIBUTES = 0x0100
_SYNCHRONIZE = 0x00100000
_PIPE_ACCESS = (
    _FILE_READ_DATA | _FILE_WRITE_DATA | _FILE_READ_ATTRIBUTES | _FILE_WRITE_ATTRIBUTES | _SYNCHRONIZE
)

_ERROR_FILE_NOT_FOUND = 2
_ERROR_BROKEN_PIPE = 109
_ERROR_PIPE_BUSY = 231
_ERROR_PIPE_NOT_CONNECTED = 233
_ERROR_MORE_DATA = 234
_ERROR_OPERATION_ABORTED = 995
_ERROR_IO_PENDING = 997
_WAIT_OBJECT_0 = 0
_WAIT_TIMEOUT = 0x102


class _OVERLAPPED(ctypes.Structure):
    # The Offset/OffsetHigh pair is the first branch of the anonymous union; for a pipe both are
    # ignored, but the struct must still be the right size and hEvent must be at the right
    # offset. Written out rather than borrowed from ctypes.wintypes because wintypes does not
    # define OVERLAPPED.
    _fields_ = [
        ("Internal", ctypes.c_void_p),
        ("InternalHigh", ctypes.c_void_p),
        ("Offset", wintypes.DWORD),
        ("OffsetHigh", wintypes.DWORD),
        ("hEvent", wintypes.HANDLE),
    ]


def _kernel32() -> Any:
    """kernel32 with argtypes set. Called lazily so this module imports on non-Windows too
    (the unit tests for framing, routing and config do not need a pipe)."""
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateFileW.argtypes = [
        wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
        wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE,
    ]
    k32.CreateFileW.restype = wintypes.HANDLE
    k32.WaitNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD]
    k32.WaitNamedPipeW.restype = wintypes.BOOL
    k32.SetNamedPipeHandleState.argtypes = [
        wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD),
        ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(wintypes.DWORD),
    ]
    k32.SetNamedPipeHandleState.restype = wintypes.BOOL
    k32.ReadFile.argtypes = [
        wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(_OVERLAPPED),
    ]
    k32.ReadFile.restype = wintypes.BOOL
    k32.WriteFile.argtypes = [
        wintypes.HANDLE, wintypes.LPCVOID, wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(_OVERLAPPED),
    ]
    k32.WriteFile.restype = wintypes.BOOL
    k32.GetOverlappedResult.argtypes = [
        wintypes.HANDLE, ctypes.POINTER(_OVERLAPPED),
        ctypes.POINTER(wintypes.DWORD), wintypes.BOOL,
    ]
    k32.GetOverlappedResult.restype = wintypes.BOOL
    k32.CreateEventW.argtypes = [
        wintypes.LPVOID, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR,
    ]
    k32.CreateEventW.restype = wintypes.HANDLE
    k32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    k32.WaitForSingleObject.restype = wintypes.DWORD
    k32.ResetEvent.argtypes = [wintypes.HANDLE]
    k32.ResetEvent.restype = wintypes.BOOL
    k32.CancelIoEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(_OVERLAPPED)]
    k32.CancelIoEx.restype = wintypes.BOOL
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    k32.CloseHandle.restype = wintypes.BOOL
    return k32


class NamedPipeChannel:
    """The pipe client, over ctypes/kernel32. No pywin32.

    **Overlapped, and why it is not optional.** The pipe is duplex and both directions are live
    at once: the read loop is blocked in ReadFile while the decode thread wants to write a
    `partial`. On a *synchronous* handle Windows serialises operations on that handle, so a
    blocked read could hold up a write indefinitely; and docs/protocol.md §9 requires a write
    that has not completed within WRITE_DEADLINE_MS to be treated as overload, which a blocking
    WriteFile cannot express. Opening a second handle is not an option either: the parent
    creates the pipe with nMaxInstances = 1 (§2.2), so a second CreateFileW would get
    ERROR_PIPE_BUSY. Overlapped I/O with one event per direction is what is left, and it is
    what the C++ side does for the same reasons.

    **Unverified here:** whether SetNamedPipeHandleState reliably puts a CreateFileW handle into
    message read mode from ctypes is docs/protocol.md §14 open question 3. This class treats a
    failure as a warning, not an error, because the framing layer is a byte-stream decoder
    (§2.2) and therefore does not depend on the answer. ERROR_MORE_DATA is handled either way.
    """

    def __init__(self, handle: int, k32: Any, log: EventLogger | None = None) -> None:
        self._h = handle
        self._k32 = k32
        self._log = log
        self._closed = False
        self._write_lock = threading.Lock()
        self._read_ev = k32.CreateEventW(None, True, False, None)
        self._write_ev = k32.CreateEventW(None, True, False, None)
        if not self._read_ev or not self._write_ev:
            raise TransportError("CreateEventW failed")

    @classmethod
    def connect(
        cls, pipe_name: str, log: EventLogger | None = None, retry_ms: int = CONNECT_RETRY_MS
    ) -> "NamedPipeChannel":
        if sys.platform != "win32":  # pragma: no cover - the product is Windows-only
            raise TransportError("named pipes require Windows")
        k32 = _kernel32()
        deadline = time.monotonic() + retry_ms / 1000.0
        while True:
            handle = k32.CreateFileW(
                pipe_name, _PIPE_ACCESS, 0, None, _OPEN_EXISTING, _FILE_FLAG_OVERLAPPED, None
            )
            if handle and handle != _INVALID_HANDLE_VALUE:
                break
            err = ctypes.get_last_error()
            remaining_ms = int(max(0.0, deadline - time.monotonic()) * 1000)
            if err == _ERROR_PIPE_BUSY and remaining_ms > 0:
                # All instances busy. The parent creates the instance before spawning us, so
                # this is not expected; wait rather than fail, but say so.
                if log:
                    log.warning("pipe_busy", pipe=pipe_name, wait_ms=remaining_ms)
                k32.WaitNamedPipeW(pipe_name, min(remaining_ms, 1000))
                continue
            if err == _ERROR_FILE_NOT_FOUND and remaining_ms > 0:
                time.sleep(0.01)
                continue
            raise TransportError(
                f"CreateFileW({pipe_name!r}) failed, GetLastError={err}"
                + (
                    " (ERROR_ACCESS_DENIED — the pipe's DACL does not grant this logon session "
                    "the rights in docs/protocol.md §2.3)"
                    if err == 5
                    else ""
                )
            )

        chan = cls(handle, k32, log)
        mode = wintypes.DWORD(_PIPE_READMODE_MESSAGE)
        if not k32.SetNamedPipeHandleState(handle, ctypes.byref(mode), None, None):
            if log:
                log.warning(
                    "set_read_mode_failed",
                    err=ctypes.get_last_error(),
                    note="continuing: the frame decoder is byte-stream oriented "
                    "(docs/protocol.md §2.2), so message mode is an optimisation, not a "
                    "correctness requirement",
                )
        return chan

    def server_process_id(self) -> int | None:
        """PID of the process that created the pipe, or None if the API is unavailable.

        docs/protocol.md §2.4 step 5 and §14 open question 1: the mirror of
        GetNamedPipeClientProcessId is *believed* to exist in kernel32 on Vista and later, but
        the spec explicitly declines to assert its signature or availability, and neither do we.
        If it is absent, this returns None and the caller skips the check and logs that it did —
        the check being silently skipped is the outcome §14 says to avoid.
        """
        fn = getattr(self._k32, "GetNamedPipeServerProcessId", None)
        if fn is None:
            return None
        fn.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        fn.restype = wintypes.BOOL
        pid = wintypes.DWORD(0)
        if not fn(self._h, ctypes.byref(pid)):
            return None
        return int(pid.value)

    def read(self, max_bytes: int = READ_CHUNK) -> bytes:
        if self._closed:
            return b""
        buf = ctypes.create_string_buffer(max_bytes)
        nread = wintypes.DWORD(0)
        ov = _OVERLAPPED()
        ov.hEvent = self._read_ev
        self._k32.ResetEvent(self._read_ev)
        ok = self._k32.ReadFile(self._h, buf, max_bytes, ctypes.byref(nread), ctypes.byref(ov))
        if not ok:
            err = ctypes.get_last_error()
            if err == _ERROR_IO_PENDING:
                self._k32.WaitForSingleObject(self._read_ev, 0xFFFFFFFF)
                if not self._k32.GetOverlappedResult(
                    self._h, ctypes.byref(ov), ctypes.byref(nread), True
                ):
                    err = ctypes.get_last_error()
                    if err in (_ERROR_BROKEN_PIPE, _ERROR_PIPE_NOT_CONNECTED,
                               _ERROR_OPERATION_ABORTED):
                        return b""
                    if err != _ERROR_MORE_DATA:
                        raise TransportError(f"GetOverlappedResult(read) failed, err={err}")
            elif err in (_ERROR_BROKEN_PIPE, _ERROR_PIPE_NOT_CONNECTED, _ERROR_OPERATION_ABORTED):
                # End of stream. Windows signals it as an error, not as a zero-length read.
                return b""
            elif err == _ERROR_MORE_DATA:
                # Message longer than our buffer. §2.2: continue reading the remainder into the
                # same frame buffer — which is exactly what returning these bytes to the
                # stream-oriented decoder does.
                pass
            else:
                raise TransportError(f"ReadFile failed, err={err}")
        return buf.raw[: nread.value]

    def write(self, data: bytes) -> None:
        if self._closed:
            raise TransportError("write on a closed channel")
        deadline_ms = int(proto.WRITE_DEADLINE_MS)
        with self._write_lock:
            nwritten = wintypes.DWORD(0)
            ov = _OVERLAPPED()
            ov.hEvent = self._write_ev
            self._k32.ResetEvent(self._write_ev)
            ok = self._k32.WriteFile(
                self._h, data, len(data), ctypes.byref(nwritten), ctypes.byref(ov)
            )
            if not ok:
                err = ctypes.get_last_error()
                if err == _ERROR_IO_PENDING:
                    waited = self._k32.WaitForSingleObject(self._write_ev, deadline_ms)
                    if waited == _WAIT_TIMEOUT:
                        # §9: a write that misses WRITE_DEADLINE_MS is overload, not patience.
                        # Cancel first — abandoning an overlapped write whose buffer is about to
                        # go out of scope corrupts the process, not just the connection.
                        self._k32.CancelIoEx(self._h, ctypes.byref(ov))
                        self._k32.GetOverlappedResult(
                            self._h, ctypes.byref(ov), ctypes.byref(nwritten), True
                        )
                        raise WriteDeadlineExceeded(
                            f"write of {len(data)} bytes exceeded WRITE_DEADLINE_MS={deadline_ms}"
                        )
                    if waited != _WAIT_OBJECT_0:
                        raise TransportError(f"WaitForSingleObject(write) returned {waited}")
                    if not self._k32.GetOverlappedResult(
                        self._h, ctypes.byref(ov), ctypes.byref(nwritten), True
                    ):
                        raise TransportError(
                            f"GetOverlappedResult(write) failed, err={ctypes.get_last_error()}"
                        )
                elif err in (_ERROR_BROKEN_PIPE, _ERROR_PIPE_NOT_CONNECTED):
                    raise TransportError("broken pipe on write")
                else:
                    raise TransportError(f"WriteFile failed, err={err}")
            if nwritten.value != len(data):
                # A writer MUST issue exactly one write per frame (§2.2). A short write would
                # split a frame across writes, which is the thing that rule forbids.
                raise TransportError(f"short write: {nwritten.value} of {len(data)} bytes")

    def cancel(self) -> None:
        if not self._closed:
            self._k32.CancelIoEx(self._h, None)

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        self._k32.CancelIoEx(self._h, None)
        for handle in (self._read_ev, self._write_ev, self._h):
            if handle:
                self._k32.CloseHandle(handle)
        self._h = 0


# ─── Warm-up measurement ────────────────────────────────────────────────────────────────────

#: Fallback origin for `warm_ms`: the moment this module was imported.
_IMPORT_MONOTONIC = time.monotonic()


def process_age_ms() -> int:
    """Milliseconds since this *process* started, not since this module was imported.

    docs/protocol.md §6.2 defines `warm_ms` as "milliseconds from process start to ready", and
    it feeds the cold/warm start metric in plan.md §4. Timing from module import would silently
    omit interpreter startup and every import above — which for a torch backend is seconds, and
    is exactly the cost the metric is meant to expose. GetProcessTimes gives the real creation
    time. If it is unavailable (non-Windows, or the call fails) we fall back to import time and
    the number is an underestimate; the log records which origin was used so a bench result is
    never quietly comparing two different quantities.
    """
    if sys.platform == "win32":
        try:
            k32 = ctypes.WinDLL("kernel32", use_last_error=True)
            creation = wintypes.FILETIME()
            exited = wintypes.FILETIME()
            kernel = wintypes.FILETIME()
            user = wintypes.FILETIME()
            k32.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
            k32.GetProcessTimes.restype = wintypes.BOOL
            k32.GetCurrentProcess.restype = wintypes.HANDLE
            if k32.GetProcessTimes(
                k32.GetCurrentProcess(),
                ctypes.byref(creation), ctypes.byref(exited),
                ctypes.byref(kernel), ctypes.byref(user),
            ):
                now = wintypes.FILETIME()
                k32.GetSystemTimeAsFileTime.argtypes = [ctypes.POINTER(wintypes.FILETIME)]
                k32.GetSystemTimeAsFileTime(ctypes.byref(now))

                def as_int(ft: wintypes.FILETIME) -> int:
                    return (ft.dwHighDateTime << 32) | ft.dwLowDateTime

                # FILETIME is 100 ns ticks.
                return max(0, (as_int(now) - as_int(creation)) // 10_000)
        except Exception:  # pragma: no cover - defensive; never fail a handshake over a metric
            pass
    return int((time.monotonic() - _IMPORT_MONOTONIC) * 1000)


# ─── The server ─────────────────────────────────────────────────────────────────────────────


class _Rejected(Exception):
    """Handshake refused. Carries the exit code; the error frame has already been sent."""

    def __init__(self, exit_code: int) -> None:
        super().__init__(f"rejected, exit={exit_code}")
        self.exit_code = exit_code


class Server:
    """Connection lifetime: handshake, load, ready, then route until the pipe closes.

    Interfaces this class codes against, all of which live in modules owned by other authors.
    They are listed here because they are the seams where an integration mistake will show up:

    `backend_loader(backend_cfg, on_progress, log, allow_hostile) -> backend`
        Default `kvit_asr.connection.open_backend`, imported lazily. `on_progress(stage, percent)`
        may be called any number of times; each call becomes a `loading` message. The returned
        object exposes `model_id: str`, `mode: str` (one of VALID_MODES), and optionally
        `chunk_ms: int | None`, `context_supported: bool`, `close()`.

        `log` and `allow_hostile` are passed as keywords, so a loader that does not care may
        accept and ignore them — but it must accept them. They are not optional extras: two of
        `LoadedBackend`'s obligations are to log (an ignored `start.context`, an honoured
        `stable_bytes_override`), and `allow_hostile` is the gate on the latter.

    `session_factory(backend=..., send=..., config=..., log=...) -> session`
        Default `kvit_asr.connection.ConnectionSession` — **not** `kvit_asr.session.Session`,
        which is session-scoped (one per `start`) where this role is connection-scoped. The
        returned object exposes `handle_control(msg: dict) -> None`,
        `handle_audio(pcm: bytes) -> None`, `close()`, and optionally
        `current_session_id: str | None` and `connection_fatal: bool`. `send(msg: dict)` writes
        one control frame and is safe to call from any thread. The session raises
        `protocol.ProtocolError` for anything docs/protocol.md §11 names.
    """

    def __init__(
        self,
        cfg: ServerConfig,
        channel: Channel,
        *,
        log: EventLogger | None = None,
        backend_loader: Callable[..., Any] | None = None,
        session_factory: Callable[..., Any] | None = None,
    ) -> None:
        self.cfg = cfg
        self.channel = channel
        self.log = log or EventLogger(level=cfg.log_level, log_transcripts=cfg.log_transcripts)
        self._backend_loader = backend_loader
        self._session_factory = session_factory
        self._reader = proto.FrameReader()
        self._backend: Any = None
        self._session: Any = None
        self._loading_stage = "starting"
        self._heartbeat_stop = threading.Event()
        self._dead = False

    # ── outbound ────────────────────────────────────────────────────────────────────────────

    def send(self, msg: dict[str, Any]) -> None:
        """Write one control frame. Thread-safe (the channel serialises writes)."""
        payload = encode_control_payload(msg)
        if len(payload) > int(proto.MAX_PAYLOAD):
            # Our own message is too big for the wire. That is a local bug, and truncating it
            # would put a malformed JSON object on a stream the peer must be able to trust.
            raise ProtocolError_new(
                "internal_error", True, f"outbound {msg.get('t')} is {len(payload)} bytes"
            )
        self.channel.write(proto.encode_frame(FRAME_CONTROL, payload))
        self._log_message("tx", msg)

    def _log_message(self, direction: str, msg: dict[str, Any]) -> None:
        fields = {k: v for k, v in msg.items() if k not in ("text", "t")}
        if "text" in msg:
            fields["text"] = self.log.redact(msg.get("text"))
        self.log.debug(f"{direction}_{msg.get('t', '?')}", **fields)

    def _send_error(self, code: str, fatal: bool, msg: str, session_id: str | None = None,
                    supported: list[int] | None = None,
                    available: list[str] | None = None,
                    retry_after_ms: int | None = None) -> None:
        frame: dict[str, Any] = {"t": "error", "code": code, "fatal": fatal, "msg": msg}
        if session_id is not None:
            frame["session_id"] = session_id
        elif not fatal:
            # §11.1: session_id is REQUIRED when fatal is false. A non-fatal error without one
            # is unactionable — the peer cannot tell which session to abort.
            self.log.error("nonfatal_error_without_session", code=code)
            frame["fatal"] = True
        if supported is not None:
            frame["supported"] = supported
        # §5.1 with model_unavailable, §2.7 with server_busy. Both exist so the refusal names
        # what the user can do next instead of only what went wrong.
        if available is not None:
            frame["available"] = available
        if retry_after_ms is not None:
            frame["retry_after_ms"] = retry_after_ms
        try:
            self.send(frame)
        except TransportError as exc:
            self.log.warning("error_frame_unsent", code=code, reason=str(exc))

    # ── lifecycle ───────────────────────────────────────────────────────────────────────────

    def run(self) -> int:
        try:
            self._handshake()
            self._load_backend_and_report()
            self._serve()
            return EXIT_OK
        except _Rejected as exc:
            return exc.exit_code
        except TransportError as exc:
            # The parent is gone, or wedged. Nothing to report to and nobody to report it to;
            # exit promptly rather than sitting on a model that holds VRAM.
            self.log.error("transport_failed", reason=str(exc))
            return EXIT_TRANSPORT
        except ProtocolError as exc:
            code, fatal, message, sid = _error_fields(exc)
            self.log.error("protocol_error", code=code, fatal=fatal, msg=message, session_id=sid)
            self._best_effort_error(code, fatal, message, sid)
            return EXIT_PROTOCOL_ERROR
        except KeyboardInterrupt:  # pragma: no cover - interactive debugging only
            self.log.info("interrupted")
            return EXIT_OK
        except Exception as exc:
            # The rule this whole method exists for: an unhandled fault becomes a fatal
            # `internal_error` on the wire, never a traceback on stderr and a parent waiting for
            # a `ready` that will never come.
            self.log.exception("unhandled", exc)
            self._best_effort_error("internal_error", True, f"{type(exc).__name__}: {exc}", None)
            return EXIT_INTERNAL
        finally:
            self._teardown()

    def _best_effort_error(self, code: str, fatal: bool, msg: str, sid: str | None) -> None:
        """§11.2: one best-effort attempt to tell the peer, then close regardless."""
        try:
            self._send_error(code, fatal, msg, sid)
        except Exception as exc:  # pragma: no cover - the stream is already untrusted
            self.log.warning("error_frame_failed", code=code, reason=repr(exc))

    def _teardown(self) -> None:
        self._heartbeat_stop.set()
        for name, obj in (("session", self._session), ("backend", self._backend)):
            close = getattr(obj, "close", None) if obj is not None else None
            if close is not None:
                try:
                    close()
                except Exception as exc:
                    self.log.exception(f"{name}_close_failed", exc)
        try:
            self.channel.close()
        except Exception:  # pragma: no cover
            pass

    # ── §5 handshake ────────────────────────────────────────────────────────────────────────

    def _handshake(self) -> None:
        """Read exactly one frame, require it to be `hello`, answer or reject.

        §5: the Client MUST send `hello` as the first frame and the Server MUST send nothing
        else before it answers. Anything else here — audio, `start`, a second `hello` — is a
        fatal `state_violation`, because the handshake is what establishes that both peers agree
        on the message set.
        """
        for ftype, payload in self._frames(blocking_hint="hello"):
            if ftype != FRAME_CONTROL:
                raise ProtocolError_new(
                    "state_violation", True, "audio frame before hello (docs/protocol.md §5)"
                )
            msg = decode_control_dict(payload)
            self._log_message("rx", msg)
            if msg.get("t") != "hello":
                raise ProtocolError_new(
                    "state_violation", True, f"expected hello, got {msg.get('t')!r}"
                )
            offered = msg.get("proto")
            supported = int(proto.PROTO_VERSION)
            if offered != supported:
                # §5: reply proto_unsupported and close. No silent downgrade, no best-effort
                # continuation — a version we do not implement is a message set we cannot parse.
                self.log.error("proto_unsupported", offered=offered, supported=supported)
                self._send_error(
                    "proto_unsupported",
                    True,
                    f"this sidecar speaks proto {supported}, the client offered {offered!r}",
                    supported=[supported],
                )
                raise _Rejected(EXIT_PROTO_UNSUPPORTED)
            if self.cfg.proto is not None and self.cfg.proto != offered:
                # Not fatal: the command line is advisory, the wire is authoritative (§5). But
                # a parent whose --proto disagrees with its own hello has a bug worth seeing.
                self.log.warning("proto_arg_mismatch", arg=self.cfg.proto, hello=offered)

            # §5.1. One backend per process, so the catalogue has exactly one entry and the only
            # question is whether the client asked for something else. Refusing is the whole
            # point: loading a different model than the one requested produces transcripts that
            # are merely *worse*, which is the hardest kind of wrong for a user to notice.
            served = self._served_model_ids()
            requested = msg.get("model")
            if requested is not None and requested not in served:
                self.log.error("model_unavailable", requested=requested, available=sorted(served))
                self._send_error(
                    "model_unavailable",
                    True,
                    f"this server serves {sorted(served)[0]!r}, not {requested!r}",
                    available=sorted(served),
                )
                raise _Rejected(EXIT_PROTO_UNSUPPORTED)

            self.send({
                "t": "hello_ack",
                "proto": supported,
                "server": kd_config.SERVER_BUILD_ID,
                # `mode` is deliberately absent: it is not known until the backend has loaded,
                # and §5.1 makes every field but `id` optional precisely so a server can say
                # what it knows now rather than guess. `ready.mode` is authoritative anyway.
                "models": [{"id": sorted(served)[0], "context_supported": False}],
            })
            self.log.info("handshake_ok", proto=supported, client=msg.get("client"),
                          model=requested)
            return
        raise TransportError("end of stream during handshake")

    def _served_model_ids(self) -> set[str]:
        """Every id a client may legitimately name in `hello.model` for this process.

        Both spellings are accepted because both are in circulation: `models.json` keys entries
        by backend id, while `ready.model` reports the model id, and they differ for every
        backend that takes a `--model`. Refusing one of them would be a distinction the user
        never agreed to make.
        """
        ids = {self.cfg.backend.id}
        if self.cfg.backend.model:
            ids.add(self.cfg.backend.model)
        return ids

    # ── loading → ready ─────────────────────────────────────────────────────────────────────

    def _load_backend_and_report(self) -> None:
        model_id = self.cfg.backend.model or self.cfg.backend.id
        self._emit_loading(model_id, "import")
        heartbeat = threading.Thread(
            target=self._loading_heartbeat, args=(model_id,), name="loading-heartbeat", daemon=True
        )
        heartbeat.start()
        t_load = time.monotonic()
        try:
            self._backend = self._load_backend(model_id)
        except Exception as exc:
            self._heartbeat_stop.set()
            self.log.exception("model_load_failed", exc, backend=self.cfg.backend.id)
            # §11.3: model_load_failed is fatal and the Client surfaces `msg` verbatim, so the
            # message must name the thing a human can fix — the backend, the model, the
            # exception — and not just "load failed".
            self._send_error(
                "model_load_failed",
                True,
                f"backend {self.cfg.backend.id!r} model {model_id!r}: "
                f"{type(exc).__name__}: {exc}",
            )
            raise _Rejected(EXIT_MODEL_LOAD_FAILED) from exc
        finally:
            self._heartbeat_stop.set()
        load_ms = int((time.monotonic() - t_load) * 1000)

        mode = getattr(self._backend, "mode", None)
        if mode not in VALID_MODES:
            raise ProtocolError_new(
                "internal_error",
                True,
                f"backend reported mode {mode!r}; docs/protocol.md §6.2 allows only "
                f"{VALID_MODES}",
            )
        ready: dict[str, Any] = {
            "t": "ready",
            "model": str(getattr(self._backend, "model_id", model_id)),
            "warm_ms": process_age_ms(),
            "mode": mode,
        }
        chunk_ms = getattr(self._backend, "chunk_ms", None)
        if chunk_ms is not None:
            ready["chunk_ms"] = int(chunk_ms)
        # §6.1: a Server without context support MUST report context_supported:false rather than
        # silently ignoring `start.context`, because plan.md §4 compares context-enabled and
        # context-free runs separately and a silent drop would corrupt that comparison.
        ready["context_supported"] = bool(getattr(self._backend, "context_supported", False))

        self._session = self._make_session()
        self.send(ready)
        self.log.info(
            "ready", model=ready["model"], warm_ms=ready["warm_ms"], load_ms=load_ms, mode=mode,
            context_supported=ready["context_supported"],
        )

    def _emit_loading(self, model_id: str, stage: str, progress: int | None = None) -> None:
        self._loading_stage = stage
        msg: dict[str, Any] = {"t": "loading", "model": model_id, "stage": stage}
        if progress is not None:
            msg["progress"] = int(progress)
        self.send(msg)

    def _loading_heartbeat(self, model_id: str) -> None:
        """A `loading` at least every LOADING_HEARTBEAT_MS (§6.6).

        The Client's READY_TIMEOUT_MS restarts on each `loading`, so this is what stops a cold
        model load that legitimately takes minutes from being killed as a hang.
        """
        interval = int(proto.LOADING_HEARTBEAT_MS) / 1000.0
        while not self._heartbeat_stop.wait(interval):
            try:
                self._emit_loading(model_id, self._loading_stage)
            except Exception as exc:
                # The pipe died while loading. Nothing to do from this thread; the main thread
                # will hit the same failure on its next write and exit.
                self.log.warning("heartbeat_failed", reason=repr(exc))
                return

    def _load_backend(self, model_id: str) -> Any:
        def on_progress(stage: str, progress: int | None = None) -> None:
            self._emit_loading(model_id, stage, progress)

        loader = self._backend_loader
        if loader is None:
            # Lazy, and inside the try/except in the caller: a backend whose runtime is missing
            # (no onnxruntime, no torch) must produce `model_load_failed` for *that* backend
            # rather than an ImportError at package import time that takes the whole server down.
            from kvit_asr.connection import open_backend as loader  # type: ignore[no-redef]

        return loader(
            self.cfg.backend,
            on_progress=on_progress,
            log=self.log,
            allow_hostile=self.cfg.allow_hostile_backend,
        )

    def _make_session(self) -> Any:
        factory = self._session_factory
        if factory is None:
            # `kvit_asr.session.Session` is session-scoped and this is a connection-scoped role;
            # ConnectionSession is the object between them. See kvit_asr/connection.py's header.
            from kvit_asr.connection import ConnectionSession as factory  # type: ignore[no-redef]

        return factory(
            backend=self._backend, send=self.send, config=self.cfg, log=self.log
        )

    # ── routing ─────────────────────────────────────────────────────────────────────────────

    def _frames(self, blocking_hint: str = "") -> Iterator[tuple[int, bytes]]:
        """Frames off the wire, forever, until end of stream.

        Reads, pushes, then drains completely: fixture 002 (three frames in one read) fails any
        caller that handles one frame per read.
        """
        while True:
            frame = _next_frame(self._reader)
            while frame is not None:
                yield frame
                frame = _next_frame(self._reader)
            data = self.channel.read(READ_CHUNK)
            if not data:
                self._reader.notify_eof()
                # Drain what the buffer still holds; a partial frame here raises
                # truncated_frame from the decoder (fixture 011), an empty buffer is an orderly
                # shutdown (fixture 010).
                frame = _next_frame(self._reader)
                while frame is not None:
                    yield frame
                    frame = _next_frame(self._reader)
                self.log.info("end_of_stream", waiting_for=blocking_hint or None)
                return
            self._reader.push(data)

    def _serve(self) -> None:
        for ftype, payload in self._frames():
            try:
                self._dispatch(ftype, payload)
            except ProtocolError as exc:
                code, fatal, message, sid = _error_fields(exc)
                self.log.error("protocol_error", code=code, fatal=fatal, msg=message,
                               session_id=sid)
                self._send_error(code, fatal, message, sid)
                if fatal:
                    raise _Rejected(EXIT_PROTOCOL_ERROR) from exc
            except (TransportError, _Rejected):
                # _Rejected means a decision has already been made and reported (a fatal peer
                # error, say). Letting it fall into the generic handler below would rewrite a
                # deliberate teardown as an internal fault.
                raise
            except Exception as exc:
                # An unhandled backend fault. §11.3 gives it the right shape: if a session is
                # open it is `decode_failed`, non-fatal — the model misbehaved on this
                # utterance, the process is fine, and the next session may well work — and the
                # session is closed with `stopped` so the Client's state machine does not hang
                # in FINALIZING. With no session open there is nothing to abort but the
                # connection, so it is a fatal `internal_error`.
                sid = getattr(self._session, "current_session_id", None)
                self.log.exception("backend_fault", exc, session_id=sid)
                if sid:
                    self._send_error("decode_failed", False, f"{type(exc).__name__}: {exc}", sid)
                    self._abort_session(sid)
                else:
                    self._send_error("internal_error", True, f"{type(exc).__name__}: {exc}")
                    raise _Rejected(EXIT_INTERNAL) from exc

    def _abort_session(self, session_id: str) -> None:
        """Close a session the backend broke, so the Client is not left waiting for `stopped`."""
        abort = getattr(self._session, "abort", None)
        if abort is not None:
            try:
                abort(session_id, reason="error")
                return
            except Exception as exc:
                self.log.exception("session_abort_failed", exc, session_id=session_id)
        self.send({"t": "stopped", "session_id": session_id, "reason": "error"})

    def _dispatch(self, ftype: int, payload: bytes) -> None:
        if ftype == FRAME_AUDIO:
            if len(payload) % 2:
                # §6.3: an odd payload means the sender split a 16-bit sample, i.e. its buffer
                # arithmetic is wrong. Fatal, not rounded down.
                raise ProtocolError_new(
                    "bad_message", True, f"audio payload has odd length {len(payload)}"
                )
            self._session.handle_audio(payload)
            self._check_session_fatal()
            return
        if ftype != FRAME_CONTROL:  # pragma: no cover - the decoder rejects unknown types first
            raise ProtocolError_new("unknown_frame_type", True, f"frame type {ftype:#04x}")

        msg = decode_control_dict(payload)
        self._log_message("rx", msg)
        t = msg.get("t")
        if t == "hello":
            raise ProtocolError_new("state_violation", True, "second hello on one connection")
        if t in ("hello_ack", "loading", "ready", "partial", "final", "stopped"):
            # A known message type that only the Server may send. Not `unknown_message` — we
            # know exactly what it is; it is illegal in this direction (§11.3 state_violation).
            raise ProtocolError_new("state_violation", True, f"{t} is a Server→Client message")
        if t == "error":
            self._handle_peer_error(msg)
            return
        self._session.handle_control(msg)
        self._check_session_fatal()

    def _check_session_fatal(self) -> None:
        """End the connection when a session reports a §9/§11.1 fault that is not survivable.

        `overload_audio` is the case that exists today: the session has already written the
        `error` frame with `fatal:true` and, correctly, no `stopped` — §11.1 says the connection
        is finished, so there is no session teardown left to acknowledge. All that remains is to
        stop reading, and doing it here rather than inside the session is what keeps connection
        lifetime in one place.
        """
        if getattr(self._session, "connection_fatal", False):
            self.log.error("session_reported_connection_fatal")
            raise _Rejected(EXIT_PROTOCOL_ERROR)

    def _handle_peer_error(self, msg: dict[str, Any]) -> None:
        """§11.1/§13: act on `fatal`, even for a code this build does not recognise.

        The Client sends `stability_violation` (fatal:false) when our own `stable_bytes` broke
        the §8.2 invariant. That is the ICER-relevant event in plan.md §1.4, so it is logged at
        error level with the session id attached, and the session is closed — the connection is
        not, because the next session may be fine.
        """
        code = str(msg.get("code", ""))
        fatal = bool(msg.get("fatal", True))
        sid = msg.get("session_id")
        self.log.error("peer_error", code=code, fatal=fatal, msg=msg.get("msg"), session_id=sid)
        if fatal:
            raise _Rejected(EXIT_PROTOCOL_ERROR)
        if sid:
            handler = getattr(self._session, "handle_control", None)
            if handler is not None:
                handler(msg)
            else:  # pragma: no cover - session always has handle_control
                self._abort_session(str(sid))


def ProtocolError_new(code: str, fatal: bool, msg: str, session_id: str | None = None):
    """Build a `protocol.ProtocolError` without assuming its constructor signature.

    `kvit_asr.protocol` owns that exception. It is constructed here in three places, and its
    signature is the sort of thing that differs harmlessly between two authors, so the
    construction is funnelled through one function that tries the keyword form first and falls
    back to a positional message. Named oddly on purpose: it is a shim, and it should be deleted
    once protocol.py's constructor is settled.
    """
    try:
        return ProtocolError(code=code, fatal=fatal, msg=msg, session_id=session_id)
    except TypeError:
        exc = ProtocolError(f"{code}: {msg}")
        exc.code = code  # type: ignore[attr-defined]
        exc.fatal = fatal  # type: ignore[attr-defined]
        exc.msg = msg  # type: ignore[attr-defined]
        exc.session_id = session_id  # type: ignore[attr-defined]
        return exc


def _error_fields(exc: BaseException) -> tuple[str, bool, str, str | None]:
    """(code, fatal, msg, session_id) from a ProtocolError, with sane defaults.

    Defaults matter: an exception that forgot to say whether it was fatal is treated as fatal,
    because continuing on a stream we may have mis-parsed is the outcome §3.3 forbids.
    """
    # `code_raw` first, and it is not a stylistic preference: `ProtocolError.code` is an
    # `ErrorCode` member of a `(str, Enum)` mixin rather than a `StrEnum`, so `str()` on it yields
    # "ErrorCode.state_violation". §11.3 defines `code` as the stable machine-readable identifier
    # the peer branches on, and §13 tells a receiver to treat an unrecognised one as an error about
    # an error — so that spelling would put every error this server raises outside the catalogue.
    # `code_raw` is the wire spelling, kept by ProtocolError for exactly this.
    code = str(
        getattr(exc, "code_raw", None) or getattr(exc, "code", None) or "internal_error"
    )
    fatal = bool(getattr(exc, "fatal", True))
    msg = str(getattr(exc, "msg", None) or str(exc))
    sid = getattr(exc, "session_id", None)
    return code, fatal, msg, (str(sid) if sid else None)


# ─── Entry point ────────────────────────────────────────────────────────────────────────────


def _serve_websocket(cfg: ServerConfig, log: EventLogger) -> int:
    """docs/protocol.md §2.6: listen, and give each accepted connection its own `Server`.

    Every connection is wholly independent (§2.7) — its own handshake, its own `ready`, its own
    single session, its own `session_id` space. Nothing is shared and nothing is keyed on
    `session_id`, which is only unique within one client's process (§7.1) and will collide the
    first time two people start the app on the same afternoon: both begin at `s1`.

    This function does not return until the process is interrupted. Its exit code is about the
    *listener*, not about any one connection — a client that fails its handshake is that
    client's problem, and a server that exited because somebody spoke proto 2 at it would be a
    denial of service with extra steps.
    """
    from . import ws as ws_mod

    host, port = cfg.listen
    if host not in ("127.0.0.1", "::1", "localhost"):
        # §2.8, and worth a line in the log every single time rather than once in a README:
        # there is no authentication, and ws:// is unencrypted.
        log.warning(
            "listening_on_a_routable_address",
            host=host,
            note="there is no authentication and ws:// is unencrypted (docs/protocol.md §2.8): "
            "anyone who can reach this port can send audio, and anyone on the path can read it",
        )

    def refuse(conn: Any) -> None:
        # §2.7: refuse in words, after reading `hello`, instead of `hello_ack`. Silence and a
        # connection that never becomes ready both present to the user as "it hangs", which is
        # the failure this project spends the most effort not producing.
        message = conn.recv_message()
        if message is None:
            return
        conn.send_binary(
            proto.encode_control_frame(
                proto.ErrorMessage(
                    code=proto.ErrorCode.server_busy.value,
                    fatal=True,
                    msg=f"this server accepts {cfg.max_connections} connection(s) at a time",
                    retry_after_ms=5000,
                )
            )
        )

    def serve(channel: Any, peer: str) -> None:
        log.info("client_connected", peer=peer)
        try:
            code = Server(cfg, channel, log=log).run()
        except Exception as exc:  # pragma: no cover - a bug here must not kill the listener
            log.exception("connection_crashed", exc, peer=peer)
            code = EXIT_INTERNAL
        log.info("client_disconnected", peer=peer, exit_code=code)

    try:
        ws_mod.serve_forever(
            host,
            port,
            serve,
            max_connections=cfg.max_connections,
            log=log,
            on_refused=refuse,
        )
    except KeyboardInterrupt:
        log.info("stopping")
    except OSError as exc:
        log.error("listen_failed", host=host, port=port, reason=str(exc))
        return EXIT_TRANSPORT
    return 0


def main(argv: list[str] | None = None, *, channel: Channel | None = None) -> int:
    """Parse, connect, run. Returns the process exit code.

    `channel` is injected by the tests; in production the sidecar always connects to the pipe
    name the parent generated and passed on the command line.
    """
    cfg = kd_config.parse_args(argv)
    log = EventLogger(level=cfg.log_level, log_transcripts=cfg.log_transcripts)
    changed = kd_config.apply_offline_policy()
    log.info(
        "start",
        pid=os.getpid(),
        python=sys.version.split()[0],
        backend=cfg.backend.id,
        model=cfg.backend.model,
        model_dir=str(cfg.backend.model_dir) if cfg.backend.model_dir else None,
        pipe=cfg.pipe_name,
        offline_env=sorted(changed) or None,
        log_transcripts=cfg.log_transcripts or None,
    )
    if cfg.log_transcripts:
        log.warning(
            "transcript_logging_enabled",
            note="recognised text will be written to stderr; plan.md §1.9 wants this off "
            "outside a debugging session",
        )

    if channel is None and cfg.listen is not None:
        return _serve_websocket(cfg, log)

    if channel is None:
        try:
            channel = NamedPipeChannel.connect(cfg.pipe_name, log)
        except TransportError as exc:
            log.error("connect_failed", pipe=cfg.pipe_name, reason=str(exc))
            return EXIT_TRANSPORT
        if cfg.parent_pid is not None:
            actual = channel.server_process_id()
            if actual is None:
                log.warning(
                    "parent_pid_check_skipped",
                    expected=cfg.parent_pid,
                    note="GetNamedPipeServerProcessId unavailable — docs/protocol.md §14 "
                    "open question 1; the check is skipped, not silently passed",
                )
            elif actual != cfg.parent_pid:
                log.error("peer_mismatch", expected=cfg.parent_pid, actual=actual)
                channel.close()
                return EXIT_PEER_MISMATCH
            else:
                log.info("peer_validated", parent_pid=actual)

    return Server(cfg, channel, log=log).run()
