"""The per-session decode loop: audio in, ``partial`` / ``final`` / ``stopped`` out.

Implements the Server half of docs/protocol.md §6.3 (audio attribution), §7.3/§7.4 (utterance and
seq numbering), §8 (cumulative text and ``stable_bytes``), §9 (backpressure), and §10.4 (cancel
during final); plus plan.md §1.6's decoder gating during silence.

One :class:`Session` object exists per ``start`` message and dies at ``stopped``. It owns the
audio buffer, the VAD, the decoder gating, the utterance counter, the seq counter, and the
stability computation. It does **not** own the pipe, the framing, the JSON codec, or the
connection state machine — it emits plain ``dict`` message bodies through an injected callable and
lets the transport encode and write them. That split is what makes this file testable without a
pipe and what keeps it from having a second opinion about the wire format.

Zero third-party dependencies, by house rule. The backend is injected and only the backend may
import numpy / onnxruntime / torch.

─── The two mistakes that are silent ───────────────────────────────────────────────────────────

**1. ``partial.text`` is cumulative, not a delta** (docs/protocol.md §8.1). It is the complete
current hypothesis for the current utterance. Getting this backwards produces no error anywhere —
the frames are well formed, the offsets are in range, the Client accepts every message — and the
user's document fills with fragments. Nothing in this file ever concatenates hypotheses; the
backend returns the whole thing and it is passed through untouched.

**2. Offsets are UTF-8 bytes, and Python strings are code points.** Everything numeric that
crosses the wire is produced by :mod:`kvit_asr.stability`, which works in bytes throughout. This
module never computes an offset itself.

─── Threading ──────────────────────────────────────────────────────────────────────────────────

The decode path — :meth:`on_audio`, :meth:`pump`, :meth:`tick`, :meth:`on_stop`,
:meth:`on_discontinuity` — is single-threaded and must be driven from one thread.

:meth:`on_cancel` is the one exception, and it is the exception on purpose. docs/protocol.md
§10.4 describes a race the design cannot remove: the user presses ``Esc`` at the same instant the
server commits an utterance, so the ``cancel`` is read while a final decode is in flight. If
``cancel`` could only be processed by the decode thread, it would sit behind the very decode it is
meant to abandon. So :meth:`on_cancel` may be called from the reader thread, it takes a lock, and
every text emission re-checks the cancelled flag under that same lock immediately before consuming
a ``seq``. The result is exactly the obligation §10.4 puts on the Server: *after reading* the
cancel, no further ``partial`` or ``final`` for that session — and ``stopped`` regardless.

The injected ``emit`` callable must therefore be safe to call from two threads. That is the
transport's job, not this module's; a transport that serialises writes per frame satisfies it.
"""

from __future__ import annotations

import threading
from dataclasses import dataclass, field
from typing import Any, Callable, Protocol, Sequence, runtime_checkable

from .stability import StabilityParams, StabilityTracker, StabilityViolation, monotonic_ms

__all__ = [
    "Session",
    "SessionConfig",
    "Backend",
    "Vad",
    "VadEvent",
    "SPEECH_START",
    "SPEECH_END",
]


# ─── Collaborator contracts ─────────────────────────────────────────────────────────────────────
#
# These are the interfaces this module needs from modules it does not own (``kvit_asr.vad``,
# ``kvit_asr.backends.*``). They are stated here as typing Protocols rather than imported, so that
# (a) this file imports on a bare stdlib CPython with no backend installed, and (b) the shape is
# written down in one place where the backend authors can disagree with it explicitly.


@runtime_checkable
class Backend(Protocol):
    """A streaming ASR backend, reduced to what the decode loop actually needs.

    Contract, stated precisely because "hypothesis" is doing load-bearing work:

    * :meth:`reset` starts a new utterance. Everything the backend knows about the previous one is
      discarded. It is also what plan.md §1.6 calls for at a silence endpoint, so that 60 seconds
      of silence is never fed to a model that hallucinates "Thank you." over it.
    * :meth:`accept_audio` takes 16 kHz mono s16le PCM and returns the **complete current
      hypothesis for the utterance so far**, or ``None`` if the hypothesis has not changed.
      It is cumulative-since-reset. Returning a delta here corrupts every document this tool
      touches, silently.
    * :meth:`finalise` flushes the decoder and returns the complete text of the utterance. The
      session calls :meth:`reset` afterwards; a backend need not reset itself.

    A backend may raise. The session converts any exception into a session-scoped
    ``decode_failed`` (docs/protocol.md §11.3, ``fatal:false``) rather than killing the connection,
    because the child is not at fault as a *process*.

    Optionally, a backend may also expose::

        def pending_stable_bytes_override(self) -> int | None: ...

    called immediately after :meth:`accept_audio` and :meth:`finalise`. Returning a number means
    "publish exactly this ``stable_bytes``, whatever the stability computation says" — the test
    hook behind `backends.base.Hypothesis.stable_bytes_override`, gated on the sidecar's
    `--allow-hostile-backend` flag and refused without it. It exists because docs/protocol.md §8.3
    (what a Client does when a Server retracts already-published stable text) cannot otherwise be
    exercised end to end: stability is *this* side's computation, so a correct pipeline cannot
    produce the input §8.3 handles. A backend without the method is the normal case and drives
    exactly the behaviour it always did.
    """

    def reset(self, context: Sequence[str] = ()) -> None: ...

    def accept_audio(self, pcm: bytes) -> str | None: ...

    def finalise(self) -> str: ...


SPEECH_START = "speech_start"
SPEECH_END = "speech_end"


@dataclass(frozen=True)
class VadEvent:
    """A speech-boundary event located inside the PCM chunk that produced it.

    ``offset`` is a **byte** offset into the chunk just pushed, so the session can split that chunk
    and feed only the speech part to the decoder. A VAD that can only answer per-chunk reports
    ``offset = 0`` for a start and ``offset = len(chunk)`` for an end; that is a coarser
    endpointing, not a protocol violation.
    """

    kind: str      # SPEECH_START | SPEECH_END
    offset: int


@runtime_checkable
class Vad(Protocol):
    """Voice activity detection with endpointing.

    The **endpoint-silence timer** (plan.md §1.6, ~0.8 s — the one that ends an *utterance*) lives
    inside the VAD, because it is a property of the audio stream and the VAD is the only thing
    looking at it. The **session-silence timer** (120 s, the one that ends the *session*) is the
    Client's and never appears here — plan.md §1.6 is explicit that a naive implementation merges
    these two, so they are kept in different processes.
    """

    def reset(self) -> None: ...

    def push(self, pcm: bytes) -> Sequence[VadEvent]: ...


Emit = Callable[[dict], None]


# ─── Configuration ──────────────────────────────────────────────────────────────────────────────


@dataclass
class SessionConfig:
    sample_rate: int = 16000

    # docs/protocol.md §9: the Server's inbound audio queue, bounded. On overflow the response is
    # an explicit fatal ``overload_audio`` and teardown — never a dropped frame. "Silently dropping
    # or delaying audio produces mysteriously bad transcription and is not acceptable."
    audio_queue_max_ms: int = 3000

    stability: StabilityParams = field(default_factory=StabilityParams)

    # Vocabulary hints from ``start.context`` (docs/protocol.md §6.1), handed to the backend at
    # every utterance reset. Empty is not the same as absent; the *absent* case is the caller's to
    # report via ``ready.context_supported``, because plan.md §4 compares context-enabled and
    # context-free runs separately.
    context: Sequence[str] = ()

    # A discontinuity means the audio stream is not contiguous. docs/protocol.md §6.1 permits the
    # Server to "reset decoder state or force an utterance boundary"; this does both, because the
    # decoder state that would have finished the utterance no longer exists and pretending
    # otherwise splices a lie into the transcript. Configurable so the bake-off can measure the
    # alternative, not because the alternative is recommended.
    finalise_on_discontinuity: bool = True

    # An utterance that decodes to nothing produces no ``final``. Emitting an empty one would
    # burn a ``seq`` and an ``utterance_id`` to say nothing, and docs/protocol.md §6.2 requires
    # exactly one ``final`` per ``utterance_id`` — so an utterance with no text is simply never
    # opened rather than opened and closed empty.
    emit_empty_finals: bool = False

    # Decode inline on the calling thread. True is the simple single-threaded server: on_audio()
    # decodes before it returns and the audio buffer never grows, so the §9 bound never bites.
    # Set False when a separate decode thread calls pump(), which is the arrangement the bound
    # exists for.
    inline_decode: bool = True

    # Whether a `partial` may carry a non-zero `stable_bytes` at all.
    #
    # **Set this False for any backend that re-decodes the whole utterance from scratch.** A
    # growing-window batch model does not extend its previous hypothesis, it replaces it: once it
    # can see where the sentence ended it re-cases the opening word and inserts the punctuation it
    # was not yet sure of, so `so I was thinking we` becomes `So I was thinking, we`. That revises
    # byte 0 — and §8.2 binds a new hypothesis against the *previously published* mark, so once a
    # non-zero mark is out there, no value of `stable_bytes` makes the next message conforming.
    # `StabilityTracker` is right to raise; the mistake is upstream, in having published at all.
    #
    # Note what this is NOT: it is not a stability parameter that trades latency for safety.
    # `stability_n/m/hold_ms` delay the mark, and delaying a mark that will later be contradicted
    # at byte 0 only changes when the abort happens. There is no setting of the three knobs that
    # makes a revising backend safe, which is why this is a separate switch and not a tuning.
    #
    # The cost is that nothing commits until the utterance ends. The live preview is unaffected —
    # `partial` still carries the full revising text, the Client renders everything above the
    # stable mark as pending (plan.md §1.5), and with the mark pinned at 0 that is the whole
    # hypothesis. So the user still watches the words arrive; they land in the document at the
    # endpoint instead of a few hundred milliseconds earlier.
    publish_stable_partials: bool = True


# ─── The session ────────────────────────────────────────────────────────────────────────────────


class Session:
    """The decode loop for one session, from ``start`` to ``stopped``."""

    def __init__(
        self,
        session_id: str,
        *,
        emit: Emit,
        backend: Backend,
        vad: Vad | None = None,
        config: SessionConfig | None = None,
        clock: Callable[[], int] = monotonic_ms,
    ) -> None:
        self.session_id = session_id
        self._emit_raw = emit
        self._backend = backend
        self._vad = vad
        self._cfg = config or SessionConfig()
        self._clock = clock

        self._stability = StabilityTracker(self._cfg.stability, clock=clock)

        self._lock = threading.Lock()

        # docs/protocol.md §7.4: seq is scoped to the SESSION, not the utterance, starts at 1, and
        # is consumed only by `partial` and `final`. `loading`, `ready`, `hello_ack`, `stopped` and
        # `error` do not consume seq numbers — they are not part of the text stream.
        self._seq = 0
        # §7.3: utterance_id starts at 1 for the first VAD-delimited utterance of the session.
        self._utterance_id = 0
        self._utterance_open = False

        self._pending = bytearray()
        self._bytes_per_sample = 2
        self._max_audio_bytes = (
            self._cfg.audio_queue_max_ms * self._cfg.sample_rate * self._bytes_per_sample // 1000
        )

        self._audio_bytes_total = 0     # session audio timeline, including discarded silence
        self._speech = vad is None      # with no VAD every sample is speech

        self._last_text: str | None = None
        self._last_stable = 0
        # True once this utterance's `stable_bytes` has been forced by the backend. See
        # `_stable_bytes_for`; normal sessions never set it.
        self._stability_forced = False

        self._cancelled = False
        self._terminal = False          # `stopped` (or a fatal error) has been emitted
        self._stopping = False
        self.connection_fatal = False   # owner must tear the connection down (§9, §11.1)

    # ── State ───────────────────────────────────────────────────────────────────────────────────

    @property
    def utterance_id(self) -> int:
        return self._utterance_id

    @property
    def seq(self) -> int:
        return self._seq

    @property
    def ended(self) -> bool:
        """True once ``stopped`` (or a fatal error) has been emitted. Terminal (§6.2)."""
        return self._terminal

    @property
    def cancelled(self) -> bool:
        return self._cancelled

    @property
    def audio_pos_ms(self) -> int:
        return self._audio_bytes_total * 1000 // (self._cfg.sample_rate * self._bytes_per_sample)

    @property
    def buffered_audio_bytes(self) -> int:
        with self._lock:
            return len(self._pending)

    # ── Inbound: audio ──────────────────────────────────────────────────────────────────────────

    def on_audio(self, pcm: bytes) -> None:
        """A ``0x02`` frame's payload.

        A zero-length payload is legal and is a no-op (docs/protocol.md §6.3, fixture 003). An
        odd-length payload is ``bad_message`` and is the *framing* layer's to reject, not this
        one's — by the time audio reaches a session it has been validated.

        Audio arriving after the session has ended is discarded and counted, per §6.3's Server
        obligation: losing the race between a ``stop`` and an in-flight audio frame is a Client bug
        this rule contains rather than escalates.
        """
        if not pcm:
            return

        with self._lock:
            if self._terminal or self._cancelled:
                self._audio_bytes_total += len(pcm)   # keep the timeline honest even when discarding
                return
            self._pending += pcm
            self._audio_bytes_total += len(pcm)
            overloaded = len(self._pending) > self._max_audio_bytes

        if overloaded:
            # §9: the only legal responses are to keep up or to fail loudly. Not the newest, not
            # the oldest, not "every other one".
            self._fail_fatal(
                "overload_audio",
                f"inbound audio queue exceeded {self._cfg.audio_queue_max_ms} ms "
                f"({self._max_audio_bytes} bytes)",
            )
            return

        if self._cfg.inline_decode:
            self.pump()

    def pump(self) -> None:
        """Drain the audio buffer through the VAD and the decoder.

        Call from the decode thread when ``inline_decode`` is False; :meth:`on_audio` calls it for
        you otherwise.
        """
        while True:
            with self._lock:
                if self._terminal or self._cancelled or not self._pending:
                    return
                chunk = bytes(self._pending)
                self._pending.clear()
            self._consume(chunk)

    # ── Inbound: control ────────────────────────────────────────────────────────────────────────

    def on_discontinuity(self, gap_ms: int, reason: str | None = None) -> None:
        """docs/protocol.md §6.1 ``discontinuity``: the audio stream is not contiguous.

        Resets the VAD and the decoder. The two silent alternatives — pad with zeroes, or splice
        the audio together — both hand the decoder a lie and produce mysteriously bad transcription
        that is indistinguishable from a bad model.

        The gap is added to the session's audio timeline so that ``audio_pos_ms`` keeps tracking
        wall time rather than tracking bytes-we-happened-to-receive.

        MUST NOT be fatal (§6.1).
        """
        if self._terminal or self._cancelled:
            return

        # Decode whatever arrived before the gap; it is still contiguous with itself.
        self.pump()

        if self._utterance_open and self._cfg.finalise_on_discontinuity:
            self._close_utterance()
        else:
            self._reset_decoder()

        if self._vad is not None:
            self._vad.reset()
            self._speech = False

        if gap_ms > 0:
            self._audio_bytes_total += (
                gap_ms * self._cfg.sample_rate * self._bytes_per_sample // 1000
            )

    def on_stop(self) -> None:
        """docs/protocol.md §6.1 ``stop``: finalise what exists, then end.

        Flush the buffered audio, flush the decoder, emit any remaining ``final`` for the open
        utterance, then ``stopped``. Idempotent — §6.1 makes ``stop`` idempotent with respect to
        sessions that are already gone, because the Client's teardown path also runs on crash
        paths where its own state is uncertain.
        """
        with self._lock:
            if self._terminal or self._stopping:
                return
            self._stopping = True

        try:
            self.pump()
            if self._utterance_open:
                self._close_utterance()
        except StabilityViolation as v:
            self._fail_session("decode_failed", _violation_msg(v))
            return
        except Exception as exc:  # a backend fault, not a protocol fault
            self._fail_session("decode_failed", f"backend raised while finalising: {exc!r}")
            return

        self._send_stopped("stop")

    def on_cancel(self) -> None:
        """docs/protocol.md §6.1 / §10.4 ``cancel``: discard, emit nothing further.

        May be called from the transport's reader thread while the decode thread is inside a
        ``final``. Sets the cancelled flag first, so an in-flight decode that finishes a
        microsecond later finds the gate closed, then emits ``stopped`` immediately rather than
        waiting for that decode — §6.1 requires ``stopped`` so the Client's teardown is
        acknowledged rather than timed out, and making it wait on the very decode being abandoned
        is how a teardown hangs.

        Idempotent.
        """
        with self._lock:
            self._cancelled = True
            first = not self._terminal
            if first:
                self._terminal = True
            self._pending.clear()

        if first:
            self._emit({"t": "stopped", "session_id": self.session_id, "reason": "cancel"})

    def tick(self) -> None:
        """Advance the T-millisecond stability hold when no new hypothesis has arrived.

        plan.md §1.4 step 5 is a timer, and a candidate that matured on the clock is still eligible
        even though the backend has said nothing since.

        **The owner MUST call this on a timer — roughly every ``T / 2`` ms — for the duration of
        the session.** This is not an optimisation for the pause case, which is how it was first
        described here, and the difference matters: step 5 requires the candidate to have been
        unchanged for T ms, and :meth:`_decode` only re-evaluates when the backend hands over a
        new hypothesis. While someone is still talking, the candidate advances on almost every
        hypothesis, each advance restarts the dwell, and a loop that never ticks therefore
        publishes ``stable_bytes = 0`` on every ``partial`` until the text stops changing — the
        overlay fills up and nothing is ever eligible to commit. The failure is silent: every
        message is well formed and the invariant is trivially satisfied by never committing
        anything. See :meth:`kvit_asr.stability.StabilityTracker.poll`.

        Emits at most one ``partial``, and only when ``stable_bytes`` actually advanced — an
        identical re-send would consume a ``seq`` to say nothing.
        """
        if self._terminal or self._cancelled or not self._utterance_open:
            return
        if self._last_text is None:
            return
        if self._stability_forced:
            # A backend forcing `stable_bytes` owns this utterance's sequence entirely; polling
            # would publish the tracker's own value over the top of the forced one. See
            # `_stable_bytes_for`.
            return
        if not self._cfg.publish_stable_partials:
            # The dwell has nothing to mature: `_stable_bytes_for` never fed the tracker, so
            # poll() would evaluate a hypothesis it has never seen. Stand down for the same reason
            # the forced case does — this session publishes no mark before the `final`.
            return
        try:
            stable = self._stability.poll()
        except StabilityViolation as v:
            self._fail_session("decode_failed", _violation_msg(v))
            return
        if stable > self._last_stable:
            self._emit_partial(self._last_text, stable)

    # ── Decoding ────────────────────────────────────────────────────────────────────────────────

    def _consume(self, chunk: bytes) -> None:
        """Feed one contiguous PCM chunk through VAD gating into the decoder."""
        try:
            if self._vad is None:
                if not self._utterance_open:
                    self._open_utterance()
                self._decode(chunk)
                return

            events = self._vad.push(chunk)
            cursor = 0
            for ev in events:
                off = max(0, min(int(ev.offset), len(chunk)))
                segment = chunk[cursor:off]
                if segment:
                    self._route(segment)
                cursor = off
                if ev.kind == SPEECH_START:
                    self._speech = True
                    if not self._utterance_open:
                        self._open_utterance()
                elif ev.kind == SPEECH_END:
                    self._speech = False
                    # plan.md §1.6: the endpoint ends the utterance; the session continues.
                    if self._utterance_open:
                        self._close_utterance()
            tail = chunk[cursor:]
            if tail:
                self._route(tail)
        except StabilityViolation as v:
            self._fail_session("decode_failed", _violation_msg(v))
        except Exception as exc:
            self._fail_session("decode_failed", f"backend raised while decoding: {exc!r}")

    def _route(self, segment: bytes) -> None:
        """Speech goes to the decoder; silence is dropped.

        plan.md §1.6: "Do not feed 60 seconds of silence to a streaming model — several ASR models
        hallucinate on it." ``wan-dancer``'s own ``notes/transcribe.py`` carries a comment about
        Whisper emitting "Thank you." / "Bye" over trailing silence with timestamps past the end of
        the audio. Dropping silence here is decoder *gating*, not audio dropping in the §9 sense:
        the frame was received, accounted for in the timeline, and deliberately not decoded.
        """
        if self._speech and self._utterance_open:
            self._decode(segment)

    def _decode(self, pcm: bytes) -> None:
        hypothesis = self._backend.accept_audio(pcm)
        if hypothesis is None:
            return
        stable = self._stable_bytes_for(hypothesis)
        if hypothesis != self._last_text or stable != self._last_stable:
            self._emit_partial(hypothesis, stable)

    def _stable_bytes_override(self) -> int | None:
        """What the backend demands be published, if it demands anything.

        Read through ``getattr`` because the method is optional on the ``Backend`` protocol — see
        its docstring. A backend that does not offer it is the normal case and must reach exactly
        the code path it always did, so this returns ``None`` rather than raising or defaulting.
        """
        hook = getattr(self._backend, "pending_stable_bytes_override", None)
        if hook is None:
            return None
        value = hook()
        return None if value is None else int(value)

    def _stable_bytes_for(self, text: str) -> int:
        """The ``stable_bytes`` to publish: the computed one, or the one the backend forced.

        Once an override has been honoured, this utterance stays under the backend's control to
        the end of it. That is the part that is easy to get wrong, so it is spelled out.

        The tracker raises :class:`StabilityViolation` precisely when a hypothesis contradicts what
        was already published — which is what a hostile backend is *for*. Left running, it would
        abort the utterance with ``decode_failed`` before the non-conforming frame ever reached the
        wire, and the §8.3 handling this hook exists to exercise would go on being untested while
        the test appeared to run. Its state is also stale the moment a forced value is published,
        so resuming the computation later would raise about a contradiction this session created.

        :meth:`tick` stands down for the same utterance too: the dwell timer would otherwise
        re-publish the tracker's own value over the top of a forced one. The flag clears at the
        utterance boundary, so the next utterance is computed normally.
        """
        override = self._stable_bytes_override()
        if override is not None:
            self._stability_forced = True
            return override
        # Checked after the override so `--allow-hostile-backend` still reaches the wire: the
        # hostile path exists precisely to emit a non-conforming frame, and a backend that forces
        # a value has said it owns this utterance's sequence whatever the mode.
        if not self._cfg.publish_stable_partials:
            # Deliberately without `observe()`: feeding the tracker would build a window and a
            # candidate for a mark that is never published, and the next revision would then raise
            # about a contradiction of something that never left this process.
            return 0
        if self._stability_forced:
            # The backend went quiet about it mid-utterance. Hold the last published value rather
            # than resuming a computation whose state no longer describes what was sent.
            return self._last_stable
        return self._stability.observe(text)

    # ── Utterance lifecycle ─────────────────────────────────────────────────────────────────────

    def _open_utterance(self) -> None:
        self._utterance_id += 1                      # §7.3: starts at 1, increments by exactly 1
        self._utterance_open = True
        self._last_text = None
        self._last_stable = 0
        self._reset_decoder()

    def _reset_decoder(self) -> None:
        self._backend.reset(self._cfg.context)
        self._stability.reset(self._utterance_id)    # plan.md §1.4 step 7
        self._last_text = None
        self._last_stable = 0
        # A forced `stable_bytes` is scoped to one utterance: §8.2's requirements are per
        # utterance_id, so the next one starts computed again even against the same backend.
        self._stability_forced = False

    def _close_utterance(self) -> None:
        """Flush the decoder and emit the ``final`` for the open utterance."""
        text = self._backend.finalise()
        self._utterance_open = False

        if text or self._cfg.emit_empty_finals or self._last_text is not None:
            # `self._last_text is not None` means a `partial` already went out under this id, so
            # §6.2's "exactly one final per utterance_id" obliges us to close it even if the
            # backend's flush came back empty.
            # A `final` is entirely stable by definition (§6.2), and §6.3's decoder enforces it:
            # `final.stable_bytes`, when present, MUST equal len(utf8(text)). So a backend's
            # `stable_bytes_override` is deliberately **not** honoured here — forcing a partial
            # mark onto a `final` would produce a frame rejected as `bad_message` for the wrong
            # reason, which is precisely the trap `backends.mock` warns about: a test that
            # proves nothing while appearing to pass. Overrides shape `partial`, which is where
            # §8.2's monotonicity requirement lives and where §8.3's handling is provoked.
            self._stable_bytes_override()   # consumed, so it cannot leak into the next utterance
            if self._stability_forced:
                # The tracker's state describes a history this session did not send, so
                # `check_emission` would raise about our own deliberate mutation. The answer the
                # protocol demands needs no history.
                stable = len(text.encode("utf-8"))
            else:
                stable = self._stability.finalise(text)
            self._emit_final(text, stable)
        else:
            # Nothing was said and nothing was published. Give the id back so the next utterance
            # is n rather than n+1 — an id the Client never saw and never will is a hole it would
            # have to explain to itself.
            self._utterance_id -= 1

        self._backend.reset(self._cfg.context)
        self._stability.reset(self._utterance_id)
        self._last_text = None
        self._last_stable = 0
        self._stability_forced = False

    # ── Emission ────────────────────────────────────────────────────────────────────────────────

    def _emit_partial(self, text: str, stable_bytes: int) -> None:
        msg = {
            "t": "partial",
            "session_id": self.session_id,
            "utterance_id": self._utterance_id,
            # `text` is CUMULATIVE for this utterance (docs/protocol.md §8.1). Never a delta.
            "text": text,
            "stable_bytes": stable_bytes,
            "audio_pos_ms": self.audio_pos_ms,
        }
        if self._emit_text(msg):
            self._last_text = text
            self._last_stable = stable_bytes

    def _emit_final(self, text: str, stable_bytes: int) -> None:
        msg = {
            "t": "final",
            "session_id": self.session_id,
            "utterance_id": self._utterance_id,
            "text": text,
            "audio_pos_ms": self.audio_pos_ms,
            # Redundant but permitted (§6.2), and it keeps one code path for both message types.
            "stable_bytes": stable_bytes,
        }
        self._emit_text(msg)

    def _emit_text(self, msg: dict) -> bool:
        """Consume a ``seq`` and send, unless the session has been cancelled or ended.

        The cancelled check and the seq increment happen under the same lock, in that order. That
        is the whole of §10.4's Server obligation: a ``cancel`` read by another thread between the
        decode finishing and the message being built cannot slip past, because the flag is read
        after that thread set it and before this thread commits to a sequence number.
        """
        with self._lock:
            if self._terminal or self._cancelled:
                return False
            self._seq += 1
            msg["seq"] = self._seq
        self._emit(msg)
        return True

    def _send_stopped(self, reason: str) -> None:
        with self._lock:
            if self._terminal:
                return
            self._terminal = True
        self._emit({"t": "stopped", "session_id": self.session_id, "reason": reason})

    def _fail_session(self, code: str, msg: str) -> None:
        """A session-scoped error: abort this session, keep the connection (§11.1, ``fatal:false``).

        ``session_id`` is REQUIRED on a non-fatal error.

        **Discrepancy, reported rather than resolved locally** (docs/protocol.md §14): §11.3 has no
        code for "the Server detected that its own next message would violate §8.2".
        ``stability_violation`` exists but is documented as sent *by the Client* about the Server.
        ``decode_failed`` is the closest conforming fit — Server-sent, ``fatal:false``,
        session-scoped, "backend raised while decoding this session" — and it is what this uses,
        with the violation detail in ``msg``. The alternative, sending a message we know to be
        inadmissible and letting the Client raise ``stability_violation``, is worse: by then the
        offending bytes may already be in the user's document.
        """
        with self._lock:
            if self._terminal:
                return
            self._terminal = True
        self._emit({
            "t": "error",
            "session_id": self.session_id,
            "code": code,
            "fatal": False,
            "msg": msg,
        })
        self._emit({"t": "stopped", "session_id": self.session_id, "reason": "error"})

    def _fail_fatal(self, code: str, msg: str) -> None:
        """A connection-scoped error (§11.1, ``fatal:true``): the sender writes nothing more.

        No ``stopped`` follows — ``fatal:true`` means the connection is finished, and §6.2's
        ``stopped`` is a session-teardown acknowledgement that has nowhere left to go. The owner
        observes :attr:`connection_fatal` and tears the connection down.
        """
        with self._lock:
            if self._terminal:
                return
            self._terminal = True
            self._pending.clear()
        self.connection_fatal = True
        self._emit({"t": "error", "session_id": self.session_id, "code": code,
                    "fatal": True, "msg": msg})

    def _emit(self, msg: dict[str, Any]) -> None:
        try:
            self._emit_raw(msg)
        except Exception:
            # A write failure means the peer is gone; §10.3's recovery is a new child and a new
            # pipe, which is the owner's business. There is nobody left to tell, so swallowing is
            # the honest response — but the connection is over.
            self.connection_fatal = True


def _violation_msg(v: StabilityViolation) -> str:
    """§8.3 step 3's diagnosis, in the one form that is safe to put on the wire.

    This string travels in ``error.msg``, and the Client logs it verbatim into a file the user
    keeps. So it must carry no recognised text: plan.md §1.9 and ``src/util/log.h`` both say
    transcripts never enter a diagnostic channel, and the Client cannot redact prose it did not
    compose. Everything identifying *which* rule broke and *where* is a number and survives; the
    words do not, and are available on this side under ``--log-transcripts``.

    §8.3 step 3 does ask a receiver to log both texts, and this is not that case — the Server is
    reporting a violation it caught in its own next message rather than one it received, so the
    only copy of those texts that matters is here, where the privacy gate already exists.

    ``diverge_offset`` is the load-bearing field. Zero means the backend revised the *first word*,
    which is the signature of a model that re-decodes the whole utterance rather than extending
    it, and it is the difference between "tune the stability knobs" and "this backend must never
    publish a mark before the final" (see ``SessionConfig.publish_stable_partials``).
    """
    return (
        f"server-side stability violation ({v.kind}): utterance_id={v.utterance_id} "
        f"previous_stable_bytes={v.previous_stable_bytes} "
        f"diverge_offset={v.diverge_offset} "
        f"published_prefix=<{len(v.previous_prefix)}B redacted> "
        f"incoming_text=<{len(v.incoming_text.encode('utf-8'))}B redacted>"
    )
