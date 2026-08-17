r"""End-to-end tests for :mod:`kvit_asr.server` — docs/protocol.md §2, §5, §6, §10, §11.

Run with::

    C:/Python312/python.exe -m unittest discover -s server/tests -t server

stdlib ``unittest`` only, and the mock backend, so the whole thing runs on a bare interpreter with
no model, no GPU and no network.

─── Two transports, on purpose ────────────────────────────────────────────────────────────────

* :class:`MemoryChannel` — an in-memory duplex byte channel. ``kvit_asr.server``'s own ``Channel``
  docstring names "the in-memory pair in server/tests/test_server.py" as the second implementation
  of that protocol, so this is the seam it was designed against. Every routing, handshake,
  lifecycle and error test uses it, because those tests are about *messages*, and a pipe adds
  nothing to them but a way to hang.
* :class:`TestOverARealNamedPipe` — one class that creates a real ``\\.\pipe\...`` instance with
  ``CreateNamedPipeW`` and runs the same handshake/session/teardown through
  ``NamedPipeChannel``. Nothing else in the repository exercises that class at all, and the
  ctypes handle plumbing it contains is exactly the sort of code that is correct in review and
  wrong at runtime.

**Everything is bounded.** Every read has a deadline, every thread join has a timeout, and the
in-memory streams treat a stalled peer as end-of-stream after :data:`STALL_TIMEOUT` rather than
waiting forever. A hanging pipe test that blocks CI forever is worse than a failing one: the
failing one tells you something.

─── What this file no longer bridges ──────────────────────────────────────────────────────────

It used to carry three integration shims, because three seams between finished modules did not
meet and the end-to-end path could not otherwise be tested at all. All three are now module code
and the shims are gone:

1. ``decode_control`` returning a ``Message`` where ``server.py`` wanted a ``dict`` —
   ``server.decode_control_dict``.
2. the missing **connection-scoped** session object between ``server.Server`` and the
   session-scoped ``session.Session`` — ``connection.ConnectionSession``.
3. the backend interface mismatch, and a loader that took a name and returned a class where the
   server passes a ``BackendConfig`` and expects a loaded instance —
   ``connection.LoadedBackend`` and ``connection.open_backend``.

Two knobs are still injected here, and both are injected *through* the real classes rather than
around them: :func:`mock_backend_loader` builds a ``MockBackend`` with test-chosen settings and
wraps it in the real ``LoadedBackend``, and the harness passes ``ConnectionSession`` a ticking
clock (see :class:`TickingClock`) so a session's worth of audio fed in a microsecond can still
mature a T-millisecond stability hold.
"""

import ctypes
import io
import json
import os
import secrets
import sys
import threading
import time
import unittest
import unittest.mock

_SERVER_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _SERVER_DIR not in sys.path:
    sys.path.insert(0, _SERVER_DIR)

from kvit_asr import protocol as proto  # noqa: E402
from kvit_asr import server as server_mod  # noqa: E402
from kvit_asr import session as session_mod  # noqa: E402
from kvit_asr.backends.base import ResetReason  # noqa: E402
from kvit_asr.backends.mock import MockBackend, MockConfig  # noqa: E402
from kvit_asr.config import BackendConfig, ServerConfig  # noqa: E402
from kvit_asr.config import SessionConfig as CliSessionConfig  # noqa: E402
from kvit_asr.connection import ConnectionSession, LoadedBackend  # noqa: E402
from kvit_asr.protocol import StabilityCheck, check_stability  # noqa: E402
from kvit_asr.server import EventLogger, Server, TransportError  # noqa: E402
from kvit_asr.stability import StabilityParams  # noqa: E402

# Deadlines. Generous enough that a loaded machine does not fail spuriously, short enough that a
# wedged run is over in seconds rather than at the CI job's own timeout.
RECV_TIMEOUT = 5.0
JOIN_TIMEOUT = 10.0
STALL_TIMEOUT = 20.0

CHUNK_MS = 240
FRAME_CONTROL = int(proto.FrameType.CONTROL)
FRAME_AUDIO = int(proto.FrameType.AUDIO)


def speech(ms: int = CHUNK_MS, amp: int = 5000) -> bytes:
    """Non-silent 16 kHz mono s16le PCM (docs/protocol.md §6.3)."""
    import array

    n = 16000 * ms // 1000
    return array.array("h", [amp if i % 2 else -amp for i in range(n)]).tobytes()


def wire_code(err: dict) -> str:
    """``error.code`` as docs/protocol.md §11.3 spells it: the bare machine-readable identifier.

    This used to *normalise* ``"ErrorCode.state_violation"`` away, working around
    ``_error_fields`` calling ``str()`` on a ``(str, Enum)`` member. ``_error_fields`` now uses
    ``ProtocolError.code_raw``, so the helper asserts instead of repairing — a regression would
    otherwise be quietly absorbed at all thirteen call sites and only :class:`TestErrorCodeSpelling`
    would notice.
    """
    code = err["code"]
    assert not code.startswith("ErrorCode."), (
        f"error.code reached the wire as the repr of a Python enum member ({code!r}); §11.3 "
        "wants the bare identifier and §13 tells a receiver to treat anything else as an error "
        "about an error"
    )
    return code


# ─── Test-chosen settings, injected through the real classes ───────────────────────────────────


class TickingClock:
    """A monotonic-ms clock that advances by ``step`` on every read.

    plan.md §1.4 step 5 requires a stability candidate to be *unchanged* for T ms before it is
    eligible. A streaming decoder grows its candidate on every hypothesis, so the hold only ever
    matures on a re-evaluation with no new evidence — which is what ``Session.tick`` is for. A
    real clock would work too, at the cost of sleeping T ms per hypothesis in every test.
    """

    def __init__(self, step: int = 1000) -> None:
        self.now = 0
        self.step = step

    def __call__(self) -> int:
        self.now += self.step
        return self.now


def ticking_router(**kwargs):
    """``connection.ConnectionSession`` with a :class:`TickingClock`. The default factory here."""
    return ConnectionSession(clock=TickingClock(), **kwargs)


def mock_backend_loader(mock_config=None, **facade_overrides):
    """A ``backend_loader`` for ``Server``: a :class:`MockBackend` in the real
    :class:`~kvit_asr.connection.LoadedBackend`.

    ``connection.open_backend`` would do this too, but it builds the mock from
    ``--backend-opt`` strings; these tests want a ``MockConfig`` object (hostile modes, seeded
    scripts, an injected decode failure). ``facade_overrides`` writes over the facade's declared
    fields afterwards — that is how a backend reporting an illegal ``ready.mode`` is tested
    without a backend that lies.
    """

    def loader(backend_cfg, on_progress=None):
        if on_progress is not None:
            on_progress("weights", 50)
        mock = MockBackend(mock_config or MockConfig(seed=17))
        mock.load()
        facade = LoadedBackend(mock)
        facade.mock = mock  # the tests reach through to the mock's own state
        for k, v in facade_overrides.items():
            setattr(facade, k, v)
        return facade

    return loader


# ─── In-memory transport ────────────────────────────────────────────────────────────────────────


class _Stream:
    """One direction of a duplex channel: an unbounded byte buffer with an EOF flag.

    ``read`` blocks until bytes arrive, the writer closes, or ``STALL_TIMEOUT`` elapses. That last
    clause is the whole safety story of this file: a stalled peer becomes an orderly end of stream,
    so the reader unwinds and the test fails on an assertion instead of hanging.
    """

    def __init__(self) -> None:
        self._buf = bytearray()
        self._closed = False
        self._cv = threading.Condition()

    def write(self, data: bytes) -> None:
        with self._cv:
            if self._closed:
                raise TransportError("write on a closed stream")
            self._buf += data
            self._cv.notify_all()

    def read(self, max_bytes: int, timeout: float = STALL_TIMEOUT) -> bytes:
        deadline = time.monotonic() + timeout
        with self._cv:
            while not self._buf and not self._closed:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return b""
                self._cv.wait(min(remaining, 0.2))
            if not self._buf:
                return b""
            out = bytes(self._buf[:max_bytes])
            del self._buf[:max_bytes]
            return out

    def close(self) -> None:
        with self._cv:
            self._closed = True
            self._cv.notify_all()


class MemoryChannel:
    """``kvit_asr.server.Channel`` over a pair of :class:`_Stream` objects."""

    def __init__(self, inbound: _Stream, outbound: _Stream) -> None:
        self._in = inbound
        self._out = outbound

    def read(self, max_bytes: int = 65541) -> bytes:
        return self._in.read(max_bytes)

    def write(self, data: bytes) -> None:
        self._out.write(data)

    def close(self) -> None:
        self._out.close()

    def cancel(self) -> None:
        self._in.close()


class ClientEnd:
    """The test's half of the connection: the protocol **Client** (plus a frame decoder)."""

    def __init__(self, to_server: _Stream, from_server: _Stream) -> None:
        self._to = to_server
        self._from = from_server
        self._reader = proto.FrameReader()
        self._eof = False

    # -- writing --------------------------------------------------------------------------------

    def send(self, msg: dict) -> None:
        payload = json.dumps(msg, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self._to.write(proto.encode_frame(FRAME_CONTROL, payload))

    def send_raw(self, frame_type: int, payload: bytes) -> None:
        self._to.write(proto.encode_frame(frame_type, payload))

    def send_audio(self, pcm: bytes) -> None:
        self._to.write(proto.encode_frame(FRAME_AUDIO, pcm))

    def close_write(self) -> None:
        """Orderly end of stream at a frame boundary (fixture 010)."""
        self._to.close()

    # -- reading --------------------------------------------------------------------------------

    def recv(self, timeout: float = RECV_TIMEOUT):
        """One control message, or None at end of stream / on timeout."""
        deadline = time.monotonic() + timeout
        while True:
            frame = self._reader.next_frame()
            if frame is not None:
                if frame.type != FRAME_CONTROL:
                    raise AssertionError(f"server sent a {frame.type:#04x} frame")
                return server_mod.decode_control_dict(frame.payload)
            if self._eof:
                return None
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            data = self._from.read(65541, timeout=remaining)
            if not data:
                self._eof = True
                self._reader.notify_eof()
                continue
            self._reader.push(data)

    def drain(self, timeout: float = RECV_TIMEOUT) -> list:
        out = []
        while True:
            msg = self.recv(timeout=timeout)
            if msg is None:
                return out
            out.append(msg)


# ─── Harness ────────────────────────────────────────────────────────────────────────────────────


class ServerHarness:
    """A ``Server`` on a thread, a ``ClientEnd`` in the test, and a bounded teardown."""

    #: The real router, with a ticking clock. Two tests' worth of reasoning, in one place:
    #: :class:`TickingClock` because these tests feed a session's worth of audio in a microsecond
    #: and a real clock would leave every candidate inside its T-ms hold, and ``vad="none"``
    #: because :func:`speech` starts at sample zero — the energy endpointer calibrates its noise
    #: floor from the opening 200 ms, so a stream that opens *on* speech calibrates the floor to
    #: the speech and hears nothing after it. VAD gating has its own tests, which lead with
    #: silence; everything else here is about the protocol, not the detector.
    DEFAULT_CFG = ServerConfig(
        pipe_name=r"\\.\pipe\kvit-dictate-inmemory",
        backend=BackendConfig(id="mock", model="mock"),
        session=CliSessionConfig(vad="none"),
    )

    def __init__(self, *, backend_loader=None, session_factory=None, cfg=None) -> None:
        c2s, s2c = _Stream(), _Stream()
        self.client = ClientEnd(c2s, s2c)
        self.stderr = io.StringIO()
        self.cfg = cfg or self.DEFAULT_CFG
        self.server = Server(
            self.cfg,
            MemoryChannel(c2s, s2c),
            log=EventLogger(stream=self.stderr, level="debug"),
            backend_loader=backend_loader or mock_backend_loader(),
            session_factory=session_factory or ticking_router,
        )
        self.exit_code = None
        self._thread = threading.Thread(
            target=self._run, name="kvit-test-server", daemon=True
        )
        self._thread.start()

    def _run(self) -> None:
        try:
            self.exit_code = self.server.run()
        except BaseException as exc:  # pragma: no cover - a crash out of run() is itself a bug
            self.exit_code = exc

    def finish(self, join_timeout: float = JOIN_TIMEOUT):
        """Close the write side, wait for the server to exit, return its exit code."""
        self.client.close_write()
        self._thread.join(timeout=join_timeout)
        if self._thread.is_alive():
            raise AssertionError("server thread did not exit; see STALL_TIMEOUT")
        if isinstance(self.exit_code, BaseException):
            raise self.exit_code
        return self.exit_code

    @property
    def log_lines(self) -> list:
        return [ln for ln in self.stderr.getvalue().splitlines() if ln.strip()]


class ServerTestCase(unittest.TestCase):
    """Base: guarantees a bounded teardown for whatever the test started."""

    def setUp(self) -> None:
        self.harness = None

    def start(self, **kwargs) -> ServerHarness:
        self.harness = ServerHarness(**kwargs)
        self.addCleanup(self._shutdown)
        return self.harness

    def _shutdown(self) -> None:
        if self.harness is not None:
            try:
                self.harness.finish()
            except AssertionError:
                raise
            except Exception:
                pass

    # -- conversation helpers -------------------------------------------------------------------

    #: Message types ``expect`` steps over on the way to its target. ``loading`` is a heartbeat
    #: whose count is not the Server's to promise (§6.2), and ``partial``/``final`` are the text
    #: stream, which the tests that care about it read with ``recv``/``drain`` instead. Anything
    #: else arriving where a specific message was expected is a failure, not noise.
    PASSTHROUGH = ("loading", "partial", "final")

    def expect(self, client: ClientEnd, t: str, timeout: float = RECV_TIMEOUT):
        """The next message of type ``t``."""
        skipped = []
        deadline = time.monotonic() + timeout
        while True:
            msg = client.recv(timeout=max(0.0, deadline - time.monotonic()))
            if msg is None:
                self.fail(f"timed out waiting for {t!r}; saw {[m['t'] for m in skipped]}")
            if msg.get("t") == t:
                return msg
            if msg.get("t") in self.PASSTHROUGH:
                skipped.append(msg)
                continue
            self.fail(f"expected {t!r}, got {msg!r} (after {[m['t'] for m in skipped]})")

    def handshake(self, client: ClientEnd, proto_version: int = 1) -> dict:
        client.send({"t": "hello", "proto": proto_version, "client": "kvit-dictate/test"})
        return self.expect(client, "hello_ack")

    def ready(self, client: ClientEnd) -> dict:
        self.handshake(client)
        return self.expect(client, "ready")


# ─── §5 Handshake and version negotiation ───────────────────────────────────────────────────────


class TestHandshake(ServerTestCase):
    def test_hello_is_answered_with_hello_ack(self):
        """§5: a handshake needs a positive acknowledgement or it is not a handshake. A Client that
        received only ``loading`` would know a peer is alive but not that its version was
        accepted."""
        h = self.start()
        ack = self.handshake(h.client)
        self.assertEqual(ack["proto"], proto.PROTO_VERSION)
        self.assertEqual(ack["server"], "kvit-asr/0.1")

    def test_hello_ack_precedes_everything_else(self):
        h = self.start()
        h.client.send({"t": "hello", "proto": 1})
        first = h.client.recv()
        self.assertEqual(first["t"], "hello_ack")

    def test_a_proto_version_the_server_does_not_speak_is_rejected(self):
        """§5: no silent downgrade, no best-effort continuation. A version we do not implement is
        a message set we cannot parse."""
        h = self.start()
        h.client.send({"t": "hello", "proto": 2})
        err = self.expect(h.client, "error")
        self.assertEqual(wire_code(err), "proto_unsupported")
        self.assertTrue(err["fatal"])
        self.assertEqual(err["supported"], [proto.PROTO_VERSION])
        self.assertIn("2", err["msg"])
        self.assertEqual(h.finish(), server_mod.EXIT_PROTO_UNSUPPORTED)

    def test_a_rejected_handshake_produces_no_ready(self):
        h = self.start()
        h.client.send({"t": "hello", "proto": 99})
        remaining = h.client.drain()
        self.assertEqual([m["t"] for m in remaining], ["error"])

    def test_audio_before_hello_is_a_state_violation(self):
        h = self.start()
        h.client.send_audio(speech(20))
        err = self.expect(h.client, "error")
        self.assertEqual(wire_code(err), "state_violation")
        self.assertTrue(err["fatal"])
        self.assertEqual(h.finish(), server_mod.EXIT_PROTOCOL_ERROR)

    def test_a_message_other_than_hello_first_is_a_state_violation(self):
        h = self.start()
        h.client.send({"t": "start", "session_id": "s1", "sample_rate": 16000})
        err = self.expect(h.client, "error")
        self.assertEqual(wire_code(err), "state_violation")
        self.assertIn("hello", err["msg"])

    def test_a_second_hello_on_one_connection_is_a_state_violation(self):
        h = self.start()
        self.ready(h.client)
        h.client.send({"t": "hello", "proto": 1})
        err = self.expect(h.client, "error")
        self.assertEqual(wire_code(err), "state_violation")
        self.assertIn("second hello", err["msg"])

    def test_end_of_stream_during_the_handshake_is_a_transport_exit(self):
        h = self.start()
        self.assertEqual(h.finish(), server_mod.EXIT_TRANSPORT)


# ─── §6.2 ready ─────────────────────────────────────────────────────────────────────────────────


class TestReady(ServerTestCase):
    def test_ready_declares_a_mode_from_the_phase_0a_taxonomy(self):
        """§6.2: any other value is ``bad_message``. The field is what keeps latency numbers
        interpretable (plan.md §3), so a backend that mislabels itself must not reach the wire."""
        h = self.start()
        ready = self.ready(h.client)
        self.assertIn(ready["mode"], server_mod.VALID_MODES)
        self.assertEqual(ready["model"], "mock")
        self.assertIsInstance(ready["warm_ms"], int)
        self.assertGreaterEqual(ready["warm_ms"], 0)
        self.assertIn("context_supported", ready)
        self.assertIs(ready["context_supported"], False)

    def test_ready_is_sent_exactly_once(self):
        """There is no "ready again": a model change is a new child process (plan.md §1.1)."""
        h = self.start()
        self.ready(h.client)
        h.client.send({"t": "start", "session_id": "s1", "sample_rate": 16000})
        h.client.send({"t": "stop", "session_id": "s1"})
        self.expect(h.client, "stopped")
        h.client.close_write()
        self.assertNotIn("ready", [m["t"] for m in h.client.drain()])

    def test_a_backend_reporting_an_unknown_mode_never_reaches_the_wire(self):
        h = self.start(backend_loader=mock_backend_loader(mode="turbo-streaming"))
        self.handshake(h.client)
        err = self.expect(h.client, "error")
        self.assertEqual(wire_code(err), "internal_error")
        self.assertIn("turbo-streaming", err["msg"])

    def test_context_supported_is_reported_honestly(self):
        """§6.1/plan.md §4: a Server without context support MUST say so, because
        context-enabled and context-free runs are scored in separate columns and a silent drop
        would corrupt the comparison."""
        h = self.start(
            backend_loader=mock_backend_loader(MockConfig(seed=1, supports_context=True))
        )
        self.assertIs(self.ready(h.client)["context_supported"], True)


# ─── The whole session, end to end ──────────────────────────────────────────────────────────────


class TestFullSession(ServerTestCase):
    """hello → hello_ack → ready → start → audio → partial* → stop → final → stopped."""

    def _run_session(self, session_id="s7", chunks=8):
        h = self.start()
        self.ready(h.client)
        h.client.send(
            {"t": "start", "session_id": session_id, "sample_rate": 16000,
             "context": ["llama.cpp", "Qwen3"]}
        )
        for _ in range(chunks):
            h.client.send_audio(speech())
        h.client.send({"t": "stop", "session_id": session_id})
        messages = []
        while True:
            msg = self.expect_any(h.client)
            messages.append(msg)
            if msg["t"] == "stopped":
                break
        return h, messages

    def expect_any(self, client):
        msg = client.recv()
        if msg is None:
            self.fail("timed out waiting for a message")
        return msg

    def test_the_session_produces_partials_a_final_and_a_stopped(self):
        _h, messages = self._run_session()
        kinds = [m["t"] for m in messages]
        self.assertIn("partial", kinds)
        self.assertEqual(kinds.count("final"), 1)
        self.assertEqual(kinds[-1], "stopped")
        self.assertEqual(messages[-1]["reason"], "stop")

    def test_every_message_echoes_the_session_id_verbatim(self):
        """§7.1: the Server MUST NOT normalise, trim, or re-case it. This is the rule that stops a
        late ``final`` from being injected into whatever window the user has since focused."""
        _h, messages = self._run_session(session_id="S7_a-B")
        for msg in messages:
            self.assertEqual(msg["session_id"], "S7_a-B")

    def test_seq_is_strictly_increasing_and_only_text_messages_consume_it(self):
        """§7.4: ``seq`` is scoped to the session, starts at 1, and is consumed only by ``partial``
        and ``final``. ``stopped`` is not part of the text stream and must not burn a number."""
        _h, messages = self._run_session()
        seqs = [m["seq"] for m in messages if m["t"] in ("partial", "final")]
        self.assertEqual(seqs, list(range(1, len(seqs) + 1)))
        for msg in messages:
            if msg["t"] in ("stopped", "error"):
                self.assertNotIn("seq", msg)

    def test_utterance_id_starts_at_one(self):
        _h, messages = self._run_session()
        ids = {m["utterance_id"] for m in messages if m["t"] in ("partial", "final")}
        self.assertEqual(ids, {1})

    def test_stable_bytes_is_monotonic_and_its_prefix_never_mutates(self):
        """docs/protocol.md §8.2 requirement 3, **both halves**.

        The second half is the one with teeth and the one that is easy to forget: a non-decreasing
        ``stable_bytes`` whose underlying bytes changed has still retracted text the Client may
        already have typed into the user's document. Checked here with
        ``kvit_asr.protocol.check_stability``, which is a different implementation from the
        ``kvit_asr.stability`` tracker that produced the numbers — so the two have to agree.
        """
        _h, messages = self._run_session()
        prev_text, prev_stable = "", 0
        saw_text = False
        for msg in messages:
            if msg["t"] not in ("partial", "final"):
                continue
            saw_text = True
            text, stable = msg["text"], msg["stable_bytes"]
            self.assertGreaterEqual(stable, 0)
            self.assertLessEqual(stable, len(text.encode("utf-8")))
            verdict, offset = check_stability(prev_text, prev_stable, text, stable)
            self.assertEqual(verdict, StabilityCheck.OK, f"{msg!r} at {offset}")
            prev_text, prev_stable = text, stable
        self.assertTrue(saw_text)

    def test_stable_bytes_actually_advances_rather_than_sitting_at_zero(self):
        """Monotonicity is satisfied trivially by a server that never publishes anything, so the
        interesting assertion is that a prefix does become eligible."""
        _h, messages = self._run_session(chunks=12)
        stables = [m["stable_bytes"] for m in messages if m["t"] == "partial"]
        self.assertTrue(stables)
        self.assertGreater(max(stables), 0)

    def test_the_final_is_entirely_stable_and_extends_the_published_prefix(self):
        """§6.2: ``final.text`` is entirely stable by definition; §8.2's last sentence: it MUST
        begin with the highest stable prefix published for that utterance."""
        _h, messages = self._run_session()
        partials = [m for m in messages if m["t"] == "partial"]
        final = next(m for m in messages if m["t"] == "final")
        self.assertEqual(final["stable_bytes"], len(final["text"].encode("utf-8")))
        self.assertIn("audio_pos_ms", final)
        high_water = max((p["stable_bytes"] for p in partials), default=0)
        published = max(
            (p for p in partials if p["stable_bytes"] == high_water),
            key=lambda p: p["seq"],
            default=None,
        )
        if published is not None:
            self.assertTrue(
                final["text"].encode("utf-8").startswith(
                    published["text"].encode("utf-8")[:high_water]
                )
            )

    def test_partial_text_is_cumulative_not_a_delta(self):
        """§8.1. Getting this backwards produces no error anywhere — the frames are well formed,
        the offsets are in range, the Client accepts every message — and the user's document fills
        with fragments. The observable signature is that the text grows overall rather than each
        message being a short fragment."""
        _h, messages = self._run_session(chunks=10)
        partials = [m["text"] for m in messages if m["t"] == "partial"]
        self.assertGreater(len(partials), 2)
        self.assertGreater(len(partials[-1]), len(partials[0]))
        # A delta stream would have roughly constant-length messages; a cumulative one ends with a
        # message longer than every individual one that preceded it.
        self.assertEqual(partials[-1], max(partials, key=len))

    def test_audio_pos_ms_tracks_the_audio_that_was_sent(self):
        h, messages = self._run_session(chunks=8)
        final = next(m for m in messages if m["t"] == "final")
        self.assertEqual(final["audio_pos_ms"], 8 * CHUNK_MS)

    def test_the_connection_survives_a_completed_session(self):
        h = self.start()
        self.ready(h.client)
        for sid in ("s1", "s2"):
            h.client.send({"t": "start", "session_id": sid, "sample_rate": 16000})
            for _ in range(4):
                h.client.send_audio(speech())
            h.client.send({"t": "stop", "session_id": sid})
            while True:
                msg = self.expect_any(h.client)
                self.assertEqual(msg["session_id"], sid)
                if msg["t"] == "stopped":
                    break
        self.assertEqual(h.finish(), server_mod.EXIT_OK)


# ─── §6.1 / §10.4 cancel ────────────────────────────────────────────────────────────────────────


class TestCancel(ServerTestCase):
    def test_cancel_discards_and_emits_nothing_further(self):
        """§6.1: the Server MUST NOT emit any ``partial`` or ``final`` for this session after it
        *reads* the ``cancel`` — and MUST still emit ``stopped``, so the Client's teardown is
        acknowledged rather than timed out."""
        h = self.start()
        self.ready(h.client)
        h.client.send({"t": "start", "session_id": "s3", "sample_rate": 16000})
        for _ in range(6):
            h.client.send_audio(speech())
        h.client.send({"t": "cancel", "session_id": "s3"})
        h.client.close_write()

        messages = h.client.drain()
        stopped_at = next(i for i, m in enumerate(messages) if m["t"] == "stopped")
        self.assertEqual(messages[stopped_at]["reason"], "cancel")
        self.assertEqual([m["t"] for m in messages[stopped_at + 1:]], [])
        self.assertEqual(h.finish(), server_mod.EXIT_OK)

    def test_audio_after_a_cancel_is_discarded_and_never_decoded(self):
        h = self.start()
        self.ready(h.client)
        h.client.send({"t": "start", "session_id": "s4", "sample_rate": 16000})
        h.client.send({"t": "cancel", "session_id": "s4"})
        self.expect(h.client, "stopped")
        for _ in range(6):
            h.client.send_audio(speech())
        h.client.close_write()
        self.assertEqual(h.client.drain(), [])

    def test_cancel_is_idempotent(self):
        h = self.start()
        self.ready(h.client)
        h.client.send({"t": "start", "session_id": "s5", "sample_rate": 16000})
        h.client.send({"t": "cancel", "session_id": "s5"})
        h.client.send({"t": "cancel", "session_id": "s5"})
        h.client.close_write()
        stopped = [m for m in h.client.drain() if m["t"] == "stopped"]
        self.assertEqual(len(stopped), 2)   # the second names an already-ended session
        self.assertEqual(stopped[0]["reason"], "cancel")
        self.assertEqual(stopped[1]["reason"], "unknown_session")


# ─── §6.1 / §10.5 idempotent teardown, and state violations ─────────────────────────────────────


class TestLifecycleRules(ServerTestCase):
    def test_stop_for_a_session_that_was_never_started_is_answered_not_refused(self):
        """§10.5: a Server MUST NOT treat this as an error, because the Client's teardown path runs
        on crash and shutdown paths where its own state is uncertain."""
        h = self.start()
        self.ready(h.client)
        h.client.send({"t": "stop", "session_id": "never-existed"})
        stopped = self.expect(h.client, "stopped")
        self.assertEqual(stopped["session_id"], "never-existed")
        self.assertEqual(stopped["reason"], "unknown_session")
        self.assertEqual(h.finish(), server_mod.EXIT_OK)

    def test_start_while_a_session_is_open_is_a_fatal_state_violation(self):
        """§7.2: sessions do not nest and are not concurrent; the pipe is not a multiplexer."""
        h = self.start()
        self.ready(h.client)
        h.client.send({"t": "start", "session_id": "s1", "sample_rate": 16000})
        h.client.send({"t": "start", "session_id": "s2", "sample_rate": 16000})
        err = self.expect(h.client, "error")
        self.assertEqual(wire_code(err), "state_violation")
        self.assertTrue(err["fatal"])
        self.assertEqual(h.finish(), server_mod.EXIT_PROTOCOL_ERROR)

    def test_a_server_to_client_message_arriving_from_the_client_is_a_state_violation(self):
        """Not ``unknown_message``: we know exactly what a ``partial`` is, it is simply illegal in
        this direction."""
        h = self.start()
        self.ready(h.client)
        h.client.send({
            "t": "partial", "session_id": "s1", "utterance_id": 1, "seq": 1,
            "text": "hello", "stable_bytes": 0,
        })
        err = self.expect(h.client, "error")
        self.assertEqual(wire_code(err), "state_violation")
        self.assertIn("Server→Client", err["msg"])

    def test_an_odd_length_audio_payload_is_bad_message_not_rounded_down(self):
        """§6.3: an odd length means the sender split a 16-bit sample, i.e. its buffer arithmetic
        is wrong."""
        h = self.start()
        self.ready(h.client)
        h.client.send({"t": "start", "session_id": "s1", "sample_rate": 16000})
        h.client.send_raw(FRAME_AUDIO, b"\x01\x02\x03")
        err = self.expect(h.client, "error")
        self.assertEqual(wire_code(err), "bad_message")
        self.assertTrue(err["fatal"])

    def test_a_zero_length_audio_payload_is_legal_and_a_no_op(self):
        """Fixture 003. It exists as a legal state so that a sender's empty-buffer edge case does
        not have to be special-cased into a protocol error."""
        h = self.start()
        self.ready(h.client)
        h.client.send({"t": "start", "session_id": "s1", "sample_rate": 16000})
        h.client.send_audio(b"")
        h.client.send({"t": "stop", "session_id": "s1"})
        self.expect(h.client, "stopped")
        self.assertEqual(h.finish(), server_mod.EXIT_OK)

    def test_audio_with_no_session_open_is_discarded_and_counted_not_an_error(self):
        """§6.3, the Server obligation: losing the race between a ``stop`` and an in-flight audio
        frame is a Client bug this rule contains rather than escalates. It must not be buffered
        for the next session."""
        h = self.start()
        self.ready(h.client)
        for _ in range(3):
            h.client.send_audio(speech())
        h.client.send({"t": "start", "session_id": "s1", "sample_rate": 16000})
        h.client.send({"t": "stop", "session_id": "s1"})
        stopped = self.expect(h.client, "stopped")
        self.assertEqual(stopped["session_id"], "s1")
        self.assertEqual(h.finish(), server_mod.EXIT_OK)
        self.assertEqual(
            h.server._session.discarded_audio_bytes, 3 * len(speech())
        )

    def test_an_unparseable_control_payload_is_rejected_at_the_layer_that_can_see_it(self):
        """§1: a receiver MUST detect a failure at the lowest layer that can see it, and §4.1
        forbids rescuing a malformed control payload with U+FFFD."""
        h = self.start()
        self.ready(h.client)
        h.client.send_raw(FRAME_CONTROL, b"\xe9 not utf-8")
        err = self.expect(h.client, "error")
        self.assertIn(wire_code(err), ("bad_utf8", "bad_json", "bad_message"))
        self.assertTrue(err["fatal"])

    def test_discontinuity_is_accepted_and_is_never_fatal(self):
        """§6.1: the honest third option between padding with zeroes and splicing. The Server MUST
        accept it and MUST NOT treat it as fatal."""
        h = self.start()
        self.ready(h.client)
        h.client.send({"t": "start", "session_id": "s1", "sample_rate": 16000})
        for _ in range(4):
            h.client.send_audio(speech())
        h.client.send({
            "t": "discontinuity", "session_id": "s1", "gap_ms": 120,
            "reason": "AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY",
        })
        for _ in range(4):
            h.client.send_audio(speech())
        h.client.send({"t": "stop", "session_id": "s1"})
        messages = []
        while True:
            msg = h.client.recv()
            self.assertIsNotNone(msg, "timed out after a discontinuity")
            messages.append(msg)
            if msg["t"] == "stopped":
                break
        self.assertNotIn("error", [m["t"] for m in messages])
        # §6.1 permits the Server to force an utterance boundary; this one does, so the second
        # utterance must be numbered n+1 and its stability high-water starts again (§7.3, §8.2).
        ids = sorted({m["utterance_id"] for m in messages if m["t"] in ("partial", "final")})
        self.assertEqual(ids, list(range(1, len(ids) + 1)))
        self.assertEqual(h.finish(), server_mod.EXIT_OK)


# ─── §11 Errors: the failure that must not be a traceback ───────────────────────────────────────


class TestBackendLoadFailure(ServerTestCase):
    """"A traceback on stderr with no error frame on the pipe" is the failure mode this server is
    written to avoid: the parent waits for a ``ready`` that never comes, and the user sees a tray
    icon stuck on "loading" until a timeout it cannot explain."""

    @staticmethod
    def _exploding_loader(exc):
        def loader(backend_cfg, on_progress=None):
            raise exc
        return loader

    def test_a_backend_that_raises_on_load_produces_a_fatal_error_message(self):
        h = self.start(
            backend_loader=self._exploding_loader(
                ImportError("No module named 'onnxruntime'")
            )
        )
        self.handshake(h.client)
        err = self.expect(h.client, "error")
        self.assertEqual(wire_code(err), "model_load_failed")
        self.assertTrue(err["fatal"])
        # §11.3: the Client surfaces `msg` verbatim, so it must name the thing a human can fix.
        self.assertIn("onnxruntime", err["msg"])
        self.assertIn("mock", err["msg"])
        self.assertEqual(h.finish(), server_mod.EXIT_MODEL_LOAD_FAILED)

    def test_no_ready_follows_a_failed_load(self):
        h = self.start(backend_loader=self._exploding_loader(RuntimeError("CUDA OOM")))
        self.handshake(h.client)
        # `loading` before the failure is legal (§6.2) and expected; `ready` is not.
        self.assertEqual(
            [m["t"] for m in h.client.drain() if m["t"] != "loading"], ["error"]
        )

    def test_the_traceback_goes_to_the_log_as_a_field_not_as_loose_stderr(self):
        """A raw multi-line traceback interleaved with JSON lines is exactly what the parent's log
        capture cannot parse."""
        h = self.start(backend_loader=self._exploding_loader(ValueError("boom")))
        self.handshake(h.client)
        self.expect(h.client, "error")
        h.finish()
        self.assertTrue(h.log_lines)
        for line in h.log_lines:
            json.loads(line)   # every line, no exceptions: one JSON object per line
        records = [json.loads(ln) for ln in h.log_lines]
        failure = next(r for r in records if r["ev"] == "model_load_failed")
        self.assertIn("traceback", failure)
        self.assertIn("ValueError", failure["traceback"])

    def test_an_unhandled_fault_after_ready_becomes_a_session_scoped_decode_failed(self):
        """§11.3: the child is not at fault as a *process*; its model misbehaved on this
        utterance. So the session is aborted and the connection survives."""
        h = self.start(
            backend_loader=mock_backend_loader(
                MockConfig(seed=1, decode_failure_after_ms=300)
            )
        )
        self.ready(h.client)
        h.client.send({"t": "start", "session_id": "s1", "sample_rate": 16000})
        for _ in range(6):
            h.client.send_audio(speech())
        err = self.expect(h.client, "error")
        self.assertEqual(wire_code(err), "decode_failed")
        self.assertFalse(err["fatal"])
        self.assertEqual(err["session_id"], "s1")     # §11.1: required when fatal is false
        self.expect(h.client, "stopped")
        self.assertEqual(h.finish(), server_mod.EXIT_OK)


class TestErrorCodeSpelling(ServerTestCase):
    """Every `error.code` on this wire is §11.3's bare identifier, whichever path produced it.

    This began as an ``expectedFailure`` pinning a real defect: ``_error_fields`` did
    ``str(exc.code)`` on an ``ErrorCode`` member of a ``(str, Enum)`` mixin, which spells itself
    ``"ErrorCode.state_violation"``. It now uses ``ProtocolError.code_raw`` and the test passes.
    Both halves are kept, because the two codes reach the wire by different routes and only one of
    them was ever wrong — which is what made it easy to miss.
    """

    def test_a_code_originated_as_a_string_is_already_correct(self):
        """The handshake rejection calls ``_send_error`` with a literal, so it never meets
        ``_error_fields``. Half the codes on this wire are therefore already right, which is what
        makes the other half easy to miss."""
        h = self.start()
        h.client.send({"t": "hello", "proto": 2})
        err = self.expect(h.client, "error")
        self.assertEqual(err["code"], "proto_unsupported")

    def test_a_code_originated_as_a_protocol_error_is_the_bare_wire_spelling(self):
        """The other half: anything raised as a ``ProtocolError`` and shaped by
        ``_error_fields``. §11.3's identifier, not the repr of a Python enum member."""
        h = self.start()
        h.client.send_audio(speech(20))
        err = self.expect(h.client, "error")
        self.assertEqual(err["code"], "state_violation")

    def test_the_error_shape_is_otherwise_conforming(self):
        """Whatever the spelling, the rest of §6.2's `error` schema must hold — in particular
        §11.1's rule that `session_id` is REQUIRED when `fatal` is false."""
        h = self.start(
            backend_loader=mock_backend_loader(
                MockConfig(seed=1, decode_failure_after_ms=300)
            )
        )
        self.ready(h.client)
        h.client.send({"t": "start", "session_id": "s1", "sample_rate": 16000})
        for _ in range(6):
            h.client.send_audio(speech())
        err = self.expect(h.client, "error")
        self.assertEqual(set(err) & {"t", "code", "fatal", "msg"}, {"t", "code", "fatal", "msg"})
        self.assertIsInstance(err["fatal"], bool)
        self.assertIsInstance(err["msg"], str)
        self.assertFalse(err["fatal"])
        self.assertIn("session_id", err)


class TestLogging(ServerTestCase):
    def test_recognised_text_is_redacted_by_default(self):
        """plan.md §1.9: "no transcripts or audio persisted by default" is not satisfied by a
        program that writes them to a stream the parent happens to be redirecting to a file."""
        h = self.start()
        self.ready(h.client)
        h.client.send({"t": "start", "session_id": "s1", "sample_rate": 16000})
        for _ in range(6):
            h.client.send_audio(speech())
        h.client.send({"t": "stop", "session_id": "s1"})
        self.expect(h.client, "stopped")
        h.finish()

        blob = h.stderr.getvalue()
        self.assertIn("redacted", blob)
        # The default script's first line; if it appears, the redaction is not working.
        self.assertNotIn("llama.cpp", blob)
        for line in h.log_lines:
            json.loads(line)


# ─── A real named pipe ──────────────────────────────────────────────────────────────────────────
#
# Everything above proves the protocol. This proves the transport: `NamedPipeChannel` is ~200 lines
# of ctypes against kernel32 with hand-declared argtypes and a hand-rolled OVERLAPPED, and nothing
# else in the repository runs it. The classic failure — a HANDLE truncated to 32 bits because an
# argtype was missing — presents as "the pipe randomly stops working", which is not a thing a
# review catches.
#
# The test acts as the C++ side: it is the pipe *server* (it creates the instance) and the protocol
# *Client* (it drives the session). Every wait has a timeout, and the pipe is opened
# FILE_FLAG_OVERLAPPED specifically so that they can.

_PIPE_ACCESS_DUPLEX = 0x00000003
_FILE_FLAG_OVERLAPPED = 0x40000000
_FILE_FLAG_FIRST_PIPE_INSTANCE = 0x00080000
_PIPE_TYPE_MESSAGE = 0x00000004
_PIPE_READMODE_MESSAGE = 0x00000002
_PIPE_WAIT = 0x00000000
_PIPE_REJECT_REMOTE_CLIENTS = 0x00000008
_INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
_ERROR_IO_PENDING = 997
_ERROR_PIPE_CONNECTED = 535
_ERROR_MORE_DATA = 234
_WAIT_OBJECT_0 = 0
_WAIT_TIMEOUT = 0x102


class _PipeServer:
    """The parent's half of docs/protocol.md §2.2, in ctypes. Test-only."""

    def __init__(self, name: str) -> None:
        from ctypes import wintypes

        self.name = name
        self.k32 = server_mod._kernel32()
        k32 = self.k32
        k32.CreateNamedPipeW.argtypes = [
            wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
            wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
        ]
        k32.CreateNamedPipeW.restype = wintypes.HANDLE
        k32.ConnectNamedPipe.argtypes = [
            wintypes.HANDLE, ctypes.POINTER(server_mod._OVERLAPPED)
        ]
        k32.ConnectNamedPipe.restype = wintypes.BOOL
        k32.DisconnectNamedPipe.argtypes = [wintypes.HANDLE]
        k32.DisconnectNamedPipe.restype = wintypes.BOOL

        self.h = k32.CreateNamedPipeW(
            name,
            _PIPE_ACCESS_DUPLEX | _FILE_FLAG_OVERLAPPED | _FILE_FLAG_FIRST_PIPE_INSTANCE,
            _PIPE_TYPE_MESSAGE | _PIPE_READMODE_MESSAGE | _PIPE_WAIT
            | _PIPE_REJECT_REMOTE_CLIENTS,
            1,            # nMaxInstances: exactly one sidecar per pipe name (§2.2)
            262144, 262144, 0, None,
        )
        if not self.h or self.h == _INVALID_HANDLE_VALUE:
            raise OSError(f"CreateNamedPipeW failed, err={ctypes.get_last_error()}")
        self.event = k32.CreateEventW(None, True, False, None)
        self.reader = proto.FrameReader()
        self._eof = False

    # -- connection -----------------------------------------------------------------------------

    def begin_connect(self):
        ov = server_mod._OVERLAPPED()
        ov.hEvent = self.event
        self.k32.ResetEvent(self.event)
        ok = self.k32.ConnectNamedPipe(self.h, ctypes.byref(ov))
        err = 0 if ok else ctypes.get_last_error()
        if not ok and err not in (_ERROR_IO_PENDING, _ERROR_PIPE_CONNECTED):
            raise OSError(f"ConnectNamedPipe failed, err={err}")
        return ov, err == _ERROR_PIPE_CONNECTED

    def wait_connected(self, ov, already, timeout_ms=10000) -> bool:
        from ctypes import wintypes

        if already:
            return True
        waited = self.k32.WaitForSingleObject(self.event, timeout_ms)
        if waited != _WAIT_OBJECT_0:
            return False
        n = wintypes.DWORD(0)
        return bool(
            self.k32.GetOverlappedResult(self.h, ctypes.byref(ov), ctypes.byref(n), False)
        )

    # -- framed I/O -----------------------------------------------------------------------------

    def send(self, msg: dict) -> None:
        payload = json.dumps(msg, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.write(proto.encode_frame(FRAME_CONTROL, payload))

    def send_audio(self, pcm: bytes) -> None:
        self.write(proto.encode_frame(FRAME_AUDIO, pcm))

    def write(self, data: bytes, timeout_ms=5000) -> None:
        from ctypes import wintypes

        ov = server_mod._OVERLAPPED()
        ov.hEvent = self.event
        self.k32.ResetEvent(self.event)
        n = wintypes.DWORD(0)
        ok = self.k32.WriteFile(self.h, data, len(data), ctypes.byref(n), ctypes.byref(ov))
        if not ok:
            err = ctypes.get_last_error()
            if err != _ERROR_IO_PENDING:
                raise OSError(f"WriteFile failed, err={err}")
            if self.k32.WaitForSingleObject(self.event, timeout_ms) != _WAIT_OBJECT_0:
                self.k32.CancelIoEx(self.h, ctypes.byref(ov))
                raise TimeoutError("write to the pipe timed out")
            if not self.k32.GetOverlappedResult(
                self.h, ctypes.byref(ov), ctypes.byref(n), True
            ):
                raise OSError(f"GetOverlappedResult(write) failed, err={ctypes.get_last_error()}")
        if int(n.value) != len(data):
            raise OSError(f"short write: {n.value} of {len(data)}")

    def _read_once(self, timeout_ms) -> bytes:
        from ctypes import wintypes

        buf = ctypes.create_string_buffer(65541)
        ov = server_mod._OVERLAPPED()
        ov.hEvent = self.event
        self.k32.ResetEvent(self.event)
        n = wintypes.DWORD(0)
        ok = self.k32.ReadFile(self.h, buf, 65541, ctypes.byref(n), ctypes.byref(ov))
        if not ok:
            err = ctypes.get_last_error()
            if err == _ERROR_IO_PENDING:
                if self.k32.WaitForSingleObject(self.event, timeout_ms) != _WAIT_OBJECT_0:
                    self.k32.CancelIoEx(self.h, ctypes.byref(ov))
                    return b""
                if not self.k32.GetOverlappedResult(
                    self.h, ctypes.byref(ov), ctypes.byref(n), True
                ):
                    return b""
            elif err not in (_ERROR_MORE_DATA,):
                return b""
        return buf.raw[: int(n.value)]

    def recv(self, timeout_ms=5000):
        deadline = time.monotonic() + timeout_ms / 1000.0
        while True:
            frame = self.reader.next_frame()
            if frame is not None:
                return server_mod.decode_control_dict(frame.payload)
            if self._eof:
                return None
            remaining = int((deadline - time.monotonic()) * 1000)
            if remaining <= 0:
                return None
            data = self._read_once(remaining)
            if not data:
                self._eof = True
                self.reader.notify_eof()
                continue
            self.reader.push(data)

    def close(self) -> None:
        for fn, handle in ((self.k32.CancelIoEx, self.h),):
            try:
                fn(handle, None)
            except Exception:  # pragma: no cover
                pass
        try:
            self.k32.DisconnectNamedPipe(self.h)
        except Exception:  # pragma: no cover
            pass
        for handle in (self.event, self.h):
            if handle:
                self.k32.CloseHandle(handle)
        self.h = 0
        self.event = 0


@unittest.skipUnless(sys.platform == "win32", "named pipes require Windows")
class TestOverARealNamedPipe(ServerTestCase):
    """The same conversation, over ``\\\\.\\pipe\\...``, through ``NamedPipeChannel``."""

    def setUp(self):
        super().setUp()
        # §2.2: a per-launch unpredictable name, never reused. Here it also keeps two concurrent
        # test runs from colliding on FILE_FLAG_FIRST_PIPE_INSTANCE.
        self.name = r"\\.\pipe\kvit-dictate-test-" + secrets.token_hex(16)
        self.pipe = _PipeServer(self.name)
        self.addCleanup(self.pipe.close)

        self.channel = None
        self.exit_code = None
        self.stderr = io.StringIO()

        ov, already = self.pipe.begin_connect()
        self.thread = threading.Thread(
            target=self._serve, name="kvit-test-pipe-server", daemon=True
        )
        self.thread.start()
        self.assertTrue(
            self.pipe.wait_connected(ov, already), "the sidecar never connected to the pipe"
        )
        self.addCleanup(self._join)

    def _serve(self):
        from kvit_asr.server import NamedPipeChannel

        log = EventLogger(stream=self.stderr, level="debug")
        try:
            self.channel = NamedPipeChannel.connect(self.name, log)
            cfg = ServerConfig(
                pipe_name=self.name,
                backend=BackendConfig(id="mock", model="mock"),
                session=CliSessionConfig(vad="none"),  # see ServerHarness.DEFAULT_CFG
            )
            self.exit_code = Server(
                cfg,
                self.channel,
                log=log,
                backend_loader=mock_backend_loader(),
                session_factory=ticking_router,
            ).run()
        except BaseException as exc:  # pragma: no cover
            self.exit_code = exc

    def _join(self):
        self.pipe.close()
        self.thread.join(timeout=JOIN_TIMEOUT)
        if self.thread.is_alive():  # pragma: no cover
            raise AssertionError("the sidecar thread did not exit after the pipe closed")

    def _expect(self, t, timeout_ms=8000):
        while True:
            msg = self.pipe.recv(timeout_ms)
            self.assertIsNotNone(msg, f"timed out waiting for {t!r} over the pipe")
            if msg.get("t") == "loading":
                continue
            self.assertEqual(msg.get("t"), t, msg)
            return msg

    def test_handshake_and_a_whole_session_over_a_real_pipe(self):
        self.pipe.send({"t": "hello", "proto": 1, "client": "kvit-dictate/test"})
        ack = self._expect("hello_ack")
        self.assertEqual(ack["proto"], proto.PROTO_VERSION)
        ready = self._expect("ready")
        self.assertIn(ready["mode"], server_mod.VALID_MODES)

        self.pipe.send({"t": "start", "session_id": "p1", "sample_rate": 16000})
        for _ in range(8):
            self.pipe.send_audio(speech())
        self.pipe.send({"t": "stop", "session_id": "p1"})

        seen = []
        while True:
            msg = self.pipe.recv(8000)
            self.assertIsNotNone(msg, "timed out during the session over the pipe")
            seen.append(msg)
            if msg["t"] == "stopped":
                break
        kinds = [m["t"] for m in seen]
        self.assertIn("partial", kinds)
        self.assertEqual(kinds.count("final"), 1)
        for msg in seen:
            self.assertEqual(msg["session_id"], "p1")

        prev_text, prev_stable = "", 0
        for msg in seen:
            if msg["t"] not in ("partial", "final"):
                continue
            verdict, _ = check_stability(
                prev_text, prev_stable, msg["text"], msg["stable_bytes"]
            )
            self.assertEqual(verdict, StabilityCheck.OK, msg)
            prev_text, prev_stable = msg["text"], msg["stable_bytes"]

    def test_a_proto_mismatch_is_rejected_over_a_real_pipe(self):
        self.pipe.send({"t": "hello", "proto": 7})
        err = self._expect("error")
        self.assertEqual(wire_code(err), "proto_unsupported")
        self.assertTrue(err["fatal"])


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
