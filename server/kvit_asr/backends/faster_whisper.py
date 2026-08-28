# Copyright 2026 Kvit
# Licensed under the Apache License, Version 2.0. See LICENSE-APACHE, or
# https://www.apache.org/licenses/LICENSE-2.0
"""Whisper through CTranslate2 (`faster-whisper`) — rows E and E′ of the candidate table.

`bench/CANDIDATES.md` §2: `Systran/faster-whisper-large-v3` (row E, a CT2 conversion of
`openai/whisper-large-v3`) and `Systran/faster-distil-whisper-large-v3` (row E′, the distilled
sibling). §4 places both in the **second tier**: batch accuracy references and the cheapest
CPU-side fallback, deferred until Phase 1 picks 1–4 have run. Nothing here is a streaming
candidate.

This backend is the CTranslate2 half of a deliberate pair. `whisper_transformers.py` runs the
*same family of weights* under torch in another project's read-only venv; this one runs them under
a runtime with **no PyTorch at all** (`envs/ct2` — `faster-whisper` pulls `ctranslate2`, `av`,
`tokenizers` and `onnxruntime`, per `CANDIDATES.md` §3). Keeping the decoding parameters of the two
adapters identical is intentional: it means a difference in the Phase 1 numbers is attributable to
the runtime and the quantisation, not to two people's taste in thresholds.

──────────────────────────────────────────────────────────────────────────────────────────────
Pseudo-streaming, said plainly
──────────────────────────────────────────────────────────────────────────────────────────────

CTranslate2 does not make Whisper causal. It is the same batch encoder-decoder over 30-second
windows, executed faster. So this backend is `PseudoStreamingBackend` from `base.py` — an
overlapping window that *we* built, re-transcribed from scratch every hop — and `ready.mode` is
`"pseudo-streaming"`. `CANDIDATES.md` §4 requires this mode's latency numbers to be reported in a
separate block from the native-streaming rows; labelling it anything else would corrupt the only
comparison Phases 1 and 2 exist to make. "faster" in the package name is a throughput claim about
batch decoding, not a streaming claim, and the two are routinely confused.

──────────────────────────────────────────────────────────────────────────────────────────────
`compute_type`, and the Blackwell report this project has NOT verified
──────────────────────────────────────────────────────────────────────────────────────────────

`bench/CANDIDATES.md` §6 item 2 records, as an open question rather than a fact:

    "**CTranslate2 on Blackwell.** Multiple third-party reports of `CUBLAS_STATUS_NOT_SUPPORTED`
    on RTX 50-series with int8 compute, worked around by forcing `compute_type="float16"`.
    Not verified here."

Nothing in this module upgrades that to knowledge. What it does instead:

* `compute_type` is **configuration**, never a hard-coded constant.
* `compute_type="auto"` resolves to `float16` on CUDA and `int8` on CPU. On CUDA that is the
  reported workaround, chosen because it is the option with a chance of working on the machine
  this project actually targets; on CPU it is CTranslate2's ordinary recommendation and the
  Blackwell report says nothing about CPU kernels.
* Selecting an int8 CUDA compute type is allowed and logs a warning that names the report and its
  status. It is not blocked: a run that reproduces or refutes the report is exactly the evidence
  §6 is waiting for, and a guard that refused would make that run impossible.
* A load or decode failure whose text mentions cuBLAS, cuDNN or a missing DLL is re-raised with
  the compute-type advice and the CUDA 12 / cuDNN 9 requirement attached (`CANDIDATES.md` §3
  records that this env needs those DLLs and no torch to supply them), rather than escaping as an
  opaque runtime error.

──────────────────────────────────────────────────────────────────────────────────────────────
No auto-download, and where the weights come from
──────────────────────────────────────────────────────────────────────────────────────────────

plan.md §1.9: no model auto-download at run time; fetching is an explicit, separate,
user-initiated action. `local_files_only=True` is therefore passed on every load unless a human
sets `allow_download=true` for a deliberate one-off, and if the installed `WhisperModel` does not
accept that keyword this module **fails** rather than dropping it — silently dropping it would
turn a cache miss into a multi-gigabyte download in the middle of a dictation session.

Weights resolve in this order: an explicit `model_dir` (a directory containing `model.bin`,
`config.json`, `tokenizer.json`, `vocabulary.*`); else `models_root`/`KVIT_ASR_MODELS_DIR` joined
with the model's last path component; else the model id handed to `faster_whisper`, which resolves
it out of the shared `HF_HOME` (`c:\\apps\\llama-swap\\gguf\\hf`, plan.md §5.2). Already cached on
this machine per plan.md §5.2: `Systran/faster-whisper-base.en` (141 MB) — the smoke-test model,
not a candidate. Rows E and E′ are not cached and a human downloads them once; the error message
carries the command.

──────────────────────────────────────────────────────────────────────────────────────────────
The silence-hallucination guard (plan.md §1.6)
──────────────────────────────────────────────────────────────────────────────────────────────

Whisper emits "Thank you." / "Bye" over trailing silence *with timestamps past the end of the
audio* — the observation plan.md §1.6 takes from `..\\wan-dancer\\notes\\transcribe.py`. That is a
property of the weights, not of the runtime, so CTranslate2 inherits it whole.

The guard is therefore **imported from `whisper_transformers.py` rather than reimplemented**. Two
copies of a filter this specific would drift, and the drift would be invisible: it would show up
only as rows D and E ceasing to be comparable on the hallucination-rate metric that plan.md §4
gates on. The imported helper is private by name (`_apply_hallucination_guard`); this is the same
cross-module borrowing `zipformer_hotwords.py` does from `sherpa_onnx_streaming.py`, and if a third
Whisper-family backend ever appears the function should move to `base.py`.

It filters on *position*, never on phrase. A blocklist of known-hallucinated phrases was rejected
in the sibling module for a reason that holds here: "thank you" is an ordinary dictated sentence,
and silently deleting text the user said is the failure mode this project is least willing to
accept.

One CTranslate2-specific interaction: `vad_filter=True` makes faster-whisper drop silence *before*
decoding and report timestamps against the shortened audio, which changes what "past the end"
means. `vad_filter` therefore defaults to **off** — the real endpointing is the server's Silero VAD
(plan.md §3 Phase 2 item 3, `kvit_asr/vad.py`), and two VADs in series would gate the audio twice
and make the measured endpoint latency uninterpretable.

──────────────────────────────────────────────────────────────────────────────────────────────
VERIFIED / UNVERIFIED — and here, almost everything is UNVERIFIED
──────────────────────────────────────────────────────────────────────────────────────────────

`whisper_transformers.py` could check its assumptions by reading `transformers 5.14.1` in the venv
that already has it. **This module has no such luxury.** plan.md §5.1(a) records that
`faster-whisper` is absent from every environment on this machine, `envs/ct2` does not exist, and
creating it is a human decision (`server/README.md` has the command). So nothing below was read out
of an installed package and nothing was executed. Everything is written against the documented
public API of `faster-whisper` and must be checked on first real run:

 A. `WhisperModel(model_size_or_path, device=…, device_index=…, compute_type=…, cpu_threads=…,
    num_workers=…, download_root=…, local_files_only=…)`. The constructor is inspected at load and
    an unexpected keyword becomes a specific `FasterWhisperApiMismatch` listing what it *does*
    accept — not a `TypeError` from inside a library.
 B. `model.transcribe(audio, …) -> (segments, info)` where `segments` is a **generator** and
    `info` carries `.duration` and `.language`. The laziness matters: decoding happens while the
    generator is consumed, so the consumption is inside the try/except that turns a decode failure
    into `decode_failed`, and the timing measurement brackets the consumption rather than the call.
 C. Segment attributes `.start`, `.end`, `.text`. Adapted into the `{"text", "timestamp"}` shape
    the shared guard expects; a segment missing them is an API mismatch, not a silent skip.
 D. `hotwords=` on `transcribe()`. Documented for recent faster-whisper, and reported to be honoured
    only when `initial_prompt` is absent. This module refuses to set both rather than passing both
    and letting an unverified precedence rule decide which one biased the run — plan.md §4 scores
    context-enabled runs in their own column and needs to know which mechanism produced them.
 E. `ctranslate2.get_supported_compute_types(device)`. Used, if present, to reject an unsupported
    `compute_type` at load with the supported list in the message instead of at first decode. Its
    absence is tolerated (best-effort diagnostic), its disagreement is not.
 F. Whether float16 CUDA decoding actually works on sm_120 at all. See the Blackwell section.
 G. Wall-clock cost per window. The re-decode-every-hop design is affordable only if a window
    decode is well under the hop; a decode that exceeds it is warned about once, because at that
    point the reported latency is queueing delay rather than model latency.
"""

from __future__ import annotations

import inspect
import logging
import os
import random
import sys
import time
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path
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

# Borrowed from the sibling Whisper backend. Both names are stdlib-only at import time (that module
# keeps every heavy import inside `_load()`), so this costs nothing on a bare interpreter and
# cannot make a missing torch break this backend. See the docstring for why the guard is shared
# rather than copied.
from .whisper_transformers import (  # noqa: PLC2701 - deliberate cross-backend reuse, see docstring
    DroppedSegment,
    _apply_hallucination_guard,
    _as_bool,
    _as_float,
    _as_int,
    _as_temperatures,
)

__all__ = [
    "DEFAULT_MODEL",
    "KNOWN_MODELS",
    "FasterWhisperConfig",
    "FasterWhisperBackend",
    "FasterWhisperUnavailable",
    "FasterWhisperApiMismatch",
    "FasterWhisperWeightsMissing",
    "create",
]

log = logging.getLogger(__name__)

# Row E of bench/CANDIDATES.md §2. Not the cached one — see KNOWN_MODELS — because the default
# should name the candidate this backend exists to measure, and a default that quietly pointed at
# the 141 MB smoke-test model would put `base.en` numbers in a `large-v3` column.
DEFAULT_MODEL = "Systran/faster-whisper-large-v3"

# Model id -> (what it is, approximate on-disk size). Used only for error messages and for the
# download hint; nothing here is a whitelist, and any CT2 Whisper conversion or local directory is
# accepted. Sizes are from the model cards / plan.md §5.2 and are approximate.
KNOWN_MODELS: dict[str, tuple[str, str]] = {
    "Systran/faster-whisper-large-v3": (
        "CANDIDATES.md row E — the batch accuracy reference", "~3.1 GB",
    ),
    "Systran/faster-distil-whisper-large-v3": (
        "CANDIDATES.md row E′ — distilled, roughly half the size", "~1.5 GB",
    ),
    "Systran/faster-whisper-base.en": (
        "already cached (plan.md §5.2) — smoke test only, not a candidate", "141 MB",
    ),
}

# The environment this backend expects, quoted in error messages so a human reading a log does not
# have to open models.json or CANDIDATES.md to find out which python.exe was meant.
ENV_DIR = r"D:\projects\kvit-dictate\envs\ct2"
EXPECTED_INTERPRETER = ENV_DIR + r"\Scripts\python.exe"
REQUIREMENTS_FILE = r"server\requirements\ct2.txt"


class FasterWhisperUnavailable(BackendLoadError):
    """`faster_whisper` / `ctranslate2` / `numpy` are not importable here. This backend is disabled.

    A `BackendLoadError`, so the server maps it to `model_load_failed` (docs/protocol.md §11.3) and
    surfaces `msg` verbatim. It can never reach package-import time: `kvit_asr` must stay importable
    on a bare stdlib CPython, which is why every import that can raise this lives in `_load()`.
    """


class FasterWhisperApiMismatch(BackendLoadError):
    """faster-whisper is installed but is not shaped the way this adapter was written against.

    Every UNVERIFIED item in the module docstring funnels into this exception, deliberately, with
    the installed versions and what was actually found — rather than a `TypeError` escaping from
    inside a library call that a reader would reasonably blame on the model.
    """


class FasterWhisperWeightsMissing(BackendLoadError):
    """The CT2 weights are not on this disk and downloads are disabled.

    plan.md §1.9: model fetching is an explicit, separate, user-initiated action. The message
    carries the command a human runs.
    """


# ── Configuration ──────────────────────────────────────────────────────────────────────────────
#
# The coercion helpers (`_as_int` and friends) are imported from whisper_transformers rather than
# duplicated: they exist because `--backend-opt KEY=VALUE` delivers everything as a string
# (kvit_asr/config.py's BackendConfig.options), and two copies of "how do we parse a bool" would be
# two places for the answer to differ.


@dataclass
class FasterWhisperConfig:
    """Everything this backend needs, with no reference to argparse or models.json."""

    model: str = DEFAULT_MODEL

    model_dir: str | None = None
    """Explicit directory holding the CT2 conversion (`model.bin`, `config.json`, `tokenizer.json`,
    `vocabulary.*`). Takes precedence over everything else and is passed to faster-whisper
    verbatim, so no hub lookup happens at all."""

    models_root: str | None = None
    """Directory of unpacked models; the model's last path component is appended. Falls back to
    `$KVIT_ASR_MODELS_DIR`, matching the sherpa-onnx backend. Empty means "let faster-whisper
    resolve the id from HF_HOME"."""

    # -- window driver (base.PseudoStreamingBackend) ---------------------------------------------
    window_ms: int = 8000
    """How much audio the model sees. Audio older than this scrolls out and its transcript is
    frozen by the wrapper — `PseudoStreamingBackend`'s docstring is explicit about what that costs.
    8 s comfortably covers a dictation utterance, which endpoints after ~0.8 s of silence
    (plan.md §1.6)."""

    hop_ms: int = 640
    """How often a new hypothesis is produced; also `ready.chunk_ms`. 640 ms is the largest cell of
    plan.md §4's `chunk_ms` sweep and the cheapest — smaller hops multiply the re-decode cost
    linearly. Identical default to `whisper_transformers.py` so the two Whisper rows are driven by
    the same window."""

    # -- device / precision ------------------------------------------------------------------------
    device: str = "auto"
    """`"auto"` | `"cpu"` | `"cuda"`. CTranslate2 takes these strings directly and `"auto"` is its
    own value, so it is passed through rather than resolved here — with one exception noted in
    `_resolve_compute_type()`, which cannot leave the precision to chance."""

    device_index: int = 0

    compute_type: str = "auto"
    """`"auto"` | `float16` | `bfloat16` | `float32` | `int8` | `int8_float16` | `int8_bfloat16` |
    `int8_float32`. See the Blackwell section of the module docstring: `"auto"` resolves to
    `float16` on CUDA (the *reported, unverified* workaround for `CUBLAS_STATUS_NOT_SUPPORTED` on
    RTX 50-series) and `int8` on CPU. Explicitly choosing an int8 CUDA type is permitted and
    warned about; refusing it would make the run that settles CANDIDATES.md §6 item 2
    impossible."""

    cpu_threads: int = 0
    """0 = CTranslate2's own default. Relevant because CANDIDATES.md §6 item 1 makes CPU the
    baseline plan for the ONNX rows on this 28-thread machine, and the same argument applies to a
    CPU CT2 run."""

    num_workers: int = 1
    """Parallel decodes inside one model. Left at 1: this backend decodes one window at a time by
    construction, and >1 costs memory for a concurrency that never happens."""

    # -- decoding ---------------------------------------------------------------------------------
    #
    # Deliberately identical to whisper_transformers.py, so that a Phase 1 difference between rows D
    # and E is attributable to the runtime and the quantisation rather than to decoding policy. Two
    # of them differ from faster-whisper's own defaults, and that is recorded rather than hidden.
    language: str = "en"
    task: str = "transcribe"
    temperature: tuple[float, ...] = (0.0, 0.2, 0.4, 0.6, 0.8, 1.0)
    compression_ratio_threshold: float = 1.35
    """faster-whisper's own default is 2.4; 1.35 is what `..\\wan-dancer\\notes\\transcribe.py` uses
    and what the transformers baseline inherited from it. Kept for comparability, and configurable
    because it was tuned there for sung vocals over a dense mix, which is not our workload."""

    logprob_threshold: float = -1.0
    no_speech_threshold: float = 0.6
    condition_on_previous_text: bool = False
    """faster-whisper's spelling of `condition_on_prev_tokens`. Default False for a reason that is
    ours rather than inherited: conditioning on previous text is a documented driver of
    hallucination loops, and every window here is re-decoded from scratch anyway."""

    beam_size: int = 1
    """1 = greedy. faster-whisper's default is 5. Beam search costs latency the pseudo-streaming
    path can least afford; it is exposed because Phase 1 measures accuracy offline, where the cost
    does not apply."""

    # -- hallucination guard (plan.md §1.6) --------------------------------------------------------
    hallucination_guard: bool = True
    guard_tolerance_s: float = 0.0
    """Seconds of slack past the end of the audio before a segment is dropped. 0.0 reproduces
    `notes/transcribe.py` (`start < duration` keeps). Raise it only with a measurement in hand."""

    silence_peak_floor: int = 0
    """Skip decoding a window whose peak |sample| is <= this. 0 means "skip only digital silence",
    which costs nothing and cannot suppress speech. Anything above 0 is a crude amplitude gate that
    WILL suppress quiet speech; the real gate is the server's Silero VAD."""

    vad_filter: bool = False
    """faster-whisper's built-in Silero VAD. **Off**, and not merely by default — see the guard
    section of the module docstring. Turning it on shortens the audio before decoding, which moves
    the timestamps the past-the-end guard compares against, and double-gates audio the server has
    already gated."""

    # -- context / vocabulary biasing ---------------------------------------------------------------
    context_mode: str = "initial_prompt"
    """`"initial_prompt"` | `"hotwords"` | `"off"`. Both non-off modes are **soft** biasing —
    CANDIDATES.md §2 row E says exactly that — and neither is comparable to the transducer hotwords
    of the zipformer arm. `"off"` declares `context_supported:false` honestly. There is no fourth
    state in which `start.context` is accepted and dropped: plan.md §4 scores context-enabled and
    context-free runs in separate columns, so a silent drop would put a context-free run in the
    context-enabled column."""

    hotwords_note_ack: bool = False
    """Set true to acknowledge UNVERIFIED item D before using `context_mode="hotwords"`.

    Not paranoia about a keyword: the `hotwords` argument is reported to be honoured only when
    `initial_prompt` is absent, and this project has verified neither the argument nor the
    precedence. An unacknowledged hotwords run would produce a context-enabled measurement whose
    biasing mechanism is unknown, which is worse than no measurement."""

    context_join: str = ", "
    context_max_chars: int = 200
    """Whisper's prompt window is ~224 tokens shared with the transcript, so an over-long prompt
    displaces the audio's own context. The term list is truncated here and the truncation logged."""

    # -- loading ------------------------------------------------------------------------------------
    allow_download: bool = False
    """False sets `local_files_only=True`. plan.md §1.9: no model auto-download at run time. True
    exists only for a human doing a deliberate one-off fetch, and is logged at WARNING when used."""

    warmup_noise_peak: int = 400
    """Amplitude of the synthetic warm-up buffer (~1.2 % of full scale). Not silence — see
    `_warm_up()`."""

    extra_transcribe_kwargs: dict[str, Any] = field(default_factory=dict)
    """Passed straight into `transcribe()`. An escape hatch for Phase 1 experiments; anything used
    more than once should become a named field."""

    @classmethod
    def from_mapping(cls, options: Mapping[str, Any]) -> "FasterWhisperConfig":
        """Build from a flat mapping, coercing the strings a command line delivers.

        Keys may use `-` or `_`. An **unknown key raises** rather than being ignored, on the same
        reasoning as the other backends: a misspelled `--backend-opt hop-ms=320` that silently kept
        the default would mislabel a plan.md §4 sweep cell, and the mislabelling would only ever be
        visible in the results file, long after the run.
        """
        norm = {str(k).replace("-", "_").lstrip("_"): v for k, v in options.items()}
        known = set(cls.__dataclass_fields__)
        unknown = sorted(set(norm) - known)
        if unknown:
            raise ValueError(
                f"unknown option(s) for the faster-whisper backend: {', '.join(unknown)}; "
                f"recognised: {', '.join(sorted(known))}"
            )

        kwargs: dict[str, Any] = {}
        for name in ("model", "model_dir", "models_root", "device", "compute_type", "language",
                     "task", "context_mode", "context_join"):
            if norm.get(name) is not None:
                kwargs[name] = str(norm[name])
        for name in ("window_ms", "hop_ms", "device_index", "cpu_threads", "num_workers",
                     "beam_size", "silence_peak_floor", "context_max_chars", "warmup_noise_peak"):
            if norm.get(name) is not None:
                kwargs[name] = _as_int(norm[name], name)
        for name in ("compression_ratio_threshold", "logprob_threshold", "no_speech_threshold",
                     "guard_tolerance_s"):
            if norm.get(name) is not None:
                kwargs[name] = _as_float(norm[name], name)
        for name in ("condition_on_previous_text", "hallucination_guard", "vad_filter",
                     "allow_download", "hotwords_note_ack"):
            if norm.get(name) is not None:
                kwargs[name] = _as_bool(norm[name], name)
        if norm.get("temperature") is not None:
            kwargs["temperature"] = _as_temperatures(norm["temperature"], "temperature")
        if norm.get("extra_transcribe_kwargs") is not None:
            extra = norm["extra_transcribe_kwargs"]
            if not isinstance(extra, Mapping):
                raise ValueError("extra_transcribe_kwargs: expected a mapping")
            kwargs["extra_transcribe_kwargs"] = dict(extra)

        cfg = cls(**kwargs)
        _validate(cfg)
        return cfg


_CONTEXT_MODES = ("initial_prompt", "hotwords", "off")

#: `compute_type` values CTranslate2 documents. Validated locally so a typo is a config error at
#: construction rather than a library error minutes into a load; the authoritative list for the
#: *installed* build is `ctranslate2.get_supported_compute_types(device)`, checked at load
#: (UNVERIFIED item E).
_COMPUTE_TYPES = (
    "auto",
    "int8",
    "int8_float32",
    "int8_float16",
    "int8_bfloat16",
    "float16",
    "bfloat16",
    "float32",
)

_INT8_CUDA_TYPES = ("int8", "int8_float16", "int8_bfloat16", "int8_float32")


def _validate(cfg: FasterWhisperConfig) -> None:
    if cfg.context_mode not in _CONTEXT_MODES:
        raise ValueError(
            f"context_mode must be one of {'|'.join(_CONTEXT_MODES)}, got {cfg.context_mode!r}"
        )
    if cfg.compute_type not in _COMPUTE_TYPES:
        raise ValueError(
            f"compute_type must be one of {'|'.join(_COMPUTE_TYPES)}, got {cfg.compute_type!r}"
        )
    if cfg.device not in ("auto", "cpu", "cuda"):
        raise ValueError(f"device must be auto|cpu|cuda, got {cfg.device!r}")
    if cfg.beam_size < 1:
        raise ValueError(f"beam_size must be >= 1, got {cfg.beam_size}")
    if cfg.context_mode == "hotwords" and not cfg.hotwords_note_ack:
        # Refusing rather than warning. A hotwords run whose biasing mechanism is unverified fills
        # plan.md §4's context-enabled column with a number nobody can interpret, and the person
        # reading that column later has no way to tell.
        raise ValueError(
            "context_mode='hotwords' requires hotwords_note_ack=true.\n"
            "UNVERIFIED item D: faster-whisper's `hotwords` argument is documented for recent "
            "versions and is reported to be honoured only when `initial_prompt` is absent, and "
            "this project has verified neither. Acknowledge that before producing a "
            "context-enabled measurement with it, or use context_mode='initial_prompt', whose "
            "mechanism is at least the documented one for every version."
        )


# ── The backend ────────────────────────────────────────────────────────────────────────────────


class FasterWhisperBackend(PseudoStreamingBackend):
    """CT2 Whisper (`large-v3`, `distil-large-v3`, or any conversion) behind an overlapping window.

    Everything about the window — buffering, hop timing, sliding, prefix stitching, cumulative
    assembly — is `base.PseudoStreamingBackend`'s. This class supplies exactly three things: the
    capability declaration, the model load, and one offline `_transcribe_batch(pcm) -> str`. That is
    the ~50-line adapter plan.md §1.1 promises; the rest of this file is the honesty tax on a
    runtime nobody here has run.

    Threading: single-owner, not thread-safe. One decode loop drives one instance.
    """

    def __init__(
        self,
        config: FasterWhisperConfig | Mapping[str, Any] | None = None,
        **overrides: Any,
    ) -> None:
        # Three call shapes, matching the other backends: a config object, a flat mapping (what
        # `--backend-opt` produces), or keyword arguments. Mapping and keyword forms go through
        # `from_mapping`, so an unknown option is rejected there rather than silently ignored.
        if isinstance(config, FasterWhisperConfig):
            if overrides:
                raise TypeError("pass either a FasterWhisperConfig or option keywords, not both")
            cfg = config
            _validate(cfg)
        else:
            merged: dict[str, Any] = dict(config or {})
            merged.update(overrides)
            cfg = FasterWhisperConfig.from_mapping(merged)
        super().__init__(window_ms=cfg.window_ms, hop_ms=cfg.hop_ms)
        self.config = cfg
        self._model: Any = None
        self._np: Any = None
        self._resolved_source: str = ""
        self._resolved_compute_type: str = ""
        self._resolved_device: str = ""
        self._context_text: str = ""
        self._warned_slow_decode = False
        self._warned_int8_cuda = False
        self._warned_duration_mismatch = False
        # Counters that belong in `bench/results/*.json`: a run whose transcripts were filtered, or
        # whose decode fell behind real time, is not directly comparable with one that was not, and
        # that fact has to survive to the record rather than living in a log nobody reads.
        self.dropped_segment_count = 0
        self.decode_count = 0
        self.decode_ms_total = 0
        self.diagnostics: list[str] = []

    # -- construction from the command line ---------------------------------------------------------

    @classmethod
    def from_backend_config(cls, cfg: Any) -> "FasterWhisperBackend":
        """`kvit_asr.connection.open_backend`'s hook: build from a parsed `BackendConfig`.

        Without this, `open_backend` falls back to `cls(**--backend-opt)` and the *typed* command
        line — `--model`, `--model-dir`, `--chunk-ms`, `--device` — is dropped silently, loading
        `DEFAULT_MODEL` on CT2's own defaults and filing the result under whichever row of
        plan.md §4 the operator believed they had selected.

        Written out rather than derived, because the mapping is per-backend:

        * `--chunk-ms` is the **hop** of a window we built (`PseudoStreamingBackend`), not a model
          property. Free to choose here, unlike the sherpa-onnx rows where each chunk size is a
          different set of weights.
        * `--model-dir` *is* meaningful: it is the CT2 conversion directory, and passing it means
          no hub lookup happens at all. Mapped straight through.
        * `--decoding` names a transducer decoding method and Whisper has none; `beam_size` is the
          nearest knob and is a `--backend-opt`. Rejected loudly rather than ignored.
        * `--device dml` is refused: CTranslate2 takes `auto`/`cpu`/`cuda` and nothing else, and a
          value it does not recognise fails deep inside the library rather than here.

        `--backend-opt` is applied last, so it overrides anything derived here.
        """
        options: dict[str, Any] = {}
        if getattr(cfg, "model", None):
            options["model"] = cfg.model
        if getattr(cfg, "model_dir", None) is not None:
            options["model_dir"] = str(cfg.model_dir)
        if getattr(cfg, "chunk_ms", None) is not None:
            options["hop_ms"] = cfg.chunk_ms

        device = str(getattr(cfg, "device", "auto") or "auto")
        if device == "dml":
            raise ValueError(
                "--device dml is not available for the faster-whisper backend: CTranslate2 "
                "accepts auto, cpu or cuda. Use cpu or cuda."
            )
        if device != "auto":
            # "auto" is CTranslate2's own value and resolving it here would hide which device was
            # actually chosen — the thing the --device help text promises will be reported.
            options["device"] = device

        if getattr(cfg, "decoding", None):
            raise ValueError(
                f"--decoding {cfg.decoding!r} is not meaningful for the faster-whisper backend: "
                "it is an attention encoder-decoder, not a transducer, so it has no "
                "greedy_search/modified_beam_search choice. The nearest knob is "
                "`--backend-opt beam_size=N`."
            )

        options.update(getattr(cfg, "options", None) or {})
        return cls(FasterWhisperConfig.from_mapping(options))

    # -- declaration ------------------------------------------------------------------------------

    @property
    def capabilities(self) -> BackendCapabilities:
        cfg = self.config
        return BackendCapabilities(
            model_id=cfg.model,
            mode=StreamingMode.PSEUDO_STREAMING,
            # Soft biasing through the prompt or the hotwords argument, which is all Whisper has
            # (CANDIDATES.md §2 row E). Declared from configuration, so a run started with
            # context_mode="off" reports false rather than claiming a capability it was told not to
            # use.
            supports_context=(cfg.context_mode != "off"),
            natural_chunk_ms=cfg.hop_ms,
            chunk_ms_configurable=True,
            supported_chunk_ms=(),
            # No decoder state survives a window, but the *window* is a lie if it contains spliced
            # audio, and CT2 will transcribe the splice without complaint. There is a buffer to
            # throw away even where there is no state to protect.
            requires_contiguous_audio=True,
            notes=(
                f"faster-whisper/CTranslate2, window={cfg.window_ms}ms hop={cfg.hop_ms}ms, "
                f"device={cfg.device} compute_type={cfg.compute_type}, "
                f"context={cfg.context_mode}, guard={'on' if cfg.hallucination_guard else 'off'}"
            ),
        )

    # -- load -------------------------------------------------------------------------------------

    def _load(self, progress: ProgressFn | None) -> None:
        """Import faster-whisper and construct the `WhisperModel`.

        Every import here is a *backend* dependency and lives in this method rather than at module
        scope, so an interpreter without CTranslate2 disables this one backend instead of taking
        the server down. `import kvit_asr.backends.faster_whisper` must succeed on a bare stdlib
        CPython — that is the point of the lazy import, and it is a tested property.
        """
        cfg = self.config

        self._report(progress, "import numpy", 2)
        try:
            import numpy as np  # noqa: PLC0415 - backend-local import, deliberately lazy
        except ImportError as exc:
            raise FasterWhisperUnavailable(_missing_runtime_message("numpy", exc)) from exc
        self._np = np

        self._report(progress, "import faster_whisper", 8)
        try:
            # Importing faster_whisper imports ctranslate2, which loads the CT2 native library and
            # (on CUDA builds) resolves cuBLAS/cuDNN DLLs. A missing DLL therefore surfaces here as
            # an ImportError or OSError, not at first decode — which is the better place for it.
            from faster_whisper import WhisperModel  # noqa: PLC0415
        except ImportError as exc:
            raise FasterWhisperUnavailable(_missing_runtime_message("faster_whisper", exc)) from exc
        except OSError as exc:  # DLL load failure on Windows
            raise FasterWhisperUnavailable(_dll_message(exc)) from exc

        device = "cuda" if cfg.device == "auto" and self._cuda_available() else cfg.device
        if device == "auto":
            device = "cpu"
        self._resolved_device = device
        compute_type = self._resolve_compute_type(device)
        self._resolved_compute_type = compute_type
        self._check_compute_type_supported(device, compute_type)

        source = self._resolve_model_source()
        self._resolved_source = source
        self._log_environment(device, compute_type, source)

        kwargs: dict[str, Any] = {
            "device": device,
            "device_index": cfg.device_index,
            "compute_type": compute_type,
            "cpu_threads": cfg.cpu_threads,
            "num_workers": cfg.num_workers,
            # plan.md §1.9. Checked against the real signature below rather than trusted: if this
            # build of faster-whisper does not accept the keyword, we must fail, because dropping
            # it would re-enable exactly the download the policy forbids.
            "local_files_only": not cfg.allow_download,
        }
        self._check_kwargs(WhisperModel.__init__, kwargs, "WhisperModel.__init__", skip=("self",))
        if cfg.allow_download:
            log.warning(
                "allow_download=true: this load MAY fetch %s over the network. plan.md §1.9 makes "
                "model fetching an explicit, separate, user-initiated action, so this should only "
                "ever be a human doing a deliberate one-off — never a configured default.",
                cfg.model,
            )

        self._report(progress, "weights", 25)
        t0 = time.perf_counter()
        try:
            self._model = WhisperModel(source, **kwargs)
        except Exception as exc:  # noqa: BLE001 - re-raised with the context that makes it legible
            raise self._load_failure(exc) from exc
        log.info(
            "loaded %s in %.1fs (device=%s index=%d compute_type=%s)",
            source, time.perf_counter() - t0, device, cfg.device_index, compute_type,
        )

        self._report(progress, "check model", 90)
        self._check_model_shape()
        if self._context:
            # `set_context()` is not lifecycle-ordered; if terms arrived before load, rebuild the
            # prompt now that there is something to validate it against.
            self.set_context(self._context)

    # -- device / precision -------------------------------------------------------------------------

    def _cuda_available(self) -> bool:
        """Best-effort CUDA probe through CTranslate2, which is the only CUDA-aware thing here.

        There is no torch in `envs/ct2` — that is the whole point of the environment
        (CANDIDATES.md §3) — so `torch.cuda.is_available()` is not an option. A failure to probe is
        reported and treated as "no CUDA": guessing "yes" would produce a load error instead of a
        slow CPU run, and a slow CPU run that says so in the log is the more useful outcome.
        """
        try:
            import ctranslate2  # noqa: PLC0415
        except Exception as exc:  # noqa: BLE001 - probing must never be why a load fails
            log.debug("could not import ctranslate2 to probe for CUDA: %s", exc)
            return False
        getter = getattr(ctranslate2, "get_cuda_device_count", None)
        if getter is None:
            log.warning(
                "ctranslate2 has no get_cuda_device_count(); cannot probe for a GPU, assuming CPU. "
                "Pass device=cuda explicitly to override."
            )
            return False
        try:
            count = int(getter())
        except Exception as exc:  # noqa: BLE001
            log.warning("ctranslate2.get_cuda_device_count() failed (%s); assuming CPU", exc)
            return False
        if count <= 0:
            note = "device=auto resolved to CPU: ctranslate2 reports 0 CUDA devices"
            self.diagnostics.append(note)
            log.warning(
                "%s. large-v3 on CPU is far slower than real time, so any latency figure from "
                "this run is queueing delay — record it as CPU or do not record it at all.", note,
            )
        return count > 0

    def _resolve_compute_type(self, device: str) -> str:
        """Turn `compute_type="auto"` into a concrete choice, and warn about the int8 CUDA report.

        `"auto"` is a legal CTranslate2 value, but leaving it to the library would mean the run
        record could not say what precision produced the numbers — and precision is exactly the
        variable CANDIDATES.md §6 item 2 is unsure about. So it is resolved here, logged, and
        recorded in `describe()`.
        """
        cfg = self.config
        if cfg.compute_type == "auto":
            # CUDA -> float16: the workaround the third-party reports describe. UNVERIFIED, and the
            # docstring says so; chosen because it is the option with a chance of working on the
            # card this project targets. CPU -> int8: CTranslate2's ordinary recommendation, and
            # the Blackwell report says nothing about CPU kernels.
            resolved = "float16" if device == "cuda" else "int8"
            log.info(
                "compute_type=auto resolved to %r on device=%s (see bench/CANDIDATES.md §6 item 2: "
                "the RTX 50-series CUBLAS_STATUS_NOT_SUPPORTED report is third-party and "
                "UNVERIFIED here)", resolved, device,
            )
            return resolved
        if device == "cuda" and cfg.compute_type in _INT8_CUDA_TYPES and not self._warned_int8_cuda:
            self._warned_int8_cuda = True
            note = (
                f"compute_type={cfg.compute_type!r} on CUDA is exactly the configuration "
                "bench/CANDIDATES.md §6 item 2 records third-party reports of "
                "CUBLAS_STATUS_NOT_SUPPORTED for on RTX 50-series hardware. That report is "
                "UNVERIFIED by this project and is not being treated as fact — this run is "
                "allowed to proceed, and if it works it is evidence worth recording in "
                "CANDIDATES.md. If it fails with a cuBLAS error, the reported workaround is "
                "compute_type=float16."
            )
            self.diagnostics.append(note)
            log.warning("%s", note)
        return cfg.compute_type

    def _check_compute_type_supported(self, device: str, compute_type: str) -> None:
        """Ask CTranslate2 what this build actually supports. UNVERIFIED item E — best effort.

        Absence of the query function is tolerated (it is a diagnostic, not a gate). Disagreement
        is not: failing at load with the supported list in the message beats failing at the first
        decode with a message about a kernel.
        """
        try:
            import ctranslate2  # noqa: PLC0415
        except Exception:  # noqa: BLE001
            return
        getter = getattr(ctranslate2, "get_supported_compute_types", None)
        if getter is None:
            log.debug("ctranslate2 has no get_supported_compute_types(); skipping the check")
            return
        try:
            supported = set(getter(device))
        except Exception as exc:  # noqa: BLE001
            log.debug("get_supported_compute_types(%r) failed: %s", device, exc)
            return
        if compute_type not in supported:
            raise FasterWhisperApiMismatch(
                f"compute_type={compute_type!r} is not supported by this CTranslate2 build on "
                f"device={device!r}. Supported: {', '.join(sorted(supported)) or '(none reported)'}. "
                f"(ctranslate2 {_version('ctranslate2')}, faster_whisper "
                f"{_version('faster_whisper')})\n"
                "On CUDA, bench/CANDIDATES.md §6 item 2 records float16 as the reported working "
                "choice on RTX 50-series; on CPU, int8 is the usual one."
            )

    # -- model location -----------------------------------------------------------------------------

    def _resolve_model_source(self) -> str:
        """What to hand `WhisperModel(...)` as its first argument. Never fetches anything.

        Three cases, in priority order:
          1. `model_dir` — an explicit directory, passed verbatim, no hub lookup at all.
          2. `models_root` / `$KVIT_ASR_MODELS_DIR` — the model's last path component appended,
             matching the sherpa-onnx backend so one environment variable serves every adapter.
          3. the model id itself — resolved by faster-whisper out of the shared `HF_HOME`
             (plan.md §5.2) with `local_files_only=True`, which turns a cache miss into an error
             rather than a download.
        """
        cfg = self.config
        if cfg.model_dir:
            path = Path(cfg.model_dir).expanduser()
            if not path.is_dir():
                raise FasterWhisperWeightsMissing(
                    f"model_dir does not exist: {path}\n" + self._download_hint()
                )
            self._check_ct2_dir(path)
            return str(path)

        root = cfg.models_root or os.environ.get("KVIT_ASR_MODELS_DIR") or ""
        if root:
            path = Path(root).expanduser() / cfg.model.replace("\\", "/").rsplit("/", 1)[-1]
            if path.is_dir():
                self._check_ct2_dir(path)
                return str(path)
            log.info(
                "no directory at %s; falling back to resolving %r from the HuggingFace cache "
                "(HF_HOME=%s)", path, cfg.model, os.environ.get("HF_HOME", "<unset>"),
            )

        direct = Path(cfg.model).expanduser()
        if direct.is_dir():
            self._check_ct2_dir(direct)
            return str(direct)
        return cfg.model

    def _check_ct2_dir(self, path: Path) -> None:
        """Confirm a directory looks like a CTranslate2 conversion before handing it over.

        A directory holding the *transformers* form of Whisper (`model.safetensors`) is a common
        and confusing mistake here — the two are different formats of the same weights, and
        faster-whisper's error for it is not obviously about that. Missing files are a warning
        rather than an error: the exact file set has not been verified against a real conversion
        on this machine (nothing is installed), so refusing on the strength of an unverified
        expectation could block a load that would have worked.
        """
        if not (path / "model.bin").is_file():
            hint = ""
            if (path / "model.safetensors").is_file() or (path / "pytorch_model.bin").is_file():
                hint = (
                    " It contains transformers-format weights instead. CTranslate2 needs a "
                    "converted model — that is what the `Systran/faster-whisper-*` repositories "
                    "are, and it is why row E is a different download from row D."
                )
            log.warning("%s has no model.bin, so it may not be a CTranslate2 conversion.%s",
                        path, hint)

    def _download_hint(self) -> str:
        """The exact fetch a human runs. The sidecar never runs it (plan.md §1.9)."""
        what, size = KNOWN_MODELS.get(self.config.model, ("", "size unknown"))
        described = f" ({what}, {size})" if what else f" ({size})"
        return (
            f"Weights for {self.config.model!r}{described} are not on this disk, and the sidecar "
            "does not fetch them: plan.md §1.9 makes model fetching an explicit, separate, "
            "user-initiated action and the sidecar performs no network access at all.\n\n"
            "A human downloads it once, into the shared cache plan.md §5.2 already uses:\n\n"
            f"    set HF_HOME=c:\\apps\\llama-swap\\gguf\\hf\n"
            f"    hf download {self.config.model}\n"
            "    (older Hugging Face CLIs spell this `huggingface-cli download`)\n\n"
            "or unpacks it under %KVIT_ASR_MODELS_DIR% and passes --model-dir. Already cached on "
            "this machine: Systran/faster-whisper-base.en (141 MB) — useful as a smoke test, not "
            "as a candidate."
        )

    # -- load diagnostics ---------------------------------------------------------------------------

    def _log_environment(self, device: str, compute_type: str, source: str) -> None:
        """Log what is actually going to run. Cheap, and the first thing wanted after a bad run."""
        log.info(
            "interpreter=%s python=%s faster_whisper=%s ctranslate2=%s numpy=%s | "
            "device=%s index=%d compute_type=%s | source=%s | HF_HOME=%s local_files_only=%s",
            sys.executable, sys.version.split()[0], _version("faster_whisper"),
            _version("ctranslate2"), getattr(self._np, "__version__", "unknown"),
            device, self.config.device_index, compute_type, source,
            os.environ.get("HF_HOME", "<unset>"), not self.config.allow_download,
        )

    def _check_kwargs(
        self, func: Any, kwargs: Mapping[str, Any], what: str, *, skip: Sequence[str] = ()
    ) -> None:
        """Refuse to call `func` with a keyword it does not accept. UNVERIFIED items A and B.

        The alternative — filtering unknown keywords out — is what makes an adapter written against
        an unavailable library dangerous: `local_files_only` would vanish and the process would
        download a model, `compute_type` would vanish and the run record would name a precision
        that never happened. A signature with `**kwargs` is accepted as-is, because then nothing
        can be rejected on the evidence available.
        """
        try:
            sig = inspect.signature(func)
        except (TypeError, ValueError) as exc:  # C extension or a wrapper with no signature
            log.debug("cannot inspect %s (%s); passing keywords unchecked", what, exc)
            return
        if any(p.kind is inspect.Parameter.VAR_KEYWORD for p in sig.parameters.values()):
            return
        accepted = {n for n in sig.parameters if n not in skip}
        unknown = sorted(set(kwargs) - accepted)
        if unknown:
            raise FasterWhisperApiMismatch(
                f"{what} does not accept {', '.join(unknown)} in the installed build "
                f"(faster_whisper {_version('faster_whisper')}, ctranslate2 "
                f"{_version('ctranslate2')}). It accepts: {', '.join(sorted(accepted))}.\n"
                "This adapter was written against the documented public API and never ran against "
                "an installed copy (see the module docstring); a keyword it cannot pass is a real "
                "difference, not a nuisance — dropping it silently could re-enable downloads or "
                "change the precision the run record claims."
            )

    def _check_model_shape(self) -> None:
        """Confirm the object exposes what this adapter drives, before `ready` is sent."""
        if not callable(getattr(self._model, "transcribe", None)):
            raise FasterWhisperApiMismatch(
                f"WhisperModel for {self.config.model!r} has no callable `transcribe`; this "
                f"adapter was written against faster-whisper's WhisperModel API "
                f"(installed: {_version('faster_whisper')})"
            )
        probe: dict[str, Any] = {
            "language": self.config.language,
            "task": self.config.task,
            "beam_size": self.config.beam_size,
            "temperature": tuple(self.config.temperature),
            "compression_ratio_threshold": self.config.compression_ratio_threshold,
            "log_prob_threshold": self.config.logprob_threshold,
            "no_speech_threshold": self.config.no_speech_threshold,
            "condition_on_previous_text": self.config.condition_on_previous_text,
            "vad_filter": self.config.vad_filter,
            "without_timestamps": False,
        }
        if self.config.context_mode == "initial_prompt":
            probe["initial_prompt"] = ""
        elif self.config.context_mode == "hotwords":
            probe["hotwords"] = ""
        # Checked at load rather than at the first decode: `ready` is a promise that the next
        # utterance will work, and a keyword mismatch discovered mid-session costs a session.
        self._check_kwargs(self._model.transcribe, probe, "WhisperModel.transcribe", skip=("self",))

    def _load_failure(self, exc: Exception) -> BackendLoadError:
        """Turn a `WhisperModel(...)` failure into something a human can act on."""
        text = f"{type(exc).__name__}: {exc}"
        low = text.lower()

        if any(tok in low for tok in ("cublas", "cudnn", "cuda_error", "libcublas", "cudart")):
            return BackendLoadError(
                f"CTranslate2 failed to initialise CUDA for {self.config.model!r} with "
                f"compute_type={self._resolved_compute_type!r}: {text}\n\n"
                + _blackwell_advice()
            )
        if "dll" in low or "shared library" in low or "cannot load" in low:
            return BackendLoadError(_dll_message(exc))
        if any(
            tok in low
            for tok in ("local_files_only", "offline", "not found", "couldn't connect",
                        "no such file", "cannot find", "does not appear to have")
        ) and not self.config.allow_download:
            return FasterWhisperWeightsMissing(
                self._download_hint() + f"\n\nUnderlying error: {text}"
            )
        return BackendLoadError(
            f"failed to construct WhisperModel for {self.config.model!r} "
            f"(source={self._resolved_source!r}, faster_whisper {_version('faster_whisper')}, "
            f"ctranslate2 {_version('ctranslate2')}, interpreter {sys.executable}): {text}"
        )

    def _unload(self) -> None:
        # CTranslate2 frees device memory when the model object is collected; there is no documented
        # explicit release, and inventing one (calling an undocumented `unload_model`) would be a
        # guess about a library nobody here has run. Dropping the reference is the honest action.
        self._model = None
        self._np = None

    # -- warm-up -------------------------------------------------------------------------------------

    def _warm_up(self, audio_ms: int) -> None:
        """Warm on low-level noise rather than the base class's digital silence.

        Same two reasons as `whisper_transformers.py`, and they are worth restating because getting
        this wrong produces a `warm_ms` that looks like a measurement: `silence_peak_floor`
        short-circuits an all-zero window *without decoding*, so warming on silence would time the
        skip; and Whisper on pure silence is precisely the hallucination case plan.md §1.6
        describes, making it the least representative input available for timing a real decode.

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

    # -- context --------------------------------------------------------------------------------------

    def set_context(self, terms: Sequence[str]) -> bool:
        """Turn `start.context` into an `initial_prompt` or a `hotwords` string.

        Both are **soft** biasing: they make the decoder *more likely* to spell `llama.cpp` that way
        and guarantee nothing (CANDIDATES.md §2 row E). That is the honest description of what this
        row can do, and it is why the context-biasing arm of the bake-off is a transducer with real
        hotwords rather than any Whisper.

        Exactly one mechanism is ever sent. UNVERIFIED item D: `hotwords` is reported to be honoured
        only when `initial_prompt` is absent, so passing both would leave the run record unable to
        say which one biased it.
        """
        applied = super().set_context(terms)  # stores self._context, returns supports_context
        self._context_text = ""
        if not applied:
            if self._context:
                log.warning(
                    "%d context item(s) supplied but context_mode='off'; they are ignored and "
                    "context_supported is reported false. Treat this run as context-free "
                    "(plan.md §4).",
                    len(self._context),
                )
            return applied
        if not self._context:
            return applied

        text = self.config.context_join.join(t.strip() for t in self._context if t.strip())
        if len(text) > self.config.context_max_chars:
            cut = text[: self.config.context_max_chars].rsplit(self.config.context_join, 1)[0]
            log.warning(
                "context prompt truncated from %d to %d characters; %d of %d term(s) kept. "
                "Whisper's prompt window is shared with the transcript, so a long prompt displaces "
                "the audio's own context.",
                len(text), len(cut), cut.count(self.config.context_join) + 1, len(self._context),
            )
            text = cut
        self._context_text = text
        log.info("context %s (%d chars): %r", self.config.context_mode, len(text), text)
        return applied

    # -- decode ----------------------------------------------------------------------------------------

    def _transcribe_batch(self, pcm: bytes) -> str:
        """Transcribe one whole window offline. Called once per hop by `PseudoStreamingBackend`."""
        if self._model is None:
            raise BackendDecodeError("_transcribe_batch called before load()")
        if not pcm:
            return ""

        audio_s = len(pcm) / (SAMPLE_RATE * 2)
        if pcm_peak(pcm) <= self.config.silence_peak_floor:
            # Digital silence by default. Feeding it to Whisper is the documented way to get
            # "Thank you." out of a model (plan.md §1.6), and it costs a full decode to do so.
            log.debug("window of %.2fs is below the peak floor; not decoded", audio_s)
            return ""

        # float32 in [-1, 1) at 16 kHz. `<i2` names little-endian explicitly rather than relying on
        # host order: the wire format is fixed at s16le by docs/protocol.md §6.3, and a byte-swapped
        # read produces audio that decodes to plausible-looking nonsense rather than failing.
        samples = self._np.frombuffer(pcm, dtype="<i2").astype(self._np.float32) / 32768.0

        kwargs = self._transcribe_kwargs()
        t0 = time.perf_counter()
        try:
            result = self._model.transcribe(samples, **kwargs)
            if not (isinstance(result, tuple) and len(result) == 2):
                raise FasterWhisperApiMismatch(
                    f"WhisperModel.transcribe returned {type(result).__name__}; this adapter "
                    "expects the documented (segments, info) tuple "
                    f"(faster_whisper {_version('faster_whisper')})"
                )
            segments, info = result
            # UNVERIFIED item B, and the single most important line in this method: `segments` is a
            # generator and **the decode happens here**, while it is consumed. Materialising it
            # inside this try/except is what turns a decode failure into `decode_failed` instead of
            # an exception escaping from a list comprehension three frames up; timing it here is
            # what makes the measured decode cost real.
            collected = list(segments)
        except FasterWhisperApiMismatch:
            raise
        except Exception as exc:  # noqa: BLE001
            raise self._decode_failure(exc, audio_s, kwargs) from exc
        decode_ms = int(round((time.perf_counter() - t0) * 1000))
        self.decode_count += 1
        self.decode_ms_total += decode_ms
        self._note_decode_cost(decode_ms, audio_s)

        return self._extract_text(collected, info, audio_s)

    def _transcribe_kwargs(self) -> dict[str, Any]:
        cfg = self.config
        kwargs: dict[str, Any] = {
            "language": cfg.language,
            "task": cfg.task,
            "beam_size": cfg.beam_size,
            "temperature": list(cfg.temperature),
            "compression_ratio_threshold": cfg.compression_ratio_threshold,
            "log_prob_threshold": cfg.logprob_threshold,
            "no_speech_threshold": cfg.no_speech_threshold,
            "condition_on_previous_text": cfg.condition_on_previous_text,
            "vad_filter": cfg.vad_filter,
            # Segment timestamps are what the hallucination guard filters on, so they are requested
            # whenever the guard is on. `without_timestamps=True` would make the guard inert while
            # leaving it apparently enabled.
            "without_timestamps": not cfg.hallucination_guard,
        }
        if self._context_text:
            # Exactly one, never both — see set_context().
            if cfg.context_mode == "initial_prompt":
                kwargs["initial_prompt"] = self._context_text
            elif cfg.context_mode == "hotwords":
                kwargs["hotwords"] = self._context_text
        kwargs.update(cfg.extra_transcribe_kwargs)
        return kwargs

    def _decode_failure(
        self, exc: Exception, audio_s: float, kwargs: Mapping[str, Any]
    ) -> BackendDecodeError | BackendLoadError:
        """Classify a decode failure. cuBLAS gets the Blackwell advice; everything else is verbatim."""
        text = f"{type(exc).__name__}: {exc}"
        low = text.lower()
        if any(tok in low for tok in ("cublas", "cudnn", "cuda")):
            # Still non-fatal (docs/protocol.md §11.3: `decode_failed` aborts the session and keeps
            # the connection), because retrying with a different compute_type means a restart the
            # parent performs, not a state this process can recover into by itself.
            return BackendDecodeError(
                f"CTranslate2 decode failed on a {audio_s:.2f}s window with "
                f"compute_type={self._resolved_compute_type!r}: {text}\n\n" + _blackwell_advice()
            )
        return BackendDecodeError(
            f"faster-whisper decode failed on a {audio_s:.2f}s window "
            f"(model={self.config.model!r}, kwargs={sorted(kwargs)}): {text}"
        )

    def _extract_text(self, segments: Sequence[Any], info: Any, audio_s: float) -> str:
        """Assemble the window transcript and run the shared hallucination guard over it."""
        if not segments:
            return ""

        if not self.config.hallucination_guard:
            return " ".join(str(getattr(s, "text", "")).strip() for s in segments).strip()

        # Adapt faster-whisper's Segment objects into the `{"text", "timestamp"}` shape the shared
        # guard consumes (UNVERIFIED item C). A segment with no `text` attribute is an API mismatch
        # rather than something to skip: skipping would silently shorten transcripts.
        chunks: list[dict[str, Any]] = []
        for seg in segments:
            if not hasattr(seg, "text"):
                raise FasterWhisperApiMismatch(
                    f"a transcribe() segment is {type(seg).__name__} with no `.text`; this adapter "
                    "expects faster-whisper Segment objects with .start/.end/.text "
                    f"(faster_whisper {_version('faster_whisper')})"
                )
            chunks.append({
                "text": str(seg.text),
                "timestamp": (getattr(seg, "start", None), getattr(seg, "end", None)),
            })

        # Our own byte count, not `info.duration`. The two should agree, but `info.duration` is the
        # library's view (and becomes duration-after-VAD when vad_filter is on), while the guard's
        # question is "did the model claim text beyond the audio *we* handed it".
        kept, dropped = _apply_hallucination_guard(chunks, audio_s, self.config.guard_tolerance_s)
        if dropped:
            self.dropped_segment_count += len(dropped)
            # plan.md §1.6 asks for this to be logged when it happens: it is evidence about the
            # model, and silently better output would hide a real property of the candidate.
            log.warning(
                "hallucination guard dropped %d segment(s) beyond %.2fs of audio: %s",
                len(dropped), audio_s, "; ".join(str(d) for d in dropped),
            )
        self._sanity_check_duration(info, audio_s)
        return " ".join(kept).strip()

    def _sanity_check_duration(self, info: Any, audio_s: float) -> None:
        """Warn once if the library's idea of the audio length disagrees with ours.

        A disagreement means the guard is comparing timestamps against the wrong number, so the
        hallucination-rate metric plan.md §4 collects would be measuring something else. Half a
        second of tolerance covers rounding; more than that is a real difference — most likely
        `vad_filter` having removed audio.
        """
        duration = getattr(info, "duration", None)
        if duration is None or self._warned_duration_mismatch:
            return
        try:
            reported = float(duration)
        except (TypeError, ValueError):
            return
        if abs(reported - audio_s) > 0.5:
            self._warned_duration_mismatch = True
            note = (
                f"transcribe() reports duration={reported:.2f}s for a window we measured at "
                f"{audio_s:.2f}s. The past-the-end hallucination guard compares against our "
                "number; if vad_filter is on, that is expected and the guard is measuring "
                "something other than what plan.md §1.6 describes."
            )
            self.diagnostics.append(note)
            log.warning("%s", note)

    def _note_decode_cost(self, decode_ms: int, audio_s: float) -> None:
        """Warn once when a window decode costs more than the hop it must keep up with."""
        if decode_ms <= self.hop_ms or self._warned_slow_decode:
            return
        self._warned_slow_decode = True
        note = (
            f"a {audio_s:.2f}s window took {decode_ms}ms to decode, longer than the {self.hop_ms}ms "
            "hop: this backend is behind real time and its latency figures are queueing delay, not "
            "model latency"
        )
        self.diagnostics.append(note)
        log.warning("%s", note)

    # -- reporting -------------------------------------------------------------------------------------

    def describe(self) -> dict:
        """Base fields plus what a result file needs to interpret this run."""
        d = super().describe()
        d.update(
            runtime="faster-whisper/ctranslate2",
            faster_whisper_version=_version("faster_whisper"),
            ctranslate2_version=_version("ctranslate2"),
            window_ms=self.window_ms,
            hop_ms=self.hop_ms,
            device=self._resolved_device or self.config.device,
            compute_type=self._resolved_compute_type or self.config.compute_type,
            model_source=self._resolved_source or self.config.model,
            language=self.config.language,
            beam_size=self.config.beam_size,
            vad_filter=self.config.vad_filter,
            hallucination_guard=self.config.hallucination_guard,
            dropped_segments=self.dropped_segment_count,
            decodes=self.decode_count,
            mean_decode_ms=(
                round(self.decode_ms_total / self.decode_count) if self.decode_count else None
            ),
            context_mode=self.config.context_mode,
            context_text=self._context_text or None,
            diagnostics=list(self.diagnostics),
        )
        return d


# ── Helpers ────────────────────────────────────────────────────────────────────────────────────


def _version(package: str) -> str:
    """Installed version of `package`, or "not installed". Never raises."""
    try:
        module = __import__(package)
    except Exception:  # noqa: BLE001 - a broken third-party import must not break an error message
        return "not installed"
    return str(getattr(module, "__version__", "unknown"))


def _blackwell_advice() -> str:
    """The one paragraph a human needs when CUDA fails here. Reports, never asserts."""
    return (
        "bench/CANDIDATES.md §6 item 2 records multiple THIRD-PARTY, UNVERIFIED reports of "
        "CUBLAS_STATUS_NOT_SUPPORTED from CTranslate2 on RTX 50-series (Blackwell, sm_120) with "
        "int8 compute, worked around by forcing compute_type=float16. This project has not "
        "verified either the failure or the workaround, so treat the following as the first thing "
        "to try, not as a diagnosis:\n"
        "  --backend-opt compute_type=float16      (the reported workaround)\n"
        "  --backend-opt device=cpu                (int8 on CPU is unaffected by the report; slow)\n"
        "Also confirm the CUDA 12 and cuDNN 9 runtime DLLs are on PATH — bench/CANDIDATES.md §3 "
        "notes that envs/ct2 needs them and has no torch to supply them, which is the difference "
        "between this environment and the one the transformers baseline runs in.\n"
        "Whatever the outcome, record it in CANDIDATES.md §6: settling that item is worth more "
        "than the run that produced it."
    )


def _dll_message(exc: Exception) -> str:
    """Windows DLL failure — nearly always the CUDA/cuDNN runtime, not the Python package."""
    return (
        f"loading the CTranslate2 native library failed: {type(exc).__name__}: {exc}\n"
        "On Windows this is almost always a missing CUDA 12 / cuDNN 9 runtime DLL rather than a "
        "broken Python package. bench/CANDIDATES.md §3 records that envs/ct2 needs those DLLs and "
        "carries no torch to supply them — which is exactly the difference between this "
        "environment and the wan-dancer venv the transformers baseline uses, where torch ships its "
        "own CUDA runtime.\n"
        "A CPU-only run avoids the whole question: --backend-opt device=cpu"
    )


def _missing_runtime_message(package: str, exc: ImportError) -> str:
    """The message a human sees when this backend is launched under the wrong interpreter."""
    return (
        f"`import {package}` failed in this interpreter ({sys.executable}), so the faster-whisper "
        f"backend is unavailable: {type(exc).__name__}: {exc}\n"
        "This backend needs its own environment — bench/CANDIDATES.md §3 calls it envs/ct2 — "
        "because faster-whisper pulls ctranslate2, av, tokenizers and onnxruntime, and none of "
        "those may be added to an existing environment. In particular NOTHING is to be installed "
        "into D:\\projects\\wan-dancer\\.venv: it belongs to another project, it is read-only for "
        "kvit-dictate, and plan.md §5.1 is explicit that corrupting a working GPU stack is a worse "
        "and far less reversible cost than disk.\n"
        "A human creates it once (server/README.md has the full form):\n\n"
        "    set UV_CACHE_DIR=D:\\uv-cache\n"
        f"    uv venv --python 3.12 {ENV_DIR}\n"
        f"    uv pip install --python {EXPECTED_INTERPRETER} -r {REQUIREMENTS_FILE}\n\n"
        "UV_CACHE_DIR must be on D:. uv hardlinks packages from its cache into the venv, hardlinks "
        "cannot cross volumes, and a C: cache with D: venvs silently full-copies every package "
        "(plan.md §5.2 item 4).\n"
        "The sidecar never installs anything itself, and never downloads a model (plan.md §1.9)."
    )


def create(options: Mapping[str, Any] | None = None, **kwargs: Any) -> FasterWhisperBackend:
    """Factory a caller can use without importing the config dataclass.

    `backends.REGISTRY` resolves to the *class*, not to this function, so this exists for the bench
    harness and for symmetry with the other backend modules. Registry entry, for whoever owns
    `backends/__init__.py`:

        "faster-whisper": ".faster_whisper:FasterWhisperBackend"

    matching a `--backend faster-whisper` spelling in models.json.
    """
    return FasterWhisperBackend(options, **kwargs)
