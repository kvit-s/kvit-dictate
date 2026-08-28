# Copyright 2026 Kvit
# Licensed under the Apache License, Version 2.0. See LICENSE-APACHE, or
# https://www.apache.org/licenses/LICENSE-2.0
"""The adapter contract every ASR model implements.

plan.md §1.1 justifies the whole two-process split with one sentence: *"In Python each new
candidate is a ~50-line adapter."* That promise is only kept if this module carries the shared
work. So this file is deliberately fat and the concrete backends are deliberately thin: lifecycle
timing, state guards, PCM arithmetic, capability declaration, and a complete overlapping-window
pseudo-streaming driver all live here.

**Zero third-party imports, now and forever.** This module is imported at package import time, so
an `import numpy` here would take the entire server down on a machine where one backend's runtime
is missing. Heavy imports are the concrete backend's business and must happen inside that
backend's module (and ideally inside its ``_load()``), so that a missing runtime disables one
backend rather than breaking the server. See ``kvit_asr.backends.__init__`` for the lazy loader
that turns an ``ImportError`` into "this backend is unavailable".

─── Division of labour with the rest of the server ─────────────────────────────────────────────

A backend produces **hypotheses**. It does not produce protocol messages, and it deliberately
knows nothing about ``session_id``, ``utterance_id``, ``seq``, or ``stable_bytes``:

* VAD, utterance boundaries and utterance ids — the server (plan.md §1.1, §1.6).
* Stability computation and ``stable_bytes`` — the server (plan.md §1.4, docs/protocol.md §8.2).
* Cumulative text within an utterance — **the backend** (docs/protocol.md §8.1). A backend that
  returns deltas is broken; ``PseudoStreamingBackend`` below exists partly so that a batch model
  wrapped in a window cannot accidentally become a delta source.

The one exception is ``Hypothesis.stable_bytes_override``, which exists solely so the mock backend
can force an inadmissible ``stable_bytes`` onto the wire and let the C++ client's violation
handling (docs/protocol.md §8.3) be tested end to end. It is documented at its definition as the
test hook it is.

─── One unverified thing, recorded rather than guessed ─────────────────────────────────────────

``warm_ms`` on the wire is defined by docs/protocol.md §6.2 as *"Milliseconds from process start
to ready"* — a number only the server process can compute, because only it knows when the process
started. This module therefore measures two different, narrower things: ``load_ms`` (how long
``load()`` took) and ``warm_ms`` (how long the warm-up inference took). Both are real
measurements. The server must add them to its own process-start clock rather than copying
``Backend.warm_ms`` into the ``ready`` message; the naming collision is a wart and is called out
here so nobody wires the wrong number to the wire.
"""

from __future__ import annotations

import abc
import array
import enum
import sys
import time
from dataclasses import dataclass
from typing import Callable, Optional, Sequence

__all__ = [
    "SAMPLE_RATE",
    "CHANNELS",
    "BYTES_PER_SAMPLE",
    "StreamingMode",
    "ResetReason",
    "Hypothesis",
    "BackendCapabilities",
    "ProgressFn",
    "BackendError",
    "BackendLoadError",
    "BackendDecodeError",
    "BackendStateError",
    "Backend",
    "PseudoStreamingBackend",
    "ms_to_bytes",
    "bytes_to_ms",
    "utf8_len",
    "pcm_peak",
    "pcm_samples",
]

# ─── Audio format (docs/protocol.md §6.3, fixed in proto 1) ─────────────────────────────────
#
# 16 kHz, mono, signed 16-bit little-endian. Fixed by the protocol, restated here so a backend
# never has to re-derive the byte arithmetic and never has to guess whether "chunk" means samples,
# bytes or milliseconds. In this file "chunk" always means milliseconds.

SAMPLE_RATE = 16000
CHANNELS = 1
BYTES_PER_SAMPLE = 2
_BYTES_PER_MS = SAMPLE_RATE * BYTES_PER_SAMPLE // 1000  # 32


def ms_to_bytes(ms: int) -> int:
    """Milliseconds of audio -> byte count, rounded down to a whole sample."""
    return (ms * SAMPLE_RATE // 1000) * BYTES_PER_SAMPLE


def bytes_to_ms(n: int) -> int:
    """Byte count -> milliseconds, rounded down."""
    return (n // BYTES_PER_SAMPLE) * 1000 // SAMPLE_RATE


def utf8_len(s: str) -> int:
    """Length of ``s`` in UTF-8 **bytes**.

    Spelled out as a named function, and used everywhere an offset is produced, because
    docs/protocol.md §8.2 measures every offset in UTF-8 bytes while a Python string is a sequence
    of code points. ``len(s)`` and ``utf8_len(s)`` differ for every non-ASCII string, and that
    silent difference is the single most likely bug on this side of the wire.
    """
    return len(s.encode("utf-8"))


def pcm_samples(pcm: bytes) -> array.array:
    """Decode an s16le payload into a signed-16 ``array``.

    ``array.frombytes`` uses **host** byte order, so on a big-endian host the samples would be
    byte-swapped. x64 Windows is the only target this project has, but the swap is written
    explicitly rather than assumed away, on the same principle as ``encode_audio_frame`` in
    src/protocol.h: code should not become wrong by accident if it is ever read elsewhere.
    """
    if len(pcm) % BYTES_PER_SAMPLE:
        raise BackendError(
            f"odd-length PCM payload ({len(pcm)} bytes): a sample was split",
            wire_code="bad_message",
        )
    a = array.array("h")
    a.frombytes(pcm)
    if sys.byteorder == "big":
        a.byteswap()
    return a


def pcm_peak(pcm: bytes) -> int:
    """Peak absolute sample amplitude, 0..32767. Cheap enough to run on every chunk.

    Not a VAD. Real endpointing is Silero ONNX in the server (plan.md §3, Phase 2 item 3); this is
    only for backends that need to notice "this chunk is digital silence" — which the mock does,
    because plan.md §1.6 requires a silence-hallucination test and that test needs a backend that
    can tell silence from speech without pulling in a VAD dependency.
    """
    a = pcm_samples(pcm)
    if not a:
        return 0
    lo = min(a)
    hi = max(a)
    # -(-32768) overflows a signed 16-bit range conceptually; clamp rather than negate blindly.
    neg = 32767 if lo <= -32768 else -lo
    return max(hi, neg, 0)


# ─── Declared properties (plan.md §3 Phase 0A step 2; docs/protocol.md §6.2) ────────────────


class StreamingMode(str, enum.Enum):
    """How the backend actually streams. Reported in ``ready.mode``.

    **This is not decoration.** plan.md §3 Phase 0A: *"The streaming-mode column keeps latency
    comparisons interpretable — a batch Whisper adapter wrapped in a sliding window is not
    comparable to a natively streaming model."* bench/CANDIDATES.md §2 then uses exactly these
    three values, and its selection table (§4, pick 4) requires the pseudo-streaming baseline's
    latency numbers to be *reported in a separate block*. A backend that mislabels itself here
    silently corrupts the bake-off, which is the whole point of Phases 1 and 2.

    The string values are the wire spellings from docs/protocol.md §6.2 and must match
    ``kd::proto::StreamingMode`` in src/protocol.h. Any other value is ``bad_message``.

    Note a wording discrepancy, resolved in favour of the frozen document: plan.md §3 writes the
    third value as ``pseudo-streaming/overlapping-window``; docs/protocol.md §6.2 and
    src/protocol.h both say ``pseudo-streaming``. The wire value is ``pseudo-streaming``.
    """

    NATIVE_STREAMING = "native-streaming"
    """Causal / cache-aware architecture whose *published* recipe is chunked inference."""

    STATEFUL_CHUNKED = "stateful-chunked"
    """Not designed for streaming, but has an official chunked mode that carries decoder state."""

    PSEUDO_STREAMING = "pseudo-streaming"
    """A batch model driven by a sliding or growing window **that we built**. See
    ``PseudoStreamingBackend``. Latency figures are not comparable with the two modes above."""


class ResetReason(str, enum.Enum):
    """Why ``reset()`` is being called. Backends may treat these differently.

    ``DISCONTINUITY`` is the one with teeth: docs/protocol.md §6.1 says a Client that detects a
    capture glitch sends ``discontinuity`` rather than padding with zeroes or splicing, precisely
    so the backend can drop state instead of being handed a lie. A backend that treats it as an
    ordinary utterance boundary is not wrong, but a backend that ignores it is.
    """

    SESSION_START = "session-start"
    UTTERANCE_BOUNDARY = "utterance-boundary"
    DISCONTINUITY = "discontinuity"
    CANCEL = "cancel"
    SESSION_END = "session-end"
    WARMUP = "warmup"


@dataclass(frozen=True, slots=True)
class Hypothesis:
    """One transcription hypothesis for the **current utterance**.

    ``text`` is **cumulative**, never a delta (docs/protocol.md §8.1). Each hypothesis supersedes
    the previous one for the same utterance; it may shrink, reword and repunctuate freely, and the
    server's stability computation is what decides how much of it is safe to commit.
    """

    text: str
    is_final: bool = False
    audio_pos_ms: Optional[int] = None
    """Decoder position in the session audio timeline, ms. Optional on ``partial`` and required on
    ``final`` (docs/protocol.md §6.2); a backend that can supply it should, because Phase 2's
    latency decomposition may promote it to required (docs/protocol.md §14 item 4)."""

    stable_bytes_override: Optional[int] = None
    """**Test hook. Normal backends leave this ``None``.**

    Stability is the server's computation (plan.md §1.4) and a correct server will never let a
    hypothesis retract already-published stable text. That is exactly why the C++ client's
    violation handling (docs/protocol.md §8.3) is otherwise untestable end to end: nothing in a
    correct pipeline can produce the input it handles. This field lets a deliberately hostile
    backend — see ``kvit_asr.backends.mock`` — force a chosen ``stable_bytes`` onto the wire.

    A UTF-8 **byte** offset into ``text``, like everything else in §8.2.

    The server SHOULD refuse to honour this unless it was started with an explicit
    allow-hostile-backend flag, and MUST log loudly when it does."""

    hostile: bool = False
    """True when this hypothesis was manufactured to break a protocol invariant. Purely so the
    server can log the fact rather than discovering it from the client's error message."""

    @property
    def text_bytes(self) -> int:
        """``len(utf8(text))`` — the upper bound on any offset into this hypothesis."""
        return utf8_len(self.text)


@dataclass(frozen=True, slots=True)
class BackendCapabilities:
    """Everything the server must know about a backend without importing it.

    Assembled once at construction and reported in ``ready`` (docs/protocol.md §6.2).
    """

    model_id: str
    """Model identifier **as spelled in models.json** (docs/protocol.md §6.2). The server echoes
    it verbatim; it is not a display name and must not be prettified."""

    mode: StreamingMode

    supports_context: bool = False
    """Whether ``start.context`` is honoured — reported as ``ready.context_supported``.

    Declared rather than inferred because plan.md §4 compares context-enabled and context-free
    modes **separately**: *"A backend without vocabulary-context support must not silently receive
    an easier or harder task than one with it."* bench/CANDIDATES.md §5.1 makes this concrete and
    uncomfortable — the leading streaming candidate (Nemotron via sherpa-onnx) is ``greedy_search``
    only and therefore cannot be biased at all, while the biasing arm (zipformer) is expected to
    lose on raw WER. Getting this flag wrong would make that comparison uninterpretable rather
    than merely disappointing."""

    natural_chunk_ms: int = 0
    """The chunk size the model actually wants, in ms; 0 for "no natural size" (batch models).
    Reported as ``ready.chunk_ms``."""

    chunk_ms_configurable: bool = False
    """Whether ``natural_chunk_ms`` can be changed at all.

    Two very different situations that plan.md §4's ``chunk_ms ∈ {80,160,320,640}`` sweep must not
    conflate: a pseudo-streaming wrapper's hop is a free parameter, whereas the Nemotron ONNX
    exports ship **one repo per chunk size** (bench/CANDIDATES.md §2 row A′) — changing it there
    means loading different weights, i.e. a different backend instance, not a setter call."""

    supported_chunk_ms: tuple[int, ...] = ()
    """The discrete chunk sizes this backend can be *instantiated* with, if it is fixed-size.
    Empty means either "continuous" (when ``chunk_ms_configurable``) or "not applicable"."""

    requires_contiguous_audio: bool = True
    """True if a gap in the audio corrupts decoder state until ``reset()``. Almost always true for
    stateful models; the server uses it to decide whether ``discontinuity`` forces a reset."""

    notes: str = ""
    """Free text for logs — quantisation, execution provider, anything a result file should carry
    so a measurement can be attributed later."""

    def ready_fields(self) -> dict:
        """The ``ready`` message fields this backend contributes (docs/protocol.md §6.2).

        Returned as a dict so the spelling of the wire keys lives in exactly one place. The server
        adds ``t`` and ``warm_ms``; ``warm_ms`` is *not* here because it is a process-start
        measurement the backend cannot make (see the module docstring).
        """
        fields: dict = {
            "model": self.model_id,
            "mode": self.mode.value,
            "context_supported": self.supports_context,
        }
        if self.natural_chunk_ms > 0:
            fields["chunk_ms"] = self.natural_chunk_ms
        return fields


ProgressFn = Callable[[str, Optional[int]], None]
"""``(stage, progress) -> None``, called during ``load()``.

Maps one-to-one onto ``loading.stage`` and ``loading.progress`` (docs/protocol.md §6.2):
``stage`` is free text (``"import onnxruntime"``, ``"weights"``, ``"warmup"``), ``progress`` is
0..100 or ``None`` for unknown. ``None`` genuinely means unknown — a Client MUST NOT invent a
progress bar from its absence, so a backend that cannot estimate must pass ``None`` rather than a
comforting fake.

The server is expected to rate-limit these into ``loading`` frames (``LOADING_HEARTBEAT_MS`` =
5000, and each one restarts the Client's ``READY_TIMEOUT_MS``). A backend should call it at every
genuinely slow step rather than trying to pace itself."""


# ─── Errors ─────────────────────────────────────────────────────────────────────────────────


class BackendError(Exception):
    """Base for everything a backend raises.

    ``wire_code`` is the docs/protocol.md §11.3 code the server should put on the wire, so the
    mapping from "the backend blew up" to "which error the client sees" is made by the code that
    knows what happened, not guessed by a generic handler two layers up.
    """

    wire_code = "internal_error"
    fatal = True

    def __init__(self, msg: str, *, wire_code: Optional[str] = None):
        super().__init__(msg)
        if wire_code is not None:
            self.wire_code = wire_code


class BackendLoadError(BackendError):
    """Missing weights, OOM, failed import. -> ``model_load_failed`` (fatal, connection over)."""

    wire_code = "model_load_failed"
    fatal = True


class BackendDecodeError(BackendError):
    """The backend raised while decoding. -> ``decode_failed`` (**non-fatal**: the session is
    aborted, the connection survives, a new session may follow)."""

    wire_code = "decode_failed"
    fatal = False


class BackendStateError(BackendError):
    """``feed()`` before ``load()``, and similar local misuse. A server bug, not a peer bug."""

    wire_code = "internal_error"
    fatal = True


# ─── The contract ───────────────────────────────────────────────────────────────────────────


class Backend(abc.ABC):
    """What every model adapter implements.

    A concrete backend supplies ``capabilities``, ``_load``, ``feed``, ``finalize`` and ``_reset``.
    Everything else — timing, guards, warm-up, the no-op hooks — is inherited. Lifecycle::

        b = SomeBackend(...)
        b.load(progress)          # slow; reports stages so the server can send `loading`
        b.warm_up()               # slow once; makes `ready.warm_ms` a measurement, not a guess
        b.reset(ResetReason.SESSION_START)
        for chunk in audio:
            for h in b.feed(chunk):
                ...               # cumulative hypotheses for the current utterance
        final = b.finalize()      # closes the utterance
        b.reset(ResetReason.UTTERANCE_BOUNDARY)
        ...
        b.unload()

    ``feed`` may return zero, one, or several hypotheses for one chunk. Zero is normal and common:
    a native-streaming model with an 80 ms chunk fed a 20 ms buffer has nothing to say yet.
    """

    def __init__(self) -> None:
        self._loaded = False
        self._load_ms: Optional[int] = None
        self._warm_ms: Optional[int] = None
        self._context: tuple[str, ...] = ()

    # ── Declaration ─────────────────────────────────────────────────────────────────────────

    @property
    @abc.abstractmethod
    def capabilities(self) -> BackendCapabilities:
        """Constant for the lifetime of the instance. Must be answerable *before* ``load()``, so
        the server can log what it is about to load and so a manifest can be validated without
        paying the load cost."""

    # ── Lifecycle ───────────────────────────────────────────────────────────────────────────

    @property
    def is_loaded(self) -> bool:
        return self._loaded

    @property
    def load_ms(self) -> Optional[int]:
        """Measured duration of ``load()``. ``None`` until it has run."""
        return self._load_ms

    @property
    def warm_ms(self) -> Optional[int]:
        """Measured duration of the warm-up inference. ``None`` until ``warm_up()`` has run.

        **Not** the wire's ``ready.warm_ms``, which is process start -> ready. See the module
        docstring; the collision is in the protocol's naming, not fixable here."""
        return self._warm_ms

    def load(self, progress: Optional[ProgressFn] = None) -> int:
        """Load the model. Idempotent. Returns the measured load time in ms.

        Times ``_load`` and brackets it with progress reports so that even a backend whose
        ``_load`` reports nothing still produces a start and an end for the server to relay.
        """
        if self._loaded:
            return self._load_ms or 0
        t0 = time.perf_counter()
        self._report(progress, "load", 0)
        try:
            self._load(progress)
        except BackendError:
            raise
        except Exception as exc:  # any runtime's failure mode; the server needs one code for it
            raise BackendLoadError(f"{type(exc).__name__}: {exc}") from exc
        self._loaded = True
        self._load_ms = _elapsed_ms(t0)
        self._report(progress, "loaded", 100)
        return self._load_ms

    @abc.abstractmethod
    def _load(self, progress: Optional[ProgressFn]) -> None:
        """Do the slow work. **All heavy imports belong here**, not at module scope, so that an
        absent runtime costs one backend rather than the server. Call ``progress`` at every step
        that can take seconds."""

    def warm_up(self, *, audio_ms: int = 1000, progress: Optional[ProgressFn] = None) -> int:
        """Run one throwaway inference and measure it. Returns ms.

        This exists so ``ready.warm_ms`` is *measured*. The first inference of a session is
        routinely an order of magnitude slower than the rest — lazy kernel selection, cuDNN
        autotuning, JIT, arena allocation — and a model that reports "ready" before paying that
        cost has moved it into the user's first sentence, which is precisely the latency the
        product is judged on.

        Output is discarded and the backend is reset afterwards, so warm-up can never leak text
        into a real session.
        """
        self._require_loaded("warm_up")
        self._report(progress, "warmup", None)
        t0 = time.perf_counter()
        try:
            self._warm_up(audio_ms)
        except BackendError:
            raise
        except Exception as exc:
            raise BackendLoadError(f"warm-up failed: {type(exc).__name__}: {exc}") from exc
        self._warm_ms = _elapsed_ms(t0)
        self.reset(ResetReason.WARMUP)
        return self._warm_ms

    def _warm_up(self, audio_ms: int) -> None:
        """Default warm-up: push silence through the real decode path and finalize it.

        Silence rather than noise, deliberately: the point is to touch every kernel, not to
        produce plausible text, and a backend that hallucinates over silence (plan.md §1.6) would
        produce garbage here — which is harmless, because the output is discarded and the backend
        is reset. Override if a model needs a different shape of input to exercise its graph.
        """
        self.reset(ResetReason.WARMUP)
        chunk_ms = self.capabilities.natural_chunk_ms or 100
        silence = b"\x00" * ms_to_bytes(chunk_ms)
        fed = 0
        while fed < audio_ms:
            self.feed(silence)
            fed += chunk_ms
        self.finalize()

    def unload(self) -> None:
        """Release the model. Idempotent. Safe to call on a backend that never loaded."""
        if not self._loaded:
            return
        try:
            self._unload()
        finally:
            self._loaded = False

    def _unload(self) -> None:
        """Drop references to weights/sessions. Default is a no-op — for most runtimes, dropping
        the object is all that happens, and pretending otherwise would be theatre."""

    # ── Streaming ───────────────────────────────────────────────────────────────────────────

    @abc.abstractmethod
    def feed(self, pcm: bytes) -> list[Hypothesis]:
        """Consume 16 kHz mono s16le audio; return zero or more **cumulative** hypotheses.

        Returning an empty list is normal. Returning a delta is a bug (docs/protocol.md §8.1).
        Implementations should call ``self._check_pcm(pcm)`` first.
        """

    @abc.abstractmethod
    def finalize(self) -> Optional[Hypothesis]:
        """Flush the decoder and return the final hypothesis for the current utterance.

        ``None`` means "there was nothing to finalise" — no speech was ever fed. That is a real
        state, not an error: it is what a silence-only utterance produces in a backend that does
        not hallucinate, and the server turns it into ``audio_underrun`` rather than an empty
        ``final``.

        The returned hypothesis carries ``is_final=True`` and complete text for the utterance.
        Does **not** advance to the next utterance by itself in every backend — call ``reset``.
        """

    @abc.abstractmethod
    def _reset(self, reason: ResetReason) -> None:
        """Drop per-utterance decoder state."""

    def reset(self, reason: ResetReason = ResetReason.UTTERANCE_BOUNDARY) -> None:
        """Utterance boundary, discontinuity, cancel, or session edge."""
        self._reset(reason)

    def on_discontinuity(self, gap_ms: int) -> None:
        """The client reported lost audio (docs/protocol.md §6.1).

        Default: reset if the backend declared ``requires_contiguous_audio``. The alternative —
        carrying on across a gap — hands the decoder audio that is not contiguous with its state,
        and plan.md §1.2 is explicit that this produces mysteriously bad transcription that is
        indistinguishable from a bad model.
        """
        if self.capabilities.requires_contiguous_audio:
            self.reset(ResetReason.DISCONTINUITY)

    # ── Context / vocabulary biasing ────────────────────────────────────────────────────────

    @property
    def context(self) -> tuple[str, ...]:
        return self._context

    def set_context(self, terms: Sequence[str]) -> bool:
        """Offer vocabulary hints from ``start.context``. Returns True if they were applied.

        The default stores and returns ``supports_context``, so a backend that cannot bias reports
        ``False`` honestly instead of accepting the terms and quietly discarding them. That
        honesty is load-bearing: docs/protocol.md §6.1 requires a Server without context support
        to *ignore* the field and report ``context_supported:false``, because plan.md §4 scores
        context-enabled and context-free runs in separate columns.
        """
        self._context = tuple(terms)
        return self.capabilities.supports_context

    # ── Helpers for subclasses ──────────────────────────────────────────────────────────────

    def _require_loaded(self, what: str) -> None:
        if not self._loaded:
            raise BackendStateError(f"{what}() called before load()")

    def _check_pcm(self, pcm: bytes) -> None:
        """Guard the audio contract at the backend boundary.

        An odd length means a sample was split, which docs/protocol.md §6.3 makes ``bad_message``
        at the framing layer. It should never reach here — which is exactly why it is checked
        here: if it does, something upstream is slicing buffers wrongly and a loud failure is far
        cheaper than half-rate garbled audio.
        """
        if len(pcm) % BYTES_PER_SAMPLE:
            raise BackendError(
                f"odd-length PCM chunk ({len(pcm)} bytes): a sample was split",
                wire_code="bad_message",
            )

    @staticmethod
    def _report(progress: Optional[ProgressFn], stage: str, pct: Optional[int]) -> None:
        if progress is not None:
            progress(stage, pct)

    def describe(self) -> dict:
        """A flat dict for logs and ``bench/results/*.json``. Never parsed for behaviour."""
        caps = self.capabilities
        return {
            "model": caps.model_id,
            "mode": caps.mode.value,
            "context_supported": caps.supports_context,
            "natural_chunk_ms": caps.natural_chunk_ms,
            "chunk_ms_configurable": caps.chunk_ms_configurable,
            "supported_chunk_ms": list(caps.supported_chunk_ms),
            "notes": caps.notes,
            "load_ms": self._load_ms,
            "warm_ms": self._warm_ms,
        }


def _elapsed_ms(t0: float) -> int:
    return int(round((time.perf_counter() - t0) * 1000.0))


# ─── Pseudo-streaming: a batch model driven by a window we built ────────────────────────────


class PseudoStreamingBackend(Backend):
    """Overlapping-window driver over a batch model. **This is not streaming.**

    The label is the point. plan.md §3 Phase 0A requires the streaming mode to be recorded because
    *"a batch Whisper adapter wrapped in a sliding window is not comparable to a natively
    streaming one"*, and bench/CANDIDATES.md §4 pick 4 requires this mode's latency numbers to be
    reported in a separate block. Any subclass of this class declares
    ``StreamingMode.PSEUDO_STREAMING`` and ``load()`` enforces it.

    A subclass implements exactly one method::

        def _transcribe_batch(self, pcm: bytes) -> str: ...

    which transcribes a whole buffer offline. Everything else — buffering, hop timing, window
    sliding, prefix stitching, cumulative assembly — is here. That is the ~50-line adapter
    plan.md §1.1 promises, for the batch half of the candidate list (rows D, E, E′, F, G in
    bench/CANDIDATES.md).

    ── How it works, and what it costs ─────────────────────────────────────────────────────

    Every ``hop_ms`` of audio, the whole current window is re-transcribed from scratch and the
    result is emitted as the cumulative hypothesis. While the utterance is shorter than
    ``window_ms`` the window starts at the utterance start, so the hypothesis is simply "the
    transcript of everything said so far" and cumulativity is trivially exact. Typical dictation
    utterances endpoint at ~0.8 s of silence (plan.md §1.6), so with the 8 s default window most
    real utterances never leave this regime.

    ── The honest part: what happens when the window slides ────────────────────────────────

    Past ``window_ms``, the oldest audio scrolls out of the window and the batch model can no
    longer see it. Its transcript must be **frozen** into a prefix, because there is nothing left
    to re-derive it from. The freeze point is estimated by word-overlap stitching between the
    pre-slide and post-slide transcripts, and that estimate can be wrong:

    * repeated words ("the the", "very very") make the overlap ambiguous and can freeze one word
      too few or too many;
    * a model that repunctuates or recases across the boundary produces no exact overlap at all,
      in which case the fallback freezes the entire pre-slide transcript, duplicating nothing but
      committing more than necessary;
    * a revision that *would* have crossed the boundary is lost — the frozen prefix is effectively
      a commitment made by this wrapper, and nothing downstream can undo it.

    That last point deserves emphasis because it interacts with plan.md §1.4: the frozen prefix is
    not "stable" in the §1.4 sense — no stability computation blessed it — it is merely
    unrecoverable. The server's stability computation still runs on top and still decides what
    reaches the document; this wrapper only guarantees that what it emits is cumulative, never
    that it is correct across a slide. For the bake-off this is acceptable because it is measured:
    a wrapper that mis-stitches shows up as WER, and its latency was never comparable anyway.

    Re-transcription cost grows with window length: a 10-second window re-decoded every 640 ms is
    ~15 decodes of up to 10 s of audio. This is why pseudo-streaming is a baseline and not a
    shipping strategy.
    """

    def __init__(
        self,
        *,
        window_ms: int = 8000,
        hop_ms: int = 640,
    ) -> None:
        super().__init__()
        if hop_ms <= 0:
            raise ValueError("hop_ms must be positive")
        if window_ms < hop_ms:
            raise ValueError("window_ms must be at least hop_ms")
        self._window_ms = window_ms
        self._hop_ms = hop_ms
        self._window_bytes = ms_to_bytes(window_ms)
        self._hop_bytes = ms_to_bytes(hop_ms)
        self._buf = bytearray()
        self._frozen = ""
        self._prev_window_text = ""
        self._pending_bytes = 0
        self._audio_ms = 0
        self._any_audio = False

    # ── The one thing a subclass must supply ────────────────────────────────────────────────

    @abc.abstractmethod
    def _transcribe_batch(self, pcm: bytes) -> str:
        """Transcribe a complete buffer offline. Called once per hop with the whole window."""

    # ── Driver ──────────────────────────────────────────────────────────────────────────────

    def load(self, progress: Optional[ProgressFn] = None) -> int:
        ms = super().load(progress)
        mode = self.capabilities.mode
        if mode is not StreamingMode.PSEUDO_STREAMING:
            # Refusing rather than correcting: a wrapper that silently relabelled itself would
            # put uncomparable latency numbers in the same column as the native-streaming rows,
            # which is the exact failure plan.md §3 added the column to prevent.
            raise BackendLoadError(
                f"{type(self).__name__} subclasses PseudoStreamingBackend but declares "
                f"mode={mode.value!r}; it must declare "
                f"{StreamingMode.PSEUDO_STREAMING.value!r}"
            )
        return ms

    @property
    def hop_ms(self) -> int:
        return self._hop_ms

    @property
    def window_ms(self) -> int:
        return self._window_ms

    @property
    def frozen_prefix(self) -> str:
        """Text whose audio has scrolled out of the window and can no longer be revised."""
        return self._frozen

    def feed(self, pcm: bytes) -> list[Hypothesis]:
        self._require_loaded("feed")
        self._check_pcm(pcm)
        if not pcm:
            return []
        self._buf.extend(pcm)
        self._pending_bytes += len(pcm)
        self._audio_ms += bytes_to_ms(len(pcm))
        self._any_audio = True
        out: list[Hypothesis] = []
        while self._pending_bytes >= self._hop_bytes:
            self._pending_bytes -= self._hop_bytes
            out.append(self._emit(is_final=False))
        return out

    def finalize(self) -> Optional[Hypothesis]:
        self._require_loaded("finalize")
        if not self._any_audio:
            return None
        return self._emit(is_final=True)

    def _emit(self, *, is_final: bool) -> Hypothesis:
        prev_window_text = self._prev_window_text
        slid = len(self._buf) > self._window_bytes
        if slid:
            # Keep the newest window_bytes. Everything older is gone from the model's view.
            del self._buf[: len(self._buf) - self._window_bytes]
        text = self._transcribe_batch(bytes(self._buf))
        if slid:
            self._frozen = _join_words(
                self._frozen, _scrolled_off_words(prev_window_text, text)
            )
        self._prev_window_text = text
        return Hypothesis(
            text=_join_words(self._frozen, text),
            is_final=is_final,
            audio_pos_ms=self._audio_ms,
        )

    def _reset(self, reason: ResetReason) -> None:
        self._buf.clear()
        self._frozen = ""
        self._prev_window_text = ""
        self._pending_bytes = 0
        self._any_audio = False
        if reason in (ResetReason.SESSION_START, ResetReason.SESSION_END, ResetReason.WARMUP):
            self._audio_ms = 0


def _join_words(a: str, b: str) -> str:
    """Join two text fragments with exactly one space, tolerating empties."""
    a = a.rstrip()
    b = b.lstrip()
    if not a:
        return b
    if not b:
        return a
    return f"{a} {b}"


def _norm_word(w: str) -> str:
    """Comparison key for stitching: case- and edge-punctuation-insensitive.

    Deliberately crude. A model that recases or repunctuates across a window boundary should still
    stitch, but nothing here tries to be clever about it — an over-clever matcher would silently
    freeze the wrong words, and a crude one that fails falls back to the conservative branch.
    """
    return w.strip(".,;:!?\"'()[]{}…").casefold()


def _scrolled_off_words(prev_text: str, new_text: str) -> str:
    """Estimate which words left the window between two successive transcripts.

    ``prev_text`` covers audio ``[s0, t0]``; ``new_text`` covers ``[s1, t1]`` with ``s1 > s0`` and
    ``t1 > t0``. The words that scrolled off are a prefix of ``prev_text``. Find the largest
    ``k`` such that the last ``k`` words of ``prev_text`` equal the first ``k`` words of
    ``new_text``; the first ``len(prev) - k`` words are what scrolled off.

    ``k == 0`` (no overlap found) freezes all of ``prev_text``. That is the conservative branch:
    it can commit text the next window would have revised, but the alternative — dropping it —
    would delete words the user actually said, and losing text is worse than freezing it early.
    """
    pw = prev_text.split()
    nw = new_text.split()
    if not pw:
        return ""
    pn = [_norm_word(w) for w in pw]
    nn = [_norm_word(w) for w in nw]
    limit = min(len(pn), len(nn))
    for k in range(limit, 0, -1):
        if pn[len(pn) - k :] == nn[:k]:
            return " ".join(pw[: len(pw) - k])
    return " ".join(pw)
