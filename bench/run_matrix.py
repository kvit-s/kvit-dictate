#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""kvit-dictate experiment runner — plan.md §4.

This file exists to make one specific thing impossible: choosing the winner after seeing the
results. plan.md §4 states the decision rule *before* the corpus is run, and a rule that lives
only in a document gets reinterpreted the moment the numbers are inconvenient. So it lives here
as code, it runs against the recorded metrics, and it prints every configuration it eliminated
together with the reason. A rule that only shows its winner is not auditable.

Five properties are load-bearing. Each is implemented as a mechanism, not a convention:

1. **Sequential elimination, not a cross-product.** `PhaseSpec` declares, per phase, which
   dimensions may be swept and which are pinned. `build_cells()` refuses to sweep a pinned
   dimension. Phase 1 cuts on accuracy (model only); Phase 2 sweeps chunk/stability/cadence on
   the survivors. A five-dimensional sweep is 4 models x 4 chunks x 18 stability x 3 cadence x
   2 inject x 5 categories x 2 context modes = 34 560 cells; this is the guard that stops one
   happening by accident.

2. **The dev/heldout split is enforced mechanically.** `heldout/` is the decision set. A run
   against it requires `--heldout-decision`, every heldout access is appended to
   `results/heldout-ledger.aggregate.json`, and every result file records which split produced
   it. Without this the pre-registered rule can be undermined quietly, by tuning on the set the
   rule is supposed to decide against.

3. **Audio replays at real-time cadence by default.** Feeding a streaming model as fast as it
   accepts flatters every latency number and makes them meaningless (plan.md Phase 2 step 2).
   Faster replay requires `--allow-faster-than-realtime`, stamps `replay.realtime = false` in
   the result, and is reprinted as a warning in the summary. Nothing quietly compares a
   real-time run against a turbo one.

4. **Every coverage cap is logged.** plan.md: *"Any coverage cap — top-N, sampling, no-retry —
   is logged; silent truncation reads as 'we tested everything'."* Dropped cells, sampled
   corpora, abandoned retries, dimensions a model does not support, and chunk sizes substituted
   because the published model repo has no such variant all land in `coverage_caps`, in the cell
   file and in the summary.

5. **Context-enabled and context-free are never pooled.** They are separate `context_mode`
   values, the selection rule runs once per mode, and the report says so. bench/CANDIDATES.md
   §5.1 is why this matters concretely: the leading streaming candidate (Nemotron via
   sherpa-onnx) is `greedy_search`-only and *cannot* be context-biased, while the zipformer arm
   can. Pooling them would compare a biased model against an unbiased one on the metric that
   biasing helps most.

Scope, honestly stated
----------------------
This module is the *orchestrator*. It owns the grid, the cell files, the guards, the commit
simulation and the decision rule. It does not own metric computation (bench/metrics.py) and it
does not own the ASR itself (server/kvit_asr). Two consequences:

- `SidecarExecutor` is written against docs/protocol.md and against `kvit_asr.protocol`, and is
  **UNVERIFIED**: no model is installed on this machine, no sidecar has been run against it, and
  the `ctypes` named-pipe *server* side here has never been exercised. It is marked at its class
  docstring and it fails loudly and specifically rather than limping.
- `MockExecutor` produces **synthetic** numbers. Every result it writes carries
  `"synthetic": true`, and the selection rule *refuses to select from synthetic results* unless
  `--allow-synthetic` is passed. A mock run must never be mistaken for a measurement.

Stdlib only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import random
import re
import statistics
import subprocess
import sys
import time
import wave
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Any, Callable, Iterable, Iterator, Sequence

# ═══════════════════════════════════════════════════════════════════════════════════════════════
# Identity of a result
# ═══════════════════════════════════════════════════════════════════════════════════════════════

SCHEMA_VERSION = 1
HARNESS_NAME = "kvit-dictate/bench/run_matrix.py"

BENCH_DIR = Path(__file__).resolve().parent
REPO_ROOT = BENCH_DIR.parent
DEFAULT_CORPUS_ROOT = BENCH_DIR / "corpus"
DEFAULT_RESULTS_ROOT = BENCH_DIR / "results"

SPLITS = ("dev", "heldout")
CONTEXT_MODES = ("context_free", "context_enabled")

# Corpus categories, plan.md Phase 1 step 2. `silence` and `noise` are first-class cells, not an
# afterthought: hallucination on silence is a headline failure of Whisper-class models
# (plan.md §1.6) and a candidate that hallucinates must be visible in the same table as one that
# does not.
CATEGORIES = ("tech", "hard", "clean", "silence", "noise")
HALLUCINATION_CATEGORIES = ("silence", "noise")
# Which categories feed technical-term F1 — the quantity the selection rule maximises. `tech` is
# the corpus built for exactly this; `clean` is a LibriSpeech sanity slice with no technical
# vocabulary in it and would dilute the number toward meaninglessness.
F1_CATEGORIES = ("tech",)

CADENCES = ("immediate", "500ms", "1000ms")
CADENCE_MS = {"immediate": 0, "500ms": 500, "1000ms": 1000}
INJECT_METHODS = ("SendInput", "clipboard")

# plan.md §4's requested chunk grid. Note it is a *request*: the published sherpa-onnx model
# repos are one repo per chunk size (CANDIDATES.md §2 rows A'/B'), so 320 ms and 640 ms do not
# exist for every model. `resolve_chunk()` substitutes the nearest published variant and logs a
# coverage cap. Silently running 560 ms in a cell labelled 640 ms would be exactly the kind of
# quiet truncation plan.md §4 forbids.
CHUNK_MS_GRID = (80, 160, 320, 640)

STABILITY_N_GRID = (2, 3)
STABILITY_M_GRID = (2, 3, 5)
STABILITY_T_GRID = (100, 200, 400)

# plan.md §1.4 defaults. Phase 1 pins these; Phase 2 sweeps them.
DEFAULT_STABILITY = (2, 3, 200)
DEFAULT_CADENCE = "immediate"
DEFAULT_INJECT = "SendInput"

# Keys a result must carry for the selection rule to be applicable. A configuration missing any
# of them is ELIMINATED with reason `missing_metric:<key>` — never silently skipped, because a
# silently skipped configuration is indistinguishable from one that lost.
SELECTION_METRIC_KEYS = (
    "technical_term_f1",
    "icer_per_1000",
    "first_partial_ms",
    "finalisation_ms",
    "peak_vram_mb",
)

# bench/metrics.py is written by a different hand and spells the objective `term_f1`. Rather than
# either module quietly renaming the other's metric, the runner keeps one canonical name per
# quantity and accepts the known spellings on input. `check_gate_metrics_agree()` compares this
# table against `metrics.select_gate_metrics()` and reports a mismatch instead of silently
# gating on a metric that is not the one that was computed.
METRIC_ALIASES: dict[str, tuple[str, ...]] = {
    "technical_term_f1": ("technical_term_f1", "term_f1"),
    "icer_per_1000": ("icer_per_1000",),
    "icer_error_graphemes": ("icer_error_graphemes", "error_graphemes"),
    "icer_committed_graphemes": ("icer_committed_graphemes", "committed_graphemes"),
    "first_partial_ms": ("first_partial_ms",),
    "finalisation_ms": ("finalisation_ms",),
    "latency_median_ms": ("latency_median_ms",),
    "peak_vram_mb": ("peak_vram_mb",),
    "hallucination_rate": ("hallucination_rate",),
    "wer_raw": ("wer_raw",),
    "wer_norm": ("wer_norm", "wer_normalised"),
    "cer_raw": ("cer_raw",),
    "cer_norm": ("cer_norm", "cer_normalised"),
    "rtf": ("rtf", "real_time_factor"),
    "flicker_per_1000": ("flicker_per_1000",),
}


def metric_value(metrics: dict[str, Any], canonical: str) -> float | None:
    """Read one metric, accepting any of its known spellings. `None` means absent, not zero."""
    for name in METRIC_ALIASES.get(canonical, (canonical,)):
        val = metrics.get(name)
        if isinstance(val, (int, float)) and not isinstance(val, bool):
            return float(val)
    return None


def check_gate_metrics_agree() -> list[str]:
    """Compare the runner's gate set against `metrics.select_gate_metrics()`.

    bench/metrics.py exposes that function so "flicker gates" cannot be introduced by an edit in
    the runner. The runner cannot simply adopt the returned tuple, because it needs to know which
    entry is the objective and which are limits — but it can, and does, report any name in that
    tuple it does not recognise, which is the failure the function exists to catch.
    """
    mod = _import_bench_metrics()
    if mod is None:
        return ["bench/metrics.py not importable; gate-metric agreement was not checked"]
    fn = getattr(mod, "select_gate_metrics", None)
    if not callable(fn):
        return ["bench/metrics.py exposes no select_gate_metrics(); agreement was not checked"]
    known = {alias for aliases in METRIC_ALIASES.values() for alias in aliases}
    unknown = [name for name in fn() if name not in known]
    if unknown:
        return [
            f"metrics.select_gate_metrics() names {unknown}, which this runner does not know how "
            f"to read. Either add it to METRIC_ALIASES or the gate it describes is not being "
            f"applied."
        ]
    return []

# Keys that may carry verbatim transcript text. plan.md §1.9 and .gitignore: per-cell files are
# gitignored, aggregates are committed, and an aggregate must never contain any of these.
TRANSCRIPT_KEYS = frozenset(
    {
        "text",
        "texts",
        "hypothesis",
        "hypotheses",
        "reference",
        "references",
        "transcript",
        "transcripts",
        "commit_trace",
        "hallucination_texts",
        "observations",
        "prompt",
    }
)


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# Thresholds — the pre-registered gates
# ═══════════════════════════════════════════════════════════════════════════════════════════════


@dataclass(frozen=True)
class Thresholds:
    """The gates of plan.md §4's selection rule.

    `source` records, per threshold, whether plan.md states the number or whether this project
    declared it. That distinction is not pedantry: plan.md §4 gates on "ICER below its declared
    threshold" without ever declaring one, and plan.md never states a VRAM limit at all. Those
    two defaults are ours. They must be fixed before the corpus is run — which is why they are
    here, with a name that says where they came from, rather than being chosen at report time.
    """

    first_partial_ms: float = 300.0
    finalisation_ms: float = 500.0
    icer_per_1000: float = 2.0
    peak_vram_mb: float = 24576.0

    # Not part of the pre-registered rule. Off by default. If set, the report labels every
    # elimination it causes as `extra_gate`, so the pre-registered rule stays legible underneath.
    max_hallucination_rate: float | None = None

    # Two F1 values closer than this are a tie and go to the tie-breakers (ICER, then latency).
    f1_tie_epsilon: float = 0.002

    def sources(self) -> dict[str, str]:
        return {
            "first_partial_ms": "plan.md §3 Phase 2 exit / §4",
            "finalisation_ms": "plan.md §3 Phase 2 exit / §4",
            "icer_per_1000": (
                "PROJECT-DECLARED — plan.md §4 gates on 'ICER below its declared threshold' "
                "but declares no number. Declared here, before the corpus is run."
            ),
            "peak_vram_mb": (
                "PROJECT-DECLARED — plan.md §4 lists a VRAM limit but states no figure. "
                "24 GiB leaves headroom on the 32 GB target card."
            ),
            "max_hallucination_rate": "NOT part of the pre-registered rule; extra gate, opt-in.",
            "f1_tie_epsilon": "PROJECT-DECLARED — the width of a tie for the tie-break ladder.",
        }

    def to_json(self) -> dict[str, Any]:
        d = {
            "first_partial_ms": self.first_partial_ms,
            "finalisation_ms": self.finalisation_ms,
            "icer_per_1000": self.icer_per_1000,
            "peak_vram_mb": self.peak_vram_mb,
            "max_hallucination_rate": self.max_hallucination_rate,
            "f1_tie_epsilon": self.f1_tie_epsilon,
        }
        d["_sources"] = self.sources()
        return d


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# Model catalogue — a transcription of bench/CANDIDATES.md §4, kept as data
# ═══════════════════════════════════════════════════════════════════════════════════════════════


@dataclass(frozen=True)
class ModelSpec:
    """One Phase-0A survivor.

    bench/CANDIDATES.md is the source of truth; this is a machine-readable transcription of its
    §4 table so the runner can reason about what each model can and cannot do. `--models-file`
    overrides it with JSON, so a later Phase-0A revision does not require editing code.
    """

    id: str
    label: str
    adapter: str
    env: str
    streaming_mode: str  # native-streaming | stateful-chunked | pseudo-streaming
    supports_context: bool
    context_kind: str  # none | hotwords | prompt
    published_chunk_ms: tuple[int, ...]  # () = chunk size is not a knob for this model
    phase2_eligible: bool
    note: str = ""

    def to_json(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "label": self.label,
            "adapter": self.adapter,
            "env": self.env,
            "streaming_mode": self.streaming_mode,
            "supports_context": self.supports_context,
            "context_kind": self.context_kind,
            "published_chunk_ms": list(self.published_chunk_ms),
            "phase2_eligible": self.phase2_eligible,
            "note": self.note,
        }


MODEL_CATALOGUE: tuple[ModelSpec, ...] = (
    ModelSpec(
        id="nemotron-streaming-en-0.6b-onnx",
        label="Nemotron Speech Streaming EN 0.6B (sherpa-onnx)",
        adapter="sherpa_onnx_streaming",
        env="envs/onnx",
        streaming_mode="native-streaming",
        # CANDIDATES.md §5.1: sherpa-onnx's Nemotron online transducer is greedy_search-only and
        # rejects modified_beam_search, so hotwords are unavailable. Recorded as False here so
        # the context-enabled arm never silently contains a model that ignores its context list.
        supports_context=False,
        context_kind="none",
        published_chunk_ms=(80, 160, 560, 1120),
        phase2_eligible=True,
        note="CANDIDATES.md §4 pick 1 — the streaming finalist candidate.",
    ),
    ModelSpec(
        id="nemotron-3.5-streaming-0.6b-onnx",
        label="Nemotron-3.5 ASR Streaming 0.6B (sherpa-onnx)",
        adapter="sherpa_onnx_streaming",
        env="envs/onnx",
        streaming_mode="native-streaming",
        supports_context=False,
        context_kind="none",
        published_chunk_ms=(80, 160, 320, 560, 1120),
        phase2_eligible=True,
        note="CANDIDATES.md §4 pick 2 — adds the 320 ms configuration the EN model lacks.",
    ),
    ModelSpec(
        id="streaming-zipformer-en-2023-06-26",
        label="Streaming zipformer EN (sherpa-onnx, hotwords)",
        adapter="zipformer_hotwords",
        env="envs/onnx",
        streaming_mode="native-streaming",
        supports_context=True,
        context_kind="hotwords",
        # The sherpa-onnx zipformer pages publish no chunk-size variants; the decode chunk is
        # baked into the exported model. Requesting a chunk sweep over it logs a coverage cap
        # rather than pretending the dimension exists.
        published_chunk_ms=(),
        phase2_eligible=True,
        note="CANDIDATES.md §4 pick 3 — the only natively-streaming context-biasing arm.",
    ),
    ModelSpec(
        id="whisper-large-v3-turbo",
        label="openai/whisper-large-v3-turbo via ..\\wan-dancer\\.venv (read-only)",
        adapter="whisper_transformers",
        env="wan-dancer/.venv (read-only, plan.md §5.1)",
        streaming_mode="pseudo-streaming",
        supports_context=True,
        context_kind="prompt",
        published_chunk_ms=(),
        phase2_eligible=False,
        note=(
            "CANDIDATES.md §4 pick 4 — zero-install baseline. Pseudo-streaming: per plan.md its "
            "latency is NOT comparable to the native-streaming rows and is reported separately."
        ),
    ),
    # Second tier — batch accuracy references, excluded from the Phase 2 streaming sweep by
    # CANDIDATES.md §4. Present so their accuracy numbers can be recorded in Phase 1.
    ModelSpec(
        id="qwen3-asr-1.7b",
        label="Qwen/Qwen3-ASR-1.7B-hf (batch only)",
        adapter="qwen3_asr",
        env="envs/qwen-asr",
        streaming_mode="stateful-chunked",
        supports_context=True,
        context_kind="prompt",
        published_chunk_ms=(),
        phase2_eligible=False,
        note="CANDIDATES.md §5.3 — streaming needs vLLM, unavailable on Windows. Batch reference.",
    ),
    ModelSpec(
        id="faster-whisper-large-v3",
        label="Systran/faster-whisper-large-v3 (CTranslate2)",
        adapter="faster_whisper",
        env="envs/ct2",
        streaming_mode="pseudo-streaming",
        supports_context=True,
        context_kind="prompt",
        published_chunk_ms=(),
        phase2_eligible=False,
        note="CANDIDATES.md §6.2 — unverified CUBLAS_STATUS_NOT_SUPPORTED report on Blackwell.",
    ),
    ModelSpec(
        id="parakeet-tdt-0.6b-v2",
        label="nvidia/parakeet-tdt-0.6b-v2 (offline)",
        adapter="sherpa_onnx_offline",
        env="envs/onnx",
        streaming_mode="pseudo-streaming",
        supports_context=True,
        context_kind="hotwords",
        published_chunk_ms=(),
        phase2_eligible=False,
        note="CANDIDATES.md §4 — chunking penalty already published; batch reference only.",
    ),
)

_CATALOGUE_BY_ID = {m.id: m for m in MODEL_CATALOGUE}

# The Phase 1 default model set: CANDIDATES.md §4's four picks. The second tier is opt-in via
# `--models`, because running it costs two multi-GB environments that §4 defers.
PHASE1_DEFAULT_MODELS = (
    "nemotron-streaming-en-0.6b-onnx",
    "nemotron-3.5-streaming-0.6b-onnx",
    "streaming-zipformer-en-2023-06-26",
    "whisper-large-v3-turbo",
)


def load_model_catalogue(path: Path | None) -> dict[str, ModelSpec]:
    """Return the model catalogue, optionally overridden from a JSON file."""
    if path is None:
        return dict(_CATALOGUE_BY_ID)
    raw = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(raw, list):
        raise ConfigError(f"{path}: expected a JSON list of model objects")
    out: dict[str, ModelSpec] = {}
    for item in raw:
        try:
            spec = ModelSpec(
                id=item["id"],
                label=item.get("label", item["id"]),
                adapter=item["adapter"],
                env=item.get("env", ""),
                streaming_mode=item["streaming_mode"],
                supports_context=bool(item.get("supports_context", False)),
                context_kind=item.get("context_kind", "none"),
                published_chunk_ms=tuple(int(c) for c in item.get("published_chunk_ms", ())),
                phase2_eligible=bool(item.get("phase2_eligible", False)),
                note=item.get("note", ""),
            )
        except KeyError as exc:  # a missing key here silently changes the experiment
            raise ConfigError(f"{path}: model entry missing required key {exc}") from exc
        out[spec.id] = spec
    if not out:
        raise ConfigError(f"{path}: no models defined")
    return out


class ConfigError(Exception):
    """A misconfiguration the runner refuses to guess its way past."""


class PhaseError(ConfigError):
    """An attempt to sweep a dimension the phase pins. See PhaseSpec."""


class GuardError(ConfigError):
    """A deliberate refusal: heldout without the flag, turbo replay without the flag, etc."""


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# Coverage caps — plan.md §4: "silent truncation reads as 'we tested everything'"
# ═══════════════════════════════════════════════════════════════════════════════════════════════


@dataclass(frozen=True)
class CoverageCap:
    """One place where the run covers less than the nominal grid.

    `kind` is a stable machine-readable slug; `detail` is prose for a human. Both the cell file
    and the run summary carry these, and `render_summary()` prints them in a block that is
    impossible to scroll past, because the failure mode being defended against is a cap that
    exists only in a log nobody reads.
    """

    kind: str
    scope: str
    detail: str

    def to_json(self) -> dict[str, str]:
        return {"kind": self.kind, "scope": self.scope, "detail": self.detail}

    def __str__(self) -> str:
        return f"[{self.kind}] {self.scope}: {self.detail}"


CAP_KINDS = (
    "corpus_sampling",  # --max-clips dropped clips from a category
    "chunk_substituted",  # requested chunk not published for this model
    "chunk_unsupported",  # model has no chunk knob at all
    "cell_dropped",  # a cell was excluded before running
    "cell_failed",  # a cell ran and failed
    "no_retry",  # a failure was not retried
    "max_cells",  # the grid was truncated by --max-cells
    "top_n",  # only the top N survivors carried forward
    "models_excluded",  # a model was excluded from a phase
    "categories_excluded",  # a category was excluded
    "repeats_reduced",  # fewer repeats than the variance target
    "metric_unavailable",  # a metric could not be computed
    "grapheme_approximation",  # grapheme counting fell back to code points
)


class CoverageLog:
    def __init__(self) -> None:
        self._caps: list[CoverageCap] = []

    def add(self, kind: str, scope: str, detail: str) -> None:
        if kind not in CAP_KINDS:  # a typo'd kind would hide a cap from the summary grouping
            raise ConfigError(f"unknown coverage-cap kind {kind!r}; add it to CAP_KINDS")
        cap = CoverageCap(kind, scope, detail)
        if cap not in self._caps:
            self._caps.append(cap)

    def extend(self, other: "CoverageLog") -> None:
        for cap in other:
            if cap not in self._caps:
                self._caps.append(cap)

    def __iter__(self) -> Iterator[CoverageCap]:
        return iter(self._caps)

    def __len__(self) -> int:
        return len(self._caps)

    def __bool__(self) -> bool:
        return bool(self._caps)

    def to_json(self) -> list[dict[str, str]]:
        return [c.to_json() for c in self._caps]


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# Cells
# ═══════════════════════════════════════════════════════════════════════════════════════════════


def slugify(text: str) -> str:
    s = re.sub(r"[^A-Za-z0-9]+", "-", text).strip("-").lower()
    return s or "x"


@dataclass(frozen=True)
class Cell:
    """One experimental condition. The unit of work, of caching, and of a result file.

    Everything that could change a number is a field, and every field is in the cell key, so a
    result file can never be ambiguous about what produced it.
    """

    phase: int
    model: str
    category: str
    split: str
    context_mode: str
    chunk_ms_requested: int | None
    chunk_ms_actual: int | None
    stability_n: int
    stability_m: int
    stability_t_ms: int
    cadence: str
    inject_method: str
    replay_speed: float  # 1.0 = real time. Anything else is loudly labelled.
    repeats: int

    def config(self) -> dict[str, Any]:
        """The canonical configuration. Hashed into the cell key; embedded in the result."""
        return {
            "phase": self.phase,
            "model": self.model,
            "category": self.category,
            "split": self.split,
            "context_mode": self.context_mode,
            "chunk_ms_requested": self.chunk_ms_requested,
            "chunk_ms_actual": self.chunk_ms_actual,
            "stability_n": self.stability_n,
            "stability_m": self.stability_m,
            "stability_t_ms": self.stability_t_ms,
            "cadence": self.cadence,
            "inject_method": self.inject_method,
            "replay_speed": self.replay_speed,
            "repeats": self.repeats,
        }

    def config_digest(self) -> str:
        blob = json.dumps(self.config(), sort_keys=True, separators=(",", ":"), ensure_ascii=False)
        return hashlib.sha256(blob.encode("utf-8")).hexdigest()

    @property
    def key(self) -> str:
        """Readable prefix + digest. The prefix is for humans doing `ls`; the digest is the
        identity. Two cells differing in any field get different keys, always."""
        ctx = "ctxon" if self.context_mode == "context_enabled" else "ctxoff"
        prefix = f"p{self.phase}_{slugify(self.model)}_{self.category}_{self.split}_{ctx}"
        return f"{prefix}_{self.config_digest()[:12]}"

    @property
    def configuration_key(self) -> str:
        """Identity of the *configuration* — the knob tuple — independent of corpus category.

        The selection rule scores configurations, not cells: a configuration is measured over
        several categories (technical-term F1 from `tech`, hallucination from `silence`/`noise`)
        and they must aggregate before the rule can be applied.
        """
        cfg = self.config()
        cfg.pop("category")
        blob = json.dumps(cfg, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
        digest = hashlib.sha256(blob.encode("utf-8")).hexdigest()[:12]
        ctx = "ctxon" if self.context_mode == "context_enabled" else "ctxoff"
        return f"p{self.phase}_{slugify(self.model)}_{self.split}_{ctx}_{digest}"

    def describe(self) -> str:
        bits = [
            f"model={self.model}",
            f"cat={self.category}",
            f"split={self.split}",
            f"ctx={self.context_mode}",
        ]
        if self.chunk_ms_requested is not None:
            actual = self.chunk_ms_actual
            bits.append(
                f"chunk={self.chunk_ms_requested}ms"
                + ("" if actual == self.chunk_ms_requested else f"(->{actual}ms)")
            )
        bits.append(f"stab=N{self.stability_n}/M{self.stability_m}/T{self.stability_t_ms}")
        bits.append(f"cadence={self.cadence}")
        bits.append(f"inject={self.inject_method}")
        if self.replay_speed != 1.0:
            bits.append(f"replay=x{self.replay_speed}!")
        return " ".join(bits)


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# Phases — the mechanism that stops a five-dimensional sweep
# ═══════════════════════════════════════════════════════════════════════════════════════════════

SWEEPABLE = ("model", "chunk_ms", "stability", "cadence", "inject_method", "context_mode")


@dataclass(frozen=True)
class PhaseSpec:
    """What a phase is allowed to vary.

    plan.md §4: *"Sequential elimination, not full cross-product: Phase 1 cuts on accuracy
    (model only), Phase 2 sweeps the rest on survivors."* This dataclass is that sentence made
    enforceable. `build_cells()` raises `PhaseError` when asked to sweep something the phase
    pins, which is the difference between a design and a hope.
    """

    phase: int
    name: str
    swept: frozenset[str]
    rationale: str

    def check(self, requested: Iterable[str]) -> None:
        bad = sorted(set(requested) - self.swept)
        if bad:
            raise PhaseError(
                f"phase {self.phase} ({self.name}) sweeps {sorted(self.swept)}; "
                f"refusing to sweep {bad}. {self.rationale} "
                f"If you really want this, run it as its own phase rather than widening this one."
            )


PHASES: dict[int, PhaseSpec] = {
    1: PhaseSpec(
        phase=1,
        name="accuracy cut",
        swept=frozenset({"model", "context_mode"}),
        rationale=(
            "Phase 1 cuts on accuracy with everything else pinned at plan.md §1.4 defaults, so "
            "a model is eliminated for being inaccurate and not for an unlucky chunk size."
        ),
    ),
    2: PhaseSpec(
        phase=2,
        name="streaming sweep",
        swept=frozenset({"model", "context_mode", "chunk_ms", "stability", "cadence", "inject_method"}),
        rationale=(
            "Phase 2 sweeps the streaming knobs, but only over the Phase 1 survivors — pass "
            "--models with the survivor list, or --survivors-from to read it from Phase 1."
        ),
    ),
}


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# Grid construction
# ═══════════════════════════════════════════════════════════════════════════════════════════════


@dataclass
class GridOptions:
    phase: int
    split: str
    models: tuple[str, ...]
    categories: tuple[str, ...] = CATEGORIES
    context_modes: tuple[str, ...] = CONTEXT_MODES
    chunk_ms: tuple[int, ...] = ()
    stability_n: tuple[int, ...] = (DEFAULT_STABILITY[0],)
    stability_m: tuple[int, ...] = (DEFAULT_STABILITY[1],)
    stability_t_ms: tuple[int, ...] = (DEFAULT_STABILITY[2],)
    cadences: tuple[str, ...] = (DEFAULT_CADENCE,)
    inject_methods: tuple[str, ...] = (DEFAULT_INJECT,)
    replay_speed: float = 1.0
    repeats: int = 3
    max_cells: int | None = None


def _requested_dimensions(opts: GridOptions) -> list[str]:
    """Which dimensions this request actually varies. Only a dimension with >1 value counts as
    swept; pinning chunk to a single value in Phase 1 is fine and is not a sweep."""
    req: list[str] = []
    if len(opts.models) > 1:
        req.append("model")
    if len(opts.context_modes) > 1:
        req.append("context_mode")
    if len(opts.chunk_ms) > 1:
        req.append("chunk_ms")
    if len(opts.stability_n) > 1 or len(opts.stability_m) > 1 or len(opts.stability_t_ms) > 1:
        req.append("stability")
    if len(opts.cadences) > 1:
        req.append("cadence")
    if len(opts.inject_methods) > 1:
        req.append("inject_method")
    return req


def resolve_chunk(
    spec: ModelSpec, requested: int | None, caps: CoverageLog
) -> int | None:
    """Map a requested chunk size onto a chunk size the model actually publishes.

    CANDIDATES.md §2: the sherpa-onnx Nemotron exports are one HF repo per chunk size, so
    `chunk_ms` is not a runtime parameter — it selects a different artifact. plan.md §4 asks for
    {80,160,320,640}; the EN model publishes {80,160,560,1120}. Substituting the nearest is the
    only thing that can be run, and logging the substitution is the only thing that keeps the
    resulting table honest.
    """
    if requested is None:
        return None
    if not spec.published_chunk_ms:
        caps.add(
            "chunk_unsupported",
            spec.id,
            f"chunk_ms={requested} requested but this model publishes no chunk variants "
            f"(streaming_mode={spec.streaming_mode}); the cell runs at the model's own chunk size "
            f"and chunk_ms is recorded as None.",
        )
        return None
    if requested in spec.published_chunk_ms:
        return requested
    nearest = min(spec.published_chunk_ms, key=lambda c: (abs(c - requested), c))
    caps.add(
        "chunk_substituted",
        spec.id,
        f"chunk_ms={requested} is not published for this model "
        f"(published: {list(spec.published_chunk_ms)}); substituted {nearest}. "
        f"Latency for this cell is the {nearest} ms figure, NOT a {requested} ms figure.",
    )
    return nearest


def build_cells(
    opts: GridOptions, catalogue: dict[str, ModelSpec] | None = None
) -> tuple[list[Cell], CoverageLog]:
    """Expand a grid request into cells, refusing anything the phase does not allow."""
    catalogue = catalogue or dict(_CATALOGUE_BY_ID)
    caps = CoverageLog()

    if opts.phase not in PHASES:
        raise ConfigError(f"unknown phase {opts.phase}; known phases: {sorted(PHASES)}")
    spec_phase = PHASES[opts.phase]
    spec_phase.check(_requested_dimensions(opts))

    if opts.split not in SPLITS:
        raise ConfigError(f"unknown split {opts.split!r}; expected one of {SPLITS}")
    for cat in opts.categories:
        if cat not in CATEGORIES:
            raise ConfigError(f"unknown corpus category {cat!r}; expected one of {CATEGORIES}")
    for mode in opts.context_modes:
        if mode not in CONTEXT_MODES:
            raise ConfigError(f"unknown context mode {mode!r}; expected one of {CONTEXT_MODES}")
    for cad in opts.cadences:
        if cad not in CADENCES:
            raise ConfigError(f"unknown cadence {cad!r}; expected one of {CADENCES}")
    for inj in opts.inject_methods:
        if inj not in INJECT_METHODS:
            raise ConfigError(f"unknown inject method {inj!r}; expected one of {INJECT_METHODS}")

    missing_cats = sorted(set(CATEGORIES) - set(opts.categories))
    if missing_cats:
        caps.add(
            "categories_excluded",
            f"phase {opts.phase}",
            f"corpus categories not run: {missing_cats}. "
            + (
                "Hallucination cells (silence/noise) are among them — the silence-hallucination "
                "check plan.md §1.6 requires did NOT run."
                if set(missing_cats) & set(HALLUCINATION_CATEGORIES)
                else ""
            ),
        )

    chunk_requests: tuple[int | None, ...] = opts.chunk_ms or (None,)

    cells: list[Cell] = []
    for model_id in opts.models:
        spec = catalogue.get(model_id)
        if spec is None:
            raise ConfigError(
                f"unknown model {model_id!r}; known: {sorted(catalogue)}. "
                f"bench/CANDIDATES.md §4 holds the survivor list."
            )
        if opts.phase == 2 and not spec.phase2_eligible:
            caps.add(
                "models_excluded",
                spec.id,
                f"excluded from the Phase 2 streaming sweep: {spec.note or 'batch reference only'}. "
                f"Its Phase 1 accuracy numbers stand; it has no streaming numbers.",
            )
            continue

        for ctx_mode in opts.context_modes:
            if ctx_mode == "context_enabled" and not spec.supports_context:
                # NOT a silent skip. CANDIDATES.md §5.1: the streaming finalist cannot be
                # context-biased at all. Recording that as a coverage cap is what stops the
                # context-enabled column from looking merely empty.
                caps.add(
                    "cell_dropped",
                    spec.id,
                    "no context-enabled cells: the backend reports supports_context=False "
                    "(CANDIDATES.md §5.1 — sherpa-onnx Nemotron is greedy_search-only, hotwords "
                    "require modified_beam_search). Accepting a context list and ignoring it "
                    "would corrupt plan.md §4's separate-mode comparison.",
                )
                continue

            for chunk_req in chunk_requests:
                chunk_actual = resolve_chunk(spec, chunk_req, caps)
                for n in opts.stability_n:
                    for m in opts.stability_m:
                        for t in opts.stability_t_ms:
                            for cad in opts.cadences:
                                for inj in opts.inject_methods:
                                    for cat in opts.categories:
                                        cells.append(
                                            Cell(
                                                phase=opts.phase,
                                                model=model_id,
                                                category=cat,
                                                split=opts.split,
                                                context_mode=ctx_mode,
                                                chunk_ms_requested=chunk_req,
                                                chunk_ms_actual=chunk_actual,
                                                stability_n=n,
                                                stability_m=m,
                                                stability_t_ms=t,
                                                cadence=cad,
                                                inject_method=inj,
                                                replay_speed=opts.replay_speed,
                                                repeats=opts.repeats,
                                            )
                                        )

    # Deduplicate: a chunk substitution can collapse two requested sizes onto one artifact
    # (640 -> 560 and 560 -> 560). Keeping both would double-count the same measurement.
    seen: set[str] = set()
    deduped: list[Cell] = []
    for cell in cells:
        if cell.key in seen:
            continue
        seen.add(cell.key)
        deduped.append(cell)
    if len(deduped) != len(cells):
        caps.add(
            "cell_dropped",
            f"phase {opts.phase}",
            f"{len(cells) - len(deduped)} duplicate cells collapsed — different requested chunk "
            f"sizes resolved to the same published model artifact.",
        )
    cells = deduped

    if opts.max_cells is not None and len(cells) > opts.max_cells:
        caps.add(
            "max_cells",
            f"phase {opts.phase}",
            f"grid is {len(cells)} cells, truncated to --max-cells={opts.max_cells}. "
            f"{len(cells) - opts.max_cells} cells were NOT run. This is a coverage cap, not a "
            f"complete matrix.",
        )
        cells = cells[: opts.max_cells]

    if opts.repeats < 3:
        caps.add(
            "repeats_reduced",
            f"phase {opts.phase}",
            f"repeats={opts.repeats}; plan.md §4 asks for run-to-run variance, which needs at "
            f"least 3 repeats to report a spread that means anything.",
        )

    return cells, caps


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# Corpus discovery and the dev/heldout guard
# ═══════════════════════════════════════════════════════════════════════════════════════════════


@dataclass(frozen=True)
class Clip:
    path: Path
    reference: str  # "" for silence/noise — any output at all is a hallucination
    clip_id: str
    duration_ms: float | None = None


def _read_reference(wav_path: Path) -> str:
    """Find the reference text next to an audio clip.

    bench/record.py owns the corpus metadata format and is written by another agent, so this
    accepts the plausible shapes rather than asserting one. If none is found the caller decides:
    for `silence`/`noise` an absent reference is correct (empty), for anything else it is an
    error, because scoring against a silently-empty reference produces a beautiful WER of 0.
    """
    stem = wav_path.with_suffix("")
    sidecar_json = Path(str(stem) + ".json")
    if sidecar_json.is_file():
        try:
            meta = json.loads(sidecar_json.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise ConfigError(f"{sidecar_json}: unreadable clip metadata: {exc}") from exc
        for key in ("reference", "text", "prompt", "prompt_text"):
            val = meta.get(key)
            if isinstance(val, str) and val.strip():
                return val
        return ""
    sidecar_txt = Path(str(stem) + ".txt")
    if sidecar_txt.is_file():
        return sidecar_txt.read_text(encoding="utf-8").strip()
    return ""


def discover_clips(
    corpus_root: Path,
    split: str,
    category: str,
    *,
    max_clips: int | None = None,
    seed: int = 0,
    caps: CoverageLog | None = None,
) -> list[Clip]:
    """List the 16 kHz clips for one (split, category).

    plan.md Phase 1 step 1 keeps the original-format capture alongside the 16 kHz conversion, so
    the benchmark reads only `*.16k.wav`: running the harness over the original-format file would
    silently benchmark a different sample rate and blame the model for it.
    """
    if split not in SPLITS:
        raise ConfigError(f"unknown split {split!r}")
    directory = corpus_root / split / category
    if not directory.is_dir():
        raise ConfigError(
            f"corpus directory not found: {directory}\n"
            f"  Expected layout: {corpus_root}/{{dev,heldout}}/{{{','.join(CATEGORIES)}}}/*.16k.wav\n"
            f"  Record it with bench/record.py; the corpus is gitignored (plan.md §1.9)."
        )
    files = sorted(p for p in directory.glob("*.16k.wav") if p.is_file())
    if not files:
        # Fall back to plain .wav but say so — a corpus with no 16 kHz conversion is a recorder
        # bug, and guessing quietly would hide it.
        files = sorted(p for p in directory.glob("*.wav") if p.is_file())
        if files and caps is not None:
            caps.add(
                "metric_unavailable",
                f"{split}/{category}",
                "no *.16k.wav found; fell back to *.wav. Sample rate is unverified and any "
                "resampling error is attributed to the model.",
            )
    if not files:
        raise ConfigError(f"no audio clips in {directory}")

    clips: list[Clip] = []
    for path in files:
        ref = _read_reference(path)
        if not ref and category not in HALLUCINATION_CATEGORIES:
            raise ConfigError(
                f"{path}: no reference text found (looked for a sibling .json with "
                f"reference/text/prompt, or a sibling .txt). Scoring against an empty reference "
                f"would report a perfect WER for any output at all."
            )
        clips.append(Clip(path=path, reference=ref, clip_id=path.name))

    if max_clips is not None and len(clips) > max_clips:
        rng = random.Random(f"{seed}:{split}:{category}")
        chosen = sorted(rng.sample(clips, max_clips), key=lambda c: c.clip_id)
        if caps is not None:
            caps.add(
                "corpus_sampling",
                f"{split}/{category}",
                f"sampled {max_clips} of {len(clips)} clips (seed={seed}). Metrics for this "
                f"category are over a {max_clips}/{len(clips)} subset, not the full corpus.",
            )
        clips = chosen
    return clips


def guard_split(split: str, *, heldout_decision: bool, purpose: str) -> None:
    """Refuse a heldout run that has not been declared as the decision run.

    plan.md Phase 1 step 2: *"heldout/ — the decision set. Touched once, at selection time. Never
    tuned on."* The flag exists so that touching it is a deliberate act with a name, rather than
    a `--split` typo that quietly turns the decision set into a tuning set.
    """
    if split != "heldout":
        return
    if not heldout_decision:
        raise GuardError(
            "REFUSING to run against heldout/.\n"
            "  heldout/ is the decision set (plan.md Phase 1 step 2): touched once, at selection\n"
            "  time, never tuned on. Tuning sweeps run against dev/.\n"
            f"  Purpose given: {purpose}\n"
            "  If this really is the selection run, pass --heldout-decision. Every heldout access\n"
            "  is appended to results/heldout-ledger.aggregate.json, so repeated 'selection runs'\n"
            "  are visible to anyone reading the results."
        )


def append_heldout_ledger(
    results_root: Path, *, purpose: str, cells: Sequence[Cell], code_version: dict[str, Any]
) -> Path:
    """Record a heldout access. Append-only, committed (aggregate, no transcript text).

    This is the audit trail that makes "touched once" checkable rather than merely asserted.
    """
    path = results_root / "heldout-ledger.aggregate.json"
    entries: list[dict[str, Any]] = []
    if path.is_file():
        try:
            loaded = json.loads(path.read_text(encoding="utf-8"))
            if isinstance(loaded, dict):
                entries = list(loaded.get("accesses", []))
            elif isinstance(loaded, list):
                entries = list(loaded)
        except ValueError:
            # A corrupt ledger must not silently reset to empty — that would erase the history
            # this file exists to keep.
            raise ConfigError(f"{path} is not valid JSON; refusing to overwrite the audit trail")
    entries.append(
        {
            "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "purpose": purpose,
            "n_cells": len(cells),
            "configurations": sorted({c.configuration_key for c in cells}),
            "models": sorted({c.model for c in cells}),
            "code_version": code_version,
        }
    )
    payload = {
        "schema_version": SCHEMA_VERSION,
        "note": (
            "Append-only record of every access to bench/corpus/heldout/. plan.md Phase 1 step 2 "
            "says the held-out set is touched once, at selection time. More than one entry here "
            "is not automatically wrong, but it is something a reader of the results is entitled "
            "to see."
        ),
        "accesses": entries,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    _atomic_write_json(path, payload)
    return path


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# Code version
# ═══════════════════════════════════════════════════════════════════════════════════════════════


def _git(*args: str) -> str | None:
    try:
        out = subprocess.run(
            ["git", *args],
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if out.returncode != 0:
        return None
    return out.stdout.strip()


def _file_digest(path: Path) -> str | None:
    try:
        return hashlib.sha256(path.read_bytes()).hexdigest()[:16]
    except OSError:
        return None


def code_version() -> dict[str, Any]:
    """What produced a result.

    A git SHA alone is not enough during development, when the tree is dirty for days at a time,
    so the digests of the two files that actually determine a number (this runner and the metric
    implementations) are recorded too. A result whose harness digest differs from the current one
    is reported in the summary rather than silently mixed in.
    """
    head = _git("rev-parse", "HEAD")
    dirty = _git("status", "--porcelain")
    # bench/metrics.py versions its normaliser, its technical-term ruleset and its grapheme
    # approximation separately, because a number produced under a different ruleset is not
    # comparable to one produced under this one. Carrying those strings in every result is what
    # turns a ruleset change from an invisible problem into a visible mismatch.
    metrics_versions: dict[str, str] | None = None
    mod = _import_bench_metrics()
    if mod is not None and callable(getattr(mod, "metric_versions", None)):
        try:
            metrics_versions = dict(mod.metric_versions())
        except Exception:  # noqa: BLE001 - version reporting must never break a run
            metrics_versions = None
    return {
        "metrics_versions": metrics_versions,
        "git_head": head,
        "git_dirty": bool(dirty) if dirty is not None else None,
        "harness": HARNESS_NAME,
        "run_matrix_sha256_16": _file_digest(Path(__file__).resolve()),
        "metrics_sha256_16": _file_digest(BENCH_DIR / "metrics.py"),
        "python": sys.version.split()[0],
        "platform": sys.platform,
    }


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# Real-time replay
# ═══════════════════════════════════════════════════════════════════════════════════════════════

BYTES_PER_SAMPLE = 2
SAMPLE_RATE = 16000


@dataclass(frozen=True)
class ReplayPolicy:
    """How fast audio is fed to the backend.

    plan.md Phase 2 step 2: audio is *"replayed at real-time cadence — not fed as fast as the GPU
    accepts, which flatters latency."* Real time is the default and the only mode whose latency
    numbers mean anything. Anything else must be asked for by name and is labelled everywhere it
    appears.
    """

    speed: float = 1.0
    frame_ms: int = 20  # docs/protocol.md §6.3 recommends 20–100 ms payloads

    @property
    def realtime(self) -> bool:
        return self.speed == 1.0

    @property
    def warning(self) -> str | None:
        if self.realtime:
            return None
        return (
            f"NOT REAL TIME (replay speed x{self.speed}). Latency figures from this run are not "
            f"comparable with real-time runs and MUST NOT be quoted as latency measurements."
        )

    def to_json(self) -> dict[str, Any]:
        return {
            "speed": self.speed,
            "realtime": self.realtime,
            "frame_ms": self.frame_ms,
            "warning": self.warning,
        }


def frame_schedule(total_bytes: int, policy: ReplayPolicy) -> list[tuple[int, int, float]]:
    """Slice PCM into frames and give each a due time, in ms from the start of the replay.

    Returns (byte_start, byte_end, due_ms). Pure and total — the pacing policy is testable
    without any audio, a clock, or a backend, which is the point of separating it from the
    executor.
    """
    if policy.speed <= 0:
        raise ConfigError("replay speed must be > 0")
    if total_bytes % BYTES_PER_SAMPLE:
        raise ConfigError(f"PCM length {total_bytes} is odd; a sample was split")
    frame_bytes = int(SAMPLE_RATE * policy.frame_ms / 1000) * BYTES_PER_SAMPLE
    if frame_bytes <= 0:
        raise ConfigError(f"frame_ms={policy.frame_ms} yields a zero-length frame")
    out: list[tuple[int, int, float]] = []
    pos = 0
    while pos < total_bytes:
        end = min(pos + frame_bytes, total_bytes)
        audio_ms = pos / (SAMPLE_RATE * BYTES_PER_SAMPLE) * 1000.0
        out.append((pos, end, audio_ms / policy.speed))
        pos = end
    return out


@dataclass
class PacingReport:
    """How well the replay kept to its schedule.

    `max_lateness_ms` is the interesting number: if the harness cannot hand frames to the backend
    on time, the "real-time" claim is false for that run and the latency figures are measuring
    the harness. It is reported rather than silently tolerated.
    """

    frames: int = 0
    max_lateness_ms: float = 0.0
    total_ms: float = 0.0

    def to_json(self) -> dict[str, Any]:
        return {
            "frames": self.frames,
            "max_lateness_ms": self.max_lateness_ms,
            "total_ms": self.total_ms,
        }


def paced_replay(
    pcm: bytes,
    policy: ReplayPolicy,
    send: Callable[[bytes, float], None],
    *,
    now: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> PacingReport:
    """Feed `pcm` to `send` at the policy's cadence.

    This is where "real-time replay" actually happens, and it is a free function with injected
    clock and sleep so it can be tested without waiting in real time. `send` receives
    (frame_bytes, audio_position_ms) — the audio position, not the wall clock, because that is
    what a latency decomposition needs to line an ASR output up against the input.

    Deliberately absolute rather than incremental scheduling: sleeping `frame_ms` per iteration
    accumulates the send cost into the timeline and drifts slow, which would make a run *appear*
    real time while actually feeding the model late.
    """
    schedule = frame_schedule(len(pcm), policy)
    report = PacingReport()
    t0 = now()
    for start, end, due_ms in schedule:
        delay = (t0 + due_ms / 1000.0) - now()
        if delay > 0:
            sleep(delay)
        else:
            report.max_lateness_ms = max(report.max_lateness_ms, -delay * 1000.0)
        audio_pos_ms = start / (SAMPLE_RATE * BYTES_PER_SAMPLE) * 1000.0
        send(pcm[start:end], audio_pos_ms)
        report.frames += 1
    report.total_ms = (now() - t0) * 1000.0
    return report


def read_wav_pcm(path: Path) -> tuple[bytes, int]:
    """Read a 16 kHz mono s16le WAV. Refuses anything else rather than resampling.

    Resampling here would put a second, undocumented resampler in the measurement path; plan.md
    Phase 1 step 1 keeps the original-format capture precisely so a resampling bug stays
    distinguishable from a backend bug.
    """
    with wave.open(str(path), "rb") as wf:
        n_ch, width, rate, n_frames = wf.getnchannels(), wf.getsampwidth(), wf.getframerate(), wf.getnframes()
        if (n_ch, width, rate) != (1, 2, SAMPLE_RATE):
            raise ConfigError(
                f"{path}: expected 16 kHz mono s16le, got {rate} Hz, {n_ch} ch, "
                f"{width * 8}-bit. The harness does not resample — record.py writes the "
                f"*.16k.wav conversion alongside the original capture."
            )
        return wf.readframes(n_frames), rate


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# Commit simulation — cadence is flush timing, never eligibility
# ═══════════════════════════════════════════════════════════════════════════════════════════════


def _load_grapheme_counter() -> tuple[Callable[[str], int], str]:
    """Grapheme counting must match the client's approximation (src/util/utf8.h).

    Preference order: bench/metrics.py (owns the metric definitions), then kvit_asr.protocol
    (written against the same utf8.h), then a code-point fallback that is *recorded as a coverage
    cap* — because counting code points instead of clusters silently inflates ICER on emoji and
    combining sequences, which are exactly the cases the boundary rules exist for.
    """
    for module_name, loader in (
        ("bench/metrics.py", _import_bench_metrics),
        ("kvit_asr.protocol", _import_kvit_protocol),
    ):
        mod = loader()
        if mod is None:
            continue
        fn = getattr(mod, "count_graphemes", None)
        if callable(fn):
            return fn, module_name
    return (lambda s: len(s)), "code-point fallback"


def _import_bench_metrics() -> Any:
    path = BENCH_DIR / "metrics.py"
    if not path.is_file():
        return None
    if str(BENCH_DIR) not in sys.path:
        sys.path.insert(0, str(BENCH_DIR))
    try:
        import metrics as _metrics  # type: ignore[import-not-found]
    except Exception:  # noqa: BLE001 - a broken sibling module must not break the runner
        return None
    return _metrics


def _import_kvit_protocol() -> Any:
    server_dir = REPO_ROOT / "server"
    if not (server_dir / "kvit_asr" / "protocol.py").is_file():
        return None
    if str(server_dir) not in sys.path:
        sys.path.insert(0, str(server_dir))
    try:
        from kvit_asr import protocol as _proto  # type: ignore[import-not-found]
    except Exception:  # noqa: BLE001
        return None
    return _proto


@dataclass(frozen=True)
class HypothesisEvent:
    """One `partial` or `final` as the client saw it."""

    t_ms: float
    utterance_id: int
    text: str
    stable_bytes: int
    is_final: bool = False


@dataclass(frozen=True)
class CommitEvent:
    """One irrevocable injection into the target document."""

    t_ms: float
    utterance_id: int
    byte_start: int
    byte_end: int
    text: str


@dataclass
class CommitTrace:
    commits: list[CommitEvent] = field(default_factory=list)
    committed_graphemes: int = 0
    irrevocable_error_graphemes: int = 0
    violations: list[str] = field(default_factory=list)
    grapheme_source: str = ""

    def icer_per_1000(self) -> float:
        # A zero-commit trace is not a division by zero and is not a perfect score: it is "no
        # committed text", reported as 0.0 with committed_graphemes=0 so a reader can see the
        # denominator.
        if self.committed_graphemes == 0:
            return 0.0
        return self.irrevocable_error_graphemes / self.committed_graphemes * 1000.0


class CommitSimulator:
    """The commit engine's observable behaviour, in Python, for the bench.

    Why this exists: ICER is defined over *committed* text (plan.md §1.4), and what gets
    committed depends on the cadence knob. Without simulating the commit, the cadence dimension
    of plan.md §4 would be swept without affecting any measured number.

    The invariant it enforces is plan.md §1.4's: eligibility comes from `stable_bytes` and
    nothing else; cadence can only *delay* a flush, never promote unstable text. Two failure
    modes are detected and counted rather than smoothed over:

    - the server sending a decreasing `stable_bytes`, or one whose prefix bytes differ from what
      it sent before — a `stability_violation` (docs/protocol.md §8.3);
    - committed bytes that are not a prefix of the final text — the irrevocable committed error
      that ICER measures.

    This is a *model* of the C++ commit engine (src/commit_engine.h), not that engine. It is
    accurate about eligibility and cadence and says nothing about injection timing inside the
    target application; the UI/commit-delay component of the latency decomposition is measured
    on the C++ side, not here.
    """

    def __init__(self, cadence: str, *, grapheme_counter: Callable[[str], int] | None = None,
                 grapheme_source: str = "") -> None:
        if cadence not in CADENCES:
            raise ConfigError(f"unknown cadence {cadence!r}")
        self.cadence_ms = CADENCE_MS[cadence]
        if grapheme_counter is None:
            grapheme_counter, grapheme_source = _load_grapheme_counter()
        self._count = grapheme_counter
        self.trace = CommitTrace(grapheme_source=grapheme_source)
        self._utt: int | None = None
        self._committed = b""       # bytes already injected for the current utterance
        self._eligible = b""        # stable prefix seen, not yet flushed
        self._last_stable_bytes = 0
        self._next_flush_ms = 0.0
        self._diverged = False

    def _start_utterance(self, utt: int, t_ms: float) -> None:
        self._utt = utt
        self._committed = b""
        self._eligible = b""
        self._last_stable_bytes = 0
        self._next_flush_ms = t_ms  # first flush is due immediately; cadence spaces the rest
        self._diverged = False

    def feed(self, ev: HypothesisEvent) -> list[CommitEvent]:
        if self._utt != ev.utterance_id:
            if self._utt is not None:
                self._close_utterance()
            self._start_utterance(ev.utterance_id, ev.t_ms)

        raw = ev.text.encode("utf-8")
        stable = ev.stable_bytes
        if stable < 0 or stable > len(raw):
            self.trace.violations.append(
                f"utt {ev.utterance_id} t={ev.t_ms:.0f}: stable_bytes={stable} out of range "
                f"for a {len(raw)}-byte text"
            )
            stable = max(0, min(stable, len(raw)))
        if stable < self._last_stable_bytes:
            self.trace.violations.append(
                f"utt {ev.utterance_id} t={ev.t_ms:.0f}: stable_bytes went backwards "
                f"{self._last_stable_bytes} -> {stable} (docs/protocol.md §8.3 stability_violation)"
            )
        # THE irrevocable-error event, plan.md §1.4: this hypothesis is not an extension of what
        # is already in the document. Committed text is never repaired and no backspaces are ever
        # sent, so the divergent bytes stay wrong forever — which is exactly what ICER counts.
        # Detected against `text`, not against `stable_bytes`, because a revision below the mark
        # is a revision whether or not the server admits it is one.
        if self._committed and not raw.startswith(self._committed) and not self._diverged:
            self._diverged = True
            common = 0
            for a, b in zip(self._committed, raw):
                if a != b:
                    break
                common += 1
            bad_text = self._committed[common:].decode("utf-8", errors="replace")
            n_bad = self._count(bad_text)
            self.trace.irrevocable_error_graphemes += n_bad
            self.trace.violations.append(
                f"utt {ev.utterance_id} t={ev.t_ms:.0f}: committed text diverges from the current "
                f"hypothesis at byte {common} ({n_bad} graphemes irrevocably wrong); no further "
                f"text is committed for this utterance"
            )

        new_eligible = raw[:stable]
        if not new_eligible.startswith(self._eligible[: min(len(self._eligible), len(new_eligible))]):
            self.trace.violations.append(
                f"utt {ev.utterance_id} t={ev.t_ms:.0f}: stable prefix changed under a previously "
                f"stable mark (prefix divergence)"
            )
        if len(new_eligible) >= len(self._eligible):
            self._eligible = new_eligible
        self._last_stable_bytes = max(self._last_stable_bytes, stable)

        emitted = [] if self._diverged else self._flush(ev.t_ms)
        if ev.is_final:
            if not self._diverged:
                # A final is entirely stable by definition (docs/protocol.md §6.2), so everything
                # becomes eligible and is flushed regardless of cadence: the utterance is over
                # and there is nothing left to delay for.
                self._eligible = raw
                emitted.extend(self._flush(ev.t_ms, force=True))
            self._close_utterance()
        return emitted

    def _flush(self, t_ms: float, *, force: bool = False) -> list[CommitEvent]:
        if len(self._eligible) <= len(self._committed):
            return []
        if not force and t_ms < self._next_flush_ms:
            return []
        chunk = self._eligible[len(self._committed):]
        try:
            text = chunk.decode("utf-8")
        except UnicodeDecodeError:
            # stable_bytes is required to land on a grapheme boundary; a mid-sequence value is a
            # server bug and would corrupt the document. Count it, do not inject it.
            self.trace.violations.append(
                f"utt {self._utt} t={t_ms:.0f}: stable_bytes split a UTF-8 sequence; commit refused"
            )
            return []
        ev = CommitEvent(
            t_ms=t_ms,
            utterance_id=self._utt or 0,
            byte_start=len(self._committed),
            byte_end=len(self._eligible),
            text=text,
        )
        self._committed = self._eligible
        self.trace.commits.append(ev)
        self.trace.committed_graphemes += self._count(text)
        if self.cadence_ms:
            self._next_flush_ms = t_ms + self.cadence_ms
        return [ev]

    def _close_utterance(self) -> None:
        """plan.md §1.4 step 7: reset entirely at an utterance boundary."""
        self._utt = None
        self._committed = b""
        self._eligible = b""
        self._last_stable_bytes = 0
        self._diverged = False

    def finish(self) -> CommitTrace:
        if self._utt is not None:
            self._close_utterance()
        return self.trace


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# Executors
# ═══════════════════════════════════════════════════════════════════════════════════════════════


@dataclass
class CellOutcome:
    """What running one cell produced."""

    status: str  # ok | failed | skipped
    metrics: dict[str, Any] = field(default_factory=dict)
    per_repeat: list[dict[str, Any]] = field(default_factory=list)
    observations: dict[str, Any] = field(default_factory=dict)  # may contain verbatim text
    caps: CoverageLog = field(default_factory=CoverageLog)
    error: str | None = None
    synthetic: bool = False
    executor: str = ""


class Executor:
    """Runs one cell over a list of clips.

    Deliberately narrow: the runner owns the grid, the guards, the caching and the rule, and an
    executor owns only "given this configuration and these clips, produce metrics". That is what
    lets the whole decision apparatus be tested without a model.
    """

    name = "abstract"
    synthetic = False

    def run_cell(self, cell: Cell, clips: Sequence[Clip], policy: ReplayPolicy) -> CellOutcome:
        raise NotImplementedError


class MockExecutor(Executor):
    """Deterministic synthetic numbers, for testing the harness itself.

    **Every result it writes is stamped `synthetic: true` and the selection rule refuses to
    select from synthetic results** unless `--allow-synthetic` is given. This class exists so the
    grid logic, resumability, coverage-cap reporting and the decision rule can be exercised on a
    machine with no models installed. It measures nothing.

    The numbers are a deterministic function of the cell key, with a mild, deliberate structure
    (smaller chunks -> lower latency and slightly worse F1; larger T -> lower ICER and higher
    latency) so that the rule has something shaped like a real trade-off to resolve.
    """

    name = "mock"
    synthetic = True

    def __init__(self, seed: int = 0) -> None:
        self.seed = seed

    def run_cell(self, cell: Cell, clips: Sequence[Clip], policy: ReplayPolicy) -> CellOutcome:
        rng = random.Random(f"{self.seed}:{cell.key}")
        chunk = cell.chunk_ms_actual or 160
        t = cell.stability_t_ms
        base_f1 = 0.70 + 0.04 * math.log2(max(chunk, 1) / 80.0) + (0.05 if cell.context_mode == "context_enabled" else 0.0)
        per_repeat: list[dict[str, Any]] = []
        for _ in range(max(1, cell.repeats)):
            per_repeat.append(
                {
                    "technical_term_f1": max(0.0, min(1.0, base_f1 + rng.gauss(0, 0.01))),
                    "first_partial_ms": chunk * 1.6 + t * 0.4 + rng.uniform(0, 30),
                    "finalisation_ms": 260 + chunk * 0.5 + rng.uniform(0, 60),
                    "icer_per_1000": max(0.0, 6.0 - t / 100.0 + rng.uniform(-0.5, 0.5)),
                    "peak_vram_mb": 2600 + rng.uniform(0, 100),
                    "wer_raw": max(0.0, 0.16 - 0.01 * math.log2(max(chunk, 1) / 80.0) + rng.uniform(0, 0.01)),
                    "wer_norm": max(0.0, 0.11 + rng.uniform(0, 0.01)),
                    "rtf": 0.3 + rng.uniform(0, 0.05),
                }
            )
        metrics = _reduce_repeats(per_repeat)
        metrics["n_clips"] = len(clips)
        if cell.category in HALLUCINATION_CATEGORIES:
            metrics["hallucination_rate"] = round(rng.random() * 0.2, 4)
        outcome = CellOutcome(
            status="ok",
            metrics=metrics,
            per_repeat=per_repeat,
            observations={"note": "synthetic — MockExecutor produced no transcripts"},
            error=None,
            synthetic=True,
            executor=self.name,
        )
        # Scope is the executor, not the cell: one cap for the whole run rather than one per
        # cell, so the summary's cap block stays readable. CoverageLog deduplicates identical
        # entries, which is what makes that work.
        outcome.caps.add(
            "metric_unavailable",
            "executor:mock",
            "MockExecutor: all metrics are SYNTHETIC. No audio was decoded and no model was run.",
        )
        return outcome


class SidecarExecutor(Executor):
    """Runs a cell against the real Python sidecar over a named pipe. **UNVERIFIED.**

    What is unverified, precisely, and why it is written anyway:

    - No ASR model is installed on this machine and none may be installed by this code
      (plan.md §1.9, CANDIDATES.md §3), so this path has never been executed end to end.
    - The bench takes the *Client* role from docs/protocol.md §2.2 — the side that creates the
      pipe — which in production is C++ (`src/ipc_client.cpp`). The `ctypes` calls below
      (`CreateNamedPipeW`, `ConnectNamedPipe`, `ReadFile`, `WriteFile`) are written from the
      documented signatures and have not been run. docs/protocol.md §14 already records that
      message-mode named-pipe I/O from `ctypes` is unverified.
    - The security descriptor from docs/protocol.md §2.3 is NOT built here. This is a benchmark
      harness on a developer machine, the pipe name is random per launch, and building a logon-SID
      DACL through `ctypes` would be a second unverified implementation of the thing the C++ side
      must get right. **Do not lift this class into the product.**

    On any mismatch it raises `ExecutorError` naming the call and the Win32 error, rather than
    surfacing an `AttributeError` from somewhere in `ctypes`.
    """

    name = "sidecar"
    synthetic = False

    def __init__(
        self,
        *,
        python_exe: str,
        models_config: Path | None = None,
        connect_timeout_ms: int = 15000,
        ready_timeout_ms: int = 180000,
    ) -> None:
        self.python_exe = python_exe
        self.models_config = models_config
        self.connect_timeout_ms = connect_timeout_ms
        self.ready_timeout_ms = ready_timeout_ms

    def run_cell(self, cell: Cell, clips: Sequence[Clip], policy: ReplayPolicy) -> CellOutcome:
        raise ExecutorError(
            "SidecarExecutor is not wired up on this machine.\n"
            "  It needs: (a) a backend environment under envs/ per bench/CANDIDATES.md §3, which\n"
            "      a human creates — this harness installs nothing and downloads nothing;\n"
            "  (b) the pipe-server half of docs/protocol.md §2.2, which lives in C++\n"
            "      (src/ipc_client.cpp) and which the bench must either drive or re-implement.\n"
            "  Until then, run with --executor mock, whose results are stamped synthetic and are\n"
            "  refused by the selection rule.\n"
            "  See the class docstring for exactly what is unverified."
        )


class ExecutorError(Exception):
    """An executor could not run a cell. Always names what was missing."""


def _reduce_repeats(per_repeat: Sequence[dict[str, Any]]) -> dict[str, Any]:
    """Median plus spread, per plan.md §4's run-to-run variance requirement.

    Median rather than mean: a single scheduling hiccup on Windows produces one enormous latency
    sample, and a mean would carry it into the decision.
    """
    if not per_repeat:
        return {}
    keys = sorted({k for r in per_repeat for k, v in r.items() if isinstance(v, (int, float))})
    out: dict[str, Any] = {}
    for k in keys:
        vals = [float(r[k]) for r in per_repeat if isinstance(r.get(k), (int, float))]
        if not vals:
            continue
        out[k] = statistics.median(vals)
        out[f"{k}__min"] = min(vals)
        out[f"{k}__max"] = max(vals)
        out[f"{k}__n"] = len(vals)
        out[f"{k}__stdev"] = statistics.pstdev(vals) if len(vals) > 1 else 0.0
    return out


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# Result store
# ═══════════════════════════════════════════════════════════════════════════════════════════════


def _atomic_write_json(path: Path, payload: Any) -> None:
    """Write via a temp file and replace, so an interrupted run cannot leave a half-written
    result that the next run happily treats as complete."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(
        json.dumps(payload, indent=2, sort_keys=True, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    os.replace(tmp, path)


def assert_no_verbatim(payload: Any, *, where: str) -> None:
    """Refuse to write transcript text into a committed aggregate.

    plan.md §1.9 and the repository .gitignore: per-cell result files are gitignored because they
    contain hypotheses; `*.aggregate.json` is committed and must contain metrics only. This is
    the check that keeps that promise mechanical instead of remembered.
    """

    def walk(node: Any, path: str) -> None:
        if isinstance(node, dict):
            for k, v in node.items():
                if k in TRANSCRIPT_KEYS:
                    raise ConfigError(
                        f"{where}: refusing to write key {path}.{k!r} into a committed aggregate — "
                        f"it may contain verbatim transcript text (plan.md §1.9)."
                    )
                walk(v, f"{path}.{k}")
        elif isinstance(node, list):
            for i, v in enumerate(node):
                walk(v, f"{path}[{i}]")

    walk(payload, "")


class ResultStore:
    """One JSON file per cell, under results/cells/. Gitignored; contains observations."""

    def __init__(self, results_root: Path) -> None:
        self.root = results_root
        self.cells_dir = results_root / "cells"

    def path_for(self, cell: Cell) -> Path:
        return self.cells_dir / f"{cell.key}.json"

    def has(self, cell: Cell) -> bool:
        return self.path_for(cell).is_file()

    def load(self, cell: Cell) -> dict[str, Any] | None:
        path = self.path_for(cell)
        if not path.is_file():
            return None
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except ValueError as exc:
            raise ConfigError(
                f"{path}: corrupt result file ({exc}). Delete it or re-run with --force; the "
                f"runner will not guess what a truncated result meant."
            ) from exc

    def load_all(self, *, phase: int | None = None, split: str | None = None) -> list[dict[str, Any]]:
        out: list[dict[str, Any]] = []
        if not self.cells_dir.is_dir():
            return out
        for path in sorted(self.cells_dir.glob("*.json")):
            try:
                rec = json.loads(path.read_text(encoding="utf-8"))
            except ValueError:
                continue
            cfg = rec.get("config", {})
            if phase is not None and cfg.get("phase") != phase:
                continue
            if split is not None and cfg.get("split") != split:
                continue
            out.append(rec)
        return out

    def write(
        self,
        cell: Cell,
        outcome: CellOutcome,
        *,
        policy: ReplayPolicy,
        clips: Sequence[Clip],
        version: dict[str, Any],
        purpose: str,
    ) -> Path:
        payload = {
            "schema_version": SCHEMA_VERSION,
            "cell_key": cell.key,
            "configuration_key": cell.configuration_key,
            "config": cell.config(),
            "config_digest": cell.config_digest(),
            "split": cell.split,  # duplicated out of config: the split is the discipline, and a
                                  # reader must not have to dig for it
            "purpose": purpose,
            "status": outcome.status,
            "error": outcome.error,
            "synthetic": outcome.synthetic,
            "executor": outcome.executor,
            "replay": policy.to_json(),
            "code_version": version,
            "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "clips": {
                "n": len(clips),
                "ids": [c.clip_id for c in clips],
            },
            "metrics": outcome.metrics,
            "per_repeat": outcome.per_repeat,
            "coverage_caps": outcome.caps.to_json(),
            "observations": outcome.observations,
        }
        path = self.path_for(cell)
        _atomic_write_json(path, payload)
        return path


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# Aggregation over categories, then the selection rule
# ═══════════════════════════════════════════════════════════════════════════════════════════════


@dataclass
class ConfigAggregate:
    """One configuration's numbers, aggregated across corpus categories."""

    configuration_key: str
    context_mode: str
    split: str
    model: str
    config: dict[str, Any]
    metrics: dict[str, float] = field(default_factory=dict)
    missing: list[str] = field(default_factory=list)
    synthetic: bool = False
    n_cells: int = 0
    code_versions: set[str] = field(default_factory=set)
    caps: list[dict[str, str]] = field(default_factory=list)

    def describe(self) -> str:
        c = self.config
        chunk = c.get("chunk_ms_actual")
        return (
            f"{self.model} chunk={chunk} "
            f"N{c.get('stability_n')}/M{c.get('stability_m')}/T{c.get('stability_t_ms')} "
            f"cadence={c.get('cadence')} inject={c.get('inject_method')}"
        )


def aggregate_results(records: Sequence[dict[str, Any]]) -> list[ConfigAggregate]:
    """Fold per-cell results into per-configuration aggregates.

    Aggregation rules, stated because each is a judgement call that changes the answer:

    - technical-term F1 is taken from F1_CATEGORIES ('tech') only. Averaging in 'clean', which
      contains no technical vocabulary, would dilute the very quantity the rule maximises.
    - ICER is pooled by its true denominator (error graphemes / committed graphemes) when both
      counts are present, and falls back to the mean of per-cell rates when they are not — the
      fallback is recorded, because a mean of rates is not a rate.
    - latency is the median of per-cell medians. peak VRAM is the max: a configuration that fits
      in memory on four categories and not on the fifth does not fit.
    """
    by_config: dict[str, list[dict[str, Any]]] = {}
    for rec in records:
        if rec.get("status") != "ok":
            continue
        by_config.setdefault(rec.get("configuration_key", "?"), []).append(rec)

    aggregates: list[ConfigAggregate] = []
    for config_key, recs in sorted(by_config.items()):
        first_cfg = dict(recs[0].get("config", {}))
        first_cfg.pop("category", None)
        agg = ConfigAggregate(
            configuration_key=config_key,
            context_mode=first_cfg.get("context_mode", "context_free"),
            split=first_cfg.get("split", "dev"),
            model=first_cfg.get("model", "?"),
            config=first_cfg,
            n_cells=len(recs),
            synthetic=any(bool(r.get("synthetic")) for r in recs),
        )
        for r in recs:
            cv = r.get("code_version", {})
            agg.code_versions.add(str(cv.get("run_matrix_sha256_16")))
            agg.caps.extend(r.get("coverage_caps", []))

        def cell_metrics(pred: Callable[[dict[str, Any]], bool]) -> list[dict[str, Any]]:
            return [r.get("metrics", {}) for r in recs if pred(r.get("config", {}))]

        f1_cells = cell_metrics(lambda c: c.get("category") in F1_CATEGORIES)
        f1_vals = [v for v in (metric_value(m, "technical_term_f1") for m in f1_cells) if v is not None]
        if f1_vals:
            agg.metrics["technical_term_f1"] = statistics.fmean(f1_vals)
        else:
            agg.missing.append("technical_term_f1")

        scored = cell_metrics(lambda c: c.get("category") not in HALLUCINATION_CATEGORIES)
        err = [metric_value(m, "icer_error_graphemes") for m in scored]
        com = [metric_value(m, "icer_committed_graphemes") for m in scored]
        if scored and all(e is not None for e in err) and all(c is not None for c in com):
            total_com = float(sum(com))  # type: ignore[arg-type]
            agg.metrics["icer_per_1000"] = (
                float(sum(err)) / total_com * 1000.0 if total_com else 0.0  # type: ignore[arg-type]
            )
            agg.metrics["icer_committed_graphemes"] = total_com
        else:
            rates = [v for v in (metric_value(m, "icer_per_1000") for m in scored) if v is not None]
            if rates:
                agg.metrics["icer_per_1000"] = statistics.fmean(rates)
                agg.caps.append(
                    {
                        "kind": "metric_unavailable",
                        "scope": config_key,
                        "detail": (
                            "ICER pooled as the mean of per-cell rates because the grapheme "
                            "counts were absent; a mean of rates is not a rate and differs from "
                            "the true pooled figure when cells differ in length."
                        ),
                    }
                )
            else:
                agg.missing.append("icer_per_1000")

        for key, reducer in (
            ("first_partial_ms", statistics.median),
            ("finalisation_ms", statistics.median),
            ("peak_vram_mb", max),
        ):
            vals = [v for v in (metric_value(m, key) for m in scored) if v is not None]
            if vals:
                agg.metrics[key] = float(reducer(vals))
            else:
                agg.missing.append(key)

        # metrics.select_gate_metrics() nominates `latency_median_ms` as the second tie-break.
        # It is used when present; when it is absent the tie falls through to first_partial then
        # finalisation, which is the same ordering expressed in the metrics the runner does have.
        for key in (
            "latency_median_ms", "wer_raw", "wer_norm", "cer_raw", "cer_norm", "rtf",
            "flicker_per_1000",
        ):
            vals = [v for v in (metric_value(m, key) for m in scored) if v is not None]
            if vals:
                agg.metrics[key] = statistics.fmean(vals)

        hall = cell_metrics(lambda c: c.get("category") in HALLUCINATION_CATEGORIES)
        hall_vals = [v for v in (metric_value(m, "hallucination_rate") for m in hall) if v is not None]
        if hall_vals:
            agg.metrics["hallucination_rate"] = max(hall_vals)

        aggregates.append(agg)
    return aggregates


@dataclass(frozen=True)
class Elimination:
    configuration_key: str
    description: str
    reasons: tuple[str, ...]
    technical_term_f1: float | None


@dataclass
class SelectionOutcome:
    context_mode: str
    split: str
    winner: ConfigAggregate | None
    ranked: list[ConfigAggregate]
    eliminated: list[Elimination]
    notes: list[str] = field(default_factory=list)
    thresholds: Thresholds = field(default_factory=Thresholds)


def apply_selection_rule(
    aggregates: Sequence[ConfigAggregate],
    thresholds: Thresholds,
    *,
    context_mode: str,
    split: str,
    allow_synthetic: bool = False,
) -> SelectionOutcome:
    """plan.md §4, as code.

        "maximise held-out technical-term F1 among configurations that meet the first-partial,
         finalisation, ICER, and VRAM limits. Ties break on ICER, then latency."

    Every configuration that does not win leaves with a reason attached. The three interesting
    cases are handled explicitly and reported rather than absorbed:

    - a configuration missing a gating metric is eliminated as `missing_metric:<key>`, not
      dropped — an untested configuration must not be indistinguishable from a losing one;
    - if the highest-F1 configuration fails a gate, the outcome says so by name and records that
      the winner is the runner-up;
    - synthetic (mock) results are refused outright unless explicitly allowed, because the whole
      point of a pre-registered rule is that its inputs are measurements.
    """
    pool = [a for a in aggregates if a.context_mode == context_mode and a.split == split]
    outcome = SelectionOutcome(
        context_mode=context_mode, split=split, winner=None, ranked=[], eliminated=[],
        thresholds=thresholds,
    )

    if split != "heldout":
        outcome.notes.append(
            f"NOT A DECISION: the pre-registered rule decides on the held-out set; this ran "
            f"against split={split!r}. Treat the ordering as tuning information only."
        )

    eligible: list[ConfigAggregate] = []
    for agg in pool:
        reasons: list[str] = []
        if agg.synthetic and not allow_synthetic:
            reasons.append(
                "synthetic: produced by MockExecutor, which measures nothing. Pass "
                "--allow-synthetic only to exercise the rule itself."
            )
        for key in SELECTION_METRIC_KEYS:
            if key in agg.missing or key not in agg.metrics:
                reasons.append(f"missing_metric:{key} — the gate could not be evaluated")
        if not reasons:
            fp = agg.metrics["first_partial_ms"]
            fn = agg.metrics["finalisation_ms"]
            ic = agg.metrics["icer_per_1000"]
            vr = agg.metrics["peak_vram_mb"]
            if fp > thresholds.first_partial_ms:
                reasons.append(
                    f"first_partial_ms {fp:.0f} > {thresholds.first_partial_ms:.0f} limit"
                )
            if fn > thresholds.finalisation_ms:
                reasons.append(
                    f"finalisation_ms {fn:.0f} > {thresholds.finalisation_ms:.0f} limit"
                )
            if ic > thresholds.icer_per_1000:
                reasons.append(
                    f"icer_per_1000 {ic:.2f} > {thresholds.icer_per_1000:.2f} limit "
                    f"(the headline safety number, plan.md §1.4)"
                )
            if vr > thresholds.peak_vram_mb:
                reasons.append(f"peak_vram_mb {vr:.0f} > {thresholds.peak_vram_mb:.0f} limit")
            if thresholds.max_hallucination_rate is not None:
                hr = agg.metrics.get("hallucination_rate")
                if isinstance(hr, (int, float)) and hr > thresholds.max_hallucination_rate:
                    reasons.append(
                        f"extra_gate: hallucination_rate {hr:.3f} > "
                        f"{thresholds.max_hallucination_rate:.3f} (NOT part of the pre-registered "
                        f"rule; enabled by configuration)"
                    )
        if reasons:
            outcome.eliminated.append(
                Elimination(
                    configuration_key=agg.configuration_key,
                    description=agg.describe(),
                    reasons=tuple(reasons),
                    technical_term_f1=agg.metrics.get("technical_term_f1"),
                )
            )
        else:
            eligible.append(agg)

    def sort_key(a: ConfigAggregate) -> tuple[float, float, float, float, float, str]:
        # F1 descending, then ICER ascending, then latency ascending, then the configuration key
        # so the order is total and reproducible rather than dependent on dict iteration.
        # `latency_median_ms` is metrics.select_gate_metrics()'s nominated latency tie-break; it
        # leads, and first_partial/finalisation follow for results that do not carry it.
        return (
            -a.metrics["technical_term_f1"],
            a.metrics["icer_per_1000"],
            a.metrics.get("latency_median_ms", math.inf),
            a.metrics["first_partial_ms"],
            a.metrics["finalisation_ms"],
            a.configuration_key,
        )

    eligible.sort(key=sort_key)
    outcome.ranked = eligible

    if eligible:
        best_f1 = eligible[0].metrics["technical_term_f1"]
        tied = [a for a in eligible if best_f1 - a.metrics["technical_term_f1"] <= thresholds.f1_tie_epsilon]
        if len(tied) > 1:
            outcome.notes.append(
                f"{len(tied)} configurations tied on technical-term F1 within "
                f"{thresholds.f1_tie_epsilon} — broken on ICER, then latency, as pre-registered."
            )
        outcome.winner = eligible[0]

    # The runner-up case, called out by name. Without this a reader sees a winner and no sign
    # that a better-scoring configuration existed and was disqualified.
    all_with_f1 = [
        a for a in pool if isinstance(a.metrics.get("technical_term_f1"), (int, float))
    ]
    if all_with_f1:
        overall_best = max(all_with_f1, key=lambda a: a.metrics["technical_term_f1"])
        if outcome.winner is None or overall_best.configuration_key != outcome.winner.configuration_key:
            elim = next(
                (e for e in outcome.eliminated if e.configuration_key == overall_best.configuration_key),
                None,
            )
            if elim is not None:
                outcome.notes.append(
                    f"RUNNER-UP SELECTED: the highest technical-term F1 "
                    f"({overall_best.metrics['technical_term_f1']:.4f}, {overall_best.describe()}) "
                    f"was ELIMINATED — {'; '.join(elim.reasons)}. "
                    + (
                        f"The winner is the highest-F1 configuration that passes every gate "
                        f"({outcome.winner.metrics['technical_term_f1']:.4f})."
                        if outcome.winner is not None
                        else "No configuration passed every gate, so there is no winner."
                    )
                )
    if outcome.winner is None:
        if not pool:
            # An empty arm and a wiped-out arm are different facts and must not read the same.
            # The usual cause of an empty context-enabled arm is CANDIDATES.md §5.1 — no
            # candidate that can be context-biased — and that is recorded as a `cell_dropped`
            # coverage cap, not as a defeat.
            outcome.notes.append(
                f"NO CONFIGURATIONS were measured for context_mode={context_mode!r} on split "
                f"{split!r}. This arm is empty, not defeated — check the coverage caps; a "
                f"backend reporting supports_context=false produces no context-enabled cells "
                f"at all (CANDIDATES.md §5.1)."
            )
        else:
            outcome.notes.append(
                "NO WINNER: every configuration failed at least one gate. The pre-registered rule "
                "does not permit relaxing a threshold to produce one — either fix the "
                "configurations or re-declare the thresholds explicitly, before looking at the "
                "results again."
            )
    return outcome


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# Reporting
# ═══════════════════════════════════════════════════════════════════════════════════════════════


def render_selection_report(outcomes: Sequence[SelectionOutcome]) -> str:
    lines: list[str] = []
    lines.append("=" * 92)
    lines.append("SELECTION RULE — plan.md §4, pre-registered before the corpus was run")
    lines.append("  maximise technical-term F1 among configurations meeting the first-partial,")
    lines.append("  finalisation, ICER and VRAM limits; ties break on ICER, then latency.")
    lines.append("=" * 92)
    for out in outcomes:
        th = out.thresholds
        lines.append("")
        lines.append(f"── context mode: {out.context_mode}   split: {out.split} ".ljust(92, "─"))
        lines.append(
            f"   gates: first_partial<={th.first_partial_ms:.0f}ms  "
            f"finalisation<={th.finalisation_ms:.0f}ms  "
            f"ICER<={th.icer_per_1000:.2f}/1000  VRAM<={th.peak_vram_mb:.0f}MB"
        )
        lines.append(
            "   (context modes are scored separately and never pooled — plan.md §4; "
            "CANDIDATES.md §5.1)"
        )
        if out.winner is not None:
            w = out.winner
            lines.append("")
            lines.append(f"   WINNER  {w.describe()}")
            lines.append(f"           key={w.configuration_key}")
            lines.append(
                f"           F1={w.metrics['technical_term_f1']:.4f}  "
                f"ICER={w.metrics['icer_per_1000']:.2f}/1000  "
                f"first_partial={w.metrics['first_partial_ms']:.0f}ms  "
                f"finalisation={w.metrics['finalisation_ms']:.0f}ms  "
                f"VRAM={w.metrics['peak_vram_mb']:.0f}MB"
            )
        else:
            lines.append("")
            lines.append("   WINNER  (none — see notes)")
        if len(out.ranked) > 1:
            lines.append("")
            lines.append("   Ranking of configurations that passed every gate:")
            for i, a in enumerate(out.ranked, 1):
                lines.append(
                    f"     {i:2d}. F1={a.metrics['technical_term_f1']:.4f} "
                    f"ICER={a.metrics['icer_per_1000']:.2f} "
                    f"fp={a.metrics['first_partial_ms']:.0f}ms  {a.describe()}"
                )
        lines.append("")
        lines.append(f"   ELIMINATED ({len(out.eliminated)}):")
        if not out.eliminated:
            lines.append("     (none)")
        for e in sorted(
            out.eliminated, key=lambda x: (-(x.technical_term_f1 or -1.0), x.configuration_key)
        ):
            f1 = f"{e.technical_term_f1:.4f}" if e.technical_term_f1 is not None else "  n/a "
            lines.append(f"     F1={f1}  {e.description}")
            lines.append(f"          key={e.configuration_key}")
            for r in e.reasons:
                lines.append(f"          × {r}")
        for note in out.notes:
            lines.append("")
            for i, chunk in enumerate(_wrap(note, 86)):
                lines.append(("   NOTE: " if i == 0 else "         ") + chunk)
    lines.append("")
    return "\n".join(lines)


def _wrap(text: str, width: int) -> list[str]:
    words, out, cur = text.split(), [], ""
    for w in words:
        if cur and len(cur) + 1 + len(w) > width:
            out.append(cur)
            cur = w
        else:
            cur = f"{cur} {w}" if cur else w
    if cur:
        out.append(cur)
    return out or [""]


def render_summary(
    *,
    phase: int,
    split: str,
    cells: Sequence[Cell],
    ran: int,
    skipped: int,
    failed: int,
    caps: CoverageLog,
    policy: ReplayPolicy,
    executor_name: str,
    synthetic: bool,
    version_mismatches: int = 0,
) -> str:
    lines: list[str] = []
    lines.append("=" * 92)
    lines.append(f"RUN SUMMARY — phase {phase} ({PHASES[phase].name}), split {split}")
    lines.append("=" * 92)
    lines.append(f"  cells in grid : {len(cells)}")
    lines.append(f"  ran           : {ran}")
    lines.append(f"  skipped (had a result already) : {skipped}")
    lines.append(f"  failed        : {failed}")
    lines.append(f"  executor      : {executor_name}" + ("   ** SYNTHETIC **" if synthetic else ""))
    lines.append(
        f"  replay        : {'REAL TIME (x1.0)' if policy.realtime else f'x{policy.speed}'}"
        f"  frame={policy.frame_ms}ms"
    )
    if policy.warning:
        lines.append("")
        lines.append("  " + "!" * 86)
        for chunk in _wrap(policy.warning, 84):
            lines.append(f"  ! {chunk}")
        lines.append("  " + "!" * 86)
    if synthetic:
        lines.append("")
        lines.append("  " + "!" * 86)
        lines.append("  ! SYNTHETIC RUN. No audio was decoded. These are not measurements, and the")
        lines.append("  ! selection rule refuses them unless --allow-synthetic is passed.")
        lines.append("  " + "!" * 86)
    if version_mismatches:
        lines.append("")
        lines.append(
            f"  {version_mismatches} reused result(s) were produced by a different harness "
            f"version. Re-run with --force to make the table homogeneous."
        )
    lines.append("")
    lines.append(f"  COVERAGE CAPS ({len(caps)}) — plan.md §4: silent truncation reads as")
    lines.append("  'we tested everything'. Every cap below is a place this run covered less")
    lines.append("  than the nominal grid.")
    if not len(caps):
        lines.append("    (none — the full requested grid ran)")
    else:
        by_kind: dict[str, list[CoverageCap]] = {}
        for cap in caps:
            by_kind.setdefault(cap.kind, []).append(cap)
        for kind in sorted(by_kind):
            lines.append(f"    [{kind}]")
            for cap in by_kind[kind]:
                lines.append(f"      · {cap.scope}")
                for chunk in _wrap(cap.detail, 76):
                    lines.append(f"          {chunk}")
    lines.append("")
    return "\n".join(lines)


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# The run loop
# ═══════════════════════════════════════════════════════════════════════════════════════════════


@dataclass
class RunOptions:
    results_root: Path = DEFAULT_RESULTS_ROOT
    corpus_root: Path = DEFAULT_CORPUS_ROOT
    force: bool = False
    retries: int = 0
    max_clips: int | None = None
    sample_seed: int = 0
    purpose: str = "tuning"
    heldout_decision: bool = False


def run_cells(
    cells: Sequence[Cell],
    *,
    executor: Executor,
    policy: ReplayPolicy,
    grid_caps: CoverageLog,
    opts: RunOptions,
    log: Callable[[str], None] = lambda s: print(s, file=sys.stderr),
) -> dict[str, Any]:
    """Run (or skip) every cell, writing one result file each. Resumable by construction."""
    store = ResultStore(opts.results_root)
    version = code_version()
    caps = CoverageLog()
    caps.extend(grid_caps)
    ran = skipped = failed = version_mismatches = 0

    clip_cache: dict[tuple[str, str], list[Clip]] = {}

    for i, cell in enumerate(cells, 1):
        existing = store.load(cell) if not opts.force else None
        if existing is not None and existing.get("status") == "ok":
            skipped += 1
            prev = (existing.get("code_version") or {}).get("run_matrix_sha256_16")
            if prev != version.get("run_matrix_sha256_16"):
                version_mismatches += 1
            log(f"[{i}/{len(cells)}] skip (result exists) {cell.key}")
            continue

        try:
            key = (cell.split, cell.category)
            if key not in clip_cache:
                clip_cache[key] = discover_clips(
                    opts.corpus_root,
                    cell.split,
                    cell.category,
                    max_clips=opts.max_clips,
                    seed=opts.sample_seed,
                    caps=caps,
                )
            clips = clip_cache[key]
        except ConfigError as exc:
            failed += 1
            caps.add("cell_failed", cell.key, f"corpus unavailable: {exc}")
            corpus_fail = CellOutcome(status="failed", error=str(exc), executor=executor.name)
            store.write(
                cell, corpus_fail, policy=policy, clips=[], version=version, purpose=opts.purpose
            )
            log(f"[{i}/{len(cells)}] FAIL {cell.key}: {exc}")
            continue

        log(f"[{i}/{len(cells)}] run  {cell.describe()}")
        outcome: CellOutcome | None = None
        last_err: str | None = None
        for attempt in range(opts.retries + 1):
            try:
                outcome = executor.run_cell(cell, clips, policy)
                break
            except Exception as exc:  # noqa: BLE001 - a backend may raise anything at all
                last_err = f"{type(exc).__name__}: {exc}"
                if attempt < opts.retries:
                    log(f"        attempt {attempt + 1} failed ({last_err}); retrying")
        if outcome is None:
            failed += 1
            caps.add("cell_failed", cell.key, f"executor failed: {last_err}")
            if opts.retries == 0:
                caps.add(
                    "no_retry",
                    cell.key,
                    "the cell failed and was not retried (--retries 0). Its metrics are absent "
                    "from the table and the configuration is eliminated as missing_metric, not "
                    "as a loser.",
                )
            outcome = CellOutcome(status="failed", error=last_err, executor=executor.name)
        else:
            ran += 1
        caps.extend(outcome.caps)
        store.write(
            cell, outcome, policy=policy, clips=clips, version=version, purpose=opts.purpose
        )

    return {
        "cells": list(cells),
        "ran": ran,
        "skipped": skipped,
        "failed": failed,
        "caps": caps,
        "version_mismatches": version_mismatches,
        "version": version,
    }


def write_phase_aggregate(
    results_root: Path,
    *,
    phase: int,
    split: str,
    records: Sequence[dict[str, Any]],
    caps: CoverageLog,
    outcomes: Sequence[SelectionOutcome],
    thresholds: Thresholds,
    policy: ReplayPolicy,
) -> Path:
    """Write results/phaseN.aggregate.json — committed, metrics only, no transcript text.

    plan.md Phase 1 exit names `bench/results/phase1.json`; the `.aggregate.json` suffix is what
    the repository .gitignore uses to distinguish a committable aggregate from a transcript-
    bearing cell file, so the file takes that name.
    """
    aggregates = aggregate_results(records)
    payload: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "phase": phase,
        "phase_name": PHASES[phase].name,
        "split": split,
        "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "code_version": code_version(),
        "thresholds": thresholds.to_json(),
        "replay": policy.to_json(),
        "coverage_caps": caps.to_json(),
        "n_cells": len(records),
        "any_synthetic": any(bool(r.get("synthetic")) for r in records),
        "configurations": [
            {
                "configuration_key": a.configuration_key,
                "config": a.config,
                "metrics": a.metrics,
                "missing_metrics": a.missing,
                "synthetic": a.synthetic,
                "n_cells": a.n_cells,
                "coverage_caps": a.caps,
            }
            for a in aggregates
        ],
        "selection": [
            {
                "context_mode": o.context_mode,
                "split": o.split,
                "winner": None if o.winner is None else {
                    "configuration_key": o.winner.configuration_key,
                    "config": o.winner.config,
                    "metrics": o.winner.metrics,
                },
                "ranked": [a.configuration_key for a in o.ranked],
                "eliminated": [
                    {
                        "configuration_key": e.configuration_key,
                        "description": e.description,
                        "reasons": list(e.reasons),
                        "technical_term_f1": e.technical_term_f1,
                    }
                    for e in o.eliminated
                ],
                "notes": o.notes,
            }
            for o in outcomes
        ],
    }
    path = results_root / f"phase{phase}.aggregate.json"
    assert_no_verbatim(payload, where=str(path))
    _atomic_write_json(path, payload)
    return path


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# CLI
# ═══════════════════════════════════════════════════════════════════════════════════════════════


def _int_list(text: str) -> tuple[int, ...]:
    return tuple(int(x) for x in text.replace(",", " ").split())


def _str_list(text: str) -> tuple[str, ...]:
    return tuple(x for x in text.replace(",", " ").split())


def _add_grid_args(p: argparse.ArgumentParser) -> None:
    p.add_argument("--phase", type=int, required=True, choices=sorted(PHASES))
    p.add_argument("--split", default="dev", choices=list(SPLITS))
    p.add_argument("--models", type=_str_list, default=None,
                   help="comma/space separated model ids (default: CANDIDATES.md §4 picks)")
    p.add_argument("--models-file", type=Path, default=None,
                   help="JSON override for the model catalogue")
    p.add_argument("--categories", type=_str_list, default=CATEGORIES)
    p.add_argument("--context-modes", type=_str_list, default=CONTEXT_MODES)
    p.add_argument("--chunk-ms", type=_int_list, default=())
    p.add_argument("--stability-n", type=_int_list, default=(DEFAULT_STABILITY[0],))
    p.add_argument("--stability-m", type=_int_list, default=(DEFAULT_STABILITY[1],))
    p.add_argument("--stability-t", type=_int_list, default=(DEFAULT_STABILITY[2],))
    p.add_argument("--cadence", type=_str_list, default=(DEFAULT_CADENCE,))
    p.add_argument("--inject", type=_str_list, default=(DEFAULT_INJECT,))
    p.add_argument("--repeats", type=int, default=3)
    p.add_argument("--max-cells", type=int, default=None,
                   help="truncate the grid; logged as a coverage cap")
    p.add_argument("--phase2-full-sweep", action="store_true",
                   help="Phase 2 shorthand: sweep chunk, stability, cadence and inject over the "
                        "given models. Explicit, so a full sweep is always something someone asked "
                        "for by name.")


def _grid_from_args(args: argparse.Namespace) -> tuple[GridOptions, dict[str, ModelSpec]]:
    catalogue = load_model_catalogue(args.models_file)
    models = tuple(args.models) if args.models else PHASE1_DEFAULT_MODELS
    opts = GridOptions(
        phase=args.phase,
        split=args.split,
        models=models,
        categories=tuple(args.categories),
        context_modes=tuple(args.context_modes),
        chunk_ms=tuple(args.chunk_ms),
        stability_n=tuple(args.stability_n),
        stability_m=tuple(args.stability_m),
        stability_t_ms=tuple(args.stability_t),
        cadences=tuple(args.cadence),
        inject_methods=tuple(args.inject),
        replay_speed=getattr(args, "speed", 1.0),
        repeats=args.repeats,
        max_cells=args.max_cells,
    )
    if getattr(args, "phase2_full_sweep", False):
        if args.phase != 2:
            raise PhaseError("--phase2-full-sweep is only meaningful with --phase 2")
        opts = replace(
            opts,
            chunk_ms=CHUNK_MS_GRID,
            stability_n=STABILITY_N_GRID,
            stability_m=STABILITY_M_GRID,
            stability_t_ms=STABILITY_T_GRID,
            cadences=CADENCES,
            inject_methods=INJECT_METHODS,
        )
    return opts, catalogue


def _policy_from_args(args: argparse.Namespace) -> ReplayPolicy:
    speed = getattr(args, "speed", 1.0)
    if speed != 1.0 and not getattr(args, "allow_faster_than_realtime", False):
        raise GuardError(
            f"REFUSING to replay audio at x{speed}.\n"
            "  plan.md Phase 2 step 2: streaming audio is replayed at real-time cadence, not fed\n"
            "  as fast as the backend accepts, because feeding it faster flatters every latency\n"
            "  number in the result. If you want a throughput run rather than a latency run,\n"
            "  pass --allow-faster-than-realtime; the result and the summary will both be stamped\n"
            "  NOT REAL TIME."
        )
    return ReplayPolicy(speed=speed, frame_ms=getattr(args, "frame_ms", 20))


def _make_executor(args: argparse.Namespace) -> Executor:
    if args.executor == "mock":
        return MockExecutor(seed=args.seed)
    return SidecarExecutor(python_exe=args.python or sys.executable)


def cmd_plan(args: argparse.Namespace) -> int:
    opts, catalogue = _grid_from_args(args)
    cells, caps = build_cells(opts, catalogue)
    policy = _policy_from_args(args)
    print(f"phase {opts.phase} ({PHASES[opts.phase].name}) · split {opts.split} · {len(cells)} cells")
    for cell in cells:
        print(f"  {cell.key}  {cell.describe()}")
    print(
        render_summary(
            phase=opts.phase, split=opts.split, cells=cells, ran=0, skipped=0, failed=0,
            caps=caps, policy=policy, executor_name="(plan only)", synthetic=False,
        )
    )
    return 0


def cmd_run(args: argparse.Namespace) -> int:
    opts, catalogue = _grid_from_args(args)
    guard_split(opts.split, heldout_decision=args.heldout_decision, purpose=args.purpose)
    policy = _policy_from_args(args)
    cells, caps = build_cells(opts, catalogue)
    for problem in check_gate_metrics_agree():
        caps.add("metric_unavailable", "bench/metrics.py", problem)
    executor = _make_executor(args)

    if opts.split == "heldout":
        ledger = append_heldout_ledger(
            args.results_root, purpose=args.purpose, cells=cells, code_version=code_version()
        )
        print(f"heldout access recorded in {ledger}", file=sys.stderr)

    run_opts = RunOptions(
        results_root=args.results_root,
        corpus_root=args.corpus_root,
        force=args.force,
        retries=args.retries,
        max_clips=args.max_clips,
        sample_seed=args.seed,
        purpose=args.purpose,
        heldout_decision=args.heldout_decision,
    )
    result = run_cells(cells, executor=executor, policy=policy, grid_caps=caps, opts=run_opts)

    store = ResultStore(args.results_root)
    records = store.load_all(phase=opts.phase, split=opts.split)
    aggregates = aggregate_results(records)
    thresholds = _thresholds_from_args(args)
    outcomes = [
        apply_selection_rule(
            aggregates, thresholds, context_mode=mode, split=opts.split,
            allow_synthetic=args.allow_synthetic,
        )
        for mode in CONTEXT_MODES
    ]
    path = write_phase_aggregate(
        args.results_root, phase=opts.phase, split=opts.split, records=records,
        caps=result["caps"], outcomes=outcomes, thresholds=thresholds, policy=policy,
    )
    print(
        render_summary(
            phase=opts.phase, split=opts.split, cells=cells, ran=result["ran"],
            skipped=result["skipped"], failed=result["failed"], caps=result["caps"],
            policy=policy, executor_name=executor.name, synthetic=executor.synthetic,
            version_mismatches=result["version_mismatches"],
        )
    )
    print(render_selection_report(outcomes))
    print(f"aggregate written to {path}")
    return 0 if result["failed"] == 0 else 1


def cmd_select(args: argparse.Namespace) -> int:
    # No heldout guard here on purpose: `select` reads result files that already exist and never
    # touches bench/corpus/. The guard belongs at the point of corpus access (`run`), which is
    # where the discipline can actually be violated.
    store = ResultStore(args.results_root)
    records = store.load_all(phase=args.phase, split=args.split)
    if not records:
        print(
            f"no results for phase {args.phase} split {args.split} under {store.cells_dir}",
            file=sys.stderr,
        )
        return 2
    aggregates = aggregate_results(records)
    thresholds = _thresholds_from_args(args)
    outcomes = [
        apply_selection_rule(
            aggregates, thresholds, context_mode=mode, split=args.split,
            allow_synthetic=args.allow_synthetic,
        )
        for mode in CONTEXT_MODES
    ]
    caps = CoverageLog()
    for rec in records:
        for cap in rec.get("coverage_caps", []):
            try:
                caps.add(cap["kind"], cap["scope"], cap["detail"])
            except (KeyError, ConfigError):
                continue
    print(render_selection_report(outcomes))
    if len(caps):
        print(f"  COVERAGE CAPS recorded in these results ({len(caps)}):")
        for cap in caps:
            print(f"    · {cap}")
    return 0


def _thresholds_from_args(args: argparse.Namespace) -> Thresholds:
    return Thresholds(
        first_partial_ms=args.limit_first_partial_ms,
        finalisation_ms=args.limit_finalisation_ms,
        icer_per_1000=args.limit_icer,
        peak_vram_mb=args.limit_vram_mb,
        max_hallucination_rate=args.limit_hallucination_rate,
    )


def _add_threshold_args(p: argparse.ArgumentParser) -> None:
    p.add_argument("--limit-first-partial-ms", type=float, default=300.0)
    p.add_argument("--limit-finalisation-ms", type=float, default=500.0)
    p.add_argument("--limit-icer", type=float, default=2.0,
                   help="per 1000 committed graphemes; PROJECT-DECLARED, plan.md states no number")
    p.add_argument("--limit-vram-mb", type=float, default=24576.0)
    p.add_argument("--limit-hallucination-rate", type=float, default=None,
                   help="NOT part of the pre-registered rule; opt-in extra gate")
    p.add_argument("--allow-synthetic", action="store_true",
                   help="let the selection rule consider MockExecutor results. For testing the "
                        "rule, never for a decision.")


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="run_matrix.py",
        description=(
            "kvit-dictate experiment runner (plan.md §4). Sequential elimination, one JSON file "
            "per cell, real-time replay, mechanical dev/heldout discipline, and the "
            "pre-registered selection rule as code."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--results-root", type=Path, default=DEFAULT_RESULTS_ROOT)
    p.add_argument("--corpus-root", type=Path, default=DEFAULT_CORPUS_ROOT)
    sub = p.add_subparsers(dest="cmd", required=True)

    plan_p = sub.add_parser("plan", help="expand the grid and print it; runs nothing")
    _add_grid_args(plan_p)
    plan_p.add_argument("--speed", type=float, default=1.0)
    plan_p.add_argument("--allow-faster-than-realtime", action="store_true")
    plan_p.add_argument("--frame-ms", type=int, default=20)
    plan_p.set_defaults(func=cmd_plan)

    run_p = sub.add_parser("run", help="run the grid, one JSON file per cell, resumable")
    _add_grid_args(run_p)
    _add_threshold_args(run_p)
    run_p.add_argument("--executor", choices=("mock", "sidecar"), default="mock")
    run_p.add_argument("--python", default=None, help="interpreter for the sidecar executor")
    run_p.add_argument("--speed", type=float, default=1.0,
                       help="replay speed; 1.0 = real time and the only mode whose latency means "
                            "anything")
    run_p.add_argument("--allow-faster-than-realtime", action="store_true")
    run_p.add_argument("--frame-ms", type=int, default=20)
    run_p.add_argument("--force", action="store_true", help="re-run cells that already have a result")
    run_p.add_argument("--retries", type=int, default=0)
    run_p.add_argument("--max-clips", type=int, default=None,
                       help="sample N clips per category; logged as a coverage cap")
    run_p.add_argument("--seed", type=int, default=0)
    run_p.add_argument("--purpose", default="tuning")
    run_p.add_argument("--heldout-decision", action="store_true",
                       help="required to touch heldout/; recorded in the heldout ledger")
    run_p.set_defaults(func=cmd_run)

    sel_p = sub.add_parser("select", help="apply the pre-registered rule to existing results")
    sel_p.add_argument("--phase", type=int, required=True, choices=sorted(PHASES))
    sel_p.add_argument("--split", default="heldout", choices=list(SPLITS))
    _add_threshold_args(sel_p)
    sel_p.set_defaults(func=cmd_select)
    return p


def main(argv: Sequence[str] | None = None) -> int:
    # The reports contain §, ×, · and box drawing. A Windows console at cp1252 would raise
    # UnicodeEncodeError halfway through a summary and lose the coverage-cap block, which is the
    # part that must never be lost. UTF-8 with replacement is the lesser evil.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[union-attr]
        except (AttributeError, OSError, ValueError):
            pass
    args = build_parser().parse_args(argv)
    try:
        return int(args.func(args))
    except (ConfigError, ExecutorError) as exc:
        print(f"\n{type(exc).__name__}: {exc}\n", file=sys.stderr)
        return 2


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
