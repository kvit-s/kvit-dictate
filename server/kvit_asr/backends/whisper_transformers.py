"""Whisper via the HuggingFace `transformers` pipeline — the zero-install accuracy baseline.

`bench/CANDIDATES.md` §4 pick 4 (row D): `openai/whisper-large-v3-turbo`, already cached in
`HF_HOME=c:\\apps\\llama-swap\\gguf\\hf`, run under an interpreter that already has torch. It
costs nothing to stand up, it anchors the accuracy scale for every other candidate, and it is the
natural partner for the manual `Win+H` comparison in plan.md §4.

──────────────────────────────────────────────────────────────────────────────────────────────
This runs under **another project's** interpreter, and installs nothing
──────────────────────────────────────────────────────────────────────────────────────────────

plan.md §5.1 and `bench/CANDIDATES.md` §3 are explicit and this module inherits the rule without
softening it: `D:\\projects\\wan-dancer\\.venv` is a working GPU stack belonging to another
project (it contains `sageattention` compiled against that exact torch build) and it is
**read-only for kvit-dictate**. We run the Whisper baseline that is already proven there and
install nothing — not `faster-whisper`, not an "obviously compatible" pin, not under a
`--dry-run` justification.

That is exactly the case `models.json`'s model→interpreter mapping exists for (plan.md §1.1): the
entry for this backend names `D:\\projects\\wan-dancer\\.venv\\Scripts\\python.exe` as its
interpreter, and selecting the model restarts the sidecar under it. So this module must be
importable under a bare stdlib CPython (it is — every heavy import is inside `_load()`), and must
fail with a legible message rather than a bare `ModuleNotFoundError` when it is launched under an
interpreter without torch.

──────────────────────────────────────────────────────────────────────────────────────────────
Pseudo-streaming, said plainly
──────────────────────────────────────────────────────────────────────────────────────────────

Whisper is a batch encoder-decoder over 30-second windows. There is no cache-aware causal mode to
drive, so this backend is `PseudoStreamingBackend` from `base.py`: an overlapping window that we
built, re-transcribed from scratch every hop. `ready.mode` is therefore `"pseudo-streaming"` and
its latency numbers are **not** comparable with the native-streaming rows — `CANDIDATES.md` §4
requires them to be reported in a separate block. Dressing this up as streaming would corrupt the
only comparison Phases 1 and 2 exist to make.

──────────────────────────────────────────────────────────────────────────────────────────────
The silence-hallucination guard (plan.md §1.6)
──────────────────────────────────────────────────────────────────────────────────────────────

`..\\wan-dancer\\notes\\transcribe.py` carries the observation plan.md §1.6 quotes: Whisper emits
`"Thank you."` / `"Bye"` over trailing silence, **with timestamps past the end of the audio**.
Its fix — keep only chunks whose start timestamp lies inside the audio — is reproduced here in
`_apply_hallucination_guard()`, and every drop is logged and counted.

Two honest caveats:

* VAD gating upstream is the real fix (plan.md §1.6: stop feeding at the endpoint, reset decoder
  state, resume on speech onset). This guard is a second line of defence inside the backend, not
  a substitute for the server's endpointing.
* It is a *timestamp* guard, not a phrase blocklist. A hallucination that lands with plausible
  timestamps inside the audio survives it. A blocklist of known-hallucinated phrases was
  considered and rejected: it would silently delete text a user actually said ("thank you" is a
  perfectly ordinary dictated sentence), and silent deletion is the failure mode this project is
  least willing to accept. The hallucination-on-silence rate is a measured metric in plan.md §4,
  not something to paper over here.

──────────────────────────────────────────────────────────────────────────────────────────────
Decoding parameters are inherited from a known-working invocation, not invented
──────────────────────────────────────────────────────────────────────────────────────────────

plan.md §5.1: *"Phase 1's baseline adapter can be a near-copy of it."* The temperature-fallback
tuple and the three thresholds below are copied from `notes/transcribe.py`, which is a proven
working invocation of this exact model on this exact GPU. They were tuned there for **sung vocals
over a dense mix**, which is not our workload; they are kept as defaults because matching a
known-working call is worth more than a fresh guess, and every one of them is configurable so
Phase 1 can measure whether they help or hurt dictation. `condition_on_prev_tokens=False` is kept
for a reason that *is* ours: conditioning on previous text is a documented driver of
hallucination loops, and every window here is re-decoded from scratch anyway.

──────────────────────────────────────────────────────────────────────────────────────────────
Environment — nothing to install, one thing to check
──────────────────────────────────────────────────────────────────────────────────────────────

No lock file exists for this backend and none should: its environment is `..\\wan-dancer\\.venv`
exactly as it is today (plan.md §5.1: `python 3.12.9 / torch 2.10.0+cu130 / transformers 5.14.1 /
numpy 2.4.4`). If that venv ever stops working for us, the response is a **new** locked env under
`envs/`, never a package added to that one. The weights are already cached; if they are not, this
module raises with the `hf download` command rather than fetching anything (plan.md §1.9: no
model auto-download at run time).

──────────────────────────────────────────────────────────────────────────────────────────────
Verified / UNVERIFIED
──────────────────────────────────────────────────────────────────────────────────────────────

Nothing here has been *executed* — no environment was created and running the wan-dancer venv is
a human decision. What follows was read directly out of the installed
`transformers 5.14.1` sources under that venv on 2026-08-16, which is stronger than memory but
weaker than a run:

VERIFIED by reading the installed source:
 1. `pipeline(task, model=…, dtype=…, device=…)` — `dtype` is the 5.x spelling; `torch_dtype` is
    deprecated (`pipelines/base.py` warns and drops it). `notes/transcribe.py` already uses
    `dtype`.
 2. `pipeline(..., local_files_only=True)` is honoured: `pipelines/__init__.py` reads
    `kwargs["local_files_only"]` into `hub_kwargs`, which every component loader receives.
 3. The ASR pipeline accepts `{"raw": <array>, "sampling_rate": int}`
    (`automatic_speech_recognition.py` `preprocess`), and its result is
    `{"text": str, "chunks": [{"text": str, "timestamp": (start, end)}, …]}` when
    `return_timestamps=True`.
 4. Temperature fallback is driven by a shared loop for both short-form and long-form input, so a
    temperature *tuple* is meaningful even for the few-second windows this backend feeds.
 5. `WhisperTokenizer.get_prompt_ids()` exists and `WhisperGenerationMixin.generate()` takes
    `prompt_ids`.

UNVERIFIED — checked at run time and turned into a specific error, never a silent fallback:
 A. Whether the pipeline forwards `prompt_ids` through `generate_kwargs` intact, and whether the
    prompt text can leak into the transcript. `set_context()` builds the ids and
    `_transcribe_batch()` logs loudly if the output looks like it starts with the prompt. It does
    **not** strip it — a silent repair here would hide the defect from the Phase 1 measurement.
 B. Whether a `(start, end)` timestamp can be `None` for a *leading* chunk in short-form output.
    Treated as "unusable timestamp" and dropped, with a log line, because a hypothesis whose
    position cannot be checked cannot be checked against the audio length either.
 C. Wall-clock cost per window on this GPU. The re-decode-every-hop design is affordable only if
    a window decode is well under the hop; `_transcribe_batch()` logs decode time and warns once
    when a decode takes longer than the hop, because at that point the backend is falling behind
    real time and the latency numbers stop meaning anything.
"""

from __future__ import annotations

import logging
import random
import sys
import time
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from typing import Any

from .base import (
    SAMPLE_RATE,
    BackendCapabilities,
    BackendDecodeError,
    BackendLoadError,
    ProgressFn,
    PseudoStreamingBackend,
    ResetReason,
    StreamingMode,
    ms_to_bytes,
    pcm_peak,
)

__all__ = [
    "DEFAULT_MODEL",
    "WhisperTransformersConfig",
    "WhisperTransformersBackend",
    "WhisperUnavailable",
    "WhisperApiMismatch",
    "WhisperWeightsMissing",
    "DroppedSegment",
    "create",
]

log = logging.getLogger(__name__)

DEFAULT_MODEL = "openai/whisper-large-v3-turbo"

# The interpreter this backend is meant to run under (models.json entry `whisper-large-v3-turbo`).
# Quoted in error messages so a human reading a log knows which python.exe was supposed to be used
# without having to open models.json.
EXPECTED_INTERPRETER = r"D:\projects\wan-dancer\.venv\Scripts\python.exe"


class WhisperUnavailable(BackendLoadError):
    """torch / transformers / numpy are not importable here. This backend is disabled.

    A `BackendLoadError`, so the server maps it to `model_load_failed` (docs/protocol.md §11.3)
    and surfaces `msg` verbatim. It never reaches package-import time: `kvit_asr` must stay
    importable on a bare stdlib CPython, so the imports that can raise this live in `_load()`.
    """


class WhisperApiMismatch(BackendLoadError):
    """transformers is installed but is not shaped the way this adapter was written against.

    Raised deliberately, with the installed version and what was actually found, rather than
    letting a `KeyError` or `TypeError` escape from inside a decode. Every unverified API detail
    in the module docstring funnels into this exception.
    """


class WhisperWeightsMissing(BackendLoadError):
    """The model is not in the local HuggingFace cache and downloads are disabled.

    plan.md §1.9: no model auto-download at run time; fetching is an explicit, separate,
    user-initiated action. The message carries the command a human runs.
    """


# ── Configuration ──────────────────────────────────────────────────────────────────────────────


def _as_bool(value: Any, name: str) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        low = value.strip().lower()
        if low in ("1", "true", "yes", "on"):
            return True
        if low in ("0", "false", "no", "off"):
            return False
    raise ValueError(f"{name}: expected a boolean, got {value!r}")


def _as_int(value: Any, name: str) -> int:
    if isinstance(value, bool):  # bool is an int subclass; almost never what was meant
        raise ValueError(f"{name}: expected an integer, got {value!r}")
    if isinstance(value, int):
        return value
    if isinstance(value, str) and value.strip().lstrip("-").isdigit():
        return int(value.strip())
    raise ValueError(f"{name}: expected an integer, got {value!r}")


def _as_float(value: Any, name: str) -> float:
    if isinstance(value, bool):
        raise ValueError(f"{name}: expected a number, got {value!r}")
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, str):
        try:
            return float(value.strip())
        except ValueError:
            pass
    raise ValueError(f"{name}: expected a number, got {value!r}")


def _as_temperatures(value: Any, name: str) -> tuple[float, ...]:
    """Parse a temperature-fallback ladder: `0.0,0.2,0.4` or a real sequence."""
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return (float(value),)
    if isinstance(value, str):
        parts = [p for p in value.replace(";", ",").split(",") if p.strip()]
        return tuple(_as_float(p, name) for p in parts)
    if isinstance(value, Sequence):
        return tuple(_as_float(v, name) for v in value)
    raise ValueError(f"{name}: expected a number or a comma-separated list, got {value!r}")


@dataclass
class WhisperTransformersConfig:
    """Everything this backend needs, with no reference to argparse or models.json."""

    model: str = DEFAULT_MODEL

    # -- window driver (base.PseudoStreamingBackend) ---------------------------------------------
    window_ms: int = 8000
    """How much audio the model sees. Longer = better context and worse re-decode cost; audio
    older than this scrolls out and its transcript is frozen by the wrapper (see
    `PseudoStreamingBackend`'s docstring on what that costs). 8 s comfortably covers a dictation
    utterance, which endpoints after ~0.8 s of silence (plan.md §1.6)."""

    hop_ms: int = 640
    """How often a new hypothesis is produced. Also `ready.chunk_ms`. 640 ms is the largest cell of
    plan.md §4's `chunk_ms` sweep and the cheapest; smaller hops multiply the re-decode cost
    linearly, which is why pseudo-streaming is a baseline and not a shipping strategy."""

    # -- device / precision ----------------------------------------------------------------------
    device: str = "auto"       # "auto" | "cpu" | "cuda" | "cuda:N" | "N"
    dtype: str = "auto"        # "auto" | "float16" | "bfloat16" | "float32"

    # -- decoding (see the docstring: inherited from notes/transcribe.py) ------------------------
    language: str = "en"
    task: str = "transcribe"
    temperature: tuple[float, ...] = (0.0, 0.2, 0.4, 0.6, 0.8, 1.0)
    compression_ratio_threshold: float = 1.35
    logprob_threshold: float = -1.0
    no_speech_threshold: float = 0.6
    condition_on_prev_tokens: bool = False
    num_beams: int = 1
    """1 = greedy. Beam search costs latency the pseudo-streaming path can least afford; it is
    exposed because Phase 1 measures accuracy offline, where the cost does not apply."""

    # -- hallucination guard (plan.md §1.6) ------------------------------------------------------
    hallucination_guard: bool = True
    guard_tolerance_s: float = 0.0
    """Seconds of slack allowed past the end of the audio before a segment is dropped. 0.0
    reproduces `notes/transcribe.py` exactly (`start < duration` keeps). Raise it only with a
    measurement in hand."""

    silence_peak_floor: int = 0
    """Skip decoding a window whose peak |sample| is <= this. Default 0 means "skip only digital
    silence", which costs nothing and cannot suppress speech. Anything above 0 is a crude
    amplitude gate that WILL suppress quiet speech — the real gate is the server's Silero VAD
    (plan.md §3 Phase 2 item 3), and this is deliberately not a substitute for it."""

    # -- context / vocabulary biasing ------------------------------------------------------------
    context_mode: str = "prompt"
    """`"prompt"` = feed `start.context` to Whisper as `prompt_ids` (soft biasing, all Whisper has
    — `CANDIDATES.md` §2 row D). `"off"` = declare `context_supported:false` and ignore the field
    honestly. Never a third state where the field is accepted and dropped: plan.md §4 scores
    context-enabled and context-free runs in separate columns."""

    context_join: str = ", "
    context_max_chars: int = 200
    """Whisper's prompt window is small (~224 tokens shared with the transcript). An over-long
    prompt displaces the audio's own context, so the term list is truncated at this many
    characters and the truncation is logged."""

    # -- loading -----------------------------------------------------------------------------------
    allow_download: bool = False
    """False sets `local_files_only=True` on every component. plan.md §1.9: no model
    auto-download at run time. True exists only for a human doing a deliberate one-off fetch."""

    warmup_noise_peak: int = 400
    """Amplitude of the synthetic warm-up buffer (~1.2 % of full scale). Not silence — see
    `_warm_up()`."""

    extra_generate_kwargs: dict[str, Any] = field(default_factory=dict)
    """Passed straight into `generate_kwargs`. An escape hatch for Phase 1 experiments; anything
    used more than once should become a named field."""

    @classmethod
    def from_mapping(cls, options: Mapping[str, Any]) -> "WhisperTransformersConfig":
        """Build from a flat mapping, coercing the strings a command line delivers.

        Keys may use `-` or `_`. An **unknown key raises** rather than being ignored, on the same
        reasoning as the sherpa-onnx backend: a misspelled `--hop-ms` that silently kept the
        default would mislabel a plan.md §4 sweep cell, and the mislabelling would only ever be
        visible in the results file.
        """
        norm = {str(k).replace("-", "_").lstrip("_"): v for k, v in options.items()}
        known = set(cls.__dataclass_fields__)
        unknown = sorted(set(norm) - known)
        if unknown:
            raise ValueError(
                f"unknown option(s) for the transformers-whisper backend: {', '.join(unknown)}; "
                f"recognised: {', '.join(sorted(known))}"
            )

        kwargs: dict[str, Any] = {}
        for name in ("model", "device", "dtype", "language", "task", "context_mode",
                     "context_join"):
            if norm.get(name) is not None:
                kwargs[name] = str(norm[name])
        for name in ("window_ms", "hop_ms", "num_beams", "silence_peak_floor",
                     "context_max_chars", "warmup_noise_peak"):
            if norm.get(name) is not None:
                kwargs[name] = _as_int(norm[name], name)
        for name in ("compression_ratio_threshold", "logprob_threshold", "no_speech_threshold",
                     "guard_tolerance_s"):
            if norm.get(name) is not None:
                kwargs[name] = _as_float(norm[name], name)
        for name in ("condition_on_prev_tokens", "hallucination_guard", "allow_download"):
            if norm.get(name) is not None:
                kwargs[name] = _as_bool(norm[name], name)
        if norm.get("temperature") is not None:
            kwargs["temperature"] = _as_temperatures(norm["temperature"], "temperature")
        if norm.get("extra_generate_kwargs") is not None:
            extra = norm["extra_generate_kwargs"]
            if not isinstance(extra, Mapping):
                raise ValueError("extra_generate_kwargs: expected a mapping")
            kwargs["extra_generate_kwargs"] = dict(extra)

        cfg = cls(**kwargs)
        if cfg.context_mode not in ("prompt", "off"):
            raise ValueError(f"context_mode must be 'prompt' or 'off', got {cfg.context_mode!r}")
        return cfg


# ── The silence-hallucination guard ────────────────────────────────────────────────────────────


@dataclass(frozen=True)
class DroppedSegment:
    """One hypothesis segment the guard removed. Kept so a run record can show what was dropped."""

    text: str
    start: float | None
    end: float | None
    reason: str

    def __str__(self) -> str:  # for log lines
        return f"[{self.start}..{self.end}] {self.text!r} ({self.reason})"


def _apply_hallucination_guard(
    chunks: Sequence[Mapping[str, Any]],
    audio_s: float,
    tolerance_s: float,
) -> tuple[list[str], list[DroppedSegment]]:
    """Drop hypothesis segments whose timestamps lie beyond the audio that was decoded.

    plan.md §1.6, from `..\\wan-dancer\\notes\\transcribe.py`: Whisper emits "Thank you." / "Bye"
    over trailing silence *with timestamps past the end of the file*. The position is the tell, not
    the wording — so this filters on position only, and never on phrase.

    Segments with no usable start timestamp are dropped too (UNVERIFIED item B): a segment whose
    position is unknown cannot be checked against the audio length, and the conservative reading of
    "unknown position" for a model that is known to invent trailing text is that it is trailing
    text. Every drop is returned so the caller can log it.
    """
    kept: list[str] = []
    dropped: list[DroppedSegment] = []
    limit = audio_s + tolerance_s

    for chunk in chunks:
        text = str(chunk.get("text", ""))
        stamp = chunk.get("timestamp")
        start: float | None = None
        end: float | None = None
        if isinstance(stamp, (tuple, list)) and len(stamp) == 2:
            start = None if stamp[0] is None else float(stamp[0])
            end = None if stamp[1] is None else float(stamp[1])

        if not text.strip():
            continue  # empty segment: nothing to keep and nothing worth reporting
        if start is None:
            dropped.append(DroppedSegment(text, start, end, "no usable start timestamp"))
            continue
        if start >= limit:
            dropped.append(
                DroppedSegment(text, start, end, f"starts at/after end of audio ({audio_s:.2f}s)")
            )
            continue
        kept.append(text.strip())

    return kept, dropped


# ── The backend ────────────────────────────────────────────────────────────────────────────────


class WhisperTransformersBackend(PseudoStreamingBackend):
    """`openai/whisper-large-v3-turbo` (or any Whisper checkpoint) behind an overlapping window.

    Everything about the window — buffering, hop timing, sliding, prefix stitching, cumulative
    assembly — is `base.PseudoStreamingBackend`'s. This class supplies exactly three things: the
    capability declaration, the model load, and one offline `_transcribe_batch(pcm) -> str`.

    Threading: single-owner, not thread-safe. One decode loop drives one instance.
    """

    def __init__(
        self,
        config: WhisperTransformersConfig | Mapping[str, Any] | None = None,
        **overrides: Any,
    ) -> None:
        # Three call shapes, because the registry (`backends.create_backend`) passes whatever the
        # command line produced and the bench harness passes a dataclass: a config object, a flat
        # mapping, or keyword arguments. Mapping and keyword forms go through `from_mapping`, so an
        # unknown option is rejected there rather than silently ignored.
        if isinstance(config, WhisperTransformersConfig):
            if overrides:
                raise TypeError(
                    "pass either a WhisperTransformersConfig or option keywords, not both"
                )
            cfg = config
        else:
            merged: dict[str, Any] = dict(config or {})
            merged.update(overrides)
            cfg = WhisperTransformersConfig.from_mapping(merged)
        if cfg.context_mode not in ("prompt", "off"):
            # Checked here as well as in from_mapping(), because `capabilities.supports_context` is
            # derived from this string and a typo would otherwise become a silent
            # `context_supported:false` on the wire.
            raise ValueError(f"context_mode must be 'prompt' or 'off', got {cfg.context_mode!r}")
        super().__init__(window_ms=cfg.window_ms, hop_ms=cfg.hop_ms)
        self.config = cfg
        self._pipe: Any = None
        self._np: Any = None
        self._torch: Any = None
        self._prompt_ids: Any = None
        self._prompt_text: str = ""
        self._timestamps_available = True
        self._warned_slow_decode = False
        self._warned_no_timestamps = False
        # Counters and notes that belong in `bench/results/*.json`: a run whose transcripts were
        # filtered, or whose decode fell behind real time, is not directly comparable with one
        # that was not, and that fact has to survive to the record rather than living in a log.
        self.dropped_segment_count = 0
        self.decode_count = 0
        self.decode_ms_total = 0
        self.diagnostics: list[str] = []

    # -- construction from the command line -------------------------------------------------------

    @classmethod
    def from_backend_config(cls, cfg: Any) -> "WhisperTransformersBackend":
        """`kvit_asr.connection.open_backend`'s hook: build from a parsed `BackendConfig`.

        Without this, `open_backend` falls back to `cls(**--backend-opt)` and every *typed* field
        of the command line — `--model`, `--chunk-ms`, `--device` — is dropped on the floor. That
        failure is silent and expensive: models.json's `whisper-large-v3-turbo` entry passes
        `--model openai/whisper-large-v3-turbo`, and a backend that never saw it would load
        `DEFAULT_MODEL` instead and file the result under the wrong row of plan.md §4.

        The mapping is written out rather than derived, because it is genuinely per-backend:

        * `--chunk-ms` is this backend's **hop**, not a model property. It is a batch model in a
          window we built (`PseudoStreamingBackend`), so the hop is a free parameter — unlike the
          sherpa-onnx rows, where each chunk size is a separate set of weights.
        * `--model-dir` has no meaning here and is **not** mapped. `transformers` resolves a
          checkpoint through `HF_HOME` with `local_files_only` (see `allow_download`), so there is
          no directory-of-unpacked-files concept to point at. Passing one is an error rather than
          a silent no-op — a run that thought it was reading local weights and was not is exactly
          the kind of unattributable measurement plan.md §4 cannot afford.
        * `--decoding` names a transducer decoding method (greedy_search / modified_beam_search).
          Whisper is an attention encoder-decoder and has neither; `num_beams` is the nearest
          thing and is a `--backend-opt`. Rejected loudly for the same reason.

        `--backend-opt` is applied last, so it overrides anything derived here.
        """
        options: dict[str, Any] = {}
        if getattr(cfg, "model", None):
            options["model"] = cfg.model
        if getattr(cfg, "chunk_ms", None) is not None:
            options["hop_ms"] = cfg.chunk_ms

        device = str(getattr(cfg, "device", "auto") or "auto")
        if device == "dml":
            raise ValueError(
                "--device dml is not available for the transformers-whisper backend: it runs on "
                "torch, which has no DirectML device on this machine. Use cpu or cuda."
            )
        if device != "auto":
            # "auto" is left unmapped so the config's own "auto" resolution runs and *reports*
            # what it chose, which is what the --device help text promises.
            options["device"] = device

        if getattr(cfg, "model_dir", None) is not None:
            raise ValueError(
                "--model-dir is not meaningful for the transformers-whisper backend: a checkpoint "
                "is resolved through HF_HOME, not from a directory of unpacked files. Point "
                "HF_HOME at the cache (models.json does this for the whisper-large-v3-turbo "
                "entry) and pass the checkpoint id as --model."
            )
        if getattr(cfg, "decoding", None):
            raise ValueError(
                f"--decoding {cfg.decoding!r} is not meaningful for the transformers-whisper "
                "backend: it is an attention encoder-decoder, not a transducer, so it has no "
                "greedy_search/modified_beam_search choice. The nearest knob is "
                "`--backend-opt num_beams=N`."
            )

        options.update(getattr(cfg, "options", None) or {})
        return cls(WhisperTransformersConfig.from_mapping(options))

    # -- declaration ----------------------------------------------------------------------------

    @property
    def capabilities(self) -> BackendCapabilities:
        cfg = self.config
        return BackendCapabilities(
            model_id=cfg.model,
            mode=StreamingMode.PSEUDO_STREAMING,
            # Soft biasing through Whisper's prompt, which is all Whisper has (CANDIDATES.md §2
            # row D: "prompt/initial_prompt only — soft"). Declared from configuration so that a
            # run started with context_mode="off" reports false rather than claiming a capability
            # it was told not to use.
            supports_context=(cfg.context_mode == "prompt"),
            natural_chunk_ms=cfg.hop_ms,
            chunk_ms_configurable=True,
            supported_chunk_ms=(),
            # A gap inside the window means the wrapper is holding audio that never happened
            # consecutively, and Whisper will happily transcribe the splice. There is no decoder
            # state to protect, but there is a buffer to throw away.
            requires_contiguous_audio=True,
            notes=(
                f"transformers pipeline, window={cfg.window_ms}ms hop={cfg.hop_ms}ms, "
                f"device={cfg.device}, dtype={cfg.dtype}, "
                f"guard={'on' if cfg.hallucination_guard else 'off'}"
            ),
        )

    # -- load -----------------------------------------------------------------------------------

    def _load(self, progress: ProgressFn | None) -> None:
        """Import torch/transformers/numpy and build the ASR pipeline.

        Every import in this method is a *backend* dependency. They are here, not at module scope,
        so that an interpreter without torch disables this one backend instead of taking the
        server down (see `kvit_asr.__init__`).
        """
        cfg = self.config

        self._report(progress, "import numpy", 2)
        try:
            import numpy as np  # noqa: PLC0415 - backend-local import, deliberately lazy
        except ImportError as exc:
            raise WhisperUnavailable(_missing_runtime_message("numpy", exc)) from exc
        self._np = np

        self._report(progress, "import torch", 5)
        try:
            import torch  # noqa: PLC0415
        except ImportError as exc:
            raise WhisperUnavailable(_missing_runtime_message("torch", exc)) from exc
        self._torch = torch

        self._report(progress, "import transformers", 15)
        try:
            from transformers import pipeline  # noqa: PLC0415
        except ImportError as exc:
            raise WhisperUnavailable(_missing_runtime_message("transformers", exc)) from exc

        device = self._resolve_device(torch)
        dtype = self._resolve_dtype(torch, device)
        self._log_environment(torch, device, dtype)

        self._report(progress, "weights", 25)
        t0 = time.perf_counter()
        try:
            self._pipe = pipeline(
                "automatic-speech-recognition",
                model=cfg.model,
                dtype=dtype,
                device=device,
                # VERIFIED item 2: threaded into hub_kwargs for model, tokenizer and feature
                # extractor alike. plan.md §1.9 — the sidecar performs no network access.
                local_files_only=not cfg.allow_download,
            )
        except Exception as exc:  # noqa: BLE001 - re-raised with the context that makes it legible
            raise self._load_failure(exc) from exc
        log.info(
            "loaded %s in %.1fs (device=%r dtype=%s)",
            cfg.model, time.perf_counter() - t0, device, dtype,
        )

        self._report(progress, "check pipeline", 90)
        self._check_pipeline_shape()
        if self._context:
            # Context can arrive before load in principle (set_context is not lifecycle-ordered);
            # rebuild the prompt now that a tokenizer exists.
            self.set_context(self._context)

    def _resolve_device(self, torch: Any) -> int | str:
        """Map the configured device onto what `pipeline(device=…)` takes.

        Ints are used because that is the form `notes/transcribe.py` uses and it is therefore the
        proven path on this machine: `0` = first CUDA device, `-1` = CPU.
        """
        spec = self.config.device.strip().lower()
        if spec in ("auto", ""):
            if torch.cuda.is_available():
                return 0
            log.warning(
                "torch reports no CUDA device; %s will run on CPU. large-v3-turbo on CPU is far "
                "slower than real time, so any latency figure from this run is meaningless — "
                "record it as CPU or do not record it at all.",
                self.config.model,
            )
            self.diagnostics.append("ran on CPU: torch.cuda.is_available() was False")
            return -1
        if spec == "cpu":
            return -1
        if spec == "cuda":
            return 0
        if spec.startswith("cuda:"):
            return _as_int(spec.split(":", 1)[1], "device")
        return _as_int(spec, "device")

    def _resolve_dtype(self, torch: Any, device: int | str) -> Any:
        """`"auto"` reproduces `notes/transcribe.py`: float16 on GPU, float32 on CPU."""
        spec = self.config.dtype.strip().lower()
        on_gpu = not (isinstance(device, int) and device < 0)
        if spec in ("auto", ""):
            return torch.float16 if on_gpu else torch.float32
        named = {
            "float16": torch.float16, "fp16": torch.float16, "half": torch.float16,
            "bfloat16": torch.bfloat16, "bf16": torch.bfloat16,
            "float32": torch.float32, "fp32": torch.float32, "float": torch.float32,
        }
        if spec not in named:
            raise ValueError(
                f"dtype must be one of auto|{'|'.join(sorted(set(named)))}, got {spec!r}"
            )
        if named[spec] is torch.float16 and not on_gpu:
            # float16 on CPU is either unsupported or pathologically slow depending on the build.
            # Refusing beats producing a number nobody can interpret.
            raise ValueError("dtype=float16 with device=cpu: use float32 on CPU")
        return named[spec]

    def _log_environment(self, torch: Any, device: Any, dtype: Any) -> None:
        """Log what is actually going to run. Cheap, and the first thing wanted after a bad run."""
        import os  # noqa: PLC0415 - trivial, kept next to its only use

        try:
            import transformers  # noqa: PLC0415
            tf_version = transformers.__version__
        except Exception:  # noqa: BLE001 - diagnostics must never be the reason a load fails
            tf_version = "unknown"

        cuda_note = "cpu"
        if torch.cuda.is_available():
            try:
                cap = torch.cuda.get_device_capability(0)
                cuda_note = (
                    f"{torch.cuda.get_device_name(0)} sm_{cap[0]}{cap[1]} "
                    f"cuda={torch.version.cuda}"
                )
            except Exception as exc:  # noqa: BLE001
                cuda_note = f"cuda available, details unavailable: {exc}"
        log.info(
            "interpreter=%s python=%s torch=%s transformers=%s numpy=%s | %s | device=%r dtype=%s "
            "| HF_HOME=%s local_files_only=%s",
            sys.executable, sys.version.split()[0], torch.__version__, tf_version,
            getattr(self._np, "__version__", "unknown"), cuda_note, device, dtype,
            os.environ.get("HF_HOME", "<unset>"), not self.config.allow_download,
        )

    def _check_pipeline_shape(self) -> None:
        """Confirm the pipeline exposes what this adapter drives, before `ready` is sent."""
        for attr in ("tokenizer", "feature_extractor", "model"):
            if getattr(self._pipe, attr, None) is None:
                raise WhisperApiMismatch(
                    f"the ASR pipeline for {self.config.model!r} has no `{attr}`; this adapter "
                    "was written against transformers' AutomaticSpeechRecognitionPipeline "
                    f"(installed: {_transformers_version()})"
                )
        fe_rate = getattr(self._pipe.feature_extractor, "sampling_rate", None)
        if fe_rate is not None and int(fe_rate) != SAMPLE_RATE:
            # The pipeline would resample for us, but silently: proto 1 fixes the wire at 16 kHz
            # (docs/protocol.md §6.3) and a model wanting anything else is not a Whisper checkpoint
            # this adapter understands.
            raise WhisperApiMismatch(
                f"{self.config.model!r} expects {fe_rate} Hz audio; proto 1 audio is "
                f"{SAMPLE_RATE} Hz mono s16le and this adapter does not resample"
            )
        if self.config.context_mode == "prompt" and not hasattr(
            self._pipe.tokenizer, "get_prompt_ids"
        ):
            # We declared supports_context=True in `capabilities`; if the tokenizer cannot build a
            # prompt, that declaration is a lie and plan.md §4's context-enabled column would be
            # filled with a context-free run. Fail instead of degrading.
            raise WhisperApiMismatch(
                f"context_mode='prompt' but {type(self._pipe.tokenizer).__name__} has no "
                "get_prompt_ids(); this backend declared context_supported=true and cannot honour "
                f"it (transformers {_transformers_version()}). Start it with context_mode=off to "
                "declare context-free honestly."
            )

    def _load_failure(self, exc: Exception) -> BackendLoadError:
        """Turn a pipeline construction failure into something a human can act on."""
        text = f"{type(exc).__name__}: {exc}"
        looks_offline = any(
            token in text.lower()
            for token in ("local_files_only", "offline", "not found", "couldn't connect",
                          "no such file", "cannot find")
        )
        if looks_offline and not self.config.allow_download:
            return WhisperWeightsMissing(
                f"{self.config.model!r} is not in the local HuggingFace cache "
                f"(HF_HOME as seen by this process), and downloads are disabled.\n"
                "plan.md §1.9 makes model fetching an explicit, separate, user-initiated action, "
                "so the sidecar will not fetch it. A human runs, once:\n\n"
                f"    hf download {self.config.model}\n"
                "    (older Hugging Face CLIs spell this `huggingface-cli download`)\n\n"
                f"with HF_HOME set as models.json sets it. Underlying error: {text}"
            )
        return BackendLoadError(
            f"failed to build the ASR pipeline for {self.config.model!r} "
            f"(transformers {_transformers_version()}, interpreter {sys.executable}): {text}"
        )

    def _unload(self) -> None:
        self._pipe = None
        self._prompt_ids = None
        torch = self._torch
        self._torch = None
        self._np = None
        if torch is not None:
            try:
                if torch.cuda.is_available():
                    # The weights are freed when the pipeline is dropped; this returns the caching
                    # allocator's blocks so the next child process can have the VRAM. Best effort:
                    # a failure here must not turn shutdown into an error.
                    torch.cuda.empty_cache()
            except Exception as exc:  # noqa: BLE001
                log.debug("torch.cuda.empty_cache() failed during unload: %s", exc)

    # -- warm-up ---------------------------------------------------------------------------------

    def _warm_up(self, audio_ms: int) -> None:
        """Warm on low-level noise rather than the base class's digital silence.

        Two reasons, both specific to this backend. `silence_peak_floor` short-circuits an all-zero
        window without decoding (that is the point of it), so warming on silence would measure the
        cost of *skipping* a decode and report it as `warm_ms` — a number that looks like a
        measurement and is not. And Whisper on pure silence is precisely the hallucination case
        plan.md §1.6 describes, so it is the least representative input available for timing the
        real decode path.

        Deterministic (`random.Random(0)`), so two runs of the bake-off warm on identical audio.
        """
        self.reset(ResetReason.WARMUP)
        peak = max(1, int(self.config.warmup_noise_peak))
        rng = random.Random(0)
        n_samples = ms_to_bytes(max(audio_ms, self.hop_ms)) // 2
        noise = bytearray()
        for _ in range(n_samples):
            noise += int(rng.randint(-peak, peak)).to_bytes(2, "little", signed=True)
        self.feed(bytes(noise))
        self.finalize()

    # -- context ---------------------------------------------------------------------------------

    def set_context(self, terms: Sequence[str]) -> bool:
        """Turn `start.context` into Whisper `prompt_ids`. UNVERIFIED item A.

        Whisper's biasing is a soft prompt: the terms are prepended as previous-text context, which
        makes the decoder *more likely* to spell `llama.cpp` that way and guarantees nothing. That
        is the honest description of what row D can do (`CANDIDATES.md` §2), and it is why the
        context-biasing arm of the bake-off is a transducer with real hotwords, not this.
        """
        applied = super().set_context(terms)  # stores self._context, returns supports_context
        self._prompt_ids = None
        self._prompt_text = ""
        if not applied or not self._context:
            if self._context and not applied:
                log.warning(
                    "%d context item(s) supplied but context_mode='off'; they are ignored and "
                    "context_supported is reported false. Treat this run as context-free "
                    "(plan.md §4).",
                    len(self._context),
                )
            return applied
        if self._pipe is None:
            # Called before load(); _load() rebuilds the prompt once a tokenizer exists.
            return applied

        text = self.config.context_join.join(t.strip() for t in self._context if t.strip())
        if len(text) > self.config.context_max_chars:
            cut = text[: self.config.context_max_chars].rsplit(self.config.context_join, 1)[0]
            log.warning(
                "context prompt truncated from %d to %d characters; %d of %d term(s) kept. "
                "Whisper's prompt window is shared with the transcript, so a long prompt "
                "displaces the audio's own context.",
                len(text), len(cut), cut.count(self.config.context_join) + 1, len(self._context),
            )
            text = cut
        if not text:
            return applied

        try:
            ids = self._pipe.tokenizer.get_prompt_ids(text, return_tensors="pt")
        except Exception as exc:  # noqa: BLE001
            raise WhisperApiMismatch(
                f"tokenizer.get_prompt_ids({text!r}) failed "
                f"(transformers {_transformers_version()}): {type(exc).__name__}: {exc}. This "
                "backend declared context_supported=true; it will not silently continue "
                "context-free, because plan.md §4 scores the two separately."
            ) from exc
        to = getattr(ids, "to", None)
        if to is not None and getattr(self._pipe, "model", None) is not None:
            try:
                ids = to(self._pipe.model.device)
            except Exception as exc:  # noqa: BLE001
                log.debug("could not move prompt_ids to the model device: %s", exc)
        self._prompt_ids = ids
        self._prompt_text = text
        log.info("context prompt (%d chars): %r", len(text), text)
        return applied

    # -- decode ------------------------------------------------------------------------------------

    def _transcribe_batch(self, pcm: bytes) -> str:
        """Transcribe one whole window offline. Called once per hop by `PseudoStreamingBackend`."""
        if self._pipe is None:
            raise BackendDecodeError("_transcribe_batch called before load()")
        if not pcm:
            return ""

        audio_s = len(pcm) / (SAMPLE_RATE * 2)
        if pcm_peak(pcm) <= self.config.silence_peak_floor:
            # Digital silence by default. Feeding it to Whisper is the documented way to get
            # "Thank you." out of a model (plan.md §1.6), and it costs a full decode to do so.
            log.debug("window of %.2fs is below the peak floor; not decoded", audio_s)
            return ""

        samples = self._np.frombuffer(pcm, dtype="<i2").astype(self._np.float32) / 32768.0

        generate_kwargs: dict[str, Any] = {
            "language": self.config.language,
            "task": self.config.task,
            "condition_on_prev_tokens": self.config.condition_on_prev_tokens,
            "temperature": tuple(self.config.temperature),
            "compression_ratio_threshold": self.config.compression_ratio_threshold,
            "logprob_threshold": self.config.logprob_threshold,
            "no_speech_threshold": self.config.no_speech_threshold,
        }
        if self.config.num_beams > 1:
            generate_kwargs["num_beams"] = self.config.num_beams
        if self._prompt_ids is not None:
            generate_kwargs["prompt_ids"] = self._prompt_ids
        generate_kwargs.update(self.config.extra_generate_kwargs)

        t0 = time.perf_counter()
        try:
            result = self._pipe(
                # Explicit dict form (VERIFIED item 3): it states the sample rate rather than
                # letting the feature extractor assume its own, so a future 8 kHz checkpoint would
                # resample instead of silently transcribing chipmunks.
                {"raw": samples, "sampling_rate": SAMPLE_RATE},
                return_timestamps=self.config.hallucination_guard,
                generate_kwargs=generate_kwargs,
            )
        except Exception as exc:  # noqa: BLE001
            # Non-fatal by design: `decode_failed` aborts the session and keeps the connection
            # (docs/protocol.md §11.3), so one bad window does not cost the loaded model.
            raise BackendDecodeError(
                f"whisper decode failed on a {audio_s:.2f}s window "
                f"(model={self.config.model!r}, generate_kwargs={sorted(generate_kwargs)}): "
                f"{type(exc).__name__}: {exc}"
            ) from exc
        decode_ms = int(round((time.perf_counter() - t0) * 1000))
        self.decode_count += 1
        self.decode_ms_total += decode_ms
        self._note_decode_cost(decode_ms, audio_s)

        return self._extract_text(result, audio_s)

    def _extract_text(self, result: Any, audio_s: float) -> str:
        """Pull text out of the pipeline result and run the hallucination guard over it."""
        if isinstance(result, list):
            # The 5.x signature is annotated `-> list[dict]` while a single input returns a bare
            # dict in practice. Both are accepted rather than betting on one.
            if not result:
                return ""
            result = result[0]
        if not isinstance(result, Mapping):
            raise WhisperApiMismatch(
                f"the ASR pipeline returned {type(result).__name__}; this adapter expects a dict "
                f"with a 'text' key (transformers {_transformers_version()})"
            )
        full_text = str(result.get("text", "")).strip()

        if not self.config.hallucination_guard:
            return full_text

        chunks = result.get("chunks")
        if not isinstance(chunks, Sequence) or isinstance(chunks, (str, bytes)):
            # No per-segment timestamps: the guard cannot run. Say so loudly, once, because the
            # measured hallucination-on-silence numbers (plan.md §4) mean something different for a
            # run in which the guard was inert.
            if not self._warned_no_timestamps:
                self._warned_no_timestamps = True
                self._timestamps_available = False
                note = (
                    "the pipeline returned no 'chunks', so the past-the-end hallucination guard "
                    "is inert for this run"
                )
                self.diagnostics.append(note)
                log.warning("%s (result keys: %s)", note, sorted(result))
            return full_text

        kept, dropped = _apply_hallucination_guard(chunks, audio_s, self.config.guard_tolerance_s)
        if dropped:
            self.dropped_segment_count += len(dropped)
            # plan.md §1.6 asks for this to be logged when it happens: it is evidence about the
            # model, and silently better output would hide a real property of the candidate.
            log.warning(
                "hallucination guard dropped %d segment(s) beyond %.2fs of audio: %s",
                len(dropped), audio_s, "; ".join(str(d) for d in dropped),
            )
        text = " ".join(kept).strip()

        if self._prompt_text and text.lower().startswith(self._prompt_text.lower()):
            # UNVERIFIED item A. Whisper's generate normally strips the prompt from the output; if
            # it does not, the transcript would open with the vocabulary list. Reported, not
            # repaired — a silent strip would hide it from the Phase 1 measurement, and it would
            # also delete text if the user genuinely dictated the same words.
            log.error(
                "the transcript begins with the context prompt %r — prompt_ids may be leaking "
                "into the output. Nothing is stripped; investigate before trusting this run.",
                self._prompt_text,
            )
        return text

    def _note_decode_cost(self, decode_ms: int, audio_s: float) -> None:
        """Warn once when a window decode costs more than the hop it must keep up with."""
        if decode_ms <= self.hop_ms or self._warned_slow_decode:
            return
        self._warned_slow_decode = True
        note = (
            f"a {audio_s:.2f}s window took {decode_ms}ms to decode, longer than the {self.hop_ms}ms "
            "hop: this backend is behind real time and its latency figures are queueing delay, "
            "not model latency"
        )
        self.diagnostics.append(note)
        log.warning("%s", note)

    # -- reporting ---------------------------------------------------------------------------------

    def describe(self) -> dict:
        """Base fields plus what a result file needs to interpret this run."""
        d = super().describe()
        d.update(
            window_ms=self.window_ms,
            hop_ms=self.hop_ms,
            device=self.config.device,
            dtype=self.config.dtype,
            language=self.config.language,
            hallucination_guard=self.config.hallucination_guard,
            timestamps_available=self._timestamps_available,
            dropped_segments=self.dropped_segment_count,
            decodes=self.decode_count,
            mean_decode_ms=(
                round(self.decode_ms_total / self.decode_count) if self.decode_count else None
            ),
            context_prompt=self._prompt_text or None,
            diagnostics=list(self.diagnostics),
        )
        return d


# ── Helpers ────────────────────────────────────────────────────────────────────────────────────


def _transformers_version() -> str:
    try:
        import transformers  # noqa: PLC0415
    except ImportError:
        return "not installed"
    return str(getattr(transformers, "__version__", "unknown"))


def _missing_runtime_message(package: str, exc: ImportError) -> str:
    """The message a human sees when this backend is launched under the wrong interpreter."""
    return (
        f"`import {package}` failed in this interpreter ({sys.executable}), so the "
        "transformers-whisper baseline is unavailable: "
        f"{type(exc).__name__}: {exc}\n"
        "This backend is meant to run under the interpreter models.json names for it:\n"
        f"    {EXPECTED_INTERPRETER}\n"
        "which already has torch 2.10.0+cu130 (sm_120), transformers 5.14.1 and numpy 2.4.4 "
        "(plan.md §5.1).\n"
        "Nothing is to be installed — not into that venv, which belongs to another project and is "
        "read-only for kvit-dictate (plan.md §5.1, bench/CANDIDATES.md §3), and not by the sidecar "
        "at run time (plan.md §1.9). If that venv is gone, the response is a NEW locked "
        "environment under envs/, created by a human, never a package added to that one."
    )


def create(options: Mapping[str, Any] | None = None, **kwargs: Any) -> WhisperTransformersBackend:
    """Factory a caller can use without importing the config dataclass.

    `backends.REGISTRY` resolves to the *class*, not to this function, so this exists for the bench
    harness and for symmetry with the other backend modules. Registry entry, for whoever owns
    `backends/__init__.py`:

        "transformers-whisper": ".whisper_transformers:WhisperTransformersBackend"

    matching the `--backend transformers-whisper` spelling in models.json.
    """
    return WhisperTransformersBackend(options, **kwargs)
