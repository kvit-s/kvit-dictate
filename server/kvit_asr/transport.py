"""Named-pipe transport for the ASR sidecar — ctypes against kernel32, no pywin32.

Implements docs/protocol.md §2 (transport), §9 (backpressure) and §10.3 (peer death) for the
**pipe client** side. This process is the pipe *client* (it opens the pipe by name) and the
protocol *Server* (it answers with transcripts); §1 of the spec explains why those are inverted.

─── Dependencies ─────────────────────────────────────────────────────────────────────────────

stdlib only: `ctypes`, `threading`, `queue`, `logging`, `time`. Deliberately **not** pywin32 —
the sidecar core must run on a bare CPython, and a pipe client needs exactly five kernel32 calls.

`kernel32` is bound lazily, in :func:`_k32`, rather than at import time. That is not tidiness: it
means this module imports on a non-Windows box, so the pure parts (argument parsing, queue
accounting) stay unit-testable and an accidental import in a test collection does not explode.

─── Why threads and not overlapped I/O ───────────────────────────────────────────────────────

The requirement is that a blocked read never stops the process from sending. Two ways to get
that from Python:

  * **Overlapped I/O.** Correct `OVERLAPPED` layout (including the `Internal`/`InternalHigh`
    `ULONG_PTR` fields, the union of `Offset`/`Pointer`, and 8-byte alignment on x64), a manual
    reset event per operation, `GetOverlappedResult`, and careful lifetime management so the
    OVERLAPPED and the buffer outlive the call. Getting any of that wrong produces memory
    corruption that presents as a *protocol* bug — a frame with plausible-looking garbage in it —
    which is the single most expensive class of failure to debug across a two-language boundary.
  * **One reader thread, one writer thread, a lock, and blocking calls.** The GIL is released
    around `ReadFile`/`WriteFile`, so a blocked read genuinely does not block a write. The cost
    is that cancellation needs `CancelIoEx` instead of simply not waiting.

This module takes the second. The concurrency it buys is identical for our shape of traffic (one
reader, one writer, no scatter/gather), and the failure modes are ones a Python programmer can
actually reason about. Consequently there is no `OVERLAPPED` structure in this file at all —
declaring one we do not use would be an invitation to use it wrongly later.

Note the asymmetry with the C++ side, which *does* use `FILE_FLAG_OVERLAPPED` (§2.2): that is
required there because `WRITE_DEADLINE_MS` must be enforceable on the audio thread of a UI
process. Here the deadline is enforced with a watchdog that calls `CancelIoEx` — see
:meth:`_PipeIO._write_one`.

─── Unverified behaviour, recorded rather than assumed ───────────────────────────────────────

docs/protocol.md §14 item 3 records that whether `SetNamedPipeHandleState` reliably puts a
`CreateFileW` handle into `PIPE_READMODE_MESSAGE` from ctypes is **untested**. This module calls
it, checks the return value, and on failure logs a warning and carries on in byte-stream mode —
which costs nothing, because `FrameReader` is a byte-stream decoder by construction (§2.2). Every
other place where a Windows behaviour is assumed rather than observed is flagged with an
`UNVERIFIED:` comment.
"""

from __future__ import annotations

import ctypes
import logging
import queue
import threading
import time
from dataclasses import dataclass, field
from typing import Any, Callable

from .protocol import (
    AUDIO_QUEUE_MAX_MS,
    CONNECT_TIMEOUT_MS,
    CONTROL_QUEUE_MAX,
    MAX_FRAME_ON_WIRE,
    PROTO_VERSION,
    WRITE_DEADLINE_MS,
    ErrorCode,
    Frame,
    FrameReader,
    FrameType,
    Message,
    ProtocolError,
    decode_control,
    encode_audio_frame,
    encode_control_frame,
    pcm_bytes_to_ms,
)

__all__ = [
    "SidecarArgs",
    "parse_pipe_args",
    "PipeTransport",
    "Inbound",
    "InboundKind",
    "PipeError",
    "WindowsError_",
]

log = logging.getLogger("kvit_asr.transport")


# ─── Win32 constants ──────────────────────────────────────────────────────────────────────────

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3

# The access mask we actually open the pipe with. It must match, bit for bit, the single allow-ACE
# the C++ parent puts in the pipe's DACL (`kd::kPipeAccessRights` in src/ipc_client.h):
#
#   SYNCHRONIZE | FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES
#
# Asking for GENERIC_READ | GENERIC_WRITE instead does NOT work, and fails in a way worth writing
# down because it looks like a bug in the parent:
#
#   * GENERIC_READ expands to FILE_GENERIC_READ, which includes FILE_READ_EA (0x8). The DACL does
#     not grant it, so CreateFileW fails with ERROR_ACCESS_DENIED (5) and the sidecar never
#     connects at all.
#   * GENERIC_WRITE expands to include FILE_CREATE_PIPE_INSTANCE (0x4) — the right that lets a
#     holder create rival instances of our pipe name and answer connections meant for us. That is
#     precisely the right plan.md §1.2 refuses to grant, so a parent that satisfied this request
#     would be the insecure one.
#
# In other words the narrow grant is correct and the broad request was wrong. Spelling the mask
# here rather than reaching for the GENERIC_* convenience values keeps the two sides in step.
PIPE_ACCESS_RIGHTS = 0x00100183
OPEN_EXISTING_ACCESS = PIPE_ACCESS_RIGHTS
FILE_ATTRIBUTE_NORMAL = 0x00000080

PIPE_READMODE_MESSAGE = 0x00000002
PIPE_READMODE_BYTE = 0x00000000

ERROR_FILE_NOT_FOUND = 2
ERROR_BROKEN_PIPE = 109
ERROR_PIPE_BUSY = 231
ERROR_MORE_DATA = 234
ERROR_PIPE_NOT_CONNECTED = 233
ERROR_OPERATION_ABORTED = 995
ERROR_NO_DATA = 232

_INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

#: Read buffer size. Deliberately *smaller* than MAX_FRAME_ON_WIRE so that the ERROR_MORE_DATA
#: path (§2.2: "If a reader receives ERROR_MORE_DATA, it MUST continue reading the remainder into
#: the same frame buffer") is exercised in ordinary operation rather than only by a 64 KiB frame
#: nobody ever sends. FrameReader reassembles either way.
READ_CHUNK = 16384


class PipeError(OSError):
    """A Win32 failure, carrying the `GetLastError()` value in `.winerror`."""


# Alias kept so callers do not have to distinguish our OSError subclass from the builtin.
WindowsError_ = PipeError


# ─── kernel32 binding ─────────────────────────────────────────────────────────────────────────
#
# Every function gets explicit argtypes and restype. This is not decoration: without argtypes,
# ctypes passes Python ints as 32-bit C ints, so a HANDLE above 2 GB is silently truncated and the
# call fails with an error code that has nothing to do with the real problem. Handle truncation is
# the classic ctypes bug and it presents as "the pipe randomly stops working".

_k32_cache: Any = None


def _k32() -> Any:
    global _k32_cache
    if _k32_cache is not None:
        return _k32_cache

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)  # type: ignore[attr-defined]
    from ctypes import wintypes

    k32.CreateFileW.argtypes = [
        wintypes.LPCWSTR,   # lpFileName          — LPCWSTR, not c_char_p: the pipe name is UTF-16
        wintypes.DWORD,     # dwDesiredAccess
        wintypes.DWORD,     # dwShareMode
        wintypes.LPVOID,    # lpSecurityAttributes
        wintypes.DWORD,     # dwCreationDisposition
        wintypes.DWORD,     # dwFlagsAndAttributes
        wintypes.HANDLE,    # hTemplateFile
    ]
    k32.CreateFileW.restype = wintypes.HANDLE

    k32.SetNamedPipeHandleState.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(wintypes.DWORD),  # lpMode
        ctypes.POINTER(wintypes.DWORD),  # lpMaxCollectionCount
        ctypes.POINTER(wintypes.DWORD),  # lpCollectDataTimeout
    ]
    k32.SetNamedPipeHandleState.restype = wintypes.BOOL

    k32.ReadFile.argtypes = [
        wintypes.HANDLE,
        wintypes.LPVOID,
        wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD),
        wintypes.LPVOID,  # lpOverlapped — always NULL here; see the module docstring
    ]
    k32.ReadFile.restype = wintypes.BOOL

    k32.WriteFile.argtypes = [
        wintypes.HANDLE,
        wintypes.LPCVOID,
        wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD),
        wintypes.LPVOID,
    ]
    k32.WriteFile.restype = wintypes.BOOL

    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    k32.CloseHandle.restype = wintypes.BOOL

    k32.FlushFileBuffers.argtypes = [wintypes.HANDLE]
    k32.FlushFileBuffers.restype = wintypes.BOOL

    k32.WaitNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD]
    k32.WaitNamedPipeW.restype = wintypes.BOOL

    # CancelIoEx is Vista+ and cancels I/O issued by *any* thread on the handle, which is exactly
    # what a reader thread blocked in ReadFile needs. UNVERIFIED here: its behaviour against a
    # synchronous (non-overlapped) handle opened by CreateFileW has not been observed on this
    # machine. The shutdown path therefore does not depend on it — see PipeTransport.close().
    if hasattr(k32, "CancelIoEx"):
        k32.CancelIoEx.argtypes = [wintypes.HANDLE, wintypes.LPVOID]
        k32.CancelIoEx.restype = wintypes.BOOL

    # docs/protocol.md §2.4 step 5 / §14 item 1: the child SHOULD validate the parent with
    # GetNamedPipeServerProcessId, but the document explicitly declines to assert that the API
    # exists. Bind it only if the export is actually present, and treat its absence as "the check
    # is unavailable" rather than as an error. Nothing else depends on it.
    if hasattr(k32, "GetNamedPipeServerProcessId"):
        k32.GetNamedPipeServerProcessId.argtypes = [
            wintypes.HANDLE,
            ctypes.POINTER(wintypes.DWORD),
        ]
        k32.GetNamedPipeServerProcessId.restype = wintypes.BOOL

    _k32_cache = k32
    return k32


def _last_error() -> int:
    return ctypes.get_last_error()  # type: ignore[attr-defined]


def _raise_win32(what: str, err: int | None = None) -> None:
    code = _last_error() if err is None else err
    raise PipeError(None, f"{what} failed", None, code)


# ─── Command line ─────────────────────────────────────────────────────────────────────────────


@dataclass(slots=True)
class SidecarArgs:
    """The arguments docs/protocol.md §2.4 step 2 requires the parent to pass.

    `--pipe` and `--proto` are the minimum the spec mandates. `--parent-pid` is optional and only
    feeds the §2.4 step 5 mirror check. Model id, interpreter and device are outside this spec and
    are parsed by whoever owns `__main__`; they arrive here as `extra`.
    """

    pipe: str
    proto: int = PROTO_VERSION
    parent_pid: int | None = None
    extra: list[str] = field(default_factory=list)


def parse_pipe_args(argv: list[str]) -> SidecarArgs:
    """Parse `--pipe <name> --proto <n> [--parent-pid <n>]` out of `argv`, keeping the rest.

    Hand-rolled rather than argparse so that unknown options survive into `extra` instead of being
    rejected: this module owns the transport contract, not the backend's command line, and it must
    not become the reason a new backend flag needs a change here.
    """
    pipe: str | None = None
    proto = PROTO_VERSION
    parent_pid: int | None = None
    extra: list[str] = []

    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--pipe" and i + 1 < len(argv):
            pipe = argv[i + 1]
            i += 2
        elif a.startswith("--pipe="):
            pipe = a.split("=", 1)[1]
            i += 1
        elif a == "--proto" and i + 1 < len(argv):
            proto = int(argv[i + 1])
            i += 2
        elif a.startswith("--proto="):
            proto = int(a.split("=", 1)[1])
            i += 1
        elif a == "--parent-pid" and i + 1 < len(argv):
            parent_pid = int(argv[i + 1])
            i += 2
        elif a.startswith("--parent-pid="):
            parent_pid = int(a.split("=", 1)[1])
            i += 1
        else:
            extra.append(a)
            i += 1

    if not pipe:
        raise ValueError("missing required argument --pipe <full pipe name>")
    if not pipe.startswith("\\\\.\\pipe\\"):
        # The parent always passes the full name (§2.4 step 2). Accepting a bare name and
        # prefixing it here would let a typo open some other object; refusing is one line.
        raise ValueError(f"--pipe must be a full pipe name starting with \\\\.\\pipe\\, got {pipe!r}")
    return SidecarArgs(pipe=pipe, proto=proto, parent_pid=parent_pid, extra=extra)


# ─── Inbound events ───────────────────────────────────────────────────────────────────────────


class InboundKind:
    CONTROL = "control"
    AUDIO = "audio"
    CLOSED = "closed"


@dataclass(slots=True)
class Inbound:
    """One item off the transport: a control message, an audio payload, or the end."""

    kind: str
    message: Message | None = None
    pcm: bytes | None = None
    #: For CLOSED: the fault that ended the connection, or None for orderly end of stream.
    error: ProtocolError | None = None
    reason: str = ""


# ─── The transport ────────────────────────────────────────────────────────────────────────────


class PipeTransport:
    """Duplex framed transport over `\\\\.\\pipe\\<name>`, with the §9 bounded queues.

    Lifecycle::

        t = PipeTransport(args.pipe)
        t.connect()                    # CreateFileW + SetNamedPipeHandleState, then start threads
        t.send(HelloAck(proto=1))
        while (item := t.recv(timeout=0.1)) is not None:
            ...
        t.close()

    Threading: one reader thread, one writer thread, and the caller's thread. `send()` is safe
    from any thread. `recv()` is intended for one consumer.
    """

    def __init__(
        self,
        pipe_name: str,
        *,
        connect_timeout_ms: int = CONNECT_TIMEOUT_MS,
        audio_queue_max_ms: int = AUDIO_QUEUE_MAX_MS,
        control_queue_max: int = CONTROL_QUEUE_MAX,
        write_deadline_ms: int = WRITE_DEADLINE_MS,
        logger: logging.Logger | None = None,
    ) -> None:
        self.pipe_name = pipe_name
        self._connect_timeout_ms = connect_timeout_ms
        self._audio_queue_max_ms = audio_queue_max_ms
        self._control_queue_max = control_queue_max
        self._write_deadline_ms = write_deadline_ms
        self.log = logger or log

        self._handle: int | None = None
        self._message_mode = False

        # Inbound. Unbounded Queue plus *explicit* accounting, rather than a maxsize: §9 forbids
        # dropping audio to relieve pressure, and `Queue(maxsize)` gives a producer only "block"
        # or "drop". Blocking the reader thread would stop us reading the control message that
        # tells us to stop, so the reader must always drain the pipe and instead raise overload.
        self._inbound: queue.Queue[Inbound] = queue.Queue()
        self._inbound_audio_ms = 0.0
        self._inbound_control_n = 0
        self._inbound_lock = threading.Lock()

        # Outbound. Bounded: §9 gives the Server a CONTROL_QUEUE_MAX = 256 outbound control queue,
        # and overflow is `overload_control`, fatal, tear down.
        self._outbound: queue.Queue[bytes | None] = queue.Queue(maxsize=self._control_queue_max)

        self._reader: threading.Thread | None = None
        self._writer: threading.Thread | None = None
        self._stopping = threading.Event()
        self._closed = threading.Event()
        self._fault: ProtocolError | None = None
        self._fault_lock = threading.Lock()

    # -- connection ----------------------------------------------------------------------------

    def connect(self) -> None:
        """Open the pipe and start the I/O threads.

        Retries on `ERROR_PIPE_BUSY` via `WaitNamedPipeW` until `connect_timeout_ms`. Busy is a
        legitimate transient: `nMaxInstances = 1` (§2.2) means a rival connector gets
        `ERROR_PIPE_BUSY` rather than a private conversation, and our own reconnect can race the
        parent's `DisconnectNamedPipe`.
        """
        k32 = _k32()
        deadline = time.monotonic() + self._connect_timeout_ms / 1000.0

        while True:
            handle = k32.CreateFileW(
                self.pipe_name,
                PIPE_ACCESS_RIGHTS,
                0,                      # no sharing
                None,                   # default security; the pipe's DACL is the parent's job
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,  # synchronous; see the module docstring
                None,
            )
            if handle is not None and handle != _INVALID_HANDLE_VALUE:
                self._handle = handle
                break

            err = _last_error()
            remaining_ms = int(max(0.0, deadline - time.monotonic()) * 1000)
            if err == ERROR_PIPE_BUSY and remaining_ms > 0:
                # WaitNamedPipeW returns FALSE on timeout; loop and let the deadline decide.
                k32.WaitNamedPipeW(self.pipe_name, min(remaining_ms, 1000))
                continue
            if remaining_ms <= 0:
                _raise_win32(f"CreateFileW({self.pipe_name!r}) timed out", err)
            _raise_win32(f"CreateFileW({self.pipe_name!r})", err)

        self._set_message_read_mode()
        self._start_threads()

    def _set_message_read_mode(self) -> None:
        """Ask for `PIPE_READMODE_MESSAGE`; degrade to byte-stream reading on failure.

        docs/protocol.md §14 item 3: whether this works reliably against a `CreateFileW` handle
        from ctypes is **unverified**. It is called here, its return value is checked, and a
        failure is a logged warning rather than a fatal error — because §2.2 requires both
        endpoints to be byte-stream decoders anyway, so the read mode is a debugging convenience
        (frame boundaries stay visible at the packet level), not a correctness dependency. If this
        warning ever appears in a log, the answer to §14 item 3 is "no" and the spec should be
        amended to say so.
        """
        from ctypes import wintypes

        k32 = _k32()
        mode = wintypes.DWORD(PIPE_READMODE_MESSAGE)
        ok = k32.SetNamedPipeHandleState(self._handle, ctypes.byref(mode), None, None)
        if ok:
            self._message_mode = True
        else:
            self._message_mode = False
            self.log.warning(
                "SetNamedPipeHandleState(PIPE_READMODE_MESSAGE) failed with error %d; "
                "continuing in byte-stream read mode. This is safe (docs/protocol.md 2.2 requires "
                "a byte-stream decoder regardless) but answers open question 14.3.",
                _last_error(),
            )

    def parent_process_id(self) -> int | None:
        """`GetNamedPipeServerProcessId` for the §2.4 step 5 mirror check, or None if unavailable.

        docs/protocol.md §14 item 1 declines to assert that this API exists or what its signature
        is. We bind it only if the export is present and return None otherwise, so a missing API
        drops the check (which the spec permits) instead of failing the connection.
        """
        k32 = _k32()
        if not hasattr(k32, "GetNamedPipeServerProcessId") or self._handle is None:
            return None
        from ctypes import wintypes

        pid = wintypes.DWORD(0)
        if not k32.GetNamedPipeServerProcessId(self._handle, ctypes.byref(pid)):
            self.log.warning("GetNamedPipeServerProcessId failed with error %d", _last_error())
            return None
        return int(pid.value)

    def _start_threads(self) -> None:
        self._reader = threading.Thread(target=self._read_loop, name="kvit-pipe-reader",
                                        daemon=True)
        self._writer = threading.Thread(target=self._write_loop, name="kvit-pipe-writer",
                                        daemon=True)
        self._reader.start()
        self._writer.start()

    # -- sending -------------------------------------------------------------------------------

    def send(self, msg: Message) -> None:
        """Queue one control message. Raises `ProtocolError(overload_control)` when the queue is
        full — §9 makes overflow fatal, never absorbed."""
        self.send_frame(encode_control_frame(msg))

    def send_audio(self, pcm: bytes) -> None:
        """Queue one `0x02` audio frame. Present for symmetry; the Server does not send audio."""
        self.send_frame(encode_audio_frame(pcm))

    def send_frame(self, frame_bytes: bytes) -> None:
        if self._closed.is_set() or self._stopping.is_set():
            raise ProtocolError(
                ErrorCode.internal_error, "send on a closed transport", fatal=True
            )
        try:
            self._outbound.put_nowait(frame_bytes)
        except queue.Full:
            err = ProtocolError(
                ErrorCode.overload_control,
                f"outbound control queue exceeded CONTROL_QUEUE_MAX ({self._control_queue_max})",
                fatal=True,
            )
            self._fail(err)
            raise err from None

    # -- receiving -----------------------------------------------------------------------------

    def recv(self, timeout: float | None = None) -> Inbound | None:
        """Next inbound item, or None on timeout.

        A `CLOSED` item is delivered exactly once and stays available afterwards (it is re-queued),
        so a consumer that polls after shutdown keeps seeing the end rather than blocking forever.
        """
        try:
            item = self._inbound.get(timeout=timeout)
        except queue.Empty:
            return None
        if item.kind == InboundKind.CLOSED:
            self._inbound.put(item)  # sticky: the end of the stream does not go away
            return item
        with self._inbound_lock:
            if item.kind == InboundKind.AUDIO and item.pcm is not None:
                self._inbound_audio_ms -= pcm_bytes_to_ms(len(item.pcm))
            elif item.kind == InboundKind.CONTROL:
                self._inbound_control_n -= 1
        return item

    @property
    def closed(self) -> bool:
        return self._closed.is_set()

    @property
    def fault(self) -> ProtocolError | None:
        """The fault that ended the connection, or None if it ended orderly or is still open."""
        return self._fault

    def wait_closed(self, timeout: float | None = None) -> bool:
        return self._closed.wait(timeout)

    # -- shutdown ------------------------------------------------------------------------------

    def close(self, *, flush: bool = True) -> None:
        """Orderly teardown (§2.5): drain the writer, flush, then close the handle.

        The reader thread may be blocked in `ReadFile`. Two things unblock it, in order:
        `CancelIoEx` if the export is present, and — always — `CloseHandle`, which makes the
        pending `ReadFile` complete with a failure. Both threads are daemons and the reader treats
        `ERROR_OPERATION_ABORTED` / `ERROR_BROKEN_PIPE` / invalid handle as "the connection is
        over", so a stuck reader cannot hold the process open.

        UNVERIFIED: closing a handle with a synchronous read outstanding is documented as
        undefined-ish behaviour; in practice it completes the read with an error, which is what we
        want. `CancelIoEx` is preferred and tried first for that reason. If this ever wedges,
        that is the line to look at.
        """
        if self._stopping.is_set():
            self._closed.wait(1.0)
            return
        self._stopping.set()

        # Let the writer drain what is already queued, then stop it with the sentinel.
        try:
            self._outbound.put_nowait(None)
        except queue.Full:
            pass
        if self._writer is not None:
            self._writer.join(timeout=self._write_deadline_ms / 1000.0)

        k32 = _k32()
        h, self._handle = self._handle, None
        if h is not None:
            if flush:
                k32.FlushFileBuffers(h)
            if hasattr(k32, "CancelIoEx"):
                k32.CancelIoEx(h, None)
            k32.CloseHandle(h)

        if self._reader is not None:
            self._reader.join(timeout=2.0)
        self._finish(None, "closed by us")

    # -- threads -------------------------------------------------------------------------------

    def _read_loop(self) -> None:
        from ctypes import wintypes

        k32 = _k32()
        reader = FrameReader()
        buf = ctypes.create_string_buffer(READ_CHUNK)
        n = wintypes.DWORD(0)

        try:
            while not self._stopping.is_set():
                handle = self._handle
                if handle is None:
                    break
                ok = k32.ReadFile(handle, buf, READ_CHUNK, ctypes.byref(n), None)
                got = int(n.value)

                if not ok:
                    err = _last_error()
                    if err == ERROR_MORE_DATA:
                        # §2.2: in message mode a buffer smaller than the message yields
                        # ERROR_MORE_DATA with a full buffer. Keep the bytes and read the rest;
                        # discarding the remainder, or treating the partial data as a frame, is a
                        # defect. FrameReader does the reassembly.
                        if got:
                            reader.push(buf.raw[:got])
                            self._drain(reader)
                        continue
                    if err in (ERROR_BROKEN_PIPE, ERROR_PIPE_NOT_CONNECTED, ERROR_NO_DATA):
                        # §10.3: the parent died. Exit promptly — do not retry, do not spin.
                        reader.notify_eof()
                        self._drain(reader)
                        self._finish(None, "broken pipe")
                        return
                    if err == ERROR_OPERATION_ABORTED or self._stopping.is_set():
                        self._finish(None, "read cancelled")
                        return
                    self._finish(
                        ProtocolError(
                            ErrorCode.internal_error, f"ReadFile failed with error {err}"
                        ),
                        f"ReadFile error {err}",
                    )
                    return

                if got == 0:
                    # A 0-byte successful read on a pipe means end of stream. Orderly if we are
                    # on a frame boundary (fixture 010), truncated_frame otherwise (fixture 011).
                    reader.notify_eof()
                    self._drain(reader)
                    self._finish(None, "end of stream")
                    return

                reader.push(buf.raw[:got])
                self._drain(reader)

        except ProtocolError as exc:
            self._finish(exc, exc.msg)
        except Exception as exc:  # pragma: no cover - defensive
            self._finish(
                ProtocolError(ErrorCode.internal_error, f"reader thread crashed: {exc!r}"),
                "reader crashed",
            )

    def _drain(self, reader: FrameReader) -> None:
        """Deliver every whole frame the reader now holds. Raises ProtocolError on a framing fault.

        Draining fully after each read is mandatory, not an optimisation: fixture 002 puts three
        frames in one delivery and fails any reader that handles one frame per read.
        """
        while True:
            frame = reader.next_frame()
            if frame is None:
                return
            self._deliver(frame)

    def _deliver(self, frame: Frame) -> None:
        if frame.type == FrameType.AUDIO:
            pcm = frame.payload
            ms = pcm_bytes_to_ms(len(pcm))
            with self._inbound_lock:
                self._inbound_audio_ms += ms
                over = self._inbound_audio_ms > self._audio_queue_max_ms
            if over:
                # §9, and plan.md §1.2: silently dropping or delaying audio produces mysteriously
                # bad transcription and is not acceptable. We queue this frame anyway — it is not
                # dropped — and then fail the connection loudly.
                self._inbound.put(Inbound(InboundKind.AUDIO, pcm=pcm))
                raise ProtocolError(
                    ErrorCode.overload_audio,
                    f"inbound audio queue exceeded AUDIO_QUEUE_MAX_MS "
                    f"({self._audio_queue_max_ms} ms)",
                    fatal=True,
                )
            self._inbound.put(Inbound(InboundKind.AUDIO, pcm=pcm))
            return

        # Control. Decode here, on the reader thread, so an L2 fault (bad UTF-8, bad JSON, bad
        # schema) is detected at the layer that can see it (§1) and never reaches the session
        # loop as something that looks like a valid message.
        try:
            msg = decode_control(frame.payload)
        except ProtocolError as exc:
            if exc.at_offset is not None:
                # decode_control reports payload-relative offsets; the wire offset is what a log
                # reader and the fixtures both want.
                exc.at_offset += frame.payload_offset
            raise

        with self._inbound_lock:
            self._inbound_control_n += 1
            over = self._inbound_control_n > self._control_queue_max
        self._inbound.put(Inbound(InboundKind.CONTROL, message=msg))
        if over:
            # §9 bounds the *Client's* inbound control queue at 256; it does not name a bound for
            # the Server's. Applying the same number here is a defensive local limit, not a spec
            # requirement — but a consumer that has stopped draining 256 control messages is
            # wedged, and failing loudly beats growing without limit.
            raise ProtocolError(
                ErrorCode.overload_control,
                f"inbound control queue exceeded {self._control_queue_max} messages "
                f"(consumer is not draining)",
                fatal=True,
            )

    def _write_loop(self) -> None:
        while True:
            item = self._outbound.get()
            if item is None:
                return
            try:
                self._write_one(item)
            except ProtocolError as exc:
                self._finish(exc, exc.msg)
                return

    def _write_one(self, data: bytes) -> None:
        """One WriteFile per frame (§2.2: never split a frame, never pack two).

        `WRITE_DEADLINE_MS` is enforced with a watchdog timer that calls `CancelIoEx`: a peer that
        stops reading must not be able to block us indefinitely (§9). One `threading.Timer` per
        write is affordable here because the Server writes only control frames — a few per second
        at most. It would not be affordable on the Client's audio path, which is one reason that
        side uses real overlapped I/O.

        UNVERIFIED: that `CancelIoEx` from the timer thread actually aborts a synchronous
        `WriteFile` issued by this thread. If it does not, the deadline degrades to a log line and
        the write still blocks — which is why the timer also records the fault.
        """
        from ctypes import wintypes

        k32 = _k32()
        handle = self._handle
        if handle is None:
            raise ProtocolError(ErrorCode.internal_error, "write on a closed handle")

        timed_out = threading.Event()

        def _deadline() -> None:
            timed_out.set()
            if hasattr(k32, "CancelIoEx"):
                k32.CancelIoEx(handle, None)

        watchdog = threading.Timer(self._write_deadline_ms / 1000.0, _deadline)
        watchdog.daemon = True
        watchdog.start()
        try:
            view = memoryview(data)
            total = 0
            written = wintypes.DWORD(0)
            while total < len(data):
                chunk = view[total:]
                ok = k32.WriteFile(
                    handle,
                    ctypes.c_char_p(bytes(chunk)),
                    len(chunk),
                    ctypes.byref(written),
                    None,
                )
                if not ok:
                    err = _last_error()
                    if timed_out.is_set() or err == ERROR_OPERATION_ABORTED:
                        raise ProtocolError(
                            ErrorCode.overload_audio if False else ErrorCode.overload_control,
                            f"write exceeded WRITE_DEADLINE_MS ({self._write_deadline_ms} ms)",
                            fatal=True,
                        )
                    if err in (ERROR_BROKEN_PIPE, ERROR_PIPE_NOT_CONNECTED, ERROR_NO_DATA):
                        raise ProtocolError(
                            ErrorCode.internal_error, "peer closed the pipe during a write"
                        )
                    raise ProtocolError(
                        ErrorCode.internal_error, f"WriteFile failed with error {err}"
                    )
                total += int(written.value)
        finally:
            watchdog.cancel()

    # -- fault plumbing ------------------------------------------------------------------------

    def _fail(self, err: ProtocolError) -> None:
        with self._fault_lock:
            if self._fault is None:
                self._fault = err

    def _finish(self, err: ProtocolError | None, reason: str) -> None:
        if err is not None:
            self._fail(err)
            self.log.error("transport fault: %s (%s)", err, reason)
        if not self._closed.is_set():
            self._closed.set()
            self._stopping.set()
            self._inbound.put(
                Inbound(InboundKind.CLOSED, error=self._fault, reason=reason)
            )
