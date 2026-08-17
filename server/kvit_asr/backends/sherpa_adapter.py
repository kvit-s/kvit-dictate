"""`sherpa_onnx_streaming` and `zipformer_hotwords`, wearing the `Backend` ABC.

Both of those modules were written standalone, against a plausible lifecycle of their own::

    b.load(progress); b.start_session(id, context); b.accept_pcm(pcm) -> str
    b.finish_utterance() -> str; b.discard_utterance(); b.end_session(); b.close()

`backends.base.Backend` — which is what `backends.load_backend` resolves to and what
`connection.LoadedBackend` drives — asks for a different one::

    b.load(progress); b.reset(ResetReason); b.feed(pcm) -> [Hypothesis]
    b.finalize() -> Hypothesis | None; b.unload()

Neither is wrong and neither module is at fault. What was missing is the thing this file is: an
owner for the space between them. This is the same defect, and the same shape of fix, as
`kvit_asr.connection` — handover.md §5.8 and §5.19 record the pattern (*a contract written down in
a header, implemented on one side, and never called from the other*), and the symptom here was
that `--backend sherpa-onnx`, which three entries of models.json actually pass, resolved to
nothing: the class is not a `Backend` subclass, so `load_backend` refused it even on a machine
with sherpa-onnx installed.

Wrapping rather than rewriting is deliberate. The inner lifecycle is documented, reasoned about
(see `finish_utterance` on why the stream is dropped rather than `reset`) and carries eight
recorded UNVERIFIED items against sherpa-onnx's real API. Re-shaping it to fit the ABC would put
those decisions somewhere new on a machine where none of them can be executed. Translating it
keeps them where their reasoning is.

─── The three translations, and what each costs ───────────────────────────────────────────────

**Reset reasons → lifecycle calls.** The ABC has one `reset(reason)`; the inner has four distinct
calls. The mapping is in `_reset` and is total, so a reason nobody thought about becomes
`discard_utterance` — the conservative branch, since dropping an in-flight utterance is recoverable
and carrying one across an unknown boundary is not.

**Session identity.** `start_session` wants a `session_id`; the ABC carries none — session identity
lives one layer up, in `connection.ConnectionSession`. The adapter therefore synthesises
`"adapter-N"`, which is used by the inner only for log lines. A log correlating a sherpa warning
with a protocol `session_id` is not possible through this seam, and that is the cost.

**Context, at session start only.** `LoadedBackend` calls `set_context` before every reset, but the
inner applies hotwords in `start_session`/`_create_stream`. So a context change *within* a session
is stored and not applied until the next one. `start.context` is per-session in
docs/protocol.md §6.1, so nothing in the protocol can currently reach that case — but a future
mid-session vocabulary update would silently do nothing, which is why it is written down here.

Zero third-party imports at module scope, and the inner modules are imported lazily inside
`_inner_classes()`, so the package rule holds: asking for `sherpa-onnx` must not import the
zipformer module, and neither import may pull in `sherpa_onnx` itself (which happens inside the
inner `load()`).
"""

from __future__ import annotations

import contextlib
from typing import Any, Iterator, Mapping, Optional, Sequence

from .base import (
    Backend,
    BackendCapabilities,
    BackendDecodeError,
    BackendError,
    Hypothesis,
    ProgressFn,
    ResetReason,
    StreamingMode,
    bytes_to_ms,
)

__all__ = [
    "SherpaOnnxBackend",
    "ZipformerBackend",
    "create",
    "create_zipformer",
]


@contextlib.contextmanager
def _as_decode_error(what: str) -> Iterator[None]:
    """Translate an inner failure during decoding into `decode_failed`.

    The fourth translation, and the one with consequences beyond tidiness. The inner raises
    `SherpaOnnxBackendError`, which is a plain `RuntimeError` and **not** a
    `backends.base.BackendError` — so without this it reaches the server as an unclassified
    exception and is reported as a fatal `internal_error`, taking the connection down with it.

    docs/protocol.md §11.3 makes `decode_failed` explicitly **non-fatal**: the session is aborted,
    the connection survives, and a new session may follow. That is the right outcome for a decode
    that blew up on one utterance, and it is the difference between a hotkey press that fails and
    a hotkey that stops working until the process is restarted.

    A `BackendError` from the inner already carries its own `wire_code` and is left alone.
    """
    try:
        yield
    except BackendError:
        raise
    except Exception as exc:  # noqa: BLE001 — any runtime's failure mode; see the docstring
        raise BackendDecodeError(f"{what} failed: {type(exc).__name__}: {exc}") from exc


class SherpaOnnxBackend(Backend):
    """`sherpa_onnx_streaming.SherpaOnnxStreamingBackend` as a `backends.base.Backend`.

    Construct it from a parsed command line with :meth:`from_backend_config`, from a flat option
    mapping with :meth:`from_options`, or directly from an already-built inner object — the last
    form is what makes this testable on a machine with no sherpa-onnx, since the inner can be a
    stand-in.
    """

    @staticmethod
    def _inner_classes() -> tuple[type, type]:
        """`(backend_cls, config_cls)`, imported now rather than at module scope.

        Lazy for the reason `backends/__init__.py` gives: asking for one backend must not import
        another's module. Neither of these imports a model runtime — `sherpa_onnx` itself is
        imported inside the inner `load()`.
        """
        from . import sherpa_onnx_streaming as m  # noqa: PLC0415 — deliberately lazy

        return m.SherpaOnnxStreamingBackend, m.SherpaOnnxStreamingConfig

    # ── construction ────────────────────────────────────────────────────────────────────────

    def __init__(self, inner: Any) -> None:
        super().__init__()
        self._inner = inner
        # Whether a decode stream exists for the current utterance. `finalize()` needs it because
        # base.py defines a `None` return as "there was nothing to finalise", which the server
        # turns into `audio_underrun` — a different outcome from an empty `final`.
        self._utterance_open = False
        # Absolute position in the session's audio, ms. `Hypothesis.audio_pos_ms` is optional on
        # `partial` and required on `final` (docs/protocol.md §6.2); the inner reports no position
        # at all, so it is counted here from the bytes actually fed.
        self._audio_ms = 0
        self._sessions = 0

    @classmethod
    def from_options(cls, options: Mapping[str, Any] | None = None, **overrides: Any):
        backend_cls, config_cls = cls._inner_classes()
        merged: dict[str, Any] = dict(options or {})
        merged.update(overrides)
        return cls(backend_cls(config_cls.from_mapping(merged)))

    @classmethod
    def from_backend_config(cls, cfg: Any):
        """`kvit_asr.connection.open_backend`'s hook: build from a parsed `BackendConfig`.

        Written out rather than derived, because the mapping is genuinely per-backend:

        * `--chunk-ms` is **not** a free parameter here. Each published chunk size is a separate
          repository with different weights (see the inner module's "Chunk size is a model, not a
          parameter"), so this value is a *declaration* that the inner cross-checks against the
          model directory name and rejects on contradiction.
        * `--decoding` maps to `decoding_method`. Left unset it stays `greedy_search`, which is
          the only method the Nemotron transducer supports (bench/CANDIDATES.md §5.1).
        * `--device` maps to sherpa-onnx's `provider`. `auto` is deliberately **not** resolved
          here: the inner's default is `cpu` — CANDIDATES.md §6.1 makes CPU the baseline because
          onnxruntime-gpu's sm_120 support is unconfirmed — and `load()` logs the providers
          onnxruntime actually advertises, which is the reporting the `--device` help promises.
        * `dml` is refused rather than translated. sherpa-onnx's provider strings are not verified
          for DirectML on this machine, and inventing one would fail deep inside the library with
          a message about an unknown provider instead of here with a message about a choice.

        `--backend-opt` is applied last, so it overrides anything derived here.
        """
        options: dict[str, Any] = {}
        if getattr(cfg, "model", None):
            options["model"] = cfg.model
        if getattr(cfg, "model_dir", None) is not None:
            options["model_dir"] = str(cfg.model_dir)
        if getattr(cfg, "chunk_ms", None) is not None:
            options["chunk_ms"] = cfg.chunk_ms
        if getattr(cfg, "decoding", None):
            options["decoding_method"] = cfg.decoding

        device = str(getattr(cfg, "device", "auto") or "auto")
        if device == "dml":
            raise ValueError(
                "--device dml is not available for the sherpa-onnx backend: this project has not "
                "verified a DirectML execution provider for sherpa-onnx on this machine, and "
                "guessing the provider string would fail inside the library rather than here. "
                "Use cpu (the CANDIDATES.md §6.1 baseline) or cuda."
            )
        if device != "auto":
            options["provider"] = device

        options.update(getattr(cfg, "options", None) or {})
        return cls.from_options(options)

    # ── declaration ─────────────────────────────────────────────────────────────────────────

    @property
    def inner(self) -> Any:
        """The wrapped lifecycle object. For the bench harness and for `diagnostics`."""
        return self._inner

    @property
    def capabilities(self) -> BackendCapabilities:
        """Note one honest departure from base.py's "constant for the lifetime of the instance".

        `natural_chunk_ms` is 0 before `load()` when the command line did not declare one, because
        the inner derives it from the model directory name and that directory has not been read
        yet. It is settled by the time `ready` is built — the server sends `ready` only after
        `load()` returns — so the wire value is always the resolved one. Reporting a guess before
        the files have been looked at would be the worse wart: `ready.chunk_ms` feeds plan.md §4's
        sweep, and a mislabelled cell corrupts the comparison silently.
        """
        inner = self._inner
        return BackendCapabilities(
            model_id=str(inner.model_id),
            mode=StreamingMode(str(inner.mode)),
            # Declared by the inner, never inferred. For the Nemotron rows this is always False
            # (greedy_search only); for the zipformer arm the instance narrows the class default
            # according to the decoding method actually selected.
            supports_context=bool(inner.supports_context),
            natural_chunk_ms=int(getattr(inner, "chunk_ms", None) or 0),
            # A different chunk size is a different repository of weights, not a setter call.
            chunk_ms_configurable=False,
            # The published set is per-family and the inner validates against it at load; listing
            # it here before the family is known would be a guess.
            supported_chunk_ms=(),
            # A cache-aware transducer's encoder state is exactly what a gap corrupts.
            requires_contiguous_audio=True,
            notes=self._notes(),
        )

    def _notes(self) -> str:
        cfg = getattr(self._inner, "config", None)
        if cfg is None:  # a stand-in inner, in tests
            return type(self._inner).__name__
        return (
            f"sherpa-onnx {type(self._inner).__name__}, "
            f"decoding={getattr(cfg, 'decoding_method', '?')}, "
            f"provider={getattr(cfg, 'provider', '?')}, "
            f"precision={getattr(cfg, 'precision', '?')}"
        )

    # ── lifecycle ───────────────────────────────────────────────────────────────────────────

    def _load(self, progress: Optional[ProgressFn]) -> None:
        self._inner.load(progress)

    def _warm_up(self, audio_ms: int) -> None:
        """Skip base.py's silence pass when the inner already warmed itself inside `load()`.

        `SherpaOnnxStreamingBackend.load()` runs `warmup_ms` of silence through a throwaway stream
        for exactly the reason base.py's `warm_up` exists, so doing it twice buys nothing and adds
        a second of startup before `ready`. When it is switched off (`warmup_ms=0`) the base
        behaviour is the right one and is used.
        """
        cfg = getattr(self._inner, "config", None)
        if cfg is not None and int(getattr(cfg, "warmup_ms", 0) or 0) > 0:
            return
        super()._warm_up(audio_ms)

    def _unload(self) -> None:
        self._inner.close()

    def feed(self, pcm: bytes) -> list[Hypothesis]:
        self._require_loaded("feed")
        self._check_pcm(pcm)
        if not pcm:
            # §6.3 allows a zero-length payload. It advances nothing, so it produces nothing:
            # emitting a hypothesis here would restart the stability dwell (plan.md §1.4 step 5)
            # on a frame that carried no audio.
            return []
        with _as_decode_error("accept_pcm"):
            text = self._inner.accept_pcm(pcm)
        self._utterance_open = True
        self._audio_ms += bytes_to_ms(len(pcm))
        # Exactly one hypothesis per chunk, and it is the inner's **cumulative** text for the
        # utterance, not a delta (docs/protocol.md §8.1). Emitting it even when unchanged is
        # deliberate: the stability rule counts a candidate as stable when it is unchanged across
        # hypotheses, so suppressing repeats here would starve the very computation that decides
        # what may be committed.
        return [Hypothesis(text=text, audio_pos_ms=self._audio_ms)]

    def finalize(self) -> Optional[Hypothesis]:
        self._require_loaded("finalize")
        if not self._utterance_open:
            # base.py: None means "there was nothing to finalise". The server turns that into
            # `audio_underrun` rather than an empty `final`, which is the right report for an
            # utterance that never received a sample.
            return None
        with _as_decode_error("finish_utterance"):
            text = self._inner.finish_utterance()
        self._utterance_open = False
        return Hypothesis(text=text, is_final=True, audio_pos_ms=self._audio_ms)

    def _reset(self, reason: ResetReason) -> None:
        self._utterance_open = False
        if not self._inner.is_loaded:
            # `reset` before `load` is legal on the ABC (the server resets at session start, and a
            # warm-up resets too), but every inner call below raises without a recognizer. There
            # is no state to drop yet, so doing nothing is the correct translation.
            return

        if reason is ResetReason.SESSION_START:
            self._sessions += 1
            self._audio_ms = 0
            # The inner uses `session_id` only for log lines; the protocol's own id lives in
            # `connection.ConnectionSession` and does not reach this seam. See the module
            # docstring.
            self._inner.start_session(f"adapter-{self._sessions}", context=self.context)
        elif reason is ResetReason.SESSION_END:
            self._audio_ms = 0
            self._inner.end_session()
        else:
            # UTTERANCE_BOUNDARY, DISCONTINUITY, CANCEL, WARMUP — and anything added later. Drop
            # the in-flight stream, which for a cache-aware transducer is a full reset of encoder
            # cache and feature history. The audio clock keeps running: it is a position in the
            # *session* timeline, and utterances do not restart it.
            self._inner.discard_utterance()

    # ── context ─────────────────────────────────────────────────────────────────────────────

    def set_context(self, terms: Sequence[str]) -> bool:
        """Store the hints; they reach the inner at the next `SESSION_START`.

        The inner applies hotwords when it opens a session and when it creates a stream, so there
        is no route for a mid-session change — see the module docstring. Storing rather than
        raising is correct because `start.context` is per-session in docs/protocol.md §6.1: the
        only call that can happen mid-session repeats what was already applied.
        """
        return super().set_context(terms)


class ZipformerBackend(SherpaOnnxBackend):
    """The context-biasing arm: `zipformer_hotwords.ZipformerHotwordsBackend` as a `Backend`.

    A separate registry name rather than `--backend sherpa-onnx --decoding modified_beam_search`,
    because the base class **refuses** that decoding method (bench/CANDIDATES.md §5.1,
    k2-fsa/sherpa-onnx#3572) — the biasing arm is a different class, not a flag. Selecting
    `--decoding greedy_search` here is still allowed and still meaningful: it measures the same
    model context-free, and the inner narrows `supports_context` to False so the run is filed in
    the right column of plan.md §4.
    """

    @staticmethod
    def _inner_classes() -> tuple[type, type]:
        from . import zipformer_hotwords as m  # noqa: PLC0415 — deliberately lazy

        return m.ZipformerHotwordsBackend, m.ZipformerHotwordsConfig


def create(options: Mapping[str, Any] | None = None, **kwargs: Any) -> SherpaOnnxBackend:
    """Factory a caller can use without importing the config dataclass. Registry entry:

        "sherpa-onnx": ".sherpa_adapter:SherpaOnnxBackend"
    """
    return SherpaOnnxBackend.from_options(options, **kwargs)


def create_zipformer(options: Mapping[str, Any] | None = None, **kwargs: Any) -> ZipformerBackend:
    """As `create`, for the biasing arm. Registry entry:

        "zipformer": ".sherpa_adapter:ZipformerBackend"
    """
    return ZipformerBackend.from_options(options, **kwargs)
