# Copyright 2026 Kvit
# Licensed under the Apache License, Version 2.0. See LICENSE-APACHE, or
# https://www.apache.org/licenses/LICENSE-2.0
"""Streaming zipformer transducer with hotword (context) biasing, via sherpa-onnx.

`bench/CANDIDATES.md` §4 pick 3, row C — `sherpa-onnx-streaming-zipformer-en-2023-06-26` and its
siblings (`-2023-06-21`, `-2023-02-21`). This is **the context-biasing arm of the bake-off**, and
it exists for one reason: it is the only natively-streaming candidate in the shortlist that can be
biased toward a vocabulary at all. CANDIDATES.md §5.1 records the finding that shaped this file —
the fastest, most accurate streaming candidate (Nemotron via sherpa-onnx) is `greedy_search`-only
and therefore cannot use hotwords, so without this row plan.md §4's context-enabled column has no
native-streaming entrant. It is expected to lose on raw WER; that is not a disappointment, it is
the measurement. What it buys is a number for what biasing is worth on the technical corpus.

`supports_context` is **True** — but only when the configuration can actually honour it; see
"Honesty rule" below.

──────────────────────────────────────────────────────────────────────────────────────────────
How biasing is wired
──────────────────────────────────────────────────────────────────────────────────────────────

`start.context` (docs/protocol.md §6.1) is a list of ≤256 vocabulary hints, ≤128 bytes each, e.g.
`["llama.cpp","Qwen3","sm_120"]`. sherpa-onnx implements biasing as transducer **hotwords**, which
require `decoding_method="modified_beam_search"`; a boost score is added to paths that match a
hotword's token sequence. Two delivery mechanisms exist, and this module prefers the first:

 1. **Per-stream hotwords** — `recognizer.create_stream(hotwords=...)`. Right shape for us:
    context arrives per session and the recognizer is built once at process start.
 2. **Recognizer-level `hotwords_file`** — a text file passed to `from_transducer`. Used only as
    a fallback, and taking it **rebuilds the recognizer**, which is logged as the model reload it
    is rather than hidden inside `start`.

Which one is used is decided at first `start` by introspecting `create_stream`, and is recorded in
`diagnostics` so a run record says which path produced its numbers.

──────────────────────────────────────────────────────────────────────────────────────────────
Honesty rule (plan.md §4, docs/protocol.md §6.1/§6.2)
──────────────────────────────────────────────────────────────────────────────────────────────

Context-enabled and context-free runs are compared separately, so `ready.context_supported` must
describe *this configuration*, not this model family. Two consequences, both implemented:

 * Configured with `decoding_method="greedy_search"` — which is a legitimate configuration,
   because plan.md §4 needs this same model measured context-free as well as context-enabled —
   the instance sets `supports_context = False`. The alternative, claiming support and then
   dropping the hints, would put a context-free result in the context-enabled column.
 * If hotwords are requested and cannot be delivered (no mechanism, or the tokenisation inputs a
   BPE model needs are absent), that is an **error**, not a warning-and-continue. A run that
   quietly decoded without the biasing it reported is worse than a run that failed.

──────────────────────────────────────────────────────────────────────────────────────────────
UNVERIFIED — as with `sherpa_onnx_streaming.py`, none of this has been executed: sherpa-onnx is
not installed on this machine and no model has been downloaded. Written against the sherpa-onnx
hotwords documentation (k2-fsa.github.io/sherpa/onnx/hotwords/index.html, the source
CANDIDATES.md cites for the biasing column). Check on first real run:
──────────────────────────────────────────────────────────────────────────────────────────────

 1. The keyword name for per-stream hotwords on `create_stream`. Believed to be `hotwords=`; some
    versions used `contexts_list=`. Both are probed by signature introspection and, if neither is
    present, the recognizer-level fallback is taken — no keyword is guessed.
 2. The content format of that argument, and of `hotwords_file`: believed to be one phrase per
    line, plain text, with tokenisation performed by sherpa-onnx from `modeling_unit` +
    `bpe_vocab`. If a build instead expects pre-tokenised text (`▁LLAMA ▁CPP`), hotwords will
    appear to be accepted and will bias nothing — check the decoded output against a known
    hotword before trusting any context-enabled measurement.
 3. `bpe_vocab`. The documentation describes a two-column vocabulary file produced by
    sentencepiece (`bpe.vocab`), while the published model directories are known to ship
    `bpe.model`. `_find_bpe_vocab()` prefers `bpe.vocab`, falls back to `bpe.model` with a loud
    warning, and lets the config override both.
 4. Per-hotword boost via a `:score` suffix. Supported in recent sherpa-onnx, unconfirmed for the
    version we will install, and **off by default** here: if the suffix is not understood, `:2.0`
    becomes part of the phrase and biases nothing while looking like it worked. Worse, several of
    our real hotwords (`C:\\path`, `host:port`) contain colons — see `_check_colons()`.
 5. Token casing. LibriSpeech-derived English BPE models emit uppercase; others do not. A hotword
    whose casing does not match the model's tokens will not match anything. `tokens.txt` is
    inspected at load and a mismatch is warned about; nothing is silently case-folded unless
    `hotword_case` says to.
 6. `hotwords_score` default. sherpa-onnx's documented default is 1.5; treated as a tunable, and
    plan.md §4 should sweep it if biasing turns out to matter.
"""

from __future__ import annotations

import inspect
import logging
import os
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Mapping

from .sherpa_onnx_streaming import (
    PROTOCOL_SAMPLE_RATE,
    ModelFilesMissing,
    SherpaOnnxApiMismatch,
    SherpaOnnxBackendError,
    SherpaOnnxStreamingBackend,
    SherpaOnnxStreamingConfig,
    _as_bool,
    _as_float,
    _as_int,
    _build_recognizer,
    _require_attr,
    _sherpa_version,
)

__all__ = [
    "ZipformerHotwordsConfig",
    "ZipformerHotwordsBackend",
    "create",
]

log = logging.getLogger(__name__)

# docs/protocol.md §6.1: `start.context` is at most 256 items of at most 128 bytes. The core
# validates the wire message; these are restated so this module fails the same way when it is
# driven from the bench harness rather than from a pipe.
MAX_CONTEXT_ITEMS = 256
MAX_CONTEXT_ITEM_BYTES = 128

# Candidate keyword names for per-stream hotwords, in preference order. UNVERIFIED item 1.
_STREAM_HOTWORD_KWARGS = ("hotwords", "contexts_list")


@dataclass
class ZipformerHotwordsConfig(SherpaOnnxStreamingConfig):
    """`SherpaOnnxStreamingConfig` plus the biasing knobs.

    `decoding_method` defaults to `modified_beam_search` here — that is the whole point of this
    backend — but `greedy_search` remains selectable so the same model can be measured
    context-free (plan.md §4). Selecting it flips `supports_context` to False; see the honesty
    rule in the module docstring.
    """

    decoding_method: str = "modified_beam_search"
    hotwords_score: float = 1.5          # sherpa-onnx documented default; UNVERIFIED item 6
    max_active_paths: int = 4            # beam width for modified_beam_search
    modeling_unit: str = "bpe"           # "bpe" | "cjkchar" | "cjkchar+bpe" — English zipformer
    bpe_vocab: str | None = None         # None = look in the model directory
    hotwords_file: str | None = None     # a static, always-on hotword list, in addition to
                                         # start.context. Read at load; never written to.
    per_hotword_boost: bool = False      # emit `phrase :score` lines — UNVERIFIED item 4
    hotword_case: str = "asis"           # "asis" | "upper" | "lower"

    @classmethod
    def from_mapping(cls, options: Mapping[str, Any]) -> "ZipformerHotwordsConfig":
        norm = {str(k).replace("-", "_").lstrip("_"): v for k, v in options.items()}
        biasing_keys = ("hotwords_score", "max_active_paths", "modeling_unit", "bpe_vocab",
                        "hotwords_file", "per_hotword_boost", "hotword_case")
        biasing = {k: norm.pop(k) for k in biasing_keys if k in norm}

        # Reuse the parent's validation and coercion for everything it owns, so an unknown option
        # is still a hard error and `--chunk-ms 160` still arrives as an int.
        base = SherpaOnnxStreamingConfig.from_mapping(norm)
        kwargs = {f: getattr(base, f) for f in SherpaOnnxStreamingConfig.__dataclass_fields__}
        if "decoding_method" not in norm:
            # The parent's default is greedy_search; this backend's is modified_beam_search.
            kwargs["decoding_method"] = cls.decoding_method

        for name in ("modeling_unit", "bpe_vocab", "hotwords_file", "hotword_case"):
            if biasing.get(name) is not None:
                kwargs[name] = str(biasing[name])
        if biasing.get("hotwords_score") is not None:
            kwargs["hotwords_score"] = _as_float(biasing["hotwords_score"], "hotwords_score")
        if biasing.get("max_active_paths") is not None:
            kwargs["max_active_paths"] = _as_int(biasing["max_active_paths"], "max_active_paths")
        if biasing.get("per_hotword_boost") is not None:
            kwargs["per_hotword_boost"] = _as_bool(
                biasing["per_hotword_boost"], "per_hotword_boost"
            )

        case = kwargs.get("hotword_case", cls.hotword_case)
        if case not in ("asis", "upper", "lower"):
            raise ValueError(f"hotword_case must be asis|upper|lower, got {case!r}")
        return cls(**kwargs)


class ZipformerHotwordsBackend(SherpaOnnxStreamingBackend):
    """Streaming zipformer with per-session hotword biasing.

    Same lifecycle as `SherpaOnnxStreamingBackend`; the only behavioural differences are that
    `start_session` acts on `context`, and that `_create_stream` attaches the hotwords.
    """

    supports_context = True   # class default; the instance narrows it — see __init__

    def __init__(self, config: ZipformerHotwordsConfig) -> None:
        super().__init__(config)
        self.config: ZipformerHotwordsConfig = config
        # Instance attribute shadowing the class attribute. `ready.context_supported` must
        # describe THIS configuration: with greedy_search selected, hotwords are unavailable and
        # claiming otherwise would file a context-free run in plan.md §4's context-enabled column.
        self.supports_context = config.decoding_method == "modified_beam_search"
        self._hotwords_text: str = ""        # the lines currently in force, "" = none
        self._stream_hotword_kwarg: str | None = None   # resolved at first use
        self._hotwords_path: Path | None = None         # temp file, fallback path only
        self._rebuilt_with_hotwords = False   # true once the fallback baked hotwords in
        self._token_case: str = "unknown"
        self._bpe_vocab: Path | None = None

    # -- load -----------------------------------------------------------------------------------

    def _validate_decoding_method(self) -> None:
        """Override: this is the one shortlisted model where beam search is available.

        The parent refuses anything but `greedy_search` because CANDIDATES.md §5.1 found that the
        Nemotron online transducer rejects `modified_beam_search`. That finding is about Nemotron,
        not about sherpa-onnx, and the zipformer transducer is exactly the model the hotwords
        documentation is written for.
        """
        allowed = ("greedy_search", "modified_beam_search")
        if self.config.decoding_method not in allowed:
            raise SherpaOnnxBackendError(
                f"decoding_method={self.config.decoding_method!r} is not one of "
                f"{', '.join(allowed)}. Hotword biasing requires modified_beam_search "
                "(k2-fsa.github.io/sherpa/onnx/hotwords/); greedy_search is offered only so the "
                "same model can be measured context-free (plan.md §4)."
            )

    def _recognizer_kwargs(self, encoder: Path, decoder: Path, joiner: Path,
                           tokens: Path) -> dict[str, Any]:
        kwargs = super()._recognizer_kwargs(encoder, decoder, joiner, tokens)
        cfg = self.config
        if not self.supports_context:
            # greedy_search: pass no biasing keywords at all, so the recognizer that runs is
            # exactly the context-free one whose numbers we intend to report.
            self._note(
                "decoding_method=greedy_search: biasing is disabled and context_supported is "
                "reported as false"
            )
            return kwargs

        kwargs["max_active_paths"] = cfg.max_active_paths
        kwargs["hotwords_score"] = cfg.hotwords_score
        kwargs["modeling_unit"] = cfg.modeling_unit
        self._token_case = _token_case(tokens)
        if cfg.modeling_unit in ("bpe", "cjkchar+bpe"):
            self._bpe_vocab = self._find_bpe_vocab()
            kwargs["bpe_vocab"] = str(self._bpe_vocab)

        # A static hotword list from settings, always in force. Distinct from start.context, which
        # is per session; both end up in the same list.
        if cfg.hotwords_file:
            static = Path(cfg.hotwords_file).expanduser()
            if not static.is_file():
                raise ModelFilesMissing(f"hotwords_file not found: {static}")
            kwargs["hotwords_file"] = str(static)
        return kwargs

    def _find_bpe_vocab(self) -> Path:
        """Locate the vocabulary sherpa-onnx needs to tokenise a raw hotword phrase.

        UNVERIFIED item 3, and the one place in this module where the documentation and the known
        contents of the model repositories do not line up. The docs describe a two-column
        sentencepiece vocabulary (`bpe.vocab`); the published sherpa-onnx zipformer directories are
        known to ship `bpe.model`. Preferring `.vocab`, falling back to `.model` with a warning,
        and letting the config override both is the honest handling: the fallback may work, and if
        it does not it will fail inside sherpa-onnx with the file name in the message.
        """
        if self.config.bpe_vocab:
            explicit = Path(self.config.bpe_vocab).expanduser()
            if not explicit.is_file():
                raise ModelFilesMissing(f"bpe_vocab not found: {explicit}")
            return explicit
        model_dir = self._model_dir
        if model_dir is None:   # unreachable: called from load(), after the directory is resolved
            raise SherpaOnnxBackendError("model directory not resolved before bpe_vocab lookup")
        vocab = model_dir / "bpe.vocab"
        if vocab.is_file():
            return vocab
        model = model_dir / "bpe.model"
        if model.is_file():
            self._note(
                f"{model_dir.name} has no bpe.vocab; passing bpe.model to `bpe_vocab` instead. "
                "The sherpa-onnx documentation describes a two-column sentencepiece vocabulary "
                "for this argument, so this is UNVERIFIED: if hotwords bias nothing, generate "
                "bpe.vocab from bpe.model with sentencepiece and set --bpe-vocab."
            )
            log.warning("%s", self.diagnostics[-1])
            return model
        raise ModelFilesMissing(
            f"{model_dir}: modeling_unit={self.config.modeling_unit} needs a BPE vocabulary to "
            "tokenise hotwords, and neither bpe.vocab nor bpe.model is present. Either the model "
            "directory is incomplete, or this model is not BPE — set --modeling-unit accordingly."
        )

    # -- session --------------------------------------------------------------------------------

    def start_session(self, session_id: str, context: Iterable[str] | None = None,
                      sample_rate: int = PROTOCOL_SAMPLE_RATE) -> None:
        self._require_loaded()
        if sample_rate != self.config.sample_rate:
            raise SherpaOnnxBackendError(
                f"start.sample_rate={sample_rate} but this backend is configured for "
                f"{self.config.sample_rate} Hz (docs/protocol.md §6.1)"
            )
        self._session_id = session_id
        self._drop_stream()

        items = _clean_context(context, self.config.hotword_case)
        if items and not self.supports_context:
            # Cannot happen through a conforming Client — the server reports
            # context_supported:false for this configuration and a Client that sends context
            # anyway gets it ignored per §6.1 — but if it does happen, say so rather than
            # producing a context-free run wearing a context-enabled label.
            self._ignored_context_items += len(items)
            log.warning(
                "session %s supplied %d context item(s) but this instance runs greedy_search; "
                "they are ignored and the run is context-FREE",
                session_id, len(items),
            )
            items = []

        if items:
            _check_colons(items, self.config.per_hotword_boost)
            self._check_case(items)
        self._set_hotwords(items)

    def _set_hotwords(self, items: list[str]) -> None:
        """Install `items` as the hotword list for the coming session."""
        text = _render_hotwords(items, self.config.hotwords_score, self.config.per_hotword_boost)
        if text == self._hotwords_text:
            return
        self._hotwords_text = text
        if not text:
            if self._rebuilt_with_hotwords:
                # The recognizer itself carries the previous session's hotwords, so simply
                # forgetting the text here would leave this "context-free" session biased by the
                # last context-enabled one — a cross-contamination that plan.md §4's separate
                # comparison would never survive. Rebuild clean.
                log.info("context cleared; rebuilding the recognizer without hotwords")
                self._rebuild(hotwords_path=None)
            self._clear_hotwords_file()
            return

        if self._stream_hotword_kwarg is None:
            self._stream_hotword_kwarg = self._detect_stream_hotword_kwarg()

        if self._stream_hotword_kwarg == "":
            # Fallback: no per-stream mechanism, so the list must go into the recognizer, which
            # means rebuilding it. Logged at WARNING because it is a model reload happening inside
            # what the Client thinks is a session start — a latency event worth seeing in the log
            # rather than a mystery in the first-partial numbers.
            log.warning(
                "sherpa-onnx %s exposes no per-stream hotwords argument; rebuilding the "
                "recognizer with a hotwords file. This reloads the model on every context "
                "change.", _sherpa_version(),
            )
            self._rebuild_with_hotwords_file(text)
        else:
            log.info(
                "hotwords: %d phrase(s) via create_stream(%s=...), score %.2f",
                len(items), self._stream_hotword_kwarg, self.config.hotwords_score,
            )

    def _detect_stream_hotword_kwarg(self) -> str:
        """Which keyword `create_stream` takes, or `""` if it takes none. UNVERIFIED item 1."""
        create_stream = _require_attr(self._recognizer, "create_stream", "OnlineRecognizer")
        try:
            sig = inspect.signature(create_stream)
        except (TypeError, ValueError):
            # No introspectable signature (a C-implemented callable). Probing by calling it with a
            # guessed keyword would either work or raise TypeError, and a TypeError from inside a
            # pybind11 overload set is not reliably distinguishable from a real argument error —
            # so take the documented fallback rather than guess.
            self._note(
                "create_stream() has no introspectable signature; using the hotwords-file "
                "mechanism rather than guessing a keyword name"
            )
            return ""
        names = set(sig.parameters)
        if any(p.kind is inspect.Parameter.VAR_KEYWORD for p in sig.parameters.values()):
            names |= set(_STREAM_HOTWORD_KWARGS)   # **kwargs: assume the documented name
        for candidate in _STREAM_HOTWORD_KWARGS:
            if candidate in names:
                self._note(f"hotwords delivered per stream via create_stream({candidate}=...)")
                return candidate
        self._note(
            f"create_stream{sig} accepts none of {', '.join(_STREAM_HOTWORD_KWARGS)}; "
            "falling back to a recognizer-level hotwords file"
        )
        return ""

    def _rebuild_with_hotwords_file(self, text: str) -> None:
        self._rebuild(hotwords_path=self._write_hotwords_file(text))

    def _rebuild(self, hotwords_path: Path | None) -> None:
        """Rebuild the recognizer, with or without a hotwords file.

        Only the fallback mechanism reaches here. It is a full model load; the caller logs it.
        """
        kwargs = dict(self._recognizer_kwargs_used)
        if hotwords_path is None:
            kwargs.pop("hotwords_file", None)
        else:
            kwargs["hotwords_file"] = str(hotwords_path)
            kwargs["hotwords_score"] = self.config.hotwords_score
        count = 0 if hotwords_path is None else len(self._hotwords_text.splitlines())
        self._recognizer = _build_recognizer(
            kwargs, context=f"model={self.config.model} with {count} hotwords"
        )
        self._rebuilt_with_hotwords = hotwords_path is not None

    def _write_hotwords_file(self, text: str) -> Path:
        """Write the hotword list where sherpa-onnx can read it.

        The file holds vocabulary hints supplied by the Client — identifiers and product names,
        not transcripts — so it is not covered by plan.md §1.9's no-persistence rule for
        recognised text. It is still short-lived: written to the user's temp directory, replaced
        on every context change, and deleted at `close()`. UTF-8 without a BOM, because that is
        what every other encoded surface in this project is.
        """
        self._clear_hotwords_file()
        fd, name = tempfile.mkstemp(prefix="kvit-hotwords-", suffix=".txt")
        try:
            with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(text)
        except Exception:
            os.unlink(name)
            raise
        self._hotwords_path = Path(name)
        return self._hotwords_path

    def _clear_hotwords_file(self) -> None:
        if self._hotwords_path is None:
            return
        try:
            self._hotwords_path.unlink()
        except OSError as exc:   # a leftover temp file is untidy, never a reason to fail a session
            log.debug("could not remove hotwords file %s: %s", self._hotwords_path, exc)
        self._hotwords_path = None

    def _create_stream(self) -> Any:
        create_stream = _require_attr(self._recognizer, "create_stream", "OnlineRecognizer")
        if not self._hotwords_text or not self._stream_hotword_kwarg:
            # No context this session, or the recognizer-level fallback already carries it.
            return create_stream()
        try:
            return create_stream(**{self._stream_hotword_kwarg: self._hotwords_text})
        except TypeError as exc:
            raise SherpaOnnxApiMismatch(
                f"create_stream({self._stream_hotword_kwarg}=...) was rejected even though the "
                f"signature advertised that keyword (sherpa-onnx {_sherpa_version()}): {exc}. "
                "Hotword delivery is UNVERIFIED (see the module docstring); this configuration "
                "must not be reported as context-enabled until it is checked."
            ) from exc
        except Exception as exc:   # noqa: BLE001
            raise SherpaOnnxBackendError(
                f"sherpa-onnx failed to create a stream with "
                f"{len(self._hotwords_text.splitlines())} hotwords: "
                f"{type(exc).__name__}: {exc}"
            ) from exc

    def end_session(self) -> None:
        super().end_session()
        # Deliberately keep `_hotwords_text` and the recognizer as they are: the next session
        # normally carries the same context, and dropping it here would force the expensive
        # rebuild path on every session when the fallback mechanism is in use.

    def close(self) -> None:
        self._clear_hotwords_file()
        super().close()

    # -- checks that exist because the failure modes are silent ---------------------------------

    def _check_case(self, items: list[str]) -> None:
        """Warn when hotword casing cannot match the model's tokens. UNVERIFIED item 5."""
        if self._token_case == "upper" and any(c.islower() for item in items for c in item):
            self._note(
                "tokens.txt is uppercase but some hotwords contain lowercase letters; those "
                "hotwords cannot match any token sequence and will bias nothing. Set "
                "--hotword-case upper, or supply uppercase context."
            )
            log.warning("%s", self.diagnostics[-1])
        elif self._token_case == "lower" and any(c.isupper() for item in items for c in item):
            self._note(
                "tokens.txt is lowercase but some hotwords contain uppercase letters; consider "
                "--hotword-case lower"
            )
            log.warning("%s", self.diagnostics[-1])


# ── helpers ────────────────────────────────────────────────────────────────────────────────────

def _clean_context(context: Iterable[str] | None, case: str) -> list[str]:
    """Normalise `start.context` into hotword phrases.

    Newlines are the record separator in every sherpa-onnx hotword format, so a phrase containing
    one would split into two hotwords. Rather than dropping it, the newline is collapsed to a
    space: the protocol permits any UTF-8 in a context item, and this is the only transformation
    that preserves the whole phrase.
    """
    items: list[str] = []
    for raw in context or ():
        if not isinstance(raw, str):
            raise ValueError(f"context items must be strings, got {type(raw).__name__}")
        phrase = " ".join(raw.split())   # collapses \n, \r, \t and runs of spaces
        if not phrase:
            continue
        if len(phrase.encode("utf-8")) > MAX_CONTEXT_ITEM_BYTES:
            # docs/protocol.md §6.1. Truncating a UTF-8 string by bytes is how mojibake is made,
            # so an over-long item is refused rather than cut.
            raise ValueError(
                f"context item exceeds {MAX_CONTEXT_ITEM_BYTES} bytes: {phrase[:40]!r}…"
            )
        if case == "upper":
            phrase = phrase.upper()
        elif case == "lower":
            phrase = phrase.lower()
        if phrase not in items:
            items.append(phrase)
    if len(items) > MAX_CONTEXT_ITEMS:
        raise ValueError(
            f"{len(items)} context items exceeds the protocol maximum of {MAX_CONTEXT_ITEMS} "
            "(docs/protocol.md §6.1)"
        )
    return items


def _render_hotwords(items: list[str], score: float, per_hotword_boost: bool) -> str:
    """Render the hotword list. One phrase per line — the format both mechanisms take.

    `per_hotword_boost` appends ` :score`. It is off by default because it is UNVERIFIED (item 4)
    and its failure mode is silent: a build that does not understand the suffix treats `:1.5` as
    part of the phrase, which then matches nothing. With it off, every hotword takes the global
    `hotwords_score` passed to the recognizer, which is what plan.md §4 would sweep anyway.
    """
    if not items:
        return ""
    if per_hotword_boost:
        lines = [f"{phrase} :{score:g}" for phrase in items]
    else:
        lines = list(items)
    return "\n".join(lines) + "\n"


def _check_colons(items: list[str], per_hotword_boost: bool) -> None:
    """Warn about phrases that may collide with the `:score` suffix syntax.

    Not hypothetical for this corpus: plan.md §3 Phase 1 puts paths and CLI flags in `tech/`, and
    `C:\\Users`, `127.0.0.1:8080` and `host:port` all carry a colon. If the installed sherpa-onnx
    parses a trailing `:token` as a boost score, such a phrase is silently mangled.
    """
    suspects = [p for p in items if ":" in p]
    if not suspects:
        return
    log.warning(
        "%d hotword(s) contain ':' (%s%s). Some sherpa-onnx versions read a trailing ':score' as "
        "a per-hotword boost, which would silently truncate the phrase. per_hotword_boost=%s. "
        "Verify against the decoded output before trusting a context-enabled measurement.",
        len(suspects), ", ".join(suspects[:3]), ", …" if len(suspects) > 3 else "",
        per_hotword_boost,
    )


def _token_case(tokens: Path) -> str:
    """Peek at `tokens.txt` to learn whether the model's units are upper, lower or mixed case.

    Cheap (the file is a few thousand short lines) and worth doing, because a casing mismatch
    between a hotword and the model's tokens produces exactly nothing — no error, no bias, no
    signal in the transcript that anything was attempted.
    """
    upper = lower = 0
    try:
        with tokens.open("r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                unit = line.split(" ", 1)[0]
                # Skip the special symbols — `<blk>`, `<unk>`, `<sos/eos>` are lowercase in every
                # icefall model regardless of the casing of its actual vocabulary, so counting
                # them makes every model look "mixed" and the check useless.
                if unit.startswith("<") or not unit:
                    continue
                for ch in unit:
                    if ch.isupper():
                        upper += 1
                    elif ch.islower():
                        lower += 1
    except OSError as exc:
        log.debug("could not inspect %s: %s", tokens, exc)
        return "unknown"
    if upper and not lower:
        return "upper"
    if lower and not upper:
        return "lower"
    if not upper and not lower:
        return "unknown"
    return "mixed"


def create(options: Mapping[str, Any] | None = None, **kwargs: Any) -> ZipformerHotwordsBackend:
    """Factory a backend registry can call without importing the config dataclass."""
    merged: dict[str, Any] = dict(options or {})
    merged.update(kwargs)
    return ZipformerHotwordsBackend(ZipformerHotwordsConfig.from_mapping(merged))
