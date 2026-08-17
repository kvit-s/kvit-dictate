"""Native-streaming ONNX transducer backend, driven through sherpa-onnx.

Covers `bench/CANDIDATES.md` §4 picks 1 and 2 — the published sherpa-onnx ONNX exports of
NVIDIA's cache-aware FastConformer-RNNT streaming models:

    A'  csukuangfj2/sherpa-onnx-nemotron-speech-streaming-en-0.6b-{80,160,560,1120}ms[-int8]-2026-04-25
    B'  csukuangfj2/sherpa-onnx-nemotron-3.5-asr-streaming-0.6b-{80,160,320,560,1120}ms[-int8]-2026-06-11

`ready.mode` is `"native-streaming"` (docs/protocol.md §6.2): the architecture is causal and
cache-aware and the published recipe *is* chunked inference. Latency numbers from this backend
are therefore comparable with the streaming zipformer and are **not** comparable with the
pseudo-streaming Whisper baseline.

──────────────────────────────────────────────────────────────────────────────────────────────
This backend cannot do context biasing, and says so
──────────────────────────────────────────────────────────────────────────────────────────────

`supports_context` is **False**, which the server must surface as `ready.context_supported:false`
(docs/protocol.md §6.1, §6.2). CANDIDATES.md §5.1 records why: sherpa-onnx's Nemotron online
transducer supports `greedy_search` only, and hotwords require `modified_beam_search`, which the
Nemotron path explicitly rejects (k2-fsa/sherpa-onnx issue 3572, open as of 2026-08-16).

Accepting `start.context` and quietly discarding it would be worse than refusing it: plan.md §4
compares context-enabled and context-free configurations *separately*, and a backend that appears
to take the vocabulary hint while ignoring it puts a context-free result in the context-enabled
column. So `start_session()` logs the ignored hint, counts it, and the count is available for the
run record — but `supports_context` never lies. The context-biasing arm is a different model and
a different module: `zipformer_hotwords.py`.

──────────────────────────────────────────────────────────────────────────────────────────────
Chunk size is a model, not a parameter
──────────────────────────────────────────────────────────────────────────────────────────────

Each published chunk-size variant is a **separate repository with different weights**, not a
runtime switch, which is why `models.json` carries one entry per chunk size. `chunk_ms` here is
therefore a *declaration* that is cross-checked against the model directory name; a mismatch is a
hard error rather than a warning, because `ready.chunk_ms` feeds plan.md §4's sweep and a
mislabelled cell silently corrupts the comparison it exists to make.

**Discrepancy worth recording** (plan.md §4 vs CANDIDATES.md §1.1): the plan sweeps
`chunk_ms ∈ {80,160,320,640}`, but the published Nemotron variants are 80/160/560/1120 ms, plus
320 ms for Nemotron-3.5 only. There is no 640 ms export of either model, and no 320 ms export of
the English-only model. The document wins: this backend accepts only the published values, and
the 640 ms cell has no entrant from this family.

──────────────────────────────────────────────────────────────────────────────────────────────
No downloads, ever
──────────────────────────────────────────────────────────────────────────────────────────────

plan.md §1.9: no network access during dictation, and model fetching is an explicit, separate,
user-initiated action. This module reads model files from a directory that already exists and
raises `ModelFilesMissing` — with the command a human should run — if they do not. It never
contacts the network, and `sherpa_onnx` is imported lazily inside `load()` so that a missing
runtime disables one backend instead of breaking the server at package-import time.

──────────────────────────────────────────────────────────────────────────────────────────────
UNVERIFIED — every API detail below was written against sherpa-onnx's documentation and its
published Python examples, on a machine where sherpa-onnx is NOT installed. None of it has been
executed. Check these first on the first real run:
──────────────────────────────────────────────────────────────────────────────────────────────

 1. `sherpa_onnx.OnlineRecognizer.from_transducer(...)` — existence, and that it accepts the
    keyword names used in `_RECOGNIZER_REQUIRED_KWARGS` / `_RECOGNIZER_OPTIONAL_KWARGS`. The
    signature is introspected at load time and a missing *load-bearing* keyword is a loud
    `SherpaOnnxApiMismatch` naming the keyword and the installed version; a missing *tuning*
    keyword is dropped with a warning. Nothing is silently filtered.
 2. `recognizer.create_stream()`, `recognizer.is_ready(stream)`, `recognizer.decode_stream(stream)`,
    `recognizer.get_result(stream)`, `recognizer.reset(stream)`, `recognizer.is_endpoint(stream)`,
    `stream.accept_waveform(sample_rate, samples)`, `stream.input_finished()`. Each is fetched
    through `_require_attr()`, so an API change surfaces as a named error rather than as a bare
    `AttributeError` three frames deep.
 3. The return type of `get_result()` — believed to be `str` for the *online* recognizer, but some
    versions return a result object with `.text`. `_result_text()` accepts either and raises a
    specific error for anything else.
 4. That `accept_waveform` wants float32 samples in [-1, 1] at the model's sample rate. This is
    documented and consistent across every sherpa-onnx example, and it is the single most
    dangerous thing to get wrong: passing int16 through unchanged produces a model that hears
    near-silence and transcribes almost nothing, with no error anywhere. `_pcm16le_to_float32()`
    converts explicitly.
 5. Tail padding before `input_finished()`. sherpa-onnx's own streaming examples append roughly
    0.5 s of silence to flush the last frames out of the encoder's right context. The exact
    requirement is undocumented; `tail_padding_ms` defaults to 500 and is configurable.
 6. Whether `model_type` must be set for the Nemotron exports. sherpa-onnx normally reads the
    model kind from ONNX metadata; `model_type` defaults to `""` (auto) and is exposed in case it
    does not.
 7. File names inside the published repositories (`encoder*.onnx` etc). Discovery is by glob and
    classification, and ambiguity is an error that lists what was found — it is never resolved by
    guessing, because picking the wrong `.int8.onnx` variant would silently change what is being
    measured.
 8. `provider="cuda"` on this machine. CANDIDATES.md §6 item 1 could not confirm that the official
    `onnxruntime-gpu` wheel ships sm_120 kernels for Windows; the CPU EP is the baseline plan and
    is the default here. `available_providers()` is logged at load so the choice is observable.
"""

from __future__ import annotations

import array
import glob
import inspect
import logging
import os
import re
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Iterable, Mapping, Sequence

__all__ = [
    "SherpaOnnxBackendError",
    "SherpaOnnxUnavailable",
    "SherpaOnnxApiMismatch",
    "ModelFilesMissing",
    "SherpaOnnxStreamingConfig",
    "SherpaOnnxStreamingBackend",
    "create",
    "PUBLISHED_CHUNK_MS",
    "pcm16le_to_float32",
]

log = logging.getLogger(__name__)

# docs/protocol.md §6.3: proto 1 audio is 16 kHz mono s16le, full stop.
PROTOCOL_SAMPLE_RATE = 16000

# The install step, quoted verbatim in the error message a human will actually see. uv is at
# C:\Users\sk\.local\bin\uv.exe (CANDIDATES.md §3); UV_CACHE_DIR must be on D: because uv hardlinks
# from its cache into the venv and hardlinks cannot cross volumes (plan.md §5.2.4).
INSTALL_COMMAND = (
    "set UV_CACHE_DIR=D:\\uv-cache\r\n"
    "uv venv D:\\projects\\kvit-dictate\\envs\\onnx --python 3.12\r\n"
    "uv pip install --python D:\\projects\\kvit-dictate\\envs\\onnx\\Scripts\\python.exe "
    "sherpa-onnx onnxruntime numpy"
)


class SherpaOnnxBackendError(RuntimeError):
    """Base class for every failure this backend originates.

    The server maps these onto `model_load_failed` (fatal, at load) or `decode_failed`
    (non-fatal, per session) — docs/protocol.md §11.3.
    """


class SherpaOnnxUnavailable(SherpaOnnxBackendError):
    """`import sherpa_onnx` failed. This backend is disabled; the rest of the server is fine."""


class SherpaOnnxApiMismatch(SherpaOnnxBackendError):
    """sherpa-onnx is installed but does not expose the API this adapter was written against.

    Raised deliberately and with detail, rather than letting an `AttributeError` or a `TypeError`
    escape from inside the decode loop. Every API detail in this module is marked UNVERIFIED in
    the module docstring; this exception is what turns "unverified" into a legible failure.
    """


class ModelFilesMissing(SherpaOnnxBackendError):
    """The configured model directory does not hold a usable sherpa-onnx transducer export."""


# ── Published variants ─────────────────────────────────────────────────────────────────────────
#
# CANDIDATES.md §1.1 and the model table, rows A'/B'. Used for three things: validating a
# configured chunk size, telling a human exactly which repository to fetch, and refusing to invent
# a variant that does not exist.

@dataclass(frozen=True)
class _Family:
    key: str                       # substring that identifies the family in a repo id or dir name
    display: str
    chunk_ms: tuple[int, ...]
    repo_template: str             # {chunk} and {int8} substituted


_FAMILIES: tuple[_Family, ...] = (
    _Family(
        key="nemotron-3.5-asr-streaming-0.6b",
        display="Nemotron-3.5 ASR streaming 0.6B (multilingual)",
        chunk_ms=(80, 160, 320, 560, 1120),
        repo_template=(
            "csukuangfj2/sherpa-onnx-nemotron-3.5-asr-streaming-0.6b-{chunk}ms{int8}-2026-06-11"
        ),
    ),
    _Family(
        key="nemotron-speech-streaming-en-0.6b",
        display="Nemotron Speech Streaming EN 0.6B",
        chunk_ms=(80, 160, 560, 1120),
        repo_template=(
            "csukuangfj2/sherpa-onnx-nemotron-speech-streaming-en-0.6b-{chunk}ms{int8}-2026-04-25"
        ),
    ),
)

PUBLISHED_CHUNK_MS: dict[str, tuple[int, ...]] = {f.key: f.chunk_ms for f in _FAMILIES}

# `-160ms-` in a repo id or directory name. Anchored on the `ms` so it cannot match a date.
_CHUNK_IN_NAME = re.compile(r"(?<![0-9])(\d{2,4})ms(?![0-9])")


def _family_for(name: str) -> _Family | None:
    lowered = name.lower()
    for fam in _FAMILIES:
        if fam.key in lowered:
            return fam
    return None


# ── PCM conversion ─────────────────────────────────────────────────────────────────────────────

def pcm16le_to_float32(pcm: bytes) -> Any:
    """Convert protocol audio (16 kHz mono s16le) to float32 samples in [-1, 1].

    This is the conversion the module docstring calls the most dangerous thing to get wrong.
    sherpa-onnx expects *float* samples; handing it int16 values produces amplitudes around
    ±32768 where it expects ±1, which the feature extractor turns into something the model has
    never seen. There is no exception and no warning — the model simply transcribes almost
    nothing, which reads exactly like a bad model. Hence: explicit, in one place, with a test.

    Divisor is 32768.0, not 32767.0: 32768 is the magnitude of the most negative representable
    sample, so it is the only divisor that maps the whole int16 range into [-1, 1] without
    clipping. The cost is that +32767 maps to 0.99997 instead of 1.0, which is inaudible and is
    what every audio toolchain does.

    Returns a numpy `float32` array when numpy is importable (the fast path, and what sherpa-onnx
    examples pass), otherwise an `array.array('f')`. numpy is a *backend* dependency: importing it
    here is allowed, importing it at package level is not.
    """
    if len(pcm) % 2 != 0:
        # docs/protocol.md §6.3 makes an odd-length audio payload `bad_message` at the framing
        # layer, so this should be unreachable from the wire. It is checked anyway because the
        # bench harness feeds this function from files.
        raise ValueError(f"PCM payload has odd length {len(pcm)}; a sample was split")

    samples = array.array("h")
    samples.frombytes(pcm)
    if sys.byteorder != "little":
        # The protocol says little-endian and the project is Windows/x64 only. Handled rather
        # than asserted because a wrong-endian read is silent, and silence is the failure mode
        # this whole function exists to prevent.
        samples.byteswap()

    try:
        import numpy as np  # noqa: PLC0415 - backend-local import, see module docstring
    except ImportError:
        out = array.array("f", (s / 32768.0 for s in samples))
        return out
    return np.frombuffer(samples.tobytes(), dtype="<i2").astype(np.float32) / 32768.0


def _silence(duration_ms: int) -> Any:
    """`duration_ms` of float32 zeros at the protocol sample rate."""
    n = max(0, int(PROTOCOL_SAMPLE_RATE * duration_ms / 1000))
    try:
        import numpy as np  # noqa: PLC0415
    except ImportError:
        return array.array("f", [0.0] * n)
    return np.zeros(n, dtype=np.float32)


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
    if isinstance(value, bool):  # bool is an int subclass; almost never what was meant here
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


@dataclass
class SherpaOnnxStreamingConfig:
    """Everything this backend needs, with no reference to argparse or models.json.

    `model` is what `models.json` passes as `--model`: either a directory path, or a Hugging Face
    repo id whose last path component names a directory under `models_root`. A repo id is *not* a
    download instruction — it is resolved to a local directory and nothing else (plan.md §1.9).
    """

    model: str
    model_dir: str | None = None
    models_root: str | None = None      # default: $KVIT_ASR_MODELS_DIR
    chunk_ms: int | None = None
    precision: str = "auto"             # "auto" | "fp32" | "int8"
    provider: str = "cpu"               # CANDIDATES.md §6.1 — CPU is the baseline, GPU the option
    num_threads: int = 4
    decoding_method: str = "greedy_search"
    feature_dim: int = 80
    sample_rate: int = PROTOCOL_SAMPLE_RATE
    model_type: str = ""                # "" = let sherpa-onnx read it from ONNX metadata
    enable_endpoint_detection: bool = False   # the sidecar runs Silero VAD; see the note in load()
    rule1_min_trailing_silence: float = 2.4
    rule2_min_trailing_silence: float = 1.2
    rule3_min_utterance_length: float = 300.0
    blank_penalty: float = 0.0
    tail_padding_ms: int = 500          # UNVERIFIED item 5
    warmup_ms: int = 300                # 0 disables; see `load()`
    extra: dict[str, Any] = field(default_factory=dict)   # passed through to from_transducer

    @classmethod
    def from_mapping(cls, options: Mapping[str, Any]) -> "SherpaOnnxStreamingConfig":
        """Build from a flat mapping, coercing the strings a command line delivers.

        Keys may use either `-` or `_`. An **unknown key raises** rather than being ignored: a
        misspelled `--chunk-ms` that silently kept the default would mislabel a whole plan.md §4
        sweep cell, and the mislabelling would only be visible in the results.
        """
        norm = {str(k).replace("-", "_").lstrip("_"): v for k, v in options.items()}
        known = {f.name for f in cls.__dataclass_fields__.values() if not f.name.startswith("_")}
        unknown = sorted(set(norm) - known)
        if unknown:
            raise ValueError(
                f"unknown option(s) for the sherpa-onnx streaming backend: {', '.join(unknown)}; "
                f"recognised: {', '.join(sorted(known))}"
            )
        if "model" not in norm:
            raise ValueError("the sherpa-onnx streaming backend requires `model`")

        kwargs: dict[str, Any] = {"model": str(norm["model"])}
        for name in ("model_dir", "models_root", "precision", "provider", "decoding_method",
                     "model_type"):
            if norm.get(name) is not None:
                kwargs[name] = str(norm[name])
        for name in ("chunk_ms", "num_threads", "feature_dim", "sample_rate", "tail_padding_ms",
                     "warmup_ms"):
            if norm.get(name) is not None:
                kwargs[name] = _as_int(norm[name], name)
        for name in ("rule1_min_trailing_silence", "rule2_min_trailing_silence",
                     "rule3_min_utterance_length", "blank_penalty"):
            if norm.get(name) is not None:
                kwargs[name] = _as_float(norm[name], name)
        if norm.get("enable_endpoint_detection") is not None:
            kwargs["enable_endpoint_detection"] = _as_bool(
                norm["enable_endpoint_detection"], "enable_endpoint_detection"
            )
        if norm.get("extra") is not None:
            extra = norm["extra"]
            if not isinstance(extra, Mapping):
                raise ValueError("extra: expected a mapping")
            kwargs["extra"] = dict(extra)
        return cls(**kwargs)


# ── sherpa-onnx access, guarded ────────────────────────────────────────────────────────────────

def _import_sherpa_onnx() -> Any:
    try:
        import sherpa_onnx  # noqa: PLC0415 - lazy on purpose: see the module docstring
    except ImportError as exc:
        raise SherpaOnnxUnavailable(
            "sherpa-onnx is not importable in this interpreter "
            f"({sys.executable}), so the ONNX streaming backend is unavailable.\n"
            "The server does not install anything — plan.md §1.9 makes environment creation an "
            "explicit, user-initiated action. A human runs, once:\n\n"
            f"{INSTALL_COMMAND}\n\n"
            "then re-selects this model. Nothing may be installed into "
            "D:\\projects\\wan-dancer\\.venv (plan.md §5.1)."
        ) from exc
    return sherpa_onnx


def _require_attr(obj: Any, name: str, what: str) -> Any:
    """Fetch an attribute or raise a `SherpaOnnxApiMismatch` that says what was expected.

    Exists so that an API drift in sherpa-onnx reads as "this adapter was written against an API
    that changed" rather than as `AttributeError: 'OnlineRecognizer' object has no attribute ...`
    surfacing from inside a decode loop.
    """
    value = getattr(obj, name, None)
    if value is None:
        available = ", ".join(sorted(a for a in dir(obj) if not a.startswith("_")))
        raise SherpaOnnxApiMismatch(
            f"sherpa-onnx {what} has no `{name}`. This adapter was written against the "
            f"documented sherpa-onnx online-recognizer API and that detail is marked UNVERIFIED "
            f"in the module docstring. Installed version: {_sherpa_version()}. "
            f"Available attributes: {available}"
        )
    return value


def _sherpa_version() -> str:
    try:
        import sherpa_onnx  # noqa: PLC0415
    except ImportError:
        return "not installed"
    return str(getattr(sherpa_onnx, "__version__", "unknown"))


def _result_text(recognizer: Any, stream: Any) -> str:
    """Current hypothesis for `stream`, as a string. UNVERIFIED item 3."""
    get_result = _require_attr(recognizer, "get_result", "OnlineRecognizer")
    result = get_result(stream)
    if isinstance(result, str):
        return result.strip()
    text = getattr(result, "text", None)
    if isinstance(text, str):
        return text.strip()
    raise SherpaOnnxApiMismatch(
        f"OnlineRecognizer.get_result() returned {type(result).__name__}, which is neither a str "
        f"nor an object with a `.text` string. Expected one of those two "
        f"(sherpa-onnx {_sherpa_version()})."
    )


# ── Model file discovery ───────────────────────────────────────────────────────────────────────

def _resolve_model_dir(cfg: SherpaOnnxStreamingConfig) -> Path:
    """Locate the already-downloaded model directory. Never fetches anything."""
    if cfg.model_dir:
        candidate = Path(cfg.model_dir).expanduser()
    else:
        root = cfg.models_root or os.environ.get("KVIT_ASR_MODELS_DIR") or ""
        model = cfg.model.replace("\\", "/")
        if not root:
            candidate = Path(model).expanduser()
            if not candidate.is_absolute() and "/" in model:
                # A bare repo id with no root configured: refuse rather than guess a location.
                raise ModelFilesMissing(
                    f"`{cfg.model}` looks like a Hugging Face repo id, but neither `model_dir` "
                    "nor KVIT_ASR_MODELS_DIR is set, so there is nothing to resolve it against. "
                    "Set KVIT_ASR_MODELS_DIR to the directory holding downloaded sherpa-onnx "
                    "models, or pass an explicit --model-dir. "
                    + _download_hint(cfg)
                )
        else:
            # Last path component only: `csukuangfj2/sherpa-onnx-…` lives as `sherpa-onnx-…`.
            candidate = Path(root).expanduser() / model.rsplit("/", 1)[-1]

    if not candidate.is_dir():
        raise ModelFilesMissing(
            f"model directory not found: {candidate}\n" + _download_hint(cfg)
        )
    return candidate


def _download_hint(cfg: SherpaOnnxStreamingConfig) -> str:
    """The exact fetch a human runs. The server never runs it (plan.md §1.9)."""
    repo = cfg.model
    fam = _family_for(cfg.model) or (_family_for(cfg.model_dir or "") if cfg.model_dir else None)
    if fam is not None and cfg.chunk_ms is not None and "/" not in repo:
        repo = fam.repo_template.format(
            chunk=cfg.chunk_ms, int8="-int8" if cfg.precision == "int8" else ""
        )
    return (
        "Model files are never fetched automatically — plan.md §1.9 makes model fetching an "
        "explicit, separate, user-initiated action, and the sidecar performs no network access "
        "at all. A human downloads it once, e.g.\n\n"
        f"    hf download {repo} --local-dir %KVIT_ASR_MODELS_DIR%\\{repo.rsplit('/', 1)[-1]}\n"
        "    (older Hugging Face CLIs spell this `huggingface-cli download`)\n\n"
        "and re-selects the model."
    )


def _classify_onnx(files: Sequence[Path], role: str, want_int8: bool | None) -> list[Path]:
    hits = [p for p in files if role in p.name.lower()]
    if want_int8 is True:
        hits = [p for p in hits if ".int8." in p.name.lower()]
    elif want_int8 is False:
        hits = [p for p in hits if ".int8." not in p.name.lower()]
    return sorted(hits)


def _find_model_files(model_dir: Path, precision: str) -> tuple[Path, Path, Path, Path]:
    """Find encoder / decoder / joiner / tokens.txt.

    Discovery is by glob because the exact file names inside the published repositories are
    UNVERIFIED (docstring item 7) — the zipformer repos use long
    `encoder-epoch-99-avg-1-chunk-16-left-128.onnx` names, and the Nemotron exports may not.
    Ambiguity is an **error listing the candidates**, never a guess: silently picking the int8
    graph when fp32 was requested would change what is being measured while the run record still
    said `fp32`.
    """
    onnx = sorted(Path(p) for p in glob.glob(str(model_dir / "*.onnx")))
    if not onnx:
        raise ModelFilesMissing(f"no .onnx files in {model_dir}")

    if precision not in ("auto", "fp32", "int8"):
        raise ValueError(f"precision must be auto|fp32|int8, got {precision!r}")

    orders: tuple[bool | None, ...]
    if precision == "int8":
        orders = (True,)
    elif precision == "fp32":
        orders = (False,)
    else:
        orders = (False, True)   # prefer full precision; int8 only if that is all there is

    chosen: dict[str, Path] = {}
    for want_int8 in orders:
        found: dict[str, Path] = {}
        ok = True
        for role in ("encoder", "decoder", "joiner"):
            hits = _classify_onnx(onnx, role, want_int8)
            if len(hits) == 1:
                found[role] = hits[0]
            elif len(hits) > 1:
                raise ModelFilesMissing(
                    f"{model_dir}: {len(hits)} candidate {role} graphs "
                    f"({', '.join(p.name for p in hits)}). Which one is intended cannot be "
                    "guessed — set `precision` explicitly, or point `model_dir` at a directory "
                    "holding exactly one export."
                )
            else:
                ok = False
                break
        if ok:
            chosen = found
            break

    if not chosen:
        raise ModelFilesMissing(
            f"{model_dir}: could not find an encoder/decoder/joiner triple"
            f"{'' if precision == 'auto' else f' at precision {precision}'}. "
            f"Files present: {', '.join(p.name for p in onnx)}"
        )

    tokens = model_dir / "tokens.txt"
    if not tokens.is_file():
        raise ModelFilesMissing(f"{model_dir}: tokens.txt is missing")

    return chosen["encoder"], chosen["decoder"], chosen["joiner"], tokens


# ── Recognizer construction ────────────────────────────────────────────────────────────────────

# Keywords whose absence means this adapter cannot do its job correctly. A build of sherpa-onnx
# that does not accept these is not one this module can drive, and saying so loudly beats guessing.
_RECOGNIZER_REQUIRED_KWARGS = ("tokens", "encoder", "decoder", "joiner", "decoding_method")

# Keywords that tune behaviour. If a build does not accept one, it is dropped **with a warning
# naming it** — never silently, because a dropped `provider` or `num_threads` changes the latency
# numbers plan.md §4 records.
_RECOGNIZER_OPTIONAL_KWARGS = (
    "num_threads", "provider", "sample_rate", "feature_dim", "model_type",
    "enable_endpoint_detection", "rule1_min_trailing_silence", "rule2_min_trailing_silence",
    "rule3_min_utterance_length", "blank_penalty", "hotwords_file", "hotwords_score",
    "max_active_paths", "modeling_unit", "bpe_vocab",
)


def _build_recognizer(kwargs: Mapping[str, Any], *, context: str) -> Any:
    """Call `OnlineRecognizer.from_transducer` with signature checking. UNVERIFIED item 1."""
    sherpa_onnx = _import_sherpa_onnx()
    recognizer_cls = _require_attr(sherpa_onnx, "OnlineRecognizer", "module")
    factory = _require_attr(recognizer_cls, "from_transducer", "OnlineRecognizer")

    try:
        sig = inspect.signature(factory)
    except (TypeError, ValueError):   # a C-implemented callable with no introspectable signature
        sig = None

    passed = dict(kwargs)
    if sig is not None:
        accepts_var_kw = any(
            p.kind is inspect.Parameter.VAR_KEYWORD for p in sig.parameters.values()
        )
        if not accepts_var_kw:
            names = set(sig.parameters)
            missing_required = [k for k in _RECOGNIZER_REQUIRED_KWARGS
                                if k in passed and k not in names]
            if missing_required:
                raise SherpaOnnxApiMismatch(
                    "sherpa-onnx OnlineRecognizer.from_transducer does not accept the "
                    f"load-bearing keyword(s) {', '.join(missing_required)}. This adapter was "
                    f"written against the documented API (UNVERIFIED, see module docstring). "
                    f"Installed version: {_sherpa_version()}. "
                    f"Accepted keywords: {', '.join(sorted(names))}"
                )
            dropped = sorted(k for k in passed if k not in names)
            for k in dropped:
                log.warning(
                    "sherpa-onnx %s does not accept `%s`; dropping it. The value %r will NOT "
                    "take effect, and any measurement that assumed it is invalid.",
                    _sherpa_version(), k, passed[k],
                )
                passed.pop(k)

    try:
        return factory(**passed)
    except Exception as exc:   # noqa: BLE001 - re-raised with the context that makes it legible
        raise SherpaOnnxBackendError(
            f"sherpa-onnx failed to build the online recognizer ({context}). "
            f"version={_sherpa_version()} keywords={sorted(passed)} "
            f"decoding_method={passed.get('decoding_method')!r} "
            f"provider={passed.get('provider')!r}: {type(exc).__name__}: {exc}"
        ) from exc


# ── The backend ────────────────────────────────────────────────────────────────────────────────

class SherpaOnnxStreamingBackend:
    """Native-streaming ONNX transducer, one utterance at a time.

    Lifecycle expected by the server's session loop:

        b = SherpaOnnxStreamingBackend(cfg)
        b.load(progress)                       # once, before `ready`
        b.start_session(session_id, context)   # per `start`
        text = b.accept_pcm(pcm)               # per audio frame; CUMULATIVE for the utterance
        text = b.finish_utterance()            # at a VAD endpoint or on `stop`
        b.discard_utterance()                  # on `cancel` / a discontinuity
        b.end_session()                        # per `stopped`
        b.close()

    `accept_pcm` and `finish_utterance` return the cumulative hypothesis for the *current
    utterance*, exactly as docs/protocol.md §8.1 defines `partial.text` — not a delta. Everything
    above that (stability, `stable_bytes`, grapheme boundaries, UTF-8 offsets) is the server's
    job, deliberately: it must behave identically for every backend, including the mock.

    Threading: single-owner and not thread-safe. One decode loop drives one instance.
    """

    mode = "native-streaming"          # docs/protocol.md §6.2 `ready.mode`
    supports_context = False           # CANDIDATES.md §5.1 — greedy_search only. Never lie here.

    def __init__(self, config: SherpaOnnxStreamingConfig) -> None:
        self.config = config
        self._recognizer: Any = None
        self._stream: Any = None
        self._model_dir: Path | None = None
        self._chunk_ms: int | None = config.chunk_ms
        self._session_id: str | None = None
        self._warm_ms: int | None = None
        self._recognizer_kwargs_used: dict[str, Any] = {}
        self._ignored_context_items = 0
        # Non-fatal observations worth carrying into the run record: dropped keywords, precision
        # fallbacks, unverifiable file choices. plan.md's tone: say what is unknown, in the record.
        self.diagnostics: list[str] = []

    # -- introspection the server needs for `ready` ---------------------------------------------

    @property
    def model_id(self) -> str:
        return self.config.model

    @property
    def chunk_ms(self) -> int | None:
        return self._chunk_ms

    @property
    def warm_ms(self) -> int | None:
        return self._warm_ms

    @property
    def is_loaded(self) -> bool:
        return self._recognizer is not None

    @property
    def ignored_context_items(self) -> int:
        """How many vocabulary hints were discarded because this backend cannot use them.

        Non-zero means plan.md §4's context-enabled column must not contain this run.
        """
        return self._ignored_context_items

    # -- load -----------------------------------------------------------------------------------

    def load(self, progress: Callable[[str, int | None], None] | None = None) -> None:
        """Import sherpa-onnx, resolve files, build the recognizer, warm it.

        `progress(stage, percent_or_None)` maps onto `loading` messages (docs/protocol.md §6.2);
        the server should send one at least every LOADING_HEARTBEAT_MS.
        """
        started = time.perf_counter()

        def stage(name: str, pct: int | None = None) -> None:
            log.info("loading: %s", name)
            if progress is not None:
                progress(name, pct)

        stage("import sherpa_onnx", 0)
        _import_sherpa_onnx()   # raises SherpaOnnxUnavailable with the install command
        log.info("sherpa-onnx %s, python %s", _sherpa_version(), sys.version.split()[0])

        # Observable rather than assumed: CANDIDATES.md §6.1 could not confirm that the official
        # onnxruntime-gpu wheel carries sm_120 kernels for Windows, so which EP is actually in use
        # is a fact worth logging on every load instead of being inferred from a config value.
        try:
            import onnxruntime  # noqa: PLC0415
            providers = list(onnxruntime.get_available_providers())
            log.info("onnxruntime %s providers=%s", onnxruntime.__version__, providers)
            if self.config.provider.lower() == "cuda" and not any(
                p.lower().startswith("cuda") for p in providers
            ):
                self.diagnostics.append(
                    f"provider=cuda requested but onnxruntime advertises {providers}; "
                    "sherpa-onnx may fall back to CPU silently"
                )
                log.warning("%s", self.diagnostics[-1])
        except ImportError:
            # sherpa-onnx wheels bundle their own runtime, so a missing `onnxruntime` module is
            # not necessarily an error here — only an unobservable one.
            self.diagnostics.append(
                "onnxruntime is not importable separately; the execution provider actually in use "
                "could not be observed"
            )
            log.info("%s", self.diagnostics[-1])

        stage("resolve model files", 10)
        model_dir = _resolve_model_dir(self.config)
        self._model_dir = model_dir
        self._resolve_chunk_ms(model_dir)
        encoder, decoder, joiner, tokens = _find_model_files(model_dir, self.config.precision)
        log.info(
            "model dir %s\n  encoder=%s\n  decoder=%s\n  joiner=%s\n  tokens=%s",
            model_dir, encoder.name, decoder.name, joiner.name, tokens.name,
        )

        stage("build recognizer", 40)
        kwargs = self._recognizer_kwargs(encoder, decoder, joiner, tokens)
        # Kept so that a subclass which must rebuild the recognizer (the hotwords-file fallback in
        # zipformer_hotwords.py) does not have to re-derive the file paths, and so the exact
        # construction is available for the run record.
        self._recognizer_kwargs_used = dict(kwargs)
        self._recognizer = _build_recognizer(kwargs, context=f"model={self.config.model}")

        if self.config.warmup_ms > 0:
            # Warm the ONNX session on a throwaway stream. plan.md §4 records cold *and* warm
            # start separately, and `ready.warm_ms` is meant to include the first inference — not
            # to hide it in the first user utterance. The stream is discarded, so any silence
            # hallucination it provokes goes nowhere.
            stage("warmup", 85)
            try:
                warm = self._create_stream()
                self._accept(warm, _silence(self.config.warmup_ms))
                self._drain(warm)
            except SherpaOnnxBackendError:
                raise
            except Exception as exc:   # noqa: BLE001
                raise SherpaOnnxBackendError(
                    f"warmup decode failed for {self.config.model}: {type(exc).__name__}: {exc}"
                ) from exc

        self._warm_ms = int((time.perf_counter() - started) * 1000)
        stage("ready", 100)

    def _resolve_chunk_ms(self, model_dir: Path) -> None:
        """Cross-check the declared chunk size against the model directory and the family table.

        A wrong `ready.chunk_ms` is worse than a missing one: plan.md §4 sweeps chunk size, and a
        cell labelled 160 ms that actually ran 560 ms weights corrupts the sweep silently. So a
        contradiction is fatal here, at load, rather than discovered later in the results.
        """
        names = f"{self.config.model} {model_dir.name}"
        fam = _family_for(names)
        in_name = _CHUNK_IN_NAME.search(model_dir.name) or _CHUNK_IN_NAME.search(self.config.model)
        from_name = int(in_name.group(1)) if in_name else None

        if self._chunk_ms is None:
            self._chunk_ms = from_name
            if from_name is None:
                self.diagnostics.append(
                    "chunk size is neither configured nor derivable from the model directory "
                    "name; `ready.chunk_ms` will be omitted"
                )
                log.warning("%s", self.diagnostics[-1])
        elif from_name is not None and from_name != self._chunk_ms:
            raise SherpaOnnxBackendError(
                f"configured chunk_ms={self._chunk_ms} contradicts the model directory name "
                f"({model_dir.name}, which says {from_name} ms). Each published chunk size is a "
                "separate repository with different weights, not a runtime parameter, so one of "
                "these is wrong and guessing would mislabel a plan.md §4 sweep cell."
            )

        if fam is not None and self._chunk_ms is not None and self._chunk_ms not in fam.chunk_ms:
            raise SherpaOnnxBackendError(
                f"{fam.display} publishes chunk sizes "
                f"{', '.join(str(c) for c in fam.chunk_ms)} ms; {self._chunk_ms} ms is not one of "
                "them. (plan.md §4 sweeps {80,160,320,640}; note that 640 ms has no published "
                "export in this family and 320 ms exists only for Nemotron-3.5 — "
                "bench/CANDIDATES.md §1.1.)"
            )
        if fam is None:
            self.diagnostics.append(
                f"model `{self.config.model}` matches no known Nemotron family; chunk size was "
                "not validated against a published list"
            )

    def _validate_decoding_method(self) -> None:
        """Reject a decoding method this model cannot actually run.

        Overridden by the zipformer backend, which is the one candidate in the shortlist that
        *does* support `modified_beam_search` and therefore hotwords.
        """
        cfg = self.config
        if cfg.decoding_method != "greedy_search":
            # CANDIDATES.md §5.1, and the honest half of this backend's identity. The Nemotron
            # online transducer rejects modified_beam_search inside sherpa-onnx
            # (k2-fsa/sherpa-onnx#3572), so accepting the request here would produce either a
            # confusing C++-level failure or — worse — a run that claims biasing it never did.
            raise SherpaOnnxBackendError(
                f"decoding_method={cfg.decoding_method!r} is not available for the Nemotron "
                "online transducer: sherpa-onnx supports greedy_search only for this model and "
                "explicitly rejects modified_beam_search (bench/CANDIDATES.md §5.1, "
                "k2-fsa/sherpa-onnx issue 3572). Hotwords therefore cannot work here at all — the "
                "context-biasing arm is the streaming zipformer, in "
                "kvit_asr.backends.zipformer_hotwords."
            )

    def _recognizer_kwargs(self, encoder: Path, decoder: Path, joiner: Path,
                           tokens: Path) -> dict[str, Any]:
        cfg = self.config
        self._validate_decoding_method()
        kwargs: dict[str, Any] = {
            "tokens": str(tokens),
            "encoder": str(encoder),
            "decoder": str(decoder),
            "joiner": str(joiner),
            "num_threads": cfg.num_threads,
            "sample_rate": cfg.sample_rate,
            "feature_dim": cfg.feature_dim,
            "decoding_method": cfg.decoding_method,
            "provider": cfg.provider,
            "blank_penalty": cfg.blank_penalty,
            # The sidecar owns endpointing (Silero VAD, plan.md §3 Phase 2 item 3) so that every
            # backend is endpointed identically and the comparison stays about the acoustic model.
            # sherpa-onnx's own rule-based endpointer is left off by default but kept reachable,
            # because Phase 2 may want to measure it as an alternative.
            "enable_endpoint_detection": cfg.enable_endpoint_detection,
            "rule1_min_trailing_silence": cfg.rule1_min_trailing_silence,
            "rule2_min_trailing_silence": cfg.rule2_min_trailing_silence,
            "rule3_min_utterance_length": cfg.rule3_min_utterance_length,
        }
        if cfg.model_type:
            kwargs["model_type"] = cfg.model_type
        kwargs.update(cfg.extra)
        return kwargs

    # -- session / utterance --------------------------------------------------------------------

    def start_session(self, session_id: str, context: Iterable[str] | None = None,
                      sample_rate: int = PROTOCOL_SAMPLE_RATE) -> None:
        self._require_loaded()
        if sample_rate != self.config.sample_rate:
            raise SherpaOnnxBackendError(
                f"start.sample_rate={sample_rate} but this backend is configured for "
                f"{self.config.sample_rate} Hz; proto 1 fixes it at {PROTOCOL_SAMPLE_RATE} "
                "(docs/protocol.md §6.1)"
            )
        self._session_id = session_id
        self._drop_stream()

        items = list(context or ())
        if items:
            # docs/protocol.md §6.1: a Server without context support MUST ignore `context` and
            # MUST report context_supported:false. Ignoring is therefore correct — but it is
            # logged and counted, because a run that received hints it could not use belongs in
            # the context-FREE column of plan.md §4, and that fact has to survive to the record.
            self._ignored_context_items += len(items)
            log.warning(
                "session %s supplied %d context item(s) (%s%s); this backend cannot bias "
                "(greedy_search only, bench/CANDIDATES.md §5.1) and is ignoring them. "
                "context_supported is reported as false. Treat this run as context-free.",
                session_id, len(items), ", ".join(items[:5]),
                ", …" if len(items) > 5 else "",
            )

    def accept_pcm(self, pcm: bytes) -> str:
        """Feed one audio frame; return the cumulative hypothesis for the current utterance."""
        self._require_loaded()
        if not pcm:
            # A zero-length audio payload is legal and means "no samples" (docs/protocol.md §6.3).
            return self.current_text()
        samples = pcm16le_to_float32(pcm)
        if self._stream is None:
            self._stream = self._create_stream()
        self._accept(self._stream, samples)
        self._drain(self._stream)
        return _result_text(self._recognizer, self._stream)

    def current_text(self) -> str:
        if self._stream is None:
            return ""
        return _result_text(self._recognizer, self._stream)

    def finish_utterance(self) -> str:
        """Flush the decoder and return the final cumulative text for this utterance.

        The stream is then dropped, so the next `accept_pcm` starts a fresh one. That is a full
        reset of encoder cache and feature history, which is what plan.md §1.6 asks for at an
        utterance boundary ("stop feeding at the endpoint, reset decoder state, resume on speech
        onset") and what keeps a long silence from being fed to a model that may hallucinate on
        it. `recognizer.reset(stream)` — which clears the decoded text but keeps the cached
        encoder state — is the cheaper alternative if Phase 2 measurements show the cross-
        utterance context was worth keeping; it is deliberately not the default, because the
        conservative choice is the one whose failure mode is a missing word rather than a
        hallucinated one.
        """
        self._require_loaded()
        if self._stream is None:
            return ""
        stream = self._stream
        try:
            if self.config.tail_padding_ms > 0:
                # Flush the encoder's right context. UNVERIFIED item 5.
                self._accept(stream, _silence(self.config.tail_padding_ms))
            input_finished = getattr(stream, "input_finished", None)
            if input_finished is not None:
                input_finished()
            else:
                self.diagnostics.append(
                    "OnlineStream has no input_finished(); the tail of each utterance may be "
                    "under-decoded"
                )
                log.warning("%s", self.diagnostics[-1])
            self._drain(stream)
            return _result_text(self._recognizer, stream)
        finally:
            self._stream = None

    def discard_utterance(self) -> None:
        """Drop the in-flight utterance without decoding it (`cancel`, or a discontinuity)."""
        self._drop_stream()

    def end_session(self) -> None:
        self._drop_stream()
        self._session_id = None

    def close(self) -> None:
        self._drop_stream()
        # Dropping the reference is all that can be done from Python; sherpa-onnx frees the ORT
        # sessions in its own destructor. There is no documented explicit-close call, so none is
        # invented here.
        self._recognizer = None

    def is_endpoint(self) -> bool:
        """sherpa-onnx's own rule-based endpointer, if it was enabled. Off by default."""
        if self._recognizer is None or self._stream is None:
            return False
        if not self.config.enable_endpoint_detection:
            return False
        is_endpoint = _require_attr(self._recognizer, "is_endpoint", "OnlineRecognizer")
        return bool(is_endpoint(self._stream))

    # -- sherpa-onnx plumbing -------------------------------------------------------------------

    def _note(self, text: str) -> None:
        """Record a non-fatal observation for the run record (see `diagnostics`)."""
        self.diagnostics.append(text)

    def _require_loaded(self) -> None:
        if self._recognizer is None:
            raise SherpaOnnxBackendError(
                "backend used before load(); the server must not emit `ready` until load() "
                "returns (docs/protocol.md §6.2)"
            )

    def _create_stream(self) -> Any:
        """Create a decoding stream. Overridden by the hotword backend to attach context."""
        create_stream = _require_attr(self._recognizer, "create_stream", "OnlineRecognizer")
        return create_stream()

    def _accept(self, stream: Any, samples: Any) -> None:
        accept_waveform = _require_attr(stream, "accept_waveform", "OnlineStream")
        try:
            accept_waveform(self.config.sample_rate, samples)
        except SherpaOnnxBackendError:
            raise
        except Exception as exc:   # noqa: BLE001
            raise SherpaOnnxBackendError(
                "OnlineStream.accept_waveform rejected the samples. This adapter passes float32 "
                "in [-1, 1] at "
                f"{self.config.sample_rate} Hz as {type(samples).__name__} "
                f"(sherpa-onnx {_sherpa_version()}): {type(exc).__name__}: {exc}"
            ) from exc

    def _drain(self, stream: Any) -> None:
        is_ready = _require_attr(self._recognizer, "is_ready", "OnlineRecognizer")
        decode_stream = _require_attr(self._recognizer, "decode_stream", "OnlineRecognizer")
        # Bounded rather than `while True`: a version whose is_ready() never goes false would
        # otherwise wedge the session loop with no diagnostic at all. 100k iterations is far more
        # than any real utterance needs and still terminates in seconds.
        for _ in range(100_000):
            if not is_ready(stream):
                return
            decode_stream(stream)
        raise SherpaOnnxApiMismatch(
            "OnlineRecognizer.is_ready() stayed true for 100000 decode iterations; refusing to "
            f"loop further (sherpa-onnx {_sherpa_version()}). Either decode_stream() is not "
            "consuming input or is_ready() does not mean what this adapter assumes."
        )

    def _drop_stream(self) -> None:
        self._stream = None


def create(options: Mapping[str, Any] | None = None, **kwargs: Any) -> SherpaOnnxStreamingBackend:
    """Factory a backend registry can call without importing the config dataclass."""
    merged: dict[str, Any] = dict(options or {})
    merged.update(kwargs)
    return SherpaOnnxStreamingBackend(SherpaOnnxStreamingConfig.from_mapping(merged))
